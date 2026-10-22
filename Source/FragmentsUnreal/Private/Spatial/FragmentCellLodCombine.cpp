// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentCellLodCombine.h"

namespace UE::Fragments::CellLod
{
    bool CombineTargetStates(TConstArrayView<ECellLodState> PerViewStates, ECellLodState& OutCombined)
    {
        if (PerViewStates.IsEmpty())
        {
            return false;
        }

        // MIN, not max -- the enum runs Active_LOD0=0, Merged_LOD1=1, Skipped=2, so the numerically smallest value is the most detailed one.
        uint8 Best = static_cast<uint8>(PerViewStates[0]);
        for (const ECellLodState S : PerViewStates)
        {
            Best = FMath::Min(Best, static_cast<uint8>(S));
        }
        OutCombined = static_cast<ECellLodState>(Best);
        return true;
    }

    ECellLodState ResolveDistanceBand(ECellLodState Current, double DistCm, float LowerCm, float UpperCm)
    {
        if (DistCm > UpperCm) { return ECellLodState::Merged_LOD1; }
        if (DistCm < LowerCm) { return ECellLodState::Active_LOD0; }
        // Hysteresis holds only between the two visible states.
        if (Current == ECellLodState::Active_LOD0 || Current == ECellLodState::Merged_LOD1) { return Current; }
        return DistCm < 0.5 * (static_cast<double>(LowerCm) + UpperCm)
            ? ECellLodState::Active_LOD0 : ECellLodState::Merged_LOD1;
    }

    ECellLodState StateWhenMergeSettles(ECellLodState Current)
    {
        return Current == ECellLodState::Merged_LOD1 ? ECellLodState::Skipped : Current;
    }
}
