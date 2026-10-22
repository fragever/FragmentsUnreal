// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/CollisionProfile.h"
#include "Engine/HitResult.h"
#include "Utils/FragmentsUtils.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "FragmentsImporterSubsystem.generated.h"

class UFragmentModelWrapper;

// Cached component state for visibility toggling
struct FComponentSavedState
{
    FName CollisionProfile;
    ECollisionEnabled::Type CollisionEnabled;
    bool bGenerateOverlapEvents = false;
    bool bTickEnabled = false;
    bool bHiddenInGame = false;
};

UCLASS()
class FRAGMENTSUNREAL_API UFragmentsImporterSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	/** Load a Fragment model synchronously. */
	UFUNCTION(BlueprintCallable, meta = (AutoCreateRefTerm = "Options",
		DeprecatedFunction,
		DeprecationMessage = "Synchronous load stalls the game thread (442ms avg, 3.7s worst). Use LoadFragmentAsync. This node will be removed once async loading gains progress, cancel, and batch support."))
	FString LoadFragment(const FString& FragPath,
		const FFragmentLoadOptions& Options);

	/** Load a Fragment model without blocking the game thread. */
	UFUNCTION(BlueprintCallable, meta = (AutoCreateRefTerm = "Options"))
	void LoadFragmentAsync(const FString& FragPath,
		const FFragmentLoadOptions& Options,
		FOnFragmentLoadComplete OnComplete);

	UFUNCTION(BlueprintCallable)
	void UnloadFragment(const FString& ModelGuid);

	/** Process a Loaded Fragment model using ModelGuid. */
	UFUNCTION(BlueprintCallable)
	void ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh, FOnProcessLoadedFragmentComplete OnComplete,
		AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	void ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	TArray<int64> GetElementsByCategory(const FString& InCategory, const FString& ModelGuid);

	UFUNCTION(BlueprintCallable)
	AFragment* GetItemByLocalId(int64 InLocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemPropertySets(int64 LocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemAttributes(int64 LocalId, const FString& InModelGuid);

	/** Get basic element identity by LocalId. Returns struct with LocalId=-1 if not found. */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FFragmentElementInfo GetElementInfo(int64 LocalId, const FString& ModelGuid);

	/** Get model-level metadata JSON string. Returns empty string if model not loaded. */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FString GetModelMetadata(const FString& ModelGuid);

	/** Get model-level GUID (different from per-element GUIDs). */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FString GetModelGuid(const FString& ModelGuid);

	/** Get full element detail (identity + all attributes + property sets) in a single call. */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FFragmentElementDetail GetElementDetail(int64 LocalId, const FString& ModelGuid);

	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FFindResult FindFragmentByLocalIdUnified(int64 LocalId, const FString& ModelGuid);

	FFragmentItem* GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid);
	void GetItemData(FFragmentItem* InFragmentItem);

	UFUNCTION(BlueprintCallable)
	AFragment* GetModelFragment(const FString& InModelGuid);

	/** Get the importer instance (owns import, and the LOD2 proxy / LOD1 cell-merge builds). */
	FORCEINLINE UFragmentsImporter* GetImporter() const { return Importer; }

#if WITH_AUTOMATION_TESTS
	/** Inject (or null out) the importer from an automation test. */
	void SetImporterForTesting(UFragmentsImporter* InImporter) { Importer = InImporter; }
#endif

	/** Get the cell-merge manager. */
	FORCEINLINE FFragmentCellMergeManager* GetCellMergeManager() const
	{
		return Importer ? Importer->GetCellMergeManager() : nullptr;
	}

	/** Hot-promote on click: hide the merged LOD1 proxy of the cell containing the hit's component and let per-instance HISMC cull distance govern LOD0 visibility. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HLOD")
	bool PromoteCellAndRetryHit(const FHitResult& Hit);

	// Stats

	/** Number of fragment models currently loaded. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Fragments|Stats")
	int32 GetLoadedModelCount() const { return FragmentModels.Num(); }

	/** Total instance count across all HISMCs in the given model's BucketRoot. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Fragments|Stats")
	int32 GetModelInstanceCount(const FString& ModelGuid) const;

	/** Find a model wrapper by GUID. */
	UFragmentModelWrapper* GetModelWrapper(const FString& ModelGuid) const;

	/** PIE/packaged self-refresh: when a level-serialized BucketRoot starts play, reload its source .frag at runtime and destroy carried-over editor copies. */
	void RefreshLevelSerializedRoot(AFragment* Root);

	/** Unique IFC category strings for the given model, sorted. Empty if not loaded. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Fragments|Categories")
	TArray<FString> GetCategoriesForModel(const FString& ModelGuid) const;

	/** Show only floors in [MinFloor, MaxFloor] for the given model. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Visibility")
	void SetFloorVisibilityRange(const FString& ModelGuid, int32 MinFloor, int32 MaxFloor);

	/** It Try to get the index of the FloorId in ModelGuid. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Visibility")
	bool TryGetFloorIndexForLocalId(const FString& ModelGuid, int64 LocalId, int32& OutFloorIndex) const;

	/** Dim instances whose IFC category is NOT in VisibleCategories by writing per-instance custom data slot 1. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Visibility")
	void SetCategoryFilter(const FString& ModelGuid, const TArray<FString>& VisibleCategories, float DimmedAlpha);

	/** Dump unique IFC category strings (with per-category element counts) for loaded models to the Output Log. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Debug")
	void DumpCategories(const FString& ModelGuidOrEmpty) const;

	/** Console handler for Fragments.DumpCategories. */
	static void HandleDumpCategoriesCommand(const TArray<FString>& Args, UWorld* World);

	/** Walk the model item tree and dump every element of the given IFC category's full attribute set to the Output Log. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Debug")
	void DumpAttributesByCategory(const FString& Category, const FString& ModelGuidOrEmpty);

	/** Convenience wrapper for the original verification flow: DumpAttributesByCategory("IFCSPACE", ...). */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Debug")
	void DumpSpaceAttributes(const FString& ModelGuidOrEmpty);

	/** Console handler for Fragments.DumpSpaceAttributes. Args: optional ModelGuid. */
	static void HandleDumpSpaceAttributesCommand(const TArray<FString>& Args, UWorld* World);

	/** Console handler for Fragments.DumpAttributesByCategory. Args: <Category> [ModelGuid]. */
	static void HandleDumpAttributesByCategoryCommand(const TArray<FString>& Args, UWorld* World);

	UFUNCTION(BlueprintCallable)
	FTransform GetBaseCoordinates();

	UFUNCTION(BlueprintCallable)
	void ResetBaseCoordinates();

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}

	/** Cancels all multi-frame processing associated with the current viewer world. */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	void AbortAllProcessing();

	/** Cancel work and release every model/registration owned by the current viewer world. */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	void ResetViewerSession();

	// Visibility Management.

	static void SetHierarchyVisible(AActor* Root, bool bVisible)
	{
		if (!IsValid(Root)) return;

		if (bVisible)
		{
			RestoreActor(Root);
		}
		else
		{
			SaveAndHideActor(Root);
		}

		TArray<AActor*> Attached;
		Root->GetAttachedActors(Attached);
		for (AActor* Child : Attached)
		{
			SetHierarchyVisible(Child, bVisible);
		}
	}

	static bool IsHierarchyVisible(const AActor* Root)
	{
		return IsValid(Root) ? !Root->IsHidden() : false;
	}

	// User visibility.

	/** Hide/show a model in every LOD tier (LOD2 proxy, LOD1 merged cells, LOD0 instances), including shadows and trace collision, while preserving its view-derived LOD tier, floor filters and cooked collision. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Visibility")
	bool SetModelUserVisible(const FString& ModelGuid, bool bVisible);

	/** True unless SetModelUserVisible(false) hid this model. True for an unregistered GUID. */
	UFUNCTION(BlueprintPure, Category = "Fragments|Visibility")
	bool IsModelUserVisible(const FString& ModelGuid) const;

protected:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:

	UPROPERTY()
	TMap<FString, class UFragmentModelWrapper*> FragmentModels;

	UPROPERTY()
	class UFragmentsImporter* Importer = nullptr;

	// Visibility state caches (static for global access)

	static TMap<TWeakObjectPtr<AActor>, bool>& ActorTickCache()
	{
		static TMap<TWeakObjectPtr<AActor>, bool> S;
		return S;
	}

	static TMap<TWeakObjectPtr<UPrimitiveComponent>, FComponentSavedState>& Cache()
	{
		static TMap<TWeakObjectPtr<UPrimitiveComponent>, FComponentSavedState> S;
		return S;
	}

	static void SaveAndHideActor(AActor* A)
	{
		if (!IsValid(A)) return;

		if (!ActorTickCache().Contains(A))
		{
			ActorTickCache().Add(A, A->IsActorTickEnabled());
		}

		A->SetActorTickEnabled(false);
		A->SetActorHiddenInGame(true);

		TInlineComponentArray<UPrimitiveComponent*> PrimComps;
		A->GetComponents(PrimComps);
		for (UPrimitiveComponent* C : PrimComps)
		{
			if (!IsValid(C)) continue;

			if (!Cache().Contains(C))
			{
				FComponentSavedState State;
				State.CollisionProfile = C->GetCollisionProfileName();
				State.CollisionEnabled = C->GetCollisionEnabled();
				State.bGenerateOverlapEvents = C->GetGenerateOverlapEvents();
				State.bTickEnabled = C->IsComponentTickEnabled();
				State.bHiddenInGame = C->bHiddenInGame;
				Cache().Add(C, State);
			}

			C->SetHiddenInGame(true);
			C->SetGenerateOverlapEvents(false);
			C->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetComponentTickEnabled(false);
		}
	}

	static void RestoreActor(AActor* A)
	{
		if (!IsValid(A)) return;

		TInlineComponentArray<UPrimitiveComponent*> PrimComps;
		A->GetComponents(PrimComps);
		for (UPrimitiveComponent* C : PrimComps)
		{
			if (!IsValid(C)) continue;

			if (FComponentSavedState* Saved = Cache().Find(C))
			{
				C->SetHiddenInGame(Saved->bHiddenInGame);
				C->SetCollisionProfileName(Saved->CollisionProfile);
				C->SetCollisionEnabled(Saved->CollisionEnabled);
				C->SetGenerateOverlapEvents(Saved->bGenerateOverlapEvents);
				C->SetComponentTickEnabled(Saved->bTickEnabled);
				Cache().Remove(C);
			}
			else
			{
				C->SetHiddenInGame(false);
				C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				C->SetGenerateOverlapEvents(true);
				C->SetComponentTickEnabled(C->PrimaryComponentTick.bCanEverTick);
			}
		}

		const bool* bTick = ActorTickCache().Find(A);
		if (bTick) { A->SetActorTickEnabled(*bTick); ActorTickCache().Remove(A); }
		else { A->SetActorTickEnabled(A->PrimaryActorTick.bCanEverTick); }

		A->SetActorHiddenInGame(false);
	}
};
