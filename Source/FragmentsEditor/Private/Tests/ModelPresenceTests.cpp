// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Deleting an imported model's actor in the viewport dropped its streaming registration but left the importer holding the model, so the panel kept showing it as loaded.
#include "Misc/AutomationTest.h"
#include "EditorSubsystems/FragmentModelPresence.h"
#include "Fragment/Fragment.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Editor.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "Importer/FragmentModelWrapper.h"
#include "Importer/FragmentsImporter.h"
#include "Tests/AutomationCommon.h"
#include "Interfaces/IPluginManager.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ObjectKey.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A unique absolute path for a scratch file under the project's Intermediate directory. */
	FString ScratchPath(const TCHAR* Prefix)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), Prefix, TEXT(".frag")));
	}

	/** A scratch copy of the plugin's sample .frag, or empty if the copy failed. */
	FString CopySampleToScratch(const TCHAR* Prefix)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FragmentsUnreal"));
		if (!Plugin.IsValid()) { return FString(); }
		const FString Sample = FPaths::Combine(Plugin->GetContentDir(), TEXT("Resources"), TEXT("small_test.frag"));
		const FString Scratch = ScratchPath(Prefix);
		return IFileManager::Get().Copy(*Scratch, *Sample) == COPY_OK ? Scratch : FString();
	}

	/** The editor's importer subsystem, its importer and its world -- all a test needs. */
	struct FEditorImport
	{
		UFragmentsImporterEditorSubsystem* Subsystem = nullptr;
		UFragmentsImporter* Importer = nullptr;
		UWorld* World = nullptr;
	};
	bool GetEditorImport(FAutomationTestBase& Test, FEditorImport& Out)
	{
		Out.Subsystem = GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
		Out.Importer = Out.Subsystem ? Out.Subsystem->GetImporter() : nullptr;
		Out.World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		return Test.TestNotNull(TEXT("editor importer"), Out.Importer) && Test.TestNotNull(TEXT("editor world"), Out.World);
	}

	/** A stand-in for an imported model: the importer holds a wrapper whose root is a bucket root in the editor world. */
	AFragment* AddStandInModel(const FEditorImport& Editor, const FString& Guid, const FString& SourcePath = FString())
	{
		AFragment* Root = Editor.World->SpawnActor<AFragment>();
		Root->SetAsBucketRoot(true);
		Root->SetModelGuid(Guid);
		Root->SetFragmentPath(SourcePath);
		UFragmentModelWrapper* Wrapper = NewObject<UFragmentModelWrapper>(Editor.Importer);
		Wrapper->SetSpawnedFragment(Root);
		Editor.Importer->GetFragmentModels_Mutable().Add(Guid, Wrapper);
		return Root;
	}

	void DeleteInViewport(UWorld* World, AFragment* Root)
	{
		GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
		GEditor->SelectActor(Root, /*bInSelected=*/true, /*bNotify=*/false);
		GEditor->Exec(World, TEXT("DELETE"));
	}

	FString UniqueGuid(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentModelPresenceUnloadTest,
	"FragmentsUnreal.Editor.ModelPresence.DeletedRootUnloadsOnlyAGoneModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentModelPresenceUnloadTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::ModelPresence;

	UFragmentModelWrapper* EntryAtDeletion = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	UFragmentModelWrapper* ReimportEntry = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	const FObjectKey Key(EntryAtDeletion);

	// A deleted root must unload its matching importer entry.
	TestTrue(TEXT("a model still held under the same entry unloads"),
		ShouldUnloadAfterRootDeletion(EntryAtDeletion, Key));

	// Reimport destroys the old root, then starts a new import -- a new entry -- in the same click.
	TestFalse(TEXT("a Reimport's new entry stays"), ShouldUnloadAfterRootDeletion(ReimportEntry, Key));
	TestFalse(TEXT("a model already removed needs no unload"), ShouldUnloadAfterRootDeletion(nullptr, Key));
	TestFalse(TEXT("a root the importer never held unloads nothing"),
		ShouldUnloadAfterRootDeletion(EntryAtDeletion, FObjectKey()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentModelPresenceUndoDecisionTest,
	"FragmentsUnreal.Editor.ModelPresence.UndoReimportsOnlyAnUnloadedModelWithASource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentModelPresenceUndoDecisionTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::ModelPresence;

	const FString Existing = ScratchPath(TEXT("UndoDecisionTest-"));
	if (!TestTrue(TEXT("scratch file written"), FFileHelper::SaveStringToFile(TEXT("x"), *Existing))) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Existing); };
	const FString Missing = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("no-such-model.frag"));

	TestTrue(TEXT("an existing source file exists"), SourceFileExists(Existing));
	TestFalse(TEXT("a moved or deleted one does not"), SourceFileExists(Missing));
	TestFalse(TEXT("nor does an empty path"), SourceFileExists(FString()));

	TestTrue(TEXT("an unloaded model with its source on disk is reimported"), ShouldReimportAfterUndo(false, Existing));
	TestFalse(TEXT("a model something already loaded again is left alone"), ShouldReimportAfterUndo(true, Existing));
	TestFalse(TEXT("a model whose source has moved keeps the restored actor"), ShouldReimportAfterUndo(false, Missing));
	TestFalse(TEXT("a model with no recorded source keeps the restored actor"), ShouldReimportAfterUndo(false, FString()));
	return true;
}

// The editor's Delete command garbage-collects before the next tick (UUnrealEdEngine::DeleteActors), which nulls the importer's pointer to the deleted root.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentViewportDeleteUnloadsModelTest,
	"FragmentsUnreal.Editor.ModelPresence.ViewportDeleteUnloadsTheModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentViewportDeleteUnloadsModelTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }

	const FString Guid = UniqueGuid(TEXT("presence"));
	AFragment* Root = AddStandInModel(Editor, Guid);

	TSharedRef<bool> bHeard = MakeShared<bool>(false);
	const FDelegateHandle Handle = Editor.Subsystem->OnModelUnloaded().AddLambda(
		[bHeard, Guid](const FString& Unloaded) { if (Unloaded == Guid) { *bHeard = true; } });

	DeleteInViewport(Editor.World, Root);

	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Editor, Guid, bHeard, Handle]()
	{
		TestFalse(TEXT("the importer no longer holds a model whose root was deleted"), Editor.Importer->IsFragmentLoaded(Guid));
		TestTrue(TEXT("listeners such as the panel heard the model unload"), *bHeard);

		Editor.Subsystem->OnModelUnloaded().Remove(Handle);
		Editor.Importer->GetFragmentModels_Mutable().Remove(Guid);
		return true;
	}));
	return true;
}

// Undoing a viewport delete restored the bare root actor but the model stayed unloaded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentUndoViewportDeleteReimportsTest,
	"FragmentsUnreal.Editor.ModelPresence.UndoOfViewportDeleteReimportsTheModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentUndoViewportDeleteReimportsTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const FString Source = CopySampleToScratch(TEXT("UndoReimportTest-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }

	const FString Guid = UniqueGuid(TEXT("undo"));
	AFragment* Root = AddStandInModel(Editor, Guid, Source);
	const FObjectKey RootKey(Root);
	DeleteInViewport(Editor.World, Root);
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Editor, Guid]()
	{
		TestFalse(TEXT("the deleted model is unloaded"), Editor.Importer->IsFragmentLoaded(Guid));
		GEditor->UndoTransaction();
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Editor, Guid, RootKey, Source]()
	{
		TestFalse(TEXT("the bare actor undo restored was replaced by the reimport"),
			IsValid(Cast<AFragment>(RootKey.ResolveObjectPtr())));

		int32 BareRoots = 0;
		for (TActorIterator<AFragment> It(Editor.World); It; ++It)
		{
			BareRoots += (IsValid(*It) && It->IsBucketRoot() && It->GetModelGuid() == Guid) ? 1 : 0;
		}
		TestEqual(TEXT("no bare root is left behind"), BareRoots, 0);

		FString ReimportedGuid;
		for (const TPair<FString, UFragmentModelWrapper*>& Pair : Editor.Importer->GetFragmentModels())
		{
			if (Pair.Value && Pair.Value->FragmentPath == Source) { ReimportedGuid = Pair.Key; }
		}
		TestFalse(TEXT("undo started a reimport from the deleted model's source"), ReimportedGuid.IsEmpty());

		if (!ReimportedGuid.IsEmpty()) { Editor.Subsystem->UnloadFragment(ReimportedGuid); }
		Editor.Importer->GetFragmentModels_Mutable().Remove(Guid);
		IFileManager::Get().Delete(*Source);
		return true;
	}));
	return true;
}

// Importing 47 buildings and changing your mind after 10 left no way out but waiting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentCancelImportsTest,
	"FragmentsUnreal.Editor.ModelPresence.CancelStopsOnlyWhatIsStillImporting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentCancelImportsTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const FString Source = CopySampleToScratch(TEXT("CancelImportTest-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Source); };

	// A finished model: held by the importer, not importing.
	const FString FinishedGuid = UniqueGuid(TEXT("finished"));
	AFragment* FinishedRoot = AddStandInModel(Editor, FinishedGuid);
	ON_SCOPE_EXIT
	{
		Editor.Importer->GetFragmentModels_Mutable().Remove(FinishedGuid);
		if (IsValid(FinishedRoot)) { FinishedRoot->Destroy(); }
	};

	UFragmentsImporterEditorSubsystem* Subsystem = Editor.Subsystem;
	auto ImportingGuidFor = [Subsystem](const FString& Path)
	{
		TArray<FFragmentsCachedModel> Importing;
		Subsystem->GetImportingModels(Importing);
		const FFragmentsCachedModel* Found = Importing.FindByPredicate([&Path](const FFragmentsCachedModel& M) { return M.Path == Path; });
		return Found ? Found->Guid : FString();
	};

	FString Error;
	if (!TestTrue(TEXT("an import starts"), Subsystem->LoadAndProcessFragmentForEditor(Source, EFragmentOriginMode::UseWorldOrigin, Error))) { return false; }
	const FString Guid = ImportingGuidFor(Source);
	if (!TestFalse(TEXT("and is importing"), Guid.IsEmpty())) { return false; }

	// One model.
	TestFalse(TEXT("a finished model is not cancellable"), Subsystem->CancelImport(FinishedGuid));
	TestTrue(TEXT("cancelling the import reports it"), Subsystem->CancelImport(Guid));
	TestFalse(TEXT("it is no longer importing"), Subsystem->IsModelImporting(Guid));
	TestFalse(TEXT("nor loaded"), Editor.Importer->IsFragmentLoaded(Guid));

	// Everything still importing.
	if (!TestTrue(TEXT("the import starts again"), Subsystem->LoadAndProcessFragmentForEditor(Source, EFragmentOriginMode::UseWorldOrigin, Error))) { return false; }
	TestTrue(TEXT("Cancel all stops it"), Subsystem->CancelAllImports() >= 1);
	TestEqual(TEXT("nothing is importing"), Subsystem->GetImportingCount(), 0);
	TestFalse(TEXT("the cancelled model is not loaded"), Editor.Importer->IsFragmentLoaded(Guid));
	TestTrue(TEXT("the finished model is untouched"), Editor.Importer->IsFragmentLoaded(FinishedGuid) && IsValid(FinishedRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentShouldReimportOnMapOpenTest,
	"FragmentsUnreal.Editor.ModelPresence.ShouldReimportOnMapOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentShouldReimportOnMapOpenTest::RunTest(const FString& Parameters)
{
	using UE::Fragments::ModelPresence::ShouldReimportOnMapOpen;
	const FString Existing = CopySampleToScratch(TEXT("MapOpenDecision-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Existing.IsEmpty())) { return false; }
	const FString Missing = ScratchPath(TEXT("MapOpenMissing-"));

	TestTrue(TEXT("a saved bucket root whose source is on disk is reimported"), ShouldReimportOnMapOpen(true, Existing, false));
	TestFalse(TEXT("a model the importer already holds is left alone"), ShouldReimportOnMapOpen(true, Existing, true));
	TestFalse(TEXT("a per-element actor is never a reimport trigger"), ShouldReimportOnMapOpen(false, Existing, false));
	TestFalse(TEXT("a root whose source has moved keeps the saved actor"), ShouldReimportOnMapOpen(true, Missing, false));
	TestFalse(TEXT("a root with no recorded source keeps the saved actor"), ShouldReimportOnMapOpen(true, FString(), false));

	IFileManager::Get().Delete(*Existing);
	return true;
}

// Opening a saved level brought back each model's root actor -- path, overrides and all -- but nothing streamed its geometry in, so the models were selectable but empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentMapOpenReimportsSavedRootsTest,
	"FragmentsUnreal.Editor.ModelPresence.MapOpenReimportsSavedRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentMapOpenReimportsSavedRootsTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const FString Source = CopySampleToScratch(TEXT("MapOpenReimport-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }
	const FString Missing = ScratchPath(TEXT("MapOpenGone-"));

	// What a level load hands back: bucket roots the importer knows nothing about.
	const FString Guid = UniqueGuid(TEXT("mapopen"));
	AFragment* Saved = Editor.World->SpawnActor<AFragment>();
	Saved->SetAsBucketRoot(true);
	Saved->SetModelGuid(Guid);
	Saved->SetFragmentPath(Source);
	const FObjectKey SavedKey(Saved);

	AFragment* Orphan = Editor.World->SpawnActor<AFragment>();
	Orphan->SetAsBucketRoot(true);
	Orphan->SetModelGuid(UniqueGuid(TEXT("mapopen-gone")));
	Orphan->SetFragmentPath(Missing);

	Editor.Subsystem->ReimportLevelSerializedRoots(Editor.World);

	TestFalse(TEXT("the saved bare root was replaced by the reimport"), IsValid(Cast<AFragment>(SavedKey.ResolveObjectPtr())));
	TestTrue(TEXT("a root whose source is gone is kept, so nothing the user saved is lost"), IsValid(Orphan));

	FString ReimportedGuid;
	for (const TPair<FString, UFragmentModelWrapper*>& Pair : Editor.Importer->GetFragmentModels())
	{
		if (Pair.Value && Pair.Value->FragmentPath == Source) { ReimportedGuid = Pair.Key; }
	}
	TestFalse(TEXT("opening the map started a reimport from the saved root's source"), ReimportedGuid.IsEmpty());

	if (!ReimportedGuid.IsEmpty()) { Editor.Subsystem->UnloadFragment(ReimportedGuid); }
	if (IsValid(Orphan)) { Orphan->Destroy(); }
	IFileManager::Get().Delete(*Source);
	return true;
}

namespace
{
	/** Every Fragments actor in the world right now, so a test can tell what an import added. */
	TSet<FObjectKey> FragmentActorsIn(UWorld* World)
	{
		TSet<FObjectKey> Keys;
		for (TActorIterator<AFragment> It(World); It; ++It)
		{
			if (IsValid(*It)) { Keys.Add(FObjectKey(*It)); }
		}
		return Keys;
	}

	int32 CountFragmentActorsNotIn(UWorld* World, const TSet<FObjectKey>& Before)
	{
		int32 Added = 0;
		for (TActorIterator<AFragment> It(World); It; ++It)
		{
			Added += (IsValid(*It) && !Before.Contains(FObjectKey(*It))) ? 1 : 0;
		}
		return Added;
	}
}

// Cancelling an import must take its actors with it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentCancelLeavesNoActorsTest,
	"FragmentsUnreal.Editor.ModelPresence.CancelLeavesNoActorsBehind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentCancelLeavesNoActorsTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const FString Source = CopySampleToScratch(TEXT("CancelActorsTest-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }

	const TSet<FObjectKey> Before = FragmentActorsIn(Editor.World);
	FString Error;

	if (!TestTrue(TEXT("an import starts"), Editor.Subsystem->LoadAndProcessFragmentForEditor(Source, EFragmentOriginMode::UseWorldOrigin, Error))) { return false; }
	TestTrue(TEXT("Cancel all stops it"), Editor.Subsystem->CancelAllImports() >= 1);
	TestEqual(TEXT("a cancel straight after the start leaves no Fragments actor"), CountFragmentActorsNotIn(Editor.World, Before), 0);

	if (!TestTrue(TEXT("the import starts again"), Editor.Subsystem->LoadAndProcessFragmentForEditor(Source, EFragmentOriginMode::UseWorldOrigin, Error))) { return false; }
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Editor, Before, Source]()
	{
		TestTrue(TEXT("Cancel all stops the running import"), Editor.Subsystem->CancelAllImports() >= 1);
		TestEqual(TEXT("a cancel mid-spawn leaves no Fragments actor"), CountFragmentActorsNotIn(Editor.World, Before), 0);
		IFileManager::Get().Delete(*Source);
		return true;
	}));
	return true;
}

// The way out of a level saved with more models than fit in memory: open it, Cancel all, save.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentCancelMapOpenReimportTest,
	"FragmentsUnreal.Editor.ModelPresence.CancelAfterMapOpenClearsTheSavedModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentCancelMapOpenReimportTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const FString Source = CopySampleToScratch(TEXT("CancelMapOpen-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Source); };

	const TSet<FObjectKey> Before = FragmentActorsIn(Editor.World);
	AFragment* Saved = Editor.World->SpawnActor<AFragment>();
	Saved->SetAsBucketRoot(true);
	Saved->SetModelGuid(UniqueGuid(TEXT("cancel-mapopen")));
	Saved->SetFragmentPath(Source);
	const FObjectKey SavedKey(Saved);

	Editor.Subsystem->ReimportLevelSerializedRoots(Editor.World);
	TestTrue(TEXT("Cancel all stops the map-open reimport"), Editor.Subsystem->CancelAllImports() >= 1);

	TestFalse(TEXT("the saved root is gone"), IsValid(Cast<AFragment>(SavedKey.ResolveObjectPtr())));
	TestEqual(TEXT("and no Fragments actor took its place"), CountFragmentActorsNotIn(Editor.World, Before), 0);
	return true;
}

// Map-open reimport must avoid loading every saved model at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentMapOpenReimportOffTest,
	"FragmentsUnreal.Editor.ModelPresence.MapOpenReimportCanBeTurnedOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentMapOpenReimportOffTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	IConsoleVariable* ReimportOnMapOpen = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Fragments.Editor.ReimportOnMapOpen"));
	if (!TestNotNull(TEXT("r.Fragments.Editor.ReimportOnMapOpen exists"), ReimportOnMapOpen)) { return false; }
	const int32 Previous = ReimportOnMapOpen->GetInt();
	ReimportOnMapOpen->Set(0, ECVF_SetByCode);
	ON_SCOPE_EXIT { ReimportOnMapOpen->Set(Previous, ECVF_SetByCode); };

	const FString Source = CopySampleToScratch(TEXT("MapOpenOff-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Source); };

	AFragment* Saved = Editor.World->SpawnActor<AFragment>();
	Saved->SetAsBucketRoot(true);
	Saved->SetModelGuid(UniqueGuid(TEXT("mapopen-off")));
	Saved->SetFragmentPath(Source);
	ON_SCOPE_EXIT { if (IsValid(Saved)) { Saved->Destroy(); } };

	Editor.Subsystem->ReimportLevelSerializedRoots(Editor.World);

	TestTrue(TEXT("the saved root is left as it was"), IsValid(Saved));
	bool bStarted = false;
	for (const TPair<FString, UFragmentModelWrapper*>& Pair : Editor.Importer->GetFragmentModels())
	{
		bStarted |= Pair.Value && Pair.Value->FragmentPath == Source;
	}
	TestFalse(TEXT("no reimport started"), bStarted);
	return true;
}

// A level opened with 45 bare roots had no way to empty it short of 45 Outliner deletes: a bare root has no Remove in the panel, because nothing is loaded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentClearLevelModelsTest,
	"FragmentsUnreal.Editor.ModelPresence.ClearSceneRemovesEveryModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentClearLevelModelsTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const FString Source = CopySampleToScratch(TEXT("ClearScene-"));
	if (!TestFalse(TEXT("scratch copy of the sample .frag"), Source.IsEmpty())) { return false; }
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Source); };

	const FString LoadedGuid = UniqueGuid(TEXT("clear-loaded"));
	AFragment* LoadedRoot = AddStandInModel(Editor, LoadedGuid);
	ON_SCOPE_EXIT { Editor.Importer->GetFragmentModels_Mutable().Remove(LoadedGuid); };

	const FString BareGuid = UniqueGuid(TEXT("clear-bare"));
	AFragment* BareRoot = Editor.World->SpawnActor<AFragment>();
	BareRoot->SetAsBucketRoot(true);
	BareRoot->SetModelGuid(BareGuid);
	BareRoot->SetFragmentPath(Source);
	AFragment* BareCopy = Editor.World->SpawnActor<AFragment>();
	BareCopy->SetModelGuid(BareGuid);

	FString Error;
	if (!TestTrue(TEXT("an import starts"), Editor.Subsystem->LoadAndProcessFragmentForEditor(Source, EFragmentOriginMode::UseWorldOrigin, Error))) { return false; }

	TestEqual(TEXT("three models removed: loaded, bare, importing"), Editor.Subsystem->ClearLevelModels(), 3);

	TestFalse(TEXT("the loaded model's root is gone"), IsValid(LoadedRoot));
	TestFalse(TEXT("the bare root is gone"), IsValid(BareRoot));
	TestFalse(TEXT("and its per-element copy"), IsValid(BareCopy));
	TestEqual(TEXT("nothing is importing"), Editor.Subsystem->GetImportingCount(), 0);
	TestFalse(TEXT("the loaded model is unloaded"), Editor.Importer->IsFragmentLoaded(LoadedGuid));
	TestEqual(TEXT("no Fragments actor is left in the level"), FragmentActorsIn(Editor.World).Num(), 0);
	return true;
}

// The list counterpart: one click instead of one "Remove from list" per row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentForgetAllCachedModelsTest,
	"FragmentsUnreal.Editor.ModelPresence.RemoveAllFromListKeepsModelsInTheLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentForgetAllCachedModelsTest::RunTest(const FString& Parameters)
{
	FEditorImport Editor;
	if (!GetEditorImport(*this, Editor)) { return false; }
	const TArray<FFragmentsCachedModel> UsersList = Editor.Subsystem->GetCachedModels();
	ON_SCOPE_EXIT { Editor.Subsystem->SetCachedModelsForTests(UsersList); };

	const FString LoadedGuid = UniqueGuid(TEXT("forget-loaded"));
	AFragment* LoadedRoot = AddStandInModel(Editor, LoadedGuid);
	ON_SCOPE_EXIT
	{
		Editor.Importer->GetFragmentModels_Mutable().Remove(LoadedGuid);
		if (IsValid(LoadedRoot)) { LoadedRoot->Destroy(); }
	};

	FFragmentsCachedModel InLevel;  InLevel.Guid = LoadedGuid;                     InLevel.Path = TEXT("C:/scratch/in-level.frag");
	FFragmentsCachedModel NotA;     NotA.Guid = UniqueGuid(TEXT("forget-a"));      NotA.Path = TEXT("C:/scratch/a.frag");
	FFragmentsCachedModel NotB;     NotB.Guid = UniqueGuid(TEXT("forget-b"));      NotB.Path = TEXT("C:/scratch/b.frag");
	Editor.Subsystem->SetCachedModelsForTests({ InLevel, NotA, NotB });

	TestEqual(TEXT("both models outside the level are removed"), Editor.Subsystem->ForgetAllCachedModels(), 2);
	const TArray<FFragmentsCachedModel>& Left = Editor.Subsystem->GetCachedModels();
	TestEqual(TEXT("one model is left in the list"), Left.Num(), 1);
	TestTrue(TEXT("and it is the one in the level"), Left.Num() == 1 && Left[0].Guid == LoadedGuid);
	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS
