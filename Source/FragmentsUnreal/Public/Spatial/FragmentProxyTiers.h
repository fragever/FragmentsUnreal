// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentScreenSpaceError.h"

struct FFragmentMeshData;

/** LOD2 proxy tiers. */
namespace FragmentProxyTiers
{
	/** Reference view the tier cutoff derivation is calibrated against. */
	inline constexpr float kRefViewportHeightPx = 1080.0f;
	inline constexpr float kRefProjM11 = 1.778f;   // 1/tan(fovY/2), 90deg horizontal at 16:9

	/** Sentinel FTierSpec::ScreenSize set by BuildProxyTierSpecs (which is bounds-free and cannot compute a real value) and overwritten by FillProxyTierScreenSizes once accumulated proxy bounds exist. */
	inline constexpr float kUnresolvedScreenSize = -1.0f;

	/** One proxy LOD. Tier 0 is the nearest / most detailed. */
	struct FTierSpec
	{
		/** Elements with a smaller world bounding radius are omitted from this tier. */
		float MinRadiusCm = 0.0f;
		/** Kept-triangle fraction for survivors. 1.0 = no simplification. */
		float SimplifyRatio = 1.0f;
		/** Written to FStaticMeshRenderData::ScreenSize[i].Default. */
		float ScreenSize = kUnresolvedScreenSize;
	};

	/** Pixels of screen size per (cm radius / cm distance) for a reference view. */
	inline float ViewCoeff(float ViewportHeightPx, float ProjMatrix11)
	{
		return FragmentSSE::SseCoeff(ViewportHeightPx, ProjMatrix11);
	}

	/** World radius below which an element projects to fewer than OmitPx pixels at NearEdgeCm. */
	inline float RadiusCutoffCm(float OmitPx, float NearEdgeCm, float Coeff)
	{
		return (Coeff > 0.0f) ? (OmitPx * NearEdgeCm / Coeff) : 0.0f;
	}

	/** True if cutoffs are non-decreasing, i.e. tier N+1's element set is a subset of tier N's. */
	inline bool AreTiersNested(TConstArrayView<FTierSpec> Tiers)
	{
		for (int32 i = 1; i < Tiers.Num(); ++i)
		{
			if (Tiers[i].MinRadiusCm < Tiers[i - 1].MinRadiusCm) { return false; }
		}
		return true;
	}

	/** Local bounding-sphere radius (half the AABB diagonal) over a bare position array. */
	FRAGMENTSUNREAL_API float LocalRadiusCm(TConstArrayView<FVector3f> Positions);

	/** Local bounding-sphere radius (half the AABB diagonal) of an element's source geometry. */
	FRAGMENTSUNREAL_API float LocalRadiusCm(const FFragmentMeshData& MeshData);

	/** Scale a cached local radius into world space. */
	inline float WorldRadiusCm(float LocalRadius, const FTransform& Xform)
	{
		return LocalRadius * static_cast<float>(Xform.GetScale3D().GetAbsMax());
	}
}
