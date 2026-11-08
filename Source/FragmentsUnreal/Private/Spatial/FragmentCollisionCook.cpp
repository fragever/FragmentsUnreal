// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Spatial/FragmentCollisionCook.h"

namespace UE::Fragments::CollisionCook
{
	EUncookStep NextUncookStep(bool bPromoted, float DistToBoxCm, float UncookPaddingCm, float EligibleTime, float NowSeconds)
	{
		const bool bPending = EligibleTime != 0.0f;
		if (bPromoted || DistToBoxCm <= UncookPaddingCm)
		{
			return bPending ? EUncookStep::Cancel : EUncookStep::None;
		}
		if (!bPending)
		{
			return EUncookStep::StartCooldown;
		}
		return NowSeconds >= EligibleTime ? EUncookStep::Uncook : EUncookStep::None;
	}

	bool HasCookedBody(bool bModelCooked, bool bChunkCookPassedIt, bool bForceCollision)
	{
		return bModelCooked || bChunkCookPassedIt || bForceCollision;
	}
}
