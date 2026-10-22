// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Utils/FragmentsUtils.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MeshGenMigrationTestsPrivate
{
	FFragmentMeshVertex MakeVert(const FVector3f& P, const FVector3f& Normal = FVector3f(0, 0, 1))
	{
		FFragmentMeshVertex V;
		V.Position = P;
		V.Normal = Normal;
		V.TangentX = FVector3f(1, 0, 0);
		V.TangentYSign = 1.0f;
		V.UV0 = FVector2f(0, 0);
		V.Color = FColor::White;
		return V;
	}

	// Builds a minimal valid quad (4 verts, 2 triangles) as a triangle soup.
	FFragmentMeshData MakeValidQuad(const FVector3f& Normal = FVector3f(0, 0, 1))
	{
		FFragmentMeshData MeshData;
		MeshData.AssetName = TEXT("TestQuad");
		MeshData.bIsValid = true;
		MeshData.Vertices.Add(MakeVert(FVector3f(0, 0, 0), Normal));
		MeshData.Vertices.Add(MakeVert(FVector3f(100, 0, 0), Normal));
		MeshData.Vertices.Add(MakeVert(FVector3f(100, 100, 0), Normal));
		MeshData.Vertices.Add(MakeVert(FVector3f(0, 100, 0), Normal));
		MeshData.Indices = { 0, 1, 2, 0, 2, 3 };
		MeshData.BoundingBox = FBox3f(FVector3f(0, 0, 0), FVector3f(100, 100, 0));
		return MeshData;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshGenCommitToMeshDescriptionTest,
	"FragmentsUnreal.MeshGen.CommitToMeshDescription_ProducesValidMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshGenCommitToMeshDescriptionTest::RunTest(const FString& Parameters)
{
	using namespace MeshGenMigrationTestsPrivate;

	FFragmentMeshData MeshData = MakeValidQuad();

	UMaterialInterface* Mat = UMaterial::GetDefaultMaterial(MD_Surface);

	UStaticMesh* Mesh = UFragmentsImporter::CommitToMeshDescription(
		MeshData, GetTransientPackage(), Mat, TEXT("TestModelGuid"));

	TestNotNull(TEXT("mesh created"), Mesh);
	if (!Mesh) { return false; }

	TestTrue(TEXT("has render data"), Mesh->GetRenderData() != nullptr && Mesh->GetRenderData()->LODResources.Num() > 0);
	if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
	{
		const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
		TestEqual(TEXT("triangle count"), (int32)LOD.GetNumTriangles(), 2);
	}
	TestEqual(TEXT("material slot count"), Mesh->GetStaticMaterials().Num(), 1);
	TestNotNull(TEXT("body setup attached"), Mesh->GetBodySetup());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshGenCommitToMeshDescriptionRejectsInvalidInputTest,
	"FragmentsUnreal.MeshGen.CommitToMeshDescription_RejectsInvalidInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshGenCommitToMeshDescriptionRejectsInvalidInputTest::RunTest(const FString& Parameters)
{
	using namespace MeshGenMigrationTestsPrivate;

	UMaterialInterface* Mat = UMaterial::GetDefaultMaterial(MD_Surface);

	// (a) bIsValid = false with otherwise-valid data.
	{
		FFragmentMeshData MeshData = MakeValidQuad();
		MeshData.bIsValid = false;
		UStaticMesh* Mesh = UFragmentsImporter::CommitToMeshDescription(
			MeshData, GetTransientPackage(), Mat, TEXT("TestModelGuid"));
		TestNull(TEXT("bIsValid=false returns nullptr"), Mesh);
	}

	// (b) empty Vertices.
	{
		FFragmentMeshData MeshData = MakeValidQuad();
		MeshData.Vertices.Empty();
		UStaticMesh* Mesh = UFragmentsImporter::CommitToMeshDescription(
			MeshData, GetTransientPackage(), Mat, TEXT("TestModelGuid"));
		TestNull(TEXT("empty Vertices returns nullptr"), Mesh);
	}

	// (c) Indices with fewer than 3 entries.
	{
		FFragmentMeshData MeshData = MakeValidQuad();
		MeshData.Indices = { 0, 1 };
		UStaticMesh* Mesh = UFragmentsImporter::CommitToMeshDescription(
			MeshData, GetTransientPackage(), Mat, TEXT("TestModelGuid"));
		TestNull(TEXT("Indices.Num() < 3 returns nullptr"), Mesh);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshGenCommitToMeshDescriptionPreservesAuthoredNormalsTest,
	"FragmentsUnreal.MeshGen.CommitToMeshDescription_PreservesAuthoredNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshGenCommitToMeshDescriptionPreservesAuthoredNormalsTest::RunTest(const FString& Parameters)
{
	using namespace MeshGenMigrationTestsPrivate;

	// A distinctive tilted normal that a flat-quad geometric recompute (which would produce (0,0,1)) could never produce by accident.
	const FVector3f AuthoredNormal(0.0f, 0.6f, 0.8f);
	FFragmentMeshData MeshData = MakeValidQuad(AuthoredNormal);

	UMaterialInterface* Mat = UMaterial::GetDefaultMaterial(MD_Surface);

	UStaticMesh* Mesh = UFragmentsImporter::CommitToMeshDescription(
		MeshData, GetTransientPackage(), Mat, TEXT("TestModelGuid"));

	TestNotNull(TEXT("mesh created"), Mesh);
	if (!Mesh) { return false; }

	const bool bHasRenderData = Mesh->GetRenderData() != nullptr && Mesh->GetRenderData()->LODResources.Num() > 0;
	TestTrue(TEXT("has render data"), bHasRenderData);
	if (!bHasRenderData) { return false; }

	const FVector3f ReadBackNormal =
		Mesh->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(0);

	const bool bNormalPreserved = ReadBackNormal.Equals(AuthoredNormal, 0.05f);
	TestTrue(TEXT("authored normal survived build (not recomputed)"), bNormalPreserved);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
