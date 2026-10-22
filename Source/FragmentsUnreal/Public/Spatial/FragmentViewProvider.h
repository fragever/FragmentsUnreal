// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentRegistrationManager.h"   // FFragmentViewInfo

namespace UE::Fragments::ViewProvider
{
    /** Bound by FragmentsEditor at module startup. */
    DECLARE_DELEGATE_TwoParams(FGatherViews, UWorld* /*World*/, TArray<FFragmentViewInfo>& /*OutViews*/);

    FRAGMENTSUNREAL_API FGatherViews& Get();

    /** Appends one view per eligible viewport. */
    FRAGMENTSUNREAL_API void Gather(UWorld* World, TArray<FFragmentViewInfo>& OutViews);
}
