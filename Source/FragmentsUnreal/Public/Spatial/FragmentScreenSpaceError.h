// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"

/** Pure screen-space-error (SSE) math for the LOD refinement tree. */
namespace FragmentSSE
{
	/** Geometric error (cm) for a tier: coefficient x node bounding-sphere radius. */
	inline float GeoError(float Coeff, float RadiusCm)
	{
		return Coeff * RadiusCm;
	}

	/** Per-frame view constant: pixels of screen error per (cm geoError / cm distance). */
	inline float SseCoeff(float ViewportHeightPx, float ProjMatrix11)
	{
		return ViewportHeightPx * ProjMatrix11 * 0.5f;
	}

	/** Screen-space error in pixels for a node. */
	inline float ScreenErrorPx(float GeoErrorCm, float DistanceCm, float Coeff)
	{
		if (DistanceCm <= 1.0f)
		{
			return TNumericLimits<float>::Max();
		}
		return GeoErrorCm * Coeff / DistanceCm;
	}

	/** Return refine, coarsen, or hold from screen error and hysteresis thresholds. */
	inline int32 RefineDecision(float SsePx, float ThresholdPx, float CoarsenFactor)
	{
		if (SsePx > ThresholdPx)
		{
			return 1;
		}
		if (SsePx < ThresholdPx * CoarsenFactor)
		{
			return -1;
		}
		return 0;
	}
}
