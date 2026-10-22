// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Pins the transaction behaviour of material overrides.
#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Overrides/FragmentOverrideState.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "Importer/FragmentOverrides.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A scratch .frag path whose sidecar this test owns and deletes. Never a real model. */
	FString ScratchFragPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OverrideUndoTests"), TEXT("scratch.frag"));
	}

	void RemoveScratchSidecar()
	{
		const FString Sidecar = UE::Fragments::Overrides::GetSidecarPath(ScratchFragPath());
		FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Sidecar);
	}

	FFragmentMaterialOverrideEntry MakeUndoTestMeta(const FString& Key)
	{
		FFragmentMaterialOverrideEntry Meta;
		Meta.SourceKey = Key;
		Meta.SourceMaterialName = TEXT("mat_1_2_3_255");
		return Meta;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideApplyNoActorTest,
	"FragmentsUnreal.Overrides.ApplyToWorld_NoActorStillWritesSidecar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideApplyNoActorTest::RunTest(const FString& Parameters)
{
	RemoveScratchSidecar();

	UFragmentOverrideState* State = NewObject<UFragmentOverrideState>();
	State->Initialise(TEXT("no-actor-guid"), ScratchFragPath(), FFragmentOverrides());
	State->SetOverride(TEXT("k1"), TEXT("/Game/M_Brick"), MakeUndoTestMeta(TEXT("k1")));

	// No AFragment exists for this GUID.
	bool bBroadcast = false;
	FDelegateHandle H = UFragmentOverrideState::OnOverridesChanged().AddLambda(
		[&bBroadcast](const FString&) { bBroadcast = true; });

	State->ApplyToWorld();

	UFragmentOverrideState::OnOverridesChanged().Remove(H);

	FFragmentOverrides RoundTripped;
	const bool bLoaded = UE::Fragments::Overrides::Load(ScratchFragPath(), TEXT("no-actor-guid"), RoundTripped);
	TestTrue(TEXT("sidecar written with no actor loaded"), bLoaded);
	TestEqual(TEXT("override survived the round trip"), RoundTripped.MaterialOverrides.Num(), 1);
	TestTrue(TEXT("change was broadcast"), bBroadcast);

	RemoveScratchSidecar();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideApplyPreservesPlacementTest,
	"FragmentsUnreal.Overrides.ApplyToWorld_PreservesPlacementSiblingBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideApplyPreservesPlacementTest::RunTest(const FString& Parameters)
{
	RemoveScratchSidecar();

	// This test used to pin the OPPOSITE contract: that an independent, state-bypassing disk write of Placement survived an ApplyToWorld.

	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
	if (!Subsystem) { AddError(TEXT("No editor subsystem.")); return false; }

	// Use a fresh GUID so cached override state cannot affect repeat runs.
	const FString UniqueGuid = FString::Printf(TEXT("placement-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// P0: written to the sidecar first, then loaded through the FACTORY -- deliberately not NewObject + Initialise.
	const FVector P0(111.0, 222.0, 333.0);
	UE::Fragments::Overrides::Update(ScratchFragPath(), UniqueGuid,
		[&P0](FFragmentOverrides& Out)
		{
			Out.Placement.bHasValue = true;
			Out.Placement.Location = P0;
		});

	UFragmentOverrideState* State = Subsystem->GetOrCreateOverrideState(UniqueGuid, ScratchFragPath());
	TestNotNull(TEXT("state created"), State);

	// A material-only edit must leave Placement exactly as the state found it.
	State->SetOverride(TEXT("k1"), TEXT("/Game/M_Brick"), MakeUndoTestMeta(TEXT("k1")));
	State->ApplyToWorld();

	FFragmentOverrides RoundTripped;
	bool bLoaded = UE::Fragments::Overrides::Load(ScratchFragPath(), UniqueGuid, RoundTripped);
	TestTrue(TEXT("sidecar still present"), bLoaded);
	TestEqual(TEXT("material edit did not drop the override"), RoundTripped.MaterialOverrides.Num(), 1);
	TestTrue(TEXT("placement bHasValue survived a material-only edit"), RoundTripped.Placement.bHasValue);
	TestEqual(TEXT("placement location survived a material-only edit"), RoundTripped.Placement.Location, P0);

	// The other half of sole ownership: a Placement change made THROUGH the state must reach disk.
	const FVector P1(444.0, 555.0, 666.0);
	const FRotator R1(10.0, 20.0, 30.0);
	State->GetOverridesMutable().Placement.Location = P1;
	State->GetOverridesMutable().Placement.Rotation = R1;
	State->ApplyToWorld();

	bLoaded = UE::Fragments::Overrides::Load(ScratchFragPath(), UniqueGuid, RoundTripped);
	TestTrue(TEXT("sidecar still present after placement edit"), bLoaded);
	TestEqual(TEXT("placement the state carries reached disk"), RoundTripped.Placement.Location, P1);
	TestEqual(TEXT("placement rotation the state carries reached disk"), RoundTripped.Placement.Rotation, R1);
	TestEqual(TEXT("material override still intact alongside the placement write"),
		RoundTripped.MaterialOverrides.Num(), 1);

	RemoveScratchSidecar();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideUndoRestoresTest,
	"FragmentsUnreal.Overrides.Undo_RestoresStateAndSidecar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideUndoRestoresTest::RunTest(const FString& Parameters)
{
	RemoveScratchSidecar();

	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
	if (!Subsystem) { AddError(TEXT("No editor subsystem.")); return false; }

	UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(TEXT("undo-restore-guid"), ScratchFragPath());
	TestNotNull(TEXT("state created"), State);

	{
		FScopedTransaction Tx(NSLOCTEXT("FragTest", "SetOverride", "Set Fragment Material Override"));
		State->Modify();
		State->SetOverride(TEXT("k1"), TEXT("/Game/M_Brick"), MakeUndoTestMeta(TEXT("k1")));
		State->ApplyToWorld();
	}

	TestEqual(TEXT("override present after edit"), State->GetOverrides().MaterialOverrides.Num(), 1);

	GEditor->UndoTransaction();

	// The in-memory state must be restored...
	TestEqual(TEXT("override gone after undo"), State->GetOverrides().MaterialOverrides.Num(), 0);

	// PostEditUndo must also persist the restored state.
	FFragmentOverrides RoundTripped;
	UE::Fragments::Overrides::Load(ScratchFragPath(), TEXT("undo-restore-guid"), RoundTripped);
	TestEqual(TEXT("sidecar reflects the undo"), RoundTripped.MaterialOverrides.Num(), 0);

	GEditor->RedoTransaction();
	TestEqual(TEXT("override back after redo"), State->GetOverrides().MaterialOverrides.Num(), 1);

	UE::Fragments::Overrides::Load(ScratchFragPath(), TEXT("undo-restore-guid"), RoundTripped);
	TestEqual(TEXT("sidecar reflects the redo"), RoundTripped.MaterialOverrides.Num(), 1);

	RemoveScratchSidecar();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideResetAllIsOneStepTest,
	"FragmentsUnreal.Overrides.ResetAll_IsOneUndoStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideResetAllIsOneStepTest::RunTest(const FString& Parameters)
{
	RemoveScratchSidecar();

	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
	if (!Subsystem) { AddError(TEXT("No editor subsystem.")); return false; }

	UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(TEXT("reset-all-guid"), ScratchFragPath());

	// Three overrides, each its own transaction -- three separate user gestures.
	const TCHAR* Keys[] = { TEXT("k1"), TEXT("k2"), TEXT("k3") };
	for (const TCHAR* Key : Keys)
	{
		FScopedTransaction Tx(NSLOCTEXT("FragTest", "Set", "Set Fragment Material Override"));
		State->Modify();
		State->SetOverride(Key, TEXT("/Game/M_Brick"), MakeUndoTestMeta(Key));
		State->ApplyToWorld();
	}
	TestEqual(TEXT("three overrides set"), State->GetOverrides().MaterialOverrides.Num(), 3);

	// Reset All -- ONE transaction covering all three.
	{
		FScopedTransaction Tx(NSLOCTEXT("FragTest", "ResetAll", "Reset All Fragment Material Overrides"));
		State->Modify();
		State->ClearAllOverrides();
		State->ApplyToWorld();
	}
	TestEqual(TEXT("all cleared"), State->GetOverrides().MaterialOverrides.Num(), 0);

	// One undo restores all three material overrides.
	GEditor->UndoTransaction();
	TestEqual(TEXT("one undo restores all three"), State->GetOverrides().MaterialOverrides.Num(), 3);

	RemoveScratchSidecar();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPlacementUndoTest,
	"FragmentsUnreal.Overrides.PlacementEdit_IsUndoable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPlacementUndoTest::RunTest(const FString& Parameters)
{
	RemoveScratchSidecar();

	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
	if (!Subsystem) { AddError(TEXT("No editor subsystem.")); return false; }

	// Use a fresh GUID because override state persists across test runs.
	const FString UniqueGuid = FString::Printf(TEXT("placement-undo-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(UniqueGuid, ScratchFragPath());
	TestNotNull(TEXT("state created"), State);
	TestFalse(TEXT("no placement to begin with"), State->GetOverrides().Placement.bHasValue);

	// Exactly the shape of SFragmentsPanel::OnSavePlacementClicked: one transaction, Modify, mutate Placement through the state, ApplyToWorld.
	const FVector Saved(10.0, 20.0, 30.0);
	{
		FScopedTransaction Tx(NSLOCTEXT("FragTest", "SavePlacement", "Save Fragment Placement"));
		State->Modify();
		FFragmentPlacement& Placement = State->GetOverridesMutable().Placement;
		Placement.Location = Saved;
		Placement.Rotation = FRotator(1.0, 2.0, 3.0);
		Placement.bHasValue = true;
		State->ApplyToWorld();
	}

	FFragmentOverrides RoundTripped;
	UE::Fragments::Overrides::Load(ScratchFragPath(), UniqueGuid, RoundTripped);
	TestTrue(TEXT("placement written to sidecar"), RoundTripped.Placement.bHasValue);
	TestEqual(TEXT("saved placement on disk"), RoundTripped.Placement.Location, Saved);

	GEditor->UndoTransaction();

	TestFalse(TEXT("in-memory placement reverted by undo"), State->GetOverrides().Placement.bHasValue);

	// PostEditUndo must push the revert through to disk too, or the artist sees "undone" in the editor and still gets the moved model on the next import.
	UE::Fragments::Overrides::Load(ScratchFragPath(), UniqueGuid, RoundTripped);
	TestFalse(TEXT("sidecar placement reverted by undo"), RoundTripped.Placement.bHasValue);

	GEditor->RedoTransaction();
	TestTrue(TEXT("placement back after redo"), State->GetOverrides().Placement.bHasValue);
	UE::Fragments::Overrides::Load(ScratchFragPath(), UniqueGuid, RoundTripped);
	TestEqual(TEXT("sidecar reflects the redo"), RoundTripped.Placement.Location, Saved);

	RemoveScratchSidecar();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideViewStateOpensNoTransactionTest,
	"FragmentsUnreal.Overrides.ViewState_OpensNoTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideViewStateOpensNoTransactionTest::RunTest(const FString& Parameters)
{
	// Highlight and isolate are VIEW state, not document state.
	if (!GEditor || !GEditor->Trans)
	{
		AddError(TEXT("No transaction buffer; this test needs an editor context."));
		return false;
	}

	RemoveScratchSidecar();

	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>();
	if (!Subsystem) { AddError(TEXT("No editor subsystem.")); return false; }

	// Use a fresh GUID because override state persists for the process lifetime.
	const FString UniqueGuid = FString::Printf(TEXT("viewstate-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(UniqueGuid, ScratchFragPath());

	// CONTROL.
	const int32 BeforeEdit = GEditor->Trans->GetQueueLength();
	{
		FScopedTransaction Tx(NSLOCTEXT("FragTest", "ViewStateControl", "Set Fragment Material Override"));
		State->Modify();
		State->SetOverride(TEXT("vs1"), TEXT("/Game/M_Brick"), MakeUndoTestMeta(TEXT("vs1")));
		State->ApplyToWorld();
	}
	TestTrue(TEXT("a material edit adds a transaction"),
		GEditor->Trans->GetQueueLength() > BeforeEdit);

	// SUBJECT.
	const int32 BeforeView = GEditor->Trans->GetQueueLength();
	State->ApplyToComponents();
	TestEqual(TEXT("component reconciliation adds no transaction"),
		GEditor->Trans->GetQueueLength(), BeforeView);

	RemoveScratchSidecar();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
