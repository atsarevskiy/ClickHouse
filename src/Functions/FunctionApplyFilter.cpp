#include <memory>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionApplyFilter.h>
#include <Functions/IFunction.h>
#include <Functions/IFunctionAdaptors.h>
#include <Interpreters/Context.h>
#include <Interpreters/BloomFilter.h>
#include <Processors/QueryPlan/RuntimeFilterLookup.h>
#include <IO/WriteHelpers.h>
#include <Common/CurrentThread.h>
#include <Common/FunctionDocumentation.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int TOO_FEW_ARGUMENTS_FOR_FUNCTION;
    extern const int LOGICAL_ERROR;
}

/// Special function for JOIN runtime filtering
/// Syntax: __applyFilter(label, key)
/// - label: a String const whose NAME and VALUE are both the STABLE structural id
///   (`_runtime_filter_<hash>`). It exists only to carry that id into the DAG for EXPLAIN and the
///   plan-step hash; the function ignores its value at execution.
/// - key: Value of any type that is checked to be present in the filter.
/// The actual RFL rendezvous key is the RANDOM `random_key` carried as instance state (set at plan
/// build, never materialized in the plan), so the random value never enters any hash. A
/// default-constructed instance (empty key, e.g. a deserialized plan) is inert: all rows pass.
/// Returns false if the key should be filtered
class FunctionApplyFilter final : public IFunction
{
public:
    static constexpr auto name = "__applyFilter";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionApplyFilter>(); }

    explicit FunctionApplyFilter(String random_key_ = {}) : random_key(std::move(random_key_)) {}

    String getName() const override { return name; }

    bool isVariadic() const override { return false; }
    bool isInjective(const ColumnsWithTypeAndName &) const override { return false; }

    /// A runtime filter's result is not a pure function of its arguments — it depends on the
    /// dynamically built filter, which differs between executions of the same plan (e.g. recursive
    /// CTE iterations or materialized-view blocks). `isDeterministic() == false` keeps it out of
    /// the query condition cache (which keys on the now-deterministic filter expression and would
    /// otherwise serve a stale per-granule result to a later execution with different keys). We
    /// keep `isDeterministicInScopeOfQuery() == true` so the filter can still be pushed into
    /// PREWHERE: within a single read the built filter is fixed, so the predicate is stable there.
    bool isDeterministic() const override { return false; }

    bool isSuitableForConstantFolding() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }
    size_t getNumberOfArguments() const override { return 2; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() != 2)
            throw Exception(ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION,
                            "Number of arguments for function {} can't be {}, should be 2",
                            getName(), arguments.size());

        if (!WhichDataType(arguments[0]).isString())
            throw Exception(
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "First argument of function '{}' must be a String filter key",
                    getName());

        return std::make_shared<DataTypeUInt8>();
    }

    DataTypePtr getReturnTypeForDefaultImplementationForDynamic() const override
    {
        return std::make_shared<DataTypeUInt8>();
    }

    bool useDefaultImplementationForConstants() const override { return true; }
    bool useDefaultImplementationForNulls() const override { return false; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        /// `random_key` is the per-plan-build RFL rendezvous key, carried as instance state (not in
        /// the DAG). An empty key means an inert (e.g. deserialized) instance: all rows pass.
        if (random_key.empty())
            return DataTypeUInt8().createColumnConst(input_rows_count, true);

        auto query_context = CurrentThread::tryGetQueryContext();
        if (!query_context)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Query context is not available for {}", getName());
        auto filter_lookup = query_context->getRuntimeFilterLookup();
        if (!filter_lookup)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Runtime filter lookup was not initialized");

        /// Look up the filter by the random key; if it has not been registered/built yet, all rows pass.
        auto filter = filter_lookup->find(random_key);
        if (!filter)
            return DataTypeUInt8().createColumnConst(input_rows_count, true);

        const auto & data_column = arguments[1];

        return filter->find(data_column);
    }

private:
    /// Random per-plan-build RFL key; never appears in the plan/DAG/EXPLAIN, so it is never hashed.
    const String random_key;
};

/// Build an `__applyFilter` overload resolver whose function instance carries `random_key` out of band.
FunctionOverloadResolverPtr createApplyFilterOverloadResolver(String random_key)
{
    return std::make_shared<FunctionToOverloadResolverAdaptor>(std::make_shared<FunctionApplyFilter>(std::move(random_key)));
}

REGISTER_FUNCTION(FilterContains)
{
    factory.registerFunction<FunctionApplyFilter>(FunctionDocumentation::INTERNAL_FUNCTION_DOCS, FunctionFactory::Case::Sensitive);
}

}
