// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentModelTier.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Exactly one caster per building, always the coarsest resident tier. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShadowOwnershipExactlyOneCasterTest,
	"FragmentsUnreal.ShadowOwnership.ExactlyOneCaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowOwnershipExactlyOneCasterTest::RunTest(const FString& Parameters)
{
	using namespace FragmentModelTier;

	for (const ETier Tier : { ETier::Proxy, ETier::Cells })
	{
		const int32 Casters = (ShouldProxyCast(Tier) ? 1 : 0) + (ShouldCellsCast(Tier) ? 1 : 0);
		TestEqual(
			FString::Printf(TEXT("tier %d has exactly one caster"), static_cast<int32>(Tier)),
			Casters, 1);
	}

	// The coarsest RESIDENT representation casts — that is the rule, not merely "one of them".
	TestTrue(TEXT("proxy tier: the proxy casts"),   ShouldProxyCast(ETier::Proxy));
	TestFalse(TEXT("proxy tier: cells do not"),     ShouldCellsCast(ETier::Proxy));
	TestFalse(TEXT("cells tier: the proxy does not"), ShouldProxyCast(ETier::Cells));
	TestTrue(TEXT("cells tier: cells cast"),        ShouldCellsCast(ETier::Cells));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
