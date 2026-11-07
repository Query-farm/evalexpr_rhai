#define DUCKDB_EXTENSION_MAIN

#include "evalexpr_rhai_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
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

    static constexpr const char *EVALEXPR_RHAI_VERSION = "2026110701";

    // Settings that bound every evaluation (0 means unlimited).
    struct LimitSetting
    {
        const char *name;
        const char *description;
        uint64_t default_value;
    };

    static const LimitSetting LIMIT_SETTINGS[] = {
        {"evalexpr_rhai_max_operations",
         "Maximum number of Rhai operations a single evalexpr_rhai() evaluation may perform (0 = unlimited)",
         100000000},
        {"evalexpr_rhai_max_string_size",
         "Maximum length in bytes of any string created by an evalexpr_rhai() script (0 = unlimited)", 16777216},
        {"evalexpr_rhai_max_array_size",
         "Maximum number of elements of any array created by an evalexpr_rhai() script (0 = unlimited)", 1000000},
        {"evalexpr_rhai_max_map_size",
         "Maximum number of properties of any object map created by an evalexpr_rhai() script (0 = unlimited)",
         1000000},
        {"evalexpr_rhai_max_call_levels",
         "Maximum depth of nested function calls in an evalexpr_rhai() script (0 = unlimited)", 64},
    };

    static uint64_t GetLimit(ClientContext &context, const LimitSetting &setting)
    {
        Value value;
        if (context.TryGetCurrentSetting(setting.name, value) && !value.IsNull())
        {
            return value.GetValue<uint64_t>();
        }
        return setting.default_value;
    }

    // Polled by the Rust engine while a script runs, so long-running scripts can be cancelled.
    static bool EvalexprRhaiIsInterrupted(void *data)
    {
        try
        {
            return static_cast<ClientContext *>(data)->IsInterrupted();
        }
        catch (...)
        {
            return false;
        }
    }

    struct RustEngineDeleter
    {
        void operator()(EvalEngine *ptr) const
        {
            evalexpr_engine_free(ptr);
        }
    };

    // One Rhai engine (with its packages, limits and compiled-expression cache) per thread.
    struct EvalexprRhaiLocalState : public FunctionLocalState
    {
        explicit EvalexprRhaiLocalState(ClientContext &context_p) : context(context_p)
        {
            EvalLimits limits;
            limits.max_operations = GetLimit(context, LIMIT_SETTINGS[0]);
            limits.max_string_size = GetLimit(context, LIMIT_SETTINGS[1]);
            limits.max_array_size = GetLimit(context, LIMIT_SETTINGS[2]);
            limits.max_map_size = GetLimit(context, LIMIT_SETTINGS[3]);
            limits.max_call_levels = GetLimit(context, LIMIT_SETTINGS[4]);
            engine.reset(evalexpr_engine_new(limits, EvalexprRhaiIsInterrupted, &context));
            if (!engine)
            {
                throw InternalException("evalexpr_rhai: failed to create the Rhai engine");
            }
        }

        ClientContext &context;
        unique_ptr<EvalEngine, RustEngineDeleter> engine;
        // Scratch buffers reused across chunks.
        vector<idx_t> rows;
        vector<EvalInput> inputs;
    };

    static unique_ptr<FunctionLocalState> EvalexprRhaiInitLocalState(ExpressionState &state,
                                                                     const BoundFunctionExpression &,
                                                                     FunctionData *)
    {
        return make_uniq<EvalexprRhaiLocalState>(state.GetContext());
    }

    // Owns the results handed out by the Rust library, so they are released on every path
    // (including when an exception is thrown while they are being copied into the vector).
    class RustResults
    {
    public:
        explicit RustResults(idx_t count) : values(count)
        {
            for (auto &value : values)
            {
                value.tag = ResultCString::Tag::Err;
                value.err._0 = nullptr;
            }
        }
        ~RustResults()
        {
            for (auto &value : values)
            {
                free_result_cstring(value);
            }
        }
        RustResults(const RustResults &) = delete;
        RustResults &operator=(const RustResults &) = delete;

        ResultCString *Data()
        {
            return values.data();
        }

    private:
        vector<ResultCString> values;
    };

    static void EvalexprRhaiFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<EvalexprRhaiLocalState>();
        auto &context = local_state.context;
        auto engine = local_state.engine.get();

        const bool has_context = args.ColumnCount() == 2;
        // Every row is evaluated, even when all arguments are constant: scripts may be
        // non-deterministic (rand(), shuffle(), ...), which is why the function is VOLATILE.
        // Compiled expressions are cached, so a constant script is only parsed once per thread.
        const idx_t count = args.size();

        UnifiedVectorFormat expression_format;
        args.data[0].ToUnifiedFormat(expression_format);
        auto expression_data = UnifiedVectorFormat::GetData<string_t>(expression_format);

        UnifiedVectorFormat context_format;
        const string_t *context_data = nullptr;
        if (has_context)
        {
            args.data[1].ToUnifiedFormat(context_format);
            context_data = UnifiedVectorFormat::GetData<string_t>(context_format);
        }

        result.SetVectorType(VectorType::FLAT_VECTOR);
        auto &entries = StructVector::GetEntries(result);
        auto union_tag_data = FlatVector::GetDataMutable<uint8_t>(entries[0]);
        auto union_ok_data = FlatVector::GetDataMutable<string_t>(entries[1]);
        auto union_error_data = FlatVector::GetDataMutable<string_t>(entries[2]);

        // Gather the rows with a non-NULL expression and evaluate them in one call.
        auto &rows = local_state.rows;
        auto &inputs = local_state.inputs;
        rows.clear();
        inputs.clear();
        for (idx_t i = 0; i < count; i++)
        {
            auto expression_idx = expression_format.sel->get_index(i);
            if (!expression_format.validity.RowIsValid(expression_idx))
            {
                FlatVector::SetNull(result, i, true);
                continue;
            }
            const auto &expression = expression_data[expression_idx];
            EvalInput input {expression.GetData(), expression.GetSize(), nullptr, 0};

            // A NULL (or absent) context evaluates the expression without a `context` variable.
            if (context_data)
            {
                auto context_idx = context_format.sel->get_index(i);
                if (context_format.validity.RowIsValid(context_idx))
                {
                    input.context_json = context_data[context_idx].GetData();
                    input.context_len = context_data[context_idx].GetSize();
                }
            }
            rows.push_back(i);
            inputs.push_back(input);
        }

        RustResults results(inputs.size());
        evalexpr_engine_eval_batch(engine, inputs.data(), inputs.size(), results.Data());
        if (context.IsInterrupted())
        {
            throw InterruptException();
        }

        for (idx_t r = 0; r < rows.size(); r++)
        {
            const auto i = rows[r];
            const auto &eval_result = results.Data()[r];
            const bool is_error = eval_result.tag == ResultCString::Tag::Err;
            const char *text = is_error ? eval_result.err._0 : eval_result.ok._0;
            text = text ? text : "";
            if (is_error)
            {
                union_tag_data[i] = 1;
                FlatVector::ValidityMutable(entries[1]).SetInvalid(i);
                union_ok_data[i] = string_t();
                union_error_data[i] = StringVector::AddStringOrBlob(entries[2], text);
            }
            else
            {
                union_tag_data[i] = 0;
                FlatVector::ValidityMutable(entries[2]).SetInvalid(i);
                union_ok_data[i] = StringVector::AddStringOrBlob(entries[1], text);
                union_error_data[i] = string_t();
            }
        }

    }

    // Declares a positional-only parameter with a real name, so duckdb_functions() reports it instead of col<N>.
    static FunctionParameter Positional(const char *name, LogicalType type)
    {
        return FunctionParameter(name, std::move(type), {}, FunctionParameterKind::POSITIONAL_ONLY);
    }

    static void LoadInternal(ExtensionLoader &loader)
    {
        auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
        for (auto &setting : LIMIT_SETTINGS)
        {
            config.AddExtensionOption(setting.name, setting.description, LogicalType::UBIGINT,
                                      Value::UBIGINT(setting.default_value));
        }

        ScalarFunctionSet evalexpr_rhai_set("evalexpr_rhai");

        child_list_t<LogicalType> members = {{"ok", LogicalType::JSON()}, {"error", LogicalType::VARCHAR}};

        // VOLATILE: scripts can call the random number functions (rand(), shuffle(), ...).
        auto evalexpr_with_context = ScalarFunction({Positional("expression", LogicalType::VARCHAR), Positional("context", LogicalType::JSON())}, LogicalType::UNION(members), EvalexprRhaiFunction);
        evalexpr_with_context.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
        evalexpr_with_context.SetVolatile();
        evalexpr_with_context.SetInitStateCallback(EvalexprRhaiInitLocalState);
        evalexpr_rhai_set.AddFunction(evalexpr_with_context);

        auto evalexpr_no_context = ScalarFunction({Positional("expression", LogicalType::VARCHAR)}, LogicalType::UNION(members), EvalexprRhaiFunction);
        evalexpr_no_context.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
        evalexpr_no_context.SetVolatile();
        evalexpr_no_context.SetInitStateCallback(EvalexprRhaiInitLocalState);
        evalexpr_rhai_set.AddFunction(evalexpr_no_context);

        evalexpr_rhai_set.SetFallible();
        CreateScalarFunctionInfo info(evalexpr_rhai_set);

        // Documentation for the single-argument variant
        FunctionDescription desc_no_context;
        desc_no_context.parameter_names = {"expression"};
        desc_no_context.parameter_types = {LogicalType::VARCHAR};
        desc_no_context.description = "Evaluate a Rhai scripting language expression and return the result as JSON (UNION of ok JSON / error VARCHAR). Evaluation is bounded by the evalexpr_rhai_max_* settings and can be cancelled.";
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

        QueryFarmSendTelemetry(loader, "evalexpr_rhai", EVALEXPR_RHAI_VERSION);
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
        return EVALEXPR_RHAI_VERSION;
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
