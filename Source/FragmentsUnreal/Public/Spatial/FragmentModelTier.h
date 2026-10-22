// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentScreenSpaceError.h"

/** Model-level LOD tier: is a building represented by its LOD2 whole-building proxy, or by its LOD1/LOD0 cells? */
namespace FragmentModelTier
{
	enum class ETier : uint8
	{
		/** Only the whole-building LOD2 proxy is shown. Cells are hidden; the proxy casts shadows. */
		Proxy,
		/** LOD1 merged cells (and LOD0 HISMCs within them) are shown. */
		Cells,
	};

	/** Everything the decision needs. */
	struct FDecisionInputs
	{
		float ModelBoundsRadiusCm = 0.0f;
		/** Distance from the camera to the model's AABB, not to its centre. */
		float DistToBoxCm = 0.0f;
		/** FFragmentViewInfo::SseCoeff for the live view. */
		float SseCoeff = 0.0f;
		float GeoCoeff = 0.0f;
		float ThresholdPx = 0.0f;
		float CoarsenFactor = 0.0f;

		/** Any cell of this model hidden by a floor filter. */
		bool bAnyCellFloorFiltered = false;

		/** Click override (see UFragmentRegistrationManager::PromoteModelFromProxyHit). */
		bool bPromoteOverride = false;

		/** False when this model has no proxy component (not built yet, or bGenerateProxyMesh off). */
		bool bProxyAvailable = true;

		/** Tier held right now. Returned unchanged when the SSE decision lands inside the band. */
		ETier Current = ETier::Proxy;
	};

	/** Both overrides force Cells. There is deliberately no override that forces Proxy. */
	inline ETier Decide(const FDecisionInputs& In)
	{
		if (In.bAnyCellFloorFiltered || In.bPromoteOverride || !In.bProxyAvailable)
		{
			return ETier::Cells;
		}

		const float GeoErr = FragmentSSE::GeoError(In.GeoCoeff, In.ModelBoundsRadiusCm);
		const float SsePx  = FragmentSSE::ScreenErrorPx(GeoErr, In.DistToBoxCm, In.SseCoeff);

		const int32 Dec = FragmentSSE::RefineDecision(SsePx, In.ThresholdPx, In.CoarsenFactor);
		if (Dec > 0) { return ETier::Cells; }
		if (Dec < 0) { return ETier::Proxy; }
		return In.Current;   // inside the hysteresis band
	}

	/** Distance at which a model of this radius demotes Cells -> Proxy, i.e. where ScreenErrorPx equals ThresholdPx * CoarsenFactor. */
	inline float DemoteDistanceCm(
		float GeoCoeff, float ModelBoundsRadiusCm, float ViewCoeff,
		float ThresholdPx, float CoarsenFactor)
	{
		const float Denom = ThresholdPx * CoarsenFactor;
		if (Denom <= 0.0f) { return 0.0f; }
		return GeoCoeff * ModelBoundsRadiusCm * ViewCoeff / Denom;
	}

	/** Shadow ownership: the coarsest RESIDENT representation casts, and exactly one does. */
	inline bool ShouldProxyCast(ETier Tier) { return Tier == ETier::Proxy; }
	inline bool ShouldCellsCast(ETier Tier) { return Tier == ETier::Cells; }
}
