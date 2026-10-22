// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// Manages cell visibility, model LOD, and glass occlusion.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Utils/FragmentCellPartition.h"
#include "Selection/FragmentSelectionTypes.h"
#include "Spatial/FragmentModelTier.h"
#include "FragmentRegistrationManager.generated.h"

class AFragment;
class UFragmentModelWrapper;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMeshComponent;
struct FHitResult;

/** Per-cell LOD state for the cell-merge state machine. */
UENUM()
enum class ECellLodState : uint8
{
	/** Merged proxy mesh hidden (or not yet built). */
	Active_LOD0,

	/** Merged proxy mesh visible. */
	Merged_LOD1,

	/** Out of frustum or beyond cull — nothing visible for this cell. */
	Skipped,
};

/** Glass occluder flip state for a single HISMC. */
UENUM()
enum class EGlassOccluderState : uint8
{
	NotGlass,
	Transparent,
	Occluding,
};

/** Tracks registration state for a single HISMC. */
USTRUCT()
struct FHISMCRegistrationState
{
	GENERATED_BODY()

	/** The HISMC being tracked. */
	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component = nullptr;

	/** Cull distance copied from the HISMC's SetCullDistances end value. */
	UPROPERTY()
	float CullDistance = 0.0f;

	/** Whether the component is currently registered with the scene. */
	UPROPERTY()
	bool bIsRegistered = false;

	/** Cached world-space bounds center (HISMC bounds never change — instances are set once). */
	UPROPERTY()
	FVector CachedBoundsOrigin = FVector::ZeroVector;

	/** Cached bounding sphere radius. */
	UPROPERTY()
	float CachedBoundsRadius = 0.0f;

	/** Tagged as occludable glass (IFCPLATE) by the importer. Set once at RegisterModel. */
	UPROPERTY()
	bool bIsOccludableGlass = false;

	/** Current glass occluder flip state. */
	UPROPERTY()
	EGlassOccluderState OccluderState = EGlassOccluderState::NotGlass;

	/** True for structural IFC categories (walls, slabs, roofs, beams, columns, stairs). */
	UPROPERTY()
	bool bIsStructural = false;

	/** Stashed from the bucket key so RebuildCellHISMCIndex doesn't need to reverse-scan Buckets or do a ComponentTag round-trip. */
	UPROPERTY()
	FCellId CellId;

	UPROPERTY()
	bool bIsShell = false;

	/** When true, EvaluateOneHISMC keeps QueryOnly collision regardless of spatial gating distance. */
	UPROPERTY()
	bool bForceCollision = false;
};

/** Tracks per-cell state for cell-first iteration in UpdateRegistration. */
USTRUCT()
struct FCellRegistrationData
{
	GENERATED_BODY()

	UPROPERTY() FCellId Cell;

	UPROPERTY() FBox WorldBounds = FBox(ForceInit);

	/** Current LOD state. Active_LOD0 by default until the state machine demotes. */
	UPROPERTY() ECellLodState LodState = ECellLodState::Active_LOD0;

	/** The merged static mesh component for this cell. */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> MergedComponent = nullptr;

	/** True between EnqueueMerge and Finalizer callback. */
	UPROPERTY() bool bMergeInFlight = false;

	/** Hot-promote sticky bit. */
	UPROPERTY() bool HotPromoteOverride = false;

	/** HISMCs that belong to this cell. */
	UPROPERTY() TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMCs;

	/** Forces DecideCellTargetState to return Skipped regardless of distance or HotPromoteOverride. */
	UPROPERTY() bool bFilterHidden = false;
};

/** Tracks all HISMCs for a single loaded model. */
USTRUCT()
struct FModelRegistrationData
{
	GENERATED_BODY()

	/** The bucket root actor that owns the HISMCs. */
	UPROPERTY()
	TObjectPtr<AFragment> BucketRoot = nullptr;

	/** All HISMCs and their registration state. */
	UPROPERTY()
	TArray<FHISMCRegistrationState> Components;

	/** Union bounding box center of all shell HISMCs — defines building footprint. */
	UPROPERTY()
	FVector ModelBoundsOrigin = FVector::ZeroVector;

	/** Union bounding box half-extents (axis-aligned). Tighter than sphere for rectangular buildings. */
	UPROPERTY()
	FVector ModelBoundsExtent = FVector::ZeroVector;

	/** Bounding sphere radius (used for debug draw, kept for backwards compat). */
	UPROPERTY()
	float ModelBoundsRadius = 0.0f;

	/** True if this model has deferred interior items waiting to be spawned. */
	UPROPERTY()
	bool bInteriorsPending = false;

	/** True once the interior load delegate has been fired for this model. */
	UPROPERTY()
	bool bInteriorsTriggered = false;

	/** Whether Chaos physics meshes have been cooked for ALL of this model's HISMCs. */
	UPROPERTY()
	bool bCollisionCooked = false;

	/** Index into Components[] for chunked collision cooking. -1 = not cooking. 0..N = cooking in progress (next HISMC to cook). */
	UPROPERTY()
	int32 CollisionCookProgress = -1;

	/** World time when collision uncook becomes eligible (0 = no pending uncook). */
	UPROPERTY()
	float CollisionUncookEligibleTime = 0.0f;

	/** Current LOD tier. */
	FragmentModelTier::ETier Tier = FragmentModelTier::ETier::Proxy;

	/** Number of this model's cells currently hidden by a floor filter. */
	int32 FloorFilteredCellCount = 0;

	/** Click override: forces the Cells tier regardless of distance so a distant click can resolve to per-element metadata. */
	bool bPromoteOverride = false;

	/** The merged proxy static mesh component. Null if bGenerateProxyMesh was false. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> ProxyComponent = nullptr;

	/** Model GUID for reverse lookup from click events on the proxy component. */
	UPROPERTY()
	FString ModelGuid;

	/** User-facing visibility (e.g. a hidden discipline model). */
	UPROPERTY()
	bool bUserVisible = true;

	// ShellCells / InteriorCells drive the per-cell LOD state machine.

	UPROPERTY()
	TArray<FCellRegistrationData> ShellCells;

	UPROPERTY()
	TArray<FCellRegistrationData> InteriorCells;

	/** Flat index into Components[] for every HISMC on this model. */
	UPROPERTY()
	TArray<int32> AllHISMCs;
};

/** Pending glass-occluder flip request, sorted by distance-from-threshold for budget priority. */
struct FPendingGlassFlip
{
	UHierarchicalInstancedStaticMeshComponent* Component = nullptr;
	EGlassOccluderState DesiredState = EGlassOccluderState::Transparent;
	float DistanceFromThreshold = 0.0f;
};

/** Fired when camera enters a model's interior trigger sphere. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInteriorLoadRequested, const FString&, ModelGuid);

/** Per-frame view data the LOD machine needs: origin, frustum, and the pixel/FOV coefficient. */
struct FFragmentViewInfo
{
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	FConvexVolume ViewFrustum;
	bool bHaveFrustum = false;
	/** FragmentSSE::SseCoeff for this view. ScreenErrorPx multiplies geoError/dist by this. */
	float SseCoeff = 0.0f;
};

/** Manages cell and model LOD, glass occlusion, and collision cooking. */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentRegistrationManager : public UObject
{
	GENERATED_BODY()

public:
	/** Register a model's bucket root for streaming management. */
	void RegisterModel(AFragment* BucketRoot, bool bHasDeferredInteriors = false,
		UStaticMeshComponent* ProxyComponent = nullptr, UFragmentModelWrapper* InWrapper = nullptr);

	/** Attach a proxy component to an already-registered model. */
	void AttachProxyComponent(AFragment* BucketRoot, UStaticMeshComponent* ProxyComp);

	/** Return the FBox of a registered model @params ModelGuid @params OutBounds. */
	bool GetModelBounds(const FString& ModelGuid, FBox& OutBounds) const;

	/** Get the FBox of all registered models. */
	bool GetRegisteredModelsBounds(FBox& OutBounds) const;


	/** Register only NEW (not-yet-tracked) HISMCs from a bucket root. */
	void RegisterNewComponents(AFragment* BucketRoot, UFragmentModelWrapper* InWrapper = nullptr);

	/** Remove a model from streaming management. */
	void UnregisterModel(AFragment* BucketRoot);

	/** Per-frame streaming pass. */
	void UpdateRegistration(TConstArrayView<FFragmentViewInfo> Views,
	                        UWorld* World,
	                        float CollisionCookBudgetOverrideMs = -1.0f);

	/** Public so the importer's tick can build the runtime view before calling UpdateRegistration. */
	void ExtractViewInfo(APlayerController* PC, FFragmentViewInfo& OutView) const;

	/** Hide/show all cells on the given floor. */
	void SetFloorFilterForModel(const FString& ModelGuid, int64 FloorId, bool bVisible);

	/** Unique Floor values across the model's ShellCells and InteriorCells. */
	void CollectModelFloors(const FString& ModelGuid, TSet<int64>& OutFloors) const;

	/** Clear floor filters for all cells on this model. */
	void ClearFloorFilterForModel(const FString& ModelGuid);

	/** Max model tier transitions applied per UpdateRegistration tick, summed across models. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|HLOD",
		meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxModelTierTransitionsPerTick = 1;

	/** Restore proxy trace collision after actor visibility changes. */
	void RestoreProxyInteractionState(const FString& ModelGuid);

	/** Finalize callback from FFragmentCellMergeManager. */
	void AttachMergedComponent(
		const FString& ModelGuid,
		const FCellId& Cell,
		bool bShellSide,
		UStaticMesh* MergedMesh);

	/** Failure callback from FFragmentCellMergeManager. */
	void OnCellMergeFailed(
		const FString& ModelGuid,
		const FCellId& Cell,
		bool bShellSide);

	/** Hot-promote on click: flip the cell containing this hit's merged LOD1 component to Active_LOD0 by setting Cell.HotPromoteOverride and forcing the next UpdateRegistration tick (bypasses still-camera optimization). */
	bool PromoteCellAndRetryHit(const FHitResult& Hit);

	/** Click landed on a model's LOD2 proxy: set the model-scoped promote override so the model goes cell-resident on the next tick regardless of distance, bypassing the tier cap. */
	bool PromoteModelFromProxyHit(const FHitResult& Hit);

	/** Clear HotPromoteOverride and bForceCollision on every cell across every registered model. */
	void ClearAllHotPromoteOverrides();

	/** Force all cells of a model to LOD0 + enable trace collision model-wide; pair with ClearAllHotPromoteOverrides on exit. */
	void PromoteAllCellsForModel(const FString& ModelGuid);

	/** Per-model twin of ClearAllHotPromoteOverrides: undo PromoteAllCellsForModel for a single model without disturbing a HotPromoteOverride / bPromoteOverride another model holds (e.g. for click-to-select). */
	void ClearHotPromoteOverridesForModel(const FString& ModelGuid);

	/** True while the registered model holds the model-level promote override that PromoteAllCellsForModel sets. */
	bool HasModelPromoteOverride(const FString& ModelGuid) const;

	/** While a model's full-detail geometry is still being built for editing: how many of its cells are built and how many it will have. */
	bool GetLOD0BuildProgress(const FString& ModelGuid, int32& OutCellsBuilt, int32& OutCellsTotal) const;

	/** Clear HotPromoteOverride + bForceCollision on a single cell identified by Handle. */
	bool DemoteCellByHandle(const FFragmentCellHandle& Handle);

	/** Build a cell handle from a HISMC. */
	FFragmentCellHandle MakeHandleForHISMC(UHierarchicalInstancedStaticMeshComponent* HISM) const;

	/** Hide/show a model in every LOD tier: LOD2 proxy, LOD1 merged cells, LOD0 HISMCs, their shadows (including bCastHiddenShadow on merged cells) and trace collision. */
	bool SetModelUserVisible(const FString& ModelGuid, bool bVisible);

	/** Returns the user-facing visibility state. True for an unregistered GUID, matching the default. */
	bool IsModelUserVisible(const FString& ModelGuid) const;

	/** Release every registration. Call before the bucket-root actors are destroyed. */
	void ResetAllRegistrations();

	/** Drop every registration whose bucket root lives in World (or is already gone), destroying nothing. */
	void ReleaseWorld(UWorld* World);

#if !UE_BUILD_SHIPPING
	/** Dump visibility funnel snapshot to LogFragments. Bound to Frag.DumpVisibility console command. */
	void DumpVisibilitySnapshot();

	/** Dump proxy LOD selection state to LogFragments. */
	void DumpProxyLODState() const;
#endif

	/** Enable frustum-based skipping of whole cells in the per-cell merged-mesh visibility state machine. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming")
	bool bEnableFrustumCulling = true;

	/** Master toggle for distance-gated glass occluder flipping. Off = current behavior. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Glass Occluder")
	bool bEnableGlassOccluderFlip = true;

	/** Fraction of the HISMC's cull distance at which glass becomes an HZB occluder. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Glass Occluder", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GlassOccluderEnableFraction = 0.6f;

	/** Fraction of the HISMC's cull distance at which glass returns to transparent. Must be < EnableFraction. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Glass Occluder", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GlassOccluderDisableFraction = 0.4f;

	/** Max bUseAsOccluder flip operations per frame. Each flip costs a MarkRenderStateDirty(). */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Glass Occluder")
	int32 MaxGlassFlipsPerFrame = 8;

	/** Max cell LOD-state transitions applied per UpdateRegistration tick, summed across all registered models. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|HLOD",
		meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxCellTransitionsPerTick = 4;

	/** Fraction of CullDistance at which collision activates for selection/navigation. e.g., 0.3 means collision turns on within 30% of the HISMC's cull distance. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Collision", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float CollisionActivationFraction = 0.3f;

	/** Multiplier on smallest horizontal AABB extent for collision cook trigger. e.g., 1.5 means collision cooks when camera is within 1.5x the building's narrowest side beyond the AABB surface. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Collision",
		meta = (ClampMin = "0.5", ClampMax = "5.0"))
	float CollisionTriggerMultiplier = 1.5f;

	/** Outer hysteresis multiplier for collision uncook trigger. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Collision",
		meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float CollisionUncookHysteresis = 1.3f;

	/** Seconds after leaving the uncook zone before physics data is actually freed. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Collision",
		meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float CollisionCooldownSeconds = 10.0f;

	/** Time budget (ms) for collision cooking per frame across all models. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Collision",
		meta = (ClampMin = "0.5", ClampMax = "20.0"))
	float CollisionCookBudgetMs = 4.0f;

	/** Max models to uncook collision for per frame. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Collision",
		meta = (ClampMin = "1", ClampMax = "10"))
	int32 MaxCollisionUncooksPerFrame = 3;

	// Shadow casting is baked at construction by FragmentsImporter (Tier 1 Structural + Tier 2 Architectural windows/doors cast; Tier 3/4 + glass do not).

	// DEBUG / DIAGNOSTICS (stripped in shipping).

	/** Threshold (seconds) for "recently rendered" — components with GetLastRenderTimeOnScreen() > NowSeconds - this are considered visible. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Debug",
		meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float RecentlyRenderedThresholdSeconds = 0.1f;

	/** Skip the per-frame update when the camera hasn't moved. Suppresses HZB-noise shadow popping on a still camera. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Still Camera")
	bool bSkipUpdateWhenCameraStill = true;

	/** Position delta (cm) below which the camera is considered still. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Still Camera")
	float CameraStillPositionEpsilonCm = 1.0f;

	/** Rotation delta (degrees, max axis) below which the camera is considered still. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Still Camera")
	float CameraStillRotationEpsilonDeg = 0.1f;

	// SHELL-FIRST STREAMING.

	/** Fractional radius padding that triggers interior loading near a model. */
	UPROPERTY(EditAnywhere, Category = "Registration Streaming|Shell-First")
	float InteriorTriggerRadiusMultiplier = 0.1f;

	/** Fired when camera enters a model's interior trigger sphere. */
	UPROPERTY()
	FOnInteriorLoadRequested OnInteriorLoadRequested;

private:
	/** Camera state from the previous UpdateRegistration call, one entry per view. */
	struct FLastViewState
	{
		FVector  Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};
	TArray<FLastViewState> LastViewStates;
	bool bHasLastCameraState = false;

	/** Set by RegisterModel to force one full UpdateRegistration pass, bypassing the still-camera early-out. */
	bool bForceNextUpdate = false;

	/** One-shot: lift MaxCellTransitionsPerTick for the next tick. */
	bool bBypassTransitionCapNextTick = false;

	/** True if no view moved enough to warrant a per-tick pass. */
	bool ShouldSkipForStillCamera(TConstArrayView<FFragmentViewInfo> Views, bool bForcedThisTick);

	/** Queues a flip if the HISMC's distance crossed the hysteresis threshold. */
	void EvaluateGlassFlip(struct FHISMCRegistrationState& State, float DistanceToCamera);

	/** Per-HISMC maintenance — runs every tick for every HISMC. */
	void EvaluateOneHISMC(
		FHISMCRegistrationState& State,
		FModelRegistrationData& ModelData,
		int32 CompIdx,
		const FVector& CameraLocation);

	/** Sorts and applies pending flips up to MaxGlassFlipsPerFrame, then clears the queue. */
	void DrainPendingGlassFlips();

	/** Built up during UpdateRegistration, drained at the end of the same call. */
	TArray<FPendingGlassFlip> PendingGlassFlips;

	/** Find FModelRegistrationData by ModelGuid. Returns nullptr if not found. */
	FModelRegistrationData* FindModelByGuid(const FString& ModelGuid);

	/** Populate each FCellRegistrationData::HISMCs from the stashed CellId/bIsShell on FHISMCRegistrationState. */
	void RebuildCellHISMCIndex(struct FModelRegistrationData& ModelData);

	/** Mirror of the file-local SetMergedShown, for the LOD2 proxy: render visibility and trace collision together. */
	void SetProxyShown(UStaticMeshComponent* ProxyComp, bool bShown);

	/** Hide merged cells AND their HISMCs and reset LodState=Merged_LOD1 before a model returns to the Proxy tier. */
	void ResetCellsForProxyMode(struct FModelRegistrationData& ModelData);

	/** Apply a model tier change: swap proxy visibility against cell visibility and hand shadow ownership to the coarsest resident tier. */
	bool ApplyModelTierTransition(struct FModelRegistrationData& ModelData, FragmentModelTier::ETier Target);

	/** Hand shadow casting to the coarsest resident tier: proxy while Proxy-tier, merged cells while Cells-tier, never both. */
	void ApplyModelShadowOwnership(struct FModelRegistrationData& ModelData, FragmentModelTier::ETier Tier);

	/** SetModelUserVisible(true)'s reveal: replay the frozen Tier (proxy, or each cell's frozen LodState) and re-apply shadow ownership. */
	void ShowModelAtCurrentTier(struct FModelRegistrationData& ModelData);

	/** Per-model tracking data. */
	UPROPERTY()
	TArray<FModelRegistrationData> RegisteredModels;
};
