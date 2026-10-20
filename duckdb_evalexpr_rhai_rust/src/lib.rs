// duckdb_evalexpr_rust
// Copyright 2024-2025 Rusty Conover <rusty@query.farm>
// Licensed under the MIT License

use std::ffi::{c_char, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;
use std::str;

use rhai::{packages::Package, Dynamic, Engine, Scope, AST};
//use rhai_chrono::ChronoPackage;
use rhai_fs::FilesystemPackage;
use rhai_rand::RandomPackage;
use rhai_sci::SciPackage;
use rhai_url::UrlPackage;

#[repr(C)]
pub enum ResultCString {
    Ok(*mut c_char),
    Err(*mut c_char),
}

macro_rules! make_str {
    ( $s : expr , $len : expr ) => {
        unsafe { str::from_utf8_unchecked(slice::from_raw_parts($s as *const u8, $len)) }
    };
}

/// A compiled AST
///
/// This struct is used to store the compiled AST and the engine that compiled it.
/// need to be freed using the `free_ast` function.
pub struct CompiledAst {
    engine: Box<Engine>,
    ast: Box<AST>,
}

/// A result of a compiled AST
///
/// This enum is used to return the result of a compiled AST. It can either be an
/// `Ok` with a pointer to a `CompiledAst` or an `Err` with a pointer to a `c_char`
/// that contains the error message.
#[repr(C)]
pub enum ResultCompiledAst {
    Ok(*mut CompiledAst),
    Err(*mut c_char),
}

/// Convert a Rust string into a C string owned by Rust.
///
/// Interior NUL bytes (which a C string cannot hold) are replaced, so this never fails.
/// The result must be released with `free_c_string` (or one of the `free_result_*` functions).
fn to_c_string(value: String) -> *mut c_char {
    let value = match CString::new(value) {
        Ok(value) => value,
        Err(error) => {
            let mut bytes = error.into_vec();
            bytes.retain(|&b| b != 0);
            CString::new(bytes).expect("NUL bytes were removed")
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

fn new_engine() -> Engine {
    let mut engine = Engine::new();

    engine.register_global_module(RandomPackage::new().as_shared_module());
    engine.register_global_module(FilesystemPackage::new().as_shared_module());
    engine.register_global_module(UrlPackage::new().as_shared_module());
    engine.register_global_module(SciPackage::new().as_shared_module());
    //    engine.register_global_module(ChronoPackage::new().as_shared_module());
    engine
}

/// Parse the optional JSON context into a scope that exposes it as `context`.
fn make_scope(context_json: *const c_char, context_len: usize) -> Result<Option<Scope<'static>>, String> {
    if context_json.is_null() || context_len == 0 {
        return Ok(None);
    }
    let context_str = make_str!(context_json, context_len);
    // Deserialize 'Dynamic' from JSON
    let context: Dynamic = serde_json::from_str(context_str)
        .map_err(|error| format!("JSON context was not well formed: {}", error))?;
    let mut scope = Scope::new();
    scope.push("context", context);
    Ok(Some(scope))
}

fn to_json(output: Dynamic) -> Result<String, String> {
    serde_json::to_string(&output).map_err(|error| format!("Failed to serialize Rhai result to JSON: {}", error))
}

/// Compile an expression into an AST
///
/// The returned pointer must be released with `free_result_compiled_ast`. On success the
/// compiled AST it contains is owned by the caller and must be released with `free_ast`.
#[no_mangle]
pub extern "C" fn compile_ast(
    expression: *const c_char,
    expression_len: usize,
) -> *mut ResultCompiledAst {
    let compiled = catch_unwind(AssertUnwindSafe(|| {
        let expr_str = if expression.is_null() { "" } else { make_str!(expression, expression_len) };
        let engine = new_engine();
        match engine.compile(expr_str) {
            Ok(ast) => Ok(CompiledAst {
                engine: Box::new(engine),
                ast: Box::new(ast),
            }),
            Err(error) => Err(format!("{}", error)),
        }
    }));

    let result = match compiled {
        Ok(Ok(compiled)) => ResultCompiledAst::Ok(Box::into_raw(Box::new(compiled))),
        Ok(Err(error)) => ResultCompiledAst::Err(to_c_string(error)),
        Err(payload) => ResultCompiledAst::Err(to_c_string(panic_message(payload))),
    };
    Box::into_raw(Box::new(result))
}

/// Free the result of `compile_ast`.
///
/// This frees the result itself and, for an error, its message. The compiled AST of a
/// successful result is NOT freed (it is released separately with `free_ast`).
#[no_mangle]
pub extern "C" fn free_result_compiled_ast(ptr: *mut ResultCompiledAst) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let result = Box::from_raw(ptr);
        if let ResultCompiledAst::Err(error) = *result {
            free_c_string(error);
        }
    }
}

#[no_mangle]
pub extern "C" fn free_ast(ptr: *mut CompiledAst) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let _ = Box::from_raw(ptr);
    }
}

/// Free a string returned by this library.
#[no_mangle]
pub extern "C" fn free_c_string(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let _ = CString::from_raw(ptr);
    }
}

/// Free the string held by a `ResultCString` (either the value or the error).
#[no_mangle]
pub extern "C" fn free_result_cstring(result: ResultCString) {
    match result {
        ResultCString::Ok(ptr) | ResultCString::Err(ptr) => free_c_string(ptr),
    }
}

/// Evaluate an AST with a context
///
/// The context is a JSON string that will be deserialized into a `Dynamic` object
/// and passed to the AST evaluation. The returned string must be released with
/// `free_result_cstring`.
#[no_mangle]
pub extern "C" fn eval_ast(
    compiled: *mut CompiledAst,
    context_json: *const c_char,
    context_len: usize,
) -> ResultCString {
    guard(|| {
        if compiled.is_null() {
            return Err("No compiled expression".to_string());
        }
        let compiled = unsafe { &*compiled };
        // The json context is optional.
        let result = match make_scope(context_json, context_len)? {
            Some(mut scope) => compiled.engine.eval_ast_with_scope::<Dynamic>(&mut scope, &compiled.ast),
            None => compiled.engine.eval_ast::<Dynamic>(&compiled.ast),
        };
        match result {
            Ok(output) => to_json(output),
            Err(error) => Err(format!("{}", error)),
        }
    })
}

/// Evaluate an expression with a optional context
///
/// The context is a JSON string that will be deserialized into a `Dynamic` object
/// and passed to the expression evaluation. The returned string must be released with
/// `free_result_cstring`.
#[no_mangle]
pub extern "C" fn perform_eval(
    expression: *const c_char,
    expression_len: usize,
    context_json: *const c_char,
    context_len: usize,
) -> ResultCString {
    guard(|| {
        let expr_str = if expression.is_null() { "" } else { make_str!(expression, expression_len) };
        let engine = new_engine();
        // The json context is optional.
        let result = match make_scope(context_json, context_len)? {
            Some(mut scope) => engine.eval_with_scope::<Dynamic>(&mut scope, expr_str),
            None => engine.eval::<Dynamic>(expr_str),
        };
        match result {
            Ok(output) => to_json(output),
            Err(error) => Err(format!("{}", error)),
        }
    })
}

#[cfg(test)]
mod tests {}


