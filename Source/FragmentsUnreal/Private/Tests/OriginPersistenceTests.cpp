// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

// A model imported with Use Base Coordinates came back at world origin after reopening the map or pressing Play: both reload paths forced UseWorldOrigin, and nothing saved how the model had been placed.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOriginSeedBaseCoordinates,
	"FragmentsUnreal.Importer.Origin.SeededBaseIsUsedAndFirstSeedWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOriginSeedBaseCoordinates::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	const FTransform Saved(FRotator(0.0, 12.0, 0.0), FVector(-3676502.0, 50274333.0, -390.0));
	const FTransform Later(FVector(10.0, 20.0, 30.0));

	Importer->SeedBaseCoordinates(Saved);
	TestTrue(TEXT("the seeded base is the importer's base"), Importer->GetBaseCoordinates().Equals(Saved));

	// Several saved roots reload in turn; the first one sets the base, like the import-time latch.
	Importer->SeedBaseCoordinates(Later);
	TestTrue(TEXT("a second seed does not move the base"), Importer->GetBaseCoordinates().Equals(Saved));

	Importer->ResetBaseCoordinates();
	Importer->SeedBaseCoordinates(Later);
	TestTrue(TEXT("after a reset the next seed applies"), Importer->GetBaseCoordinates().Equals(Later));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOriginRootRemembersPlacement,
	"FragmentsUnreal.Importer.Origin.RootRemembersHowItWasPlaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOriginRootRemembersPlacement::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	if (!TestNotNull(TEXT("test world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	AFragment* Legacy = World->SpawnActor<AFragment>();
	TestTrue(TEXT("a root saved before this existed keeps today's World Origin reload"),
		Legacy->GetImportOriginMode() == EFragmentOriginMode::UseWorldOrigin);

	AFragment* Root = World->SpawnActor<AFragment>();
	const FTransform Base(FVector(100.0, 200.0, 300.0));
	Root->SetImportOrigin(EFragmentOriginMode::UseBaseCoordinates, Base);
	TestTrue(TEXT("mode is kept"), Root->GetImportOriginMode() == EFragmentOriginMode::UseBaseCoordinates);
	TestTrue(TEXT("base is kept"), Root->GetImportBaseCoordinates().Equals(Base));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
