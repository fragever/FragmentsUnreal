// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"

struct FAssetData;

namespace UE::Fragments::Overrides
{
	/** Whether a drag can be dropped onto a material row to become its override: exactly one asset, and it is a material or material instance. */
	FRAGMENTSEDITOR_API bool IsAcceptableOverrideDrop(TConstArrayView<FAssetData> Assets);
}
