#include "defrag_quantum.h"
#include "defrag_search.h"
#include "defrag_rewriter.h"
#include <ydb/core/blobstorage/vdisk/common/sublog.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_hugeblobctx.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_response.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_hugeblobctx.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_private_events.h>
#include <ydb/core/blobstorage/vdisk/hulldb/hull_ds_all_snap.h>
#include <ydb/core/blobstorage/vdisk/skeleton/blobstorage_takedbsnap.h>
#include <ydb/core/util/stlog.h>
#include <library/cpp/actors/core/actor_coroutine.h>

using namespace NKikimrServices;

namespace NKikimr {

    class TDefragQuantum : public TActorCoroImpl {
        std::shared_ptr<TDefragCtx> DCtx;
        const TVDiskID SelfVDiskId;
        std::optional<TChunksToDefrag> ChunksToDefrag;

        enum {
            EvResume = EventSpaceBegin(TEvents::ES_PRIVATE)
        };

    public:
        TDefragQuantum(const std::shared_ptr<TDefragCtx>& dctx, const TVDiskID& selfVDiskId,
                std::optional<TChunksToDefrag> chunksToDefrag)
            : TActorCoroImpl(65536, true, true)
            , DCtx(dctx)
            , SelfVDiskId(selfVDiskId)
            , ChunksToDefrag(std::move(chunksToDefrag))
        {}

        void ProcessUnexpectedEvent(TAutoPtr<IEventHandle> ev) override {
            Y_FAIL("unexpected event Type# 0x%08" PRIx32, ev->GetTypeRewrite());
        }

        void Run() override {
            TEvDefragQuantumResult::TStat stat{.Eof = true};

            if (ChunksToDefrag) {
                Y_VERIFY(*ChunksToDefrag);
            } else {
                TDefragQuantumFindChunks findChunks(GetSnapshot(), DCtx->HugeBlobCtx);
                while (findChunks.Scan(TDuration::MilliSeconds(10))) {
                    Yield();
                }
                ChunksToDefrag.emplace(findChunks.GetChunksToDefrag(DCtx->MaxChunksToDefrag));
            }
            if (*ChunksToDefrag) {
                stat.FoundChunksToDefrag = ChunksToDefrag->FoundChunksToDefrag;
                stat.FreedChunks = ChunksToDefrag->Chunks;
                stat.Eof = stat.FoundChunksToDefrag < DCtx->MaxChunksToDefrag;

                LockChunks(*ChunksToDefrag);

                THPTimer timer;
                TDefragQuantumFindRecords findRecords(GetSnapshot(), std::move(*ChunksToDefrag));
                findRecords.Scan(TDuration::MilliSeconds(10), std::bind(&TDefragQuantum::Yield, this));
                if (auto duration = TDuration::Seconds(timer.Passed()); duration >= TDuration::Seconds(30)) {
                    STLOG(PRI_ERROR, BS_VDISK_DEFRAG, BSVDD06, VDISKP(DCtx->VCtx->VDiskLogPrefix, "scan too long"),
                        (Duration, duration));
                }

                const TActorId rewriterActorId = Register(CreateDefragRewriter(DCtx, SelfVDiskId, SelfActorId,
                    findRecords.RetrieveSnapshot(), findRecords.GetRecordsToRewrite()));
                THolder<TEvDefragRewritten::THandle> ev;
                try {
                    ev = WaitForSpecificEvent<TEvDefragRewritten>();
                } catch (const TPoisonPillException& ex) {
                    Send(new IEventHandle(TEvents::TSystem::Poison, 0, rewriterActorId, {}, nullptr, 0));
                    throw;
                }

                if (auto duration = TDuration::Seconds(timer.Passed()); duration >= TDuration::Seconds(30)) {
                    STLOG(PRI_ERROR, BS_VDISK_DEFRAG, BSVDD07, VDISKP(DCtx->VCtx->VDiskLogPrefix, "scan + rewrite too long"),
                        (Duration, duration));
                }

                stat.RewrittenRecs = ev->Get()->RewrittenRecs;
                stat.RewrittenBytes = ev->Get()->RewrittenBytes;

                Compact();

                auto hugeStat = GetHugeStat();
                Y_VERIFY(hugeStat.LockedChunks.size() < 100);
            }

            Send(ParentActorId, new TEvDefragQuantumResult(std::move(stat)));
        }

        THullDsSnap GetSnapshot() {
            Send(DCtx->SkeletonId, new TEvTakeHullSnapshot(false));
            return std::move(WaitForSpecificEvent<TEvTakeHullSnapshotResult>()->Get()->Snap);
        }

        void Yield() {
            Send(new IEventHandle(EvResume, 0, SelfActorId, {}, nullptr, 0));
            WaitForSpecificEvent([](IEventHandle& ev) { return ev.Type == EvResume; });
        }

        void LockChunks(const TChunksToDefrag& chunks) {
            Send(DCtx->HugeKeeperId, new TEvHugeLockChunks(chunks.Chunks));
            WaitForSpecificEvent<TEvHugeLockChunksResult>();
        }

        void Compact() {
            Send(DCtx->SkeletonId, TEvCompactVDisk::Create(EHullDbType::LogoBlobs));
            WaitForSpecificEvent<TEvCompactVDiskResult>();
        }

        NHuge::THeapStat GetHugeStat() {
            Send(DCtx->HugeKeeperId, new TEvHugeStat());
            return std::move(WaitForSpecificEvent<TEvHugeStatResult>()->Get()->Stat);
        }
    };

    IActor *CreateDefragQuantumActor(const std::shared_ptr<TDefragCtx>& dctx, const TVDiskID& selfVDiskId,
            std::optional<TChunksToDefrag> chunksToDefrag) {
        return new TActorCoro(MakeHolder<TDefragQuantum>(dctx, selfVDiskId, std::move(chunksToDefrag)),
            NKikimrServices::TActivity::BS_DEFRAG_QUANTUM);
    }

} // NKikimr
