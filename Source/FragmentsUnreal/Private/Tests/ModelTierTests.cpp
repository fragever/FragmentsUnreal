// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentModelTier.h"
#include "Spatial/FragmentProxyTiers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A 50m-radius building under the reference view, at the spec's shipped defaults. */
	FragmentModelTier::FDecisionInputs MakeInputs(float DistCm, FragmentModelTier::ETier Current)
	{
		FragmentModelTier::FDecisionInputs In;
		In.ModelBoundsRadiusCm = 5000.0f;
		In.DistToBoxCm         = DistCm;
		In.SseCoeff            = FragmentProxyTiers::ViewCoeff(
			FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);
		In.GeoCoeff            = 0.033f;
		In.ThresholdPx         = 16.0f;
		In.CoarsenFactor       = 0.75f;
		In.Current             = Current;
		return In;
	}
}

/** The band is the whole point: a model between the refine and coarsen thresholds must hold, or a camera hovering at the boundary rebuilds every cell's visibility every frame. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModelTierHysteresisTest,
	"FragmentsUnreal.ModelTier.Hysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModelTierHysteresisTest::RunTest(const FString& Parameters)
{
	using namespace FragmentModelTier;

	// These inputs place the refine threshold near 10000 cm.
	TestEqual(TEXT("near: proxy promotes to cells"),
		Decide(MakeInputs(5000.0f, ETier::Proxy)), ETier::Cells);
	TestEqual(TEXT("near: cells stay cells"),
		Decide(MakeInputs(5000.0f, ETier::Cells)), ETier::Cells);

	// Well beyond the coarsen threshold (~13300cm) -> Proxy regardless of origin.
	TestEqual(TEXT("far: cells demote to proxy"),
		Decide(MakeInputs(40000.0f, ETier::Cells)), ETier::Proxy);
	TestEqual(TEXT("far: proxy stays proxy"),
		Decide(MakeInputs(40000.0f, ETier::Proxy)), ETier::Proxy);

	// Inside the band (between ~10000 and ~13300cm) -> hold whatever we already were.
	TestEqual(TEXT("band: holds cells"),
		Decide(MakeInputs(11500.0f, ETier::Cells)), ETier::Cells);
	TestEqual(TEXT("band: holds proxy"),
		Decide(MakeInputs(11500.0f, ETier::Proxy)), ETier::Proxy);

	return true;
}

/** Both overrides force Cells. Neither can force Proxy — there is no such override by design. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModelTierOverridesTest,
	"FragmentsUnreal.ModelTier.Overrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModelTierOverridesTest::RunTest(const FString& Parameters)
{
	using namespace FragmentModelTier;

	// 400m away, which SSE alone resolves to Proxy.
	FDecisionInputs Far = MakeInputs(40000.0f, ETier::Proxy);
	TestEqual(TEXT("baseline far is proxy"), Decide(Far), ETier::Proxy);

	FDecisionInputs Filtered = Far;
	Filtered.bAnyCellFloorFiltered = true;
	TestEqual(TEXT("floor filter forces cells so the filter is expressible"),
		Decide(Filtered), ETier::Cells);

	FDecisionInputs Clicked = Far;
	Clicked.bPromoteOverride = true;
	TestEqual(TEXT("click override forces cells"), Decide(Clicked), ETier::Cells);

	FDecisionInputs NoProxy = Far;
	NoProxy.bProxyAvailable = false;
	TestEqual(TEXT("no proxy component forces cells - Proxy tier has nothing to render"),
		Decide(NoProxy), ETier::Cells);

	return true;
}

/** Sub-LOD 1 derives its near edge from this distance. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModelTierDemoteDistanceTest,
	"FragmentsUnreal.ModelTier.DemoteDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModelTierDemoteDistanceTest::RunTest(const FString& Parameters)
{
	using namespace FragmentModelTier;

	const float Coeff = FragmentProxyTiers::ViewCoeff(
		FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);

	// 0.033 * 5000 * 960.12 / (16 * 0.75) = 13201.6 cm
	const float D = DemoteDistanceCm(0.033f, 5000.0f, Coeff, 16.0f, 0.75f);
	TestTrue(FString::Printf(TEXT("demote distance ~13202cm (got %.1f)"), D),
		FMath::IsNearlyEqual(D, 13201.6f, 5.0f));

	// Must agree with Decide: just inside demotes, just outside holds.
	FDecisionInputs Inside = MakeInputs(D * 0.99f, ETier::Cells);
	FDecisionInputs Outside = MakeInputs(D * 1.01f, ETier::Cells);
	TestEqual(TEXT("just inside demote distance still holds cells"),
		Decide(Inside), ETier::Cells);
	TestEqual(TEXT("just beyond demote distance drops to proxy"),
		Decide(Outside), ETier::Proxy);

	// Degenerate inputs must not divide by zero or return a negative distance.
	TestEqual(TEXT("zero threshold yields zero"), DemoteDistanceCm(0.033f, 5000.0f, Coeff, 0.0f, 0.75f), 0.0f);
	TestEqual(TEXT("zero coarsen factor yields zero"), DemoteDistanceCm(0.033f, 5000.0f, Coeff, 16.0f, 0.0f), 0.0f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
