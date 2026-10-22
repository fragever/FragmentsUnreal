// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Fragment/Fragment.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

// Unloading a level must release its model roots and components.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentRegistrationReleaseWorld,
	"FragmentsUnreal.Spatial.Registration.ReleaseWorldDropsOnlyThatWorldsModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentRegistrationReleaseWorld::RunTest(const FString& Parameters)
{
	UWorld* OldWorld = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	UWorld* OtherWorld = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	if (!TestNotNull(TEXT("old world"), OldWorld) || !TestNotNull(TEXT("other world"), OtherWorld)) { return false; }
	ON_SCOPE_EXIT { OldWorld->DestroyWorld(false); OtherWorld->DestroyWorld(false); };

	auto MakeRoot = [](UWorld* World, const FString& Guid)
	{
		AFragment* Root = World->SpawnActor<AFragment>();
		Root->SetAsBucketRoot(true);
		Root->SetModelGuid(Guid);
		return Root;
	};
	const FString OldGuid = FGuid::NewGuid().ToString();
	const FString OtherGuid = FGuid::NewGuid().ToString();

	UFragmentRegistrationManager* RegMgr = NewObject<UFragmentRegistrationManager>(GetTransientPackage());
	AFragment* OldRoot = MakeRoot(OldWorld, OldGuid);
	RegMgr->RegisterModel(OldRoot);
	RegMgr->RegisterModel(MakeRoot(OtherWorld, OtherGuid));

	RegMgr->ReleaseWorld(OldWorld);

	// SetModelUserVisible returns false when the GUID has no registration.
	TestFalse(TEXT("the old world's model is no longer registered"), RegMgr->SetModelUserVisible(OldGuid, true));
	TestTrue(TEXT("a model in another world stays registered"), RegMgr->SetModelUserVisible(OtherGuid, true));
	TestTrue(TEXT("release destroys nothing"), IsValid(OldRoot));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
