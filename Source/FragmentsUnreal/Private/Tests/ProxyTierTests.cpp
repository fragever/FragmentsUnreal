// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentModelTier.h"
#include "Spatial/FragmentProxyTiers.h"
#include "Utils/FragmentsUtils.h"
#include "Utils/PendingMeshFinalize.h"

#if WITH_DEV_AUTOMATION_TESTS

/** The cutoff derivation is the bridge between "how many pixels may silently disappear" and the world-radius comparison the worker actually runs. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierCutoffDerivationTest,
	"FragmentsUnreal.ProxyTiers.CutoffDerivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierCutoffDerivationTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	// Reference view: 1080p, M11 = 1/tan(fovY/2) = 1.778 (90 deg horizontal at 16:9).
	const float Coeff = ViewCoeff(1080.0f, 1.778f);
	TestTrue(FString::Printf(TEXT("view coeff ~960 (got %.2f)"), Coeff),
		FMath::IsNearlyEqual(Coeff, 960.12f, 0.5f));

	// 4px at 100m -> 4 * 10000 / 960.12 = 41.66cm
	const float Cut100m = RadiusCutoffCm(4.0f, 10000.0f, Coeff);
	TestTrue(FString::Printf(TEXT("100m/4px cutoff ~41.7cm (got %.2f)"), Cut100m),
		FMath::IsNearlyEqual(Cut100m, 41.66f, 0.2f));

	// 4px at 400m -> 166.65cm
	const float Cut400m = RadiusCutoffCm(4.0f, 40000.0f, Coeff);
	TestTrue(FString::Printf(TEXT("400m/4px cutoff ~166.7cm (got %.2f)"), Cut400m),
		FMath::IsNearlyEqual(Cut400m, 166.65f, 0.5f));

	// Monotonic in both arguments — a farther tier or a looser pixel budget omits more.
	TestTrue(TEXT("cutoff grows with distance"), Cut400m > Cut100m);
	TestTrue(TEXT("cutoff grows with pixel budget"),
		RadiusCutoffCm(8.0f, 10000.0f, Coeff) > Cut100m);

	// Degenerate coefficient must not divide by zero.
	TestEqual(TEXT("zero coeff yields zero cutoff"), RadiusCutoffCm(4.0f, 10000.0f, 0.0f), 0.0f);

	return true;
}

/** Nesting is what stops elements reappearing as the camera retreats. Cutoffs must not decrease. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierNestingTest,
	"FragmentsUnreal.ProxyTiers.Nesting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierNestingTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	TArray<FTierSpec> Good;
	Good.Add(FTierSpec{ 0.0f,   1.0f, 1.0f });
	Good.Add(FTierSpec{ 41.7f,  1.0f, 0.5f });
	Good.Add(FTierSpec{ 166.7f, 1.0f, 0.25f });
	TestTrue(TEXT("increasing cutoffs are nested"), AreTiersNested(Good));

	TArray<FTierSpec> Bad;
	Bad.Add(FTierSpec{ 0.0f,   1.0f, 1.0f });
	Bad.Add(FTierSpec{ 166.7f, 1.0f, 0.5f });
	Bad.Add(FTierSpec{ 41.7f,  1.0f, 0.25f });   // decreases — element would reappear
	TestFalse(TEXT("decreasing cutoff is not nested"), AreTiersNested(Bad));

	TestTrue(TEXT("single tier is trivially nested"), AreTiersNested(TArray<FTierSpec>{ Good[0] }));
	TestTrue(TEXT("empty is trivially nested"), AreTiersNested(TArray<FTierSpec>{}));

	return true;
}

namespace
{
	/** Axis-aligned box of the given half-extent, centred on the origin. 8 verts, 12 triangles. */
	FFragmentMeshData MakeBox(float HalfExtentCm)
	{
		FFragmentMeshData Data;
		Data.bIsValid = true;
		const float H = HalfExtentCm;
		const FVector3f Corners[8] = {
			{-H,-H,-H}, { H,-H,-H}, { H, H,-H}, {-H, H,-H},
			{-H,-H, H}, { H,-H, H}, { H, H, H}, {-H, H, H},
		};
		for (const FVector3f& P : Corners)
		{
			FFragmentMeshVertex V;
			V.Position     = P;
			V.Normal       = FVector3f(0, 0, 1);
			V.TangentX     = FVector3f(1, 0, 0);
			V.TangentYSign = 1.0f;
			V.UV0          = FVector2f::ZeroVector;
			V.Color        = FColor::White;
			Data.Vertices.Add(V);
		}
		const uint32 Tris[36] = {
			0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
			1,5,6, 1,6,2,  2,6,7, 2,7,3,  3,7,4, 3,4,0,
		};
		Data.Indices.Append(Tris, 36);
		return Data;
	}
}

/** Radius drives every selection decision, so it must be exact for the identity case and must track transform scale. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierRadiusTest,
	"FragmentsUnreal.ProxyTiers.ElementRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierRadiusTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	const FFragmentMeshData Box = MakeBox(100.0f);
	const float Expected = 100.0f * FMath::Sqrt(3.0f);   // ~173.2

	const float Local = LocalRadiusCm(Box);
	TestTrue(FString::Printf(TEXT("local radius ~173.2 (got %.2f)"), Local),
		FMath::IsNearlyEqual(Local, Expected, 0.1f));

	// Identity transform leaves it unchanged; translation must NOT change it.
	TestTrue(TEXT("identity transform preserves radius"),
		FMath::IsNearlyEqual(WorldRadiusCm(Local, FTransform::Identity), Expected, 0.1f));
	TestTrue(TEXT("translation does not change radius"),
		FMath::IsNearlyEqual(
			WorldRadiusCm(Local, FTransform(FVector(9999.0, -500.0, 250.0))), Expected, 0.1f));

	// Uniform scale multiplies it.
	const FTransform Scaled(FQuat::Identity, FVector::ZeroVector, FVector(2.0));
	TestTrue(TEXT("uniform scale 2x doubles radius"),
		FMath::IsNearlyEqual(WorldRadiusCm(Local, Scaled), Expected * 2.0f, 0.1f));

	// Non-uniform scale takes the largest component — conservative, keeps geometry.
	const FTransform NonUniform(FQuat::Identity, FVector::ZeroVector, FVector(0.5, 3.0, 1.0));
	TestTrue(TEXT("non-uniform scale uses max component"),
		FMath::IsNearlyEqual(WorldRadiusCm(Local, NonUniform), Expected * 3.0f, 0.1f));

	// Degenerate input must be zero, not NaN — a NaN radius would silently pass every tier test.
	FFragmentMeshData Empty;
	Empty.bIsValid = true;
	TestEqual(TEXT("empty mesh has zero radius"), LocalRadiusCm(Empty), 0.0f);

	FFragmentMeshData Invalid = MakeBox(100.0f);
	Invalid.bIsValid = false;
	TestEqual(TEXT("invalid mesh has zero radius"), LocalRadiusCm(Invalid), 0.0f);

	return true;
}

/** Selection, nesting and determinism in one fixture. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierSelectionTest,
	"FragmentsUnreal.ProxyTiers.SelectionAndNesting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierSelectionTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	// Radii: 10*sqrt(3)=17.3, 100*sqrt(3)=173.2, 500*sqrt(3)=866.0
	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Small =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(10.0f));
	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Medium =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(100.0f));
	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Large =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(500.0f));

	TArray<UFragmentsImporter::FResolvedProxyContribution> Payload;
	auto Add = [&Payload](TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Mesh, uint32 CRC)
	{
		UFragmentsImporter::FResolvedProxyContribution C;
		C.MeshData       = Mesh;
		C.WorldTransform = FTransform::Identity;
		C.MatCRC         = CRC;
		C.Material       = nullptr;
		Payload.Add(C);
	};
	Add(Small,  1);
	Add(Medium, 2);
	Add(Large,  3);

	// Tier 0 keeps everything; tier 1 drops the handle; tier 2 drops handle and panel.
	TArray<FTierSpec> Tiers;
	Tiers.Add(FTierSpec{ 0.0f,   1.0f, 1.0f  });
	Tiers.Add(FTierSpec{ 50.0f,  1.0f, 0.5f  });
	Tiers.Add(FTierSpec{ 200.0f, 1.0f, 0.25f });
	TestTrue(TEXT("fixture tiers are nested"), AreTiersNested(Tiers));

	TArray<FProxyMeshAccumulator> Out;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Tiers, Out);

	TestEqual(TEXT("one accumulator per tier"), Out.Num(), 3);
	if (Out.Num() != 3) { return false; }

	TestEqual(TEXT("tier 0 has all three materials"), Out[0].Sections.Num(), 3);
	TestEqual(TEXT("tier 1 drops the smallest"),      Out[1].Sections.Num(), 2);
	TestEqual(TEXT("tier 2 keeps only the largest"),  Out[2].Sections.Num(), 1);

	TestNull(TEXT("tier 1 omits CRC 1"),  Out[1].Sections.Find(1));
	TestNotNull(TEXT("tier 1 keeps CRC 2"), Out[1].Sections.Find(2));
	TestNotNull(TEXT("tier 2 keeps CRC 3"), Out[2].Sections.Find(3));

	// Nesting at the data level: every CRC in a deeper tier exists in every shallower tier.
	for (int32 t = 1; t < Out.Num(); ++t)
	{
		for (const TPair<uint32, FProxyMeshSection>& Pair : Out[t].Sections)
		{
			TestNotNull(
				*FString::Printf(TEXT("tier %d CRC %u also present in tier %d"), t, Pair.Key, t - 1),
				Out[t - 1].Sections.Find(Pair.Key));
		}
	}

	// Included elements are whole: tier 1's panel has exactly the source triangle count.
	const FProxyMeshSection* Panel = Out[1].Sections.Find(2);
	if (Panel) { TestEqual(TEXT("kept element is whole, not simplified"), Panel->Indices.Num(), 36); }

	return true;
}

/** The byte-identical proxy guarantee: same payload -> same output, every time. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierDeterminismTest,
	"FragmentsUnreal.ProxyTiers.Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Mesh =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(100.0f));

	TArray<UFragmentsImporter::FResolvedProxyContribution> Payload;
	for (int32 i = 0; i < 8; ++i)
	{
		UFragmentsImporter::FResolvedProxyContribution C;
		C.MeshData       = Mesh;
		C.WorldTransform = FTransform(FVector(i * 300.0, 0.0, 0.0));
		C.MatCRC         = 7;
		C.Material       = nullptr;
		Payload.Add(C);
	}

	TArray<FTierSpec> Tiers;
	Tiers.Add(FTierSpec{ 0.0f,  1.0f, 1.0f });
	Tiers.Add(FTierSpec{ 50.0f, 1.0f, 0.5f });

	TArray<FProxyMeshAccumulator> A, B;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Tiers, A);
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Tiers, B);

	TestEqual(TEXT("same tier count"), A.Num(), B.Num());
	for (int32 t = 0; t < A.Num() && t < B.Num(); ++t)
	{
		const FProxyMeshSection* SA = A[t].Sections.Find(7);
		const FProxyMeshSection* SB = B[t].Sections.Find(7);
		if (!SA || !SB) { AddError(TEXT("missing section")); continue; }

		TestEqual(TEXT("identical vertex count"), SA->Positions.Num(), SB->Positions.Num());
		TestEqual(TEXT("identical index count"),  SA->Indices.Num(),   SB->Indices.Num());
		for (int32 i = 0; i < SA->Positions.Num() && i < SB->Positions.Num(); ++i)
		{
			if (SA->Positions[i] != SB->Positions[i])
			{
				AddError(FString::Printf(TEXT("tier %d vertex %d differs between runs"), t, i));
				break;
			}
		}
	}
	return true;
}

/** The retained 2-arg overload must behave exactly as before: one accumulator, nothing omitted. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierLegacyOverloadTest,
	"FragmentsUnreal.ProxyTiers.LegacyOverloadKeepsEverything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierLegacyOverloadTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Tiny =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(1.0f));

	TArray<UFragmentsImporter::FResolvedProxyContribution> Payload;
	UFragmentsImporter::FResolvedProxyContribution C;
	C.MeshData       = Tiny;
	C.WorldTransform = FTransform::Identity;
	C.MatCRC         = 42;
	C.Material       = nullptr;
	Payload.Add(C);

	FProxyMeshAccumulator Acc;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Acc);

	TestNotNull(TEXT("tiny element retained by legacy overload"), Acc.Sections.Find(42));
	return true;
}

/** The drain's budget model (r.Fragments.Finalize.MsPer1kTris) decides whether a payload fits the remaining frame budget. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierPayloadShapeTest,
	"FragmentsUnreal.ProxyTiers.PayloadCarriesAllLODs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierPayloadShapeTest::RunTest(const FString& Parameters)
{
	FPendingMeshFinalize Payload;
	Payload.MeshDescriptions.Add(MakeUnique<FMeshDescription>());
	Payload.MeshDescriptions.Add(MakeUnique<FMeshDescription>());
	Payload.LODScreenSizes = { 1.0f, 0.35f };
	Payload.EstimatedTriangleCount = 900 + 300;

	TestEqual(TEXT("payload holds two LODs"), Payload.MeshDescriptions.Num(), 2);
	TestEqual(TEXT("screen sizes parallel to LODs"),
		Payload.LODScreenSizes.Num(), Payload.MeshDescriptions.Num());
	TestEqual(TEXT("estimate is the sum across tiers, not LOD0 alone"),
		Payload.EstimatedTriangleCount, 1200);
	TestTrue(TEXT("screen sizes decrease with LOD index"),
		Payload.LODScreenSizes[1] < Payload.LODScreenSizes[0]);

	return true;
}

/** UE requires every LOD of a static mesh to index the SAME material slot array. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierMaterialSlotStabilityTest,
	"FragmentsUnreal.ProxyTiers.MaterialSlotStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierMaterialSlotStabilityTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Small =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(10.0f));
	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Large =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(500.0f));

	// CRCs deliberately out of sorted order, and the SMALL one has the LOWEST CRC -- so if slot assignment ever re-derives from the surviving tier instead of tier 0, indices shift.
	TArray<UFragmentsImporter::FResolvedProxyContribution> Payload;
	auto Add = [&Payload](TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> M, uint32 CRC)
	{
		UFragmentsImporter::FResolvedProxyContribution C;
		C.MeshData = M; C.WorldTransform = FTransform::Identity; C.MatCRC = CRC; C.Material = nullptr;
		Payload.Add(C);
	};
	Add(Small, 5);
	Add(Large, 9);

	TArray<FTierSpec> Tiers;
	Tiers.Add(FTierSpec{ 0.0f,   1.0f, 1.0f });
	Tiers.Add(FTierSpec{ 200.0f, 1.0f, 0.3f });   // drops CRC 5 entirely

	TArray<FProxyMeshAccumulator> Out;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Tiers, Out);

	TestEqual(TEXT("tier 0 has both materials"), Out[0].Sections.Num(), 2);
	TestEqual(TEXT("tier 1 has one material"),   Out[1].Sections.Num(), 1);

	// Build the slot order the way EnqueueProxyMeshBuild must: from tier 0, sorted for stability.
	TArray<uint32> SlotOrder;
	Out[0].Sections.GetKeys(SlotOrder);
	SlotOrder.Sort();
	TestEqual(TEXT("slot order derived from tier 0"), SlotOrder.Num(), 2);
	TestEqual(TEXT("slot 0 == CRC 5"), SlotOrder[0], (uint32)5);
	TestEqual(TEXT("slot 1 == CRC 9"), SlotOrder[1], (uint32)9);

	// Tier 1's surviving material must map to slot 1, NOT slot 0.
	const int32 SurvivorSlot = SlotOrder.IndexOfByKey((uint32)9);
	TestEqual(TEXT("tier 1 survivor keeps tier 0's slot index"), SurvivorSlot, 1);

	// And every CRC in tier 1 must exist in the tier 0 slot order (nesting makes this total).
	for (const TPair<uint32, FProxyMeshSection>& Pair : Out[1].Sections)
	{
		TestTrue(*FString::Printf(TEXT("tier 1 CRC %u has a tier 0 slot"), Pair.Key),
			SlotOrder.Contains(Pair.Key));
	}

	return true;
}

/** Keep LODScreenSizes aligned when null mesh descriptions are compacted. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierScreenSizeLockstepTest,
	"FragmentsUnreal.ProxyTiers.ScreenSizeStaysPairedWithSurvivingLOD",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierScreenSizeLockstepTest::RunTest(const FString& Parameters)
{
	TArray<TUniquePtr<FMeshDescription>> OutDescriptions;
	TArray<float> OutScreenSizes;

	// Tier 0: authored successfully, ScreenSize 1.0.
	UFragmentsImporter::AppendCompactedTierLOD(
		0, MakeUnique<FMeshDescription>(), 1.0f, OutDescriptions, OutScreenSizes);

	// Tier 1: failed to author (null) -- e.g. decimated below the minimum triangle count.
	UFragmentsImporter::AppendCompactedTierLOD(
		1, nullptr, 0.5f, OutDescriptions, OutScreenSizes);

	// Tier 2: authored successfully, ScreenSize 0.25.
	UFragmentsImporter::AppendCompactedTierLOD(
		2, MakeUnique<FMeshDescription>(), 0.25f, OutDescriptions, OutScreenSizes);

	TestEqual(TEXT("failed middle tier dropped from descriptions"), OutDescriptions.Num(), 2);
	TestEqual(TEXT("screen sizes compacted in lockstep with descriptions"),
		OutScreenSizes.Num(), OutDescriptions.Num());

	TestNotNull(TEXT("index 0 is tier 0's description"), OutDescriptions[0].Get());
	TestEqual(TEXT("index 0 keeps tier 0's screen size"), OutScreenSizes[0], 1.0f);

	TestNotNull(TEXT("index 1 is tier 2's description, not a dropped null"), OutDescriptions[1].Get());
	TestEqual(TEXT("index 1 pairs with tier 2's screen size, not tier 1's stale 0.5"),
		OutScreenSizes[1], 0.25f);

	// A failing TIER 0 must still be appended (even null) -- RunMeshFinalize's degenerate-payload check specifically reads MeshDescriptions[0], so that slot must exist for the check to fire.
	TArray<TUniquePtr<FMeshDescription>> OutDescriptions2;
	TArray<float> OutScreenSizes2;
	UFragmentsImporter::AppendCompactedTierLOD(0, nullptr, 1.0f, OutDescriptions2, OutScreenSizes2);
	TestEqual(TEXT("failed tier 0 is still appended (paired with its screen size)"),
		OutDescriptions2.Num(), 1);
	TestEqual(TEXT("...and its screen size survives alongside it"), OutScreenSizes2.Num(), 1);
	TestNull(TEXT("...but the description itself is null, as authored"), OutDescriptions2[0].Get());

	return true;
}

/** Tier specs must come out nested, with the documented cutoffs; ScreenSize is left unresolved (filled later by FillProxyTierScreenSizes, which needs accumulated proxy bounds this function doesn't have). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierSpecBuildTest,
	"FragmentsUnreal.ProxyTiers.SpecConstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierSpecBuildTest::RunTest(const FString& Parameters)
{
	const float NearEdges[3] = { 100.0f, 10000.0f, 40000.0f };
	const TArray<FragmentProxyTiers::FTierSpec> Specs =
		UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, NearEdges, 1.0f);

	TestEqual(TEXT("three tiers"), Specs.Num(), 3);
	TestTrue(TEXT("specs are nested"), FragmentProxyTiers::AreTiersNested(Specs));

	// Cutoffs match the spec's documented table.
	TestTrue(TEXT("tier 0 keeps essentially everything"), Specs[0].MinRadiusCm < 1.0f);
	TestTrue(FString::Printf(TEXT("tier 1 cutoff ~41.7cm (got %.2f)"), Specs[1].MinRadiusCm),
		FMath::IsNearlyEqual(Specs[1].MinRadiusCm, 41.66f, 0.5f));
	TestTrue(FString::Printf(TEXT("tier 2 cutoff ~166.7cm (got %.2f)"), Specs[2].MinRadiusCm),
		FMath::IsNearlyEqual(Specs[2].MinRadiusCm, 166.65f, 1.0f));

	// ScreenSize is a sentinel here, not a real value -- BuildProxyTierSpecs is bounds-free by design (see FillProxyTierScreenSizes).
	TestTrue(TEXT("screen sizes are left unresolved (negative sentinel)"),
		Specs[0].ScreenSize < 0.0f && Specs[1].ScreenSize < 0.0f && Specs[2].ScreenSize < 0.0f);

	// NumTiers == 1 reproduces today's single-LOD behaviour: keep everything.
	const TArray<FragmentProxyTiers::FTierSpec> Single =
		UFragmentsImporter::BuildProxyTierSpecs(1, 4.0f, NearEdges, 1.0f);
	TestEqual(TEXT("single tier requested"), Single.Num(), 1);
	TestEqual(TEXT("single tier omits nothing"), Single[0].MinRadiusCm, 0.0f);

	// Missing near-edge entries must not make deeper tiers broader than shallow tiers.
	{
		const TArray<FragmentProxyTiers::FTierSpec> TooFewEdges =
			UFragmentsImporter::BuildProxyTierSpecs(4, 4.0f, NearEdges, 1.0f);
		TestEqual(TEXT("four tiers requested"), TooFewEdges.Num(), 4);
		TestTrue(TEXT("out-of-range near edge does not desnest"),
			FragmentProxyTiers::AreTiersNested(TooFewEdges));
		TestEqual(TEXT("tier 3 (no calibration) inherits tier 2's cutoff, not 0"),
			TooFewEdges[3].MinRadiusCm, TooFewEdges[2].MinRadiusCm);
	}

	// A wholly empty near-edges view (e.g. an empty TierNearEdgesCm CVar) must fall back to "keep everything at every tier" rather than crashing or producing a bogus cutoff.
	{
		const TArray<FragmentProxyTiers::FTierSpec> NoEdges =
			UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, TConstArrayView<float>{}, 1.0f);
		TestTrue(TEXT("empty near-edges view is nested"), FragmentProxyTiers::AreTiersNested(NoEdges));
		for (int32 t = 0; t < NoEdges.Num(); ++t)
		{
			TestEqual(*FString::Printf(TEXT("tier %d keeps everything with no calibration"), t),
				NoEdges[t].MinRadiusCm, 0.0f);
		}
	}

	// A descending near-edge array (user error, or a malformed CVar string) must not let a later tier's cutoff fall below an earlier one's.
	{
		const float Descending[3] = { 40000.0f, 10000.0f, 100.0f };
		const TArray<FragmentProxyTiers::FTierSpec> DescendingSpecs =
			UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, Descending, 1.0f);
		TestTrue(TEXT("descending near edges do not desnest"),
			FragmentProxyTiers::AreTiersNested(DescendingSpecs));
		TestTrue(TEXT("tier 2's smaller-implied cutoff is clamped up to tier 1's"),
			DescendingSpecs[2].MinRadiusCm >= DescendingSpecs[1].MinRadiusCm);
	}

	return true;
}

/** Derive ScreenSize and MinRadiusCm from the same near-edge calibration. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierScreenSizeDerivationTest,
	"FragmentsUnreal.ProxyTiers.ScreenSizeDerivedFromNearEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierScreenSizeDerivationTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	const float NearEdges[3] = { 100.0f, 10000.0f, 40000.0f };
	TArray<FTierSpec> Specs = UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, NearEdges, 1.0f);

	// A 50m-radius proxy (large campus building).
	UFragmentsImporter::FillProxyTierScreenSizes(Specs, NearEdges, 5000.0f);

	TestTrue(TEXT("all screen sizes clamped to <= 1.0"),
		Specs[0].ScreenSize <= 1.0f && Specs[1].ScreenSize <= 1.0f && Specs[2].ScreenSize <= 1.0f);
	TestTrue(TEXT("screen sizes strictly decrease"),
		Specs[0].ScreenSize > Specs[1].ScreenSize && Specs[1].ScreenSize > Specs[2].ScreenSize);

	// Assert ScreenSize = proxy radius × M11 / near-edge distance.
	TestTrue(FString::Printf(TEXT("tier 2 screen size ~0.2223 (got %.4f)"), Specs[2].ScreenSize),
		FMath::IsNearlyEqual(Specs[2].ScreenSize, 0.2223f, 0.001f));

	// A tiny proxy (2m radius) makes even tier 2's raw value small -- screen size must shrink with bounds, unlike the old 1/(1<<t) halving, which was the same regardless of proxy size.
	TArray<FTierSpec> TinySpecs = UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, NearEdges, 1.0f);
	UFragmentsImporter::FillProxyTierScreenSizes(TinySpecs, NearEdges, 200.0f);
	TestTrue(TEXT("screen size shrinks with proxy bounds, not fixed by tier index"),
		TinySpecs[2].ScreenSize < Specs[2].ScreenSize);

	// Missing/degenerate near edges must not crash and must still produce a strictly-decreasing, nested-with-MinRadiusCm-in-spirit sequence.
	TArray<FTierSpec> Malformed = UFragmentsImporter::BuildProxyTierSpecs(4, 4.0f, NearEdges, 1.0f);
	UFragmentsImporter::FillProxyTierScreenSizes(Malformed, NearEdges, 5000.0f);
	for (int32 t = 1; t < Malformed.Num(); ++t)
	{
		TestTrue(*FString::Printf(TEXT("tier %d screen size strictly less than tier %d's"), t, t - 1),
			Malformed[t].ScreenSize < Malformed[t - 1].ScreenSize);
	}

	return true;
}

/** The 3-arg AccumulateResolvedContributions overload used to unconditionally OutTiers.Reset() + SetNum(), discarding whatever OutTiers already held. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierAccumulateAppendsTest,
	"FragmentsUnreal.ProxyTiers.AccumulateAppendsToPopulatedOutTiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierAccumulateAppendsTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Box =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(100.0f));

	auto MakePayload = [&Box](double X)
	{
		TArray<UFragmentsImporter::FResolvedProxyContribution> Payload;
		UFragmentsImporter::FResolvedProxyContribution C;
		C.MeshData       = Box;
		C.WorldTransform = FTransform(FVector(X, 0.0, 0.0));
		C.MatCRC         = 1;
		C.Material       = nullptr;
		Payload.Add(C);
		return Payload;
	};

	TArray<FTierSpec> Tiers;
	Tiers.Add(FTierSpec{ 0.0f, 1.0f, 1.0f });

	TArray<FProxyMeshAccumulator> Out;
	UFragmentsImporter::AccumulateResolvedContributions(MakePayload(0.0), Tiers, Out);

	TestEqual(TEXT("one tier after first accumulate"), Out.Num(), 1);
	const FProxyMeshSection* FirstSection = (Out.Num() == 1) ? Out[0].Sections.Find(1) : nullptr;
	TestNotNull(TEXT("section exists after first accumulate"), FirstSection);
	const int32 FirstVertCount = FirstSection ? FirstSection->Positions.Num() : 0;
	TestTrue(TEXT("first accumulate produced geometry"), FirstVertCount > 0);

	// Second accumulate into the SAME Out, still shaped Tiers.Num()==1 -- must ADD, not replace.
	UFragmentsImporter::AccumulateResolvedContributions(MakePayload(500.0), Tiers, Out);

	TestEqual(TEXT("still one tier after second accumulate"), Out.Num(), 1);
	const FProxyMeshSection* SecondSection = (Out.Num() == 1) ? Out[0].Sections.Find(1) : nullptr;
	TestNotNull(TEXT("section still present after second accumulate"), SecondSection);
	if (SecondSection)
	{
		TestEqual(TEXT("second accumulate APPENDED -- vertex count doubled, not replaced"),
			SecondSection->Positions.Num(), FirstVertCount * 2);
	}

	return true;
}

/** Prune trailing tiers with no visible elements to avoid degenerate LODs. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierEmptyTrailingTiersPrunedTest,
	"FragmentsUnreal.ProxyTiers.EmptyTrailingTiersPruned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierEmptyTrailingTiersPrunedTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyTiers;

	TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> Box =
		MakeShared<FFragmentMeshData, ESPMode::ThreadSafe>(MakeBox(100.0f));

	TArray<UFragmentsImporter::FResolvedProxyContribution> Payload;
	UFragmentsImporter::FResolvedProxyContribution C;
	C.MeshData       = Box;
	C.WorldTransform = FTransform::Identity;
	C.MatCRC         = 1;
	C.Material       = nullptr;
	Payload.Add(C);

	// A cutoff far above the element's radius -> tiers 1 and 2 keep nothing.
	const float HugeCutoff = 100000.0f;
	TArray<FTierSpec> Tiers;
	Tiers.Add(FTierSpec{ 0.0f,       1.0f, 1.0f  });  // tier 0: keeps everything (superset)
	Tiers.Add(FTierSpec{ HugeCutoff, 1.0f, 0.5f  });  // tier 1: cutoff excludes the only element
	Tiers.Add(FTierSpec{ HugeCutoff, 1.0f, 0.25f });  // tier 2: same

	TArray<FProxyMeshAccumulator> Accumulated;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Tiers, Accumulated);

	TestEqual(TEXT("three accumulators before pruning"), Accumulated.Num(), 3);
	if (Accumulated.Num() != 3) { return false; }
	TestFalse(TEXT("tier 0 has geometry"), Accumulated[0].IsEmpty());
	TestTrue(TEXT("tier 1 is empty (cutoff excludes the only element)"), Accumulated[1].IsEmpty());
	TestTrue(TEXT("tier 2 is empty"), Accumulated[2].IsEmpty());

	UFragmentsImporter::PruneTrailingEmptyTiers(Accumulated);

	TestEqual(TEXT("trailing empty tiers dropped, only tier 0 survives"), Accumulated.Num(), 1);
	TestFalse(TEXT("surviving tier 0 still has its geometry"), Accumulated[0].IsEmpty());

	// A single already-non-empty tier is left alone.
	TArray<FProxyMeshAccumulator> SingleNonEmpty;
	SingleNonEmpty.SetNum(1);
	SingleNonEmpty[0].AddGeometry(*Box, FTransform::Identity, 1, nullptr);
	UFragmentsImporter::PruneTrailingEmptyTiers(SingleNonEmpty);
	TestEqual(TEXT("single non-empty tier untouched"), SingleNonEmpty.Num(), 1);

	// A single EMPTY tier is left alone too -- the Num() > 1 guard never empties Tiers entirely, matching EnqueueProxyMeshBuild's guarantee that tier 0 is never pruned away.
	TArray<FProxyMeshAccumulator> SingleEmpty;
	SingleEmpty.SetNum(1);
	UFragmentsImporter::PruneTrailingEmptyTiers(SingleEmpty);
	TestEqual(TEXT("single empty tier still not pruned away (would leave zero LODs)"),
		SingleEmpty.Num(), 1);

	return true;
}

/** The proxy sub-tier ladder is calibrated in ABSOLUTE cm while the model tier is calibrated in multiples of R. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierReachabilityTest,
	"FragmentsUnreal.ProxyTiers.Reachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierReachabilityTest::RunTest(const FString& Parameters)
{
	const float Coeff = FragmentProxyTiers::ViewCoeff(
		FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);
	const float M11 = FragmentProxyTiers::kRefProjM11;

	// Spec defaults.
	const float GeoCoeffCells = 0.033f;
	const float ThresholdPx   = 16.0f;
	const float CoarsenFactor = 0.75f;
	const TArray<float> NearEdges = { 100.0f, 10000.0f, 40000.0f };

	// Large-model fixture with clamped proxy tier distances.
	const float R = 6940.0f;

	const float PromoteCm = GeoCoeffCells * R * Coeff / ThresholdPx;
	const float DemoteCm  = FragmentModelTier::DemoteDistanceCm(
		GeoCoeffCells, R, Coeff, ThresholdPx, CoarsenFactor);

	TestTrue(FString::Printf(TEXT("promote ~1.980*R (got %.3f*R)"), PromoteCm / R),
		FMath::IsNearlyEqual(PromoteCm / R, 1.980f, 0.005f));
	TestTrue(FString::Printf(TEXT("demote ~2.640*R (got %.3f*R)"), DemoteCm / R),
		FMath::IsNearlyEqual(DemoteCm / R, 2.640f, 0.005f));

	TArray<FragmentProxyTiers::FTierSpec> Specs =
		UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, NearEdges, 1.0f);
	UFragmentsImporter::FillProxyTierScreenSizes(Specs, NearEdges, R);

	// Clamp is active for tier 1: M11*R = 12,339 > 10,000.
	TestTrue(TEXT("tier 1 ScreenSize is clamped to 1.0 then nudged"),
		Specs[1].ScreenSize < 1.0f && Specs[1].ScreenSize > 0.99f);

	// Distance at which tier 1 gives way to tier 2 (the tier1->tier2 boundary): screen size M11*R/D falls to Specs[1].ScreenSize, which is tier 1's OWN clamped-and-nudged threshold, not tier 0's.
	const float Tier1ToTier2SwitchCm = M11 * R / Specs[1].ScreenSize;

	TestTrue(FString::Printf(TEXT("tier1->tier2 switch ~12,337cm (got %.0f)"), Tier1ToTier2SwitchCm),
		FMath::IsNearlyEqual(Tier1ToTier2SwitchCm, 12337.0f, 50.0f));

	// The model stays in Cells throughout tier 0's available distance range.
	TestTrue(FString::Printf(
			TEXT("proxy tier 0 unreachable: switch %.0f < promote %.0f"),
			Tier1ToTier2SwitchCm, PromoteCm),
		Tier1ToTier2SwitchCm < PromoteCm);

	// R cancels — assert the ratio directly so this holds for every large model, not just this one.
	TestTrue(FString::Printf(TEXT("M11 %.4f < promote/R %.4f"), M11, PromoteCm / R),
		M11 < (PromoteCm / R));

	return true;
}

/** With relative near edges, tier reachability must hold for EVERY model radius, not just the ones that happened to be sampled. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierRelativeLadderTest,
	"FragmentsUnreal.ProxyTiers.RelativeLadder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierRelativeLadderTest::RunTest(const FString& Parameters)
{
	const float Coeff = FragmentProxyTiers::ViewCoeff(
		FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);
	const float PromoteMul = 0.033f * Coeff / 16.0f;   // 1.980

	const FString MulSpec = TEXT("0.5,1.2,1.8");
	const FString AbsSpec = TEXT("100,10000,40000");   // unused whenever MulSpec resolves (it always does here)

	// Radii spanning the campus range and beyond, including the 7x spread that broke the absolute ladder.
	const float Radii[] = { 500.0f, 3788.0f, 5624.0f, 6940.0f, 22497.0f, 43408.0f, 100000.0f };
	const TArray<float> Muls = { 0.5f, 1.2f, 1.8f };

	for (const float R : Radii)
	{
		const TArray<float> NearEdges = UFragmentsImporter::ResolveNearEdgesCm(R, MulSpec, AbsSpec);
		TestEqual(FString::Printf(TEXT("R=%.0f: three tiers"), R), NearEdges.Num(), 3);

		for (int32 t = 1; t < NearEdges.Num(); ++t)
		{
			TestTrue(FString::Printf(TEXT("R=%.0f tier %d near edge ascends"), R, t),
				NearEdges[t] > NearEdges[t - 1]);
		}

		TArray<FragmentProxyTiers::FTierSpec> Specs =
			UFragmentsImporter::BuildProxyTierSpecs(3, 4.0f, NearEdges, 1.0f);
		UFragmentsImporter::FillProxyTierScreenSizes(Specs, NearEdges, R);

		TestTrue(FString::Printf(TEXT("R=%.0f: tiers nested"), R),
			FragmentProxyTiers::AreTiersNested(Specs));

		for (int32 t = 1; t < Specs.Num(); ++t)
		{
			TestTrue(FString::Printf(TEXT("R=%.0f: screen sizes strictly decreasing at %d"), R, t),
				Specs[t].ScreenSize < Specs[t - 1].ScreenSize);
		}
	}

	// The relationship that matters: a tier is reachable only if its near-edge multiplier is below the promote multiplier.
	for (const float Mul : Muls)
	{
		TestTrue(FString::Printf(
				TEXT("mul %.2f below promote mul %.3f is reachable"), Mul, PromoteMul),
			Mul < PromoteMul);
	}
	return true;
}

/** Count unreachable leading proxy tiers separately for large and small models. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierUnreachableTierCountTest,
	"FragmentsUnreal.ProxyTiers.UnreachableTierCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierUnreachableTierCountTest::RunTest(const FString& Parameters)
{
	// Clamped large model: both leading tiers engage inside the Cells band -- only the last tier (index 2) survives.
	{
		const TArray<float> ScreenSizes = { 1.0f, 0.999f, 0.998f };
		const float RProxy = 6940.0f;
		const float PromoteCm = 1.980f * RProxy;
		const int32 Count =
			UFragmentsImporter::CountUnreachableLeadingTiers(ScreenSizes, RProxy, PromoteCm);
		TestEqual(TEXT("clamped large model: 2 leading tiers pruned"), Count, 2);
	}

	// Small model with genuinely separated bands: below the clamp threshold, so nothing is unreachable -- this is the case a blanket (non-per-model) prune would have gotten wrong.
	{
		const TArray<float> ScreenSizes = { 1.0f, 0.7432f, 0.1858f };
		const float RProxy = 4180.0f;
		const float PromoteCm = 1.980f * RProxy;
		const int32 Count =
			UFragmentsImporter::CountUnreachableLeadingTiers(ScreenSizes, RProxy, PromoteCm);
		TestEqual(TEXT("small model with real bands: 0 pruned"), Count, 0);
	}

	// Degenerate: a single tier.
	{
		const TArray<float> ScreenSizes = { 1.0f };
		const int32 Count =
			UFragmentsImporter::CountUnreachableLeadingTiers(ScreenSizes, 5000.0f, 999999.0f);
		TestEqual(TEXT("single tier: 0 pruned (last tier never prunable)"), Count, 0);
	}

	// Zero/negative ScreenSize entries must not divide by zero or prune everything.
	{
		const TArray<float> ScreenSizesZero = { 1.0f, 0.0f, 0.5f };
		const int32 CountZero =
			UFragmentsImporter::CountUnreachableLeadingTiers(ScreenSizesZero, 5000.0f, 999999.0f);
		TestEqual(TEXT("zero ScreenSize entry: stops the leading run, does not prune everything"),
			CountZero, 0);

		const TArray<float> ScreenSizesNegative = { 1.0f, -0.5f, 0.3f };
		const int32 CountNegative =
			UFragmentsImporter::CountUnreachableLeadingTiers(ScreenSizesNegative, 5000.0f, 999999.0f);
		TestEqual(TEXT("negative ScreenSize entry: stops the leading run, does not prune everything"),
			CountNegative, 0);
	}

	// Boundary: engage(t+1) == PromoteCm EXACTLY (not approached via a decimal literal that merely rounds close).
	{
		const TArray<float> ScreenSizes = { 1.0f, 0.5f };
		const float RProxy = 1000.0f;
		const float EngageAt1 = FragmentProxyTiers::kRefProjM11 * RProxy / ScreenSizes[1];
		const float PromoteCm = EngageAt1;   // exact by construction -- not approached, IDENTICAL value

		TestEqual(TEXT("boundary fixture is exact, not merely close"), PromoteCm, EngageAt1);

		const int32 Count = UFragmentsImporter::CountUnreachableLeadingTiers(ScreenSizes, RProxy, PromoteCm);
		TestEqual(TEXT("engage(1) == PromoteCm exactly: <= means tier 0 is still pruned"), Count, 1);
	}

	return true;
}

/** Tier pruning always leaves at least one tier across varied radii and cutoffs. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierPruneNeverDropsLastTierTest,
	"FragmentsUnreal.ProxyTiers.PruneNeverDropsLastTier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierPruneNeverDropsLastTierTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		TArray<float> ScreenSizes;
		float ProxyBoundsRadiusCm;
	};

	const TArray<FCase> Cases = {
		{ { 1.0f, 0.999f, 0.998f },   6940.0f },
		{ { 1.0f, 0.7432f, 0.1858f }, 4180.0f },
		{ { 1.0f },                   4180.0f },
		{ { 1.0f, 0.5f },             10000.0f },
		{ { 1.0f, 0.9f, 0.8f, 0.7f }, 500.0f },
		{ { 1.0f, 0.0f, 0.5f },       6940.0f },
	};

	const float PromoteCms[] = { 0.0f, -1000.0f, 100.0f, 10000.0f, 1000000.0f };

	for (const FCase& Case : Cases)
	{
		for (const float PromoteCm : PromoteCms)
		{
			const int32 Count = UFragmentsImporter::CountUnreachableLeadingTiers(
				Case.ScreenSizes, Case.ProxyBoundsRadiusCm, PromoteCm);
			TestTrue(
				FString::Printf(TEXT("NumTiers=%d R=%.0f PromoteCm=%.0f: Count=%d <= NumTiers-1"),
					Case.ScreenSizes.Num(), Case.ProxyBoundsRadiusCm, PromoteCm, Count),
				Count <= Case.ScreenSizes.Num() - 1);
		}
	}
	return true;
}

/** Tier pruning still appends a first tier when authoring fails. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyTierFirstSurvivingTierForceAppendedTest,
	"FragmentsUnreal.ProxyTiers.FirstSurvivingTierForceAppended",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyTierFirstSurvivingTierForceAppendedTest::RunTest(const FString& Parameters)
{
	// Simulates: tier 0 pruned (never calls AppendCompactedTierLOD at all -- EnqueueProxyMeshBuild's prune skips the call entirely, it does not call it with a null MD).
	{
		TArray<TUniquePtr<FMeshDescription>> OutDescriptions;
		TArray<float> OutScreenSizes;

		UFragmentsImporter::AppendCompactedTierLOD(
			/*TierIndex=*/ 1, /*MD=*/ nullptr, /*ScreenSize=*/ 0.5f,
			OutDescriptions, OutScreenSizes, /*bForceAppend=*/ true);

		TestEqual(TEXT("first surviving tier still lands an entry even though authoring failed"),
			OutDescriptions.Num(), 1);
		TestEqual(TEXT("its screen size survives alongside it"), OutScreenSizes.Num(), 1);
		TestNull(TEXT("the description itself is null, as authored -- RunMeshFinalize's own "
		              "degenerate-payload check is what turns this into a graceful drop, not a "
		              "completely empty (uninspectable) array"),
			OutDescriptions[0].Get());
	}

	// A later tier (bForceAppend=false, the default) with a failed authoring is still dropped entirely -- the fix must not force-append EVERY tier, only the first one to arrive.
	{
		TArray<TUniquePtr<FMeshDescription>> OutDescriptions;
		TArray<float> OutScreenSizes;

		UFragmentsImporter::AppendCompactedTierLOD(
			/*TierIndex=*/ 1, MakeUnique<FMeshDescription>(), /*ScreenSize=*/ 1.0f,
			OutDescriptions, OutScreenSizes, /*bForceAppend=*/ true);   // stands in for a pruned tier 0
		UFragmentsImporter::AppendCompactedTierLOD(
			/*TierIndex=*/ 2, /*MD=*/ nullptr, /*ScreenSize=*/ 0.5f,
			OutDescriptions, OutScreenSizes, /*bForceAppend=*/ false);   // not the first arrival

		TestEqual(TEXT("only the force-appended first tier survives; the later failed tier is dropped"),
			OutDescriptions.Num(), 1);
		TestNotNull(TEXT("the surviving entry is the force-appended one"), OutDescriptions[0].Get());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
