// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Utils/PendingMeshFinalize.h"

#if WITH_DEV_AUTOMATION_TESTS

/** The outlier predicate is a pure function so the threshold semantics are pinned without needing a real mesh build. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalizeOutlierPredicateTest,
	"FragmentsUnreal.MeshFinalize.OutlierPredicate_ThresholdSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFinalizeOutlierPredicateTest::RunTest(const FString& Parameters)
{
	TestTrue (TEXT("above threshold logs"),        UFragmentsImporter::ShouldLogFinalizeOutlier(150.0f, 100.0f));
	TestFalse(TEXT("below threshold silent"),      UFragmentsImporter::ShouldLogFinalizeOutlier( 50.0f, 100.0f));
	TestFalse(TEXT("exactly at threshold silent"), UFragmentsImporter::ShouldLogFinalizeOutlier(100.0f, 100.0f));

	// Zero disables.
	TestFalse(TEXT("zero threshold disables"),     UFragmentsImporter::ShouldLogFinalizeOutlier(500.0f,   0.0f));
	TestFalse(TEXT("negative threshold disables"), UFragmentsImporter::ShouldLogFinalizeOutlier(500.0f,  -1.0f));

	return true;
}

/** The two new census fields must default to 0 for producers that never set them, so a missing value reads as "not measured" in the log rather than as a real ratio of zero. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalizeVertexCensusDefaultTest,
	"FragmentsUnreal.MeshFinalize.PendingMeshFinalize_VertexCensusDefaultsZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFinalizeVertexCensusDefaultTest::RunTest(const FString& Parameters)
{
	FPendingMeshFinalize Payload;
	TestEqual(TEXT("SourceVertexCount defaults 0"), Payload.SourceVertexCount, 0);
	TestEqual(TEXT("CornerCount defaults 0"),       Payload.CornerCount,       0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
