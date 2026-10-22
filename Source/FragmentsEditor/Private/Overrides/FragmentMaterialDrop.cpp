// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Overrides/FragmentMaterialDrop.h"

#include "AssetRegistry/AssetData.h"
#include "Materials/MaterialInterface.h"

bool UE::Fragments::Overrides::IsAcceptableOverrideDrop(TConstArrayView<FAssetData> Assets)
{
	// One row takes one override, so several assets are refused rather than taking the first.
	return Assets.Num() == 1 && Assets[0].IsInstanceOf<UMaterialInterface>();
}
