// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentShellAuthoring.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ShellAuthoringTestsPrivate
{
	// A single flat 100x100 quad in the XY plane, as one 4-point profile.
	FragmentShellAuthoring::FShellSource MakeQuadSource()
	{
		FragmentShellAuthoring::FShellSource Src;
		Src.Points = {
			FVector(0, 0, 0), FVector(100, 0, 0),
			FVector(100, 100, 0), FVector(0, 100, 0)
		};
		Src.Profiles.Add(TArray<int32>{ 0, 1, 2, 3 });
		return Src;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellAuthoringQuadTest,
	"FragmentsUnreal.ShellAuthoring.AuthorShellMeshDescription_TessellatesQuad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellAuthoringQuadTest::RunTest(const FString& Parameters)
{
	using namespace ShellAuthoringTestsPrivate;

	const FragmentShellAuthoring::FShellSource Src = MakeQuadSource();
	FMeshDescription MD;
	TArray<FVector3f> CollisionPositions;
	TArray<uint32> CollisionIndices;

	const bool bOk = FragmentShellAuthoring::AuthorShellMeshDescription(
		Src, FColor(10, 20, 30, 255), MD, CollisionPositions, CollisionIndices);

	TestTrue(TEXT("authoring succeeded"), bOk);
	TestEqual(TEXT("quad tessellates to 2 triangles"), MD.Triangles().Num(), 2);
	TestEqual(TEXT("one polygon group"), MD.PolygonGroups().Num(), 1);
	TestEqual(TEXT("collision indices match triangles"), CollisionIndices.Num(), 6);
	TestEqual(TEXT("collision positions populated"), CollisionPositions.Num(), MD.Vertices().Num());

	// Normals must be computed (non-zero) and vertex color carried through.
	FStaticMeshAttributes Attributes(MD);
	TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	bool bAllNormalsValid = MD.VertexInstances().Num() > 0;
	bool bAllColorsMatch = MD.VertexInstances().Num() > 0;
	const FVector4f Expected = FVector4f(FLinearColor(FColor(10, 20, 30, 255)));
	for (const FVertexInstanceID VIID : MD.VertexInstances().GetElementIDs())
	{
		if (Normals.Get(VIID).IsNearlyZero()) { bAllNormalsValid = false; }
		if (!Colors.Get(VIID).Equals(Expected, 0.001f)) { bAllColorsMatch = false; }
	}
	TestTrue(TEXT("normals computed"), bAllNormalsValid);
	TestTrue(TEXT("vertex colors carried"), bAllColorsMatch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellAuthoringEmptySourceTest,
	"FragmentsUnreal.ShellAuthoring.AuthorShellMeshDescription_RejectsEmptySource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellAuthoringEmptySourceTest::RunTest(const FString& Parameters)
{
	FragmentShellAuthoring::FShellSource Empty;
	FMeshDescription MD;
	TArray<FVector3f> CollisionPositions;
	TArray<uint32> CollisionIndices;

	TestFalse(TEXT("empty source rejected"),
		FragmentShellAuthoring::AuthorShellMeshDescription(
			Empty, FColor::White, MD, CollisionPositions, CollisionIndices));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellAuthoringUniqueNameTest,
	"FragmentsUnreal.ShellAuthoring.MakeUniqueShellAssetName_IsUnique",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellAuthoringUniqueNameTest::RunTest(const FString& Parameters)
{
	const FString A = FragmentShellAuthoring::MakeUniqueShellAssetName();
	const FString B = FragmentShellAuthoring::MakeUniqueShellAssetName();
	TestNotEqual(TEXT("successive names differ"), A, B);
	TestTrue(TEXT("name has expected prefix"), A.StartsWith(TEXT("Shell_FB_")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModelBufferPinTest,
	"FragmentsUnreal.ShellAuthoring.ModelBufferPin_OutlivesReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModelBufferPinTest::RunTest(const FString& Parameters)
{
	UFragmentModelWrapper* Wrapper = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	Wrapper->AddToRoot();

	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(64);
	Buffer[0] = 0xAB;
	Wrapper->LoadModel(MoveTemp(Buffer));

	FModelBufferPin Pin = Wrapper->GetBufferPin();
	TestTrue(TEXT("pin is valid"), Pin.IsValid());
	if (Pin.IsValid())
	{
		TestEqual(TEXT("pin sees the loaded bytes"), (*Pin)[0], (uint8)0xAB);
	}

	// Reload with different content: the old pin must still hold the old bytes, proving the worker's view cannot be pulled out from under it.
	TArray<uint8> Second;
	Second.SetNumZeroed(64);
	Second[0] = 0xCD;
	Wrapper->LoadModel(MoveTemp(Second));

	if (Pin.IsValid())
	{
		TestEqual(TEXT("old pin unchanged after reload"), (*Pin)[0], (uint8)0xAB);
	}

	FModelBufferPin NewPin = Wrapper->GetBufferPin();
	TestTrue(TEXT("new pin is valid"), NewPin.IsValid());
	if (NewPin.IsValid())
	{
		TestEqual(TEXT("new pin sees new bytes"), (*NewPin)[0], (uint8)0xCD);
	}

	Wrapper->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellAuthoringExtractTest,
	"FragmentsUnreal.ShellAuthoring.ExtractMeshData_MatchesAuthoredMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellAuthoringExtractTest::RunTest(const FString& Parameters)
{
	using namespace ShellAuthoringTestsPrivate;

	const FragmentShellAuthoring::FShellSource Src = MakeQuadSource();
	FMeshDescription MD;
	TArray<FVector3f> CollisionPositions;
	TArray<uint32> CollisionIndices;
	const FColor Color(10, 20, 30, 255);

	TestTrue(TEXT("authoring succeeded"),
		FragmentShellAuthoring::AuthorShellMeshDescription(
			Src, Color, MD, CollisionPositions, CollisionIndices));

	FFragmentMeshData Data;
	FragmentShellAuthoring::ExtractMeshData(MD, Color, Data);

	TestTrue(TEXT("extract is valid"), Data.bIsValid);
	TestEqual(TEXT("one vertex per vertex instance"), Data.Vertices.Num(), MD.VertexInstances().Num());
	TestEqual(TEXT("index count matches triangles"), Data.Indices.Num(), MD.Triangles().Num() * 3);
	if (Data.Vertices.Num() > 0)
	{
		TestEqual(TEXT("color carried"), Data.Vertices[0].Color, Color);
	}

	// Quad spans (0,0,0)-(100,100,0) in local space (no world transform applied).
	TestTrue(TEXT("bounds min"), Data.BoundingBox.Min.Equals(FVector3f(0, 0, 0), 0.01f));
	TestTrue(TEXT("bounds max"), Data.BoundingBox.Max.Equals(FVector3f(100, 100, 0), 0.01f));

	// Every index must address a real vertex.
	bool bIndicesInRange = true;
	for (uint32 Idx : Data.Indices)
	{
		if ((int32)Idx >= Data.Vertices.Num()) { bIndicesInRange = false; break; }
	}
	TestTrue(TEXT("indices in range"), bIndicesInRange);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
