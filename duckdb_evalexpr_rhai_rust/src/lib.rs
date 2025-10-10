// duckdb_evalexpr_rust
// Copyright 2024-2025 Rusty Conover <rusty@query.farm>
// Licensed under the MIT License

use std::ffi::{c_char, CString};
use std::ptr;
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

macro_rules! make_str2 {
    ( $s : expr , $len : expr ) => {
        str::from_utf8_unchecked(slice::from_raw_parts($s as *const u8, $len))
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

/// Compile an expression into an AST
#[no_mangle]
pub extern "C" fn compile_ast(
    expression: *const c_char,
    expression_len: usize,
) -> *mut ResultCompiledAst {
    let expr_str = make_str!(expression, expression_len);
    let mut engine = Engine::new();

    engine.register_global_module(RandomPackage::new().as_shared_module());
    engine.register_global_module(FilesystemPackage::new().as_shared_module());
    engine.register_global_module(UrlPackage::new().as_shared_module());
    engine.register_global_module(SciPackage::new().as_shared_module());
    //    engine.register_global_module(ChronoPackage::new().as_shared_module());

    let ast = engine.compile(expr_str);

    match ast {
        Ok(ast) => {
            let compiled = Box::new(CompiledAst {
                engine: Box::new(engine),
                ast: Box::new(ast),
            });

            let result = Box::new(ResultCompiledAst::Ok(Box::into_raw(compiled)));
            Box::into_raw(result)
        }
        Err(error) => {
            let formatted_error = format!("{}", error);
            let error_str = CString::new(formatted_error).unwrap();
            let result = Box::new(ResultCompiledAst::Err(error_str.into_raw()));
            Box::into_raw(result)
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

/// Evaluate an AST with a context
///
/// The context is a JSON string that will be deserialized into a `Dynamic` object
/// and passed to the AST evaluation.
#[no_mangle]
pub extern "C" fn eval_ast(
    compiled: *mut CompiledAst,
    context_json: *const c_char,
    context_len: usize,
) -> ResultCString {
    if compiled.is_null() {
        return ResultCString::Ok(ptr::null_mut());
    }
    // The json context is optional.
    unsafe {
        let result = match context_len == 0 {
            false => {
                let context_str = make_str2!(context_json, context_len);

                // Deserialize 'Dynamic' from JSON
                let context: Dynamic =
                    serde_json::from_str(&context_str).expect("JSON context was not well formed.");

                // First create the state
                let mut scope = Scope::new();

                scope.push("context", context);

                // Use the context in an expression
                (*compiled)
                    .engine
                    .eval_ast_with_scope::<Dynamic>(&mut scope, &(*compiled).ast)
            }
            true => (*compiled).engine.eval_ast::<Dynamic>(&(*compiled).ast),
        };

        match result {
            Ok(output) => {
                let json = serde_json::to_string(&output)
                    .expect("Failed to serialize Rhai result to JSON");
                let value_str = CString::new(json).unwrap();
                ResultCString::Ok(value_str.into_raw())
            }
            Err(error) => {
                let formatted_error = format!("{}", error);
                let error_str = CString::new(formatted_error).unwrap();
                ResultCString::Err(error_str.into_raw())
            }
        }
    }
}

/// Evaluate an expression with a optional context
///
/// The context is a JSON string that will be deserialized into a `Dynamic` object
/// and passed to the expression evaluation.
#[no_mangle]
pub extern "C" fn perform_eval(
    expression: *const c_char,
    expression_len: usize,
    context_json: *const c_char,
    context_len: usize,
) -> ResultCString {
    if expression.is_null() || expression_len == 0 {
        return ResultCString::Ok(ptr::null_mut());
    }

    let expr_str = make_str!(expression, expression_len);

    let mut engine = Engine::new();

    engine.register_global_module(RandomPackage::new().as_shared_module());
    engine.register_global_module(FilesystemPackage::new().as_shared_module());
    engine.register_global_module(UrlPackage::new().as_shared_module());
    //    engine.register_global_module(ChronoPackage::new().as_shared_module());

    // The json context is optional.
    let result = match context_len == 0 {
        false => {
            let context_str = make_str!(context_json, context_len);

            // Deserialize 'Dynamic' from JSON
            let context: Dynamic = serde_json::from_str(&context_str).expect(
                format!("JSON context was not well formed, length {}", context_len).as_str(),
            );

            // First create the state
            let mut scope = Scope::new();

            scope.push("context", context);

            // Use the context in an expression
            engine.eval_with_scope::<Dynamic>(&mut scope, expr_str)
        }
        true => engine.eval::<Dynamic>(expr_str),
    };

    match result {
        Ok(output) => {
            let json = serde_json::to_string(&output).expect("Failed to serialize result to JSON");
            let value_str = CString::new(json).unwrap();
            ResultCString::Ok(value_str.into_raw())
        }
        Err(error) => {
            let formatted_error = format!("{}", error);
            let error_str = CString::new(formatted_error).unwrap();
            ResultCString::Err(error_str.into_raw())
        }
    }
}

#[cfg(test)]
mod tests {}


