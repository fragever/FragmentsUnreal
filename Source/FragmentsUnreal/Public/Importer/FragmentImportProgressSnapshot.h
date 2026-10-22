// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"

/** What the chunked-spawn pipeline is doing right now, in terms a progress bar can use. */
enum class EFragmentImportPhase : uint8
{
	/** Draining PendingHISMQueue -- the bulk of the work. */
	Spawning = 0,
	/** Queue drained, but ShouldDeferBucketFinalize() is still true: in-flight LOD0 shell builds must land before buckets finalize. */
	WaitingOnMeshBuilds = 1,
	/** Phase A: chunked bucket finalize. */
	BuildingBuckets = 2,
	/** Phase B: paced cell-merge enqueue. */
	MergingCells = 3,
	/** Phase C: paced override apply -- the last paced phase. */
	ApplyingOverrides = 4,
};

/** Read-only picture of one model's place in the import pipeline. POD; safe to copy per tick. */
struct FFragmentImportProgressSnapshot
{
	/** True only if a chunked spawn is running AND it is THIS model's. */
	bool bIsSpawning = false;

	/** This guid is sitting in DeferredModelLoads waiting for the current spawn to finish. */
	bool bIsQueuedForSpawn = false;

	/** The importer is running the shell-first deferred-interior second wave, not an import. bEnableShellFirstStreaming defaults false so this is normally never true. */
	bool bIsDeferredInterior = false;

	/** Meaningful only when bIsSpawning is true -- the pipeline (PendingHISMQueue, HISMItemsSpawned, TotalHISMItems) serves one model at a time, so these describe whichever model bIsSpawning is true for. */
	int32 ItemsSpawned = 0;
	int32 TotalItems = 0;

	/** Meaningful only when bIsSpawning is true, for the same one-pipeline-at-a-time reason as ItemsSpawned/TotalItems above. */
	EFragmentImportPhase Phase = EFragmentImportPhase::Spawning;

	/** 0..1 within Phase, from that phase's own cursor. */
	float PhaseFraction = 0.f;

	/** InFlightShellBuilds.Num(). */
	int32 InFlightMeshBuilds = 0;
};
