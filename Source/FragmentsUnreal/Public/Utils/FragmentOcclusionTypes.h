// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "FragmentOcclusionTypes.generated.h"

/** Occlusion role classification for fragments. */
UENUM(BlueprintType)
enum class EOcclusionRole : uint8
{
	/** Large structural elements that block visibility (walls, floors, roofs) */
	Occluder,

	/** Objects that can be hidden by occluders (furniture, doors) */
	Occludee,

	/** Transparent materials that never block anything (glass, windows) */
	NonOccluder
};
