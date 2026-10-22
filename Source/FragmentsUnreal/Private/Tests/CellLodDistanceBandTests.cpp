// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentCellLodCombine.h"

#if WITH_DEV_AUTOMATION_TESTS

using UE::Fragments::CellLod::ResolveDistanceBand;

// The hysteresis band must not leave a visible cell in Skipped state.

namespace
{
	constexpr float Lower = 1000.0f;   // Shell defaults: promote 1500, hysteresis 500
	constexpr float Upper = 2000.0f;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellLodBandSkippedCellComesBack,
	"FragmentsUnreal.Spatial.CellLodDistanceBand.SkippedCellInBandBecomesVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellLodBandSkippedCellComesBack::RunTest(const FString& Parameters)
{
	// The caller only reaches the band once the cell is in the frustum, so Skipped is never right here.
	const ECellLodState Out = ResolveDistanceBand(ECellLodState::Skipped, 1500.0, Lower, Upper);
	TestTrue(TEXT("an in-view cell inside the band is drawn"), Out != ECellLodState::Skipped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellLodBandCameraInsideCellAtZeroPromote,
	"FragmentsUnreal.Spatial.CellLodDistanceBand.CameraInsideCellWithZeroPromoteIsDrawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellLodBandCameraInsideCellAtZeroPromote::RunTest(const FString& Parameters)
{
	// LOD0PromoteCm 0: band [0, 0], camera inside the cell box so the distance is 0.
	const ECellLodState Out = ResolveDistanceBand(ECellLodState::Skipped, 0.0, 0.0f, 0.0f);
	TestTrue(TEXT("the cell the camera stands in is drawn"), Out != ECellLodState::Skipped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellLodBandHysteresisHoldsVisibleStates,
	"FragmentsUnreal.Spatial.CellLodDistanceBand.BandStillHoldsLod0AndLod1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellLodBandHysteresisHoldsVisibleStates::RunTest(const FString& Parameters)
{
	// The hysteresis itself must survive the fix: no LOD0/LOD1 flicker at the promote distance.
	TestTrue(TEXT("LOD0 holds in the band"),
		ResolveDistanceBand(ECellLodState::Active_LOD0, 1900.0, Lower, Upper) == ECellLodState::Active_LOD0);
	TestTrue(TEXT("LOD1 holds in the band"),
		ResolveDistanceBand(ECellLodState::Merged_LOD1, 1100.0, Lower, Upper) == ECellLodState::Merged_LOD1);
	TestTrue(TEXT("beyond the band is LOD1"),
		ResolveDistanceBand(ECellLodState::Active_LOD0, 2500.0, Lower, Upper) == ECellLodState::Merged_LOD1);
	TestTrue(TEXT("inside the band's lower edge is LOD0"),
		ResolveDistanceBand(ECellLodState::Merged_LOD1, 500.0, Lower, Upper) == ECellLodState::Active_LOD0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
