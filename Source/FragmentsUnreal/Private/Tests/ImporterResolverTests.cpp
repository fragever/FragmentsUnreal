// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentsImporterSubsystem.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FImporterResolverFallsBackToEditorProvider,
	"FragmentsUnreal.Importer.Resolver.FallsBackToEditorProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImporterResolverFallsBackToEditorProvider::RunTest(const FString& Parameters)
{
	// Editor worlds resolve the importer through the editor provider.
	UFragmentsImporter* const Stub = NewObject<UFragmentsImporter>(GetTransientPackage());

	auto& Resolver = UE::Fragments::ImporterResolver::GetEditorResolver();
	// Restore the existing editor provider delegate after the test.
	const UE::Fragments::ImporterResolver::FResolveEditorImporter OriginalResolver = Resolver;
	Resolver.BindLambda([Stub](UWorld*) { return Stub; });

	// A null world context cannot reach a GameInstance, so this exercises the fallback.
	UFragmentsImporter* const Resolved = UE::Fragments::ImporterResolver::Resolve(Stub);
	TestTrue(TEXT("resolver falls back to the editor provider"), Resolved == Stub);

	Resolver.Unbind();
	TestTrue(TEXT("unbound resolver yields null rather than crashing"),
		UE::Fragments::ImporterResolver::Resolve(Stub) == nullptr);

	Resolver = OriginalResolver;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FImporterResolverFailsSafeWhenSubsystemImporterIsNull,
	"FragmentsUnreal.Importer.Resolver.FailsSafeWhenSubsystemImporterIsNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImporterResolverFailsSafeWhenSubsystemImporterIsNull::RunTest(const FString& Parameters)
{
	// Resolve safely while PIE teardown leaves a subsystem without an importer.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	GameInstance->InitializeStandalone();

	// Symmetric teardown: InitializeStandalone (GameInstance.cpp) creates a dummy EWorldType::Game context/world that Shutdown() alone does not release.
	ON_SCOPE_EXIT
	{
		UWorld* DummyWorld = GameInstance->GetWorld();
		FWorldContext* Context = GameInstance->GetWorldContext();
		GameInstance->Shutdown();
		if (GEngine)
		{
			if (Context) { GEngine->DestroyWorldContext(DummyWorld); }
		}
		if (IsValid(DummyWorld)) { DummyWorld->DestroyWorld(false); }
		GameInstance->RemoveFromRoot();
	};

	UFragmentsImporterSubsystem* Subsystem = GameInstance->GetSubsystem<UFragmentsImporterSubsystem>();
	TestNotNull(TEXT("importer subsystem exists on the game instance"), Subsystem);
	if (!Subsystem) { return false; }

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	TestNotNull(TEXT("test world created"), World);
	if (!World) { return false; }
	World->SetGameInstance(GameInstance);
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	// The teardown window itself: a live subsystem, null importer.
#if WITH_AUTOMATION_TESTS
	Subsystem->SetImporterForTesting(nullptr);
#endif

	// Bind an editor fallback that would prove the bug if Resolve fell through to it: any non-null result below can only be this stub, and it must never be returned here.
	UFragmentsImporter* const WrongWorldStub = NewObject<UFragmentsImporter>(GetTransientPackage());
	auto& Resolver = UE::Fragments::ImporterResolver::GetEditorResolver();
	const UE::Fragments::ImporterResolver::FResolveEditorImporter OriginalResolver = Resolver;
	Resolver.BindLambda([WrongWorldStub](UWorld*) { return WrongWorldStub; });

	UFragmentsImporter* const Resolved = UE::Fragments::ImporterResolver::Resolve(World);

	Resolver = OriginalResolver;

	TestNull(TEXT("resolver fails safe instead of returning a different world's importer"), Resolved);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
