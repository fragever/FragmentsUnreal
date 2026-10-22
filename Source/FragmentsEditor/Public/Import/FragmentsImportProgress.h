// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Import/FragmentImportProgressMath.h"
#include "Import/FragmentsImportProgressSink.h"
#include "Importer/FragmentImportProgressSnapshot.h"

class UFragmentsImporter;

/** Keeps the editor honest about imports that are still running. */
class FRAGMENTSEDITOR_API FFragmentsImportProgress
{
public:
	explicit FFragmentsImportProgress(TSharedRef<IFragmentsImportProgressSink> InSink);
	~FFragmentsImportProgress();

	// Non-copyable: a copy would register a second ticker bound to raw `this` (see TickerHandle below) and would double-cancel every sink id still open in Entries when either copy tears down.
	FFragmentsImportProgress(const FFragmentsImportProgress&) = delete;
	FFragmentsImportProgress& operator=(const FFragmentsImportProgress&) = delete;

	/** Start showing progress for a model whose ASYNC phase is about to begin. */
	void BeginTracking(const FString& ModelGuid, const FString& DisplayName);

	/** Finish a tracked model. */
	void EndTracking(const FString& ModelGuid, bool bSuccess, const FString& ErrorMessage);

	/** End a tracked model with no outcome shown. */
	void CancelTracking(const FString& ModelGuid);

	/** Raise a failure toast with no progress entry, for failures that never reached the async phase. */
	void ReportFailure(const FString& ErrorMessage);

	/** One notice for one cancel gesture, however many models it stopped. */
	void ReportCancelled(const TArray<FString>& ModelNames);

	/** One notice when a batch's loading dialog was cancelled with files still to load. */
	void ReportLoadingStopped(int32 FilesNotLoaded);

	/** One poll pass. */
	void PollWith(TFunctionRef<FFragmentImportProgressSnapshot(const FString&)> SnapshotFor);

	int32 GetTrackedCount() const { return Entries.Num(); }

private:
	struct FEntry
	{
		/** Opaque sink id, or 0 when the sink declined to create a notification (which happens routinely: the engine drops progress if the editor is not the active window). */
		int32 SinkId = 0;
		FString DisplayName;
		/** Consecutive polls this model was neither spawning nor queued. */
		int32 MissedTicks = 0;
	};

	TMap<FString, FEntry> Entries;
	TSharedRef<IFragmentsImportProgressSink> Sink;
	FTSTicker::FDelegateHandle TickerHandle;

	/** True for the duration of Tick(). */
	bool bInTick = false;

	/** Ends the entry at the sink and drops it. Returns false if the guid was not tracked. */
	bool CloseEntry(const FString& ModelGuid);

	void EnsureTicking();
	void StopTickingIfIdle();

	/** The real ticker body: resolves snapshots from the live importer and calls PollWith. */
	bool Tick(float DeltaSeconds);
};
