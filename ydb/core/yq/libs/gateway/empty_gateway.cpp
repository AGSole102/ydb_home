#include "empty_gateway.h"

#include <ydb/core/yq/libs/graph_params/proto/graph_params.pb.h>
#include <ydb/core/yq/libs/events/events.h>

#include <library/cpp/actors/core/actor.h>

namespace NYq {

class TEmptyGateway : public NYql::IDqGateway {
public:
    explicit TEmptyGateway(NActors::TActorId runActorId) : RunActorId(runActorId) {
    }

    NThreading::TFuture<void> OpenSession(const TString& sessionId, const TString& username) override {
        Y_UNUSED(sessionId);
        Y_UNUSED(username);
        auto result = NThreading::NewPromise<void>();
        result.SetValue();
        return result;
    }

    void CloseSession(const TString& action) override {
        Y_UNUSED(action);
    }

    NThreading::TFuture<TResult> ExecutePlan(
        const TString& sessionId,
        NYql::NDqs::IDqsExecutionPlanner& plan,
        const TVector<TString>& columns,
        const THashMap<TString, TString>& secureParams,
        const THashMap<TString, TString>& queryParams,
        const NYql::TDqSettings::TPtr& settings,
        const TDqProgressWriter& progressWriter,
        const THashMap<TString, TString>& modulesMapping,
        bool discard) override
    {
        Y_UNUSED(progressWriter);
        Y_UNUSED(modulesMapping); // TODO: support.
        Y_UNUSED(discard);
        Y_UNUSED(queryParams);

        NProto::TGraphParams params;
        for (auto&& task : plan.GetTasks()) {
            *params.AddTasks() = std::move(task);
        }
        for (auto&& col : columns) {
            *params.AddColumns() = col;
        }
        for (auto&& [k, v] : secureParams) {
            (*params.MutableSecureParams())[k] = v;
        }
        settings->Save(params);
        if (plan.GetSourceID()) {
            params.SetSourceId(plan.GetSourceID().NodeId() - 1);
            params.SetResultType(plan.GetResultType());
        }
        params.SetSession(sessionId);

        NActors::TActivationContext::Send(new NActors::IEventHandle(RunActorId, {}, new TEvents::TEvGraphParams(params)));

        auto result = NThreading::NewPromise<NYql::IDqGateway::TResult>();
        NYql::IDqGateway::TResult gatewayResult;
        // fake it till you make it
        // generate dummy result for YQL facade now, remove this gateway completely
        // when top-level YQL facade call like Preprocess() is implemented
        if (plan.GetResultType()) {
            // for resultable graphs return dummy "select 1" result (it is not used and is required to satisfy YQL facade only)
            gatewayResult.SetSuccess();
            gatewayResult.Data = "[[\001\0021]]";
            gatewayResult.Truncated = true;
            gatewayResult.RowsCount = 0;
        } else {
            // for resultless results expect infinite INSERT FROM SELECT and fail YQL facade (with well known secret code?)
            gatewayResult.Issues.AddIssues({NYql::TIssue("MAGIC BREAK").SetCode(555, NYql::TSeverityIds::S_ERROR)});
        }
        result.SetValue(gatewayResult);
        return result;
    }

private:
    NActors::TActorId RunActorId;
};

TIntrusivePtr<NYql::IDqGateway> CreateEmptyGateway(NActors::TActorId runActorId) {
    return MakeIntrusive<TEmptyGateway>(runActorId);
}

} // namespace NYq
