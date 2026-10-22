// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Materials/MaterialInterface.h"
#include "Utils/FragmentsUtils.h"
#include "Spatial/FragmentRegistry.h"
#include "Utils/FragmentCellPartition.h"
#include "FragmentModelWrapper.generated.h"

// Forward declarations
class AFragment;

/** Shared, thread-safe handle to a model's raw FlatBuffer bytes. */
using FModelBufferPin = TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe>;

UCLASS()
class FRAGMENTSUNREAL_API UFragmentModelWrapper : public UObject
{
	GENERATED_BODY()


private:

	/** Not a UPROPERTY: TSharedPtr to a non-UObject payload. */
	FModelBufferPin RawBuffer;

	const Model* ParsedModel = nullptr;

	FFragmentItem ModelItem;

	FTransform BakedPlacement = FTransform::Identity;

	/** Fragment registry for per-sample visibility */
	UPROPERTY()
	UFragmentRegistry* FragmentRegistry = nullptr;

	/** Root fragment actor spawned from this model */
	UPROPERTY()
	AFragment* SpawnedFragment = nullptr;

	/** Material instances by representation index */
	UPROPERTY()
	TMap<int32, UMaterialInstanceDynamic*> MaterialsMap;

	/** Lazy lookup caches; cleared by ResetWrapper. */
	TMap<int64, int32> LocalIdToFlatIndex;
	TMap<int64, FFragmentItem*> LocalIdToItem;


public:
	/** True if Buffer is a structurally sound Fragments model: every offset and length in it stays inside the buffer. */
	static bool IsValidModelBuffer(const TArray<uint8>& Buffer);

	/** Takes ownership of the model buffer (moves from an rvalue, copies from an lvalue) and reparses. */
	void LoadModel(TArray<uint8> InBuffer)
	{
		RawBuffer = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(InBuffer));
		ParsedModel = IsValidModelBuffer(*RawBuffer) ? GetModel(RawBuffer->GetData()) : nullptr;
		LocalIdToFlatIndex.Empty();
		LocalIdToItem.Empty();
	}

	/** Shared handle to the raw buffer; capture into worker tasks that read FlatBuffer pointers (Shell*, Material*) off the game thread. */
	FModelBufferPin GetBufferPin() const { return RawBuffer; }

	const Model* GetParsedModel() const { return ParsedModel; }

	void SetModelItem(FFragmentItem InModelItem) { ModelItem = InModelItem; LocalIdToItem.Empty(); }
	FFragmentItem GetModelItem() { return ModelItem; }
	const FFragmentItem& GetModelItemRef() const { return ModelItem; }
	FFragmentItem& GetModelItemRef() { return ModelItem; }  // Non-const for geometry extraction

	/** The placement LoadFragment composed into ModelItem.GlobalTransform (sidecar or load options). */
	void SetBakedPlacement(const FTransform& InPlacement) { BakedPlacement = InPlacement; }
	const FTransform& GetBakedPlacement() const { return BakedPlacement; }

	/** LocalId → FlatBuffer item index. */
	int64 GetFlatIndexForLocalId(int64 LocalId);

	/** LocalId → hierarchy item. */
	FFragmentItem* FindItemByLocalId(int64 LocalId);

	/** Build fragment registry for per-sample visibility. */
	void BuildFragmentRegistry(const FString& ModelGuid);

	/** Get the fragment registry. */
	UFragmentRegistry* GetFragmentRegistry() const { return FragmentRegistry; }

	/** Reset wrapper state when changing worlds */
	void ResetWrapper();

	/** Check if this wrapper references actors/materials in the given world */
	bool ReferencesWorld(const UWorld* World) const;

	/** Set/Get the root fragment actor spawned from this model */
	void SetSpawnedFragment(AFragment* InSpawnedFragment) { SpawnedFragment = InSpawnedFragment; }
	AFragment* GetSpawnedFragment() const { return SpawnedFragment; }

	/** Get materials map */
	TMap<int32, UMaterialInstanceDynamic*>& GetMaterialsMap() { return MaterialsMap; }

	// Spatial cell partition === ShellCells populated at end of shell spawn pass.

	UPROPERTY()
	TMap<FCellId, FCellMetadata> ShellCells;

	UPROPERTY()
	TMap<FCellId, FCellMetadata> InteriorCells;

	UPROPERTY()
	FVector ModelAnchor = FVector::ZeroVector;

	/** LocalId → FloorIndex map. */
	TMap<int64, int16> LocalIdToFloor;

	/** True once the shell cell partition (inflate + collapse) has run. */
	UPROPERTY()
	bool bShellCellsFinalized = false;

	/** Track whether shell-cell merges have been enqueued. */
	bool bShellCellsMergeEnqueued = false;

	UPROPERTY()
	FString FragmentPath;

	UPROPERTY()
	TMap<FString, FString> MaterialOverrides;

	UPROPERTY()
	TMap<FString, TObjectPtr<UMaterialInterface>> ResolvedOverrideMaterials;

	UMaterialInterface* ResolveOverrideMaterial(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass);

	/** How LoadFragment placed this model, copied onto the editor root so a reload places it the same way. */
	EFragmentOriginMode OriginMode = EFragmentOriginMode::UseWorldOrigin;
	FTransform OriginBase = FTransform::Identity;
};
