// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentRegistrationManager.h"   // ECellLodState

namespace UE::Fragments::CellLod
{
    /** Combine one cell's per-view target states into the single state to apply. */
    bool CombineTargetStates(TConstArrayView<ECellLodState> PerViewStates, ECellLodState& OutCombined);

    /** Distance gate for an in-frustum cell that has a merged mesh: LOD1 beyond UpperCm, LOD0 inside LowerCm. */
    FRAGMENTSUNREAL_API ECellLodState ResolveDistanceBand(
        ECellLodState Current, double DistCm, float LowerCm, float UpperCm);

    /** A cell's state once its merge settles (finished or failed). */
    FRAGMENTSUNREAL_API ECellLodState StateWhenMergeSettles(ECellLodState Current);
}
