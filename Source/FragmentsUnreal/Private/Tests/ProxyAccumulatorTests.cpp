// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Utils/FragmentsUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

// Preserve source UVs and transform tangents in LOD2 proxies.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyAccumulatorCarriesSourceUVsTest,
	"FragmentsUnreal.ProxyAccumulator.CarriesSourceUVsAndWorldTangents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyAccumulatorCarriesSourceUVsTest::RunTest(const FString& Parameters)
{
	// Source element in LOCAL space: one triangle, +Z normals, +X tangent, known UVs.
	FFragmentMeshData Src;
	Src.bIsValid = true;
	auto MakeV = [](FVector3f P, FVector2f UV)
	{
		FFragmentMeshVertex V;
		V.Position     = P;
		V.Normal       = FVector3f(0, 0, 1);
		V.TangentX     = FVector3f(1, 0, 0);
		V.TangentYSign = 1.0f;
		V.UV0          = UV;
		V.Color        = FColor::White;
		return V;
	};
	Src.Vertices = {
		MakeV(FVector3f(0, 0, 0),   FVector2f(0.10f, 0.20f)),
		MakeV(FVector3f(100, 0, 0), FVector2f(0.30f, 0.40f)),
		MakeV(FVector3f(0, 100, 0), FVector2f(0.50f, 0.60f)),
	};
	Src.Indices = { 0, 1, 2 };

	// A 90-degree yaw so world space differs from local space — this is exactly where a world-space UV recompute would diverge from LOD1, but a CARRIED UV must not.
	const FTransform Xform(FRotator(0.0f, 90.0f, 0.0f), FVector(500.0, 200.0, 0.0));

	FProxyMeshAccumulator Acc;
	Acc.AddGeometry(Src, Xform, /*MaterialCRC*/ 1, /*Material*/ nullptr);

	const FProxyMeshSection* Section = Acc.Sections.Find(1);
	TestNotNull(TEXT("section created"), Section);
	if (!Section) { return false; }

	TestEqual(TEXT("UV count == verts"),      Section->UV0.Num(),          3);
	TestEqual(TEXT("tangent count == verts"), Section->TangentsX.Num(),    3);
	TestEqual(TEXT("sign count == verts"),    Section->TangentYSigns.Num(),3);

	for (int32 i = 0; i < 3; ++i)
	{
		// UVs carried untransformed == source (the LOD1-matching behavior).
		TestTrue(FString::Printf(TEXT("UV[%d] carried from source (untransformed)"), i),
			Section->UV0[i].Equals(Src.Vertices[i].UV0, 1e-4f));

		const FVector3f ExpectedPos = FVector3f(Xform.TransformPosition(FVector(Src.Vertices[i].Position)));
		TestTrue(FString::Printf(TEXT("pos[%d] world-transformed"), i),
			Section->Positions[i].Equals(ExpectedPos, 0.01f));

		const FVector3f ExpectedTan = FVector3f(Xform.TransformVectorNoScale(FVector(Src.Vertices[i].TangentX)));
		TestTrue(FString::Printf(TEXT("tangent[%d] world-transformed"), i),
			Section->TangentsX[i].Equals(ExpectedTan, 0.01f));

		TestTrue(FString::Printf(TEXT("sign[%d] carried"), i),
			FMath::IsNearlyEqual(Section->TangentYSigns[i], Src.Vertices[i].TangentYSign, 1e-4f));
	}

	// Discriminating: source tangent (1,0,0) yawed 90 deg -> world (0,1,0), so the stored tangent must differ from the raw source — proving tangents are transformed, not copied raw.
	TestTrue(TEXT("world tangent != raw source tangent under rotation"),
		!Section->TangentsX[0].Equals(Src.Vertices[0].TangentX, 0.01f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
