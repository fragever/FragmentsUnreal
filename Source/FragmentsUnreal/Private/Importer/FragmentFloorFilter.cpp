// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Importer/FragmentFloorFilter.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Misc/EngineVersionComparison.h"
#include "Templates/Function.h"

namespace UE::Fragments::FloorFilter
{
	float SlotValue(const int16* Floor, int32 MinFloor, int32 MaxFloor)
	{
		const int32 F = (Floor && *Floor >= 0) ? static_cast<int32>(*Floor) : 0;
		return (F < MinFloor || F > MaxFloor) ? 1.0f : 0.0f;
	}

	int32 WriteSlot(UInstancedStaticMeshComponent& Bucket, int32 Count, TFunctionRef<float(int32)> ValueFor)
	{
		const int32 Stride = Bucket.NumCustomDataFloats;
		if (Stride <= HiddenSlot)
		{
			return 0;
		}
		Count = FMath::Min(Count, Bucket.GetInstanceCount());
		int32 Changed = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			const int32 Index = i * Stride + HiddenSlot;
			if (!Bucket.PerInstanceSMCustomData.IsValidIndex(Index))
			{
				break;
			}
			const float Want = ValueFor(i);
			if (Bucket.PerInstanceSMCustomData[Index] != Want)
			{
				Bucket.SetCustomDataValue(i, HiddenSlot, Want, /*bMarkRenderStateDirty=*/false);
				++Changed;
			}
		}
#if UE_VERSION_OLDER_THAN(5, 4, 0)
		// No instance data manager before 5.4: the proxy only sees custom data when it is re-created.
		if (Changed > 0)
		{
			Bucket.MarkRenderStateDirty();
		}
#endif
		return Changed;
	}

	int32 ApplyToBucket(UInstancedStaticMeshComponent& Bucket, TConstArrayView<int64> LocalIds,
		const TMap<int64, int16>& LocalIdToFloor, int32 MinFloor, int32 MaxFloor)
	{
		// The floor filter owns "hidden by floor" (1) only: a cut floor becomes 1, a floor coming back goes from 1 to 0,
		// and any other value on a showing floor (a category dim) is left as it is.
		const int32 Stride = Bucket.NumCustomDataFloats;
		return WriteSlot(Bucket, LocalIds.Num(), [&](int32 i)
		{
			const float Hidden = SlotValue(LocalIdToFloor.Find(LocalIds[i]), MinFloor, MaxFloor);
			const float Current = Bucket.PerInstanceSMCustomData[i * Stride + HiddenSlot];
			return Hidden > 0.f ? Hidden : (Current >= 1.f ? 0.f : Current);
		});
	}
}
