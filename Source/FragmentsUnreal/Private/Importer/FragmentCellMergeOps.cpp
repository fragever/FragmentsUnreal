// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant


#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Spatial/FragmentCellCensus.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "Spatial/FragmentProxyDecimate.h"
#include "Spatial/FragmentProxyTiers.h"
#include "Utils/FragmentCellPartition.h"
#include "Utils/FragmentsUtils.h"
#include "Utils/BudgetDrain.h"
#include "Fragment/Fragment.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Rendering/PositionVertexBuffer.h"
#include "RawIndexBuffer.h"
#include "Engine/StaticMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// Defined (external linkage) in FragmentsImporter.cpp alongside the other Fragments CVars.
extern TAutoConsoleVariable<int32> CVarCellCollapseMinInstances;
extern TAutoConsoleVariable<int32> CVarCellMergeDecimate;
extern TAutoConsoleVariable<float> CVarCellMergeDecimateRatio;
extern TAutoConsoleVariable<int32> CVarCellMergeCensus;
extern TAutoConsoleVariable<int32> CVarCellMergeMinSimplifyTris;
extern TAutoConsoleVariable<float> CVarCellMergeWeldToleranceCm;
extern TAutoConsoleVariable<float> CVarCellMergeCollisionDecimateRatio;
extern TAutoConsoleVariable<int32> CVarCellMergeSimplifyShell;

// Sub-LOD calibration inputs.
extern TAutoConsoleVariable<float> CVarShellLOD0PromoteCm;
extern TAutoConsoleVariable<float> CVarInteriorLOD0PromoteCm;
extern TAutoConsoleVariable<float> CVarLODGeoCoeffCells;
extern TAutoConsoleVariable<float> CVarLODSSEPixelError;
extern TAutoConsoleVariable<float> CVarLODCoarsenFactor;
extern TAutoConsoleVariable<float> CVarProxyOmitPx;
extern TAutoConsoleVariable<float> CVarInteriorOmitPx;

/** The model's bounding-sphere radius, derived the same way UFragmentRegistrationManager:: RegisterModel derives FModelRegistrationData::ModelBoundsRadius: union the HISMCs' tight box bounds, then convert once. */
static float ModelBoundsRadiusFromBuckets(
	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Buckets)
{
	FBox UnionBox(ForceInit);
	for (const TPair<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Pair : Buckets)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
		if (!IsValid(HISM)) continue;
		const FBoxSphereBounds B = HISM->CalcBounds(HISM->GetComponentTransform());
		UnionBox += FBox(B.Origin - B.BoxExtent, B.Origin + B.BoxExtent);
	}
	return UnionBox.IsValid ? static_cast<float>(FBoxSphereBounds(UnionBox).SphereRadius) : 0.0f;
}

// Buckets[Key] gives us the source UStaticMesh only; we ignore its instance transforms because a single HISMC may carry instances from multiple cells.
static int32 SnapshotCellMergeSources(
	const FCellId& Cell,
	bool bShellSide,
	const TMap<FFragBucketKey, TArray<FPendingHISMItem>>& PendingItems,
	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Buckets,
	UFragmentModelWrapper* Wrapper,
	TArray<FMergeSource>& OutSources,
	TArray<UMaterialInterface*>& OutMaterials)
{
	int32 TotalTris = 0;

	// Bucket iteration order isn't deterministic; assign material slots lazily.
	TMap<UMaterialInterface*, int32> MaterialToSlot;

	for (const TPair<FFragBucketKey, TArray<FPendingHISMItem>>& BucketPair : PendingItems)
	{
		const FFragBucketKey& Key = BucketPair.Key;
		if ((Key.bShell != 0) != bShellSide) continue;

		// Buckets are already re-keyed per cell (FinalizePartitionCells' rebucket pass), so a key-cell mismatch can't contribute; the per-item Cell check below is a redundant invariant guard, not required by this filter.
		if (Key.Cell != Cell) continue;

		const TArray<FPendingHISMItem>& Items = BucketPair.Value;
		if (Items.Num() == 0) continue;

		TArray<FTransform> CellInstances;
		CellInstances.Reserve(Items.Num());
		for (const FPendingHISMItem& Item : Items)
		{
			if (Item.Cell == Cell)
			{
				CellInstances.Add(Item.WorldTransform);
			}
		}
		if (CellInstances.Num() == 0) continue;

		UHierarchicalInstancedStaticMeshComponent* const* HISMCPtr = Buckets.Find(Key);
		UHierarchicalInstancedStaticMeshComponent* HISMC = HISMCPtr ? *HISMCPtr : nullptr;
		if (!IsValid(HISMC)) continue;

		UStaticMesh* SrcMesh = Key.Mesh;
		if (!SrcMesh || !SrcMesh->GetRenderData() || SrcMesh->GetRenderData()->LODResources.Num() == 0) continue;

		const FStaticMeshLODResources& LOD = SrcMesh->GetRenderData()->LODResources[0];
		const int32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		if (NumVerts == 0) continue;

		UMaterialInterface* ResolvedMat = nullptr;
		if (Wrapper && Items.Num() > 0)
		{
			const FPendingHISMItem& First = Items[0];
			ResolvedMat = Wrapper->ResolveOverrideMaterial(First.R, First.G, First.B, First.A, First.bIsGlass);
		}
		UMaterialInterface* CellMat = ResolvedMat ? ResolvedMat : Key.Material.Get();

		int32 MaterialSlot = -1;
		if (int32* Found = MaterialToSlot.Find(CellMat))
		{
			MaterialSlot = *Found;
		}
		else
		{
			MaterialSlot = OutMaterials.Add(CellMat);
			MaterialToSlot.Add(CellMat, MaterialSlot);
		}

		FMergeSource Template;
		Template.Positions.SetNumUninitialized(NumVerts);
		Template.Normals.SetNumUninitialized(NumVerts);
		Template.TangentsX.SetNumUninitialized(NumVerts);
		Template.TangentYSigns.SetNumUninitialized(NumVerts);
		Template.UV0.SetNumUninitialized(NumVerts);
		Template.Colors.SetNumUninitialized(NumVerts);

		const FPositionVertexBuffer& PosBuf = LOD.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& VertBuf = LOD.VertexBuffers.StaticMeshVertexBuffer;
		const FColorVertexBuffer& ColorBuf = LOD.VertexBuffers.ColorVertexBuffer;
		const bool bHasColor = ColorBuf.GetNumVertices() == NumVerts;

		for (int32 i = 0; i < NumVerts; ++i)
		{
			Template.Positions[i] = PosBuf.VertexPosition(i);
			Template.Normals[i]   = VertBuf.VertexTangentZ(i);
			Template.TangentsX[i] = VertBuf.VertexTangentX(i);
			// Sign reconstruction: dot(cross(Normal, TangentX), TangentY) ∈ {-1, +1}.
			const FVector3f BiTan = VertBuf.VertexTangentY(i);
			const FVector3f Recon = FVector3f::CrossProduct(Template.Normals[i], Template.TangentsX[i]);
			Template.TangentYSigns[i] = (FVector3f::DotProduct(Recon, BiTan) >= 0.0f) ? 1.0f : -1.0f;
			Template.UV0[i] = VertBuf.GetVertexUV(i, 0);
			Template.Colors[i] = bHasColor ? ColorBuf.VertexColor(i) : FColor::White;
		}

		const FRawStaticIndexBuffer& IdxBuf = LOD.IndexBuffer;
		const int32 NumIndices = IdxBuf.GetNumIndices();
		Template.Indices.SetNumUninitialized(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			Template.Indices[i] = IdxBuf.GetIndex(i);
		}

		Template.MaterialSlot = MaterialSlot;

		// O(verts) once per template — the snapshot already walks these positions, and the per-instance scaling happens later via FragmentProxyTiers::WorldRadiusCm.
		Template.LocalRadiusCm = FragmentProxyTiers::LocalRadiusCm(Template.Positions);

		Template.Instances = MoveTemp(CellInstances);
		const int32 NumInst = Template.Instances.Num();

		OutSources.Add(MoveTemp(Template));
		TotalTris += (NumIndices / 3) * NumInst;
	}

	return TotalTris;
}

void UFragmentsImporter::FinalizePartitionCells()
{
	if (!CurrentHISMWrapper) return;

	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_CellPartition);

	// Cell bounds were accumulated as points during cell-tagging; inflate so AABBs enclose actual geometry. 200 cm is below visible LOD pop distance.
	const float CellInflateCm = 200.0f;
	const FVector InflateVec(CellInflateCm, CellInflateCm, CellInflateCm);

	// Idempotent for the shell-first interior re-entry: re-running would cumulatively grow bounds and collapse already-collapsed data.
	if (!CurrentHISMWrapper->bShellCellsFinalized)
	{
		for (TPair<FCellId, FCellMetadata>& Pair : CurrentHISMWrapper->ShellCells)
		{
			Pair.Value.WorldBounds = Pair.Value.WorldBounds.ExpandBy(InflateVec);
		}
		const int32 CollapseMin = CVarCellCollapseMinInstances.GetValueOnGameThread();
		const int32 CollapseRadius = 2;
		UE::Fragments::CellPartition::CollapseSparseCells(
			CurrentHISMWrapper->ShellCells, CollapseMin, CollapseRadius);
		CurrentHISMWrapper->bShellCellsFinalized = true;
	}

	// Interior cells run every call.
	for (TPair<FCellId, FCellMetadata>& Pair : CurrentHISMWrapper->InteriorCells)
	{
		Pair.Value.WorldBounds = Pair.Value.WorldBounds.ExpandBy(InflateVec);
	}
	const int32 CollapseMin2 = CVarCellCollapseMinInstances.GetValueOnGameThread();
	UE::Fragments::CellPartition::CollapseSparseCells(
		CurrentHISMWrapper->InteriorCells, CollapseMin2, /*MaxRadius*/ 2);

	// Reassign items whose cell was absorbed by CollapseSparseCells, by locating the cell whose post-collapse bounds now contain the item.
	auto RemapOrphans = [](TArray<FPendingHISMItem>& Items,
	                       const TMap<FCellId, FCellMetadata>& Cells,
	                       bool bShellFilter)
	{
		for (FPendingHISMItem& Item : Items)
		{
			if (Item.bShell != bShellFilter) continue;
			ensureMsgf(!Item.Cell.IsSentinel(),
				TEXT("RemapOrphans: uninitialized cell (LocalId=%lld Category=%s)."),
				Item.LocalId, *Item.Category);
			if (Item.Cell.IsSentinel()) continue;
			if (Cells.Contains(Item.Cell)) continue;
			const FVector Loc = Item.WorldTransform.GetLocation();
			bool bFoundNewCell = false;
			for (const TPair<FCellId, FCellMetadata>& Pair : Cells)
			{
				if (Pair.Value.WorldBounds.IsInsideOrOn(Loc))
				{
					Item.Cell = Pair.Key;
					bFoundNewCell = true;
					break;
				}
			}
			if (!bFoundNewCell)
			{
				// If this fires, CollapseSparseCells left a hole.
				UE_LOG(LogFragments, Error,
					TEXT("[CellSubBucket] Orphan item found no absorbing cell. LocalId=%lld Category=%s Loc=(%.0f,%.0f,%.0f) PrevCell=(F%d,X%d,Y%d) bShell=%d"),
					Item.LocalId, *Item.Category,
					Loc.X, Loc.Y, Loc.Z,
					Item.Cell.Floor, Item.Cell.GridX, Item.Cell.GridY, Item.bShell ? 1 : 0);
				ensureMsgf(false, TEXT("RemapOrphans: orphan with no absorbing cell - see preceding error log."));
			}
		}
	};
	for (TPair<FFragBucketKey, TArray<FPendingHISMItem>>& KV : PendingBucketInstances)
	{
		RemapOrphans(KV.Value, CurrentHISMWrapper->ShellCells,    true);
		RemapOrphans(KV.Value, CurrentHISMWrapper->InteriorCells, false);
	}

	// RemapOrphans updates Item.Cell in place; re-key the bucket map so HISMCs finalize against post-collapse cells.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_RebucketAfterRemap);
		TMap<FFragBucketKey, TArray<FPendingHISMItem>> Rebuilt;
		Rebuilt.Reserve(PendingBucketInstances.Num());
		int32 ReboundItems = 0;
		for (TPair<FFragBucketKey, TArray<FPendingHISMItem>>& KV : PendingBucketInstances)
		{
			for (FPendingHISMItem& Item : KV.Value)
			{
				FFragBucketKey NewKey = KV.Key;
				NewKey.Cell = Item.Cell;
				if (NewKey.Cell != KV.Key.Cell) ReboundItems++;
				Rebuilt.FindOrAdd(NewKey).Add(MoveTemp(Item));
			}
		}
		PendingBucketInstances = MoveTemp(Rebuilt);
		UE_LOG(LogFragments, Log,
			TEXT("[CellSubBucket] Rebucket-after-remap: rebound %d items, final bucket count=%d"),
			ReboundItems, PendingBucketInstances.Num());
	}

	UE_LOG(LogFragments, Log,
		TEXT("[CellPartition] ShellCells=%d InteriorCells=%d (after collapse)"),
		CurrentHISMWrapper->ShellCells.Num(),
		CurrentHISMWrapper->InteriorCells.Num());
}

void UFragmentsImporter::LogCategoryFrequencyTable() const
{
	struct FCatStats { int32 TotalBuckets = 0; int32 GlassBuckets = 0; int32 TotalInstances = 0; int32 GlassInstances = 0; };
	TMap<FString, FCatStats> CategoryStats;
	for (const auto& Pair : PendingBucketInstances)
	{
		if (Pair.Value.Num() == 0) continue;
		const FPendingHISMItem& First = Pair.Value[0];
		FCatStats& Stats = CategoryStats.FindOrAdd(First.Category);
		Stats.TotalBuckets++;
		Stats.TotalInstances += Pair.Value.Num();
		if (First.bIsGlass)
		{
			Stats.GlassBuckets++;
			Stats.GlassInstances += Pair.Value.Num();
		}
	}

	// Sort by total bucket count, descending.
	TArray<TPair<FString, FCatStats>> Sorted;
	for (const auto& KV : CategoryStats) Sorted.Add(TPair<FString, FCatStats>(KV.Key, KV.Value));
	Sorted.Sort([](const TPair<FString, FCatStats>& A, const TPair<FString, FCatStats>& B)
	{
		return A.Value.TotalBuckets > B.Value.TotalBuckets;
	});

	UE_LOG(LogFragments, Verbose, TEXT("[CategoryFreq] === Category frequency table (%d distinct categories) ==="), Sorted.Num());
	UE_LOG(LogFragments, Verbose, TEXT("[CategoryFreq] %-40s %10s %10s %12s %12s"),
		TEXT("Category"), TEXT("Buckets"), TEXT("GlassBkts"), TEXT("Instances"), TEXT("GlassInsts"));
	for (const auto& KV : Sorted)
	{
		UE_LOG(LogFragments, Verbose, TEXT("[CategoryFreq] %-40s %10d %10d %12d %12d"),
			*KV.Key, KV.Value.TotalBuckets, KV.Value.GlassBuckets, KV.Value.TotalInstances, KV.Value.GlassInstances);
	}
	UE_LOG(LogFragments, Verbose, TEXT("[CategoryFreq] === end ==="));
}

void UFragmentsImporter::EnqueueEagerCellMerges()
{
	// The wrapper has no ModelGuid field of its own — we use the importer's CurrentSpawningModelGuid (set in BuildHISMSpawnQueue) as the model handle.
	if (!CurrentHISMWrapper) return;

	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_EnqueueAllCellMerges);

	AFragment* BucketRoot = CurrentBucketRoot;
	// Was World -> GameInstance -> subsystem -> GetCellMergeManager().
	FFragmentCellMergeManager* Mgr = GetCellMergeManager();
	UFragmentRegistrationManager* RegMgr = GetRegistrationManager();

	if (!BucketRoot)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[EagerMerge] CurrentBucketRoot is null at FinalizeHISMBuckets - skipping cell-merge enqueue"));
		return;
	}
	if (!Mgr || !RegMgr)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[EagerMerge] Manager resolution failed: Mgr=%s RegMgr=%s - skipping cell-merge enqueue"),
			Mgr ? TEXT("ok") : TEXT("null"), RegMgr ? TEXT("ok") : TEXT("null"));
		return;
	}

	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Buckets =
		BucketRoot->Buckets;

	const FString ModelGuid = CurrentSpawningModelGuid;

	// Once per model, not once per cell: the value is identical for every cell and deriving it costs one CalcBounds per bucket.
	const float ModelBoundsRadiusCm = ModelBoundsRadiusFromBuckets(Buckets);

	// Capture BEFORE flipping the flag so the log can report what this call did.
	const bool bShellEnqueuedThisCall = !CurrentHISMWrapper->bShellCellsMergeEnqueued;

	// FinalizeHISMBuckets fires twice in shell-first flow; only the first call should enqueue shell merges (the second adds interiors only).
	if (!CurrentHISMWrapper->bShellCellsMergeEnqueued)
	{
		for (const TPair<FCellId, FCellMetadata>& Pair : CurrentHISMWrapper->ShellCells)
		{
			EnqueueOneCellMerge(Pair.Key, /*bShell*/ true, Mgr, RegMgr, BucketRoot, ModelGuid,
				Buckets, ModelBoundsRadiusCm);
		}
		CurrentHISMWrapper->bShellCellsMergeEnqueued = true;
	}
	// Interior cells: always enqueue.
	for (const TPair<FCellId, FCellMetadata>& Pair : CurrentHISMWrapper->InteriorCells)
	{
		EnqueueOneCellMerge(Pair.Key, /*bShell*/ false, Mgr, RegMgr, BucketRoot, ModelGuid,
			Buckets, ModelBoundsRadiusCm);
	}

	UE_LOG(LogFragments, Log,
		TEXT("[EagerMerge] Enqueued merges for model %s: shell %d (this call: %s) + interior %d"),
		*ModelGuid,
		CurrentHISMWrapper->ShellCells.Num(),
		bShellEnqueuedThisCall ? TEXT("enqueued") : TEXT("skipped (already done)"),
		CurrentHISMWrapper->InteriorCells.Num());
}

FragmentProxyDecimate::FDecimateSettings UFragmentsImporter::ApplyShellSimplifyGate(
	const FragmentProxyDecimate::FDecimateSettings& In, bool bShellSide, bool bSimplifyShell)
{
	if (!bShellSide || bSimplifyShell) { return In; }

	FragmentProxyDecimate::FDecimateSettings Out = In;
	Out.bEnabled = false;   // only ever disables; Ratio is left as-is
	return Out;
}

void UFragmentsImporter::EnqueueOneCellMerge(
	const FCellId& Cell, bool bShellSide,
	FFragmentCellMergeManager* Mgr, UFragmentRegistrationManager* RegMgr,
	AFragment* BucketRoot, const FString& ModelGuid,
	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Buckets,
	float ModelBoundsRadiusCm)
{
	TArray<FMergeSource> Sources;
	TArray<UMaterialInterface*> Materials;
	const int32 Tris = SnapshotCellMergeSources(
		Cell, bShellSide, PendingBucketInstances, Buckets, CurrentHISMWrapper, Sources, Materials);
	if (Tris == 0 || Sources.Num() == 0) return;

	const FString CellLabel = FString::Printf(TEXT("F%d_X%d_Y%d_%s"),
		Cell.Floor, Cell.GridX, Cell.GridY, bShellSide ? TEXT("Shell") : TEXT("Interior"));

	// Dry-run census: observes exactly what selection will see, changes nothing about what is built.
	if (CVarCellMergeCensus.GetValueOnGameThread() != 0)
	{
		FragmentCellCensus::FCellCensus Census =
			FragmentCellCensus::AccumulateCellCensus(Sources, Materials.Num());
		Census.bShell    = bShellSide;
		Census.ModelGuid = ModelGuid;
		Census.CellLabel = CellLabel;
		CellCensusReport.Cells.Add(MoveTemp(Census));
	}

	UFragmentRegistrationManager* RegMgrCapture = RegMgr;
	const FString ModelGuidCapture = ModelGuid;
	const FCellId CellCapture = Cell;
	const bool   bShellCapture = bShellSide;
	AFragment*   BucketRootCapture = BucketRoot;

	const FragmentProxyDecimate::FDecimateSettings DecimateSettings =
		UFragmentsImporter::ApplyShellSimplifyGate(
			FragmentProxyDecimate::ResolveSettings(
				CVarCellMergeDecimate.GetValueOnGameThread(),
				CVarCellMergeDecimateRatio.GetValueOnGameThread(),
				bDecimateCellMerge,
				CellMergeDecimationRatio),
			bShellSide,
			CVarCellMergeSimplifyShell.GetValueOnGameThread() != 0);

	// Shell and interior cells calibrate sub-LOD 0 against their own promote distance; both share the model's demote distance for sub-LOD 1, because the model demotes as a whole.
	const float NearEdgeCm = bShellSide
		? CVarShellLOD0PromoteCm.GetValueOnGameThread()
		: CVarInteriorLOD0PromoteCm.GetValueOnGameThread();

	float CellBoundsRadiusCm = 0.0f;
	if (CurrentHISMWrapper)
	{
		const TMap<FCellId, FCellMetadata>& Table =
			bShellSide ? CurrentHISMWrapper->ShellCells : CurrentHISMWrapper->InteriorCells;
		if (const FCellMetadata* Meta = Table.Find(Cell))
		{
			CellBoundsRadiusCm = static_cast<float>(Meta->WorldBounds.GetExtent().Size());
		}
	}
	if (CellBoundsRadiusCm <= 0.0f)
	{
		// Both sub-LOD screen sizes come out 0.0, so UE falls back to MinLOD and renders sub-LOD 0 forever: safe to look at, but the mesh pays 2x memory for a rung that can never display.
		UE_LOG(LogFragments, Warning,
			TEXT("[CellMerge] Cell %s has no wrapper bounds (wrapper=%s) - sub-LOD screen sizes "
			     "will be 0.0 and the merged mesh will render sub-LOD 0 at every distance, "
			     "paying for a LOD rung it can never display."),
			*CellLabel, CurrentHISMWrapper ? TEXT("present") : TEXT("null"));
	}

	const TArray<FCellSubLodSpec> SubLods = UFragmentsImporter::BuildCellSubLodSpecs(
		NearEdgeCm, ModelBoundsRadiusCm, CellBoundsRadiusCm,
		UFragmentsImporter::ResolveCellOmitPx(bShellSide),
		CVarLODGeoCoeffCells.GetValueOnGameThread(),
		CVarLODSSEPixelError.GetValueOnGameThread(),
		CVarLODCoarsenFactor.GetValueOnGameThread());

	Mgr->EnqueueMerge(
		ModelGuid, CellLabel,
		MoveTemp(Sources), MoveTemp(Materials),
		SubLods,
		DecimateSettings,
		CVarCellMergeMinSimplifyTris.GetValueOnGameThread(),
		CVarCellMergeCollisionDecimateRatio.GetValueOnGameThread(),
		[RegMgrCapture, ModelGuidCapture, CellCapture, bShellCapture, BucketRootCapture]
		(UStaticMesh* MergedMesh)
		{
			if (!IsValid(RegMgrCapture)) return;
			if (!IsValid(BucketRootCapture)) return;
			if (!MergedMesh)
			{
				RegMgrCapture->OnCellMergeFailed(ModelGuidCapture, CellCapture, bShellCapture);
				return;
			}
			RegMgrCapture->AttachMergedComponent(
				ModelGuidCapture, CellCapture, bShellCapture, MergedMesh);
		},
		CVarCellMergeWeldToleranceCm.GetValueOnGameThread());
}

void UFragmentsImporter::BuildCellMergeWorkList(
	TConstArrayView<FCellId> ShellKeys, TConstArrayView<FCellId> InteriorKeys,
	bool& bShellEnqueued, TArray<TPair<FCellId, bool>>& OutWork)
{
	// Shell cells only once (shell-first flow fires this twice); interiors every time.
	if (!bShellEnqueued)
	{
		for (const FCellId& Cell : ShellKeys) { OutWork.Emplace(Cell, /*bShell*/ true); }
		bShellEnqueued = true;
	}
	for (const FCellId& Cell : InteriorKeys) { OutWork.Emplace(Cell, /*bShell*/ false); }
}

void UFragmentsImporter::BeginCellMergeEnqueue()
{
	CellMergeEnqueueQueue.Reset();
	CellMergeEnqueueCursor = 0;

	// Snapshot the model bounds radius once for the whole queue.
	CellMergeModelBoundsRadiusCm = CurrentBucketRoot
		? ModelBoundsRadiusFromBuckets(CurrentBucketRoot->Buckets)
		: 0.0f;

	if (CurrentHISMWrapper)
	{
		TArray<FCellId> ShellKeys, InteriorKeys;
		CurrentHISMWrapper->ShellCells.GetKeys(ShellKeys);
		CurrentHISMWrapper->InteriorCells.GetKeys(InteriorKeys);
		BuildCellMergeWorkList(ShellKeys, InteriorKeys,
			CurrentHISMWrapper->bShellCellsMergeEnqueued, CellMergeEnqueueQueue);
	}

	FinalizeTailPhase = EFinalizeTailPhase::EnqueueCellMerges;
}

bool UFragmentsImporter::ProcessCellMergeEnqueueChunk()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_EnqueueCellMergesChunk);

	// Resolve managers once per tick (cheap; avoids stale member pointers).
	AFragment* BucketRoot = CurrentBucketRoot;
	// See EnqueueEagerCellMerges above for why this no longer walks the GameInstance.
	FFragmentCellMergeManager* Mgr = GetCellMergeManager();
	UFragmentRegistrationManager* RegMgr = GetRegistrationManager();

	if (!BucketRoot || !Mgr || !RegMgr || !CurrentHISMWrapper)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[EagerMerge] Paced enqueue: manager resolution failed (BucketRoot=%s Mgr=%s RegMgr=%s Wrapper=%s); "
			     "dropping %d un-enqueued cells"),
			BucketRoot ? TEXT("ok") : TEXT("null"), Mgr ? TEXT("ok") : TEXT("null"),
			RegMgr ? TEXT("ok") : TEXT("null"), CurrentHISMWrapper ? TEXT("ok") : TEXT("null"),
			CellMergeEnqueueQueue.Num() - CellMergeEnqueueCursor);
		CellMergeEnqueueCursor = CellMergeEnqueueQueue.Num();
		return true;
	}

	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Buckets = BucketRoot->Buckets;
	const FString ModelGuid = CurrentSpawningModelGuid;

	return FragmentBudgetDrain::DrainBudgeted(
		CellMergeEnqueueCursor, CellMergeEnqueueQueue.Num(),
		FrameBudgetCoordinator, GFrameCounter,
		[&](int32 i)
		{
			const double T0 = FPlatformTime::Seconds();
			const TPair<FCellId, bool>& Work = CellMergeEnqueueQueue[i];
			EnqueueOneCellMerge(Work.Key, Work.Value, Mgr, RegMgr, BucketRoot, ModelGuid,
				Buckets, CellMergeModelBoundsRadiusCm);
			FrameBudgetCoordinator.RecordWork(
				static_cast<float>((FPlatformTime::Seconds() - T0) * 1000.0), GFrameCounter);
		});
}
