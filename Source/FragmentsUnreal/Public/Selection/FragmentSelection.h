// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// Blueprint entry points for fragment selection and hit resolution.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/EngineTypes.h"
#include "Selection/FragmentSelectionTypes.h"
#include "FragmentSelection.generated.h"

class AFragment;
class APlayerController;
class UObject;

/** Fired on successful resolve. All five values are populated. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FFragmentClickResolvedDelegate,
	int64, LocalId,
	FString, ModelGuid,
	AFragment*, OwningFragment,
	FVector, HitLocation,
	FFragmentCellHandle, CellHandle);

/** Fired when the resolve gives up. Reason explains why. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFragmentClickFailedDelegate,
	EFragmentSelectFailReason, Reason);

/** Trace a screen position and promote proxy hits to resolve a LocalId. */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentSelectAtScreenPos : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFragmentClickResolvedDelegate Resolved;

	UPROPERTY(BlueprintAssignable)
	FFragmentClickFailedDelegate Failed;

	/** events. */
	UFUNCTION(BlueprintCallable,
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static UFragmentSelectAtScreenPos* SelectAtScreenPos(
		UObject* WorldContextObject,
		APlayerController* PC,
		FVector2D ScreenPos,
		float TraceLength = 100000.f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		int32 MaxRetries = 3);

	virtual void Activate() override;
	virtual UWorld* GetWorld() const override;

private:
	UFUNCTION() void DoTrace();
	void ScheduleRetrace();
	void TryResolve(const FHitResult& Hit);
	void FireResolved(int64 LocalId, AFragment* OwningFragment, const FVector& HitLoc, const FFragmentCellHandle& Handle);
	void FireFailed(EFragmentSelectFailReason Reason);

	UPROPERTY() TWeakObjectPtr<UObject> WorldContext;
	UPROPERTY() TWeakObjectPtr<APlayerController> CachedPC;

	FVector2D CachedScreenPos = FVector2D::ZeroVector;
	float CachedTraceLength = 100000.f;
	ECollisionChannel CachedChannel = ECC_Visibility;
	int32 RemainingRetries = 3;

	FVector CachedRayStart = FVector::ZeroVector;
	FVector CachedRayEnd   = FVector::ZeroVector;

	FTimerHandle RetraceTimer;
	bool bCompleted = false;
};

/** Blueprint function library for the non-async selection helpers. */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentSelectionStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Synchronous peek: deproject screen pos, line trace, resolve only if the trace lands directly on a HISMC. */
	UFUNCTION(BlueprintCallable,
		meta = (WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static bool PeekAtScreenPos(
		UObject* WorldContextObject,
		APlayerController* PC,
		FVector2D ScreenPos,
		float TraceLength,
		ECollisionChannel TraceChannel,
		int64& OutLocalId,
		FString& OutModelGuid,
		AFragment*& OutOwningFragment,
		FVector& OutHitLocation,
		FFragmentCellHandle& OutCellHandle);

	/** Walk a world-space ray and return the first hit that is NOT a ghosted/hidden fragment instance. */
	UFUNCTION(BlueprintCallable,
		meta = (WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static bool TraceFirstVisibleHit(
		UObject* WorldContextObject,
		FVector Start,
		FVector End,
		FHitResult& OutHit,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bTraceComplex = true);

	/** Object-types variant of TraceFirstVisibleHit, mirroring "Line Trace For Objects". */
	UFUNCTION(BlueprintCallable,
		meta = (WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static bool TraceFirstVisibleHitByObjectTypes(
		UObject* WorldContextObject,
		FVector Start,
		FVector End,
		FHitResult& OutHit,
		const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes,
		bool bTraceComplex = true);

	/** Clear every HotPromoteOverride across every loaded model. */
	UFUNCTION(BlueprintCallable,
		meta = (WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static void ClearAllOverrides(UObject* WorldContextObject);

	/** Force every cell of the given model to LOD0 (HotPromoteOverride) AND force trace collision on each cell's HISMCs. */
	UFUNCTION(BlueprintCallable,
		meta = (WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static void PromoteAllCellsForModel(UObject* WorldContextObject, const FString& ModelGuid);

	/** Demote a specific cell by handle. */
	UFUNCTION(BlueprintCallable,
		meta = (WorldContext = "WorldContextObject"),
		Category = "Fragments|Selection")
	static bool DemoteCell(UObject* WorldContextObject, const FFragmentCellHandle& Handle);

	/** Convenience: is this handle non-empty? */
	UFUNCTION(BlueprintPure, Category = "Fragments|Selection")
	static bool IsCellHandleValid(const FFragmentCellHandle& Handle) { return Handle.IsValid(); }
};
