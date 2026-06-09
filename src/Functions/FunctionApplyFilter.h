#pragma once

#include <Core/Types.h>
#include <memory>

namespace DB
{

class IFunctionOverloadResolver;
using FunctionOverloadResolverPtr = std::shared_ptr<IFunctionOverloadResolver>;

/// Build an `__applyFilter` overload resolver whose function instance carries `random_key` out of
/// band (as instance state, not as a DAG argument). The random key is the per-plan-build rendezvous
/// key in the query-context `IRuntimeFilterLookup`; keeping it off the plan means it never enters
/// any plan-step hash, while the stable structural id travels as the `__applyFilter` label argument.
FunctionOverloadResolverPtr createApplyFilterOverloadResolver(String random_key);

}
