#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>


/// A compiled AST
///
/// This struct is used to store the compiled AST and the engine that compiled it.
/// need to be freed using the `free_ast` function.
struct CompiledAst;

/// A result of a compiled AST
///
/// This enum is used to return the result of a compiled AST. It can either be an
/// `Ok` with a pointer to a `CompiledAst` or an `Err` with a pointer to a `c_char`
/// that contains the error message.
struct ResultCompiledAst {
  enum class Tag {
    Ok,
    Err,
  };

  struct Ok_Body {
    CompiledAst *_0;
  };

  struct Err_Body {
    char *_0;
  };

  Tag tag;
  union {
    Ok_Body ok;
    Err_Body err;
  };
};

struct ResultCString {
  enum class Tag {
    Ok,
    Err,
  };

  struct Ok_Body {
    char *_0;
  };

  struct Err_Body {
    char *_0;
  };

  Tag tag;
  union {
    Ok_Body ok;
    Err_Body err;
  };
};


extern "C" {

/// Compile an expression into an AST
///
/// The returned pointer must be released with `free_result_compiled_ast`. On success the
/// compiled AST it contains is owned by the caller and must be released with `free_ast`.
ResultCompiledAst *compile_ast(const char *expression, size_t expression_len);

/// Evaluate an AST with a context
///
/// The context is a JSON string that will be deserialized into a `Dynamic` object
/// and passed to the AST evaluation. The returned string must be released with
/// `free_result_cstring`.
ResultCString eval_ast(CompiledAst *compiled, const char *context_json, size_t context_len);

void free_ast(CompiledAst *ptr);

/// Free a string returned by this library.
void free_c_string(char *ptr);

/// Free the result of `compile_ast`.
///
/// This frees the result itself and, for an error, its message. The compiled AST of a
/// successful result is NOT freed (it is released separately with `free_ast`).
void free_result_compiled_ast(ResultCompiledAst *ptr);

/// Free the string held by a `ResultCString` (either the value or the error).
void free_result_cstring(ResultCString result);

/// Evaluate an expression with a optional context
///
/// The context is a JSON string that will be deserialized into a `Dynamic` object
/// and passed to the expression evaluation. The returned string must be released with
/// `free_result_cstring`.
ResultCString perform_eval(const char *expression,
                           size_t expression_len,
                           const char *context_json,
                           size_t context_len);

}  // extern "C"
