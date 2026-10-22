// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"

struct FMergeSource;

/** Dry-run census of what cell-merge selection WOULD keep, at every cutoff at once. */
namespace FragmentCellCensus
{
	/** Log-spaced base 2 from 0.25 cm. */
	inline constexpr int32 kNumRadiusBuckets = 24;
	inline constexpr float kRadiusBucketBaseCm = 0.25f;

	/** Powers of two from 1 triangle. */
	inline constexpr int32 kNumTemplateTriBuckets = 16;

	/** One cell's census. */
	struct FCellCensus
	{
		TArray<int64> RadiusBucketTris;
		TArray<int64> TemplateTriBucketTris;
		int64   TotalTris      = 0;
		int32   TotalInstances = 0;
		int32   NumSources     = 0;
		bool    bShell         = true;
		FString ModelGuid;
		FString CellLabel;

		FCellCensus()
		{
			RadiusBucketTris.SetNumZeroed(kNumRadiusBuckets);
			TemplateTriBucketTris.SetNumZeroed(kNumTemplateTriBuckets);
		}
	};

	FRAGMENTSUNREAL_API int32 RadiusBucketIndex(float RadiusCm);
	FRAGMENTSUNREAL_API int32 TemplateTriBucketIndex(int32 Tris);
	FRAGMENTSUNREAL_API float RadiusBucketLowerBoundCm(int32 Bucket);

	/** Walk every source and every instance exactly as ExpandSourcesToSections does, bucketing by FragmentProxyTiers::WorldRadiusCm. */
	FRAGMENTSUNREAL_API FCellCensus AccumulateCellCensus(
		TConstArrayView<FMergeSource> Sources, int32 NumMaterialSlots);

	/** Suffix sum. */
	FRAGMENTSUNREAL_API int64 SurvivingTrisAtCutoff(const FCellCensus& C, float CutoffCm);

	/** Roll per-cell censuses up to per-model / campus-wide. Bucket-wise addition. */
	FRAGMENTSUNREAL_API void MergeCensus(FCellCensus& InOut, const FCellCensus& Other);

	/** Result of inverting the census predictor: given a memory target, the cutoff that lands under it. */
	struct FBudgetResult
	{
		float  CutoffCm      = 0.0f;
		int64  PredictedTris = 0;
		double PredictedMB   = 0.0;
		double FloorMB       = 0.0;
		bool   bReachable    = false;
	};

	/** Smallest bucket-boundary cutoff whose predicted bytes land under BudgetMB. */
	FRAGMENTSUNREAL_API FBudgetResult SolveCutoffForBudget(
		const FCellCensus& C, double BudgetMB, double BytesPerCellTri, double FloorMB);

	/** Campus-wide census accumulation for one import. */
	struct FCensusReport
	{
		TArray<FCellCensus> Cells;

		/** SamplePath -> number of LOD0 template meshes built for it, populated ONLY at EnqueueShellMeshBuild (the site SamplePath is coalesced on via RegisterShellBuildWaiter / InFlightShellBuilds / MeshCache). */
		TMap<FString, int32> DistinctSamplePaths;

		/** LOD0 template UStaticMesh count, broken down by the NewObject<UStaticMesh> call site that created it. */
		TMap<FString, int32> MeshesBuiltBySite;
	};

	/** CSV to AbsPath. One row per cell plus rolled-up shell/interior/total rows. */
	FRAGMENTSUNREAL_API void WriteCensusCsv(const FCensusReport& Report, const FString& AbsPath);
}
