#define DUCKDB_EXTENSION_MAIN

#include "evalexpr_rhai_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "query_farm_telemetry.hpp"
// Include the declarations of things from Rust.
#include "rust.h"

namespace duckdb
{

    inline void evalexprFunc(DataChunk &args, ExpressionState &state, Vector &result)
    {

        auto &expression_vector = args.data[0];
        auto expression_is_constant = expression_vector.GetVectorType() == VectorType::CONSTANT_VECTOR;

        auto is_fully_constant = expression_is_constant;
        const auto count = args.size();

        UnifiedVectorFormat expression_vector_unified;
        expression_vector.ToUnifiedFormat(count, expression_vector_unified);

        auto &entries = StructVector::GetEntries(result);
        auto union_tag_data = FlatVector::GetData<uint8_t>(*entries[0]);
        auto union_ok_data = FlatVector::GetData<string_t>(*entries[1]);
        auto union_error_data = FlatVector::GetData<string_t>(*entries[2]);

        auto process_result = [&](ResultCString &eval_result, idx_t i)
        {
            if (eval_result.tag == ResultCString::Tag::Err)
            {
                union_tag_data[i] = 1;
                FlatVector::Validity(*entries[1]).SetInvalid(i);
                union_ok_data[i] = "";
                union_error_data[i] = eval_result.err._0;
            }
            else
            {
                union_tag_data[i] = 0;
                FlatVector::Validity(*entries[2]).SetInvalid(i);
                union_ok_data[i] = eval_result.ok._0;
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

            context_json_vector.ToUnifiedFormat(count, context_json_vector_unified);
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
                auto expression_as_ast_compiled = compile_ast(constant_expression.c_str(), constant_expression.size());

                if (expression_as_ast_compiled->tag == ResultCompiledAst::Tag::Err)
                {
                    for (idx_t i = 0; i < (is_fully_constant ? 1 : count); i++)
                    {
                        union_tag_data[i] = 1;
                        FlatVector::Validity(*entries[1]).SetInvalid(i);
                        union_ok_data[i] = "";
                        union_error_data[i] = expression_as_ast_compiled->err._0;
                    }
                }
                else
                {
                    ResultCString eval_result;

                    for (idx_t i = 0; i < (is_fully_constant ? 1 : count); i++)
                    {
                        if (context_json_data && context_json_vector_unified.validity.RowIsValid(context_json_vector_unified.sel->get_index(i)))
                        {
                            eval_result = eval_ast(expression_as_ast_compiled->ok._0,
                                                   context_json_data[context_is_constant ? 0 : i].GetData(),
                                                   context_json_data[context_is_constant ? 0 : i].GetSize());
                        }
                        else
                        {
                            eval_result = eval_ast(expression_as_ast_compiled->ok._0,
                                                   nullptr, 0);
                        }

                        process_result(eval_result, i);
                    }
                    free_ast(expression_as_ast_compiled->ok._0);
                }
            }
        }
        else
        {
            UnifiedVectorFormat expression_vector_unified;
            expression_vector.ToUnifiedFormat(count, expression_vector_unified);

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

                if (context_json_data && context_json_vector_unified.validity.RowIsValid(context_json_vector_unified.sel->get_index(i)))
                {
                    eval_result = perform_eval(
                        expression_data[i].GetData(), expression_data[i].GetSize(),
                        context_json_data[context_is_constant ? 0 : i].GetData(),
                        context_json_data[context_is_constant ? 0 : i].GetSize());
                }
                else
                {
                    eval_result = perform_eval(
                        expression_data[i].GetData(), expression_data[i].GetSize(),
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

    extern "C" void *duckdb_malloc(size_t size);
    extern "C" void duckdb_free(void *ptr);

    // Extension initalization.
    static void LoadInternal(ExtensionLoader &loader)
    {
        init_memory_allocation(duckdb_malloc, duckdb_free);

        ScalarFunctionSet evalexpr_rhai("evalexpr_rhai");

        child_list_t<LogicalType> members = {{"ok", LogicalType::JSON()}, {"error", LogicalType::VARCHAR}};

        auto evalexpr_with_context = ScalarFunction({LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::UNION(members), evalexprFunc);
        evalexpr_with_context.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
        evalexpr_with_context.stability = FunctionStability::VOLATILE;
        evalexpr_rhai.AddFunction(evalexpr_with_context);

        auto evalexpr_no_context = ScalarFunction({LogicalType::VARCHAR}, LogicalType::UNION(members), evalexprFunc);
        evalexpr_no_context.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
        evalexpr_no_context.stability = FunctionStability::VOLATILE;
        evalexpr_rhai.AddFunction(evalexpr_no_context);

        loader.RegisterFunction(evalexpr_rhai);

        QueryFarmSendTelemetry(loader, "evalexpr_rhai", "2025092301");
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
        return "2025092301";
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
