#define DUCKDB_EXTENSION_MAIN

#include "evalexpr_rhai_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"

// Include the declarations of things from Rust.
#include "rust.h"
#include "query_farm_telemetry.hpp"

namespace duckdb
{

    // Owners of the objects handed out by the Rust library, so they are released on every path
    // (including when an exception is thrown while the result is being copied into the vector).
    struct RustCompileResultDeleter
    {
        void operator()(ResultCompiledAst *ptr) const
        {
            free_result_compiled_ast(ptr);
        }
    };

    struct RustAstDeleter
    {
        void operator()(CompiledAst *ptr) const
        {
            free_ast(ptr);
        }
    };

    class RustResultCString
    {
    public:
        explicit RustResultCString(ResultCString value_p) : value(value_p)
        {
        }
        ~RustResultCString()
        {
            free_result_cstring(value);
        }
        RustResultCString(const RustResultCString &) = delete;
        RustResultCString &operator=(const RustResultCString &) = delete;

        bool IsError() const
        {
            return value.tag == ResultCString::Tag::Err;
        }
        const char *Get() const
        {
            const char *ptr = IsError() ? value.err._0 : value.ok._0;
            return ptr ? ptr : "";
        }

    private:
        ResultCString value;
    };

    inline void evalexprFunc(DataChunk &args, ExpressionState &state, Vector &result)
    {

        auto &expression_vector = args.data[0];
        auto expression_is_constant = expression_vector.GetVectorType() == VectorType::CONSTANT_VECTOR;

        auto is_fully_constant = expression_is_constant;
        const auto count = args.size();

        UnifiedVectorFormat expression_vector_unified;
        expression_vector.ToUnifiedFormat(expression_vector_unified);

        auto &entries = StructVector::GetEntries(result);
        auto union_tag_data = FlatVector::GetDataMutable<uint8_t>(entries[0]);
        auto union_ok_data = FlatVector::GetDataMutable<string_t>(entries[1]);
        auto union_error_data = FlatVector::GetDataMutable<string_t>(entries[2]);

        auto set_error = [&](const char *error, idx_t i)
        {
            union_tag_data[i] = 1;
            FlatVector::ValidityMutable(entries[1]).SetInvalid(i);
            union_ok_data[i] = "";
            union_error_data[i] = StringVector::AddStringOrBlob(entries[2], error);
        };

        // Copies the result into the vector; the Rust string is freed when eval_result goes out of scope.
        auto process_result = [&](ResultCString result_p, idx_t i)
        {
            RustResultCString eval_result(result_p);
            if (eval_result.IsError())
            {
                set_error(eval_result.Get(), i);
            }
            else
            {
                union_tag_data[i] = 0;
                FlatVector::ValidityMutable(entries[2]).SetInvalid(i);
                union_ok_data[i] = StringVector::AddStringOrBlob(entries[1], eval_result.Get());
                union_error_data[i] = "";
            }
        };

        const string_t *context_json_data = nullptr;
        UnifiedVectorFormat context_json_vector_unified;
        bool context_is_constant = false;

        if (args.ColumnCount() == 2)
        {
            // There can be a context column.
            auto &context_json_vector = args.data[1];
            context_is_constant = context_json_vector.GetVectorType() == VectorType::CONSTANT_VECTOR;

            is_fully_constant = expression_is_constant && context_is_constant;

            context_json_vector.ToUnifiedFormat(context_json_vector_unified);
            context_json_data = UnifiedVectorFormat::GetData<string_t>(context_json_vector_unified);
        }

        // If the expression to evaluate is constant cache the compiled AST then just
        // evaluate that.
        if (expression_is_constant)
        {
            if (ConstantVector::IsNull(expression_vector))
            {
                result.SetVectorType(VectorType::CONSTANT_VECTOR);
                ConstantVector::SetNull(result, true);
            }
            else
            {
                const auto constant_expression = ConstantVector::GetData<string_t>(expression_vector)->GetString();

                // Compile the expression to an AST.
                unique_ptr<ResultCompiledAst, RustCompileResultDeleter> expression_as_ast_compiled(
                    compile_ast(constant_expression.c_str(), constant_expression.size()));

                if (expression_as_ast_compiled->tag == ResultCompiledAst::Tag::Err)
                {
                    // The error message is copied into the vector, and freed together with the compile result.
                    const char *error = expression_as_ast_compiled->err._0 ? expression_as_ast_compiled->err._0 : "";
                    for (idx_t i = 0; i < (is_fully_constant ? 1 : count); i++)
                    {
                        set_error(error, i);
                    }
                }
                else
                {
                    // The compiled AST is owned by us (not by the compile result) and freed when this scope ends.
                    unique_ptr<CompiledAst, RustAstDeleter> compiled_ast(expression_as_ast_compiled->ok._0);

                    for (idx_t i = 0; i < (is_fully_constant ? 1 : count); i++)
                    {
                        auto context_idx = context_json_data ? context_json_vector_unified.sel->get_index(i) : 0;
                        if (context_json_data && context_json_vector_unified.validity.RowIsValid(context_idx))
                        {
                            process_result(eval_ast(compiled_ast.get(),
                                                    context_json_data[context_idx].GetData(),
                                                    context_json_data[context_idx].GetSize()),
                                           i);
                        }
                        else
                        {
                            process_result(eval_ast(compiled_ast.get(), nullptr, 0), i);
                        }
                    }
                }
            }
        }
        else
        {
            UnifiedVectorFormat expression_vector_unified;
            expression_vector.ToUnifiedFormat(expression_vector_unified);

            auto expression_data = UnifiedVectorFormat::GetData<string_t>(expression_vector_unified);
            ResultCString eval_result;

            for (idx_t i = 0; i < (is_fully_constant ? 1 : count); i++)
            {
                auto idx = expression_vector_unified.sel->get_index(i);

                if (!expression_vector_unified.validity.RowIsValid(idx))
                {
                    FlatVector::SetNull(result, i, true);
                    continue;
                }

                auto context_idx = context_json_data ? context_json_vector_unified.sel->get_index(i) : 0;
                if (context_json_data && context_json_vector_unified.validity.RowIsValid(context_idx))
                {
                    eval_result = perform_eval(
                        expression_data[idx].GetData(), expression_data[idx].GetSize(),
                        context_json_data[context_idx].GetData(),
                        context_json_data[context_idx].GetSize());
                }
                else
                {
                    eval_result = perform_eval(
                        expression_data[idx].GetData(), expression_data[idx].GetSize(),
                        nullptr, 0);
                }
                process_result(eval_result, i);
            }
        }

        if (is_fully_constant)
        {
            result.SetVectorType(VectorType::CONSTANT_VECTOR);
        }
    }

    // Extension initalization.
    static void LoadInternal(ExtensionLoader &loader)
    {
        ScalarFunctionSet evalexpr_rhai_set("evalexpr_rhai");

        child_list_t<LogicalType> members = {{"ok", LogicalType::JSON()}, {"error", LogicalType::VARCHAR}};

        auto evalexpr_with_context = ScalarFunction({LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::UNION(members), evalexprFunc);
        evalexpr_with_context.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
        evalexpr_with_context.SetVolatile();
        evalexpr_rhai_set.AddFunction(evalexpr_with_context);

        auto evalexpr_no_context = ScalarFunction({LogicalType::VARCHAR}, LogicalType::UNION(members), evalexprFunc);
        evalexpr_no_context.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
        evalexpr_no_context.SetVolatile();
        evalexpr_rhai_set.AddFunction(evalexpr_no_context);

        evalexpr_rhai_set.SetFallible();
        CreateScalarFunctionInfo info(evalexpr_rhai_set);

        // Documentation for the single-argument variant
        FunctionDescription desc_no_context;
        desc_no_context.parameter_names = {"expression"};
        desc_no_context.parameter_types = {LogicalType::VARCHAR};
        desc_no_context.description = "Evaluate a Rhai scripting language expression and return the result as JSON";
        desc_no_context.examples = {"evalexpr_rhai('40 + 2')", "evalexpr_rhai('[1, 2, 3].map(|x| x * 2)')"};
        desc_no_context.categories = {"scripting", "json"};
        info.descriptions.push_back(std::move(desc_no_context));

        // Documentation for the two-argument variant with context
        FunctionDescription desc_with_context;
        desc_with_context.parameter_names = {"expression", "context"};
        desc_with_context.parameter_types = {LogicalType::VARCHAR, LogicalType::JSON()};
        desc_with_context.description = "Evaluate a Rhai scripting language expression with a JSON context object accessible as 'context'";
        desc_with_context.examples = {"evalexpr_rhai('context.x + context.y', '{\"x\": 10, \"y\": 20}')", "evalexpr_rhai('context.items.len()', '{\"items\": [1, 2, 3]}')"};
        desc_with_context.categories = {"scripting", "json"};
        info.descriptions.push_back(std::move(desc_with_context));

        loader.RegisterFunction(std::move(info));

        QueryFarmSendTelemetry(loader, "evalexpr_rhai", "2026072501");
    }

    void EvalexprRhaiExtension::Load(ExtensionLoader &loader)
    {
        LoadInternal(loader);
    }

    std::string EvalexprRhaiExtension::Name()
    {
        return "evalexpr_rhai";
    }

    std::string EvalexprRhaiExtension::Version() const
    {
        return "2026072501";
    }

} // namespace duckdb

extern "C"
{

    DUCKDB_CPP_EXTENSION_ENTRY(evalexpr_rhai, loader)
    {
        duckdb::LoadInternal(loader);
    }
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
