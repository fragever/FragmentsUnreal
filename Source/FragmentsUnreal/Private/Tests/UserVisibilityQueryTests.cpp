// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Fragment/Fragment.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

// Unloading a model at runtime destroys its bucket root but leaves its registration behind, so a reload of the same GUID appends a second entry after the dead one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentUserVisibilityQueryTest,
	"FragmentsUnreal.Spatial.UserVisibility.QueryFollowsTheLiveRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentUserVisibilityQueryTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	if (!TestNotNull(TEXT("test world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	const FString Guid = FGuid::NewGuid().ToString();
	auto MakeRoot = [World, &Guid]()
	{
		AFragment* Root = World->SpawnActor<AFragment>();
		Root->SetAsBucketRoot(true);
		Root->SetModelGuid(Guid);
		return Root;
	};

	UFragmentRegistrationManager* RegMgr = NewObject<UFragmentRegistrationManager>(GetTransientPackage());
	AFragment* First = MakeRoot();
	RegMgr->RegisterModel(First);

	TestTrue(TEXT("a fresh registration is visible"), RegMgr->IsModelUserVisible(Guid));
	TestTrue(TEXT("hiding a registered model succeeds"), RegMgr->SetModelUserVisible(Guid, false));
	TestFalse(TEXT("hiding is reported"), RegMgr->IsModelUserVisible(Guid));

	// Visibility state must reset when a root is replaced under the same GUID.
	First->Destroy();
	RegMgr->RegisterModel(MakeRoot());
	TestTrue(TEXT("a reloaded model's live registration reads visible"), RegMgr->IsModelUserVisible(Guid));

	TestTrue(TEXT("unknown GUID reads visible"), RegMgr->IsModelUserVisible(TEXT("no-such-model")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
