// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Utils/FragmentsUtils.h"
#include "Utils/FragmentCellPartition.h"
#include "Fragment.generated.h"

// HISM bucket system structs

USTRUCT()
struct FFragLocalids
{
	GENERATED_BODY()

	UPROPERTY() TArray<int64> LocalIds;
	UPROPERTY() TArray<int64> FloorKeys;
};

USTRUCT()
struct FBIMSourceColor
{
	GENERATED_BODY()

	UPROPERTY() uint8 R = 255;
	UPROPERTY() uint8 G = 255;
	UPROPERTY() uint8 B = 255;
	UPROPERTY() uint8 A = 255;
	UPROPERTY() bool bIsGlass = false;
};

USTRUCT()
struct FFragBucketKey
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<UStaticMesh> Mesh = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> Material = nullptr;
	// Callers MUST set bShell explicitly at every construction site.
	UPROPERTY() uint8 bShell : 1;
	// Partitions each (Mesh, Material, bShell) tuple by cell so every HISMC contains instances from exactly one cell.
	UPROPERTY() FCellId Cell;

	FFragBucketKey() : bShell(1) {}

	bool operator==(const FFragBucketKey& O) const
	{
		return Mesh == O.Mesh
			&& Material == O.Material
			&& bShell == O.bShell
			&& Cell == O.Cell;
	}
};

USTRUCT()
struct FFragInstanceRef
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Comp = nullptr;
	UPROPERTY() int32 InstanceIndex = INDEX_NONE;
};

USTRUCT()
struct FFragInstanceArray
{
	GENERATED_BODY()
	UPROPERTY() TArray<FFragInstanceRef> Items;
};

FORCEINLINE uint32 GetTypeHash(const FFragBucketKey& K)
{
	const uint32 A = HashCombine(::GetTypeHash(K.Mesh), ::GetTypeHash(K.Material));
	const uint32 B = HashCombine(A, ::GetTypeHash(K.bShell));
	return HashCombine(B, ::GetTypeHash(K.Cell));
}

UCLASS()
class FRAGMENTSUNREAL_API AFragment : public AActor
{
	GENERATED_BODY()

public:
	AFragment();

	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual bool IsListedInSceneOutliner() const override { return bShouldShowInOutliner; }
#endif
	void SetShouldShowInOutliner(bool bShow) { bShouldShowInOutliner = bShow; }

	void SetFragmentPath(const FString& InPath) { FragmentPath = InPath; }
	/** How this root's model was placed at import, saved so a reload (map reopen, Play) places it the same way. */
	void SetImportOrigin(EFragmentOriginMode InMode, const FTransform& InBase) { ImportOriginMode = InMode; ImportBaseCoordinates = InBase; }
	EFragmentOriginMode GetImportOriginMode() const { return ImportOriginMode; }
	const FTransform& GetImportBaseCoordinates() const { return ImportBaseCoordinates; }
	const FString& GetFragmentPath() const { return FragmentPath; }

	void SetModelGuid(const FString& InModelGuid) { ModelGuid = InModelGuid; }
	void SetLocalId(const int64 InLocalId) { LocalId = InLocalId; }
	void SetCategory(const FString& InCategory) { Category = InCategory; }
	void SetGuid(const FString& InGuid) { Guid = InGuid; }
	void SetAttributes(TArray<struct FItemAttribute> InAttributes) { Attributes = InAttributes; }
	void SetChildren(TArray<AFragment*> InChildren) { FragmentChildren = InChildren; }
	void AddSampleInfo(const struct FFragmentSample& Sample) { Samples.Add(Sample); }
	void SetGlobalTransform(const FTransform& InGlobalTransform) { GlobalTransform = InGlobalTransform; }
	FTransform GetGlobalTransform() const { return GlobalTransform; }
	void SetData(FFragmentItem InFragmentItem);

	void AddChild(AFragment* InChild) {
		if (InChild) FragmentChildren.Add(InChild);
	}


	// HISM instance lookup
	bool GetLocalIdForInstance(const UHierarchicalInstancedStaticMeshComponent* HISM, int32 InstanceIndex, int64& OutLocalId) const;
	void GetAllLocalIdsForBucket(const UHierarchicalInstancedStaticMeshComponent* HISM, TArray<int64>& OutLocalIds) const;
	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& GetBuckets() const { return Buckets; }
	const TMap<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor>& GetBIMSourceColors() const { return BIMSourceColors; }

#if WITH_EDITOR
	void GetSelectedLocalIdsforBucket(const UHierarchicalInstancedStaticMeshComponent* HISM, TArray<int64>& OutLocalIds) const;
#endif

	// Blueprint-callable getters
	UFUNCTION(BlueprintCallable, Category = "Fragments") FString GetModelGuid() const { return ModelGuid; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") int64 GetLocalId() const { return LocalId; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") FString GetCategory() const { return Category; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") FString GetGuid() const { return Guid; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") TArray<struct FItemAttribute> GetAttributes();
	UFUNCTION(BlueprintCallable, Category = "Fragments") TArray<AFragment*> GetChildren() const { return FragmentChildren; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") AFragment* FindFragmentByLocalId(int64 InLocalId);
	UFUNCTION(BlueprintCallable, Category = "Fragments") const TArray<struct FFragmentSample>& GetSamples() const { return Samples; }

	// HISM API
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool SetHighlightedByLocalId(int64 InLocalId, bool bHighlighted);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ClearAllHISMHighlights();
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool SetHiddenByLocalId(int64 InLocalId, bool bIsHidden);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ClearAllHISMHidden();
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool IsLocalIdHidden(int64 InLocalId) const { return HiddenElementLocalIds.Contains(InLocalId); }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") int32 GetHiddenCount() const { return HiddenElementLocalIds.Num(); }
	const TSet<int64>& GetHiddenLocalIds() const { return HiddenElementLocalIds; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ShowOnlyFloor(int64 InFloorId);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void SetFloorVisible(int64 FloorKey, bool bVisible);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ShowAllFloors();
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool IsFloorVisible(int64 InFloorId) const;

	/** Sorted unique Floor IDs for this model's cells. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM")
	TArray<int64> GetAvailableFloorIds() const;
	UFUNCTION(BlueprintCallable, Category = "Fragments|Perf") void EnableProximityCulling(float StartFadeCm, float EndCullCm, bool bAlsoApplytoNonHISM = true);
	UFUNCTION(BlueprintCallable, Category = "Fragments|Perf") void EnableProximityCullingMeters(float StartFadeM, float EndCullM, bool bAlsoApplyToNonHISM = true)
	{
		EnableProximityCulling(StartFadeM * 100.0f, EndCullM * 100.0f, bAlsoApplyToNonHISM);
	}

	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void SetHISMEnabled(bool bEnable) { bUseHISM = bEnable; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool IsHISMEnabled() const { return bUseHISM; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void SetAsBucketRoot(bool bRoot) { bBucketRoot = bRoot; }
	bool IsBucketRoot() const { return bBucketRoot; }

	/** Record the placement a chunked import baked into this root's instance transforms: InBakedFrame is the model's GlobalTransform (placement * origin frame) and InBakedPlacement the placement it was built with. */
	void SetBakedFrame(const FTransform& InBakedFrame, const FTransform& InBakedPlacement)
	{
		BakedFrame = InBakedFrame;
		BakedPlacement = InBakedPlacement;
		bHasBakedFrame = true;
	}
	/** False for an actor that carries its frame as its own transform instead. */
	bool HasBakedFrame() const { return bHasBakedFrame; }
	const FTransform& GetBakedFrame() const { return BakedFrame; }
	const FTransform& GetBakedPlacement() const { return BakedPlacement; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") AFragment* GetBucketRoot();
	int32 AddHISMInstance(UStaticMesh* Mesh, UMaterialInterface* Mat, const FTransform& WorldXd, int64 InLocalId, int64 InFloorId, bool bShell);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool ResolveHitToLocalId(const FHitResult& Hit, int64& OutLocalId) const;

	/** Check whether a line trace hit a model's LOD2 proxy, and if so set that model's promote override so it becomes cell-resident next tick. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HLOD")
	bool TryPromoteModelFromHit(const FHitResult& Hit);

	/** Promote a traced LOD1 cell to LOD0 for a retry hit. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HLOD")
	bool TryPromoteCellFromHit(const FHitResult& Hit);

	/** Gets the Bounds of the Buckets geometry. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM")
	FBox GetBucketBounds(bool bOnlyVisible = true) const;

	/** Gets the world transform of the instance by local id. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM")
	bool GetWorldTransformByLocalId(int64 InLocalId, FTransform& OutWorldTransform) const;

	/** Gets the accumulated world-space bounds for every rendered instance of a LocalId. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM")
	bool GetWorldBoundsByLocalId(int64 InLocalId, FBox& OutWorldBounds) const;

	/** Resolve a LocalId to its backing HISMC instance(s). */
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM")
	bool GetInstancesForLocalId(int64 InLocalId,
		TArray<UHierarchicalInstancedStaticMeshComponent*>& OutComps,
		TArray<int32>& OutIndices) const;

protected:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	FString ModelGuid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	int64 LocalId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	FString Category;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	FString Guid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	TArray<struct FItemAttribute> Attributes;

private:

	FFragmentItem Data;
	UPROPERTY() TArray<AFragment*> FragmentChildren;
	UPROPERTY() TArray<struct FFragmentSample> Samples;
	UPROPERTY() FTransform GlobalTransform;

	UPROPERTY(Transient) bool bShouldShowInOutliner = true;

	// Drives PIE/packaged self-refresh in BeginPlay.
	UPROPERTY() FString FragmentPath;

	// HISM bucket system
	UPROPERTY() bool bUseHISM = true;
	UPROPERTY() bool bBucketRoot = false;
	// Saved with the level: the instance transforms they describe are saved too.
	UPROPERTY() bool bHasBakedFrame = false;
	UPROPERTY() FTransform BakedFrame;
	UPROPERTY() FTransform BakedPlacement;
	UPROPERTY() TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*> Buckets;
	UPROPERTY() TMap<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor> BIMSourceColors;
	UPROPERTY() TMap<UHierarchicalInstancedStaticMeshComponent*, FFragLocalids> InstanceLocalIds;
	UPROPERTY() TMap<int64, FFragInstanceArray> LocalIdToInstance;
	UPROPERTY() TSet<int64> HighlightedLocalIds;
	UPROPERTY() TSet<int64> HiddenElementLocalIds;

	/** Currently active floor filter (-1 = no filter). */
	UPROPERTY() int64 ActiveFloorId = INDEX_NONE;

	/** Whether a floor filter is currently active. */
	UPROPERTY() bool bFloorFilterActive = false;

	UHierarchicalInstancedStaticMeshComponent* GetOrCreateBucket(UStaticMesh* Mesh, UMaterialInterface* Mat, bool bShell);

	// Last, so the offsets of every member above stay what already-built modules expect.
	UPROPERTY() EFragmentOriginMode ImportOriginMode = EFragmentOriginMode::UseWorldOrigin;
	UPROPERTY() FTransform ImportBaseCoordinates;

	friend class UFragmentsImporter;
	friend class UFragmentRegistrationManager;
};
