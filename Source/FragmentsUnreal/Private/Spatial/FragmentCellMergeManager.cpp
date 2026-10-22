// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentCellMergeManager.h"

#include "Async/Async.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderingThread.h"
#include "Spatial/FragmentMeshAuthoring.h"
#include "Spatial/FragmentProxyTiers.h"
#include "Spatial/FragmentTriMeshCook.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragCellMerge, Log, All);

FragmentCellMerge::FMergeWorkerResult FragmentCellMerge::MergeMeshOnWorker(
	TArray<FMergeSource> Sources,
	int32 NumMaterialSlots,
	TArray<FCellSubLodSpec> SubLods,
	FragmentProxyDecimate::FDecimateSettings DecimateSettings,
	int32 MinSimplifyTris,
	float CollisionDecimateRatio,
	float WeldToleranceCm)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragCellMerge_WorkerCook);

	FMergeWorkerResult Result;
	if (SubLods.Num() == 0) { return Result; }

	// Collision geometry is identical to visual sub-LOD 0 geometry iff nothing modifies Sources between the two passes AND collision is not separately decimated.
	const bool bReuseSubLod0ForCollision =
			(CollisionDecimateRatio >= 1.0f)
		&& !(DecimateSettings.bEnabled && DecimateSettings.Ratio < 1.0f);

	// Collision first, from UNSIMPLIFIED sub-LOD 0 (SLOW path only). --- Sub-LOD 0 is the collision source ("collision stays sub-LOD 0"), so it must be cooked before template simplification touches the geometry.
	if (!bReuseSubLod0ForCollision)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragCellMerge_CollisionCook);

		TArray<FragmentCellMerge::FExpandedSection> CollisionExpanded =
			FragmentCellMerge::ExpandSourcesToSections(
				Sources, NumMaterialSlots, SubLods[0].MinRadiusCm);

		if (CollisionDecimateRatio < 1.0f)
		{
			for (FragmentCellMerge::FExpandedSection& Ex : CollisionExpanded)
			{
				FragmentProxyDecimate::DecimateSection(Ex.Geo, CollisionDecimateRatio);
			}
		}

		TArray<FragmentMeshAuthoring::FAuthoringSection> CollisionSections;
		CollisionSections.Reserve(CollisionExpanded.Num());
		for (FragmentCellMerge::FExpandedSection& Ex : CollisionExpanded)
		{
			if (Ex.Geo.Positions.Num() == 0 || Ex.Geo.Indices.Num() < 3) { continue; }
			FragmentMeshAuthoring::FAuthoringSection& Sec = CollisionSections.AddDefaulted_GetRef();
			Sec.MaterialSlot  = Ex.MaterialSlot;
			Sec.Positions     = Ex.Geo.Positions;
			Sec.Normals       = Ex.Geo.Normals;
			Sec.TangentsX     = Ex.Geo.TangentsX;
			Sec.TangentYSigns = Ex.Geo.TangentYSigns;
			Sec.UV0           = Ex.Geo.UV0;
			Sec.Colors        = Ex.Geo.VertexColors;
			Sec.Indices       = Ex.Geo.Indices;
		}

		FMeshDescription CollisionOnlyMD;
		TArray<FVector3f> CollisionPositions;
		TArray<uint32>    CollisionIndices;
		if (FragmentMeshAuthoring::AuthorMeshDescription(
				CollisionSections, TEXT("CellMergeCollision_"),
				CollisionOnlyMD, CollisionPositions, CollisionIndices)
			&& CollisionIndices.Num() >= 3)
		{
			Result.TriMesh = FragmentTriMeshCook::BuildTriMeshOnWorker(
				MoveTemp(CollisionPositions), MoveTemp(CollisionIndices));
		}
	}

	// Template-scoped simplify, ONCE, before any expansion.
	if (DecimateSettings.bEnabled && DecimateSettings.Ratio < 1.0f)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragCellMerge_SimplifyTemplatesOnWorker);
		for (FMergeSource& Src : Sources)
		{
			FragmentCellMerge::SimplifyTemplate(Src, DecimateSettings.Ratio, MinSimplifyTris, WeldToleranceCm);
		}
	}

	// Visual sub-LODs from the (possibly simplified) templates.
	for (int32 Lod = 0; Lod < SubLods.Num(); ++Lod)
	{
		TArray<FragmentCellMerge::FExpandedSection> Expanded =
			FragmentCellMerge::ExpandSourcesToSections(
				Sources, NumMaterialSlots, SubLods[Lod].MinRadiusCm);

		TArray<FragmentMeshAuthoring::FAuthoringSection> AuthoringSections;
		AuthoringSections.Reserve(Expanded.Num());
		for (FragmentCellMerge::FExpandedSection& Ex : Expanded)
		{
			if (Ex.Geo.Positions.Num() == 0 || Ex.Geo.Indices.Num() < 3) { continue; }

			FragmentMeshAuthoring::FAuthoringSection& Sec = AuthoringSections.AddDefaulted_GetRef();
			Sec.MaterialSlot  = Ex.MaterialSlot;
			Sec.Positions     = Ex.Geo.Positions;
			Sec.Normals       = Ex.Geo.Normals;
			Sec.TangentsX     = Ex.Geo.TangentsX;
			Sec.TangentYSigns = Ex.Geo.TangentYSigns;
			Sec.UV0           = Ex.Geo.UV0;
			Sec.Colors        = Ex.Geo.VertexColors;
			Sec.Indices       = Ex.Geo.Indices;
			// Instances left empty -> one identity instance; positions are already world-space.
		}

		TUniquePtr<FMeshDescription> MD = MakeUnique<FMeshDescription>();
		TArray<FVector3f> AuthoredPositions;
		TArray<uint32>    AuthoredIndices;

		const bool bOk = FragmentMeshAuthoring::AuthorMeshDescription(
			AuthoringSections, TEXT("CellMergeSlot_"), *MD, AuthoredPositions, AuthoredIndices);

		if (!bOk || AuthoredIndices.Num() < 3)
		{
			// Sub-LOD 0 failing is a degenerate cell: return a FULLY empty Result -- including a null TriMesh.
			if (Lod == 0) { return FMergeWorkerResult(); }
			// A later sub-LOD authoring nothing is dropped ENTIRELY — description and screen size together.
			continue;
		}

		// FAST path: sub-LOD 0 authoring succeeded and the guard proved collision is identical to this geometry -- cook the trimesh straight from this pass's own out-params instead of a separate expansion+authoring.
		if (Lod == 0 && bReuseSubLod0ForCollision)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragCellMerge_CollisionCook);
			Result.TriMesh = FragmentTriMeshCook::BuildTriMeshOnWorker(
				MoveTemp(AuthoredPositions), MoveTemp(AuthoredIndices));
			Result.bCollisionReusedSubLod0 = true;
		}

		Result.MeshDescriptions.Add(MoveTemp(MD));
		Result.LODScreenSizes.Add(SubLods[Lod].ScreenSize);
	}

	return Result;
}

TArray<FragmentCellMerge::FExpandedSection> FragmentCellMerge::ExpandSourcesToSections(
	TConstArrayView<FMergeSource> Sources, int32 NumMaterialSlots, float MinRadiusCm)
{
	TArray<FExpandedSection> Out;
	TMap<int32, int32> SlotToIndex;   // MaterialSlot -> index into Out

	for (const FMergeSource& Src : Sources)
	{
		if (Src.MaterialSlot < 0 || Src.MaterialSlot >= NumMaterialSlots) { continue; }
		if (Src.Positions.Num() == 0 || Src.Instances.Num() == 0) { continue; }

		int32 SecIdx;
		if (const int32* Found = SlotToIndex.Find(Src.MaterialSlot))
		{
			SecIdx = *Found;
		}
		else
		{
			SecIdx = Out.Num();
			Out.AddDefaulted_GetRef().MaterialSlot = Src.MaterialSlot;
			SlotToIndex.Add(Src.MaterialSlot, SecIdx);
		}
		FragmentProxyDecimate::FDecimateSection& Geo = Out[SecIdx].Geo;

		const int32 NumVerts = Src.Positions.Num();

		// Reserve this source's contribution up front (verts x instances) to avoid repeated TArray reallocation as a large instanced bucket accumulates into the slot.
		const int32 AddVerts   = NumVerts * Src.Instances.Num();
		const int32 AddIndices = Src.Indices.Num() * Src.Instances.Num();
		Geo.Positions.Reserve(Geo.Positions.Num() + AddVerts);
		Geo.Normals.Reserve(Geo.Normals.Num() + AddVerts);
		Geo.TangentsX.Reserve(Geo.TangentsX.Num() + AddVerts);
		Geo.TangentYSigns.Reserve(Geo.TangentYSigns.Num() + AddVerts);
		Geo.UV0.Reserve(Geo.UV0.Num() + AddVerts);
		Geo.VertexColors.Reserve(Geo.VertexColors.Num() + AddVerts);
		Geo.Indices.Reserve(Geo.Indices.Num() + AddIndices);

		for (const FTransform& Xform : Src.Instances)
		{
			// Selection gate.
			if (MinRadiusCm > 0.0f &&
				FragmentProxyTiers::WorldRadiusCm(Src.LocalRadiusCm, Xform) < MinRadiusCm)
			{
				continue;
			}

			const int32 IndexOffset = Geo.Positions.Num();
			for (int32 i = 0; i < NumVerts; ++i)
			{
				Geo.Positions.Add(FVector3f(Xform.TransformPosition(FVector(Src.Positions[i]))));
				Geo.Normals.Add(Src.Normals.IsValidIndex(i)
					? FVector3f(Xform.TransformVectorNoScale(FVector(Src.Normals[i])))
					: FVector3f(0, 0, 1));
				Geo.TangentsX.Add(Src.TangentsX.IsValidIndex(i)
					? FVector3f(Xform.TransformVectorNoScale(FVector(Src.TangentsX[i])))
					: FVector3f(1, 0, 0));
				Geo.TangentYSigns.Add(Src.TangentYSigns.IsValidIndex(i) ? Src.TangentYSigns[i] : 1.0f);
				Geo.UV0.Add(Src.UV0.IsValidIndex(i) ? Src.UV0[i] : FVector2f::ZeroVector);
				Geo.VertexColors.Add(Src.Colors.IsValidIndex(i) ? Src.Colors[i] : FColor::White);
			}
			for (int32 t = 0; t + 2 < Src.Indices.Num(); t += 3)
			{
				Geo.Indices.Add(Src.Indices[t]     + IndexOffset);
				Geo.Indices.Add(Src.Indices[t + 1] + IndexOffset);
				Geo.Indices.Add(Src.Indices[t + 2] + IndexOffset);
			}
		}
	}

	// A slot whose every instance fell below the cutoff contributes no section.
	Out.RemoveAll([](const FExpandedSection& S) { return S.Geo.Indices.Num() < 3; });
	return Out;
}

bool FragmentCellMerge::SimplifyTemplate(
	FMergeSource& InOut, float Ratio, int32 MinSimplifyTris, float WeldToleranceCm)
{
	if (Ratio >= 1.0f) { return false; }

	const int32 Tris = InOut.Indices.Num() / 3;
	if (Tris < MinSimplifyTris) { return false; }

	// Adapt FMergeSource's parallel arrays into the decimator's section POD.
	FragmentProxyDecimate::FDecimateSection Geo;
	Geo.Positions     = InOut.Positions;
	Geo.Normals       = InOut.Normals;
	Geo.TangentsX     = InOut.TangentsX;
	Geo.TangentYSigns = InOut.TangentYSigns;
	Geo.UV0           = InOut.UV0;
	Geo.VertexColors  = InOut.Colors;
	Geo.Indices       = InOut.Indices;

	// Weld within one element template so touching objects cannot be fused.
	if (!FragmentProxyDecimate::DecimateSection(Geo, Ratio, WeldToleranceCm))
	{
		return false;
	}

	InOut.Positions     = MoveTemp(Geo.Positions);
	InOut.Normals       = MoveTemp(Geo.Normals);
	InOut.TangentsX     = MoveTemp(Geo.TangentsX);
	InOut.TangentYSigns = MoveTemp(Geo.TangentYSigns);
	InOut.UV0           = MoveTemp(Geo.UV0);
	InOut.Colors        = MoveTemp(Geo.VertexColors);
	InOut.Indices       = MoveTemp(Geo.Indices);
	// LocalRadiusCm deliberately untouched — see the header comment.
	return true;
}

FFragmentCellMergeManager::~FFragmentCellMergeManager()
{
	// Subsystem Deinitialize should have called WaitForAllMerges() before this.
	FScopeLock Lock(&PendingLock);
	if (PendingByModel.Num() > 0)
	{
		UE_LOG(LogFragCellMerge, Warning,
			TEXT("CellMergeManager destroyed with %d models still tracked. "
			     "WaitForAllMerges() should have been called first."),
			PendingByModel.Num());
	}
}

void FFragmentCellMergeManager::EnqueueMerge(
	const FString& ModelGuid,
	const FString& CellLabel,
	TArray<FMergeSource> Sources,
	TArray<UMaterialInterface*> Materials,
	TArray<FCellSubLodSpec> SubLods,
	FragmentProxyDecimate::FDecimateSettings DecimateSettings,
	int32 MinSimplifyTris,
	float CollisionDecimateRatio,
	TFunction<void(UStaticMesh*)> Finalizer,
	float WeldToleranceCm)
{
	check(IsInGameThread());

	if (Sources.Num() == 0)
	{
		if (Finalizer) Finalizer(nullptr);
		return;
	}

	// An empty spec list would author zero descriptions and produce a mesh with no LODs.
	if (SubLods.Num() == 0)
	{
		UE_LOG(LogFragCellMerge, Warning,
			TEXT("EnqueueMerge: empty SubLods for cell %s; falling back to one full-detail sub-LOD."),
			*CellLabel);
		SubLods.Add(FCellSubLodSpec{});
	}

	// The cell must belong to a world, or there is nothing to merge for.
	UWorld* OuterForMesh = OuterWorld.Get();
	if (!OuterForMesh && GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.World() && Ctx.World()->IsGameWorld())
			{
				OuterForMesh = Ctx.World();
				break;
			}
		}
	}
	if (!OuterForMesh)
	{
		UE_LOG(LogFragCellMerge, Warning,
			TEXT("EnqueueMerge: no world for the merged mesh, skipping cell %s. The owner "
			     "set no outer world and no game world exists."), *CellLabel);
		if (Finalizer) Finalizer(nullptr);
		return;
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);

	// Keep alive during worker phase; finalize unroots after the Finalizer hands ownership to the MergedComponent UPROPERTY.
	Mesh->AddToRoot();

	// Material slots must be added on the game thread (GetStaticMaterials is UProperty).
	for (int32 SlotIdx = 0; SlotIdx < Materials.Num(); ++SlotIdx)
	{
		UMaterialInterface* Mat = Materials[SlotIdx];
		FName SlotName = *FString::Printf(TEXT("CellMergeSlot_%d"), SlotIdx);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Mat, SlotName));
	}

	TWeakObjectPtr<UStaticMesh> WeakMesh(Mesh);
	const int32 NumMaterialSlots = Materials.Num();
	const FString CellLabelCopy = CellLabel;   // capture-by-value into worker

	UE::Tasks::FTask MergeTask = UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[
			S = MoveTemp(Sources),
			NumMaterialSlots,
			SubLods = MoveTemp(SubLods),
			DecimateSettings,
			MinSimplifyTris,
			CollisionDecimateRatio,
			WeldToleranceCm,
			WeakMesh,
			Finalizer = MoveTemp(Finalizer),
			CellLabelCopy,
			Sink = FinalizeSink
		]() mutable
		{
			FragmentCellMerge::FMergeWorkerResult Result =
				FragmentCellMerge::MergeMeshOnWorker(
					MoveTemp(S), NumMaterialSlots, MoveTemp(SubLods), DecimateSettings,
					MinSimplifyTris, CollisionDecimateRatio, WeldToleranceCm);

			// Fire-and-forget; WeakMesh guard below handles model-unload races.
			AsyncTask(ENamedThreads::GameThread,
				[
					WeakMesh,
					MDs = MoveTemp(Result.MeshDescriptions),
					ScreenSizes = MoveTemp(Result.LODScreenSizes),
					TriMesh = MoveTemp(Result.TriMesh),
					Finalizer,
					CellLabelCopy,
					Sink = MoveTemp(Sink)
				]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FragCellMerge_Enqueue);
					check(IsInGameThread());

					FPendingMeshFinalize Payload;
					Payload.WeakMesh        = WeakMesh;
					Payload.MeshDescriptions = MoveTemp(MDs);
					Payload.LODScreenSizes   = MoveTemp(ScreenSizes);
					Payload.TriMesh         = MoveTemp(TriMesh);
					Payload.Finalizer       = MoveTemp(Finalizer);
					Payload.DebugLabel      = CellLabelCopy;

					// Sum across ALL sub-LODs: this figure paces the budgeted finalize drain, and counting only sub-LOD 0 would under-estimate the build cost and reintroduce the frame hitches pacing exists to prevent.
					int32 TotalTris = 0;
					for (const TUniquePtr<FMeshDescription>& MD : Payload.MeshDescriptions)
					{
						if (MD.IsValid()) { TotalTris += MD->Triangles().Num(); }
					}
					Payload.EstimatedTriangleCount = TotalTris;

					// Vertex census: the MeshDescription sink materialises one render vertex per triangle CORNER, so corners is the count that drives buffer size while vertices is the count a sharing sink could achieve.
					if (Payload.MeshDescriptions.Num() > 0 && Payload.MeshDescriptions[0].IsValid())
					{
						const FMeshDescription& MD0 = *Payload.MeshDescriptions[0];
						Payload.SourceVertexCount = MD0.Vertices().Num();
						Payload.CornerCount       = MD0.VertexInstances().Num();
					}

					if (Sink)
					{
						Sink(MoveTemp(Payload));
					}
					else
					{
						UE_LOG(LogFragCellMerge, Warning,
							TEXT("Cell merge finalize sink not set; dropping %s (finalizer fired null)."),
							*CellLabelCopy);
						if (Payload.Finalizer) { Payload.Finalizer(nullptr); }
						if (UStaticMesh* M = Payload.WeakMesh.Get()) { M->RemoveFromRoot(); }
					}
				});
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	{
		FScopeLock Lock(&PendingLock);
		FPendingMerge Pending;
		Pending.WeakMesh = WeakMesh;
		Pending.MergeTask = MergeTask;
		PendingByModel.FindOrAdd(ModelGuid).Add(MoveTemp(Pending));
	}
}

void FFragmentCellMergeManager::CancelModelMerges(const FString& ModelGuid)
{
	check(IsInGameThread());

	FScopeLock Lock(&PendingLock);
	if (TArray<FPendingMerge>* Found = PendingByModel.Find(ModelGuid))
	{
		// UE::Tasks (5.3) doesn't support cancellation; in-flight workers run to completion, and their finalizes bail when WeakMesh is stale.
		PendingByModel.Remove(ModelGuid);
	}
}

void FFragmentCellMergeManager::WaitForAllMerges()
{
	check(IsInGameThread());

	TArray<UE::Tasks::FTask> AllTasks;
	{
		FScopeLock Lock(&PendingLock);
		for (auto& Pair : PendingByModel)
		{
			for (FPendingMerge& Pending : Pair.Value)
			{
				if (Pending.MergeTask.IsValid())
				{
					AllTasks.Add(Pending.MergeTask);
				}
			}
		}
	}

	// Each worker queued a fire-and-forget AsyncTask finalize; those drain on subsequent game-thread ticks and the WeakMesh guard handles teardown races.
	for (UE::Tasks::FTask& Task : AllTasks)
	{
		Task.Wait();
	}

	FScopeLock Lock(&PendingLock);
	PendingByModel.Empty();
}

int32 FFragmentCellMergeManager::GetPendingMergeCount() const
{
	FScopeLock Lock(&PendingLock);
	int32 Total = 0;
	for (const auto& Pair : PendingByModel)
	{
		Total += Pair.Value.Num();
	}
	return Total;
}

int32 FFragmentCellMergeManager::GetInFlightMergeCount() const
{
	FScopeLock Lock(&PendingLock);
	int32 InFlight = 0;
	for (const auto& Pair : PendingByModel)
	{
		for (const FPendingMerge& Pending : Pair.Value)
		{
			if (Pending.MergeTask.IsValid() && !Pending.MergeTask.IsCompleted())
			{
				++InFlight;
			}
		}
	}
	return InFlight;
}
