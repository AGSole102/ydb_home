#include "utils.h"

#include <random>

#include <util/datetime/base.h>

#include <ydb/core/yq/libs/control_plane_storage/schema.h>
#include <ydb/core/yq/libs/db_schema/db_schema.h>

namespace NYq {

namespace {

struct TTaskInternal {
    TEvControlPlaneStorage::TTask Task;
    ui64 RetryCounter = 0;
    TInstant RetryCounterUpdatedAt = TInstant::Zero();
    bool ShouldAbortTask = false;
    TString TablePathPrefix;
    TString Owner;
    TString HostName;
    TMaybe<YandexQuery::Job> Job;
    TInstant Deadline;
};

std::pair<TString, NYdb::TParams> MakeSql(const TTaskInternal& taskInternal) {
    const auto& task = taskInternal.Task;
    const auto& retryCounter = taskInternal.RetryCounter;
    const auto& retryCounterUpdatedAt = taskInternal.RetryCounterUpdatedAt;
    const auto& hostName = taskInternal.HostName;
    const auto& owner = taskInternal.Owner;

    TSqlQueryBuilder queryBuilder(taskInternal.TablePathPrefix);
    queryBuilder.AddString("scope", task.Scope);
    queryBuilder.AddString("query_id", task.QueryId);
    queryBuilder.AddString("query", task.Query.SerializeAsString());
    queryBuilder.AddString("internal", task.Internal.SerializeAsString());
    queryBuilder.AddString("host", hostName);
    queryBuilder.AddString("owner", owner);
    queryBuilder.AddTimestamp("now", TInstant::Now());
    queryBuilder.AddUint64("retry_counter", retryCounter);
    queryBuilder.AddUint64("generation", task.Generation);
    queryBuilder.AddTimestamp("retry_counter_update_time", retryCounterUpdatedAt);

    // update queries
    queryBuilder.AddText(
        "UPDATE `" QUERIES_TABLE_NAME "` SET `" GENERATION_COLUMN_NAME "` = $generation" + (taskInternal.ShouldAbortTask ? ", `" QUERY_COLUMN_NAME "` = $query" : TString{""}) + "\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id;\n"
    );

    // update pending
    queryBuilder.AddText(
        "UPDATE `" PENDING_TABLE_NAME "` SET `" QUERY_COLUMN_NAME "` = $query, `" INTERNAL_COLUMN_NAME "` = $internal,\n"
        "`" HOST_NAME_COLUMN_NAME "` = $host, `" OWNER_COLUMN_NAME "` = $owner\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id;\n"
    );

    // update pending small
    queryBuilder.AddText(
        "UPDATE `" PENDING_SMALL_TABLE_NAME "` SET `" LAST_SEEN_AT_COLUMN_NAME "` = $now,\n"
        "`" RETRY_COUNTER_COLUMN_NAME "` = $retry_counter, `" RETRY_COUNTER_UPDATE_COLUMN_NAME "` = $retry_counter_update_time, `" IS_RESIGN_QUERY_COLUMN_NAME "` = false\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id;\n"
    );

    const auto query = queryBuilder.Build();
    return std::make_pair(query.Sql, query.Params);
}

} // namespace

std::tuple<TString, NYdb::TParams, std::function<std::pair<TString, NYdb::TParams>(const TVector<NYdb::TResultSet>&)>> MakeGetTaskUpdateQuery(
    const TTaskInternal& taskInternal,
    const std::shared_ptr<TResponseTasks>& responseTasks,
    const TInstant& taskLeaseTimestamp,
    bool disableCurrentIam,
    const TDuration& automaticQueriesTtl,
    const TDuration& resultSetsTtl)
{
    const auto& task = taskInternal.Task;

    TSqlQueryBuilder queryBuilder(taskInternal.TablePathPrefix);
    queryBuilder.AddString("scope", task.Scope);
    queryBuilder.AddString("query_id", task.QueryId);
    queryBuilder.AddTimestamp("from", taskLeaseTimestamp);

    queryBuilder.AddText(
        "SELECT `" GENERATION_COLUMN_NAME "`\n"
        "FROM `" QUERIES_TABLE_NAME "`\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id;\n"
        "SELECT `" LAST_SEEN_AT_COLUMN_NAME "`\n"
        "FROM `" PENDING_SMALL_TABLE_NAME "`\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id AND `" LAST_SEEN_AT_COLUMN_NAME "` < $from;\n"
        "SELECT `" INTERNAL_COLUMN_NAME "`, `" QUERY_COLUMN_NAME "`\n"
        "FROM `" PENDING_TABLE_NAME "`\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id;\n"
    );

    auto prepareParams = [=, taskInternal=taskInternal, responseTasks=responseTasks](const TVector<TResultSet>& resultSets) mutable {
        auto& task = taskInternal.Task;
        const auto shouldAbortTask = taskInternal.ShouldAbortTask;
        constexpr size_t expectedResultSetsSize = 3;

        if (resultSets.size() != expectedResultSetsSize || !resultSets[1].RowsCount()) {
            return std::make_pair(TString{}, TParamsBuilder{}.Build());
        }

        {
            TResultSetParser parser(resultSets[0]);
            while (parser.TryNextRow()) {
                task.Generation = parser.ColumnParser(GENERATION_COLUMN_NAME).GetOptionalUint64().GetOrElse(0) + 1;
            }
        }

        {
            TResultSetParser parser(resultSets[2]);
            while (parser.TryNextRow()) {
                if (!task.Query.ParseFromString(*parser.ColumnParser(QUERY_COLUMN_NAME).GetOptionalString())) {
                    throw TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Error parsing proto message for query. Please contact internal support";
                }
                const TInstant deadline = TInstant::Now() + (task.Query.content().automatic() ? std::min(automaticQueriesTtl, resultSetsTtl) : resultSetsTtl);
                task.Deadline = deadline;
                if (!task.Internal.ParseFromString(*parser.ColumnParser(INTERNAL_COLUMN_NAME).GetOptionalString())) {
                    throw TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Error parsing proto message for query internal. Please contact internal support";
                }

                if (disableCurrentIam) {
                    task.Internal.clear_token();
                }
            }
        }

        if (shouldAbortTask) {
            Ydb::Issue::IssueMessage abortIssueMsg;
            abortIssueMsg.set_message("Query was aborted by system due to high failure rate");
            abortIssueMsg.set_severity(NYql::TSeverityIds::S_ERROR);
            *task.Query.add_issue() = abortIssueMsg;
            task.Query.mutable_meta()->set_status(YandexQuery::QueryMeta::ABORTING_BY_SYSTEM);
        }

        responseTasks->AddTaskBlocking(task.QueryId, task);

        return MakeSql(taskInternal);
    };

    const auto query = queryBuilder.Build();
    return std::make_tuple(query.Sql, query.Params, prepareParams);
}

void TYdbControlPlaneStorageActor::Handle(TEvControlPlaneStorage::TEvGetTaskRequest::TPtr& ev)
{
    TInstant startTime = TInstant::Now();
    TRequestCountersPtr requestCounters = Counters.Requests[RT_GET_TASK];
    requestCounters->InFly->Inc();

    TEvControlPlaneStorage::TEvGetTaskRequest& request = *ev->Get();
    const TString owner = request.Owner;
    const TString hostName = request.HostName;
    const ui64 tasksBatchSize = Config.Proto.GetTasksBatchSize();
    const ui64 numTasksProportion = Config.Proto.GetNumTasksProportion();

    CPS_LOG_T("GetTaskRequest: " << owner << " " << hostName);

    NYql::TIssues issues = ValidateGetTask(owner, hostName);
    if (issues) {
        CPS_LOG_D("GetTaskRequest, validation failed: " << owner << " " << hostName << " " << issues.ToString());
        const TDuration delta = TInstant::Now() - startTime;
        SendResponseIssues<TEvControlPlaneStorage::TEvGetTaskResponse>(ev->Sender, issues, ev->Cookie, delta, requestCounters);
        LWPROBE(GetTaskRequest, owner, hostName, delta, false);
        return;
    }

    auto debugInfo = Config.Proto.GetEnableDebugMode() ? std::make_shared<TDebugInfo>() : TDebugInfoPtr{};

    auto response = std::make_shared<std::tuple<TVector<TEvControlPlaneStorage::TTask>, TString>>(); //tasks, owner

    TSqlQueryBuilder queryBuilder(YdbConnection->TablePathPrefix);
    const auto taskLeaseTimestamp = TInstant::Now() - Config.TaskLeaseTtl;
    queryBuilder.AddTimestamp("from", taskLeaseTimestamp);
    queryBuilder.AddUint64("tasks_limit", tasksBatchSize);
    queryBuilder.AddText(
        "SELECT `" SCOPE_COLUMN_NAME "`, `" QUERY_ID_COLUMN_NAME "`,\n"
        "`" RETRY_COUNTER_COLUMN_NAME "`, `" RETRY_COUNTER_UPDATE_COLUMN_NAME "`, `" QUERY_TYPE_COLUMN_NAME "`, `" IS_RESIGN_QUERY_COLUMN_NAME "`\n"
        "FROM `" PENDING_SMALL_TABLE_NAME "`\n"
        "WHERE `" LAST_SEEN_AT_COLUMN_NAME "` < $from ORDER BY `" QUERY_ID_COLUMN_NAME "` DESC LIMIT $tasks_limit;\n"
    );

    auto responseTasks = std::make_shared<TResponseTasks>();
    auto prepareParams = [=, responseTasks=responseTasks](const TVector<TResultSet>& resultSets) mutable {
        TVector<TTaskInternal> tasks;
        TVector<TPickTaskParams> pickTaskParams;
        const auto now = TInstant::Now();
        if (resultSets.empty() || !resultSets.back().RowsCount()) {
            return pickTaskParams;
        }

        TResultSetParser parser(resultSets.back());
        while (parser.TryNextRow()) {
            TTaskInternal& taskInternal = tasks.emplace_back();
            taskInternal.Owner = owner;
            taskInternal.HostName = hostName;
            taskInternal.TablePathPrefix = YdbConnection->TablePathPrefix;

            auto& task = taskInternal.Task;

            task.Scope = *parser.ColumnParser(SCOPE_COLUMN_NAME).GetOptionalString();
            task.QueryId = *parser.ColumnParser(QUERY_ID_COLUMN_NAME).GetOptionalString();

            bool isResignQuery = parser.ColumnParser(IS_RESIGN_QUERY_COLUMN_NAME).GetOptionalBool().GetOrElse(false);
            taskInternal.RetryCounter = parser.ColumnParser(RETRY_COUNTER_COLUMN_NAME).GetOptionalUint64().GetOrElse(0);
            taskInternal.RetryCounterUpdatedAt = parser.ColumnParser(RETRY_COUNTER_UPDATE_COLUMN_NAME).GetOptionalTimestamp().GetOrElse(TInstant::Zero());

            YandexQuery::QueryContent::QueryType queryType = static_cast<YandexQuery::QueryContent::QueryType>(parser.ColumnParser(QUERY_TYPE_COLUMN_NAME).GetOptionalInt64().GetOrElse(0));

            const auto retryCounterLimit = queryType == YandexQuery::QueryContent::ANALYTICS ? Config.Proto.GetAnalyticsRetryCounterLimit() : Config.Proto.GetStreamingRetryCounterLimit();
            const auto retryCounterUpdateTime = queryType == YandexQuery::QueryContent::ANALYTICS ? Config.AnalyticsRetryCounterUpdateTime : Config.StreamingRetryCounterUpdateTime;

            if (isResignQuery) {
                continue;
            }

            ++taskInternal.RetryCounter;

            if (now > taskInternal.RetryCounterUpdatedAt + retryCounterUpdateTime) {
                taskInternal.RetryCounter = 0;
                taskInternal.RetryCounterUpdatedAt = now;
            }

            if (taskInternal.RetryCounter >= retryCounterLimit) {
                taskInternal.ShouldAbortTask = true;
            }
        }

        std::shuffle(tasks.begin(), tasks.end(), std::default_random_engine());
        const size_t numTasks = (std::min(tasks.size(), tasksBatchSize) + numTasksProportion - 1) / numTasksProportion;

        for (size_t i = 0; i < numTasks; ++i) {
            auto tupleParams = MakeGetTaskUpdateQuery(tasks[i], responseTasks, taskLeaseTimestamp, Config.Proto.GetDisableCurrentIam(), Config.AutomaticQueriesTtl, Config.ResultSetsTtl); // using for win32 build
            auto readQuery = std::get<0>(tupleParams);
            auto readParams = std::get<1>(tupleParams);
            auto prepareParams = std::get<2>(tupleParams);
            pickTaskParams.emplace_back(TPickTaskParams{readQuery, readParams, prepareParams, tasks[i].Task.QueryId, tasks[i].ShouldAbortTask});
        }
        return pickTaskParams;
    };

    const auto query = queryBuilder.Build();
    auto [readStatus, resultSets] = Read(query.Sql, query.Params, requestCounters, debugInfo, TTxSettings::StaleRO());
    auto result = readStatus.Apply([=, resultSets=resultSets, requestCounters=requestCounters, debugInfo=debugInfo, responseTasks=responseTasks](const auto& readFuture) mutable {
        try {
            if (!readFuture.GetValue().IsSuccess())
                return readFuture;
        } catch (...) {
            return readFuture;
        }

        auto pickTaskParams = prepareParams(*resultSets);
        if (pickTaskParams.empty())
            return readFuture;

        auto debugInfos = std::make_shared<TVector<TDebugInfoPtr>>(pickTaskParams.size());
        if (Config.Proto.GetEnableDebugMode()) {
            for (size_t i = 0; i < pickTaskParams.size(); i++) {
                (*debugInfos)[i] = std::make_shared<TDebugInfo>();
            }
        }

        TVector<TFuture<void>> futures;
        for (size_t i = 0; i < pickTaskParams.size(); ++i) {
            futures.emplace_back(PickTask(pickTaskParams[i], requestCounters, (*debugInfos)[i], responseTasks));
        }

        auto allFuture = NThreading::WaitExceptionOrAll(futures);
        return allFuture.Apply([=, responseTasks=responseTasks](const auto& future) mutable {
            if (debugInfo) {
                for (const auto& info: *debugInfos) {
                    debugInfo->insert(debugInfo->end(), info->begin(), info->end());
                }
            }
            NYql::TIssues issues;
            auto status = MakeFuture(TStatus{EStatus::SUCCESS, std::move(issues)});
            try {
                future.GetValue();
                TVector<TEvControlPlaneStorage::TTask> tasks;
                for (const auto& [_, task] : responseTasks->GetTasksNonBlocking()) {
                    tasks.emplace_back(task);
                }
                *response = std::make_tuple(tasks, owner);
            } catch (...) {
                issues.AddIssue(CurrentExceptionMessage());
                status = MakeFuture(TStatus{EStatus::GENERIC_ERROR, std::move(issues)});
            }
            return status;
        });
    });

    auto prepare = [response] { return *response; };
    auto success = SendResponseTuple
        <TEvControlPlaneStorage::TEvGetTaskResponse,
        std::tuple<TVector<TEvControlPlaneStorage::TTask>, TString>> //tasks, owner
        ("GetTaskRequest",
        NActors::TActivationContext::ActorSystem(),
        result,
        SelfId(),
        ev,
        startTime,
        requestCounters,
        prepare,
        debugInfo);

    success.Apply([=](const auto& future) {
            TDuration delta = TInstant::Now() - startTime;
            LWPROBE(GetTaskRequest, owner, hostName, delta, future.GetValue());
        });
}

} // NYq
