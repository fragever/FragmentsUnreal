// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "EditorSubsystems/FragmentModelPresence.h"
#include "Import/FragmentImportProgressMath.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "Import/FragmentsSlateImportProgressSink.h"
#include "Importer/FragmentModelWrapper.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentOverrides.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "TimerManager.h"
#include "Overrides/FragmentOverrideState.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Utils/FragmentsLog.h"
#include "HAL/IConsoleManager.h"

// Read when a map opens, so it has to be set before the level loads: in DefaultEngine.ini under [ConsoleVariables], or on the command line with -ini:Engine:[ConsoleVariables]:r.Fragments.Editor.ReimportOnMapOpen=0.
static TAutoConsoleVariable<int32> CVarReimportOnMapOpen(
	TEXT("r.Fragments.Editor.ReimportOnMapOpen"), 1,
	TEXT("1 = opening a level reimports its saved Fragments models from source. 0 = leave the saved roots bare, ")
	TEXT("for opening a level saved with more models than fit in memory so the extra ones can be deleted."),
	ECVF_Default);

const FString UFragmentsImporterEditorSubsystem::CacheSlotName(TEXT("FragmentsEditorImportedModels"));

FString UFragmentsImporterEditorSubsystem::LoadFragment(
	const FString& FragPath,
	EFragmentOriginMode OriginMode)
{
	check(Importer);

	FFragmentLoadOptions Options;
	Options.OriginMode = OriginMode;

	const FString ModelGuid = Importer->LoadFragment(FragPath, Options);
	FragmentModels = Importer->GetFragmentModels();

	return ModelGuid;
}

FFragmentsImportProgress& UFragmentsImporterEditorSubsystem::GetImportProgress()
{
	if (!ImportProgress)
	{
		ImportProgress = MakeUnique<FFragmentsImportProgress>(MakeShared<FSlateImportProgressSink>());
	}
	return *ImportProgress;
}

void UFragmentsImporterEditorSubsystem::ReportImportFailure(const FString& ErrorMessage)
{
	GetImportProgress().ReportFailure(ErrorMessage);
}

void UFragmentsImporterEditorSubsystem::ReportLoadingStopped(int32 FilesNotLoaded)
{
	GetImportProgress().ReportLoadingStopped(FilesNotLoaded);
}

bool UFragmentsImporterEditorSubsystem::LoadAndProcessFragmentForEditor(const FString& FragPath, EFragmentOriginMode OriginMode, FString& OutErrorMessage)
{
	// One exit for every synchronous failure.
	auto FailImport = [this, &FragPath, &OutErrorMessage](const TCHAR* Message) -> bool
	{
		OutErrorMessage = Message;
		GetImportProgress().ReportFailure(OutErrorMessage);

		// Log import failures for commandlets and other hosts without Slate.
		UE_LOG(LogFragments, Warning, TEXT("[Import] '%s' failed: %s"), *FragPath, *OutErrorMessage);
		return false;
	};

	if (!Importer)
	{
		return FailImport(TEXT("Fragments importer is not initialized."));
	}

	if (FragPath.IsEmpty())
	{
		return FailImport(TEXT("Fragment path is empty."));
	}

	// Validate a reimport path before unloading the currently loaded model.
	if (!FPaths::FileExists(FragPath))
	{
		return FailImport(*FString::Printf(TEXT("Fragment file was not found on disk:\n%s"), *FragPath));
	}

	FScopedSlowTask ImportTask(0.f, FText::Format(NSLOCTEXT("FragmentsEditor", "LoadingFragmentTask", "Loading {0}..."),
		FText::FromString(FPaths::GetCleanFilename(FragPath))));
	ImportTask.MakeDialog();

	const FString ModelGuid = LoadFragment(FragPath, OriginMode);
	if (ModelGuid.IsEmpty())
	{
		return FailImport(TEXT("Failed to load fragment file."));
	}
	ImportOriginModes.Add(ModelGuid, OriginMode);

	UFragmentModelWrapper* const* WrapperPtr = FragmentModels.Find(ModelGuid);
	if (!WrapperPtr || !*WrapperPtr)
	{
		return FailImport(TEXT("Loaded fragment model wrapper was not found."));
	}

	FFragmentsCachedModel PendingEntry;
	PendingEntry.Path = FragPath;
	PendingEntry.Guid = ModelGuid;
	PendingEntry.SamplesCount = CountSamplesRecursive((*WrapperPtr)->GetModelItemRef());
	PendingEntry.Alias = TEXT("");
	PendingCacheEntries.Add(ModelGuid, PendingEntry);
	ModelImportStarted.Broadcast(ModelGuid);

	if (AFragment* ExistingRoot = FindExistingModelActorInEditorWorld(ModelGuid))
	{
		if (IsValid(ExistingRoot) && ExistingRoot->IsHISMEnabled() && ExistingRoot->GetBuckets().Num() > 0)
		{
			BindExistingModelForEditor(ModelGuid, ExistingRoot);
			AddOrUpdateCachedModel(PendingEntry);
			SaveCache();
			PendingCacheEntries.Remove(ModelGuid);
			FinalizeImportedModel(ModelGuid);
			ProcessLoadedFragmentComplete.Broadcast(true, TEXT(""), ModelGuid);
			return true;
		}
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		PendingCacheEntries.Remove(ModelGuid);
		return FailImport(TEXT("Editor world is not available."));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(World, AActor::StaticClass(), TEXT("FragmentsEditorImportOwner"));
	SpawnParams.ObjectFlags |= RF_Transient;

	AActor* OwnerRef = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!OwnerRef)
	{
		PendingCacheEntries.Remove(ModelGuid);
		return FailImport(TEXT("Could not spawn editor import owner actor."));
	}

	ImportOwners.Add(ModelGuid, OwnerRef);

	FOnProcessLoadedFragmentComplete DynamicCallback;
	DynamicCallback.BindDynamic(this, &UFragmentsImporterEditorSubsystem::HandleProcessLoadedFragmentComplete);

	// Start progress tracking when the import begins.
	GetImportProgress().BeginTracking(ModelGuid, FPaths::GetBaseFilename(FragPath));

	ProcessLoadedFragment(ModelGuid, OwnerRef, false, DynamicCallback, nullptr);
	return true;
}

void UFragmentsImporterEditorSubsystem::UnloadFragment(const FString& ModelGuid)
{
	// Cancel progress without reporting a failure.
	GetImportProgress().CancelTracking(ModelGuid);

	// Deselect the model before destroying its actor.
	if (GEditor && Importer)
	{
		// IsValid: after a viewport delete the importer still points at the deleted root, which the editor has already deselected.
		AFragment* ModelFragment = GetModelFragment(ModelGuid);
		if (IsValid(ModelFragment))
		{
			GEditor->SelectActor(ModelFragment, /*bInSelected=*/false, /*bNotify=*/true);
		}
	}

	if (Importer)
	{
		Importer->UnloadFragment(ModelGuid);
	}

	if (UFragmentModelWrapper** Found = FragmentModels.Find(ModelGuid))
	{
		*Found = nullptr;
		FragmentModels.Remove(ModelGuid);
	}

	// An import cut short here never completes, so nothing else would clear its pending entry and the panel would show the model as importing forever.
	PendingCacheEntries.Remove(ModelGuid);
	ImportOriginModes.Remove(ModelGuid);

	if (TWeakObjectPtr<AActor>* OwnerPtr = ImportOwners.Find(ModelGuid))
	{
		if (AActor* Owner = OwnerPtr->Get())
		{
			Owner->Destroy();
		}
		ImportOwners.Remove(ModelGuid);
	}

	ModelUnloaded.Broadcast(ModelGuid);
}

void UFragmentsImporterEditorSubsystem::ProcessLoadedFragment(
	const FString& InModelGuid,
	AActor* InOwnerRef,
	bool bInSaveMesh,
	FOnProcessLoadedFragmentComplete OnComplete,
	AFragment* BucketRoot)
{
	check(Importer);

	Importer->ProcessLoadedFragment(InModelGuid, InOwnerRef, bInSaveMesh, BucketRoot, OnComplete);
}

void UFragmentsImporterEditorSubsystem::ProcessLoadedFragmentItem(
	int64 InLocalId,
	const FString& InModelGuid,
	AActor* InOwnerRef,
	bool bInSaveMesh,
	AFragment* BucketRoot)
{
	check(Importer);

	Importer->ProcessLoadedFragmentItem(InLocalId, InModelGuid, InOwnerRef, bInSaveMesh, BucketRoot);
}

TArray<int64> UFragmentsImporterEditorSubsystem::GetElementsByCategory(const FString& InCategory, const FString& ModelGuid)
{
	check(Importer);

	return Importer->GetElementsByCategory(InCategory, ModelGuid);
}

AFragment* UFragmentsImporterEditorSubsystem::GetItemByLocalId(int64 InLocalId, const FString& InModelGuid)
{
	check(Importer);

	return Importer->GetItemByLocalId(InLocalId, InModelGuid);
}

FFragmentItem* UFragmentsImporterEditorSubsystem::GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid)
{
	check(Importer);

	return Importer->GetFragmentItemByLocalId(InLocalId, InModelGuid);
}

void UFragmentsImporterEditorSubsystem::GetItemData(FFragmentItem* InFragmentItem)
{
	check(Importer);

	Importer->GetItemData(InFragmentItem);
}

TArray<FItemAttribute> UFragmentsImporterEditorSubsystem::GetItemPropertySets(int64 LocalId, const FString& InModelGuid)
{
	check(Importer);

	return Importer->GetItemPropertySets(LocalId, InModelGuid);
}

TArray<FItemAttribute> UFragmentsImporterEditorSubsystem::GetItemAttributes(int64 LocalId, const FString& InModelGuid)
{
	check(Importer);

	return Importer->GetItemAttributes(LocalId, InModelGuid);
}

AFragment* UFragmentsImporterEditorSubsystem::GetModelFragment(const FString& InModelGuid)
{
	check(Importer);

	if (AFragment* ModelFragment = Importer->GetModelFragment(InModelGuid))
	{
		return ModelFragment;
	}

	return FindExistingModelActorInEditorWorld(InModelGuid);
}

FTransform UFragmentsImporterEditorSubsystem::GetBaseCoordinates()
{
	check(Importer);

	return Importer->GetBaseCoordinates();
}

void UFragmentsImporterEditorSubsystem::ResetBaseCoordinates()
{
	check(Importer);

	Importer->ResetBaseCoordinates();
}

bool UFragmentsImporterEditorSubsystem::IsFragmentLoaded(const FString& InModelGuid)
{
	check(Importer);

	return Importer->IsFragmentLoaded(InModelGuid) || FindExistingModelActorInEditorWorld(InModelGuid) != nullptr;
}

const TArray<FFragmentsCachedModel>& UFragmentsImporterEditorSubsystem::GetCachedModels() const
{
	static const TArray<FFragmentsCachedModel> EmptyModels;
	return CacheSaveGame ? CacheSaveGame->ImportedModels : EmptyModels;
}

bool UFragmentsImporterEditorSubsystem::CancelImportQuietly(const FString& ModelGuid, FString& OutModelName)
{
	const FFragmentsCachedModel* Pending = PendingCacheEntries.Find(ModelGuid);
	if (!Pending)
	{
		return false;
	}
	OutModelName = FPaths::GetBaseFilename(Pending->Path);   // read before UnloadFragment drops it

	// Unload clears active, queued, and cached import state.
	UE_LOG(LogFragments, Log, TEXT("[Import] Cancelled %s (%s)"), *OutModelName, *ModelGuid);
	UnloadFragment(ModelGuid);
	return true;
}

bool UFragmentsImporterEditorSubsystem::CancelImport(const FString& ModelGuid)
{
	FString ModelName;
	if (!CancelImportQuietly(ModelGuid, ModelName))
	{
		return false;
	}
	GetImportProgress().ReportCancelled({ ModelName });
	return true;
}

int32 UFragmentsImporterEditorSubsystem::CancelAllImports()
{
	TArray<FString> Importing;
	PendingCacheEntries.GetKeys(Importing);   // collected first: each cancel removes its entry

	TArray<FString> CancelledNames;
	for (const FString& ModelGuid : Importing)
	{
		FString ModelName;
		if (CancelImportQuietly(ModelGuid, ModelName))
		{
			CancelledNames.Add(MoveTemp(ModelName));
		}
	}
	GetImportProgress().ReportCancelled(CancelledNames);   // one notice for the whole batch
	return CancelledNames.Num();
}

int32 UFragmentsImporterEditorSubsystem::ClearLevelModels()
{
	TSet<FString> Removed;

	// Imports first and quietly: a cancel unloads the model, and a "cancelled" notice for a clear the user asked for would only be noise.
	TArray<FString> Importing;
	PendingCacheEntries.GetKeys(Importing);
	for (const FString& ModelGuid : Importing)
	{
		FString ModelName;
		if (CancelImportQuietly(ModelGuid, ModelName)) { Removed.Add(ModelGuid); }
	}

	if (Importer)
	{
		TArray<FString> Loaded;
		Importer->GetFragmentModels().GetKeys(Loaded);   // collected first: each unload removes its entry
		for (const FString& ModelGuid : Loaded)
		{
			UnloadFragment(ModelGuid);
			Removed.Add(ModelGuid);
		}
	}

	// What nothing holds: bare roots a saved level brought back, and their per-element copies.
	if (UWorld* World = GetEditorWorld())
	{
		TArray<AFragment*> Left;
		for (TActorIterator<AFragment> It(World); It; ++It)
		{
			if (IsValid(*It)) { Left.Add(*It); }
		}
		for (AFragment* Actor : Left)
		{
			if (!Actor->GetModelGuid().IsEmpty()) { Removed.Add(Actor->GetModelGuid()); }
			// Deselected first, for the reason UnloadFragment gives: a selected actor destroyed in place leaves a selection reference that fatals on the next level save.
			if (GEditor) { GEditor->SelectActor(Actor, /*bInSelected=*/false, /*bNotify=*/true); }
			Actor->Destroy();
		}
	}
	return Removed.Num();
}

int32 UFragmentsImporterEditorSubsystem::ForgetAllCachedModels()
{
	if (!CacheSaveGame) { return 0; }

	TArray<FString> Listed;
	for (const FFragmentsCachedModel& Model : CacheSaveGame->ImportedModels) { Listed.Add(Model.Guid); }

	int32 Forgotten = 0;
	for (const FString& ModelGuid : Listed)
	{
		Forgotten += ForgetCachedModel(ModelGuid) ? 1 : 0;   // refuses a model still in the level
	}
	return Forgotten;
}

#if WITH_DEV_AUTOMATION_TESTS
void UFragmentsImporterEditorSubsystem::SetCachedModelsForTests(const TArray<FFragmentsCachedModel>& Models)
{
	if (!CacheSaveGame) { return; }
	CacheSaveGame->ImportedModels = Models;
	SaveCache();
}
#endif

bool UFragmentsImporterEditorSubsystem::ForgetCachedModel(const FString& ModelGuid)
{
	if (!CacheSaveGame || ModelGuid.IsEmpty() || IsFragmentLoaded(ModelGuid))
	{
		return false;
	}

	if (CacheSaveGame->ImportedModels.RemoveAll([&ModelGuid](const FFragmentsCachedModel& Model) { return Model.Guid == ModelGuid; }) == 0)
	{
		return false;
	}
	SaveCache();
	return true;
}

void UFragmentsImporterEditorSubsystem::CollectLoadedModelGuids(TSet<FString>& OutGuids) const
{
	OutGuids.Reset();
	if (Importer)
	{
		for (const TPair<FString, UFragmentModelWrapper*>& Pair : Importer->GetFragmentModels())
		{
			OutGuids.Add(Pair.Key);
		}
	}

	// The same test FindExistingModelActorInEditorWorld applies per GUID: any live AFragment of the model counts.
	if (UWorld* World = GetEditorWorld())
	{
		for (TActorIterator<AFragment> It(World); It; ++It)
		{
			FString Guid = IsValid(*It) ? It->GetModelGuid() : FString();
			if (!Guid.IsEmpty())
			{
				OutGuids.Add(MoveTemp(Guid));
			}
		}
	}
}

bool UFragmentsImporterEditorSubsystem::GetCachedModelByGuid(const FString& ModelGuid, FFragmentsCachedModel& OutCachedModel) const
{
	if (!CacheSaveGame)
	{
		return false;
	}

	for (const FFragmentsCachedModel& ImportedModel : CacheSaveGame->ImportedModels)
	{
		if (ImportedModel.Guid == ModelGuid)
		{
			OutCachedModel = ImportedModel;
			return true;
		}
	}

	return false;
}

bool UFragmentsImporterEditorSubsystem::UpdateCachedModelAlias(const FString& ModelGuid, const FString& Alias)
{
	if (!CacheSaveGame)
	{
		LoadCache();
	}

	if (!CacheSaveGame)
	{
		return false;
	}

	for (FFragmentsCachedModel& ImportedModel : CacheSaveGame->ImportedModels)
	{
		if (ImportedModel.Guid == ModelGuid)
		{
			ImportedModel.Alias = Alias;
			SaveCache();
			return true;
		}
	}

	return false;
}

void UFragmentsImporterEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Importer = NewObject<UFragmentsImporter>(this);
	LoadCache();

	FWorldDelegates::OnWorldCleanup.AddUObject(this, &UFragmentsImporterEditorSubsystem::OnWorldCleanup);
	FEditorDelegates::OnMapOpened.AddUObject(this, &UFragmentsImporterEditorSubsystem::OnMapOpened);
	FEditorDelegates::PostUndoRedo.AddUObject(this, &UFragmentsImporterEditorSubsystem::HandlePostUndoRedo);

	// Without this nothing ever drops a deleted actor's registration.
	if (GEngine)
	{
		ActorDeletedHandle = GEngine->OnLevelActorDeleted().AddLambda(
			[this](AActor* Actor) { UnregisterDeletedActor(Actor); });
	}
}

void UFragmentsImporterEditorSubsystem::Deinitialize()
{
	// Destroying the tracker ends every surviving entry at the sink.
	ImportProgress.Reset();

	FWorldDelegates::OnWorldCleanup.RemoveAll(this);
	FEditorDelegates::OnMapOpened.RemoveAll(this);
	FEditorDelegates::PostUndoRedo.RemoveAll(this);
	if (GEngine) { GEngine->OnLevelActorDeleted().Remove(ActorDeletedHandle); }

	if (Importer)
	{
		Importer->ResetAll();
		Importer = nullptr;
	}

	FragmentModels.Empty();
	PendingCacheEntries.Empty();
	ImportOwners.Empty();
	CacheSaveGame = nullptr;

	Super::Deinitialize();
}

void UFragmentsImporterEditorSubsystem::UnregisterDeletedActor(AActor* Actor)
{
	AFragment* Frag = Cast<AFragment>(Actor);
	if (!Frag || !Importer) { return; }

	const UWorld* World = Frag->GetWorld();
	if (!World || World->WorldType != EWorldType::Editor) { return; }

	// Deleting an actor is routine in the editor and is a path the runtime never had, so nothing ever needed to unregister one.
	if (UFragmentRegistrationManager* RegMgr = Importer->GetRegistrationManager())
	{
		RegMgr->UnregisterModel(Frag);
	}

	// A model's bucket root going away takes the model with it.
	if (Frag->IsBucketRoot() && GEditor)
	{
		const FString ModelGuid = Frag->GetModelGuid();
		UFragmentModelWrapper* Entry = Importer->GetFragmentModels().FindRef(ModelGuid);
		const FObjectKey EntryAtDeletion = (Entry && Entry->GetSpawnedFragment() == Frag) ? FObjectKey(Entry) : FObjectKey();

		if (EntryAtDeletion != FObjectKey())
		{
			FModelDeletedInViewport& Deleted = ModelsDeletedInViewport.Add(FObjectKey(Frag));
			Deleted.ModelGuid = ModelGuid;
			Deleted.SourcePath = Entry->FragmentPath.IsEmpty() ? Frag->GetFragmentPath() : Entry->FragmentPath;
			if (const EFragmentOriginMode* Mode = ImportOriginModes.Find(ModelGuid)) { Deleted.OriginMode = *Mode; }
		}

		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this,
			[this, ModelGuid, EntryAtDeletion]() { HandleModelRootDeleted(ModelGuid, EntryAtDeletion); }));
	}
}

void UFragmentsImporterEditorSubsystem::HandlePostUndoRedo()
{
	if (!Importer) { return; }

	// Collected first: destroying a restored root re-enters UnregisterDeletedActor, and the reimport goes through LoadAndProcessFragmentForEditor.
	TArray<TPair<AFragment*, FModelDeletedInViewport>> Restored;
	for (auto It = ModelsDeletedInViewport.CreateIterator(); It; ++It)
	{
		AFragment* Root = Cast<AFragment>(It.Key().ResolveObjectPtr());
		if (IsValid(Root))   // still deleted otherwise: this undo was about something else
		{
			Restored.Emplace(Root, It.Value());
			It.RemoveCurrent();
		}
	}

	for (const TPair<AFragment*, FModelDeletedInViewport>& Pair : Restored)
	{
		const FModelDeletedInViewport& Deleted = Pair.Value;
		const bool bLoaded = Importer->IsFragmentLoaded(Deleted.ModelGuid);
		if (!UE::Fragments::ModelPresence::ShouldReimportAfterUndo(bLoaded, Deleted.SourcePath))
		{
			if (!bLoaded)
			{
				ReportImportFailure(FString::Printf(
					TEXT("Undo restored the model, but its fragment file was not found, so it cannot stream:\n%s"),
					*Deleted.SourcePath));
			}
			continue;
		}

		// The bare actor undo restored has none of the model's streaming state; the reimport spawns a complete root in its place.
		Pair.Key->Destroy();
		FString Error;
		LoadAndProcessFragmentForEditor(Deleted.SourcePath, Deleted.OriginMode, Error);
	}
}

void UFragmentsImporterEditorSubsystem::HandleModelRootDeleted(const FString& ModelGuid, FObjectKey EntryAtDeletion)
{
	if (!Importer || ModelGuid.IsEmpty()) { return; }

	if (UE::Fragments::ModelPresence::ShouldUnloadAfterRootDeletion(
		Importer->GetFragmentModels().FindRef(ModelGuid), EntryAtDeletion))
	{
		UnloadFragment(ModelGuid);   // broadcasts ModelUnloaded
	}
	else if (!IsFragmentLoaded(ModelGuid))
	{
		// A root from a reopened level: the importer never held the model, so there is nothing to unload, but anything showing the model still needs to hear it is gone.
		ModelUnloaded.Broadcast(ModelGuid);
	}
}

void UFragmentsImporterEditorSubsystem::HandleProcessLoadedFragmentComplete(bool bSuccess, const FString& ErrorMessage, const FString& ModelGuid)
{
	// Before the cache write and the broadcast: EndTracking is what removes the status-bar entry and, on failure, raises the toast carrying ErrorMessage.
	GetImportProgress().EndTracking(ModelGuid, bSuccess, ErrorMessage);

	if (bSuccess)
	{
		if (FFragmentsCachedModel* PendingEntry = PendingCacheEntries.Find(ModelGuid))
		{
			AddOrUpdateCachedModel(*PendingEntry);
			SaveCache();
		}

		// Outliner caches its row list at spawn time; re-query IsListedInSceneOutliner so children marked hidden actually drop.
		if (GEngine)
		{
			GEngine->BroadcastLevelActorListChanged();
		}

		FinalizeImportedModel(ModelGuid);
	}

	PendingCacheEntries.Remove(ModelGuid);
	ProcessLoadedFragmentComplete.Broadcast(bSuccess, ErrorMessage, ModelGuid);
}

void UFragmentsImporterEditorSubsystem::ReconcileOverrideStateForActor(
	const FString& ModelGuid, const FString& FragPath)
{
	// Apply restored overrides to components without rewriting the sidecar.
	if (UFragmentOverrideState* State = GetOrCreateOverrideState(ModelGuid, FragPath))
	{
		State->ApplyToComponents();
	}
}

void UFragmentsImporterEditorSubsystem::FinalizeImportedModel(const FString& ModelGuid)
{
	AFragment* Root = GetModelFragment(ModelGuid);
	if (!IsValid(Root))
	{
		return;
	}

	FFragmentsCachedModel Cached;
	if (GetCachedModelByGuid(ModelGuid, Cached) && !Cached.Path.IsEmpty())
	{
		Root->SetActorLabel(FPaths::GetBaseFilename(Cached.Path));

		// The state can be AHEAD of what this actor's components were built with.
		ReconcileOverrideStateForActor(ModelGuid, Cached.Path);
	}

	// No selection or camera move here: with several models importing, each one finishing used to steal the selection and the camera.
}

bool UFragmentsImporterEditorSubsystem::SelectModelInViewport(const FString& ModelGuid, bool bFrameCamera)
{
	AFragment* Root = GEditor ? GetModelFragment(ModelGuid) : nullptr;
	if (!IsValid(Root))
	{
		return false;
	}

	GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
	GEditor->SelectActor(Root, /*bInSelected=*/true, /*bNotify=*/true);

	if (bFrameCamera)
	{
		// Frame registered component bounds regardless of model LOD.
		GEditor->MoveViewportCamerasToActor(*Root, /*bActiveViewportOnly=*/false);
	}
	return true;
}

FText UFragmentsImporterEditorSubsystem::GetImportStageText(const FString& ModelGuid) const
{
	if (!Importer || !IsModelImporting(ModelGuid))
	{
		return FText::GetEmpty();
	}

	const FFragmentImportProgressSnapshot Snapshot = Importer->GetImportProgressSnapshot(ModelGuid);
	const FragmentImportProgress::FWorkUnits Units = FragmentImportProgress::ComputeWorkUnits(Snapshot);
	FNumberFormattingOptions WholePercent;
	WholePercent.SetMaximumFractionalDigits(0);
	return FText::Format(NSLOCTEXT("FragmentsEditor", "ImportStage", "{0} ({1})"),
		FragmentImportProgress::MakeStageText(Snapshot),
		FText::AsPercent(static_cast<float>(Units.WorkDone) / FMath::Max(1, Units.WorkToDo), &WholePercent));
}

void UFragmentsImporterEditorSubsystem::OnWorldCleanup(UWorld* World, bool, bool)
{
	if (Importer)
	{
		Importer->ReleaseRefToWorld(World);
	}
}

void UFragmentsImporterEditorSubsystem::OnMapOpened(const FString&, bool)
{
	TArray<FString> UnloadedGuids;
	if (Importer)
	{
		Importer->GetFragmentModels().GetKeys(UnloadedGuids);
		Importer->ResetAll();
	}

	FragmentModels.Empty();
	PendingCacheEntries.Empty();
	ImportOwners.Empty();
	ModelsDeletedInViewport.Empty();   // their roots belonged to the previous map

	for (const FString& Guid : UnloadedGuids)
	{
		ModelUnloaded.Broadcast(Guid);
	}

	// The opened map's saved roots carry no streaming state: bring their geometry back.
	if (GEditor)
	{
		ReimportLevelSerializedRoots(GEditor->GetEditorWorldContext().World());
	}
}

void UFragmentsImporterEditorSubsystem::ReimportLevelSerializedRoots(UWorld* World)
{
	if (!Importer || !World) { return; }

	// Collected first: the reimport spawns new roots, and destroying while iterating is unsafe.
	TArray<AFragment*> Roots;
	for (TActorIterator<AFragment> It(World); It; ++It)
	{
		if (IsValid(*It) && It->IsBucketRoot()) { Roots.Add(*It); }
	}

	if (CVarReimportOnMapOpen.GetValueOnGameThread() == 0)
	{
		if (Roots.Num() > 0)
		{
			UE_LOG(LogFragments, Display,
				TEXT("[Import] r.Fragments.Editor.ReimportOnMapOpen is 0: %d saved model(s) left bare. ")
				TEXT("Delete the ones you do not need and save, or Reimport them from the Fragments panel."),
				Roots.Num());
		}
		return;
	}

	for (AFragment* Root : Roots)
	{
		const FString SourcePath = Root->GetFragmentPath();
		const FString ModelGuid = Root->GetModelGuid();
		const bool bLoaded = !ModelGuid.IsEmpty() && Importer->IsFragmentLoaded(ModelGuid);
		if (!UE::Fragments::ModelPresence::ShouldReimportOnMapOpen(true, SourcePath, bLoaded))
		{
			if (!bLoaded && !SourcePath.IsEmpty())
			{
				ReportImportFailure(FString::Printf(
					TEXT("The level has a saved model, but its fragment file was not found, so it cannot stream:\n%s"),
					*SourcePath));
			}
			continue;
		}

		// Saved per-element copies of this model are rebuilt by the reimport as well.
		for (TActorIterator<AFragment> It(World); It; ++It)
		{
			if (IsValid(*It) && *It != Root && It->GetModelGuid() == ModelGuid) { It->Destroy(); }
		}
		// Reload the way it was imported. Roots saved before this was recorded default to World Origin.
		const EFragmentOriginMode OriginMode = Root->GetImportOriginMode();
		if (OriginMode == EFragmentOriginMode::UseBaseCoordinates)
		{
			Importer->SeedBaseCoordinates(Root->GetImportBaseCoordinates());
		}
		// Destroyed first so the reimport cannot bind to the bare actor (same as the undo path).
		Root->Destroy();
		FString Error;
		LoadAndProcessFragmentForEditor(SourcePath, OriginMode, Error);
	}
}

void UFragmentsImporterEditorSubsystem::LoadCache()
{
	if (UGameplayStatics::DoesSaveGameExist(CacheSlotName, 0))
	{
		CacheSaveGame = Cast<UFragmentsEditorCacheSaveGame>(UGameplayStatics::LoadGameFromSlot(CacheSlotName, 0));
	}

	if (!CacheSaveGame)
	{
		CacheSaveGame = Cast<UFragmentsEditorCacheSaveGame>(
			UGameplayStatics::CreateSaveGameObject(UFragmentsEditorCacheSaveGame::StaticClass()));
	}
}

void UFragmentsImporterEditorSubsystem::SaveCache()
{
	if (CacheSaveGame)
	{
		UGameplayStatics::SaveGameToSlot(CacheSaveGame, CacheSlotName, 0);
	}
}

void UFragmentsImporterEditorSubsystem::AddOrUpdateCachedModel(const FFragmentsCachedModel& CacheEntry)
{
	if (!CacheSaveGame)
	{
		LoadCache();
	}

	if (!CacheSaveGame)
	{
		return;
	}

	for (FFragmentsCachedModel& ImportedModel : CacheSaveGame->ImportedModels)
	{
		if (ImportedModel.Guid == CacheEntry.Guid)
		{
			const FString ExistingAlias = ImportedModel.Alias;
			ImportedModel = CacheEntry;
			ImportedModel.Alias = ExistingAlias;
			return;
		}
	}

	CacheSaveGame->ImportedModels.Add(CacheEntry);
}

int32 UFragmentsImporterEditorSubsystem::CountSamplesRecursive(const FFragmentItem& Item) const
{
	int32 Count = Item.Samples.Num();
	for (const FFragmentItem* Child : Item.FragmentChildren)
	{
		if (Child)
		{
			Count += CountSamplesRecursive(*Child);
		}
	}
	return Count;
}

AFragment* UFragmentsImporterEditorSubsystem::FindExistingModelActorInEditorWorld(const FString& ModelGuid) const
{
	if (ModelGuid.IsEmpty())
	{
		return nullptr;
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return nullptr;
	}

	AFragment* FirstMatchingFragment = nullptr;
	for (TActorIterator<AFragment> It(World); It; ++It)
	{
		AFragment* Fragment = *It;
		if (!IsValid(Fragment) || Fragment->GetModelGuid() != ModelGuid)
		{
			continue;
		}

		AFragment* CandidateRoot = Fragment->GetBucketRoot();
		if (IsValid(CandidateRoot) && CandidateRoot->GetModelGuid() == ModelGuid)
		{
			Fragment = CandidateRoot;
		}

		if (!FirstMatchingFragment)
		{
			FirstMatchingFragment = Fragment;
		}

		if (Fragment->IsHISMEnabled() && Fragment->GetBuckets().Num() > 0)
		{
			return Fragment;
		}
	}

	return FirstMatchingFragment;
}

bool UFragmentsImporterEditorSubsystem::BindExistingModelForEditor(const FString& ModelGuid, AFragment* ExistingRoot)
{
	if (!Importer || ModelGuid.IsEmpty() || !IsValid(ExistingRoot))
	{
		return false;
	}

	FragmentModels = Importer->GetFragmentModels();
	if (UFragmentModelWrapper** WrapperPtr = FragmentModels.Find(ModelGuid))
	{
		if (UFragmentModelWrapper* Wrapper = *WrapperPtr)
		{
			Wrapper->SetSpawnedFragment(ExistingRoot);
			return true;
		}
	}

	return false;
}

UWorld* UFragmentsImporterEditorSubsystem::GetEditorWorld() const
{
	if (!GEditor)
	{
		return nullptr;
	}

	return GEditor->GetEditorWorldContext().World();
}

UFragmentOverrideState* UFragmentsImporterEditorSubsystem::GetOrCreateOverrideState(
	const FString& ModelGuid, const FString& FragPath)
{
	if (ModelGuid.IsEmpty())
	{
		return nullptr;
	}

	if (TObjectPtr<UFragmentOverrideState>* Found = OverrideStates.Find(ModelGuid))
	{
		// FragPath is bound ONCE, at creation, and every later caller's is ignored -- deliberately, since changing it mid-session would strand the sidecar already written at the old path.
		UFragmentOverrideState* Existing = Found->Get();
		if (Existing && Existing->GetFragPath() != FragPath)
		{
			UE_LOG(LogFragments, Warning,
				TEXT("Override state for model '%s' was created with .frag path '%s' but a caller now "
				     "passes '%s'. Keeping the original; overrides will be read and written there."),
				*ModelGuid, *Existing->GetFragPath(), *FragPath);
		}
		return Existing;
	}

	// Below the lookup, NOT above it: FragPath is only needed to CREATE a state, so refusing an empty one before the cache check would deny a caller the perfectly good state another caller already built.
	if (FragPath.IsEmpty())
	{
		UE_LOG(LogFragments, Warning,
			TEXT("GetOrCreateOverrideState will not create a state for model '%s': no .frag path. "
			     "Overrides cannot be persisted without one, and writing them anyway would put them "
			     "somewhere unfindable."),
			*ModelGuid);
		return nullptr;
	}

	// Outered to the subsystem so the UPROPERTY map keeps it alive for the session.
	UFragmentOverrideState* State = NewObject<UFragmentOverrideState>(this);

	FFragmentOverrides Loaded;
	UE::Fragments::Overrides::Load(FragPath, ModelGuid, Loaded);   // absent sidecar loads as empty
	State->Initialise(ModelGuid, FragPath, Loaded);

	OverrideStates.Add(ModelGuid, State);
	return State;
}

UFragmentOverrideState* UFragmentsImporterEditorSubsystem::FindOverrideState(const FString& ModelGuid) const
{
	const TObjectPtr<UFragmentOverrideState>* Found = OverrideStates.Find(ModelGuid);
	return Found ? Found->Get() : nullptr;
}
