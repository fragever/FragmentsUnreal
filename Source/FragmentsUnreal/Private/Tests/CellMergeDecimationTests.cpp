// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "Spatial/FragmentProxyDecimate.h"
#include "Spatial/FragmentProxyTiers.h"
#include "Chaos/TriangleMeshImplicitObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A planar triangle grid with exactly NumTris triangles and distinct positions, so the simplifier has real topology to work with (unlike a degenerate fan). */
	FMergeSource MakeGridSource(int32 NumTris)
	{
		FMergeSource Src;
		Src.MaterialSlot = 0;

		// NumTris triangles = NumTris/2 quads; lay them in a strip of (NumTris/2 + 1) x 2 verts.
		const int32 NumQuads = FMath::Max(1, NumTris / 2);
		for (int32 i = 0; i <= NumQuads; ++i)
		{
			for (int32 j = 0; j < 2; ++j)
			{
				Src.Positions.Add(FVector3f((float)i, (float)j, 0.0f));
				Src.Normals.Add(FVector3f(0, 0, 1));
				Src.TangentsX.Add(FVector3f(1, 0, 0));
				Src.TangentYSigns.Add(1.0f);
				Src.UV0.Add(FVector2f((float)i, (float)j));
				Src.Colors.Add(FColor::White);
			}
		}
		for (int32 i = 0; i < NumQuads && (Src.Indices.Num() / 3) < NumTris; ++i)
		{
			const uint32 A = i * 2, B = A + 1, C = A + 2, D = A + 3;
			Src.Indices.Add(A); Src.Indices.Add(B); Src.Indices.Add(C);
			if ((Src.Indices.Num() / 3) < NumTris)
			{
				Src.Indices.Add(B); Src.Indices.Add(D); Src.Indices.Add(C);
			}
		}
		Src.Instances.Add(FTransform::Identity);
		Src.LocalRadiusCm = FragmentProxyTiers::LocalRadiusCm(Src.Positions);
		return Src;
	}
}

// Expand template instances into world-space sections while preserving UVs and tangent signs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeExpandInstancesTest,
	"FragmentsUnreal.CellMergeDecimate.ExpandsInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeExpandInstancesTest::RunTest(const FString& Parameters)
{
	// One triangle template: +Z normal, +X tangent, known UVs, on slot 0.
	FMergeSource Src;
	Src.MaterialSlot = 0;
	Src.Positions     = { FVector3f(0,0,0), FVector3f(100,0,0), FVector3f(0,100,0) };
	Src.Normals       = { FVector3f(0,0,1), FVector3f(0,0,1), FVector3f(0,0,1) };
	Src.TangentsX     = { FVector3f(1,0,0), FVector3f(1,0,0), FVector3f(1,0,0) };
	Src.TangentYSigns = { 1.0f, 1.0f, 1.0f };
	Src.UV0           = { FVector2f(0.1f,0.2f), FVector2f(0.3f,0.4f), FVector2f(0.5f,0.6f) };
	Src.Colors        = { FColor::White, FColor::White, FColor::White };
	Src.Indices       = { 0, 1, 2 };

	// Two instances: identity + a 90-deg yaw translated by (1000,0,0).
	Src.Instances = {
		FTransform::Identity,
		FTransform(FRotator(0.0f, 90.0f, 0.0f), FVector(1000.0, 0.0, 0.0)),
	};

	const TArray<FragmentCellMerge::FExpandedSection> Out =
		FragmentCellMerge::ExpandSourcesToSections(MakeArrayView(&Src, 1), /*NumMaterialSlots*/ 1, /*MinRadiusCm*/ 0.0f);

	TestEqual(TEXT("one section for one slot"), Out.Num(), 1);
	if (Out.Num() != 1) { return false; }
	TestEqual(TEXT("slot preserved"), Out[0].MaterialSlot, 0);

	const FragmentProxyDecimate::FDecimateSection& G = Out[0].Geo;
	TestEqual(TEXT("verts = template x 2 instances"), G.Positions.Num(), 6);
	TestEqual(TEXT("indices = template x 2 instances"), G.Indices.Num(), 6);
	TestEqual(TEXT("all attr arrays parallel (normals)"),  G.Normals.Num(),       6);
	TestEqual(TEXT("all attr arrays parallel (tangents)"), G.TangentsX.Num(),     6);
	TestEqual(TEXT("all attr arrays parallel (signs)"),    G.TangentYSigns.Num(), 6);
	TestEqual(TEXT("all attr arrays parallel (uv)"),       G.UV0.Num(),           6);
	TestEqual(TEXT("all attr arrays parallel (color)"),    G.VertexColors.Num(),  6);

	// Instance 0 (identity): first vert == template origin.
	TestTrue(TEXT("inst0 pos == template"), G.Positions[0].Equals(FVector3f(0,0,0), 0.01f));
	// Instance 1: first vert == (0,0,0) yawed then + (1000,0,0) == (1000,0,0).
	TestTrue(TEXT("inst1 pos world-translated"), G.Positions[3].Equals(FVector3f(1000,0,0), 0.1f));

	// UV0 carried untransformed on both instances.
	TestTrue(TEXT("inst0 uv carried"), G.UV0[0].Equals(FVector2f(0.1f,0.2f), 1e-4f));
	TestTrue(TEXT("inst1 uv carried"), G.UV0[3].Equals(FVector2f(0.1f,0.2f), 1e-4f));

	// Instance 1 tangent (1,0,0) yawed 90deg -> ~(0,1,0): proves TransformVectorNoScale, not raw copy.
	TestTrue(TEXT("inst1 tangent rotated (not raw)"),
		!G.TangentsX[3].Equals(FVector3f(1,0,0), 0.01f));

	// Indices of instance 1 are offset by the instance-0 vertex count (3).
	TestEqual(TEXT("inst1 index offset"), (int32)G.Indices[3], 3);
	return true;
}

/** Moving decimation from the EXPANDED section onto the per-template FMergeSource is the memory fix and an import-time win: today a template instanced 200x is QEM'd across 200 flattened copies. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTemplateFlooredSimplifyTest,
	"FragmentsUnreal.CellMergeDecimation.TemplateFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTemplateFlooredSimplifyTest::RunTest(const FString& Parameters)
{
	// A 12-triangle template: the exact wedge-producing band. Must pass through byte-identical.
	{
		FMergeSource Slab = MakeGridSource(/*Tris=*/ 12);
		const TArray<FVector3f> Before = Slab.Positions;
		const TArray<uint32>    BeforeIdx = Slab.Indices;

		const bool bRan = FragmentCellMerge::SimplifyTemplate(
			Slab, /*Ratio=*/ 0.5f, /*MinSimplifyTris=*/ 64);

		TestFalse(TEXT("12-tri template is below the floor and is not simplified"), bRan);
		TestEqual(TEXT("positions untouched"), Slab.Positions.Num(), Before.Num());
		TestEqual(TEXT("indices untouched"), Slab.Indices.Num(), BeforeIdx.Num());
	}

	// Boundary: exactly 64 triangles IS at the floor and must simplify.
	{
		FMergeSource AtFloor = MakeGridSource(/*Tris=*/ 64);
		const int32 BeforeTris = AtFloor.Indices.Num() / 3;

		const bool bRan = FragmentCellMerge::SimplifyTemplate(AtFloor, 0.5f, 64);

		TestTrue(TEXT("64-tri template is at the floor and simplifies"), bRan);
		TestTrue(FString::Printf(TEXT("triangles reduced from %d to %d"),
				BeforeTris, AtFloor.Indices.Num() / 3),
			(AtFloor.Indices.Num() / 3) < BeforeTris);
	}

	// 63 triangles is below the floor.
	{
		FMergeSource BelowFloor = MakeGridSource(/*Tris=*/ 63);
		TestFalse(TEXT("63-tri template is below the floor"),
			FragmentCellMerge::SimplifyTemplate(BelowFloor, 0.5f, 64));
	}

	// Ratio >= 1 never simplifies regardless of size.
	{
		FMergeSource Big = MakeGridSource(/*Tris=*/ 512);
		TestFalse(TEXT("ratio 1.0 is a no-op"),
			FragmentCellMerge::SimplifyTemplate(Big, 1.0f, 64));
	}

	return true;
}

/** LocalRadiusCm must NOT be recomputed after simplification. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimplifyDoesNotMoveSelectionRadiusTest,
	"FragmentsUnreal.CellMergeDecimation.RadiusStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSimplifyDoesNotMoveSelectionRadiusTest::RunTest(const FString& Parameters)
{
	FMergeSource Src = MakeGridSource(/*Tris=*/ 512);
	const float RadiusBefore = Src.LocalRadiusCm;

	TestTrue(TEXT("precondition: it actually simplified"),
		FragmentCellMerge::SimplifyTemplate(Src, 0.5f, 64));

	TestEqual(TEXT("LocalRadiusCm is unchanged by simplification"),
		Src.LocalRadiusCm, RadiusBefore);
	return true;
}

/** Omitted materials leave no section while surviving sections retain their slot indices. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimplifiedTemplateDroppedSlotKeepsIndexTest,
	"FragmentsUnreal.CellMergeDecimation.SimplifiedDroppedSlotKeepsIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSimplifiedTemplateDroppedSlotKeepsIndexTest::RunTest(const FString& Parameters)
{
	// Slot 0: a large (>=64 tri) template with only a tiny instance -- dropped by the radius cutoff.
	auto MakeScaledGridSource = [](int32 Slot, int32 NumTris, float InstanceScale)
	{
		FMergeSource Src = MakeGridSource(NumTris);
		Src.MaterialSlot = Slot;
		Src.Instances = { FTransform(FQuat::Identity, FVector::ZeroVector, FVector(InstanceScale)) };
		return Src;
	};

	// MakeGridSource(128)'s local radius is ~32cm (half the AABB diagonal of a 64x1 quad strip), so scale 0.1 -> world radius ~3.2cm (below the 10cm cutoff below) and scale 100 -> ~3200cm (well above it).
	TArray<FMergeSource> Sources = {
		MakeScaledGridSource(0, 128, 0.1f),     // tiny instance -> dropped at cutoff below
		MakeScaledGridSource(1, 128, 100.0f),   // large instance -> survives
		MakeScaledGridSource(2, 128, 100.0f),   // large instance -> survives
	};

	// Simplify every surviving template in place BEFORE expansion, mirroring MergeMeshOnWorker's order of operations.
	for (FMergeSource& Src : Sources)
	{
		TestTrue(TEXT("128-tri template simplifies"),
			FragmentCellMerge::SimplifyTemplate(Src, /*Ratio=*/ 0.5f, /*MinSimplifyTris=*/ 64));
	}

	// Cutoff between the tiny (~3.2cm) and large (~3200cm) instance radii drops slot 0 only.
	const TArray<FragmentCellMerge::FExpandedSection> Out =
		FragmentCellMerge::ExpandSourcesToSections(Sources, /*NumMaterialSlots=*/ 3, /*MinRadiusCm=*/ 10.0f);

	TestEqual(TEXT("slot 0 dropped, two sections survive"), Out.Num(), 2);
	if (Out.Num() != 2) { return false; }
	TestEqual(TEXT("first survivor keeps slot 1, not renumbered to 0"), Out[0].MaterialSlot, 1);
	TestEqual(TEXT("second survivor keeps slot 2, not renumbered to 1"), Out[1].MaterialSlot, 2);

	return true;
}

/** MergeMeshOnWorker's #1 highest-risk invariant: collision cooks from the UNSIMPLIFIED sources, before the in-place template simplify runs. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollisionCooksFromUnsimplifiedTest,
	"FragmentsUnreal.CellMergeDecimation.CollisionCooksFromUnsimplified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCollisionCooksFromUnsimplifiedTest::RunTest(const FString& Parameters)
{
	// A single 128-tri template, comfortably above MinSimplifyTris (64), at an aggressive 0.5 ratio.
	TArray<FMergeSource> Sources = { MakeGridSource(/*Tris=*/ 128) };
	const int32 OriginalTris = Sources[0].Indices.Num() / 3;
	TestEqual(TEXT("precondition: template has 128 triangles"), OriginalTris, 128);

	const TArray<FCellSubLodSpec> SubLods = { FCellSubLodSpec{ /*MinRadiusCm*/ 0.0f, /*ScreenSize*/ 1.0f } };

	FragmentProxyDecimate::FDecimateSettings DecimateSettings;
	DecimateSettings.bEnabled = true;
	DecimateSettings.Ratio    = 0.5f;

	FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
		MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, DecimateSettings,
		/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 1.0f);

	TestTrue(TEXT("collision cooked (TriMesh non-null)"), Result.TriMesh.IsValid());
	if (!Result.TriMesh.IsValid()) { return false; }

	const int32 CollisionTris = Result.TriMesh->Elements().GetNumTriangles();
	TestEqual(TEXT("collision triangle count == UNSIMPLIFIED template count (128) -- proves collision "
			"was cooked BEFORE template simplify ran, not after"),
		CollisionTris, OriginalTris);

	TestEqual(TEXT("sub-LOD 0 authored exactly one MeshDescription"), Result.MeshDescriptions.Num(), 1);
	if (Result.MeshDescriptions.Num() == 1)
	{
		const int32 VisualTris = Result.MeshDescriptions[0]->Triangles().Num();
		TestTrue(FString::Printf(TEXT("visual sub-LOD reflects the SIMPLIFIED geometry (%d < %d), "
				"confirming simplify really ran and this isn't a vacuous comparison"),
				VisualTris, OriginalTris),
			VisualTris < OriginalTris);
	}

	return true;
}

/** A failed sub-LOD 0 authoring returns no mesh or collision geometry. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDegenerateCellReturnsNullTriMeshTest,
	"FragmentsUnreal.CellMergeDecimation.DegenerateCellReturnsNullTriMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDegenerateCellReturnsNullTriMeshTest::RunTest(const FString& Parameters)
{
	FMergeSource Src = MakeGridSource(/*Tris=*/ 128);
	// Local radius ~32cm (see FSimplifiedTemplateDroppedSlotKeepsIndexTest); scale 0.01 -> world radius ~0.32cm, far below the 1000cm cutoff below.
	Src.Instances = { FTransform(FQuat::Identity, FVector::ZeroVector, FVector(0.01f)) };
	TArray<FMergeSource> Sources = { Src };

	const TArray<FCellSubLodSpec> SubLods = { FCellSubLodSpec{ /*MinRadiusCm*/ 1000.0f, /*ScreenSize*/ 1.0f } };

	FragmentProxyDecimate::FDecimateSettings DecimateSettings;   // disabled; not the variable under test
	DecimateSettings.bEnabled = false;

	FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
		MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, DecimateSettings,
		/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 1.0f);

	TestEqual(TEXT("degenerate cell authors zero MeshDescriptions"), Result.MeshDescriptions.Num(), 0);
	TestFalse(TEXT("degenerate cell returns a null TriMesh"), Result.TriMesh.IsValid());

	return true;
}

/** Matching default collision and visual geometry reuse sub-LOD 0's cooked mesh. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollisionReusesSubLod0WhenIdenticalTest,
	"FragmentsUnreal.CellMergeDecimation.CollisionReusesSubLod0WhenIdentical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCollisionReusesSubLod0WhenIdenticalTest::RunTest(const FString& Parameters)
{
	TArray<FMergeSource> Sources = { MakeGridSource(/*Tris=*/ 128) };

	const TArray<FCellSubLodSpec> SubLods = { FCellSubLodSpec{ /*MinRadiusCm*/ 0.0f, /*ScreenSize*/ 1.0f } };

	FragmentProxyDecimate::FDecimateSettings DecimateSettings;
	DecimateSettings.bEnabled = false;   // simplify off -> Sources unmodified between the two passes

	FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
		MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, DecimateSettings,
		/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 1.0f);

	TestTrue(TEXT("fast path taken: collision reused sub-LOD 0's own authoring output"),
		Result.bCollisionReusedSubLod0);

	TestTrue(TEXT("collision cooked (TriMesh non-null)"), Result.TriMesh.IsValid());
	TestEqual(TEXT("sub-LOD 0 authored exactly one MeshDescription (count unchanged from today)"),
		Result.MeshDescriptions.Num(), 1);
	TestEqual(TEXT("screen sizes parallel MeshDescriptions (count unchanged from today)"),
		Result.LODScreenSizes.Num(), 1);

	if (Result.TriMesh.IsValid() && Result.MeshDescriptions.Num() == 1)
	{
		const int32 CollisionTris = Result.TriMesh->Elements().GetNumTriangles();
		const int32 VisualTris    = Result.MeshDescriptions[0]->Triangles().Num();
		TestEqual(TEXT("collision triangle count equals sub-LOD 0's visual triangle count exactly"),
			CollisionTris, VisualTris);
	}

	return true;
}

/** The collision fast path requires identical collision and visual geometry. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollisionSeparateWhenSimplifyingTest,
	"FragmentsUnreal.CellMergeDecimation.CollisionSeparateWhenSimplifying",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCollisionSeparateWhenSimplifyingTest::RunTest(const FString& Parameters)
{
	const TArray<FCellSubLodSpec> SubLods = { FCellSubLodSpec{ /*MinRadiusCm*/ 0.0f, /*ScreenSize*/ 1.0f } };

	// Case (a): simplify enabled at ratio 0.5, template above MinSimplifyTris, collision undecimated.
	{
		TArray<FMergeSource> Sources = { MakeGridSource(/*Tris=*/ 128) };
		const int32 OriginalTris = Sources[0].Indices.Num() / 3;

		FragmentProxyDecimate::FDecimateSettings DecimateSettings;
		DecimateSettings.bEnabled = true;
		DecimateSettings.Ratio    = 0.5f;

		FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
			MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, DecimateSettings,
			/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 1.0f);

		TestFalse(TEXT("(a) slow path taken: simplify can run, so guard must be false"),
			Result.bCollisionReusedSubLod0);
		TestTrue(TEXT("(a) collision cooked (TriMesh non-null)"), Result.TriMesh.IsValid());
		if (Result.TriMesh.IsValid())
		{
			TestEqual(TEXT("(a) collision triangle count == UNSIMPLIFIED template count (128)"),
				Result.TriMesh->Elements().GetNumTriangles(), OriginalTris);
		}
		if (Result.MeshDescriptions.Num() == 1)
		{
			TestTrue(TEXT("(a) visual sub-LOD reflects the SIMPLIFIED geometry (proves simplify ran)"),
				Result.MeshDescriptions[0]->Triangles().Num() < OriginalTris);
		}
	}

	// Case (b): simplify disabled, collision independently decimated at ratio 0.5.
	{
		TArray<FMergeSource> Sources = { MakeGridSource(/*Tris=*/ 128) };
		const int32 OriginalTris = Sources[0].Indices.Num() / 3;

		FragmentProxyDecimate::FDecimateSettings DecimateSettings;
		DecimateSettings.bEnabled = false;

		FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
			MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, DecimateSettings,
			/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 0.5f);

		TestFalse(TEXT("(b) slow path taken: CollisionDecimateRatio < 1.0, so guard must be false"),
			Result.bCollisionReusedSubLod0);
		TestTrue(TEXT("(b) collision cooked (TriMesh non-null)"), Result.TriMesh.IsValid());
		if (Result.MeshDescriptions.Num() == 1)
		{
			TestEqual(TEXT("(b) visual sub-LOD is UNSIMPLIFIED (simplify disabled): full 128 triangles"),
				Result.MeshDescriptions[0]->Triangles().Num(), OriginalTris);
		}
		if (Result.TriMesh.IsValid())
		{
			const int32 CollisionTris = Result.TriMesh->Elements().GetNumTriangles();
			// Collision decimation is independent of the full-detail visual mesh.
			TestTrue(TEXT("(b) collision triangle count reduced from the unsimplified 128, "
					"proving CollisionDecimateRatio applied on the separate path"),
				CollisionTris < OriginalTris);
		}
	}

	return true;
}

/** Shell and interior geometry use separate simplification gates. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellSimplifyGateTest,
	"FragmentsUnreal.CellMergeDecimation.ShellSimplifyGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellSimplifyGateTest::RunTest(const FString& Parameters)
{
	FragmentProxyDecimate::FDecimateSettings EnabledIn;
	EnabledIn.bEnabled = true;
	EnabledIn.Ratio    = 0.5f;

	// Shell, SimplifyShell off (the default): gated off.
	{
		const FragmentProxyDecimate::FDecimateSettings Out =
			UFragmentsImporter::ApplyShellSimplifyGate(EnabledIn, /*bShellSide*/ true, /*bSimplifyShell*/ false);
		TestFalse(TEXT("shell + SimplifyShell=0 disables simplification"), Out.bEnabled);
	}

	// Interior, SimplifyShell off: untouched (the gate only ever restricts shell).
	{
		const FragmentProxyDecimate::FDecimateSettings Out =
			UFragmentsImporter::ApplyShellSimplifyGate(EnabledIn, /*bShellSide*/ false, /*bSimplifyShell*/ false);
		TestTrue(TEXT("interior + SimplifyShell=0 leaves bEnabled true"), Out.bEnabled);
		TestEqual(TEXT("interior + SimplifyShell=0 leaves Ratio untouched"), Out.Ratio, 0.5f);
	}

	// Shell, SimplifyShell on: shell opted back in, untouched.
	{
		const FragmentProxyDecimate::FDecimateSettings Out =
			UFragmentsImporter::ApplyShellSimplifyGate(EnabledIn, /*bShellSide*/ true, /*bSimplifyShell*/ true);
		TestTrue(TEXT("shell + SimplifyShell=1 leaves bEnabled true"), Out.bEnabled);
		TestEqual(TEXT("shell + SimplifyShell=1 leaves Ratio untouched"), Out.Ratio, 0.5f);
	}

	// Base settings already disabled: the gate must never turn simplification ON.
	{
		FragmentProxyDecimate::FDecimateSettings DisabledIn;
		DisabledIn.bEnabled = false;
		DisabledIn.Ratio    = 0.5f;
		const FragmentProxyDecimate::FDecimateSettings Out =
			UFragmentsImporter::ApplyShellSimplifyGate(DisabledIn, /*bShellSide*/ false, /*bSimplifyShell*/ true);
		TestFalse(TEXT("gate never enables what was already off"), Out.bEnabled);
	}

	return true;
}

/** The simplification gate affects the authored geometry. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellGateEndToEndTest,
	"FragmentsUnreal.CellMergeDecimation.ShellGateEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellGateEndToEndTest::RunTest(const FString& Parameters)
{
	FragmentProxyDecimate::FDecimateSettings BaseSettings;
	BaseSettings.bEnabled = true;
	BaseSettings.Ratio    = 0.5f;

	const TArray<FCellSubLodSpec> SubLods = { FCellSubLodSpec{ /*MinRadiusCm*/ 0.0f, /*ScreenSize*/ 1.0f } };

	// Shell cell: gate resolves SimplifyShell=0 -> bEnabled forced false -> sub-LOD 0 unsimplified.
	{
		TArray<FMergeSource> Sources = { MakeGridSource(/*Tris=*/ 128) };
		const int32 OriginalTris = Sources[0].Indices.Num() / 3;
		TestEqual(TEXT("precondition: template has 128 triangles"), OriginalTris, 128);

		const FragmentProxyDecimate::FDecimateSettings ShellSettings =
			UFragmentsImporter::ApplyShellSimplifyGate(BaseSettings, /*bShellSide*/ true, /*bSimplifyShell*/ false);
		TestFalse(TEXT("precondition: shell gate disabled simplify"), ShellSettings.bEnabled);

		FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
			MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, ShellSettings,
			/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 1.0f);

		TestEqual(TEXT("shell run authored exactly one MeshDescription"), Result.MeshDescriptions.Num(), 1);
		if (Result.MeshDescriptions.Num() == 1)
		{
			TestEqual(TEXT("shell run's sub-LOD 0 is the UNSIMPLIFIED 128 triangles"),
				Result.MeshDescriptions[0]->Triangles().Num(), OriginalTris);
		}
	}

	// Interior cell: the gate is a no-op (!bShellSide short-circuits) -> simplify runs as configured.
	{
		TArray<FMergeSource> Sources = { MakeGridSource(/*Tris=*/ 128) };
		const int32 OriginalTris = Sources[0].Indices.Num() / 3;

		const FragmentProxyDecimate::FDecimateSettings InteriorSettings =
			UFragmentsImporter::ApplyShellSimplifyGate(BaseSettings, /*bShellSide*/ false, /*bSimplifyShell*/ false);
		TestTrue(TEXT("precondition: interior gate leaves simplify enabled"), InteriorSettings.bEnabled);

		FragmentCellMerge::FMergeWorkerResult Result = FragmentCellMerge::MergeMeshOnWorker(
			MoveTemp(Sources), /*NumMaterialSlots=*/ 1, SubLods, InteriorSettings,
			/*MinSimplifyTris=*/ 64, /*CollisionDecimateRatio=*/ 1.0f);

		TestEqual(TEXT("interior run authored exactly one MeshDescription"), Result.MeshDescriptions.Num(), 1);
		if (Result.MeshDescriptions.Num() == 1)
		{
			TestTrue(TEXT("interior run's sub-LOD 0 is REDUCED below the unsimplified 128"),
				Result.MeshDescriptions[0]->Triangles().Num() < OriginalTris);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
