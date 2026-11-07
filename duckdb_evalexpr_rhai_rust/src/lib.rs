// duckdb_evalexpr_rust
// Copyright 2024-2026 Rusty Conover <rusty@query.farm>
// Licensed under the MIT License

// The exported functions take raw pointers from C++; their contracts are documented on each.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::slice;
use std::str;

use rhai::{packages::Package, Array, Dynamic, Engine, Map, Scope, AST};
use rhai_rand::RandomPackage;
use rhai_sci::SciPackage;
use rhai_url::UrlPackage;

// NOTE: rhai-fs (FilesystemPackage) is intentionally NOT registered. It let any SQL user read,
// write and delete arbitrary files with the permissions of the DuckDB process, bypassing
// `enable_external_access` and `allowed_directories`. rhai-sci is built without its `io`
// feature for the same reason (it reads files and fetches URLs).

#[repr(C)]
pub enum ResultCString {
    Ok(*mut c_char),
    Err(*mut c_char),
}

/// Resource limits applied to every evaluation. A value of 0 means "unlimited".
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EvalLimits {
    /// Maximum number of Rhai operations per evaluation.
    pub max_operations: u64,
    /// Maximum length (in bytes) of any string.
    pub max_string_size: u64,
    /// Maximum number of elements in any array.
    pub max_array_size: u64,
    /// Maximum number of properties in any object map.
    pub max_map_size: u64,
    /// Maximum depth of nested function calls.
    pub max_call_levels: u64,
}

/// Callback polled while a script runs; returns true when the query was interrupted.
pub type InterruptCallback = Option<extern "C" fn(*mut c_void) -> bool>;

/// How many operations run between two polls of the interrupt callback.
const INTERRUPT_POLL_INTERVAL: u64 = 1024;

/// Compiled expressions kept per engine (per DuckDB thread), and the largest one cached.
const AST_CACHE_CAPACITY: usize = 256;
const AST_CACHE_MAX_EXPRESSION_LEN: usize = 64 * 1024;

/// Size of the stack evaluations run on (see `with_large_stack`).
#[cfg(not(target_family = "wasm"))]
const EVAL_STACK_SIZE: usize = 256 * 1024 * 1024;

/// Maximum nesting depth of a result converted to JSON.
const MAX_RESULT_DEPTH: usize = 128;

/// A Rhai engine with its packages registered, the configured limits applied, and a cache of
/// compiled expressions. One is created per DuckDB thread (function local state) and must be
/// released with `evalexpr_engine_free`. It is not thread safe.
pub struct EvalEngine {
    engine: Engine,
    cache: HashMap<String, Result<Rc<AST>, String>>,
}

/// Convert a Rust string into a C string owned by Rust.
///
/// Interior NUL bytes (which a C string cannot hold) are removed, so this never fails.
/// The result must be released with `free_c_string` (or `free_result_cstring`).
fn to_c_string(value: String) -> *mut c_char {
    let value = match CString::new(value) {
        Ok(value) => value,
        Err(error) => {
            let mut bytes = error.into_vec();
            bytes.retain(|&b| b != 0);
            // SAFETY: every NUL byte was just removed.
            unsafe { CString::from_vec_unchecked(bytes) }
        }
    };
    value.into_raw()
}

fn panic_message(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        format!("Rhai evaluation panicked: {}", message)
    } else if let Some(message) = payload.downcast_ref::<String>() {
        format!("Rhai evaluation panicked: {}", message)
    } else {
        "Rhai evaluation panicked".to_string()
    }
}

/// Run `f`, turning any panic into an error so that it never unwinds across the FFI boundary
/// (which would abort the whole process).
fn guard(f: impl FnOnce() -> Result<String, String>) -> ResultCString {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(value)) => ResultCString::Ok(to_c_string(value)),
        Ok(Err(error)) => ResultCString::Err(to_c_string(error)),
        Err(payload) => ResultCString::Err(to_c_string(panic_message(payload))),
    }
}

/// Borrow a (pointer, length) pair passed from C++ as UTF-8 text.
fn input_str<'a>(ptr: *const c_char, len: usize, what: &str) -> Result<&'a str, String> {
    if ptr.is_null() || len == 0 {
        return Ok("");
    }
    let bytes = unsafe { slice::from_raw_parts(ptr.cast::<u8>(), len) };
    str::from_utf8(bytes).map_err(|error| format!("The {} is not valid UTF-8: {}", what, error))
}

fn to_usize(value: u64) -> usize {
    usize::try_from(value).unwrap_or(usize::MAX)
}

fn new_engine(
    limits: &EvalLimits,
    interrupt: InterruptCallback,
    interrupt_data: *mut c_void,
) -> Engine {
    let mut engine = Engine::new();

    engine.register_global_module(RandomPackage::new().as_shared_module());
    engine.register_global_module(UrlPackage::new().as_shared_module());
    engine.register_global_module(SciPackage::new().as_shared_module());

    // print() and debug() would otherwise write to the stdout/stderr of the DuckDB process.
    engine.on_print(|_| {});
    engine.on_debug(|_, _, _| {});

    engine.set_max_operations(limits.max_operations);
    engine.set_max_string_size(to_usize(limits.max_string_size));
    engine.set_max_array_size(to_usize(limits.max_array_size));
    engine.set_max_map_size(to_usize(limits.max_map_size));
    engine.set_max_call_levels(if limits.max_call_levels == 0 {
        usize::MAX
    } else {
        to_usize(limits.max_call_levels)
    });

    if let Some(callback) = interrupt {
        // The pointer is owned by the C++ side and outlives the engine.
        let data = interrupt_data as usize;
        engine.on_progress(move |operations| {
            if operations % INTERRUPT_POLL_INTERVAL == 0 && callback(data as *mut c_void) {
                Some("Query interrupted".into())
            } else {
                None
            }
        });
    }
    engine
}

/// Parse the optional JSON context into a scope that exposes it as `context`.
fn make_scope(context_json: *const c_char, context_len: usize) -> Result<Scope<'static>, String> {
    let mut scope = Scope::new();
    if context_json.is_null() || context_len == 0 {
        return Ok(scope);
    }
    let context_str = input_str(context_json, context_len, "JSON context")?;
    // Deserialize 'Dynamic' from JSON (serde_json bounds the nesting depth).
    let context: Dynamic = serde_json::from_str(context_str)
        .map_err(|error| format!("JSON context was not well formed: {}", error))?;
    scope.push("context", context);
    Ok(scope)
}

/// Returns true when `value` nests arrays/maps deeper than `limit`.
/// The recursion stops at `limit`, so it cannot overflow the stack.
fn too_deep(value: &Dynamic, depth: usize, limit: usize) -> bool {
    if depth > limit {
        return true;
    }
    if let Ok(array) = value.as_array_ref() {
        array.iter().any(|v| too_deep(v, depth + 1, limit))
    } else if let Ok(map) = value.as_map_ref() {
        map.values().any(|v| too_deep(v, depth + 1, limit))
    } else {
        false
    }
}

/// Drop a value without recursing through deeply nested arrays/maps (which could overflow the stack).
fn drop_flat(value: Dynamic) {
    let mut stack = vec![value];
    while let Some(mut item) = stack.pop() {
        if item.is_array() {
            if let Some(mut array) = item.write_lock::<Array>() {
                stack.extend(array.drain(..));
            }
        } else if item.is_map() {
            if let Some(mut map) = item.write_lock::<Map>() {
                stack.extend(std::mem::take(&mut *map).into_values());
            }
        }
        drop(item);
    }
}

fn to_json(output: Dynamic) -> Result<String, String> {
    let result = if too_deep(&output, 0, MAX_RESULT_DEPTH) {
        Err(format!(
            "Rhai result is nested more than {} levels deep and cannot be converted to JSON",
            MAX_RESULT_DEPTH
        ))
    } else {
        serde_json::to_string(&output)
            .map_err(|error| format!("Failed to serialize Rhai result to JSON: {}", error))
    };
    drop_flat(output);
    result
}

impl EvalEngine {
    fn compile(&mut self, expression: &str) -> Result<Rc<AST>, String> {
        if let Some(cached) = self.cache.get(expression) {
            return cached.clone();
        }
        let compiled = self
            .engine
            .compile(expression)
            .map(Rc::new)
            .map_err(|error| format!("{}", error));
        if expression.len() <= AST_CACHE_MAX_EXPRESSION_LEN {
            if self.cache.len() >= AST_CACHE_CAPACITY {
                self.cache.clear();
            }
            self.cache.insert(expression.to_string(), compiled.clone());
        }
        compiled
    }

    fn eval(&mut self, expression: &str, mut scope: Scope<'static>) -> Result<String, String> {
        let ast = self.compile(expression)?;
        let result = self.engine.eval_ast_with_scope::<Dynamic>(&mut scope, &ast);
        // Variables left in the scope may hold deeply nested values too.
        scope.into_iter().for_each(|(_, value, _)| drop_flat(value));
        match result {
            Ok(output) => to_json(output),
            Err(error) => Err(format!("{}", error)),
        }
    }
}

/// Create an evaluation engine.
///
/// `interrupt` (optional) is polled with `interrupt_data` while scripts run; when it returns
/// true the running script is terminated. Returns null only if engine creation panicked.
/// The engine must be released with `evalexpr_engine_free`.
#[no_mangle]
pub extern "C" fn evalexpr_engine_new(
    limits: EvalLimits,
    interrupt: InterruptCallback,
    interrupt_data: *mut c_void,
) -> *mut EvalEngine {
    catch_unwind(AssertUnwindSafe(|| {
        Box::into_raw(Box::new(EvalEngine {
            engine: new_engine(&limits, interrupt, interrupt_data),
            cache: HashMap::new(),
        }))
    }))
    .unwrap_or(std::ptr::null_mut())
}

/// Free an engine created by `evalexpr_engine_new`.
#[no_mangle]
pub extern "C" fn evalexpr_engine_free(engine: *mut EvalEngine) {
    if engine.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        drop(Box::from_raw(engine));
    }));
}

/// One expression (and optional JSON context) to evaluate. A null/empty context means the
/// script runs without a `context` variable.
#[repr(C)]
pub struct EvalInput {
    pub expression: *const c_char,
    pub expression_len: usize,
    pub context_json: *const c_char,
    pub context_len: usize,
}

/// Run `f` on a large, separately allocated stack.
///
/// Rhai clones, compares and drops nested arrays/maps recursively, so a script that builds a
/// deeply nested value could otherwise overflow the (small) stack of a DuckDB worker thread
/// and abort the process. The stack is only reserved address space; pages are committed as used.
#[cfg(not(target_family = "wasm"))]
fn with_large_stack<R>(f: impl FnOnce() -> R) -> R {
    stacker::grow(EVAL_STACK_SIZE, f)
}

#[cfg(target_family = "wasm")]
fn with_large_stack<R>(f: impl FnOnce() -> R) -> R {
    f()
}

/// Evaluate `count` expressions, writing one result per input into `results`.
///
/// Each expression is compiled once and cached in the engine. Every result written (the JSON
/// result, or an error message) must be released with `free_result_cstring`.
#[no_mangle]
pub extern "C" fn evalexpr_engine_eval_batch(
    engine: *mut EvalEngine,
    inputs: *const EvalInput,
    count: usize,
    results: *mut ResultCString,
) {
    if count == 0 || inputs.is_null() || results.is_null() {
        return;
    }
    let inputs = unsafe { slice::from_raw_parts(inputs, count) };
    let results = unsafe { slice::from_raw_parts_mut(results, count) };
    // Placeholders, so every slot holds a valid (freeable) result whatever happens below.
    for slot in results.iter_mut() {
        *slot = ResultCString::Err(std::ptr::null_mut());
    }

    let outcome = catch_unwind(AssertUnwindSafe(|| {
        with_large_stack(|| {
            for (input, slot) in inputs.iter().zip(results.iter_mut()) {
                *slot = guard(|| {
                    if engine.is_null() {
                        return Err("The Rhai engine could not be created".to_string());
                    }
                    let engine = unsafe { &mut *engine };
                    let expression =
                        input_str(input.expression, input.expression_len, "expression")?;
                    let scope = make_scope(input.context_json, input.context_len)?;
                    engine.eval(expression, scope)
                });
            }
        })
    }));

    if let Err(payload) = outcome {
        let message = panic_message(payload);
        for slot in results.iter_mut() {
            if let ResultCString::Err(ptr) = slot {
                if ptr.is_null() {
                    *slot = ResultCString::Err(to_c_string(message.clone()));
                }
            }
        }
    }
}

/// Free a string returned by this library.
#[no_mangle]
pub extern "C" fn free_c_string(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        drop(CString::from_raw(ptr));
    }));
}

/// Free the string held by a `ResultCString` (either the value or the error).
#[no_mangle]
pub extern "C" fn free_result_cstring(result: ResultCString) {
    match result {
        ResultCString::Ok(ptr) | ResultCString::Err(ptr) => free_c_string(ptr),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limits() -> EvalLimits {
        EvalLimits {
            max_operations: 1_000_000,
            max_string_size: 1024,
            max_array_size: 1024,
            max_map_size: 1024,
            max_call_levels: 64,
        }
    }

    fn eval(engine: *mut EvalEngine, expr: &str, ctx: &str) -> Result<String, String> {
        let input = EvalInput {
            expression: expr.as_ptr() as *const c_char,
            expression_len: expr.len(),
            context_json: ctx.as_ptr() as *const c_char,
            context_len: ctx.len(),
        };
        let mut result = ResultCString::Err(std::ptr::null_mut());
        evalexpr_engine_eval_batch(engine, &input, 1, &mut result);
        let out = match result {
            ResultCString::Ok(p) => Ok(unsafe { std::ffi::CStr::from_ptr(p) }
                .to_string_lossy()
                .into_owned()),
            ResultCString::Err(p) => Err(unsafe { std::ffi::CStr::from_ptr(p) }
                .to_string_lossy()
                .into_owned()),
        };
        free_result_cstring(result);
        out
    }

    #[test]
    fn basics_and_limits() {
        let engine = evalexpr_engine_new(limits(), None, std::ptr::null_mut());
        assert_eq!(eval(engine, "40 + 2", ""), Ok("42".to_string()));
        assert_eq!(
            eval(engine, "context.a", "{\"a\": [1]}"),
            Ok("[1]".to_string())
        );
        assert!(eval(engine, "loop {}", "")
            .unwrap_err()
            .contains("Too many operations"));
        assert!(eval(engine, "let s = \"x\"; loop { s += s; }", "").is_err());
        assert!(eval(engine, "context", "{")
            .unwrap_err()
            .contains("JSON context"));
        assert!(eval(engine, "open_file(\"/etc/passwd\")", "").is_err());
        assert!(
            eval(engine, "let a = []; for i in 0..2000 { a.push(i); } 1", "")
                .unwrap_err()
                .contains("array")
        );
        evalexpr_engine_free(engine);
    }

    #[test]
    fn deep_nesting_does_not_overflow_the_stack() {
        // A 256 KiB thread (smaller than any DuckDB worker) would overflow without the large stack.
        std::thread::Builder::new()
            .stack_size(256 * 1024)
            .spawn(|| {
                let unlimited = EvalLimits {
                    max_operations: 0,
                    max_string_size: 0,
                    max_array_size: 0,
                    max_map_size: 0,
                    max_call_levels: 0,
                };
                let engine = evalexpr_engine_new(unlimited, None, std::ptr::null_mut());
                let e =
                    eval(engine, "let a = []; for i in 0..5000 { a = [a]; } a", "").unwrap_err();
                assert!(e.contains("nested"), "{}", e);
                assert_eq!(
                    eval(
                        engine,
                        "let a = []; for i in 0..5000 { a = [a]; } a == a",
                        ""
                    ),
                    Ok("true".to_string())
                );
                assert_eq!(
                    eval(
                        engine,
                        "let a = #{}; for i in 0..5000 { a = #{x: a}; } 1",
                        ""
                    ),
                    Ok("1".to_string())
                );
                evalexpr_engine_free(engine);
            })
            .unwrap()
            .join()
            .unwrap();
    }

    extern "C" fn always_interrupted(_: *mut c_void) -> bool {
        true
    }

    #[test]
    fn interrupt() {
        let mut l = limits();
        l.max_operations = 0;
        let engine = evalexpr_engine_new(l, Some(always_interrupted), std::ptr::null_mut());
        assert!(eval(engine, "loop {}", "").is_err());
        evalexpr_engine_free(engine);
    }
}
