// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "HAL/CriticalSection.h"
#include "Tasks/Task.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "MeshDescription.h"
#include "Chaos/ImplicitFwd.h"
#include "Utils/PendingMeshFinalize.h"
#include "Spatial/FragmentProxyDecimate.h"

class UStaticMesh;
class UMaterialInterface;
struct FStaticMeshLODResources;

/** One LOD1 sub-LOD. */
struct FCellSubLodSpec
{
	/** Omit instances whose world bounding radius falls below this. 0 keeps everything. */
	float MinRadiusCm = 0.0f;
	/** Written to FStaticMeshRenderData::ScreenSize[i].Default. */
	float ScreenSize = 1.0f;
};

/** Worker-safe vertex snapshot for one (Mesh, Material) source bucket plus the per-instance world transforms that emit it into the merged buffer. */
struct FMergeSource
{
	// Per-bucket vertex template (shared across instances).
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> TangentsX;
	TArray<float>     TangentYSigns;   // +1 or -1; bitangent reconstructed in finalize
	TArray<FVector2f> UV0;
	TArray<FColor>    Colors;

	/** Triangle list, indices into Positions/Normals/etc. */
	TArray<uint32> Indices;

	/** Per-instance world transforms. */
	TArray<FTransform> Instances;

	/** Slot index into the merged mesh's material array. */
	int32 MaterialSlot = 0;

	/** Local bounding-sphere radius of the vertex template, in cm. */
	float LocalRadiusCm = 0.0f;
};

namespace FragmentCellMerge
{
	/** One expanded, world-space merged section: all instances of all sources sharing a MaterialSlot, flattened into a decimatable POD. */
	struct FExpandedSection
	{
		int32 MaterialSlot = 0;
		FragmentProxyDecimate::FDecimateSection Geo;
	};

	/** Expand each FMergeSource template x its Instances into flat world-space sections grouped by MaterialSlot (ascending), OMITTING any instance whose world bounding radius falls below MinRadiusCm. */
	FRAGMENTSUNREAL_API TArray<FExpandedSection> ExpandSourcesToSections(
		TConstArrayView<FMergeSource> Sources, int32 NumMaterialSlots, float MinRadiusCm);

	/** Simplify one TEMPLATE in place, before expansion. */
	FRAGMENTSUNREAL_API bool SimplifyTemplate(
		FMergeSource& InOut, float Ratio, int32 MinSimplifyTris, float WeldToleranceCm = 0.0f);

	/** Worker output: one authored FMeshDescription per sub-LOD, ready for BuildFromMeshDescriptions on the game thread, plus a cooked Chaos trimesh ready to attach to BodySetup->TriMeshGeometries. */
	struct FMergeWorkerResult
	{
		TArray<TUniquePtr<FMeshDescription>> MeshDescriptions;
		TArray<float>                        LODScreenSizes;   // parallel; same length
		/** Cooked trimesh to attach on the game thread; null if the merge produced no triangles. */
		Chaos::FTriangleMeshImplicitObjectPtr TriMesh;
		/** True when the collision trimesh was cooked from sub-LOD 0's own authoring output rather than from a separate expansion. */
		bool bCollisionReusedSubLod0 = false;
	};

	/** Worker-thread mesh merge. */
	FRAGMENTSUNREAL_API FMergeWorkerResult MergeMeshOnWorker(
		TArray<FMergeSource> Sources,
		int32 NumMaterialSlots,
		TArray<FCellSubLodSpec> SubLods,
		FragmentProxyDecimate::FDecimateSettings DecimateSettings,
		int32 MinSimplifyTris,
		float CollisionDecimateRatio,
		float WeldToleranceCm = 0.0f);
}

/** Manages worker-thread cell-merge static mesh construction. */
class FRAGMENTSUNREAL_API FFragmentCellMergeManager
{
public:
	FFragmentCellMergeManager() = default;
	~FFragmentCellMergeManager();

	// Non-copyable, non-movable (tasks reference `this` indirectly via captures).
	FFragmentCellMergeManager(const FFragmentCellMergeManager&) = delete;
	FFragmentCellMergeManager& operator=(const FFragmentCellMergeManager&) = delete;

	/** The world the merged cells belong to. */
	void SetOuterWorld(UWorld* InWorld) { OuterWorld = InWorld; }

	/** Dispatch a cell merge on a worker thread. */
	void EnqueueMerge(
		const FString& ModelGuid,
		const FString& CellLabel,
		TArray<FMergeSource> Sources,
		TArray<UMaterialInterface*> Materials,
		TArray<FCellSubLodSpec> SubLods,
		FragmentProxyDecimate::FDecimateSettings DecimateSettings,
		int32 MinSimplifyTris,
		float CollisionDecimateRatio,
		TFunction<void(UStaticMesh*)> Finalizer,
		float WeldToleranceCm = 0.0f);

	/** Cancel all pending tasks for a model. Game thread only. Safe to call repeatedly. */
	void CancelModelMerges(const FString& ModelGuid);

	/** Block until all in-flight worker tasks complete. Game thread only. */
	void WaitForAllMerges();

	/** For stats: cumulative count of enqueued tasks across all models. */
	int32 GetPendingMergeCount() const;

	/** Count of enqueued tasks whose worker has not yet finished — drops to 0 once every worker is done. */
	int32 GetInFlightMergeCount() const;

	/** Injected by the owning subsystem: routes a completed worker result to the importer's budget-paced finalize queue. */
	void SetFinalizeSink(TFunction<void(FPendingMeshFinalize&&)> Sink) { FinalizeSink = MoveTemp(Sink); }

private:
	struct FPendingMerge
	{
		TWeakObjectPtr<UStaticMesh> WeakMesh;
		UE::Tasks::FTask MergeTask;   // worker phase; finalize via AsyncTask(GameThread)
	};

	TMap<FString /*ModelGuid*/, TArray<FPendingMerge>> PendingByModel;
	mutable FCriticalSection PendingLock;

	TFunction<void(FPendingMeshFinalize&&)> FinalizeSink;

	/** See SetOuterWorld. */
	TWeakObjectPtr<UWorld> OuterWorld;
};
