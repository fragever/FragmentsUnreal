// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Utils/FragmentOcclusionTypes.h"
#include "FragmentOcclusionClassifier.generated.h"

/** Utility class for classifying fragments based on their BIM category and material properties. */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentOcclusionClassifier : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Classify a fragment based on its category and material alpha. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static EOcclusionRole ClassifyFragment(const FString& Category, uint8 MaterialAlpha);

	/** Check if a category represents a large structural element that should occlude. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static bool IsOccluderCategory(const FString& Category);

	/** Check if a material is transparent based on alpha value. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static bool IsTransparentMaterial(uint8 MaterialAlpha);

	/** Get the occlusion role as a display string. */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static FString GetOcclusionRoleString(EOcclusionRole Role);

	/** Transparency threshold for material classification. */
	static constexpr uint8 TransparencyThreshold = 245;

private:
	/** Set of IFC categories that represent large structural occluders */
	static const TSet<FString>& GetOccluderCategories();
};
