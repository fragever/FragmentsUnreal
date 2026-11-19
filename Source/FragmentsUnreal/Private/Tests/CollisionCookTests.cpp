// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// Issue #4: a promoted model lost its collision to the distance uncook (and never got it back), so clicks and traces
// passed through a building that was fully drawn. A promoted model is never uncooked, and a promotion's forced
// collision counts as a cooked body.
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentCollisionCook.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCollisionUncookStepTest, "FragmentsUnreal.CollisionCook.UncookStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCollisionUncookStepTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::CollisionCook;
	const float Pad = 1000.f;
	TestEqual(TEXT("inside the uncook distance: nothing"), NextUncookStep(false, 500.f, Pad, 0.f, 50.f), EUncookStep::None);
	TestEqual(TEXT("just left it: the cooldown starts"), NextUncookStep(false, 1500.f, Pad, 0.f, 50.f), EUncookStep::StartCooldown);
	TestEqual(TEXT("cooldown running: wait"), NextUncookStep(false, 1500.f, Pad, 60.f, 55.f), EUncookStep::None);
	TestEqual(TEXT("cooldown over: uncook"), NextUncookStep(false, 1500.f, Pad, 60.f, 60.f), EUncookStep::Uncook);
	TestEqual(TEXT("back inside with a cooldown pending: cancel"), NextUncookStep(false, 500.f, Pad, 60.f, 55.f), EUncookStep::Cancel);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCollisionPromotedNeverUncooksTest, "FragmentsUnreal.CollisionCook.PromotedNeverUncooks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCollisionPromotedNeverUncooksTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::CollisionCook;
	const float Pad = 1000.f;
	// The demo's case: promoted on focus, the camera 171 m out, a long still, then the first move.
	TestEqual(TEXT("promoted, far, no cooldown: none started"), NextUncookStep(true, 17100.f, Pad, 0.f, 300.f), EUncookStep::None);
	TestEqual(TEXT("promoted, far, cooldown long over: never uncooked"), NextUncookStep(true, 17100.f, Pad, 60.f, 300.f), EUncookStep::Cancel);
	TestEqual(TEXT("promoted, close: none"), NextUncookStep(true, 100.f, Pad, 0.f, 300.f), EUncookStep::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCollisionForcedCountsAsCookedTest, "FragmentsUnreal.CollisionCook.ForcedCountsAsCooked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCollisionForcedCountsAsCookedTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::CollisionCook;
	TestFalse(TEXT("nothing cooked, not forced: no body"), HasCookedBody(false, false, false));
	TestTrue(TEXT("model cooked"), HasCookedBody(true, false, false));
	TestTrue(TEXT("chunked cook passed this HISMC"), HasCookedBody(false, true, false));
	TestTrue(TEXT("forced by a promotion (which cooks it): a body"), HasCookedBody(false, false, true));
	return true;
}

#endif
