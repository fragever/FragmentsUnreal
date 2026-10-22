// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentRegistrationManager.h"   // FFragmentViewInfo

namespace FragmentsEditorViewProvider
{
    /** One FFragmentViewInfo per open, visible, PERSPECTIVE level viewport whose world is World. */
    void GatherFromLevelViewports(UWorld* World, TArray<FFragmentViewInfo>& OutViews);
}
