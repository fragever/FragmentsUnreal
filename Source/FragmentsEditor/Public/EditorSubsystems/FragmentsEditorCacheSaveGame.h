// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "FragmentsEditorCacheSaveGame.generated.h"

USTRUCT(BlueprintType)
struct FRAGMENTSEDITOR_API FFragmentsCachedModel
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fragments")
	FString Path;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fragments")
	FString Guid;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fragments")
	int32 SamplesCount = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fragments")
	FString Alias;
};

UCLASS()
class FRAGMENTSEDITOR_API UFragmentsEditorCacheSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Fragments")
	TArray<FFragmentsCachedModel> ImportedModels;
};
