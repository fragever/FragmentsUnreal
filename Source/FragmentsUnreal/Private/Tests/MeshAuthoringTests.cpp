// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Misc/AutomationTest.h"
#include "Spatial/FragmentMeshAuthoring.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Owns backing arrays for one quad section; FAuthoringSection views borrow from these, so instances of this struct must outlive any AuthorMeshDescription call using them.
	struct FQuadSectionData
	{
		int32 MaterialSlot = 0;
		TArray<FVector3f>  Positions;
		TArray<FVector3f>  Normals;
		TArray<FVector3f>  TangentsX;
		TArray<float>      TangentYSigns;
		TArray<FVector2f>  UV0;
		TArray<FColor>     Colors;
		TArray<uint32>     Indices;
		TArray<FTransform> Instances;
	};

	// One quad (4 verts, 2 tris) as backing data for the given slot, identity instance.
	FQuadSectionData MakeQuadSectionData(int32 Slot)
	{
		FQuadSectionData S;
		S.MaterialSlot = Slot;
		S.Positions = { FVector3f(0,0,0), FVector3f(100,0,0), FVector3f(100,100,0), FVector3f(0,100,0) };
		S.Normals   = { FVector3f(0,0,1), FVector3f(0,0,1), FVector3f(0,0,1), FVector3f(0,0,1) };
		S.TangentsX = { FVector3f(1,0,0), FVector3f(1,0,0), FVector3f(1,0,0), FVector3f(1,0,0) };
		S.TangentYSigns = { 1.f, 1.f, 1.f, 1.f };
		S.UV0    = { FVector2f(0,0), FVector2f(1,0), FVector2f(1,1), FVector2f(0,1) };
		S.Colors = { FColor::White, FColor::White, FColor::White, FColor::White };
		S.Indices = { 0,1,2, 0,2,3 };
		S.Instances = { FTransform::Identity };
		return S;
	}

	FragmentMeshAuthoring::FAuthoringSection ToAuthoringSection(const FQuadSectionData& Data)
	{
		FragmentMeshAuthoring::FAuthoringSection Sec;
		Sec.MaterialSlot  = Data.MaterialSlot;
		Sec.Positions     = Data.Positions;
		Sec.Normals       = Data.Normals;
		Sec.TangentsX     = Data.TangentsX;
		Sec.TangentYSigns = Data.TangentYSigns;
		Sec.UV0           = Data.UV0;
		Sec.Colors        = Data.Colors;
		Sec.Indices       = Data.Indices;
		Sec.Instances     = Data.Instances;
		return Sec;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshAuthoringMultiSectionTest,
	"FragmentsUnreal.MeshAuthoring.AuthorMeshDescription_MultiSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuthoringMultiSectionTest::RunTest(const FString& Parameters)
{
	TArray<FQuadSectionData> SectionData = { MakeQuadSectionData(0), MakeQuadSectionData(2) };
	TArray<FragmentMeshAuthoring::FAuthoringSection> Sections;
	for (const FQuadSectionData& Data : SectionData)
	{
		Sections.Add(ToAuthoringSection(Data));
	}

	FMeshDescription MD;
	TArray<FVector3f> CollisionPos;
	TArray<uint32> CollisionIdx;
	const bool bOk = FragmentMeshAuthoring::AuthorMeshDescription(
		Sections, TEXT("CellMergeSlot_"), MD, CollisionPos, CollisionIdx);

	TestTrue(TEXT("authoring succeeded"), bOk);
	TestEqual(TEXT("polygon groups == used slots"), MD.PolygonGroups().Num(), 2);
	TestEqual(TEXT("triangles == 2 quads * 2"), MD.Triangles().Num(), 4);
	TestEqual(TEXT("vertices == 2 quads * 4"), MD.Vertices().Num(), 8);
	TestEqual(TEXT("collision positions == 8"), CollisionPos.Num(), 8);
	TestEqual(TEXT("collision indices == 12"), CollisionIdx.Num(), 12);

	// Collect slot names as a set so the assertion doesn't depend on iteration order.
	FStaticMeshAttributes Attributes(MD);
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	TSet<FString> FoundSlotNames;
	for (const FPolygonGroupID PGID : MD.PolygonGroups().GetElementIDs())
	{
		FoundSlotNames.Add(SlotNames[PGID].ToString());
	}
	TestEqual(TEXT("slot name set size"), FoundSlotNames.Num(), 2);
	TestTrue(TEXT("slot names contain CellMergeSlot_0"), FoundSlotNames.Contains(TEXT("CellMergeSlot_0")));
	TestTrue(TEXT("slot names contain CellMergeSlot_2"), FoundSlotNames.Contains(TEXT("CellMergeSlot_2")));

	// Collision index/position correspondence proves the per-instance CollisionBase offset is applied correctly.
	TestTrue(TEXT("collision pos[0] == quad base vertex"),
		CollisionPos[0].Equals(FVector3f(0, 0, 0), 0.01f));
	TestTrue(TEXT("collision pos[4] == second quad's base vertex"),
		CollisionPos[4].Equals(FVector3f(0, 0, 0), 0.01f));
	TestTrue(TEXT("collision pos[6] == second quad's opposite corner"),
		CollisionPos[6].Equals(FVector3f(100, 100, 0), 0.01f));

	const TArray<uint32> ExpectedFirstTri  = { 0, 1, 2, 0, 2, 3 };
	const TArray<uint32> ExpectedSecondTri = { 4, 5, 6, 4, 6, 7 };
	bool bFirstSixMatch = true, bSecondSixMatch = true;
	for (int32 i = 0; i < 6; ++i)
	{
		bFirstSixMatch  &= (CollisionIdx[i]     == ExpectedFirstTri[i]);
		bSecondSixMatch &= (CollisionIdx[6 + i] == ExpectedSecondTri[i]);
	}
	TestTrue(TEXT("collision indices [0..5] == first quad's local indices"), bFirstSixMatch);
	TestTrue(TEXT("collision indices [6..11] == second quad's indices offset by CollisionBase"), bSecondSixMatch);

	// Spot-check the first vertex instance's normal carries through the (identity) world transform.
	TVertexInstanceAttributesRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	FVertexInstanceID FirstVIID = INDEX_NONE;
	for (const FVertexInstanceID VIID : MD.VertexInstances().GetElementIDs())
	{
		FirstVIID = VIID;
		break;
	}
	TestTrue(TEXT("first vertex instance normal == (0,0,1)"),
		InstNormals[FirstVIID].Equals(FVector3f(0, 0, 1), 0.01f));
	TestTrue(TEXT("first vertex instance UV == (0,0)"),
		InstUVs.Get(FirstVIID, 0).Equals(FVector2f(0, 0), 0.01f));

	// Empty input returns false.
	FMeshDescription Empty;
	TArray<FVector3f> P; TArray<uint32> I;
	TestFalse(TEXT("empty sections -> false"),
		FragmentMeshAuthoring::AuthorMeshDescription({}, TEXT("CellMergeSlot_"), Empty, P, I));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshAuthoringDefaultTangentNoInstanceTest,
	"FragmentsUnreal.MeshAuthoring.AuthorMeshDescription_DefaultTangentNoInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuthoringDefaultTangentNoInstanceTest::RunTest(const FString& Parameters)
{
	// A single triangle with Positions/Normals/Colors/Indices populated, but no TangentsX/TangentYSigns/UV0/Instances — every fallback path should engage.
	FragmentMeshAuthoring::FAuthoringSection Sec;
	Sec.MaterialSlot = 0;

	TArray<FVector3f> Positions = { FVector3f(0,0,0), FVector3f(100,0,0), FVector3f(0,100,0) };
	TArray<FVector3f> Normals   = { FVector3f(0,0,1), FVector3f(0,0,1), FVector3f(0,0,1) };
	TArray<FColor>    Colors    = { FColor::White, FColor::White, FColor::White };
	TArray<uint32>    Indices   = { 0, 1, 2 };

	Sec.Positions = Positions;
	Sec.Normals   = Normals;
	Sec.Colors    = Colors;
	Sec.Indices   = Indices;
	// TangentsX, TangentYSigns, UV0, Instances all left as empty views deliberately.

	TArray<FragmentMeshAuthoring::FAuthoringSection> Sections = { Sec };

	FMeshDescription MD;
	TArray<FVector3f> CollisionPos;
	TArray<uint32> CollisionIdx;
	const bool bOk = FragmentMeshAuthoring::AuthorMeshDescription(
		Sections, TEXT("ProxySlot_"), MD, CollisionPos, CollisionIdx);

	TestTrue(TEXT("authoring succeeded"), bOk);
	TestEqual(TEXT("polygon groups == 1"), MD.PolygonGroups().Num(), 1);
	TestEqual(TEXT("triangles == 1"), MD.Triangles().Num(), 1);
	TestEqual(TEXT("vertices == 3"), MD.Vertices().Num(), 3);

	FStaticMeshAttributes Attributes(MD);
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	FName FoundSlotName;
	for (const FPolygonGroupID PGID : MD.PolygonGroups().GetElementIDs())
	{
		FoundSlotName = SlotNames[PGID];
		break;
	}
	TestEqual(TEXT("slot name == ProxySlot_0"), FoundSlotName.ToString(), FString(TEXT("ProxySlot_0")));

	TVertexInstanceAttributesRef<FVector3f> InstTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> InstBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();
	FVertexInstanceID FirstVIID = INDEX_NONE;
	for (const FVertexInstanceID VIID : MD.VertexInstances().GetElementIDs())
	{
		FirstVIID = VIID;
		break;
	}
	TestTrue(TEXT("first vertex instance tangent == default (1,0,0)"),
		InstTangents[FirstVIID].Equals(FVector3f(1, 0, 0), 0.01f));
	TestTrue(TEXT("first vertex instance binormal sign == default +1"),
		FMath::IsNearlyEqual(InstBinormalSigns[FirstVIID], 1.0f, 0.01f));
	TestTrue(TEXT("first vertex instance UV == default (0,0)"),
		InstUVs.Get(FirstVIID, 0).Equals(FVector2f(0, 0), 0.01f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
