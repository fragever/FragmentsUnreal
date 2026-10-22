// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentCellLodCombine.h"

#if WITH_DEV_AUTOMATION_TESTS

using UE::Fragments::CellLod::CombineTargetStates;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellLodCombineEmptyIsNotSkipped,
	"FragmentsUnreal.Spatial.CellLodCombine.EmptyIsNotSkipped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellLodCombineEmptyIsNotSkipped::RunTest(const FString& Parameters)
{
	// The case that must never collapse into "everything culled".
	ECellLodState Out = ECellLodState::Active_LOD0;
	TestFalse(TEXT("empty input is refused"),
		CombineTargetStates(TArray<ECellLodState>(), Out));
	TestTrue(TEXT("OutCombined is left untouched on refusal"),
		Out == ECellLodState::Active_LOD0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellLodCombineMostDetailedWins,
	"FragmentsUnreal.Spatial.CellLodCombine.MostDetailedWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellLodCombineMostDetailedWins::RunTest(const FString& Parameters)
{
	ECellLodState Out = ECellLodState::Skipped;

	const TArray<ECellLodState> Single = { ECellLodState::Merged_LOD1 };
	TestTrue(TEXT("single view accepted"), CombineTargetStates(Single, Out));
	TestTrue(TEXT("single view passes through"), Out == ECellLodState::Merged_LOD1);

	// The union rule: one interested view promotes the cell for every viewport.
	const TArray<ECellLodState> Mixed = { ECellLodState::Skipped, ECellLodState::Active_LOD0 };
	TestTrue(TEXT("mixed accepted"), CombineTargetStates(Mixed, Out));
	TestTrue(TEXT("one interested view wins over a culling one"),
		Out == ECellLodState::Active_LOD0);

	const TArray<ECellLodState> MergedVsSkipped = { ECellLodState::Skipped, ECellLodState::Merged_LOD1 };
	TestTrue(TEXT("merged-vs-skipped accepted"), CombineTargetStates(MergedVsSkipped, Out));
	TestTrue(TEXT("merged beats skipped"), Out == ECellLodState::Merged_LOD1);

	const TArray<ECellLodState> Lod0VsMerged = { ECellLodState::Merged_LOD1, ECellLodState::Active_LOD0 };
	TestTrue(TEXT("lod0-vs-merged accepted"), CombineTargetStates(Lod0VsMerged, Out));
	TestTrue(TEXT("LOD0 beats merged"), Out == ECellLodState::Active_LOD0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellLodCombineUnanimousSkipIsSkip,
	"FragmentsUnreal.Spatial.CellLodCombine.UnanimousSkipIsSkip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellLodCombineUnanimousSkipIsSkip::RunTest(const FString& Parameters)
{
	// Distinct from the empty case above: here views EXIST and all agree to cull.
	ECellLodState Out = ECellLodState::Active_LOD0;
	const TArray<ECellLodState> AllSkip = {
		ECellLodState::Skipped, ECellLodState::Skipped, ECellLodState::Skipped };
	TestTrue(TEXT("unanimous skip accepted"), CombineTargetStates(AllSkip, Out));
	TestTrue(TEXT("unanimous skip culls"), Out == ECellLodState::Skipped);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
