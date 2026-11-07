# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is `evalexpr_rhai`, a DuckDB extension that enables evaluation of [Rhai](https://rhai.rs) scripting language expressions within SQL statements. It allows dynamic expression evaluation with JSON context passing.

## Build Commands

This project does not use vcpkg.

```bash
# Build release version
GEN=ninja make release

# Build debug version
GEN=ninja make debug

# Run all tests (requires building first)
make test              # runs against release build
make test_debug        # runs against debug build

# Format code
make format

```

All extension functions should be documented inside of DuckDB with CreateScalarFunctionInfo or CreateAggregateFunctionInfo or the appropriate type for the function.  This documentation of the function should include examples, parameter types and parameter names.  The function should be categorized.

When making changes the version should always be updated to the current date plus an ordinal counter in the form of YYYYMMDDCC.


## Architecture

**Hybrid C++/Rust Extension:**
- C++ layer (`src/evalexpr_rhai_extension.cpp`) handles DuckDB integration, registering the `evalexpr_rhai` scalar function
- Rust library (`duckdb_evalexpr_rhai_rust/`) implements the Rhai scripting engine and expression evaluation
- FFI interface defined in Rust, with C++ headers auto-generated via `cbindgen` into `src/include/rust.h`
- CMake uses [Corrosion](https://github.com/corrosion-rs/corrosion) to integrate Cargo builds

**Key Components:**
- `evalexpr_engine_new()` / `evalexpr_engine_free()` - one Rhai engine per DuckDB thread (FunctionLocalState),
  created with the `evalexpr_rhai_max_*` limits and an interrupt callback (`ClientContext::IsInterrupted`)
- `evalexpr_engine_eval_batch()` - evaluates a whole chunk on a large, separately allocated stack (stacker),
  caching compiled ASTs per engine
- Every `extern "C"` function is `catch_unwind`-guarded; returned strings are freed with `free_result_cstring`
- Results returned as `UNION['ok': JSON, 'error': VARCHAR]` type
- After changing the FFI, regenerate `src/include/rust.h` with `make rust_binding_headers` (needs cbindgen)

**Rhai Packages Enabled:**
- RandomPackage, UrlPackage, SciPackage (without its `io` feature)
- Never register packages that reach the host (rhai-fs, rhai-sci `io`, network): they bypass
  `enable_external_access` / `allowed_directories`.

## Testing

SQL tests are in `test/sql/`. Run with:
```bash
make test
```

Individual test file: `./build/release/test/unittest "test/sql/evalexpr_rhai.test"`

## Build Outputs

```
./build/release/duckdb                                           # Shell with extension loaded
./build/release/test/unittest                                    # Test runner
./build/release/extension/evalexpr_rhai/evalexpr_rhai.duckdb_extension  # Loadable extension
```

## Submodules

- `duckdb/` - DuckDB source (git submodule)
- `extension-ci-tools/` - DuckDB extension CI tooling (git submodule)
