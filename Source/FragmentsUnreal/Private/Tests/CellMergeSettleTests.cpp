// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentCellLodCombine.h"

#if WITH_DEV_AUTOMATION_TESTS

using UE::Fragments::CellLod::StateWhenMergeSettles;

// Holes on a still camera: registration marks every cell Merged_LOD1 with its instances hidden, and a finished merge lands its component hidden.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeSettleLod1BecomesSkipped,
	"FragmentsUnreal.Spatial.CellMergeSettle.Lod1CellShowingNothingIsSkipped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeSettleLod1BecomesSkipped::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a cell marked LOD1 when its merge settles is Skipped"),
		StateWhenMergeSettles(ECellLodState::Merged_LOD1) == ECellLodState::Skipped);
	TestTrue(TEXT("a LOD0 cell keeps showing its instances"),
		StateWhenMergeSettles(ECellLodState::Active_LOD0) == ECellLodState::Active_LOD0);
	TestTrue(TEXT("a Skipped cell stays Skipped"),
		StateWhenMergeSettles(ECellLodState::Skipped) == ECellLodState::Skipped);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
