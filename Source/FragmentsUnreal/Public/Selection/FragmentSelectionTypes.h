// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// POD types shared by selection and registration.

#pragma once

#include "CoreMinimal.h"
#include "FragmentSelectionTypes.generated.h"

/** Identifies a single cell within a loaded model. */
USTRUCT(BlueprintType)
struct FRAGMENTSUNREAL_API FFragmentCellHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fragments|Selection")
	FString ModelGuid;

	UPROPERTY(BlueprintReadOnly, Category = "Fragments|Selection")
	int32 Floor = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fragments|Selection")
	int32 GridX = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fragments|Selection")
	int32 GridY = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Fragments|Selection")
	bool bIsShell = true;

	bool IsValid() const { return !ModelGuid.IsEmpty(); }
};

/** Why a selection attempt failed. */
UENUM(BlueprintType)
enum class EFragmentSelectFailReason : uint8
{
	/** Line trace returned no blocking hit (clicked empty sky / outside any model). */
	NoTrace,
	/** Trace hit something but it isn't a fragment proxy / cell / HISMC. */
	NoFragment,
	/** Trace hit a HISMC but ResolveHitToLocalId returned false (hidden element or index out of range — typically means the element is in HiddenElementLocalIds). */
	NotResolvable,
	/** Promote/activate chain didn't converge within MaxRetries iterations. */
	Timeout,
	/** PlayerController / world context missing. Caller bug. */
	InvalidContext,
};
