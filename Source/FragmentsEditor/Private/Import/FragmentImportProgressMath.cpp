// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Import/FragmentImportProgressMath.h"

#define LOCTEXT_NAMESPACE "FragmentsImportProgress"

namespace FragmentImportProgress
{

FWorkUnits ComputeWorkUnits(const FFragmentImportProgressSnapshot& Snapshot)
{
	FWorkUnits Out;

	const int32 TotalItems = FMath::Max(0, Snapshot.TotalItems);
	if (TotalItems == 0)
	{
		// The total is not known yet -- BeginTracking opens with a placeholder before the first poll, and BuildHISMSpawnQueue has not assigned TotalHISMItems.
		Out.WorkDone = 0;
		Out.WorkToDo = 1;
		return Out;
	}

	// max(1, ...) so a tiny model (fewer than 9 items) still reserves a whole unit for the tail rather than a zero-width tail that makes the bar jump straight from 90% to done.
	const int32 TailUnits = FMath::Max(1, FMath::RoundToInt(
		TotalItems * (1.f - ItemsShareOfBar) / ItemsShareOfBar));

	Out.WorkToDo = TotalItems + TailUnits;

	if (Snapshot.Phase == EFragmentImportPhase::Spawning)
	{
		// Clamp progress because late shell items can exceed the initial total.
		Out.WorkDone = FMath::Clamp(Snapshot.ItemsSpawned, 0, TotalItems);
	}
	else
	{
		// (value - 1) maps WaitingOnMeshBuilds..ApplyingOverrides onto 0..3.
		const int32 PhaseIndex = (int32)Snapshot.Phase - 1;
		const float Fraction = FMath::Clamp(Snapshot.PhaseFraction, 0.f, 1.f);

		Out.WorkDone = TotalItems + FMath::RoundToInt(
			TailUnits * ((float)PhaseIndex + Fraction) / (float)TailPhaseCount);
	}

	Out.WorkDone = FMath::Clamp(Out.WorkDone, 0, Out.WorkToDo);
	return Out;
}

FText MakeStageText(const FFragmentImportProgressSnapshot& Snapshot)
{
	switch (Snapshot.Phase)
	{
	case EFragmentImportPhase::WaitingOnMeshBuilds:
		// The plural modifier uses the same positional argument as the displayed count.
		return FText::Format(
			LOCTEXT("StageWaitingOnMeshBuilds", "waiting on {0} {0}|plural(one=mesh build,other=mesh builds)"),
			Snapshot.InFlightMeshBuilds);
	case EFragmentImportPhase::BuildingBuckets:
		return LOCTEXT("StageBuildingBuckets", "building buckets");
	case EFragmentImportPhase::MergingCells:
		return LOCTEXT("StageMergingCells", "merging cells");
	case EFragmentImportPhase::ApplyingOverrides:
		return LOCTEXT("StageApplyingOverrides", "applying materials");
	case EFragmentImportPhase::Spawning:
	default:
		return LOCTEXT("StagePlacingGeometry", "placing geometry");
	}
}

bool IsOrphaned(const FFragmentImportProgressSnapshot& Snapshot, int32 ConsecutiveMissedTicks)
{
	if (Snapshot.bIsSpawning || Snapshot.bIsQueuedForSpawn)
	{
		return false;
	}
	return ConsecutiveMissedTicks >= OrphanTickThreshold;
}

FImportNotice MakeCancelledNotice(const TArray<FString>& ModelNames)
{
	if (ModelNames.Num() == 0)
	{
		return FImportNotice();
	}
	if (ModelNames.Num() == 1)
	{
		return { FText::Format(LOCTEXT("ImportCancelledOne", "Import cancelled: {0}"), FText::FromString(ModelNames[0])), FText::GetEmpty() };
	}

	// A toast is no place for 47 names: the first few and a count of the rest.
	constexpr int32 NamesShown = 3;
	const TArray<FString> Shown(ModelNames.GetData(), FMath::Min(NamesShown, ModelNames.Num()));
	const FText List = FText::FromString(FString::Join(Shown, TEXT(", ")));
	const int32 Rest = ModelNames.Num() - Shown.Num();
	return {
		FText::Format(LOCTEXT("ImportCancelledMany", "{0} imports cancelled"), ModelNames.Num()),
		Rest > 0 ? FText::Format(LOCTEXT("ImportCancelledNamesAndMore", "{0} and {1} more"), List, Rest) : List };
}

FImportNotice MakeLoadingStoppedNotice(int32 FilesNotLoaded)
{
	if (FilesNotLoaded <= 0)
	{
		return FImportNotice();
	}
	return {
		LOCTEXT("LoadingStoppedTitle", "Loading stopped"),
		FText::Format(LOCTEXT("LoadingStoppedDetail", "{0} {0}|plural(one=file,other=files) not loaded"), FilesNotLoaded) };
}

} // namespace FragmentImportProgress

#undef LOCTEXT_NAMESPACE
