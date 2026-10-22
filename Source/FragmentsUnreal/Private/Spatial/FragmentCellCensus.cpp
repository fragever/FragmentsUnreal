// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentCellCensus.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "Spatial/FragmentProxyTiers.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragCellCensus, Log, All);

namespace FragmentCellCensus
{
	int32 RadiusBucketIndex(float RadiusCm)
	{
		if (!(RadiusCm > kRadiusBucketBaseCm)) { return 0; }
		const int32 Bucket = FMath::FloorToInt32(
			FMath::Log2(RadiusCm / kRadiusBucketBaseCm));
		return FMath::Clamp(Bucket, 0, kNumRadiusBuckets - 1);
	}

	int32 TemplateTriBucketIndex(int32 Tris)
	{
		if (Tris <= 1) { return 0; }
		const int32 Bucket = FMath::FloorToInt32(FMath::Log2(static_cast<float>(Tris)));
		return FMath::Clamp(Bucket, 0, kNumTemplateTriBuckets - 1);
	}

	float RadiusBucketLowerBoundCm(int32 Bucket)
	{
		if (Bucket <= 0) { return 0.0f; }
		return kRadiusBucketBaseCm * FMath::Pow(2.0f, static_cast<float>(Bucket));
	}

	FCellCensus AccumulateCellCensus(TConstArrayView<FMergeSource> Sources, int32 NumMaterialSlots)
	{
		FCellCensus C;
		for (const FMergeSource& Src : Sources)
		{
			// Mirror ExpandSourcesToSections' own skips exactly, in the same order.
			if (Src.MaterialSlot < 0 || Src.MaterialSlot >= NumMaterialSlots) { continue; }
			if (Src.Positions.Num() == 0 || Src.Instances.Num() == 0) { continue; }

			const int32 TrisPerInstance = Src.Indices.Num() / 3;
			if (TrisPerInstance <= 0) { continue; }

			++C.NumSources;
			const int32 TemplateBucket = TemplateTriBucketIndex(TrisPerInstance);

			for (const FTransform& Xform : Src.Instances)
			{
				const float WorldRadius =
					FragmentProxyTiers::WorldRadiusCm(Src.LocalRadiusCm, Xform);

				C.RadiusBucketTris[RadiusBucketIndex(WorldRadius)] += TrisPerInstance;
				C.TemplateTriBucketTris[TemplateBucket] += TrisPerInstance;
				C.TotalTris += TrisPerInstance;
				++C.TotalInstances;
			}
		}
		return C;
	}

	int64 SurvivingTrisAtCutoff(const FCellCensus& C, float CutoffCm)
	{
		int64 Surviving = 0;
		for (int32 i = 0; i < kNumRadiusBuckets; ++i)
		{
			if (RadiusBucketLowerBoundCm(i) >= CutoffCm)
			{
				Surviving += C.RadiusBucketTris[i];
			}
		}
		return Surviving;
	}

	FBudgetResult SolveCutoffForBudget(
		const FCellCensus& C, double BudgetMB, double BytesPerCellTri, double FloorMB)
	{
		FBudgetResult R;
		R.FloorMB = FloorMB;

		const double CellBudgetMB = BudgetMB - FloorMB;
		if (CellBudgetMB <= 0.0)
		{
			R.bReachable = false;
			return R;
		}

		for (int32 i = 0; i < kNumRadiusBuckets; ++i)
		{
			const float  Cutoff = RadiusBucketLowerBoundCm(i);
			const int64  Tris   = SurvivingTrisAtCutoff(C, Cutoff);
			const double MB     = (double)Tris * BytesPerCellTri / (1024.0 * 1024.0);

			if (MB <= CellBudgetMB)
			{
				R.CutoffCm      = Cutoff;
				R.PredictedTris = Tris;
				R.PredictedMB   = MB + FloorMB;
				R.bReachable    = true;
				return R;
			}
		}

		// Even omitting everything cannot fit: report the tightest cutoff and flag it.
		R.CutoffCm      = RadiusBucketLowerBoundCm(kNumRadiusBuckets - 1);
		R.PredictedTris = SurvivingTrisAtCutoff(C, R.CutoffCm);
		R.PredictedMB   = (double)R.PredictedTris * BytesPerCellTri / (1024.0 * 1024.0) + FloorMB;
		R.bReachable    = false;
		return R;
	}

	void MergeCensus(FCellCensus& InOut, const FCellCensus& Other)
	{
		for (int32 i = 0; i < kNumRadiusBuckets; ++i)
		{
			InOut.RadiusBucketTris[i] += Other.RadiusBucketTris[i];
		}
		for (int32 i = 0; i < kNumTemplateTriBuckets; ++i)
		{
			InOut.TemplateTriBucketTris[i] += Other.TemplateTriBucketTris[i];
		}
		InOut.TotalTris      += Other.TotalTris;
		InOut.TotalInstances += Other.TotalInstances;
		InOut.NumSources     += Other.NumSources;
	}

	static void AppendCensusRow(FString& Out, const FString& Label, const FCellCensus& C)
	{
		Out += Label;
		Out += FString::Printf(TEXT(",%s,%lld,%d,%d"),
			C.bShell ? TEXT("Shell") : TEXT("Interior"),
			C.TotalTris, C.TotalInstances, C.NumSources);
		for (int32 i = 0; i < kNumRadiusBuckets; ++i)
		{
			Out += FString::Printf(TEXT(",%lld"), C.RadiusBucketTris[i]);
		}
		for (int32 i = 0; i < kNumTemplateTriBuckets; ++i)
		{
			Out += FString::Printf(TEXT(",%lld"), C.TemplateTriBucketTris[i]);
		}
		Out += LINE_TERMINATOR;
	}

	void WriteCensusCsv(const FCensusReport& Report, const FString& AbsPath)
	{
		FString Out;

		Out += TEXT("Cell,Category,TotalTris,Instances,Sources");
		for (int32 i = 0; i < kNumRadiusBuckets; ++i)
		{
			Out += FString::Printf(TEXT(",R>=%.2fcm"), RadiusBucketLowerBoundCm(i));
		}
		for (int32 i = 0; i < kNumTemplateTriBuckets; ++i)
		{
			Out += FString::Printf(TEXT(",Tmpl>=%dtris"), 1 << i);
		}
		Out += LINE_TERMINATOR;

		FCellCensus Shell;    Shell.bShell = true;
		FCellCensus Interior; Interior.bShell = false;

		for (const FCellCensus& C : Report.Cells)
		{
			AppendCensusRow(Out, C.ModelGuid + TEXT("/") + C.CellLabel, C);
			MergeCensus(C.bShell ? Shell : Interior, C);
		}

		AppendCensusRow(Out, TEXT("__TOTAL_SHELL__"), Shell);
		AppendCensusRow(Out, TEXT("__TOTAL_INTERIOR__"), Interior);

		FCellCensus All = Shell;
		MergeCensus(All, Interior);
		All.bShell = true;
		AppendCensusRow(Out, TEXT("__TOTAL_ALL__"), All);

		// Template duplication.
		const int32 Distinct = Report.DistinctSamplePaths.Num();
		Out += LINE_TERMINATOR;
		Out += TEXT("__TEMPLATE_DEDUP__,Site,MeshesBuilt,DistinctSamplePaths,Ratio");
		Out += LINE_TERMINATOR;

		TArray<FString> SiteNames;
		Report.MeshesBuiltBySite.GetKeys(SiteNames);
		SiteNames.Sort();

		int32 TotalMeshesBuilt = 0;
		for (const FString& Site : SiteNames)
		{
			const int32 Count = Report.MeshesBuiltBySite[Site];
			TotalMeshesBuilt += Count;
			const float Ratio = Distinct > 0 ? (float)Count / (float)Distinct : 0.0f;
			Out += FString::Printf(TEXT("__TEMPLATE_DEDUP__,%s,%d,%d,%.2f%s"),
				*Site, Count, Distinct, Ratio, LINE_TERMINATOR);
		}

		const float TotalRatio = Distinct > 0 ? (float)TotalMeshesBuilt / (float)Distinct : 0.0f;
		Out += FString::Printf(TEXT("__TEMPLATE_DEDUP__,__TOTAL__,%d,%d,%.2f%s"),
			TotalMeshesBuilt, Distinct, TotalRatio, LINE_TERMINATOR);

		if (!FFileHelper::SaveStringToFile(Out, *AbsPath))
		{
			UE_LOG(LogFragCellCensus, Warning, TEXT("[CellCensus] Failed to write %s"), *AbsPath);
			return;
		}
		UE_LOG(LogFragCellCensus, Log, TEXT("[CellCensus] Wrote %d cell rows to %s"),
			Report.Cells.Num(), *AbsPath);
	}
}
