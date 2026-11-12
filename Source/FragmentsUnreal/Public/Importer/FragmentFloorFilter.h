// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "Templates/FunctionFwd.h"

class UInstancedStaticMeshComponent;

/** The floor and category filters' per-instance pass (SetFloorVisibilityRange, SetCategoryFilter). */
namespace UE::Fragments::FloorFilter
{
	/** Custom data slot the fragment materials read as the filter's ghost value: 0 shown, 1 hidden, between = dimmed. */
	constexpr int32 HiddenSlot = 1;

	/** Slot value for an element on *Floor (null or negative: floor 0) with floors MinFloor..MaxFloor showing. */
	FRAGMENTSUNREAL_API float SlotValue(const int16* Floor, int32 MinFloor, int32 MaxFloor);

	/**
	 * Writes the filter slot of one bucket's instances 0..Count-1 (ValueFor(i) is instance i's new value), only where the
	 * value changes, and returns how many changed. From UE 5.4 a custom data write reaches the renderer as an instance
	 * update (the component's render instances are marked dirty), so the bucket's render state is never rebuilt:
	 * rebuilding it for every bucket re-created every piece of a large building (375 Park: 6,951) on each filter change.
	 * Before 5.4 a bucket that changed is marked render-state dirty, as all of them used to be.
	 */
	FRAGMENTSUNREAL_API int32 WriteSlot(UInstancedStaticMeshComponent& Bucket, int32 Count, TFunctionRef<float(int32)> ValueFor);

	/**
	 * The floor filter on one bucket (LocalIds[i] is instance i). It owns "hidden by floor" only: an element on a cut
	 * floor becomes 1, one on a floor coming back goes from 1 to 0, and any other value on a showing floor (a category
	 * dim from SetCategoryFilter or the app) is kept, so the two filters no longer undo each other.
	 */
	FRAGMENTSUNREAL_API int32 ApplyToBucket(UInstancedStaticMeshComponent& Bucket, TConstArrayView<int64> LocalIds,
		const TMap<int64, int16>& LocalIdToFloor, int32 MinFloor, int32 MaxFloor);
}
