// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Utils/FragmentsLog.h"

/** Result of a budget allocation request. */
struct FBudgetAllocationResult
{
	/** Whether any budget was allocated */
	bool bHasBudget = false;

	/** Allocated budget in milliseconds */
	float BudgetMs = 0.0f;

	FBudgetAllocationResult() = default;
	FBudgetAllocationResult(bool bInHasBudget, float InBudgetMs)
		: bHasBudget(bInHasBudget), BudgetMs(InBudgetMs) {}
};

/** Coordinates frame time budget across geometry processing and spawn work. */
struct FFrameBudgetCoordinator
{
	/** Total frame time budget in milliseconds (default: 4ms for 60 FPS with headroom) */
	float TotalFrameBudgetMs = 4.0f;

	/** Enable adaptive budget adjustment based on actual frame times */
	bool bEnableAdaptiveBudget = true;

	/** Begin a new frame. */
	void BeginFrame()
	{
		FrameStartTime = FPlatformTime::Seconds();
		bInFrame = true;
	}

	/** End the current frame and update adaptive history. */
	void EndFrame()
	{
		if (!bInFrame) return;

		const float ActualFrameTimeMs = static_cast<float>((FPlatformTime::Seconds() - FrameStartTime) * 1000.0);

		// Update rolling average for adaptive budgeting
		if (bEnableAdaptiveBudget)
		{
			FrameTimeHistory[FrameHistoryIndex] = ActualFrameTimeMs;
			FrameHistoryIndex = (FrameHistoryIndex + 1) % FrameHistorySize;
			FrameHistoryCount = FMath::Min(FrameHistoryCount + 1, FrameHistorySize);
		}

		FramesSinceLastLog++;
		const double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLogTime >= 5.0)
		{
			LogBudgetStatistics(ActualFrameTimeMs);
			LastLogTime = CurrentTime;
			FramesSinceLastLog = 0;
		}

		bInFrame = false;
	}

	/** Get average frame time from history (for adaptive budgeting). */
	float GetAverageFrameTimeMs() const
	{
		if (FrameHistoryCount == 0)
		{
			return TotalFrameBudgetMs;
		}

		float Sum = 0.0f;
		for (int32 i = 0; i < FrameHistoryCount; i++)
		{
			Sum += FrameTimeHistory[i];
		}
		return Sum / static_cast<float>(FrameHistoryCount);
	}

	/** Remaining budget this frame (production: current engine frame). */
	float AllocateRemainingMs() { return AllocateRemainingMs(GFrameCounter); }

	/** Remaining budget for an explicit frame number (unit-test seam). */
	float AllocateRemainingMs(uint64 FrameNumber)
	{
		RollFrameIfNeeded(FrameNumber);
		return FMath::Max(0.0f, TotalFrameBudgetMs - SpentThisFrameMs);
	}

	/** Record spent game-thread work against this frame (production). */
	void RecordWork(float Ms) { RecordWork(Ms, GFrameCounter); }

	/** Record spent work for an explicit frame number (unit-test seam). */
	void RecordWork(float Ms, uint64 FrameNumber)
	{
		RollFrameIfNeeded(FrameNumber);
		SpentThisFrameMs += Ms;
	}

private:
	/** Frame start time in seconds */
	double FrameStartTime = 0.0;

	/** Whether we're currently in a frame */
	bool bInFrame = false;

	/** Frame time history for adaptive budgeting */
	static constexpr int32 FrameHistorySize = 60; // 1 second at 60 FPS
	float FrameTimeHistory[FrameHistorySize] = {0.0f};
	int32 FrameHistoryIndex = 0;
	int32 FrameHistoryCount = 0;

	/** Logging */
	double LastLogTime = 0.0;
	int32 FramesSinceLastLog = 0;

	/** Per-frame spend accounting. Reset when the engine frame changes. */
	uint64 CurrentFrameNumber = 0;
	float SpentThisFrameMs = 0.0f;

	void RollFrameIfNeeded(uint64 FrameNumber)
	{
		if (FrameNumber != CurrentFrameNumber)
		{
			CurrentFrameNumber = FrameNumber;
			SpentThisFrameMs = 0.0f;
		}
	}

	void LogBudgetStatistics(float LastFrameTimeMs) const
	{
		const float AvgFrameTime = GetAverageFrameTimeMs();

		UE_LOG(LogFragments, Verbose, TEXT("[FrameBudgetCoordinator] Budget: %.1fms, Last: %.2fms, Avg: %.2fms"),
			TotalFrameBudgetMs, LastFrameTimeMs, AvgFrameTime);
	}
};
