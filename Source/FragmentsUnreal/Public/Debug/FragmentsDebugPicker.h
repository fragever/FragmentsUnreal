// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "FragmentsDebugPicker.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class AFragment;
struct FFragmentItem;

/** Debug-only actor: press a key over a Fragments HISMC and append a JSON record (LocalId, Category, applied MID color, source FlatBuffers material) to a session JSONL file in Saved/FragmentsDebug/. */
UCLASS()
class FRAGMENTSUNREAL_API AFragmentsDebugPicker : public AActor
{
	GENERATED_BODY()

public:
	AFragmentsDebugPicker();

	/** Key bound to the picker. Editable so you can avoid project shortcut conflicts. */
	UPROPERTY(EditAnywhere, Category = "Debug")
	FKey PickKey = EKeys::F8;

	/** Force mouse cursor visible while this actor is in the level. */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bForceShowCursor = true;

	/** Optional suffix appended to the session file (e.g. "vsim_facade"). */
	UPROPERTY(EditAnywhere, Category = "Debug")
	FString OutputSuffix;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Bound to PickKey. Performs cursor trace and calls CapturePick on hit. */
	void OnPickPressed();

private:
	void CapturePick(const FHitResult& Hit);

	/** Lazy-initialised output path; cached so all writes in a session go to one file. */
	FString GetOutputFilePath();

	/** Serialise one click into a single-line JSON string. */
	FString BuildRecord(const FHitResult& Hit,
	                    const AFragment* Fragment,
	                    int64 LocalId,
	                    const FFragmentItem* Item,
	                    const UHierarchicalInstancedStaticMeshComponent* Hism) const;

	FString CachedOutputPath;
	int32 ClickCounter = 0;
};
