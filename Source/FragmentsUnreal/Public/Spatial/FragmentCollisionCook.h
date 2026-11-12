// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"

/** The registration manager's collision cook / uncook decisions, kept pure so they are tested without a world. */
namespace UE::Fragments::CollisionCook
{
	/** What the uncook check does for a cooked model this pass. */
	enum class EUncookStep : uint8
	{
		None,           // nothing to do
		StartCooldown,  // the camera just left the uncook distance: start the cooldown
		Uncook,         // the cooldown ran out: drop the model's collision
		Cancel          // a pending uncook no longer applies (camera came back, or the model is promoted)
	};

	/**
	 * The uncook decision for a model whose collision is cooked. A promoted model (PromoteAllCellsForModel: it is being
	 * inspected and must stay clickable at any distance) is never uncooked; a pending cooldown is cancelled instead.
	 * EligibleTime is 0 when no cooldown is running.
	 */
	FRAGMENTSUNREAL_API EUncookStep NextUncookStep(bool bPromoted, float DistToBoxCm, float UncookPaddingCm,
		float EligibleTime, float NowSeconds);

	/**
	 * Whether a HISMC has a physics body behind it, so turning its collision on is safe: the model is cooked, the chunked
	 * cook has passed it, or a promotion forced (and cooked) it.
	 */
	FRAGMENTSUNREAL_API bool HasCookedBody(bool bModelCooked, bool bChunkCookPassedIt, bool bForceCollision);
}
