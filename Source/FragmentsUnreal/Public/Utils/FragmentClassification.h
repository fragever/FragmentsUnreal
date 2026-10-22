// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// Shared classifier helpers for fragment elements.

#pragma once

#include "CoreMinimal.h"
#include "Utils/FragmentsUtils.h"

namespace FragmentClassification
{
	/** True for IFCPLATE buckets that hold transparent glass infill panels. */
	inline bool IsOccludableGlass(const FPendingHISMItem& Item)
	{
		return Item.bIsGlass
			&& Item.Category.Equals(TEXT("IFCPLATE"), ESearchCase::IgnoreCase);
	}
}
