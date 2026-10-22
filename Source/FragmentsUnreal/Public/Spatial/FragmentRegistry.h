// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "Utils/FragmentOcclusionTypes.h"
#include "FragmentRegistry.generated.h"

// Forward declarations
class UFragmentModelWrapper;

/** Per-fragment visibility data for sample-based streaming. */
USTRUCT(BlueprintType)
struct FFragmentVisibilityData
{
	GENERATED_BODY()

	/** Fragment local ID (unique within model) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int64 LocalId = -1;

	/** Pre-computed world-space axis-aligned bounding box */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FBox WorldBounds;

	/** Maximum dimension of the bounding box (for screen size calculation) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	float MaxDimension = 0.0f;

	/** Material index for tile grouping (CRC hash input) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 MaterialIndex = 0;

	/** Whether this is a small object (dimension < 200cm) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bIsSmallObject = false;

	/** Primary representation index for geometry lookup */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 RepresentationIndex = -1;

	/** Global ID (GUID string) for external references */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString GlobalId;

	/** Category for filtering (e.g., "Wall", "Door", "Window") */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Category;

	/** Occlusion role classification for GPU occlusion culling */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	EOcclusionRole OcclusionRole = EOcclusionRole::Occludee;

	/** Material alpha value for transparency detection (0-255) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	uint8 MaterialAlpha = 255;

	FFragmentVisibilityData()
		: WorldBounds(ForceInit)
	{
	}
};

/** Fragment Registry - flat array of all fragments with pre-computed visibility data. */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentRegistry : public UObject
{
	GENERATED_BODY()

public:
	UFragmentRegistry();

	/** Build registry from fragment model. */
	void BuildFromModel(const UFragmentModelWrapper* ModelWrapper, const FString& ModelGuid);

	/** Get all fragments in the registry (const reference for iteration). */
	const TArray<FFragmentVisibilityData>& GetAllFragments() const { return Fragments; }

	/** Get fragment count. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	int32 GetFragmentCount() const { return Fragments.Num(); }

	/** Find fragment by local ID. */
	const FFragmentVisibilityData* FindFragment(int64 LocalId) const;

	/** Get index of fragment by local ID. */
	int32 GetFragmentIndex(int64 LocalId) const;

	/** Get total memory usage estimate for the registry. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	int64 GetMemoryUsage() const;

	/** Get world bounds encompassing all fragments. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	FBox GetWorldBounds() const { return WorldBounds; }

	/** Check if registry has been built. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	bool IsBuilt() const { return bIsBuilt; }

	/** Small object size threshold in cm (200 = 2m, matching engine_fragment) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Registry")
	float SmallObjectSize = 200.0f;

private:
	/** Flat array of all fragment visibility data */
	UPROPERTY()
	TArray<FFragmentVisibilityData> Fragments;

	/** Fast lookup from LocalId to array index */
	UPROPERTY()
	TMap<int64, int32> LocalIdToIndex;

	/** World bounds encompassing all fragments */
	FBox WorldBounds;

	/** Model GUID reference */
	FString ModelGuidRef;

	/** Whether registry has been built */
	bool bIsBuilt = false;

	/** Recursively collect fragment visibility data from hierarchy. */
	void CollectFragmentData(const struct FFragmentItem& Item, const struct Model* ParsedModel);
};
