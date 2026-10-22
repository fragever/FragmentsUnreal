// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentViewProvider.h"

namespace UE::Fragments::ViewProvider
{
    FGatherViews& Get()
    {
        static FGatherViews Delegate;
        return Delegate;
    }

    void Gather(UWorld* World, TArray<FFragmentViewInfo>& OutViews)
    {
        if (!World) { return; }
        if (Get().IsBound())
        {
            Get().Execute(World, OutViews);
        }
    }
}
