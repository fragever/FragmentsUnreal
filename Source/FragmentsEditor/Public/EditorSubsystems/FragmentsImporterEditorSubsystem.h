// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include "EditorSubsystem.h"
#include "EditorSubsystems/FragmentsEditorCacheSaveGame.h"
#include "Import/FragmentsImportProgress.h"
#include "Importer/FragmentsImporter.h"
#include "Utils/FragmentsUtils.h"
#include "FragmentsImporterEditorSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnEditorProcessLoadedFragmentComplete, bool /*bSuccess*/, const FString& /*ErrorMessage*/, const FString& /*ModelGuid*/);
/** A model left the scene: panel Remove, Reimport's unload, a viewport delete of its root, or a map change. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnEditorModelUnloaded, const FString& /*ModelGuid*/);
/** An import has parsed its file and started streaming the model in. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnEditorModelImportStarted, const FString& /*ModelGuid*/);

class UFragmentOverrideState;

/** Editor-only subsystem for fragment import/query during editor sessions. */
UCLASS()
class FRAGMENTSEDITOR_API UFragmentsImporterEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintCallable)
	FString LoadFragment(const FString& FragPath,
		EFragmentOriginMode OriginMode = EFragmentOriginMode::UseWorldOrigin);

	UFUNCTION(BlueprintCallable)
	bool LoadAndProcessFragmentForEditor(const FString& FragPath,
		EFragmentOriginMode OriginMode,
		FString& OutErrorMessage);

	/** Raise the import-failure notification for a failure that never entered LoadAndProcessFragmentForEditor. */
	void ReportImportFailure(const FString& ErrorMessage);

	/** A batch's loading dialog was cancelled with files still to load: one notice saying how many. */
	void ReportLoadingStopped(int32 FilesNotLoaded);

	UFUNCTION(BlueprintCallable)
	void UnloadFragment(const FString& ModelGuid);

	UFUNCTION(BlueprintCallable)
	void ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		FOnProcessLoadedFragmentComplete OnComplete,
		AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	void ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	TArray<int64> GetElementsByCategory(const FString& InCategory, const FString& ModelGuid);

	UFUNCTION(BlueprintCallable)
	AFragment* GetItemByLocalId(int64 InLocalId, const FString& InModelGuid);

	FFragmentItem* GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid);
	void GetItemData(FFragmentItem* InFragmentItem);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemPropertySets(int64 LocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemAttributes(int64 LocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	AFragment* GetModelFragment(const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	FTransform GetBaseCoordinates();

	UFUNCTION(BlueprintCallable)
	void ResetBaseCoordinates();

	UFUNCTION(BlueprintCallable)
	bool IsFragmentLoaded(const FString& InModelGuid);

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}

	const TArray<FFragmentsCachedModel>& GetCachedModels() const;
	bool GetCachedModelByGuid(const FString& ModelGuid, FFragmentsCachedModel& OutCachedModel) const;
	bool UpdateCachedModelAlias(const FString& ModelGuid, const FString& Alias);

	/** Drops an unloaded model from the cached-model list (the panel's "Remove from list"). */
	bool ForgetCachedModel(const FString& ModelGuid);

	/** Every model loaded in the editor world, in one pass: the importer's models plus any model root a saved level brought in. */
	void CollectLoadedModelGuids(TSet<FString>& OutGuids) const;

	/** Selects the model's root in the level and, with bFrameCamera, moves the viewport cameras to it. */
	bool SelectModelInViewport(const FString& ModelGuid, bool bFrameCamera);

	/** What an import in progress is doing, e.g. "placing geometry (55%)". */
	FText GetImportStageText(const FString& ModelGuid) const;
	FOnEditorProcessLoadedFragmentComplete& OnProcessLoadedFragmentComplete() { return ProcessLoadedFragmentComplete; }
	FOnEditorModelUnloaded& OnModelUnloaded() { return ModelUnloaded; }
	FOnEditorModelImportStarted& OnModelImportStarted() { return ModelImportStarted; }

	/** True from the moment an import parses the model until it finishes, fails or is unloaded. */
	bool IsModelImporting(const FString& ModelGuid) const { return PendingCacheEntries.Contains(ModelGuid); }

	int32 GetImportingCount() const { return PendingCacheEntries.Num(); }

	/** Stops one import, wherever it is -- streaming in, or waiting its turn -- and unloads the model, as if it had never been imported. */
	bool CancelImport(const FString& ModelGuid);

	/** CancelImport for every import still running; returns how many it stopped. */
	int32 CancelAllImports();

	/** Cancel imports, unload models, and delete remaining fragment actors. */
	int32 ClearLevelModels();

	/** ForgetCachedModel for every model in the list that is not in the level (the panel's "Remove All Models from List"); returns how many it removed. */
	int32 ForgetAllCachedModels();

#if WITH_DEV_AUTOMATION_TESTS
	/** Replaces and saves the cached-model list, so a test that empties it can put the user's back. */
	void SetCachedModelsForTests(const TArray<FFragmentsCachedModel>& Models);
#endif

	/** The cache entries of imports still running -- for a first import, the only record of it. */
	void GetImportingModels(TArray<FFragmentsCachedModel>& OutModels) const { PendingCacheEntries.GenerateValueArray(OutModels); }
	UFragmentsImporter* GetImporter() const { return Importer; }

	/** Reimport every saved model root in World from its source. */
	void ReimportLevelSerializedRoots(UWorld* World);

	/** Drop a destroyed AFragment's registration. */
	void UnregisterDeletedActor(AActor* Actor);

	/** The transactable override state for one model, created on demand from its sidecar. */
	UFragmentOverrideState* GetOrCreateOverrideState(const FString& ModelGuid, const FString& FragPath);

	/** Never creates. Returns nullptr for a model that has not been activated in the panel. */
	UFragmentOverrideState* FindOverrideState(const FString& ModelGuid) const;

protected:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:

	UFUNCTION()
	void HandleProcessLoadedFragmentComplete(bool bSuccess, const FString& ErrorMessage, const FString& ModelGuid);

	void OnWorldCleanup(UWorld* World, bool, bool);
	void OnMapOpened(const FString&, bool);
	/** CancelImport without the notice, so CancelAllImports raises one for the whole batch. */
	bool CancelImportQuietly(const FString& ModelGuid, FString& OutModelName);
	/** The deferred half of UnregisterDeletedActor for a bucket root; see there. */
	void HandleModelRootDeleted(const FString& ModelGuid, FObjectKey EntryAtDeletion);
	/** Bound to FEditorDelegates::PostUndoRedo: a model whose root a viewport delete unloaded, and whose root the undo brought back, is imported again from its source in place of that root. */
	void HandlePostUndoRedo();
	void LoadCache();
	void SaveCache();
	void AddOrUpdateCachedModel(const FFragmentsCachedModel& CacheEntry);
	int32 CountSamplesRecursive(const FFragmentItem& Item) const;
	AFragment* FindExistingModelActorInEditorWorld(const FString& ModelGuid) const;
	bool BindExistingModelForEditor(const FString& ModelGuid, AFragment* ExistingRoot);
	void FinalizeImportedModel(const FString& ModelGuid);
	UWorld* GetEditorWorld() const;

	/** Called from FinalizeImportedModel, where an imported actor's components go from "just built" to "on screen" without the panel having been touched. */
	void ReconcileOverrideStateForActor(const FString& ModelGuid, const FString& FragPath);

	UPROPERTY()
	TMap<FString, class UFragmentModelWrapper*> FragmentModels;

	UPROPERTY()
	class UFragmentsImporter* Importer = nullptr;

	UPROPERTY()
	UFragmentsEditorCacheSaveGame* CacheSaveGame = nullptr;

	UPROPERTY()
	TMap<FString, FFragmentsCachedModel> PendingCacheEntries;

	UPROPERTY()
	TMap<FString, TObjectPtr<UFragmentOverrideState>> OverrideStates;

	TMap<FString, TWeakObjectPtr<AActor>> ImportOwners;

	/** Non-modal progress for the async chunked-spawn phase. */
	TUniquePtr<FFragmentsImportProgress> ImportProgress;

	/** Creates ImportProgress on first use. */
	FFragmentsImportProgress& GetImportProgress();

	FOnEditorProcessLoadedFragmentComplete ProcessLoadedFragmentComplete;

	FOnEditorModelUnloaded ModelUnloaded;
	FOnEditorModelImportStarted ModelImportStarted;

	/** What an undo needs to bring back a model a viewport delete unloaded, keyed on its deleted root. */
	struct FModelDeletedInViewport
	{
		FString ModelGuid;
		FString SourcePath;
		EFragmentOriginMode OriginMode = EFragmentOriginMode::UseWorldOrigin;
	};
	TMap<FObjectKey, FModelDeletedInViewport> ModelsDeletedInViewport;

	/** The origin mode each model was imported with, so an undo reimports it the same way. */
	TMap<FString, EFragmentOriginMode> ImportOriginModes;

	FDelegateHandle ActorDeletedHandle;

	static const FString CacheSlotName;

};
