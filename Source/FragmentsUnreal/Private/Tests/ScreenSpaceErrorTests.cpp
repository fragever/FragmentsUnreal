// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentScreenSpaceError.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScreenSpaceErrorMathTest,
	"FragmentsUnreal.ScreenSpaceError.CoreMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FScreenSpaceErrorMathTest::RunTest(const FString& Parameters)
{
	using namespace FragmentSSE;

	// geoError = coeff * radius
	TestTrue(TEXT("geoError linear"), FMath::IsNearlyEqual(GeoError(0.2f, 1000.0f), 200.0f));
	TestTrue(TEXT("geoError zero coeff"), FMath::IsNearlyEqual(GeoError(0.0f, 1000.0f), 0.0f));

	// SseCoeff = height * proj11 * 0.5
	TestTrue(TEXT("sseCoeff"), FMath::IsNearlyEqual(SseCoeff(1080.0f, 1.0f), 540.0f));

	// ScreenErrorPx = geoErr * coeff / dist; halving distance doubles error.
	const float Near = ScreenErrorPx(200.0f, 500.0f, 540.0f);
	const float Far  = ScreenErrorPx(200.0f, 1000.0f, 540.0f);
	TestTrue(TEXT("closer = larger error"), Near > Far);
	TestTrue(TEXT("inverse-distance"), FMath::IsNearlyEqual(Near, Far * 2.0f, 0.01f));

	// Degenerate distance => effectively infinite error (always refine).
	TestTrue(TEXT("zero dist huge"), ScreenErrorPx(200.0f, 0.0f, 540.0f) > 1.0e9f);

	// RefineDecision hysteresis: threshold 16, coarsen 0.75 -> coarsen edge = 12.
	TestEqual(TEXT("above threshold refines"), RefineDecision(20.0f, 16.0f, 0.75f), 1);
	TestEqual(TEXT("below coarsen coarsens"), RefineDecision(10.0f, 16.0f, 0.75f), -1);
	TestEqual(TEXT("in band holds"), RefineDecision(14.0f, 16.0f, 0.75f), 0);
	TestEqual(TEXT("at threshold holds"), RefineDecision(16.0f, 16.0f, 0.75f), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
