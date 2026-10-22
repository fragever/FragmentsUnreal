// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentCellCensus.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "Spatial/FragmentProxyTiers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Unit cube: local bounding-sphere radius = sqrt(3) ~= 1.732 cm; instance scale scales it. */
	FMergeSource MakeSource(int32 MaterialSlot, TConstArrayView<float> InstanceScales, int32 NumTris)
	{
		FMergeSource Src;
		Src.MaterialSlot = MaterialSlot;
		const FVector3f Corners[8] = {
			{-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
			{-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1} };
		for (const FVector3f& C : Corners)
		{
			Src.Positions.Add(C);
			Src.Normals.Add(FVector3f(0, 0, 1));
			Src.TangentsX.Add(FVector3f(1, 0, 0));
			Src.TangentYSigns.Add(1.0f);
			Src.UV0.Add(FVector2f::ZeroVector);
			Src.Colors.Add(FColor::White);
		}
		for (int32 t = 0; t < NumTris; ++t)
		{
			Src.Indices.Add(0); Src.Indices.Add(1); Src.Indices.Add(2);
		}
		for (const float S : InstanceScales)
		{
			Src.Instances.Add(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(S)));
		}
		Src.LocalRadiusCm = FMath::Sqrt(3.0f);
		return Src;
	}
}

/** The census must count the same triangles ExpandSourcesToSections emits. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellCensusMatchesSelectionTest,
	"FragmentsUnreal.CellCensus.MatchesSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellCensusMatchesSelectionTest::RunTest(const FString& Parameters)
{
	// Radii: sqrt(3)*{1,10,100} = 1.73, 17.3, 173.2 cm. 12 triangles each.
	TArray<FMergeSource> Sources;
	Sources.Add(MakeSource(0, { 1.0f, 10.0f, 100.0f }, /*NumTris=*/ 12));

	const FragmentCellCensus::FCellCensus C =
		FragmentCellCensus::AccumulateCellCensus(Sources, /*NumMaterialSlots=*/ 1);

	TestEqual(TEXT("total tris = 3 instances x 12"), C.TotalTris, (int64)36);
	TestEqual(TEXT("instance count"), C.TotalInstances, 3);

	// Cutoffs chosen to sit on bucket boundaries so the suffix sum is exact.
	const float Cutoffs[] = { 0.0f, 8.0f, 128.0f, 4096.0f };
	for (const float Cutoff : Cutoffs)
	{
		const TArray<FragmentCellMerge::FExpandedSection> Expanded =
			FragmentCellMerge::ExpandSourcesToSections(Sources, /*NumMaterialSlots=*/ 1, Cutoff);

		int64 ActualTris = 0;
		for (const FragmentCellMerge::FExpandedSection& S : Expanded)
		{
			ActualTris += S.Geo.Indices.Num() / 3;
		}

		const int64 Predicted = FragmentCellCensus::SurvivingTrisAtCutoff(C, Cutoff);
		TestEqual(FString::Printf(TEXT("cutoff %.1fcm: census %lld == expand %lld"),
			Cutoff, Predicted, ActualTris), Predicted, ActualTris);
	}
	return true;
}

/** Suffix sums must never exceed the total, and must be monotonically non-increasing in cutoff. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellCensusMonotonicTest,
	"FragmentsUnreal.CellCensus.Monotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellCensusMonotonicTest::RunTest(const FString& Parameters)
{
	TArray<FMergeSource> Sources;
	Sources.Add(MakeSource(0, { 0.5f, 2.0f, 8.0f, 32.0f, 128.0f }, 10));
	Sources.Add(MakeSource(1, { 1.0f, 4.0f }, 200));

	const FragmentCellCensus::FCellCensus C =
		FragmentCellCensus::AccumulateCellCensus(Sources, /*NumMaterialSlots=*/ 2);

	TestEqual(TEXT("total = 5*10 + 2*200"), C.TotalTris, (int64)450);

	int64 Prev = C.TotalTris;
	for (int32 i = 0; i < FragmentCellCensus::kNumRadiusBuckets; ++i)
	{
		const int64 Surviving = FragmentCellCensus::SurvivingTrisAtCutoff(
			C, FragmentCellCensus::RadiusBucketLowerBoundCm(i));
		TestTrue(FString::Printf(TEXT("bucket %d: %lld <= %lld"), i, Surviving, Prev),
			Surviving <= Prev);
		Prev = Surviving;
	}

	// Template-triangle histogram prices MinSimplifyTris: the 200-tri template holds 400 tris, the 10-tri template holds 50.
	int64 AtOrAbove64 = 0;
	for (int32 i = 0; i < FragmentCellCensus::kNumTemplateTriBuckets; ++i)
	{
		if ((1 << i) >= 64) { AtOrAbove64 += C.TemplateTriBucketTris[i]; }
	}
	TestEqual(TEXT("tris in templates >= 64 tris"), AtOrAbove64, (int64)400);
	return true;
}

/** MergeCensus must be associative over buckets so per-cell results roll up to per-model. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellCensusMergeTest,
	"FragmentsUnreal.CellCensus.Merge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellCensusMergeTest::RunTest(const FString& Parameters)
{
	TArray<FMergeSource> A; A.Add(MakeSource(0, { 1.0f }, 10));
	TArray<FMergeSource> B; B.Add(MakeSource(0, { 100.0f }, 20));

	FragmentCellCensus::FCellCensus CA = FragmentCellCensus::AccumulateCellCensus(A, /*NumMaterialSlots=*/ 1);
	const FragmentCellCensus::FCellCensus CB = FragmentCellCensus::AccumulateCellCensus(B, /*NumMaterialSlots=*/ 1);
	FragmentCellCensus::MergeCensus(CA, CB);

	TestEqual(TEXT("merged total"), CA.TotalTris, (int64)30);
	TestEqual(TEXT("merged instances"), CA.TotalInstances, 2);
	TestEqual(TEXT("cutoff 0 keeps all"),
		FragmentCellCensus::SurvivingTrisAtCutoff(CA, 0.0f), (int64)30);
	return true;
}

/** A source whose MaterialSlot is out of range is never built by ExpandSourcesToSections. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellCensusRejectsOutOfRangeSlotTest,
	"FragmentsUnreal.CellCensus.RejectsOutOfRangeSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellCensusRejectsOutOfRangeSlotTest::RunTest(const FString& Parameters)
{
	TArray<FMergeSource> Sources;
	Sources.Add(MakeSource(/*MaterialSlot=*/ 5, { 1.0f }, 10));

	const FragmentCellCensus::FCellCensus C =
		FragmentCellCensus::AccumulateCellCensus(Sources, /*NumMaterialSlots=*/ 1);
	TestEqual(TEXT("out-of-range slot contributes zero census tris"), C.TotalTris, (int64)0);

	const TArray<FragmentCellMerge::FExpandedSection> Expanded = FragmentCellMerge::ExpandSourcesToSections(
		Sources, /*NumMaterialSlots=*/ 1, /*MinRadiusCm=*/ 0.0f);
	int64 ActualTris = 0;
	for (const FragmentCellMerge::FExpandedSection& S : Expanded)
	{
		ActualTris += S.Geo.Indices.Num() / 3;
	}
	TestEqual(TEXT("out-of-range slot contributes zero expanded tris"), ActualTris, (int64)0);
	return true;
}

/** Pins the inclusive/exclusive bucket-boundary convention. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellCensusBoundaryTest,
	"FragmentsUnreal.CellCensus.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellCensusBoundaryTest::RunTest(const FString& Parameters)
{
	const int32 Bucket = 5;
	const float BoundaryRadius = FragmentCellCensus::RadiusBucketLowerBoundCm(Bucket);

	FMergeSource Src = MakeSource(0, { 1.0f }, 10);
	Src.LocalRadiusCm = BoundaryRadius;   // scale 1.0 -> world radius == BoundaryRadius exactly.

	TArray<FMergeSource> Sources; Sources.Add(Src);

	const FragmentCellCensus::FCellCensus C =
		FragmentCellCensus::AccumulateCellCensus(Sources, /*NumMaterialSlots=*/ 1);

	const float CutoffAt    = BoundaryRadius;
	const float CutoffBelow = FragmentCellCensus::RadiusBucketLowerBoundCm(Bucket - 1);
	const float CutoffAbove = FragmentCellCensus::RadiusBucketLowerBoundCm(Bucket + 1);

	for (const float Cutoff : { CutoffAt, CutoffBelow, CutoffAbove })
	{
		const TArray<FragmentCellMerge::FExpandedSection> Expanded =
			FragmentCellMerge::ExpandSourcesToSections(Sources, /*NumMaterialSlots=*/ 1, Cutoff);

		int64 ActualTris = 0;
		for (const FragmentCellMerge::FExpandedSection& S : Expanded)
		{
			ActualTris += S.Geo.Indices.Num() / 3;
		}

		const int64 Predicted = FragmentCellCensus::SurvivingTrisAtCutoff(C, Cutoff);
		TestEqual(FString::Printf(TEXT("cutoff %.4fcm at boundary: census %lld == expand %lld"),
			Cutoff, Predicted, ActualTris), Predicted, ActualTris);
	}
	return true;
}

/** The budget solver's most important behaviour is REFUSING impossible budgets loudly. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellBudgetSolverTest,
	"FragmentsUnreal.CellCensus.BudgetSolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellBudgetSolverTest::RunTest(const FString& Parameters)
{
	TArray<FMergeSource> Sources;
	Sources.Add(MakeSource(0, { 0.5f, 2.0f, 8.0f, 32.0f, 128.0f }, 100));
	const FragmentCellCensus::FCellCensus C =
		FragmentCellCensus::AccumulateCellCensus(Sources, /*NumMaterialSlots=*/ 1);

	const double BytesPerTri = 100.0;
	const double FloorMB     = 10.0;

	// Unreachable: below the floor alone.
	{
		const FragmentCellCensus::FBudgetResult R =
			FragmentCellCensus::SolveCutoffForBudget(C, /*BudgetMB=*/ 5.0, BytesPerTri, FloorMB);
		TestFalse(TEXT("a budget below the floor is not reachable"), R.bReachable);
		TestEqual(TEXT("floor reported back to the caller"), R.FloorMB, FloorMB);
	}

	// Trivially reachable: everything fits, so no elements are omitted.
	{
		const FragmentCellCensus::FBudgetResult R =
			FragmentCellCensus::SolveCutoffForBudget(C, /*BudgetMB=*/ 1.0e6, BytesPerTri, FloorMB);
		TestTrue(TEXT("a huge budget is reachable"), R.bReachable);
		TestEqual(TEXT("no cutoff needed"), R.CutoffCm, 0.0f);
		TestEqual(TEXT("predicts every triangle"), R.PredictedTris, C.TotalTris);
	}

	// Binding: force a cutoff and verify the prediction actually lands under budget.
	{
		const double TargetMB = FloorMB +
			((double)C.TotalTris * BytesPerTri / (1024.0 * 1024.0)) * 0.5;

		const FragmentCellCensus::FBudgetResult R =
			FragmentCellCensus::SolveCutoffForBudget(C, TargetMB, BytesPerTri, FloorMB);

		TestTrue(TEXT("binding budget is reachable"), R.bReachable);
		TestTrue(FString::Printf(TEXT("cutoff %.2f is non-zero"), R.CutoffCm), R.CutoffCm > 0.0f);
		TestTrue(FString::Printf(TEXT("predicted %.3f MB <= budget %.3f MB"),
				R.PredictedMB, TargetMB),
			R.PredictedMB <= TargetMB);
		TestEqual(TEXT("prediction agrees with the suffix sum"),
			R.PredictedTris, FragmentCellCensus::SurvivingTrisAtCutoff(C, R.CutoffCm));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
