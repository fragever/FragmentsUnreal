// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Tasks/Task.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
#include "Spatial/FragmentProxyDecimate.h"
#include "Spatial/FragmentProxyTiers.h"
#include "Spatial/FragmentCellCensus.h"
#include "Spatial/FragmentCellMergeManager.h"   // FCellSubLodSpec (BuildCellSubLodSpecs' return type)
#include "Utils/FrameBudgetCoordinator.h"
#include "Utils/PendingMeshFinalize.h"
#include "Importer/DeferredPackageSaveManager.h"
#include "Importer/FragmentImportProgressSnapshot.h"
#include "Importer/FragmentModelWrapper.h"   // FModelBufferPin
#include "Utils/FragmentsLog.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Fragment/Fragment.h"
#include "FragmentsImporter.generated.h"

// Forward Declarations
class UFragmentModelWrapper;
class AFragment;
class UHierarchicalInstancedStaticMeshComponent;
class UFragmentRegistrationManager;

// Use FlatBuffers Model type
using Model = ::Model;

/** Outcome of the per-build cost gate in the finalize drain pump. */
enum class EFinalizeGate : uint8 { Run, Defer, RunAlone };

/** Callback fired when fragment loading completes. */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(
	FOnProcessLoadedFragmentComplete,
	bool, bSuccess,
	const FString&, ErrorMessage,
	const FString&, ModelGuid
);

/** Fired when an async fragment load finishes. */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(
	FOnFragmentLoadComplete,
	bool, bSuccess,
	const FString&, ErrorMessage,
	const FString&, ModelGuid
);

UCLASS()
class FRAGMENTSUNREAL_API UFragmentsImporter :public UObject
{
	GENERATED_BODY()

public:
	UFragmentsImporter();

	/** Flush queued mesh finalizers before garbage collection releases this importer. */
	virtual void BeginDestroy() override;

	void SetOwnerRef(AActor* NewOwnerRef) { OwnerRef = NewOwnerRef; }
	void GetItemData(AFragment*& InFragment);
	void GetItemData(FFragmentItem* InFragmentItem);
	TArray<FItemAttribute> GetItemPropertySets(AFragment* InFragment);
	TArray<FItemAttribute> GetItemPropertySets(FFragmentItem* InFragment);
	TArray<FItemAttribute> GetItemPropertySets(int64 LocalId, const FString& InModelGuid);
	TArray<FItemAttribute> GetItemAttributes(int64 LocalId, const FString& InModelGuid);
	AFragment* GetItemByLocalId(int64 LocalId, const FString& ModelGuid);
	FFragmentItem* GetFragmentItemByLocalId(int64 LocalId, const FString& InModelGuid);
	AFragment* GetModelFragment(const FString& ModelGuid);
	FTransform GetBaseCoordinates() { return BaseCoordinates; }
	void ResetBaseCoordinates();
	/** Adopt a saved base before reloading saved models, so they land where they were imported. */
	void SeedBaseCoordinates(const FTransform& InBase);
	void ResetAll();

	/** Called by FragmentRegistrationManager when camera enters a building's trigger sphere. */
	UFUNCTION()
	void HandleInteriorLoadRequested(const FString& ModelGuid);

	void ReleaseRefToWorld(UWorld* World);
	bool IsFragmentLoaded(const FString& ModelGuid);

	/** Unified lookup by LocalId. */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FFindResult FindFragmentByLocalIdUnified(int64 LocalId, const FString& ModelGuid);

	/** Read a .frag file from disk and inflate it if compressed. */
	static bool ReadAndInflate(const FString& FragPath,
		TArray<uint8>& OutDecompressed,
		FString& OutError);

	/** Build the model from an already-decompressed buffer: wrapper construction, LoadModel, root transform, overrides sidecar, structure map, sample grouping and the per-item build. */
	FString LoadFragmentFromBuffer(TArray<uint8>&& Decompressed,
		const FString& FragPath,
		const FFragmentLoadOptions& Options);

	/** Load Fragment Model and stores in Wrapper. */
	FString LoadFragment(const FString& FragPath,
		const FFragmentLoadOptions& Options = FFragmentLoadOptions());

	/** Load a Fragment model without blocking the game thread. */
	void LoadFragmentAsyncNative(const FString& FragPath,
		const FFragmentLoadOptions& Options,
		TFunction<void(bool, const FString&, const FString&)> OnComplete);

	/** Process a Loaded Fragment model using ModelGuid. */
	void ProcessLoadedFragment(const FString& ModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		AFragment* BucketRoot = nullptr, FOnProcessLoadedFragmentComplete OnComplete = FOnProcessLoadedFragmentComplete());
	void ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		AFragment* BucketRoot = nullptr);
	TArray<int64> GetElementsByCategory(const FString& InCategory, const FString& ModelGuid);
	void UnloadFragment(const FString& ModelGuid);

	/** Cancel tracking of in-flight LOD2 proxy build tasks for a model. */
	void CancelProxyBuilds(const FString& ModelGuid);

	/** Block until all in-flight LOD2 proxy worker tasks complete, INCLUDING any new tasks their own game-thread completions register (flush -> FinalizeProxyMesh -> EnqueueProxyMeshBuild is a two-deep chain). */
	void WaitForAllProxyBuilds();

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}
	// Non-const version.
	FORCEINLINE TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels_Mutable()
	{
		return FragmentModels;
	}

	// Get a specific fragment model by GUID
	FORCEINLINE class UFragmentModelWrapper* GetFragmentModel(const FString& ModelGuid) const
	{
		if (const auto* Found = FragmentModels.Find(ModelGuid))
		{
			return *Found;
		}
		return nullptr;
	}

	// Get owner actor reference
	FORCEINLINE AActor* GetOwnerRef() const
	{
		return OwnerRef.Get();
	}

	/** The worker-thread cell-merge coordinator, created on first use. */
	class FFragmentCellMergeManager* GetCellMergeManager();

	/** Block until in-flight cell merges complete, then release the manager. */
	void ShutdownCellMerges();

	/** Ensure the ~60 Hz registration streaming tick is running against InWorld. */
	void EnsureRegistrationStreamingTicking(UWorld* InWorld);

	/** The world this importer's models live in: OwnerRef's world, else the streaming world recorded by EnsureRegistrationStreamingTicking, else UObject::GetWorld(). */
	UWorld* ResolveModelWorld() const;

#if WITH_AUTOMATION_TESTS
	/** Test seam: true while the registration streaming timer is live on the resolved world. */
	bool IsRegistrationStreamingTickingForTesting() const;
	/** Test seam: record a model's bucket root, as the spawn path does when a model finishes. */
	void AddBucketRootForTesting(const FString& ModelGuid, AFragment* Root) { ModelBucketRoots.Add(ModelGuid, Root); }
	AFragment* GetBucketRootForTesting(const FString& ModelGuid) const { return GetBucketRootForModel(ModelGuid); }
#endif

	/** Give an AFragment the root component every path expects it to have, if it lacks one. */
	static USceneComponent* EnsureRootComponent(AFragment& Root);

	/** Get the registration manager (owns per-model LOD tier and per-cell LOD state). */
	FORCEINLINE UFragmentRegistrationManager* GetRegistrationManager() const
	{
		return RegistrationManager;
	}

	/** COMMIT PHASE: Create a UStaticMesh from FFragmentMeshData via BuildFromMeshDescriptions(bFastBuild=true). */
	static UStaticMesh* CommitToMeshDescription(
		const FFragmentMeshData& MeshData,
		UObject* Outer,
		UMaterialInterface* Material,
		const FString& ModelGuid);

	/** Pure cost-gate decision (unit-tested). */
	static EFinalizeGate DecideFinalizeGate(float RemainingMs, float EstMs, float FullBudgetMs);

	/** Linear estimate of BuildFromMeshDescriptions cost from triangle count, scaled by r.Fragments.Finalize.MsPer1kTris. */
	static float EstimateBuildMs(int32 TriangleCount);

	/** Build the (cell, bShell) work-list honoring the shell-first guard. Static/pure for tests. */
	static void BuildCellMergeWorkList(
		TConstArrayView<FCellId> ShellKeys, TConstArrayView<FCellId> InteriorKeys,
		bool& bShellEnqueued, TArray<TPair<FCellId, bool>>& OutWork);

	/** What a completed chunked spawn should start next. */
	enum class EPostSpawnAction : uint8
	{
		DrainDeferredModel,   // a model load was parked mid-spawn; start it
		ActivateInterior,     // no parked model; activate a deferred interior
		Idle                  // nothing queued
	};

	/** Decide what runs after ANY chunked spawn completes -- shell import or deferred interior alike. */
	static EPostSpawnAction DecidePostSpawnAction(int32 NumDeferredModels, int32 NumPendingInteriors);

	/** Push a completed worker result onto the game-thread finalize queue. */
	void EnqueueMeshFinalize(FPendingMeshFinalize&& Payload);

	/** True iff this finalize took long enough to be worth a log line. */
	static bool ShouldLogFinalizeOutlier(float ElapsedMs, float ThresholdMs);

	/** Runs the residual game-thread build for one finalize: BuildFromMeshDescriptions, attach the worker-cooked trimesh to a UBodySetup, then invoke the Finalizer. */
	void RunMeshFinalize(FPendingMeshFinalize& Payload);

	/** Route a finalize payload to the paced queue, or build it inline when r.Fragments.Finalize.Paced is off. */
	void DispatchMeshFinalize(FPendingMeshFinalize&& Payload);

#if WITH_EDITOR
	/** Read-only picture of ForModelGuid's place in the chunked-spawn pipeline, for progress UI. */
	FFragmentImportProgressSnapshot GetImportProgressSnapshot(const FString& ForModelGuid) const;
#endif // WITH_EDITOR

	/** Cost-gated drain of PendingMeshFinalizes against the frame budget. */
	void DrainMeshFinalizes(float MaxSliceMs = TNumericLimits<float>::Max());

	/** Count of queued-but-not-yet-built finalizes (test/diagnostic accessor). */
	int32 NumPendingMeshFinalizes() const { return PendingMeshFinalizes.Num(); }

	/** Drains PendingMeshFinalizes without building anything: for each queued payload, unroots its WeakMesh (if still valid) and invokes Finalizer(nullptr). */
	void FlushMeshFinalizes();

	/** Result of one async LOD0 shell build, delivered on the game thread. */
	struct FShellBuildResult
	{
		/** Built mesh, or null if authoring/build failed. */
		UStaticMesh* Mesh = nullptr;

		/** Local-space geometry for LOD2 proxy accumulation. */
		FFragmentMeshData ProxyData;
	};

	/** Authors one LOD0 per-element shell mesh on a background worker and finalizes it through the budget-paced queue. */
	void EnqueueShellMeshBuild(
		const FString& SamplePath,
		const Shell* ShellRef,
		const FColor& VertexColor,
		UMaterialInterface* SlotMaterial,
		bool bNeedsProxyData,
		FModelBufferPin BufferPin,
		TFunction<void(FShellBuildResult&&)> OnComplete);

	/** Registers an item against an in-flight build for SamplePath. */
	bool RegisterShellBuildWaiter(const FString& SamplePath, FPendingHISMItem&& Item);

	/** Game-thread completion for one async shell build: caches the mesh (even if null), stashes proxy data, drains all waiters, and clears the registry entry. */
	void CompleteShellBuild(const FString& SamplePath, FShellBuildResult&& Result);

	/** Number of shell builds launched but not yet completed. */
	int32 NumInFlightShellBuilds() const { return InFlightShellBuilds.Num(); }

	/** Backpressure cap for concurrent LOD0 shell builds: 2x worker threads, clamped to [8, 128]. */
	static int32 ResolveMaxInFlightShellBuilds(int32 NumWorkerThreads);

	/** True while any shell build is in flight — bucket finalize must wait, or PendingBucketInstances and the proxy accumulator would be incomplete. */
	bool ShouldDeferBucketFinalize() const { return InFlightShellBuilds.Num() > 0; }

	/** MeshCache lookup (test/diagnostic accessor). */
	UStaticMesh* FindCachedMesh(const FString& SamplePath) const
	{
		UStaticMesh* const* Found = MeshCache.Find(SamplePath);
		return Found ? *Found : nullptr;
	}

	/** Count of accumulated bucket instances across all keys (test/diagnostic accessor). */
	int32 NumPendingBucketInstances() const
	{
		int32 Total = 0;
		for (const TPair<FFragBucketKey, TArray<FPendingHISMItem>>& Pair : PendingBucketInstances)
		{
			Total += Pair.Value.Num();
		}
		return Total;
	}

	// Forward declarations -- full definitions live below, alongside RecordProxyContribution.
	struct FProxyContribution;
	struct FResolvedProxyContribution;

#if WITH_DEV_AUTOMATION_TESTS
	/** TEST-ONLY SEAM — compiled out of every non-test configuration (Shipping, Test, and any non-dev-automation build). */
	void SetCurrentHISMWrapperForTests(UFragmentModelWrapper* Wrapper) { CurrentHISMWrapper = Wrapper; }

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	void StashProxyMeshDataForTests(const FString& SamplePath, const FFragmentMeshData& MeshData)
	{
		ProxyMeshDataCache.Add(
			SamplePath,
			MakeShared<const FFragmentMeshData, ESPMode::ThreadSafe>(MeshData));
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	const FProxyMeshAccumulator* GetProxyAccumulatorForTests(const FString& ModelGuid) const
	{
		const TArray<FProxyMeshAccumulator>* Tiers = ProxyAccumulators.Find(ModelGuid);
		return (Tiers && Tiers->Num() > 0) ? &(*Tiers)[0] : nullptr;
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	void PurgeInFlightShellBuildsForModelForTests(const FString& ModelGuid)
	{
		PurgeInFlightShellBuildsForModel(ModelGuid);
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	void PurgeModelStateForTests(const FString& ModelGuid, bool bRemoveFromMap)
	{
		PurgeModelState(ModelGuid, bRemoveFromMap);
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	void BeginChunkedSpawnForTests(const FString& ModelGuid)
	{
		bIsHISMChunkedSpawning = true;
		CurrentSpawningModelGuid = ModelGuid;
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	TArray<FResolvedProxyContribution> ResolveProxyContributionsForTests(
		const TArray<FProxyContribution>& SortedContributions) const
	{
		return ResolveProxyContributions(SortedContributions);
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	void LaunchProxyContributionFlushForTests(const FString& ModelGuid, AFragment* BucketRoot)
	{
		LaunchProxyContributionFlush(ModelGuid, BucketRoot);
	}

	/** TEST-ONLY SEAM — compiled out of every non-test configuration. */
	TArray<UE::Tasks::FTask> GetPendingProxyBuildTasksForTests(const FString& ModelGuid)
	{
		FScopeLock Lock(&PendingProxyBuildsLock);
		if (const TArray<UE::Tasks::FTask>* Tasks = PendingProxyBuilds.Find(ModelGuid))
		{
			return *Tasks;
		}
		return {};
	}

	/** TEST-ONLY. */
	int32 ProxyFlushPublishCountForTests = 0;

	/** TEST-ONLY SEAM. */
	void LoadFragmentAsyncWithHandlerForTests(const FString& FragPath,
		TFunction<void(bool, const FString&, const FString&)> Handler)
	{
		LoadFragmentAsyncNative(FragPath, FFragmentLoadOptions(), MoveTemp(Handler));
	}

	/** TEST-ONLY SEAM. */
	void SetAsyncLoadInFlightForTests(bool bInFlight) { bAsyncLoadInFlight = bInFlight; }

	/** TEST-ONLY SEAM. */
	bool LoadFragmentAsyncBlockingForTests(const FString& FragPath)
	{
		bLastAsyncFired = false;
		LoadFragmentAsyncWithHandlerForTests(FragPath,
			[this](bool bSuccess, const FString& Error, const FString& Guid)
			{
				bLastAsyncSuccess = bSuccess;
				LastAsyncError = Error;
				LastAsyncGuid = Guid;
				bLastAsyncFired = true;
			});

		if (AsyncLoadTask.IsValid())
		{
			AsyncLoadTask.Wait();
		}
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		return bLastAsyncFired;
	}

	bool LastAsyncSuccessForTests() const { return bLastAsyncSuccess; }
	const FString& LastAsyncErrorForTests() const { return LastAsyncError; }
	const FString& LastAsyncGuidForTests() const { return LastAsyncGuid; }

	/** Buffered contribution OrderKeys, sorted. */
	TArray<int64> GetSortedProxyContributionKeysForTests(const FString& ModelGuid) const;
#endif // WITH_DEV_AUTOMATION_TESTS

	/** Buffers one element's proxy contribution (see FProxyContribution). */
	void RecordProxyContribution(const FString& ModelGuid, int64 OrderKey, const FString& SamplePath,
		const FTransform& WorldTransform, uint32 MatCRC, UMaterialInterface* Material);

	/** One sample's deferred contribution to its model's LOD2 proxy. */
	struct FProxyContribution
	{
		// FPendingHISMItem::QueueIndex — unique per sample by construction (unlike LocalId, which repeats across every sample of one element).
		int64 OrderKey = INDEX_NONE;
		FString SamplePath;
		FTransform WorldTransform;
		uint32 MatCRC = 0;
		UMaterialInterface* Material = nullptr;
	};

	/** One proxy contribution with its mesh data already resolved, ready to hand to a worker. */
	struct FResolvedProxyContribution
	{
		TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe> MeshData;
		FTransform WorldTransform;
		uint32 MatCRC = 0;
		UMaterialInterface* Material = nullptr;
	};

	/** Replay a resolved payload into one accumulator PER TIER, in strict index order. */
	static void AccumulateResolvedContributions(
		const TArray<FResolvedProxyContribution>& Payload,
		TConstArrayView<FragmentProxyTiers::FTierSpec> Tiers,
		TArray<FProxyMeshAccumulator>& OutTiers);

	/** Single-tier convenience: keeps every element. */
	static void AccumulateResolvedContributions(
		const TArray<FResolvedProxyContribution>& Payload,
		FProxyMeshAccumulator& OutAccumulator);

	/** Replays buffered contributions into the model's accumulator in OrderKey order. */
	void FlushProxyContributions(const FString& ModelGuid);

	/** Append each tier description and ScreenSize together to keep arrays aligned. */
	static void AppendCompactedTierLOD(
		int32 TierIndex,
		TUniquePtr<FMeshDescription> MD,
		float ScreenSize,
		TArray<TUniquePtr<FMeshDescription>>& OutDescriptions,
		TArray<float>& OutScreenSizes,
		bool bForceAppend = false);

	/** Drops trailing empty tiers (Tiers.Last().IsEmpty()) from the back of Tiers, stopping at one entry. */
	static void PruneTrailingEmptyTiers(TArray<FProxyMeshAccumulator>& Tiers);

	/** Parse ascending proxy tier near edges as multiples of model radius. */
	static TArray<float> ResolveNearEdgesCm(
		float ModelBoundsRadiusCm, const FString& MulSpec, const FString& AbsSpec);

	/** Production entry point: reads r.Fragments.Proxy.TierNearEdgeRadiiMul and r.Fragments.Proxy.TierNearEdgesCm and delegates to the pure overload above. */
	static TArray<float> ResolveNearEdgesCm(float ModelBoundsRadiusCm);

	/** Derive tier cutoffs from screen error and near-edge distance. */
	static TArray<FragmentProxyTiers::FTierSpec> BuildProxyTierSpecs(
		int32 NumTiers, float OmitPx, TConstArrayView<float> NearEdgesCm, float SimplifyRatio);

	/** Derive ScreenSize and MinRadiusCm from the same near-edge calibration. */
	static void FillProxyTierScreenSizes(
		TArray<FragmentProxyTiers::FTierSpec>& Specs, TConstArrayView<float> NearEdgesCm, float ProxyBoundsRadiusCm);

	/** Number of leading proxy render tiers that can never be selected for this model, given its own measured numbers. */
	static int32 CountUnreachableLeadingTiers(
		TConstArrayView<float> TierScreenSizes, float ProxyBoundsRadiusCm, float PromoteCm);

	/** Calibrate near and far cell LODs against promote and model-demote distances. */
	static TArray<FCellSubLodSpec> BuildCellSubLodSpecs(
		float NearEdgeCm, float ModelBoundsRadiusCm, float CellBoundsRadiusCm,
		float OmitPx, float GeoCoeffCells, float ThresholdPx, float CoarsenFactor);

	/** Pixel budget for cell sub-LOD selection. */
	static float ResolveCellOmitPx(bool bShellSide, float SharedOmitPx, float InteriorOmitPx);

	/** Production entry point: reads r.Fragments.Proxy.OmitPx and r.Fragments.Interior.OmitPx and delegates to the pure overload above. */
	static float ResolveCellOmitPx(bool bShellSide);

	/** Apply the shell/interior simplification gate to already-resolved decimate settings. */
	static FragmentProxyDecimate::FDecimateSettings ApplyShellSimplifyGate(
		const FragmentProxyDecimate::FDecimateSettings& In, bool bShellSide, bool bSimplifyShell);

	/** Write the accumulated census CSV to Saved/Profiling and clear the accumulator. */
	void FlushCellCensus();

private:

	/** True between an accepted LoadFragmentAsync and its completion. */
	bool bAsyncLoadInFlight = false;

	/** Task handle for the in-flight read+inflate worker, so teardown can drain it. */
	UE::Tasks::FTask AsyncLoadTask;

#if WITH_DEV_AUTOMATION_TESTS
	bool bLastAsyncFired = false;
	bool bLastAsyncSuccess = false;
	FString LastAsyncError;
	FString LastAsyncGuid;
#endif

	void PurgeModelState(const FString& ModelGuid, bool bRemoveFromMap);

	/** Drops ModelGuid's waiters from InFlightShellBuilds without touching any other resident model's entries. */
	void PurgeInFlightShellBuildsForModel(const FString& ModelGuid);
	void CollectPropertiesRecursive(const Model* InModel, int64 StartLocalId, TSet<int64>& Visited, TArray<FItemAttribute>& OutAttributes);
	void SpawnStaticMesh(UStaticMesh* StaticMesh, const Transform* LocalTransform, const Transform* GlobalTransform, AActor* Owner, FName OptionalTag = FName());

	// Unified SpawnFragmentModel — recursive HISM spawn with optional BucketRoot reuse.
	AFragment* SpawnFragmentModel(FFragmentItem InFragmentItem, AActor* InParent, const Meshes* MeshesRef,
		bool bSaveMeshes, UFragmentModelWrapper* InWrapperRef = nullptr,
		AFragment* BucketRoot = nullptr, int64 ParentFloorKey = INDEX_NONE);

	void RegisterDirectHISMBuckets(AFragment* BucketRoot);
	UWorld* ResolveProcessingWorld() const;
	void StartProgressiveHISMRegistration(AFragment* BucketRoot);

	UStaticMesh* CreateStaticMeshFromCircleExtrusion(
		const CircleExtrusion* CircleExtrusion,
		const Material* RefMaterial,
		const FString& AssetName,
		UObject* OuterRef,
		const FString& InModelGuid = FString()
	);

	FName AddMaterialToMesh(UStaticMesh*& CreatedMesh, const Material* RefMaterial, const FString& InModelGuid = FString());

	static bool IsPlacementCarrier(const FString& Cat);

	void BuildFullCircleExtrusion(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion, const Material* RefMaterial, UStaticMesh* StaticMesh);

	void BuildLineOnlyMesh(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion);

	TArray<FVector> SampleRingPoints(const FVector& Center, const FVector XDir, const FVector& YDir, float Radius, int SegmentCount, float ApertureRadians);

	void SavePackagesWithProgress(const TArray<UPackage*>& InPackagesToSave);

	// HISM chunked spawning

	/** Build flat spawn queue from hierarchy for HISM chunked spawning */
	void BuildHISMSpawnQueue(const FFragmentItem& InItem, const FTransform& ParentWorldTransform, int64 ParentFloorKey);

	/** Process a chunk of the HISM spawn queue per frame */
	void ProcessHISMSpawnChunk();

	/** One-shot: rebuild bucket map after orphan remap; runs before chunked drain. */
	void BeginFinalizeHISMBuckets();

	/** Drain up to BucketsPerChunk buckets per tick. Returns true when fully drained. */
	bool ProcessFinalizeBucketsChunk();

	/** Cleanup only: clears pending bucket + paced-tail state after the tail completes. */
	void EndFinalizeHISMBuckets();

	void ApplyMaterialOverrides(AFragment* BucketRoot, UFragmentModelWrapper* Wrapper);

	/** Finalize a single bucket: create HISMC, batch add instances, register */
	void FinalizeSingleHISMBucket(const FFragBucketKey& Key, const TArray<FPendingHISMItem>& Items);

	/** Inflate cell bounds, collapse sparse cells, remap orphans, and rebucket. */
	void FinalizePartitionCells();

	/** Log per-category bucket/instance frequency table from PendingBucketInstances. */
	void LogCategoryFrequencyTable() const;

	/** Snapshot each cell's bucket geometry and enqueue merge jobs on FFragmentCellMergeManager. */
	void EnqueueEagerCellMerges();

	/** Begin the paced cell-merge enqueue phase: build the work-list, set FinalizeTailPhase. */
	void BeginCellMergeEnqueue();

	/** Drain the cell-merge enqueue work-list under the frame budget. Returns true when done. */
	bool ProcessCellMergeEnqueueChunk();

	/** Begin the paced override-apply phase: snapshot the HISMC list, set FinalizeTailPhase. */
	void BeginApplyOverrides();

	/** Apply override materials under the frame budget. Returns true when done. */
	bool ProcessApplyOverridesChunk();

	/** Take ModelGuid's buffered contributions, sort them, resolve them against ProxyMeshDataCache, and launch one worker to accumulate them. */
	void LaunchProxyContributionFlush(const FString& ModelGuid, AFragment* BucketRoot);

	/** Resolve each sorted contribution's SamplePath through ProxyMeshDataCache into a worker-ready payload, preserving input order exactly. */
	TArray<FResolvedProxyContribution> ResolveProxyContributions(
		const TArray<FProxyContribution>& SortedContributions) const;

	/** Read r.Fragments.Proxy.{Tiers,OmitPx,TierNearEdgesCm,TierNearEdgeRadiiMul,Decimate,DecimateRatio} and build this import's tier specs via BuildProxyTierSpecs. */
	TArray<FragmentProxyTiers::FTierSpec> ResolveProxyTierSpecs(float ModelBoundsRadiusCm) const;

	/** Snapshot + enqueue one cell's merge. */
	void EnqueueOneCellMerge(
		const FCellId& Cell, bool bShellSide,
		class FFragmentCellMergeManager* Mgr, UFragmentRegistrationManager* RegMgr,
		AFragment* BucketRoot, const FString& ModelGuid,
		const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Buckets,
		float ModelBoundsRadiusCm);

	// Per-frame tick for the registration streaming path (called by timer).
	void TickRegistrationStreaming();

	// Extract geometry from shell representation
	void ExtractShellGeometry(
		const Shell* ShellRef,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs
	);

	// Legacy SetRenderData pipeline; current mesh authoring uses BuildFromMeshDescriptions.

	/** DATA PHASE: Build thread-safe mesh data from raw FlatBuffers Shell. */
	FFragmentMeshData BuildShellMeshDataFromFlatBuffers(
		const Shell* ShellRef,
		const Material* RefMaterial);

	/** LEGACY (SetRenderData path) — no longer used by the synchronous import path, which now goes through CommitToMeshDescription. */
	static UStaticMesh* CommitToRenderData(
		const FFragmentMeshData& MeshData,
		UObject* Outer,
		UMaterialInterface* Material,
		const FString& ModelGuid);

	/** Attach the custom runtime complex collision (ComplexAsSimple triangle mesh) to a freshly built StaticMesh. */
	static void AttachRuntimeComplexCollision(
		UStaticMesh* StaticMesh,
		const FFragmentMeshData& MeshData,
		const FString& ModelGuid);

	/** Async proxy mesh build. */
	void EnqueueProxyMeshBuild(
		const FString& ModelGuid,
		TArray<FProxyMeshAccumulator>& Tiers,
		TConstArrayView<FragmentProxyTiers::FTierSpec> TierSpecs,
		float ModelBoundsRadiusCm,
		TFunction<void(UStaticMesh*)> OnComplete);

private:

	UPROPERTY()
	TWeakObjectPtr<AActor> OwnerRef = nullptr;

	/** Last world used by registration streaming when OwnerRef is unavailable. */
	TWeakObjectPtr<UWorld> StreamingWorld;

	/** Worker-thread cell-merge coordinator. See GetCellMergeManager. */
	TUniquePtr<FFragmentCellMergeManager> CellMergeManager;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseGlassMaterial = nullptr;

	UPROPERTY()
	TMap<FString, class UFragmentModelWrapper*> FragmentModels;

	UPROPERTY()
	TMap<FString, FFragmentLookup> ModelFragmentsMap;

	/** Manages per-HISMC registration streaming based on camera distance. */
	UPROPERTY()
	TObjectPtr<UFragmentRegistrationManager> RegistrationManager = nullptr;

	UPROPERTY()
	TMap<FString, UStaticMesh*> MeshCache;

	// Representation-based mesh cache (Key = RepresentationId)
	UPROPERTY()
	TMap<int32, UStaticMesh*> RepresentationMeshCache;

	// Materials cache
	UPROPERTY()
	TMap<FString, UMaterialInstanceConstant*> MaterialsCache;

	// BaseCoordinates tracking
	UPROPERTY()
	bool bBaseCoordinatesInitialized = false;

	UPROPERTY()
	bool bSaveMaterials = false;

	UPROPERTY()
	FTransform BaseCoordinates;

	UPROPERTY()
	FVector SceneZeroZOffset = FVector::ZeroVector;

	UPROPERTY()
	TArray<UPackage*> PackagesToSave;

	FDeferredPackageSaveManager DeferredSaveManager;

	// Deferred model loads — queued when a chunked HISM spawn is already in progress
	struct FDeferredModelLoad
	{
		FString ModelGuid;
		TWeakObjectPtr<AActor> Owner;
		bool bSaveMeshes;
		AFragment* BucketRoot;
		FOnProcessLoadedFragmentComplete OnCompleteCallback;
	};
	TArray<FDeferredModelLoad> DeferredModelLoads;

	/** Start the next parked model load. */
	bool DrainNextDeferredModelLoad();

	// SHELL-FIRST STREAMING.

	/** Defer non-shell (interior) items to a distance-triggered second spawn pass. */
	UPROPERTY(EditAnywhere, Category = "Fragments|Streaming")
	bool bEnableShellFirstStreaming = false;

	/** Deferred interior items per model, keyed by ModelGuid. */
	TMap<FString, TArray<FPendingHISMItem>> DeferredInteriorQueues;

	/** Maps ModelGuid → BucketRoot actor. Populated when each model's shell import completes. */
	UPROPERTY()
	TMap<FString, AFragment*> ModelBucketRoots;

	/** Resolve the BucketRoot actor for a model by GUID. */
	AFragment* GetBucketRootForModel(const FString& ModelGuid) const;

	/** True when the current HISM spawn run is processing deferred interior items (not initial shell). */
	bool bIsProcessingDeferredInteriors = false;

	/** Interior activations deferred because a shell spawn was in progress. */
	TArray<FString> PendingInteriorActivations;

	// HLOD PROXY GENERATION.

	/** Master toggle for proxy mesh generation during shell import. */
	UPROPERTY(EditAnywhere, Category = "Fragments|HLOD")
	bool bGenerateProxyMesh = true;

	/** Decimate the whole-building proxy mesh on the worker (FragmentProxyDecimate::DecimateSection, attribute-aware; preserves UV0 + tangents). */
	UPROPERTY(EditAnywhere, Category = "Fragments|HLOD")
	bool bDecimateProxyMesh = false;

	/** Fraction of original proxy triangles to keep per section (0.0 to 1.0). Default 0.1 = 10%. */
	UPROPERTY(EditAnywhere, Category = "Fragments|HLOD", meta = (ClampMin = "0.05", ClampMax = "1.0", EditCondition = "bDecimateProxyMesh"))
	float ProxyDecimationRatio = 0.1f;

	/** Template-floored simplification for LOD1 merged cells. */
	UPROPERTY(EditAnywhere, Category = "Fragments|HLOD")
	bool bDecimateCellMerge = true;

	/** Fraction of original triangles to keep per merged cell (0.0 to 1.0). */
	UPROPERTY(EditAnywhere, Category = "Fragments|HLOD", meta = (ClampMin = "0.05", ClampMax = "1.0", EditCondition = "bDecimateCellMerge"))
	float CellMergeDecimationRatio = 0.5f;

	/** Accumulated only while r.Fragments.CellMerge.Census is non-zero. */
	FragmentCellCensus::FCensusReport CellCensusReport;

	/** Per-model proxy mesh accumulators, keyed by ModelGuid. */
	TMap<FString, TArray<FProxyMeshAccumulator>> ProxyAccumulators;

	/** Per-model buffered proxy contributions, flushed sorted at FinalizeProxyMesh. */
	TMap<FString, TArray<FProxyContribution>> PendingProxyContributions;

	/** Monotonic per-model flush generation. */
	TMap<FString, uint32> ProxyFlushEpoch;

	/** Per-model in-flight LOD2 proxy build tasks. */
	TMap<FString, TArray<UE::Tasks::FTask>> PendingProxyBuilds;
	mutable FCriticalSection PendingProxyBuildsLock;

	/** Stashed FFragmentMeshData for proxy accumulation on mesh cache hits. */
	TMap<FString, TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe>> ProxyMeshDataCache;

	/** Kicks off async LOD2 proxy build for the model. */
	void FinalizeProxyMesh(const FString& ModelGuid, AFragment* BucketRoot);

	// HISM CHUNKED SPAWNING MEMBERS
	TArray<FPendingHISMItem> PendingHISMQueue;
	TMap<FFragBucketKey, TArray<FPendingHISMItem>> PendingBucketInstances;
	UPROPERTY()
	AFragment* CurrentBucketRoot = nullptr;
	bool bIsHISMChunkedSpawning = false;
	int32 HISMItemsSpawned = 0;
	int32 TotalHISMItems = 0;
	FTimerHandle HISMSpawnTimerHandle;

	/** Monotonic counter handed out as FPendingHISMItem::QueueIndex, one increment per sample, across BuildHISMSpawnQueue's entire recursive traversal for one model. */
	int32 NextHISMQueueIndex = 0;

	/** Items waiting on one in-flight LOD0 shell build. */
	struct FPendingShellBuild
	{
		TArray<FPendingHISMItem> Waiters;
	};

	/** SamplePath -> in-flight build. */
	TMap<FString, FPendingShellBuild> InFlightShellBuilds;

	/** Consecutive ProcessHISMSpawnChunk ticks the finalize barrier has deferred bucket finalize because ShouldDeferBucketFinalize() was still true. */
	int32 ConsecutiveBucketFinalizeDeferrals = 0;

	/** Downstream steps for one item once its mesh exists: proxy accumulation, cell partition, bucket key. */
	/** Consumes Item (it is moved into PendingBucketInstances), hence the rvalue ref. */
	void ProcessResolvedHISMItem(FPendingHISMItem&& Item, UStaticMesh* Mesh, UMaterialInterface* MatIF);

	/** True once PendingHISMQueue is drained and we are chunk-finalizing buckets. */
	bool bIsFinalizingBuckets = false;

	/** Snapshot of bucket keys to finalize this batch, captured in BeginFinalizeHISMBuckets. */
	TArray<FFragBucketKey> FinalizeBucketKeys;

	/** Cursor into FinalizeBucketKeys. */
	int32 FinalizeBucketCursor = 0;

	/** Tail phases run after the chunked bucket finalize, budget-paced across ticks. */
	enum class EFinalizeTailPhase : uint8 { None, EnqueueCellMerges, ApplyOverrides };
	EFinalizeTailPhase FinalizeTailPhase = EFinalizeTailPhase::None;

	/** Work-list of (cell, bShell) pairs to enqueue as cell merges, drained per tick. */
	TArray<TPair<FCellId, bool>> CellMergeEnqueueQueue;
	int32 CellMergeEnqueueCursor = 0;

	/** The model bounds radius for CellMergeEnqueueQueue's model, snapshotted once in BeginCellMergeEnqueue. */
	float CellMergeModelBoundsRadiusCm = 0.0f;

	/** Snapshot of (HISMC, source color) to apply override materials to, drained per tick. */
	TArray<TPair<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor>> OverrideApplyQueue;
	int32 OverrideApplyCursor = 0;

	/** Drained per tick during chunked finalize. ~0.015 ms per bucket -> ~4 ms budget. */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 BucketsPerChunk = 256;
	/** Wrapper ref for material resolution during HISM chunked spawning */
	UPROPERTY()
	UFragmentModelWrapper* CurrentHISMWrapper = nullptr;

	// Meshes referencce for spawning
	const Meshes* CurrentMeshesRef = nullptr;

	// Save meshes flag
	bool bCurrentSaveMeshes = false;

	// Current model GUID being spawned
	FString CurrentSpawningModelGuid;

	// Timer Handle for the per-frame registration streaming tick.
	FTimerHandle RegistrationUpdateTimerHandle;

	// One-shot polling timer that fires Trace.Stop once all interior cell merges have drained after RegisterNewComponents.
	FTimerHandle ImportTraceStopPollHandle;

	// How many fragments to spawn per tick
	int32 FragmentsPerChunk = 10;

	// Current spawn progress (0.0 to 1.0)
	float SpawnProgress = 0.0f;

	// Total fragments to spawn
	int32 TotalFragmentsToSpawn = 0;

	// Fragments spawned thus far
	int32 FragmentsSpawned = 0;

	// FRAME BUDGET COORDINATION.

	/** Frame budget coordinator for unified time budgeting across geometry and spawning */
	FFrameBudgetCoordinator FrameBudgetCoordinator;

	/** Game-thread-only finalize queue (FIFO). */
	TArray<FPendingMeshFinalize> PendingMeshFinalizes;

	/** Total frame budget in milliseconds across all processing (geometry + spawning) */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "1.0", ClampMax = "16.0"))
	float TotalFrameBudgetMs = 4.0f;

	/** Enable adaptive budget adjustment based on actual frame times */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance")
	bool bEnableAdaptiveBudget = true;

	/** Maximum number of NEW mesh creations per frame (cache hits are unlimited). */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "0", ClampMax = "100"))
	int32 MaxNewMeshCreationsPerFrame = 0;

	/** Counter for new mesh creations this frame (reset in TickRegistrationStreaming) */
	int32 NewMeshCreationsThisFrame = 0;

	/** Editor fallback registration queue used when there is no runtime player controller. */
	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> PendingEditorHISMRegistrations;

	/** Max editor HISM RegisterComponent calls per frame when no player controller exists. */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "1", ClampMax = "500"))
	int32 MaxEditorHISMRegistrationsPerFrame = 50;

	/** Check if we can create another new mesh this frame */
	FORCEINLINE bool CanCreateNewMesh() const
	{
		return (MaxNewMeshCreationsPerFrame == 0) || (NewMeshCreationsThisFrame < MaxNewMeshCreationsPerFrame);
	}

	/** Increment the new mesh creation counter */
	FORCEINLINE void OnNewMeshCreated()
	{
		NewMeshCreationsThisFrame++;
	}

	/** Material pool for CRC-based deduplication */
	UPROPERTY()
	TMap<uint32, UMaterialInstanceDynamic*> MaterialPool;

	/** Hash material properties for pooling */
	uint32 HashMaterialProperties(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass) const;

public:
	/** Get or create pooled material instance */
	UMaterialInstanceDynamic* GetPooledMaterial(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass);
private:

	// Helpers.

	/** Suppress expensive engine subsystems during fragment import (Lumen, DF, nav, GC). */
	void SuppressImportOverhead();

	/** Restore suppressed subsystems after all models finish importing. */
	void RestoreImportOverhead();

	/** Load material overrides from the sidecar JSON into Wrapper->MaterialOverrides. */
	void LoadOverridesIntoWrapper(UFragmentModelWrapper* Wrapper, const FString& InModelGuid);

	UPROPERTY()
	FOnProcessLoadedFragmentComplete OnProcessLoadedFragmentComplete;

};

namespace UE::Fragments::ImporterResolver
{
	/** Bound by FragmentsEditor at module startup so world contexts with no GameInstance can still find an importer. */
	DECLARE_DELEGATE_RetVal_OneParam(UFragmentsImporter*, FResolveEditorImporter, UWorld*);

	FRAGMENTSUNREAL_API FResolveEditorImporter& GetEditorResolver();

	/** GameInstance subsystem first, then the editor provider. Null when neither answers. */
	FRAGMENTSUNREAL_API UFragmentsImporter* Resolve(UObject* WorldContext);
}
