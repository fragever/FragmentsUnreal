// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Fragment/Fragment.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

// The Fragments panel promotes the model whose materials are being edited to LOD0, and skips the call when it believes that model is already promoted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPromoteOverrideQueryTest,
	"FragmentsUnreal.Spatial.PromoteOverride.QueryFollowsTheRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPromoteOverrideQueryTest::RunTest(const FString& Parameters)
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

	TestFalse(TEXT("a fresh registration has no override"), RegMgr->HasModelPromoteOverride(Guid));
	RegMgr->PromoteAllCellsForModel(Guid);
	TestTrue(TEXT("promoting sets it"), RegMgr->HasModelPromoteOverride(Guid));
	RegMgr->ClearHotPromoteOverridesForModel(Guid);
	TestFalse(TEXT("clearing drops it"), RegMgr->HasModelPromoteOverride(Guid));

	// Promotion state must reset when the model reloads under the same GUID.
	RegMgr->PromoteAllCellsForModel(Guid);
	RegMgr->UnregisterModel(First);
	RegMgr->RegisterModel(MakeRoot());
	TestFalse(TEXT("a reimported model's registration starts without the override"),
		RegMgr->HasModelPromoteOverride(Guid));

	TestFalse(TEXT("unknown GUID reads false"), RegMgr->HasModelPromoteOverride(TEXT("no-such-model")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
