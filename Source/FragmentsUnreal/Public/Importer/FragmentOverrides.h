// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "FragmentOverrides.generated.h"

USTRUCT()
struct FRAGMENTSUNREAL_API FFragmentMaterialOverrideEntry
{
    GENERATED_BODY()

    UPROPERTY() FString SourceKey;
    UPROPERTY() FString ReplacementMaterialPath;
    UPROPERTY() FString SourceMaterialName;   // display label
    UPROPERTY() FString SourceMaterialAsset;  // /Game/... asset path
    UPROPERTY() int32   UsageCount = 0;
    UPROPERTY() bool    bSourceIsGlass = false;
    UPROPERTY() bool    bHasSourceColor = false;
    UPROPERTY() FLinearColor SourceColor = FLinearColor::Transparent;
};

USTRUCT()
struct FRAGMENTSUNREAL_API FFragmentPlacement
{
    GENERATED_BODY()

    UPROPERTY() FVector Location = FVector::ZeroVector;
    UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
    UPROPERTY() bool bHasValue = false;
};

USTRUCT()
struct FRAGMENTSUNREAL_API FFragmentOverrides
{
    GENERATED_BODY()

    UPROPERTY() int32   Version = 3;  // v3 adds Placement; v2 nested Materials.Overrides; reserves Geolocation as a future sibling
    UPROPERTY() FString ModelGuid;
    UPROPERTY() FString FragmentPath;
    UPROPERTY() TArray<FFragmentMaterialOverrideEntry> MaterialOverrides;
    UPROPERTY() FFragmentPlacement Placement;
};

namespace UE::Fragments::Overrides
{
    FRAGMENTSUNREAL_API FString GetSidecarPath(const FString& FragPath);

    FRAGMENTSUNREAL_API FString BuildSourceKey(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass);

    FRAGMENTSUNREAL_API bool Load(const FString& FragPath, const FString& ModelGuid, FFragmentOverrides& Out);

    FRAGMENTSUNREAL_API bool Save(const FString& FragPath, const FFragmentOverrides& In);

    // Load → mutate → Save. Use this instead of fresh-constructing + Save, which silently drops sibling blocks.
    FRAGMENTSUNREAL_API bool Update(const FString& FragPath, const FString& ModelGuid,
        TFunctionRef<void(FFragmentOverrides&)> Mutator);
}  // namespace UE::Fragments::Overrides
