// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "Spatial/FragmentModelTier.h"
#include "Spatial/FragmentProxyTiers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Unit cube centred on the origin: local bounding-sphere radius = half the AABB diagonal of a 2x2x2 box = sqrt(3) ~= 1.732 cm. */
	FMergeSource MakeCubeSource(int32 MaterialSlot, TConstArrayView<float> InstanceScales)
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
		// Two triangles is enough — this test is about selection, not topology.
		Src.Indices = { 0, 1, 2, 0, 2, 3 };

		for (const float S : InstanceScales)
		{
			Src.Instances.Add(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(S)));
		}

		Src.LocalRadiusCm = FMath::Sqrt(3.0f);
		return Src;
	}

	int32 TotalTriangles(const TArray<FragmentCellMerge::FExpandedSection>& Sections)
	{
		int32 Tris = 0;
		for (const FragmentCellMerge::FExpandedSection& S : Sections) { Tris += S.Geo.Indices.Num() / 3; }
		return Tris;
	}
}

/** Selection is per INSTANCE, not per source. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeSelectionByRadiusTest,
	"FragmentsUnreal.CellMergeSelection.ByRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeSelectionByRadiusTest::RunTest(const FString& Parameters)
{
	// Radii: 1.732, 17.32, 173.2 cm.
	const TArray<FMergeSource> Sources = { MakeCubeSource(0, { 1.0f, 10.0f, 100.0f }) };

	// Cutoff 0 keeps everything.
	TestEqual(TEXT("zero cutoff keeps all three instances"),
		TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Sources, 1, 0.0f)), 6);

	// Cutoff between instance 0 and instance 1 drops exactly one.
	TestEqual(TEXT("cutoff 10cm drops the smallest instance only"),
		TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Sources, 1, 10.0f)), 4);

	// Cutoff above every instance drops the section entirely.
	TestEqual(TEXT("cutoff 1000cm drops everything"),
		TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Sources, 1, 1000.0f)), 0);

	// An element is never PARTIALLY emitted — that is the whole point of selection over decimation.
	for (const float Cut : { 0.0f, 5.0f, 50.0f, 500.0f })
	{
		const int32 Tris = TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Sources, 1, Cut));
		TestEqual(FString::Printf(TEXT("cutoff %.0f emits whole elements"), Cut), Tris % 2, 0);
	}

	return true;
}

/** Nesting: a larger cutoff must keep a strict subset. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeSubLodNestingTest,
	"FragmentsUnreal.CellMergeSelection.SubLodNesting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeSubLodNestingTest::RunTest(const FString& Parameters)
{
	FRandomStream Rng(20260821);

	for (int32 Trial = 0; Trial < 64; ++Trial)
	{
		TArray<float> Scales;
		for (int32 i = 0; i < 12; ++i) { Scales.Add(Rng.FRandRange(0.1f, 200.0f)); }
		const TArray<FMergeSource> Sources = { MakeCubeSource(0, Scales) };

		const float CutA = Rng.FRandRange(0.0f, 100.0f);
		const float CutB = CutA + Rng.FRandRange(0.0f, 200.0f);   // CutB >= CutA

		const int32 TrisA = TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Sources, 1, CutA));
		const int32 TrisB = TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Sources, 1, CutB));

		if (!TestTrue(
			FString::Printf(TEXT("trial %d: larger cutoff keeps a subset (%d <= %d)"), Trial, TrisB, TrisA),
			TrisB <= TrisA))
		{
			return false;
		}
	}

	return true;
}

/** Selection must not depend on the order sources arrive in. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeSelectionOrderIndependenceTest,
	"FragmentsUnreal.CellMergeSelection.OrderIndependence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeSelectionOrderIndependenceTest::RunTest(const FString& Parameters)
{
	const TArray<FMergeSource> Forward = {
		MakeCubeSource(0, { 1.0f, 50.0f }),
		MakeCubeSource(1, { 5.0f, 200.0f }),
	};
	TArray<FMergeSource> Reversed = { Forward[1], Forward[0] };

	const float Cut = 20.0f;
	TestEqual(TEXT("same surviving triangle count regardless of source order"),
		TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Forward,  2, Cut)),
		TotalTriangles(FragmentCellMerge::ExpandSourcesToSections(Reversed, 2, Cut)));

	return true;
}

/** A sub-LOD that drops every instance of one material contributes no section for that slot. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeDroppedSlotKeepsIndexTest,
	"FragmentsUnreal.CellMergeSelection.DroppedSlotKeepsIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeDroppedSlotKeepsIndexTest::RunTest(const FString& Parameters)
{
	// Slot 0 is tiny (will be dropped), slot 1 is large (survives), slot 2 is large (survives).
	const TArray<FMergeSource> Sources = {
		MakeCubeSource(0, { 1.0f }),
		MakeCubeSource(1, { 100.0f }),
		MakeCubeSource(2, { 100.0f }),
	};

	const TArray<FragmentCellMerge::FExpandedSection> Out =
		FragmentCellMerge::ExpandSourcesToSections(Sources, 3, /*MinRadiusCm=*/ 50.0f);

	TestEqual(TEXT("one section dropped, two survive"), Out.Num(), 2);
	TestEqual(TEXT("first survivor keeps slot 1, not renumbered to 0"), Out[0].MaterialSlot, 1);
	TestEqual(TEXT("second survivor keeps slot 2, not renumbered to 1"), Out[1].MaterialSlot, 2);

	return true;
}

/** Sub-LOD 1's near edge IS the model's demote distance. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellSubLodSpecDerivationTest,
	"FragmentsUnreal.CellMergeSelection.SubLodSpecDerivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellSubLodSpecDerivationTest::RunTest(const FString& Parameters)
{
	const float RefCoeff = FragmentProxyTiers::ViewCoeff(
		FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);

	// Shell near edge 1500cm; 50m-radius model; 15m-radius cell; 4px budget; spec defaults.
	const TArray<FCellSubLodSpec> Specs = UFragmentsImporter::BuildCellSubLodSpecs(
		/*NearEdgeCm=*/ 1500.0f, /*ModelBoundsRadiusCm=*/ 5000.0f, /*CellBoundsRadiusCm=*/ 1500.0f,
		/*OmitPx=*/ 4.0f, /*GeoCoeffCells=*/ 0.033f, /*ThresholdPx=*/ 16.0f, /*CoarsenFactor=*/ 0.75f);

	TestEqual(TEXT("exactly two sub-LODs"), Specs.Num(), 2);

	// Sub-LOD 0: 4 * 1500 / 960.12 = 6.25cm. Keeps essentially everything, which is CORRECT at 15m.
	TestTrue(FString::Printf(TEXT("sub-LOD 0 cutoff ~6.25cm (got %.2f)"), Specs[0].MinRadiusCm),
		FMath::IsNearlyEqual(Specs[0].MinRadiusCm, 6.25f, 0.1f));

	// Sub-LOD 1: near edge is the demote distance, 13201.6cm -> 4 * 13201.6 / 960.12 = 55.0cm.
	const float Demote = FragmentModelTier::DemoteDistanceCm(0.033f, 5000.0f, RefCoeff, 16.0f, 0.75f);
	const float Expected = FragmentProxyTiers::RadiusCutoffCm(4.0f, Demote, RefCoeff);
	TestTrue(FString::Printf(TEXT("sub-LOD 1 cutoff derives from demote distance (%.2f vs %.2f)"),
			Specs[1].MinRadiusCm, Expected),
		FMath::IsNearlyEqual(Specs[1].MinRadiusCm, Expected, 0.1f));
	TestTrue(FString::Printf(TEXT("sub-LOD 1 cutoff ~55cm (got %.2f)"), Specs[1].MinRadiusCm),
		FMath::IsNearlyEqual(Specs[1].MinRadiusCm, 55.0f, 1.0f));

	// Nesting: cutoffs non-decreasing, so sub-LOD 1 is a strict subset of sub-LOD 0.
	TestTrue(TEXT("cutoffs are non-decreasing"), Specs[1].MinRadiusCm >= Specs[0].MinRadiusCm);

	// ScreenSize must be strictly decreasing or UE cannot distinguish the two LODs.
	TestTrue(FString::Printf(TEXT("screen sizes strictly decreasing (%.4f > %.4f)"),
			Specs[0].ScreenSize, Specs[1].ScreenSize),
		Specs[0].ScreenSize > Specs[1].ScreenSize);
	TestTrue(TEXT("screen sizes within [0,1]"),
		Specs[0].ScreenSize <= 1.0f && Specs[1].ScreenSize > 0.0f);

	// Degenerate model radius must not produce a sub-LOD 1 cutoff below sub-LOD 0's.
	const TArray<FCellSubLodSpec> Degenerate = UFragmentsImporter::BuildCellSubLodSpecs(
		1500.0f, 0.0f, 1500.0f, 4.0f, 0.033f, 16.0f, 0.75f);
	TestTrue(TEXT("degenerate model radius still nests"),
		Degenerate.Num() == 2 && Degenerate[1].MinRadiusCm >= Degenerate[0].MinRadiusCm);

	return true;
}

/** The interior cutoff is NOT mis-derived. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInteriorOmitPxTest,
	"FragmentsUnreal.CellMergeSelection.InteriorOmitPx",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInteriorOmitPxTest::RunTest(const FString& Parameters)
{
	// Baseline: interior near edge 1000cm at the shared 4px budget -> 4.17cm, MORE permissive than the shell's 4*1500/960.1 = 6.25cm.
	const TArray<FCellSubLodSpec> InteriorAt4 = UFragmentsImporter::BuildCellSubLodSpecs(
		1000.0f, 5000.0f, 1500.0f, 4.0f, 0.033f, 16.0f, 0.75f);
	const TArray<FCellSubLodSpec> ShellAt4 = UFragmentsImporter::BuildCellSubLodSpecs(
		1500.0f, 5000.0f, 1500.0f, 4.0f, 0.033f, 16.0f, 0.75f);

	TestTrue(FString::Printf(TEXT("interior cutoff %.2f < shell %.2f at a shared budget"),
			InteriorAt4[0].MinRadiusCm, ShellAt4[0].MinRadiusCm),
		InteriorAt4[0].MinRadiusCm < ShellAt4[0].MinRadiusCm);

	// A larger interior budget raises the interior cutoff past the shell's, which is the point.
	const TArray<FCellSubLodSpec> InteriorAt12 = UFragmentsImporter::BuildCellSubLodSpecs(
		1000.0f, 5000.0f, 1500.0f, 12.0f, 0.033f, 16.0f, 0.75f);

	TestTrue(FString::Printf(TEXT("interior at 12px (%.2f) exceeds shell at 4px (%.2f)"),
			InteriorAt12[0].MinRadiusCm, ShellAt4[0].MinRadiusCm),
		InteriorAt12[0].MinRadiusCm > ShellAt4[0].MinRadiusCm);

	// Exact value: 12 * 1000 / 960.12 = 12.50cm.
	TestTrue(FString::Printf(TEXT("interior at 12px ~12.50cm (got %.2f)"),
			InteriorAt12[0].MinRadiusCm),
		FMath::IsNearlyEqual(InteriorAt12[0].MinRadiusCm, 12.50f, 0.1f));

	// Nesting must survive at every budget, including inverted and degenerate ones.
	const float Budgets[] = { 0.0f, 1.0f, 4.0f, 12.0f, 64.0f };
	for (const float Px : Budgets)
	{
		const TArray<FCellSubLodSpec> S = UFragmentsImporter::BuildCellSubLodSpecs(
			1000.0f, 5000.0f, 1500.0f, Px, 0.033f, 16.0f, 0.75f);
		TestTrue(FString::Printf(TEXT("nesting holds at %.0fpx"), Px),
			S.Num() == 2 && S[1].MinRadiusCm >= S[0].MinRadiusCm);
	}

	// Pure overload: all four cases, no CVar involved.
	TestEqual(TEXT("interior takes its own budget"),
		UFragmentsImporter::ResolveCellOmitPx(/*bShellSide=*/ false, /*SharedOmitPx=*/ 4.0f, /*InteriorOmitPx=*/ 32.0f),
		32.0f);
	TestEqual(TEXT("shell ignores the interior budget"),
		UFragmentsImporter::ResolveCellOmitPx(/*bShellSide=*/ true, /*SharedOmitPx=*/ 4.0f, /*InteriorOmitPx=*/ 32.0f),
		4.0f);
	TestEqual(TEXT("sentinel falls back to shared (interior)"),
		UFragmentsImporter::ResolveCellOmitPx(/*bShellSide=*/ false, /*SharedOmitPx=*/ 4.0f, /*InteriorOmitPx=*/ -1.0f),
		4.0f);
	TestEqual(TEXT("shell always uses shared"),
		UFragmentsImporter::ResolveCellOmitPx(/*bShellSide=*/ true, /*SharedOmitPx=*/ 4.0f, /*InteriorOmitPx=*/ -1.0f),
		4.0f);

	// CVar-reading overload: shipped defaults now differ between shell and interior.
	TestTrue(TEXT("shipped defaults differ between shell and interior"),
		UFragmentsImporter::ResolveCellOmitPx(/*bShellSide=*/ false) != UFragmentsImporter::ResolveCellOmitPx(/*bShellSide=*/ true));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
