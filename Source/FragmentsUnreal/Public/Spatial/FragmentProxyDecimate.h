// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once
#include "CoreMinimal.h"

namespace FragmentProxyDecimate
{
	/** One proxy material section as worker-safe POD. */
	struct FDecimateSection
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector3f> TangentsX;
		TArray<float>     TangentYSigns;
		TArray<FVector2f> UV0;
		TArray<FColor>    VertexColors;
		TArray<uint32>    Indices;
	};

	/** Resolved effective decimation settings. */
	struct FDecimateSettings
	{
		bool  bEnabled = false;
		float Ratio    = 0.5f;
	};

	/** Default coincident-vertex weld distance, in cm. */
	inline constexpr float DefaultWeldToleranceCm = 0.0f;

	/** Decimate one section in place to ~Ratio of its triangles, preserving UV0 + tangents (and normals + vertex colors) via FDynamicMesh3 attribute overlays and the attribute-aware simplifier. */
	FRAGMENTSUNREAL_API bool DecimateSection(
		FDecimateSection& InOut, float Ratio, float WeldToleranceCm = DefaultWeldToleranceCm);

	/** Resolve effective decimation settings from CVar overrides and UPROPERTY defaults. */
	FRAGMENTSUNREAL_API FDecimateSettings ResolveSettings(
		int32 CVarEnable, float CVarRatio, bool PropEnabled, float PropRatio);
}
