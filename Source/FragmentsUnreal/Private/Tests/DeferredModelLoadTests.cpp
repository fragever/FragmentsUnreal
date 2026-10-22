// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DeferredModelLoadTestsPrivate
{
	/** EPostSpawnAction is an enum class, which TestEqual cannot format. */
	static int32 Decide(int32 NumDeferredModels, int32 NumPendingInteriors)
	{
		return static_cast<int32>(
			UFragmentsImporter::DecidePostSpawnAction(NumDeferredModels, NumPendingInteriors));
	}

	static int32 Expect(UFragmentsImporter::EPostSpawnAction Action)
	{
		return static_cast<int32>(Action);
	}
}

// A deferred-interior spawn must release parked model loads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPostSpawnActionPrefersParkedModelsTest,
	"FragmentsUnreal.DeferredModelLoad.PostSpawnActionPrefersParkedModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPostSpawnActionPrefersParkedModelsTest::RunTest(const FString& Parameters)
{
	using namespace DeferredModelLoadTestsPrivate;
	using EAction = UFragmentsImporter::EPostSpawnAction;

	// A parked model takes priority over pending interiors.
	TestEqual(TEXT("parked model wins over pending interior"),
		Decide(/*Models=*/ 1, /*Interiors=*/ 1), Expect(EAction::DrainDeferredModel));

	TestEqual(TEXT("parked model with no interiors still drains"),
		Decide(1, 0), Expect(EAction::DrainDeferredModel));

	TestEqual(TEXT("interior activates only once no model is parked"),
		Decide(0, 1), Expect(EAction::ActivateInterior));

	TestEqual(TEXT("nothing queued is idle"),
		Decide(0, 0), Expect(EAction::Idle));

	// A backlog still resolves to "drain one" -- the completion of the one we start is what comes back for the next.
	TestEqual(TEXT("a backlog of parked models still drains one at a time"),
		Decide(5, 3), Expect(EAction::DrainDeferredModel));

	return true;
}

#if WITH_EDITOR // GetImportProgressSnapshot is editor-only
// Purging an active spawn must drain queued model loads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAbortedSpawnDrainsParkedModelTest,
	"FragmentsUnreal.DeferredModelLoad.AbortedSpawnDrainsParkedModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAbortedSpawnDrainsParkedModelTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	if (!TestNotNull(TEXT("test world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	const FString Spawning = FGuid::NewGuid().ToString();
	const FString Parked = FGuid::NewGuid().ToString();

	// Park a load behind a spawn in progress.
	Importer->GetFragmentModels_Mutable().Add(Parked, NewObject<UFragmentModelWrapper>(Importer));
	Importer->BeginChunkedSpawnForTests(Spawning);
	Importer->ProcessLoadedFragment(Parked, World->SpawnActor<AActor>(), false, nullptr,
		FOnProcessLoadedFragmentComplete());
	TestTrue(TEXT("precondition: the load is parked"),
		Importer->GetImportProgressSnapshot(Parked).bIsQueuedForSpawn);

	// Drop the parked model's wrapper so the drained load stops at ProcessLoadedFragment's existence check -- its error is the observable proof that the purge drained the queue.
	Importer->GetFragmentModels_Mutable().Remove(Parked);
	AddExpectedError(TEXT("Model not in FragmentModels"), EAutomationExpectedErrorFlags::Contains, 1);

	Importer->PurgeModelStateForTests(Spawning, /*bRemoveFromMap=*/true);

	TestFalse(TEXT("aborting the spawn starts the parked load"),
		Importer->GetImportProgressSnapshot(Parked).bIsQueuedForSpawn);
	return true;
}
#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
