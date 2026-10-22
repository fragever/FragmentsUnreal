// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// RunMeshFinalize regression coverage.
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentShellAuthoring.h"
#include "Spatial/FragmentMeshAuthoring.h"
#include "MeshDescription.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MeshFinalizeCpuAccessTestsPrivate
{
	// Build a nonempty quad so mesh finalization reaches the CPU access path.
	TUniquePtr<FMeshDescription> MakeQuadMeshDescription()
	{
		FragmentShellAuthoring::FShellSource Src;
		Src.Points = {
			FVector(0, 0, 0), FVector(100, 0, 0),
			FVector(100, 100, 0), FVector(0, 100, 0)
		};
		Src.Profiles.Add(TArray<int32>{ 0, 1, 2, 3 });

		TUniquePtr<FMeshDescription> MD = MakeUnique<FMeshDescription>();
		TArray<FVector3f> CollisionPositions;
		TArray<uint32> CollisionIndices;
		FragmentShellAuthoring::AuthorShellMeshDescription(
			Src, FColor::White, *MD, CollisionPositions, CollisionIndices);
		return MD;
	}

	/** Slot-name prefix the proxy path authors with. */
	constexpr const TCHAR* kProxySlotPrefix = TEXT("ProxySlot_");

	/** Author an FMeshDescription with one polygon group per entry in Slots, named "ProxySlot_<slot>" -- exactly what the proxy path produces. */
	TUniquePtr<FMeshDescription> MakeMultiSlotMeshDescription(TConstArrayView<int32> Slots)
	{
		// FAuthoringSection's fields are TConstArrayView and BORROW their storage (see its doc comment), so the backing arrays must outlive the AuthorMeshDescription call below.
		struct FSlotGeometry
		{
			TArray<FVector3f> Positions, Normals, TangentsX;
			TArray<float>     TangentYSigns;
			TArray<FVector2f> UV0;
			TArray<FColor>    Colors;
			TArray<uint32>    Indices;
		};
		TArray<FSlotGeometry> Storage;
		Storage.Reserve(Slots.Num());

		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			// One independent triangle per slot, offset so nothing degenerates or welds together.
			const float X = 500.0f * i;
			FSlotGeometry& Geo = Storage.AddDefaulted_GetRef();
			Geo.Positions     = { FVector3f(X, 0, 0), FVector3f(X + 100, 0, 0), FVector3f(X, 100, 0) };
			Geo.Normals       = { FVector3f(0,0,1), FVector3f(0,0,1), FVector3f(0,0,1) };
			Geo.TangentsX     = { FVector3f(1,0,0), FVector3f(1,0,0), FVector3f(1,0,0) };
			Geo.TangentYSigns = { 1.0f, 1.0f, 1.0f };
			Geo.UV0           = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
			Geo.Colors        = { FColor::White, FColor::White, FColor::White };
			Geo.Indices       = { 0, 1, 2 };
		}

		TArray<FragmentMeshAuthoring::FAuthoringSection> Sections;
		Sections.Reserve(Slots.Num());
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			FragmentMeshAuthoring::FAuthoringSection& Sec = Sections.AddDefaulted_GetRef();
			Sec.MaterialSlot  = Slots[i];
			Sec.Positions     = Storage[i].Positions;
			Sec.Normals       = Storage[i].Normals;
			Sec.TangentsX     = Storage[i].TangentsX;
			Sec.TangentYSigns = Storage[i].TangentYSigns;
			Sec.UV0           = Storage[i].UV0;
			Sec.Colors        = Storage[i].Colors;
			Sec.Indices       = Storage[i].Indices;
		}

		TUniquePtr<FMeshDescription> MD = MakeUnique<FMeshDescription>();
		TArray<FVector3f> CollisionPositions;
		TArray<uint32> CollisionIndices;
		FragmentMeshAuthoring::AuthorMeshDescription(
			Sections, kProxySlotPrefix, *MD, CollisionPositions, CollisionIndices);
		return MD;
	}
}

/** Dropped tier materials must not renumber surviving material slots. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshFinalizeMaterialSlotStabilityTest,
	"FragmentsUnreal.MeshFinalize.RunMeshFinalize_DroppedSlotDoesNotRenumber",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshFinalizeMaterialSlotStabilityTest::RunTest(const FString& Parameters)
{
	using namespace MeshFinalizeCpuAccessTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	Mesh->AddToRoot();

	// Unroot on every exit, including the early return below -- a hand-maintained pair of RemoveFromRoot calls per return path is how the sibling test in this file ends up leaking Mesh rooted on its failure path.
	ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); Importer->RemoveFromRoot(); };

	// Four shared slots, assigned exactly as EnqueueProxyMeshBuild does.
	for (int32 i = 0; i < 4; ++i)
	{
		Mesh->GetStaticMaterials().Add(
			FStaticMaterial(nullptr, *FString::Printf(TEXT("%s%d"), kProxySlotPrefix, i)));
	}

	const int32 Lod0Slots[] = { 0, 1, 2, 3 };
	const int32 Lod1Slots[] = { 0, 2, 3 };   // slot 1 dropped -- the far-tier case

	FPendingMeshFinalize Payload;
	Payload.WeakMesh = Mesh;
	Payload.MeshDescriptions.Add(MakeMultiSlotMeshDescription(Lod0Slots));
	Payload.MeshDescriptions.Add(MakeMultiSlotMeshDescription(Lod1Slots));
	Payload.LODScreenSizes = { 1.0f, 0.5f };
	Payload.DebugLabel = TEXT("MaterialSlotStability");
	Payload.Finalizer = [](UStaticMesh*) {};

	Importer->RunMeshFinalize(Payload);

	const FStaticMeshRenderData* RD = Mesh->GetRenderData();
	if (!RD || RD->LODResources.Num() < 2)
	{
		AddError(TEXT("expected two built LODs"));
		return false;
	}

	auto CheckLod = [this, RD](int32 LOD, TConstArrayView<int32> ExpectedSlots)
	{
		TArray<int32> Got;
		for (const FStaticMeshSection& S : RD->LODResources[LOD].Sections) { Got.Add(S.MaterialIndex); }

		// Built once per LOD, not once per assertion.
		const FString Desc = FString::JoinBy(Got, TEXT(","),
			[](int32 Idx) { return FString::FromInt(Idx); });

		if (!TestEqual(*FString::Printf(TEXT("LOD%d section count (got [%s])"), LOD, *Desc),
				Got.Num(), ExpectedSlots.Num()))
		{
			return;
		}
		for (int32 i = 0; i < ExpectedSlots.Num(); ++i)
		{
			TestEqual(*FString::Printf(
					TEXT("LOD%d section %d must map to slot %d, not renumber (got [%s])"),
					LOD, i, ExpectedSlots[i], *Desc),
				Got[i], ExpectedSlots[i]);
		}
	};

	// LOD0 uses every slot, so it is the control: contiguous here is CORRECT.
	CheckLod(0, Lod0Slots);
	// LOD1 is the real assertion -- the surviving sections must keep their ORIGINAL slots, gapped. [0,1,2] would mean the geometry authored for slot 2 renders with slot 1's material.
	CheckLod(1, Lod1Slots);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshFinalizeCpuAccessDefaultTest,
	"FragmentsUnreal.MeshFinalize.PendingMeshFinalize_CpuAccessDefaultsFalse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshFinalizeCpuAccessDefaultTest::RunTest(const FString& Parameters)
{
	// Proxy (LOD2) and cell-merge (LOD1) producers never touch bNeedsCpuAccess, so a freshly constructed payload — as they build it — must default false.
	FPendingMeshFinalize Payload;
	TestFalse(TEXT("bNeedsCpuAccess defaults false"), Payload.bNeedsCpuAccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshFinalizeCpuAccessHonoredTest,
	"FragmentsUnreal.MeshFinalize.RunMeshFinalize_HonorsCpuAccessFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshFinalizeCpuAccessHonoredTest::RunTest(const FString& Parameters)
{
	using namespace MeshFinalizeCpuAccessTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	// Case 1: bNeedsCpuAccess = true (LOD0 shell-mesh producer's setting).
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->AddToRoot();

		FPendingMeshFinalize Payload;
		Payload.WeakMesh = Mesh;
		Payload.MeshDescriptions.Add(MakeQuadMeshDescription());
		Payload.DebugLabel = TEXT("CpuAccessTrue");
		Payload.bNeedsCpuAccess = true;

		bool bFinalizerFiredWithMesh = false;
		Payload.Finalizer = [&bFinalizerFiredWithMesh](UStaticMesh* M) { bFinalizerFiredWithMesh = (M != nullptr); };

		Importer->RunMeshFinalize(Payload);

		TestTrue(TEXT("finalizer fired with built mesh (true case)"), bFinalizerFiredWithMesh);
		TestTrue(TEXT("bAllowCPUAccess set true when bNeedsCpuAccess is true"), Mesh->bAllowCPUAccess);
	}

	// Case 2: bNeedsCpuAccess = false (proxy/cell-merge producers' setting).
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		Mesh->AddToRoot();

		FPendingMeshFinalize Payload;
		Payload.WeakMesh = Mesh;
		Payload.MeshDescriptions.Add(MakeQuadMeshDescription());
		Payload.DebugLabel = TEXT("CpuAccessFalse");
		Payload.bNeedsCpuAccess = false;

		bool bFinalizerFiredWithMesh = false;
		Payload.Finalizer = [&bFinalizerFiredWithMesh](UStaticMesh* M) { bFinalizerFiredWithMesh = (M != nullptr); };

		Importer->RunMeshFinalize(Payload);

		TestTrue(TEXT("finalizer fired with built mesh (false case)"), bFinalizerFiredWithMesh);
		TestFalse(TEXT("bAllowCPUAccess stays false when bNeedsCpuAccess is false"), Mesh->bAllowCPUAccess);
	}

	Importer->RemoveFromRoot();
	return true;
}

/** The multi-LOD proxy path had no test that actually calls production code. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshFinalizeMultiLODScreenSizeTest,
	"FragmentsUnreal.MeshFinalize.RunMeshFinalize_WritesScreenSizePerLOD",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshFinalizeMultiLODScreenSizeTest::RunTest(const FString& Parameters)
{
	using namespace MeshFinalizeCpuAccessTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	Mesh->AddToRoot();

	FPendingMeshFinalize Payload;
	Payload.WeakMesh = Mesh;
	Payload.MeshDescriptions.Add(MakeQuadMeshDescription());
	Payload.MeshDescriptions.Add(MakeQuadMeshDescription());
	Payload.LODScreenSizes = { 1.0f, 0.35f };
	Payload.DebugLabel = TEXT("MultiLODScreenSize");

	bool bFinalizerFiredWithMesh = false;
	Payload.Finalizer = [&bFinalizerFiredWithMesh](UStaticMesh* M) { bFinalizerFiredWithMesh = (M != nullptr); };

	Importer->RunMeshFinalize(Payload);

	TestTrue(TEXT("finalizer fired with built mesh"), bFinalizerFiredWithMesh);

	FStaticMeshRenderData* RD = Mesh->GetRenderData();
	TestNotNull(TEXT("render data exists"), RD);
	if (!RD) { Importer->RemoveFromRoot(); return false; }

	TestEqual(TEXT("both LODs built"), RD->LODResources.Num(), 2);
	if (RD->LODResources.Num() >= 2)
	{
		TestEqual(TEXT("LOD1 screen size overridden to payload value, not the engine's 0.75^LOD default"),
			RD->ScreenSize[1].Default, 0.35f);
	}

	Importer->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
