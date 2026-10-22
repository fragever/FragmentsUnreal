// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Save Placement on a chunked-import model used to write the actor transform verbatim.
#include "Misc/AutomationTest.h"
#include "Overrides/FragmentPlacementMath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// A base-coordinates origin frame and a saved placement, both with rotation, so a composition in the wrong order cannot pass by commuting.
	const FTransform OriginFrame(FRotator(0.f, 30.f, 0.f), FVector(1000.f, -2500.f, 40.f));
	const FTransform Placement(FRotator(0.f, -75.f, 0.f), FVector(-320.f, 910.f, 0.f));
	const FTransform PlacementTestDelta(FRotator(0.f, 12.f, 0.f), FVector(55.f, -140.f, 3.f));
	constexpr float Tol = 1.e-2f;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPlacementComposeTest,
	"FragmentsUnreal.Overrides.Placement_SavedPlacementReproducesCurrentPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPlacementComposeTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::Placement;
	const FTransform BakedFrame = Placement * OriginFrame;

	// Saving without movement preserves the placement.
	TestTrue(TEXT("unmoved model saves the placement it was imported with"),
		ComposeSavedPlacement(BakedFrame, Placement, FTransform::Identity).Equals(Placement, Tol));

	// The general property: reimporting with the saved value reproduces the current pose.
	const FTransform Saved = ComposeSavedPlacement(BakedFrame, Placement, PlacementTestDelta);
	TestTrue(TEXT("next import reproduces the moved pose (base-coordinates origin)"),
		(Saved * OriginFrame).Equals(BakedFrame * PlacementTestDelta, Tol));

	// World-origin mode: the origin frame is identity, so the baked frame IS the placement.
	TestTrue(TEXT("world-origin mode composes placement then delta"),
		ComposeSavedPlacement(Placement, Placement, PlacementTestDelta).Equals(Placement * PlacementTestDelta, Tol));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPlacementResetTest,
	"FragmentsUnreal.Overrides.Placement_ResetShowsUnplacedPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPlacementResetTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::Placement;
	const FTransform BakedFrame = Placement * OriginFrame;

	const FTransform ResetDelta = ActorDeltaForNoPlacement(BakedFrame, Placement);
	TestTrue(TEXT("reset puts the model where an unplaced import would"),
		(BakedFrame * ResetDelta).Equals(OriginFrame, Tol));
	TestTrue(TEXT("...and saving from there saves no placement"),
		ComposeSavedPlacement(BakedFrame, Placement, ResetDelta).Equals(FTransform::Identity, Tol));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPlacementFieldsTest,
	"FragmentsUnreal.Overrides.Placement_TypedPlacementShowsAndSavesBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPlacementFieldsTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::Placement;
	const FTransform BakedFrame = Placement * OriginFrame;
	const FTransform Typed(FRotator(0.f, 140.f, 0.f), FVector(2200.f, 75.f, -12.f));

	// The panel's Location/Rotation fields: typing a placement moves the actor by this delta...
	const FTransform DeltaForTyped = ActorDeltaForPlacement(BakedFrame, Placement, Typed);
	TestTrue(TEXT("the model lands where an import with the typed placement would put it"),
		(BakedFrame * DeltaForTyped).Equals(Typed * OriginFrame, Tol));

	// The displayed fields must match the saved placement.
	TestTrue(TEXT("reading the live pose back gives the typed placement"),
		ComposeSavedPlacement(BakedFrame, Placement, DeltaForTyped).Equals(Typed, Tol));

	TestTrue(TEXT("typing the placement it was imported with is no move at all"),
		ActorDeltaForPlacement(BakedFrame, Placement, Placement).Equals(FTransform::Identity, Tol));
	TestTrue(TEXT("an identity placement is the same as Revert"),
		ActorDeltaForPlacement(BakedFrame, Placement, FTransform::Identity)
			.Equals(ActorDeltaForNoPlacement(BakedFrame, Placement), Tol));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPlacementClipboardTest,
	"FragmentsUnreal.Overrides.Placement_ClipboardMatchesDetailsPanel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPlacementClipboardTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::Placement;
	const FVector Loc(19970.5, -21710.25, 390.0);
	const FRotator Rot(12.5, -75.0, 3.0);   // Pitch, Yaw, Roll

	// What the Details panel puts on the clipboard for a Location / Rotation property (ExportText).
	TestEqual(TEXT("location copies in the Details panel's format"),
		FormatLocationForClipboard(Loc), FString(TEXT("(X=19970.500000,Y=-21710.250000,Z=390.000000)")));
	TestEqual(TEXT("rotation copies in the Details panel's format"),
		FormatRotationForClipboard(Rot), FString(TEXT("(Pitch=12.500000,Yaw=-75.000000,Roll=3.000000)")));

	FVector OutLoc;
	FRotator OutRot;
	TestTrue(TEXT("a copied location pastes back"), ParseLocationFromClipboard(FormatLocationForClipboard(Loc), OutLoc));
	TestTrue(TEXT("...unchanged"), OutLoc.Equals(Loc, 1.e-4));
	TestTrue(TEXT("a copied rotation pastes back"), ParseRotationFromClipboard(FormatRotationForClipboard(Rot), OutRot));
	TestTrue(TEXT("...unchanged"), OutRot.Equals(Rot, 1.e-4));

	// FVector::ToString's spaced form, which other tools and logs produce.
	TestTrue(TEXT("a spaced location pastes"), ParseLocationFromClipboard(TEXT("X=1.0 Y=2.0 Z=3.0"), OutLoc));
	TestTrue(TEXT("...with its values"), OutLoc.Equals(FVector(1.0, 2.0, 3.0), 1.e-4));
	// FRotator::ToString's short form.
	TestTrue(TEXT("a short-form rotation pastes"), ParseRotationFromClipboard(TEXT("P=1.0 Y=2.0 R=3.0"), OutRot));
	TestTrue(TEXT("...with its values"), OutRot.Equals(FRotator(1.0, 2.0, 3.0), 1.e-4));

	// A paste that is not a placement must change nothing.
	TestFalse(TEXT("text is refused as a location"), ParseLocationFromClipboard(TEXT("MI_Frag_AA_MetalWhite"), OutLoc));
	TestFalse(TEXT("a partial location is refused"), ParseLocationFromClipboard(TEXT("(X=1.0,Y=2.0)"), OutLoc));
	TestFalse(TEXT("a location is refused as a rotation"),
		ParseRotationFromClipboard(FormatLocationForClipboard(Loc), OutRot));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
