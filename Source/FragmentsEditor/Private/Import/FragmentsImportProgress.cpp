// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Import/FragmentsImportProgress.h"

#include "Editor.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "Importer/FragmentsImporter.h"
#include "Utils/FragmentsLog.h"

#define LOCTEXT_NAMESPACE "FragmentsImportProgress"

namespace
{
	/** 10 Hz. */
	constexpr float PollIntervalSeconds = 0.1f;
}

FFragmentsImportProgress::FFragmentsImportProgress(TSharedRef<IFragmentsImportProgressSink> InSink)
	: Sink(InSink)
{
}

FFragmentsImportProgress::~FFragmentsImportProgress()
{
	// Every surviving entry is ended at the sink.
	for (const TPair<FString, FEntry>& Pair : Entries)
	{
		if (Pair.Value.SinkId != 0)
		{
			Sink->CancelProgress(Pair.Value.SinkId);
		}
	}
	Entries.Empty();

	// Deliberately NOT StopTickingIfIdle(): that function early-returns while bInTick is true, which protects Tick() from double-unregistering itself mid-callback (see its own comment) but is wrong here.
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FFragmentsImportProgress::BeginTracking(const FString& ModelGuid, const FString& DisplayName)
{
	if (ModelGuid.IsEmpty() || Entries.Contains(ModelGuid))
	{
		return;
	}

	FEntry Entry;
	Entry.DisplayName = DisplayName;

	// Use a positive placeholder until BuildHISMSpawnQueue provides the real total.
	Entry.SinkId = Sink->StartProgress(FText::FromString(DisplayName), 1);

	UE_LOG(LogFragments, Verbose,
		TEXT("[ImportProgress] Tracking '%s' (%s), sink id %d."),
		*DisplayName, *ModelGuid, Entry.SinkId);

	Entries.Add(ModelGuid, MoveTemp(Entry));
	EnsureTicking();
}

bool FFragmentsImportProgress::CloseEntry(const FString& ModelGuid)
{
	FEntry Entry;
	if (!Entries.RemoveAndCopyValue(ModelGuid, Entry))
	{
		return false;
	}
	if (Entry.SinkId != 0)
	{
		Sink->CancelProgress(Entry.SinkId);
	}
	StopTickingIfIdle();
	return true;
}

void FFragmentsImportProgress::EndTracking(
	const FString& ModelGuid, bool bSuccess, const FString& ErrorMessage)
{
	// CloseEntry's result is deliberately ignored for the failure branch below: a failure that arrives for an untracked guid (a synchronous failure, or one arriving after an orphan reclaim) must STILL surface.
	CloseEntry(ModelGuid);

	if (!bSuccess)
	{
		ReportFailure(ErrorMessage);
	}
}

void FFragmentsImportProgress::CancelTracking(const FString& ModelGuid)
{
	CloseEntry(ModelGuid);
}

void FFragmentsImportProgress::ReportFailure(const FString& ErrorMessage)
{
	const FText Detail = ErrorMessage.IsEmpty()
		? LOCTEXT("ImportFailedNoDetail", "The import failed without reporting a reason. See the log.")
		: FText::FromString(ErrorMessage);

	Sink->ShowFailure(LOCTEXT("ImportFailedTitle", "Fragment import failed"), Detail);
}

void FFragmentsImportProgress::ReportCancelled(const TArray<FString>& ModelNames)
{
	const FragmentImportProgress::FImportNotice Notice = FragmentImportProgress::MakeCancelledNotice(ModelNames);
	if (!Notice.Title.IsEmpty())
	{
		Sink->ShowNotice(Notice.Title, Notice.Detail);
	}
}

void FFragmentsImportProgress::ReportLoadingStopped(int32 FilesNotLoaded)
{
	const FragmentImportProgress::FImportNotice Notice = FragmentImportProgress::MakeLoadingStoppedNotice(FilesNotLoaded);
	if (!Notice.Title.IsEmpty())
	{
		Sink->ShowNotice(Notice.Title, Notice.Detail);
	}
}

void FFragmentsImportProgress::PollWith(
	TFunctionRef<FFragmentImportProgressSnapshot(const FString&)> SnapshotFor)
{
	// Collected first, mutated after: CloseEntry removes from Entries, and removing during a range-for over the same map is undefined.
	TArray<FString> Orphaned;

	for (TPair<FString, FEntry>& Pair : Entries)
	{
		const FFragmentImportProgressSnapshot Snapshot = SnapshotFor(Pair.Key);
		FEntry& Entry = Pair.Value;

		// Update missed ticks before orphan detection; queued models reset the counter every poll.
		if (Snapshot.bIsSpawning || Snapshot.bIsQueuedForSpawn)
		{
			Entry.MissedTicks = 0;
		}
		else
		{
			++Entry.MissedTicks;
		}

		if (FragmentImportProgress::IsOrphaned(Snapshot, Entry.MissedTicks))
		{
			Orphaned.Add(Pair.Key);
			continue;
		}

		if (!Snapshot.bIsSpawning)
		{
			// Queued behind another model, or in the one-frame window before ProcessLoadedFragment commits to a path.
			continue;
		}

		// The deferred-interior wave restarts the spawn pipeline with a fresh TotalHISMItems AFTER completion already fired for this model.
		if (Snapshot.bIsDeferredInterior)
		{
			continue;
		}

		if (Entry.SinkId == 0)
		{
			continue; // no notification was created; nothing to update
		}

		const FragmentImportProgress::FWorkUnits Units =
			FragmentImportProgress::ComputeWorkUnits(Snapshot);

		Sink->UpdateProgress(Entry.SinkId, Units.WorkDone, Units.WorkToDo, FText::FromString(Entry.DisplayName));
	}

	for (const FString& ModelGuid : Orphaned)
	{
		// Silently.
		UE_LOG(LogFragments, Warning,
			TEXT("[ImportProgress] '%s' stopped spawning without reporting completion; "
			     "reclaiming its progress entry. Expected after a reimport/unload that aborted an "
			     "in-flight spawn (PurgeModelState does not fire OnProcessLoadedFragmentComplete)."),
			*ModelGuid);
		CloseEntry(ModelGuid);
	}
}

bool FFragmentsImportProgress::Tick(float /*DeltaSeconds*/)
{
	// bInTick guards StopTickingIfIdle (called transitively via CloseEntry, below) against removing the ticker from inside its own callback.
	TGuardValue<bool> InTickGuard(bInTick, true);

	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
	UFragmentsImporter* Importer = Subsystem ? Subsystem->GetImporter() : nullptr;

	if (!Importer)
	{
		// No importer means nothing can be in flight.
		TArray<FString> All;
		Entries.GetKeys(All);
		for (const FString& ModelGuid : All)
		{
			CloseEntry(ModelGuid);
		}

		// Reset the ticker handle before returning false to allow later registration.
		TickerHandle.Reset();
		return false; // unregister this ticker
	}

	PollWith([Importer](const FString& ModelGuid)
	{
		return Importer->GetImportProgressSnapshot(ModelGuid);
	});

	if (Entries.Num() == 0)
	{
		// Clear the ticker handle before returning false so later work can register a new ticker.
		TickerHandle.Reset();
		return false;
	}

	return true; // false unregisters the ticker
}

void FFragmentsImportProgress::EnsureTicking()
{
	if (TickerHandle.IsValid())
	{
		return;
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FFragmentsImportProgress::Tick), PollIntervalSeconds);
}

void FFragmentsImportProgress::StopTickingIfIdle()
{
	// Never remove the ticker from inside its own callback.
	if (bInTick)
	{
		return;
	}

	if (Entries.Num() == 0 && TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
