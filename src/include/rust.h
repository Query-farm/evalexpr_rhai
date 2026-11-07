#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>


/// A Rhai engine with its packages registered, the configured limits applied, and a cache of
/// compiled expressions. One is created per DuckDB thread (function local state) and must be
/// released with `evalexpr_engine_free`. It is not thread safe.
struct EvalEngine;

/// One expression (and optional JSON context) to evaluate. A null/empty context means the
/// script runs without a `context` variable.
struct EvalInput {
  const char *expression;
  size_t expression_len;
  const char *context_json;
  size_t context_len;
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

/// Resource limits applied to every evaluation. A value of 0 means "unlimited".
struct EvalLimits {
  /// Maximum number of Rhai operations per evaluation.
  uint64_t max_operations;
  /// Maximum length (in bytes) of any string.
  uint64_t max_string_size;
  /// Maximum number of elements in any array.
  uint64_t max_array_size;
  /// Maximum number of properties in any object map.
  uint64_t max_map_size;
  /// Maximum depth of nested function calls.
  uint64_t max_call_levels;
};

/// Callback polled while a script runs; returns true when the query was interrupted.
using InterruptCallback = bool(*)(void*);


extern "C" {

/// Evaluate `count` expressions, writing one result per input into `results`.
///
/// Each expression is compiled once and cached in the engine. Every result written (the JSON
/// result, or an error message) must be released with `free_result_cstring`.
void evalexpr_engine_eval_batch(EvalEngine *engine,
                                const EvalInput *inputs,
                                size_t count,
                                ResultCString *results);

/// Free an engine created by `evalexpr_engine_new`.
void evalexpr_engine_free(EvalEngine *engine);

/// Create an evaluation engine.
///
/// `interrupt` (optional) is polled with `interrupt_data` while scripts run; when it returns
/// true the running script is terminated. Returns null only if engine creation panicked.
/// The engine must be released with `evalexpr_engine_free`.
EvalEngine *evalexpr_engine_new(EvalLimits limits,
                                InterruptCallback interrupt,
                                void *interrupt_data);

/// Free a string returned by this library.
void free_c_string(char *ptr);

/// Free the string held by a `ResultCString` (either the value or the error).
void free_result_cstring(ResultCString result);

}  // extern "C"
