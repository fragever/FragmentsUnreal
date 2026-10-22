// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

// A spawned fragment needs a root component for viewport framing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FFragmentEnsureRootComponent,
    "FragmentsUnreal.RootComponent.EnsureRootComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentEnsureRootComponent::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
    TestNotNull(TEXT("test world created"), World);
    if (!World) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };

    AFragment* Frag = World->SpawnActor<AFragment>();
    TestNotNull(TEXT("actor spawned"), Frag);
    if (!Frag) { return false; }

    // A bare spawn needs a root component for viewport framing.
    TestNull(TEXT("a bare spawn has no root -- this is the defect"), Frag->GetRootComponent());

    USceneComponent* Root = UFragmentsImporter::EnsureRootComponent(*Frag);
    TestNotNull(TEXT("EnsureRootComponent creates one"), Root);
    if (!Root) { return false; }
    TestEqual(TEXT("...and it becomes the actor's root"), Frag->GetRootComponent(), Root);
    TestTrue(TEXT("...transactional, or the move is not undoable"),
        Root->HasAnyFlags(RF_Transactional));
    TestEqual(TEXT("...and movable"),
        Root->Mobility, EComponentMobility::Movable);

    // Idempotent: a second call must not replace the root.
    USceneComponent* Again = UFragmentsImporter::EnsureRootComponent(*Frag);
    TestEqual(TEXT("EnsureRootComponent is idempotent"), Again, Root);

    return true;
}
