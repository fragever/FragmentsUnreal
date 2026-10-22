// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"

/** Placement arithmetic for a chunked-import model, whose placement is BAKED into its instance transforms at import while its actor stays at identity. */
namespace UE::Fragments::Placement
{
	/** The absolute placement to save, so that the next import (which bakes it) puts the model exactly where it stands now: Saved * OriginFrame == BakedFrame * ActorDelta. */
	FRAGMENTSEDITOR_API FTransform ComposeSavedPlacement(
		const FTransform& BakedFrame, const FTransform& BakedPlacement, const FTransform& ActorDelta);

	/** The actor delta that shows the model where an import with NO placement would put it: BakedFrame * Result == OriginFrame. */
	FRAGMENTSEDITOR_API FTransform ActorDeltaForNoPlacement(
		const FTransform& BakedFrame, const FTransform& BakedPlacement);

	/** The actor delta that shows the model where an import with placement P would put it: BakedFrame * Result == P * OriginFrame. */
	FRAGMENTSEDITOR_API FTransform ActorDeltaForPlacement(
		const FTransform& BakedFrame, const FTransform& BakedPlacement, const FTransform& P);

	/** Clipboard text for the panel's Shift+right-click copy, in the same format the Details panel copies a Location / Rotation property with, so values move freely between the two. */
	FRAGMENTSEDITOR_API FString FormatLocationForClipboard(const FVector& Location);
	FRAGMENTSEDITOR_API FString FormatRotationForClipboard(const FRotator& Rotation);

	/** Parse a pasted Location / Rotation. */
	FRAGMENTSEDITOR_API bool ParseLocationFromClipboard(const FString& Text, FVector& Out);
	FRAGMENTSEDITOR_API bool ParseRotationFromClipboard(const FString& Text, FRotator& Out);
}
