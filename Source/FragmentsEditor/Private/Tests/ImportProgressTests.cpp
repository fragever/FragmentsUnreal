// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Pins the pure half of the import-progress UI.
#include "Misc/AutomationTest.h"
#include "Importer/FragmentImportProgressSnapshot.h"
#include "Importer/FragmentsImporter.h"
#include "Import/FragmentImportProgressMath.h"
#include "Import/FragmentsImportProgress.h"
#include "Import/FragmentsImportProgressSink.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentImportProgressIdleSnapshotTest,
	"FragmentsUnreal.Import.Progress_IdleSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentImportProgressIdleSnapshotTest::RunTest(const FString& Parameters)
{
	// A freshly constructed importer has no spawn in flight.
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>();
	const FFragmentImportProgressSnapshot Snap =
		Importer->GetImportProgressSnapshot(TEXT("any-guid"));

	TestFalse(TEXT("idle importer is not spawning"), Snap.bIsSpawning);
	TestFalse(TEXT("idle importer has nothing queued"), Snap.bIsQueuedForSpawn);
	TestFalse(TEXT("idle importer is not in a deferred-interior wave"), Snap.bIsDeferredInterior);
	TestEqual(TEXT("idle importer has spawned no items"), Snap.ItemsSpawned, 0);
	TestEqual(TEXT("idle importer has no total"), Snap.TotalItems, 0);
	TestEqual(TEXT("idle importer has no in-flight mesh builds"), Snap.InFlightMeshBuilds, 0);

	// Idle models keep default progress phase values.
	TestEqual(TEXT("idle importer reports the default phase, not a fabricated one"),
		(int32)Snap.Phase, (int32)EFragmentImportPhase::Spawning);
	TestEqual(TEXT("idle importer has no phase fraction"), Snap.PhaseFraction, 0.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentImportProgressWorkUnitsTest,
	"FragmentsUnreal.Import.Progress_WorkUnits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentImportProgressWorkUnitsTest::RunTest(const FString& Parameters)
{
	using namespace FragmentImportProgress;

	// 900 items -> TailUnits = 100, WorkToDo = 1000.
	auto Make = [](EFragmentImportPhase Phase, int32 Spawned, float Frac)
	{
		FFragmentImportProgressSnapshot S;
		S.bIsSpawning = true;
		S.TotalItems = 900;
		S.ItemsSpawned = Spawned;
		S.Phase = Phase;
		S.PhaseFraction = Frac;
		return S;
	};

	// Unknown totals use a positive placeholder to keep progress visible.
	{
		FFragmentImportProgressSnapshot S;
		S.TotalItems = 0;
		const FWorkUnits U = ComputeWorkUnits(S);
		TestEqual(TEXT("unknown total does no work"), U.WorkDone, 0);
		TestEqual(TEXT("unknown total still has a positive denominator"), U.WorkToDo, 1);
	}

	// A negative total is impossible today but must clamp rather than produce a negative denominator, which would make the percentage nonsense in the status bar.
	{
		FFragmentImportProgressSnapshot S;
		S.TotalItems = -5;
		const FWorkUnits U = ComputeWorkUnits(S);
		TestEqual(TEXT("negative total clamps to the unknown-total case"), U.WorkToDo, 1);
	}

	{
		const FWorkUnits U = ComputeWorkUnits(Make(EFragmentImportPhase::Spawning, 0, 0.f));
		TestEqual(TEXT("spawn start is zero work done"), U.WorkDone, 0);
		TestEqual(TEXT("denominator is items plus the tail reserve"), U.WorkToDo, 1000);
	}

	// Reserve the final 10% for completion phases.
	{
		const FWorkUnits U = ComputeWorkUnits(Make(EFragmentImportPhase::Spawning, 900, 0.f));
		TestEqual(TEXT("queue drained is exactly 90 percent"), U.WorkDone, 900);
	}

	// ItemsSpawned can exceed TotalItems if a bucket is discovered late; clamp rather than overshoot into the tail's reserve and report a phase's work as already done.
	{
		const FWorkUnits U = ComputeWorkUnits(Make(EFragmentImportPhase::Spawning, 5000, 0.f));
		TestEqual(TEXT("overshooting item count clamps at the 90 percent mark"), U.WorkDone, 900);
	}

	// Tail phase index is (enum value - 1): WaitingOnMeshBuilds=0, BuildingBuckets=1, MergingCells=2, ApplyingOverrides=3, over a span of 4.
	{
		const FWorkUnits U = ComputeWorkUnits(Make(EFragmentImportPhase::WaitingOnMeshBuilds, 900, 0.f));
		TestEqual(TEXT("barrier sits at the 90 percent mark, not past it"), U.WorkDone, 900);
	}
	{
		// 900 + round(100 * (1 + 0.5) / 4) = 900 + 38 = 938.
		const FWorkUnits U = ComputeWorkUnits(Make(EFragmentImportPhase::BuildingBuckets, 900, 0.5f));
		TestEqual(TEXT("half way through bucket finalize"), U.WorkDone, 938);
	}
	{
		// 900 + round(100 * (3 + 1) / 4) = 1000.
		const FWorkUnits U = ComputeWorkUnits(Make(EFragmentImportPhase::ApplyingOverrides, 900, 1.f));
		TestEqual(TEXT("end of the last tail phase is exactly complete"), U.WorkDone, 1000);
		TestEqual(TEXT("and complete means done == todo"), U.WorkDone, U.WorkToDo);
	}

	// MONOTONICITY -- the property that actually matters.
	{
		TArray<FFragmentImportProgressSnapshot> Sequence;
		for (int32 Spawned = 0; Spawned <= 900; Spawned += 150)
		{
			Sequence.Add(Make(EFragmentImportPhase::Spawning, Spawned, 0.f));
		}
		Sequence.Add(Make(EFragmentImportPhase::WaitingOnMeshBuilds, 900, 0.f));
		for (const EFragmentImportPhase Phase : { EFragmentImportPhase::BuildingBuckets,
		                                          EFragmentImportPhase::MergingCells,
		                                          EFragmentImportPhase::ApplyingOverrides })
		{
			for (float Frac = 0.f; Frac <= 1.f; Frac += 0.25f)
			{
				Sequence.Add(Make(Phase, 900, Frac));
			}
		}

		int32 Previous = -1;
		for (int32 Index = 0; Index < Sequence.Num(); ++Index)
		{
			const int32 Current = ComputeWorkUnits(Sequence[Index]).WorkDone;
			TestTrue(FString::Printf(TEXT("step %d does not go backwards (%d -> %d)"),
				Index, Previous, Current), Current >= Previous);
			Previous = Current;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentImportProgressTextAndOrphanTest,
	"FragmentsUnreal.Import.Progress_TextAndOrphan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentImportProgressTextAndOrphanTest::RunTest(const FString& Parameters)
{
	using namespace FragmentImportProgress;

	FFragmentImportProgressSnapshot S;
	S.TotalItems = 900;

	// The status-bar label is the model name alone; the stage is reported here, for the panel row.
	S.Phase = EFragmentImportPhase::Spawning;
	TestEqual(TEXT("spawning stage"), MakeStageText(S).ToString(), FString(TEXT("placing geometry")));

	// The stall phase names its cause AND its count.
	S.Phase = EFragmentImportPhase::WaitingOnMeshBuilds;
	S.InFlightMeshBuilds = 3;
	TestEqual(TEXT("barrier stage names the count"), MakeStageText(S).ToString(), FString(TEXT("waiting on 3 mesh builds")));

	// Singular must not read "waiting on 1 mesh builds".
	S.InFlightMeshBuilds = 1;
	TestEqual(TEXT("barrier stage is grammatically singular for one"), MakeStageText(S).ToString(), FString(TEXT("waiting on 1 mesh build")));

	S.Phase = EFragmentImportPhase::BuildingBuckets;
	TestEqual(TEXT("bucket finalize stage"), MakeStageText(S).ToString(), FString(TEXT("building buckets")));

	S.Phase = EFragmentImportPhase::MergingCells;
	TestEqual(TEXT("cell merge stage"), MakeStageText(S).ToString(), FString(TEXT("merging cells")));

	S.Phase = EFragmentImportPhase::ApplyingOverrides;
	TestEqual(TEXT("override apply stage"), MakeStageText(S).ToString(), FString(TEXT("applying materials")));

	// ORPHAN PREDICATE.
	{
		FFragmentImportProgressSnapshot Spawning;
		Spawning.bIsSpawning = true;
		TestFalse(TEXT("a spawning model is never orphaned, however long it runs"),
			IsOrphaned(Spawning, 999));

		FFragmentImportProgressSnapshot Queued;
		Queued.bIsQueuedForSpawn = true;
		TestFalse(TEXT("a queued model is never orphaned, however long it waits"),
			IsOrphaned(Queued, 999));

		FFragmentImportProgressSnapshot Gone; // neither spawning nor queued

		// Below the threshold: this is the legitimate window between BeginTracking and ProcessLoadedFragment committing to a path.
		TestFalse(TEXT("one missed tick is not an orphan"), IsOrphaned(Gone, 1));
		TestFalse(TEXT("four missed ticks is not an orphan"),
			IsOrphaned(Gone, OrphanTickThreshold - 1));
		TestTrue(TEXT("the threshold tick is an orphan"),
			IsOrphaned(Gone, OrphanTickThreshold));
		TestTrue(TEXT("past the threshold stays an orphan"),
			IsOrphaned(Gone, OrphanTickThreshold + 10));
	}

	return true;
}

namespace
{
	/** Records what the tracker asked the UI to do, so the lifecycle can be asserted directly. */
	struct FRecordingSink : public IFragmentsImportProgressSink
	{
		int32 NextId = 0;
		TArray<int32> Started;
		TArray<int32> Cancelled;
		TArray<TPair<int32, int32>> Updates;   // (SinkId, TotalWorkDone)
		TArray<FString> Failures;              // Detail text
		TArray<FString> Notices;               // Title text

		virtual int32 StartProgress(const FText&, int32) override
		{
			Started.Add(++NextId);
			return NextId;
		}
		virtual void UpdateProgress(int32 SinkId, int32 TotalWorkDone, int32, const FText&) override
		{
			Updates.Emplace(SinkId, TotalWorkDone);
		}
		virtual void CancelProgress(int32 SinkId) override { Cancelled.Add(SinkId); }
		virtual void ShowFailure(const FText&, const FText& Detail) override
		{
			Failures.Add(Detail.ToString());
		}
		virtual void ShowNotice(const FText& Title, const FText&) override { Notices.Add(Title.ToString()); }
	};

	/** Records the same things as FRecordingSink, but always DECLINES to open a notification -- StartProgress returns 0. */
	struct FDecliningSink : public IFragmentsImportProgressSink
	{
		TArray<int32> Cancelled;
		TArray<TPair<int32, int32>> Updates;
		TArray<FString> Failures;

		virtual int32 StartProgress(const FText&, int32) override { return 0; }
		virtual void ShowNotice(const FText&, const FText&) override {}
		virtual void UpdateProgress(int32 SinkId, int32 TotalWorkDone, int32, const FText&) override
		{
			Updates.Emplace(SinkId, TotalWorkDone);
		}
		virtual void CancelProgress(int32 SinkId) override { Cancelled.Add(SinkId); }
		virtual void ShowFailure(const FText&, const FText& Detail) override
		{
			Failures.Add(Detail.ToString());
		}
	};

	FFragmentImportProgressSnapshot MakeSpawning(int32 Spawned, int32 Total)
	{
		FFragmentImportProgressSnapshot S;
		S.bIsSpawning = true;
		S.ItemsSpawned = Spawned;
		S.TotalItems = Total;
		S.Phase = EFragmentImportPhase::Spawning;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentImportProgressLifecycleTest,
	"FragmentsUnreal.Import.Progress_Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentImportProgressLifecycleTest::RunTest(const FString& Parameters)
{
	// Success: progress opens, updates, and ends with NO failure notification.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-a"), TEXT("Campus.frag"));
		TestEqual(TEXT("begin opens exactly one entry"), Sink->Started.Num(), 1);
		TestEqual(TEXT("begin tracks one model"), Progress.GetTrackedCount(), 1);

		Progress.PollWith([](const FString&) { return MakeSpawning(450, 900); });
		TestEqual(TEXT("poll pushes one update"), Sink->Updates.Num(), 1);
		TestEqual(TEXT("update carries the computed work done"), Sink->Updates[0].Value, 450);

		Progress.EndTracking(TEXT("guid-a"), true, FString());
		TestEqual(TEXT("success ends the entry"), Sink->Cancelled.Num(), 1);
		TestEqual(TEXT("success shows no failure toast"), Sink->Failures.Num(), 0);
		TestEqual(TEXT("success stops tracking"), Progress.GetTrackedCount(), 0);
	}

	// Failure: the entry ends AND a failure notification carries the message.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-b"), TEXT("Broken.frag"));
		Progress.EndTracking(TEXT("guid-b"), false, TEXT("Loaded fragment model wrapper was not found."));

		TestEqual(TEXT("failure ends the entry"), Sink->Cancelled.Num(), 1);
		TestEqual(TEXT("failure shows exactly one toast"), Sink->Failures.Num(), 1);
		TestEqual(TEXT("the toast carries the error"), Sink->Failures[0],
			FString(TEXT("Loaded fragment model wrapper was not found.")));
	}

	// Deliberate teardown (Reimport / Remove) is SILENT.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-c"), TEXT("Campus.frag"));
		Progress.CancelTracking(TEXT("guid-c"));

		TestEqual(TEXT("cancel ends the entry"), Sink->Cancelled.Num(), 1);
		TestEqual(TEXT("cancel shows no failure toast"), Sink->Failures.Num(), 0);
		TestEqual(TEXT("cancel stops tracking"), Progress.GetTrackedCount(), 0);
	}

	// ORPHAN: PurgeModelState killed the spawn and no completion will ever arrive.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-d"), TEXT("Campus.frag"));

		auto Gone = [](const FString&) { return FFragmentImportProgressSnapshot(); };
		for (int32 Tick = 0; Tick < FragmentImportProgress::OrphanTickThreshold - 1; ++Tick)
		{
			Progress.PollWith(Gone);
		}
		TestEqual(TEXT("still tracked below the threshold"), Progress.GetTrackedCount(), 1);

		Progress.PollWith(Gone);
		TestEqual(TEXT("reclaimed at the threshold"), Progress.GetTrackedCount(), 0);
		TestEqual(TEXT("orphan reclaim ends the entry"), Sink->Cancelled.Num(), 1);
		TestEqual(TEXT("orphan reclaim is silent"), Sink->Failures.Num(), 0);
	}

	// A model waiting its turn in DeferredModelLoads must NOT be reclaimed, however long it waits.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-e"), TEXT("Queued.frag"));

		auto Queued = [](const FString&)
		{
			FFragmentImportProgressSnapshot S;
			S.bIsQueuedForSpawn = true;
			return S;
		};
		for (int32 Tick = 0; Tick < FragmentImportProgress::OrphanTickThreshold * 4; ++Tick)
		{
			Progress.PollWith(Queued);
		}
		TestEqual(TEXT("a queued model is never reclaimed"), Progress.GetTrackedCount(), 1);
		TestEqual(TEXT("and is never cancelled"), Sink->Cancelled.Num(), 0);
	}

	// A miss followed by a hit resets the counter -- one dropped frame must not accumulate across an entire multi-minute import into a spurious reclaim.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-f"), TEXT("Campus.frag"));

		auto Gone = [](const FString&) { return FFragmentImportProgressSnapshot(); };
		for (int32 Cycle = 0; Cycle < 10; ++Cycle)
		{
			for (int32 Tick = 0; Tick < FragmentImportProgress::OrphanTickThreshold - 1; ++Tick)
			{
				Progress.PollWith(Gone);
			}
			Progress.PollWith([](const FString&) { return MakeSpawning(10, 900); });
		}
		TestEqual(TEXT("a spawning tick resets the miss counter"), Progress.GetTrackedCount(), 1);
	}

	// A synchronous failure never opened a progress entry, but must still surface.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.ReportFailure(TEXT("Fragment path is empty."));
		TestEqual(TEXT("a standalone failure shows a toast"), Sink->Failures.Num(), 1);
		TestEqual(TEXT("and opens no progress entry"), Sink->Started.Num(), 0);
	}

	// EndTracking for a guid that was NEVER tracked (or was already reclaimed by the watchdog) must still raise a failure toast.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.EndTracking(TEXT("never-tracked"), false, TEXT("boom"));

		TestEqual(TEXT("an untracked failure still toasts"), Sink->Failures.Num(), 1);
		TestEqual(TEXT("the toast carries the message"), Sink->Failures[0], FString(TEXT("boom")));
		TestEqual(TEXT("there was nothing to cancel"), Sink->Cancelled.Num(), 0);
	}

	// Queueing resets missed ticks rather than only suppressing orphan detection.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-g"), TEXT("Queued.frag"));

		auto Queued = [](const FString&)
		{
			FFragmentImportProgressSnapshot S;
			S.bIsQueuedForSpawn = true;
			return S;
		};
		for (int32 Tick = 0; Tick < FragmentImportProgress::OrphanTickThreshold * 4; ++Tick)
		{
			Progress.PollWith(Queued);
		}

		Progress.PollWith([](const FString&) { return FFragmentImportProgressSnapshot(); });
		TestEqual(
			TEXT("one miss after a long queue wait starts from zero, not from the wait's tick count"),
			Progress.GetTrackedCount(), 1);
	}

	// A declining sink (StartProgress returns 0) is the routine "editor is not the active window" case.
	{
		TSharedRef<FDecliningSink> Sink = MakeShared<FDecliningSink>();
		FFragmentsImportProgress Progress(Sink);

		Progress.BeginTracking(TEXT("guid-h"), TEXT("Campus.frag"));
		TestEqual(TEXT("a declined notification is still tracked"), Progress.GetTrackedCount(), 1);

		Progress.PollWith([](const FString&) { return MakeSpawning(450, 900); });
		TestEqual(TEXT("nothing to update when there is no sink id"), Sink->Updates.Num(), 0);

		Progress.EndTracking(TEXT("guid-h"), false, TEXT("still fails without a notification"));
		TestEqual(TEXT("a declined entry can still toast on failure"), Sink->Failures.Num(), 1);
	}

	// Destructor cancel-on-teardown, pinned explicitly.
	{
		TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
		{
			FFragmentsImportProgress Progress(Sink);
			Progress.BeginTracking(TEXT("guid-i"), TEXT("Campus.frag"));
		}
		TestEqual(TEXT("a surviving entry is cancelled at the sink when the tracker is destroyed"),
			Sink->Cancelled.Num(), 1);
	}

	return true;
}

// Cancelling imports -- one row, every import, or the batch loading dialog -- used to end them silently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentImportProgressCancelNoticeTest,
	"FragmentsUnreal.Import.Progress_CancelNotice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentImportProgressCancelNoticeTest::RunTest(const FString& Parameters)
{
	using namespace FragmentImportProgress;

	FImportNotice One = MakeCancelledNotice({ TEXT("Building_A") });
	TestEqual(TEXT("one cancel names the model"), One.Title.ToString(), FString(TEXT("Import cancelled: Building_A")));
	TestTrue(TEXT("and needs no detail line"), One.Detail.IsEmpty());

	FImportNotice Two = MakeCancelledNotice({ TEXT("A"), TEXT("B") });
	TestEqual(TEXT("several give the count"), Two.Title.ToString(), FString(TEXT("2 imports cancelled")));
	TestEqual(TEXT("and name them"), Two.Detail.ToString(), FString(TEXT("A, B")));

	TArray<FString> Twelve;
	for (int32 Index = 0; Index < 12; ++Index) { Twelve.Add(FString::Printf(TEXT("M%d"), Index)); }
	FImportNotice Many = MakeCancelledNotice(Twelve);
	TestEqual(TEXT("a big batch gives the count"), Many.Title.ToString(), FString(TEXT("12 imports cancelled")));
	TestEqual(TEXT("and the first few names"), Many.Detail.ToString(), FString(TEXT("M0, M1, M2 and 9 more")));

	TestTrue(TEXT("nothing cancelled, nothing to say"), MakeCancelledNotice({}).Title.IsEmpty());

	TestEqual(TEXT("a stopped batch says it stopped"), MakeLoadingStoppedNotice(44).Title.ToString(), FString(TEXT("Loading stopped")));
	TestEqual(TEXT("and how many files never started"), MakeLoadingStoppedNotice(44).Detail.ToString(), FString(TEXT("44 files not loaded")));
	TestEqual(TEXT("singular for one"), MakeLoadingStoppedNotice(1).Detail.ToString(), FString(TEXT("1 file not loaded")));
	TestTrue(TEXT("a batch that finished has nothing to say"), MakeLoadingStoppedNotice(0).Title.IsEmpty());

	// The tracker shows each as one notice, and none for an empty gesture.
	TSharedRef<FRecordingSink> Sink = MakeShared<FRecordingSink>();
	FFragmentsImportProgress Progress(Sink);
	Progress.ReportCancelled({ TEXT("Building_A") });
	Progress.ReportCancelled(Twelve);
	Progress.ReportCancelled({});
	Progress.ReportLoadingStopped(44);
	Progress.ReportLoadingStopped(0);
	TestEqual(TEXT("one notice per gesture that stopped something"), Sink->Notices.Num(), 3);
	TestTrue(TEXT("in order"), Sink->Notices.Num() == 3 && Sink->Notices[0] == TEXT("Import cancelled: Building_A")
		&& Sink->Notices[1] == TEXT("12 imports cancelled") && Sink->Notices[2] == TEXT("Loading stopped"));
	TestEqual(TEXT("a notice is not a failure"), Sink->Failures.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
