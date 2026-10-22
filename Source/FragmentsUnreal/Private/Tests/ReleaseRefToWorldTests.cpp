// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#if WITH_AUTOMATION_TESTS

// World cleanup fires for EVERY world -- a PIE world on Stop, an asset editor's preview world when it closes -- and the editor importer is session-wide.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentReleaseRefToWorldIgnoresOtherWorlds,
	"FragmentsUnreal.Importer.ReleaseRefToWorld.IgnoresOtherWorlds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentReleaseRefToWorldIgnoresOtherWorlds::RunTest(const FString& Parameters)
{
	UWorld* ModelWorld = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	UWorld* OtherWorld = UWorld::CreateWorld(EWorldType::EditorPreview, /*bInformEngineOfWorld=*/false);
	if (!TestNotNull(TEXT("model world"), ModelWorld) || !TestNotNull(TEXT("other world"), OtherWorld))
	{
		return false;
	}
	ON_SCOPE_EXIT { ModelWorld->DestroyWorld(false); OtherWorld->DestroyWorld(false); };

	AFragment* Owner = ModelWorld->SpawnActor<AFragment>();
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->SetOwnerRef(Owner);
	Importer->EnsureRegistrationStreamingTicking(ModelWorld);
	TestTrue(TEXT("precondition: streaming ticks on the model world"),
		Importer->IsRegistrationStreamingTickingForTesting());

	Importer->ReleaseRefToWorld(OtherWorld);

	TestEqual(TEXT("cleaning up another world keeps the model world"),
		Importer->ResolveModelWorld(), ModelWorld);
	TestTrue(TEXT("...and keeps its streaming timer live and clearable"),
		Importer->IsRegistrationStreamingTickingForTesting());

	// The importer's own world still releases everything.
	Importer->ReleaseRefToWorld(ModelWorld);
	TestNull(TEXT("cleaning up the model world releases it"), Importer->ResolveModelWorld());
	return true;
}

// ModelBucketRoots must release roots from unloaded worlds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentReleaseRefToWorldDropsBucketRoots,
	"FragmentsUnreal.Importer.ReleaseRefToWorld.DropsTheWorldsBucketRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentReleaseRefToWorldDropsBucketRoots::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	if (!TestNotNull(TEXT("world"), World)) { return false; }
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	AFragment* Owner = World->SpawnActor<AFragment>();
	AFragment* Root = World->SpawnActor<AFragment>();
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->SetOwnerRef(Owner);
	Importer->AddBucketRootForTesting(TEXT("model-a"), Root);
	TestTrue(TEXT("precondition: the root is recorded"), Importer->GetBucketRootForTesting(TEXT("model-a")) == Root);

	Importer->ReleaseRefToWorld(World);

	TestNull(TEXT("the released world's bucket root is no longer held"), Importer->GetBucketRootForTesting(TEXT("model-a")));
	TestTrue(TEXT("release destroys nothing"), IsValid(Root));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
