// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Importer/FragmentImportProgressSnapshot.h"

/** Pure arithmetic and text for the import progress bar. */
namespace FragmentImportProgress
{
	/** Fraction of the bar owned by the item-spawn loop; the finalize tail owns the rest. */
	constexpr float ItemsShareOfBar = 0.9f;

	/** Number of tail phases the last (1 - ItemsShareOfBar) is divided between: WaitingOnMeshBuilds, BuildingBuckets, MergingCells, ApplyingOverrides. */
	constexpr int32 TailPhaseCount = 4;

	struct FWorkUnits
	{
		int32 WorkDone = 0;
		/** NEVER zero or negative. */
		int32 WorkToDo = 1;
	};

	/** Map a snapshot onto the units FSlateNotificationManager wants. */
	FRAGMENTSEDITOR_API FWorkUnits ComputeWorkUnits(const FFragmentImportProgressSnapshot& Snapshot);

	/** A notice's text. An empty Title means "show nothing". */
	struct FImportNotice
	{
		FText Title;
		FText Detail;
	};

	/** Cancelled imports: one names the model ("Import cancelled: Building_A"); several give the count and the first few names ("12 imports cancelled" / "A, B, C and 9 more"). */
	FRAGMENTSEDITOR_API FImportNotice MakeCancelledNotice(const TArray<FString>& ModelNames);

	/** A batch whose loading dialog was cancelled: how many files never started. Zero, nothing. */
	FRAGMENTSEDITOR_API FImportNotice MakeLoadingStoppedNotice(int32 FilesNotLoaded);

	/** What the import is doing now, e.g. "waiting on 3 mesh builds". */
	FRAGMENTSEDITOR_API FText MakeStageText(const FFragmentImportProgressSnapshot& Snapshot);

	/** Consecutive polls a tracked model may be neither spawning nor queued before we conclude it is gone. */
	constexpr int32 OrphanTickThreshold = 5;

	/** Has this tracked model disappeared without reporting completion? */
	FRAGMENTSEDITOR_API bool IsOrphaned(
		const FFragmentImportProgressSnapshot& Snapshot, int32 ConsecutiveMissedTicks);
}
