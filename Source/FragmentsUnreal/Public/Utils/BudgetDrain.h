// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once
#include "CoreMinimal.h"
#include "Utils/FrameBudgetCoordinator.h"

namespace FragmentBudgetDrain
{
	/** Drain items [Cursor, Count) by calling DoWork(index) for each, stopping once the per-frame budget is exhausted. */
	template<typename FDoWork>
	bool DrainBudgeted(int32& Cursor, int32 Count, FFrameBudgetCoordinator& Budget,
	                   uint64 FrameNumber, FDoWork&& DoWork)
	{
		int32 Processed = 0;
		while (Cursor < Count)
		{
			if (Processed > 0 && Budget.AllocateRemainingMs(FrameNumber) <= 0.0f)
			{
				break;
			}
			DoWork(Cursor);
			++Cursor;
			++Processed;
		}
		return Cursor >= Count;
	}
}
