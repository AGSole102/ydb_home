#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>

namespace NKikimr::NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {

TParams BuildUpdateParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$items")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").Uint64(101)
                    .AddMember("Text").String("New")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").Uint64(209)
                    .AddMember("Text").String("New")
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

TParams BuildInsertParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$items")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").Uint64(109)
                    .AddMember("Text").String("New")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").Uint64(209)
                    .AddMember("Text").String("New")
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

TParams BuildDeleteParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$items")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").Uint64(101)
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").Uint64(209)
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

TParams BuildUpdateIndexParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$items")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").String("Primary1")
                    .AddMember("Index2").String("SecondaryNew1")
                    .AddMember("Value").String("ValueNew1")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").String("Primary5")
                    .AddMember("Index2").String("SecondaryNew2")
                    .AddMember("Value").String("ValueNew2")
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

TParams BuildDeleteIndexParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$items")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").String("Primary1")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").String("Primary5")
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

TParams BuildInsertIndexParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$items")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Key").String("Primary10")
                    .AddMember("Index2").String("SecondaryNew10")
                    .AddMember("Value").String("ValueNew10")
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

} // namespace

Y_UNIT_TEST_SUITE(KqpPerf) {
    Y_UNIT_TEST_TWIN(Upsert, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = BuildUpdateParams(db);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':Uint64,'Text':String>>;

            UPSERT INTO EightShard
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 0,
            .ExpectedUpdates = 2,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        // TODO: Get rid of additional precompute stage for adding optionality to row members in NewEngine
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 2 : 1);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 2);
        }
    }

    Y_UNIT_TEST_TWIN(Replace, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = BuildUpdateParams(db);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':Uint64,'Text':String>>;

            REPLACE INTO EightShard
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 0,
            .ExpectedUpdates = 2,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        // Single-phase REPLACE in NewEngine require additional runtime write callable
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 2 : 1);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 2);
        }
    }

    Y_UNIT_TEST_TWIN(UpdateOn, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = BuildUpdateParams(db);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':Uint64,'Text':String>>;

            UPDATE EightShard ON
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 1, // Non-existing keys don't count in reads
            .ExpectedUpdates = 1,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        // Two-phase UPDATE ON in NewEngine require more complex runtime callables
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 3 : 2);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 2);
        }
    }

    Y_UNIT_TEST_TWIN(Insert, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = BuildInsertParams(db);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':Uint64,'Text':String>>;

            INSERT INTO EightShard
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 0, // Non-existing keys don't count in reads
            .ExpectedUpdates = 2,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        // Three-phase INSERT in NewEngine require more complex runtime callables
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 3);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 2);
        }
    }

    Y_UNIT_TEST_TWIN(DeleteOn, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = BuildDeleteParams(db);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':Uint64>>;

            DELETE FROM EightShard ON
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 0,
            .ExpectedDeletes = 2,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        // TODO: Get rid of additional precompute stage for adding optionality to row members in NewEngine
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 2 : 1);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 2);
        }
    }

    Y_UNIT_TEST_TWIN(Update, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = db.GetParamsBuilder()
            .AddParam("$key").Uint64(201).Build()
        .Build();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $key AS Uint64;

            UPDATE EightShard
            SET Data = Data + 1
            WHERE Key = $key;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 1,
            .ExpectedUpdates = 1,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 1);
        }
    }

    Y_UNIT_TEST_TWIN(Delete, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto params = db.GetParamsBuilder()
            .AddParam("$key").Uint64(201).Build()
            .AddParam("$text").String("Value1").Build()
        .Build();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $key AS Uint64;
            DECLARE $text AS String;

            DELETE FROM EightShard
            WHERE Key = $key AND Text = $text;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/EightShard", {
            .ExpectedReads = 1,
            .ExpectedDeletes = 1,
        });

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

        for (const auto& phase : stats.query_phases()) {
            UNIT_ASSERT(phase.affected_shards() <= 1);
        }
    }

    Y_UNIT_TEST_TWIN(IndexUpsert, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = BuildUpdateIndexParams(db);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':String,'Index2':String,'Value':String>>;

            UPSERT INTO SecondaryWithDataColumns
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 3);
    }

    Y_UNIT_TEST_TWIN(IndexReplace, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = BuildUpdateIndexParams(db);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':String,'Index2':String,'Value':String>>;

            REPLACE INTO SecondaryWithDataColumns
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 3);
    }

    Y_UNIT_TEST_TWIN(IndexUpdateOn, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = BuildUpdateIndexParams(db);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':String,'Index2':String,'Value':String>>;

            UPDATE SecondaryWithDataColumns ON
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 2);
    }

    Y_UNIT_TEST_TWIN(IndexDeleteOn, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = BuildDeleteIndexParams(db);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':String>>;

            DELETE FROM SecondaryWithDataColumns ON
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 2);
    }

    Y_UNIT_TEST_TWIN(IndexInsert, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = BuildInsertIndexParams(db);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $items AS List<Struct<'Key':String,'Index2':String,'Value':String>>;

            INSERT INTO SecondaryWithDataColumns
            SELECT * FROM AS_TABLE($items);
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 5 : 3);
    }

    Y_UNIT_TEST_TWIN(IdxLookupJoin, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = db.GetParamsBuilder()
            .AddParam("$key").Int32(3).Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $key AS Int32;

            SELECT *
            FROM Join1 AS t1
            INNER JOIN Join2 AS t2 ON t1.Fk21 = t2.Key1 AND t1.Fk22 = t2.Key2
            WHERE t1.Key = $key;
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 3 : 3);
    }

    Y_UNIT_TEST_TWIN(IdxLookupJoinThreeWay, UseNewEngine) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = db.GetParamsBuilder()
            .AddParam("$key").Int32(3).Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $key AS Int32;

            SELECT t1.Key, t3.Value
            FROM Join1 AS t1
            INNER JOIN Join2 AS t2 ON t1.Fk21 = t2.Key1 AND t1.Fk22 = t2.Key2
            INNER JOIN KeyValue2 AS t3 ON t2.Name = t3.Key
            WHERE t1.Key = $key;
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 5 : 4);
    }
}

} // namespace NKikimr::NKqp
