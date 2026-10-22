// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentProxyDecimate.h"

#if WITH_DEV_AUTOMATION_TESTS

// Guards the decimate step: simplification must REDUCE triangles AND preserve UV0 + tangents (not fall back to (0,0)/(1,0,0)), because the proxy carries textured-override material fidelity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimatePreservesAttributesTest,
	"FragmentsUnreal.ProxyDecimate.PreservesUVsAndTangents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimatePreservesAttributesTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	// Build an N x N vertex grid (planar, +Z normal), two triangles per cell.
	const int32 N = 12;                       // 11*11*2 = 242 triangles
	FDecimateSection S;
	const FVector3f Normal(0, 0, 1);
	const FVector3f Tangent(0, 1, 0);         // deliberately NOT the (1,0,0) fallback
	for (int32 y = 0; y < N; ++y)
	{
		for (int32 x = 0; x < N; ++x)
		{
			S.Positions.Add(FVector3f(x * 100.0f, y * 100.0f, 0.0f));
			S.Normals.Add(Normal);
			S.TangentsX.Add(Tangent);
			S.TangentYSigns.Add(1.0f);
			S.UV0.Add(FVector2f(x / float(N - 1), y / float(N - 1)));  // non-zero, varying
			S.VertexColors.Add(FColor(10, 20, 30, 255));
		}
	}
	auto VID = [N](int32 x, int32 y) { return (uint32)(y * N + x); };
	for (int32 y = 0; y < N - 1; ++y)
	{
		for (int32 x = 0; x < N - 1; ++x)
		{
			S.Indices.Add(VID(x, y));     S.Indices.Add(VID(x + 1, y)); S.Indices.Add(VID(x + 1, y + 1));
			S.Indices.Add(VID(x, y));     S.Indices.Add(VID(x + 1, y + 1)); S.Indices.Add(VID(x, y + 1));
		}
	}

	const int32 OrigTris = S.Indices.Num() / 3;
	TestTrue(TEXT("test fixture has triangles"), OrigTris > 100);

	const bool bRan = DecimateSection(S, 0.5f);
	TestTrue(TEXT("decimation ran"), bRan);

	const int32 NewTris = S.Indices.Num() / 3;
	// Reduced, and roughly the requested fraction (allow slack for boundary constraints).
	TestTrue(FString::Printf(TEXT("triangles reduced (%d -> %d)"), OrigTris, NewTris),
		NewTris < OrigTris);
	TestTrue(TEXT("reduction near target ratio"),
		NewTris <= (int32)(OrigTris * 0.75f) && NewTris >= 4);

	// Parallel arrays stay consistent.
	TestEqual(TEXT("normals count == positions"),  S.Normals.Num(),       S.Positions.Num());
	TestEqual(TEXT("tangents count == positions"), S.TangentsX.Num(),     S.Positions.Num());
	TestEqual(TEXT("signs count == positions"),    S.TangentYSigns.Num(), S.Positions.Num());
	TestEqual(TEXT("uv count == positions"),       S.UV0.Num(),           S.Positions.Num());
	TestEqual(TEXT("color count == positions"),    S.VertexColors.Num(),  S.Positions.Num());

	// Attributes survived: at least one UV is non-zero and at least one tangent is not (1,0,0).
	bool bAnyNonZeroUV = false;
	for (const FVector2f& UV : S.UV0)
	{
		if (!UV.Equals(FVector2f::ZeroVector, 1e-4f)) { bAnyNonZeroUV = true; break; }
	}
	TestTrue(TEXT("UV0 preserved (not reset to (0,0))"), bAnyNonZeroUV);

	bool bAnyNonFallbackTangent = false;
	for (const FVector3f& T : S.TangentsX)
	{
		if (!T.Equals(FVector3f(1, 0, 0), 1e-3f)) { bAnyNonFallbackTangent = true; break; }
	}
	TestTrue(TEXT("tangents preserved (not reset to (1,0,0))"), bAnyNonFallbackTangent);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimateResolveSettingsTest,
	"FragmentsUnreal.ProxyDecimate.ResolveSettingsPrecedence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimateResolveSettingsTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	// Sentinel CVars (-1) defer to the UPROPERTY.
	{
		const FDecimateSettings R = ResolveSettings(/*CVarEnable*/ -1, /*CVarRatio*/ -1.0f,
			/*PropEnabled*/ true, /*PropRatio*/ 0.5f);
		TestTrue(TEXT("defer: enabled from prop"), R.bEnabled);
		TestTrue(TEXT("defer: ratio from prop"), FMath::IsNearlyEqual(R.Ratio, 0.5f));
	}
	// CVar enable=0 overrides prop true -> disabled.
	{
		const FDecimateSettings R = ResolveSettings(0, -1.0f, true, 0.5f);
		TestFalse(TEXT("cvar disables"), R.bEnabled);
	}
	// CVar enable=1 overrides prop false -> enabled.
	{
		const FDecimateSettings R = ResolveSettings(1, -1.0f, false, 0.5f);
		TestTrue(TEXT("cvar enables"), R.bEnabled);
	}
	// CVar ratio overrides prop ratio, and is clamped to [0.05, 1.0].
	{
		const FDecimateSettings R = ResolveSettings(-1, 0.25f, true, 0.5f);
		TestTrue(TEXT("cvar ratio override"), FMath::IsNearlyEqual(R.Ratio, 0.25f));
	}
	{
		const FDecimateSettings R = ResolveSettings(-1, 5.0f, true, 0.5f);  // out of range high
		TestTrue(TEXT("ratio clamped high"), FMath::IsNearlyEqual(R.Ratio, 1.0f));
	}
	{
		const FDecimateSettings R = ResolveSettings(-1, 0.001f, true, 0.5f); // out of range low
		TestTrue(TEXT("ratio clamped low"), FMath::IsNearlyEqual(R.Ratio, 0.05f));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
