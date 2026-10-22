// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Pins UFragmentOverrideState's mutators as PURE data operations -- no world, no transaction, no SFragmentsPanel, no .frag on disk.
#include "Misc/AutomationTest.h"
#include "Overrides/FragmentOverrideState.h"
#include "Editor.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	UFragmentOverrideState* MakeState()
	{
		UFragmentOverrideState* State = NewObject<UFragmentOverrideState>();
		State->Initialise(TEXT("guid-1"), TEXT("C:/models/a.frag"), FFragmentOverrides());
		return State;
	}

	FFragmentMaterialOverrideEntry MakeMeta(const FString& Key)
	{
		FFragmentMaterialOverrideEntry Meta;
		Meta.SourceKey = Key;
		Meta.SourceMaterialName = TEXT("mat_1_2_3_255");
		Meta.UsageCount = 7;
		return Meta;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideStateMutatorsTest,
	"FragmentsUnreal.Overrides.State_Mutators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideStateMutatorsTest::RunTest(const FString& Parameters)
{
	UFragmentOverrideState* State = MakeState();

	// SetOverride on an absent key appends.
	State->SetOverride(TEXT("rgba_1_2_3_255_glass_0"), TEXT("/Game/M_Brick"), MakeMeta(TEXT("rgba_1_2_3_255_glass_0")));
	TestEqual(TEXT("one entry after first set"), State->GetOverrides().MaterialOverrides.Num(), 1);

	// SetOverride on a PRESENT key replaces in place rather than appending -- a duplicated SourceKey would make FindOverride's answer depend on array order.
	State->SetOverride(TEXT("rgba_1_2_3_255_glass_0"), TEXT("/Game/M_Stone"), MakeMeta(TEXT("rgba_1_2_3_255_glass_0")));
	TestEqual(TEXT("still one entry after re-set"), State->GetOverrides().MaterialOverrides.Num(), 1);
	TestEqual(TEXT("path replaced"),
		State->FindOverride(TEXT("rgba_1_2_3_255_glass_0"))->ReplacementMaterialPath, FString(TEXT("/Game/M_Stone")));

	// An EMPTY replacement path is a clear, not an entry with an empty path: the sidecar's contract is that a present entry means a live override.
	State->SetOverride(TEXT("rgba_9_9_9_255_glass_0"), FString(), MakeMeta(TEXT("rgba_9_9_9_255_glass_0")));
	TestNull(TEXT("empty path stores no entry"), State->FindOverride(TEXT("rgba_9_9_9_255_glass_0")));

	// ClearOverride reports whether it changed anything, so a caller can skip a transaction.
	TestTrue(TEXT("clear of present key reports change"), State->ClearOverride(TEXT("rgba_1_2_3_255_glass_0")));
	TestFalse(TEXT("clear of absent key reports no change"), State->ClearOverride(TEXT("rgba_1_2_3_255_glass_0")));

	// ClearAllOverrides returns the count removed, which is what lets Reset All skip opening a transaction when there is nothing to reset.
	State->SetOverride(TEXT("k1"), TEXT("/Game/A"), MakeMeta(TEXT("k1")));
	State->SetOverride(TEXT("k2"), TEXT("/Game/B"), MakeMeta(TEXT("k2")));
	TestEqual(TEXT("clear all returns removed count"), State->ClearAllOverrides(), 2);
	TestEqual(TEXT("clear all empties"), State->GetOverrides().MaterialOverrides.Num(), 0);
	TestEqual(TEXT("clear all on empty returns zero"), State->ClearAllOverrides(), 0);

	// Initialise must preserve sibling blocks -- Placement lives in the same struct and is not this method's business.
	FFragmentOverrides Loaded;
	Loaded.Placement.bHasValue = true;
	Loaded.Placement.Location = FVector(1, 2, 3);
	UFragmentOverrideState* Second = NewObject<UFragmentOverrideState>();
	Second->Initialise(TEXT("guid-2"), TEXT("C:/models/b.frag"), Loaded);
	TestTrue(TEXT("placement survives Initialise"), Second->GetOverrides().Placement.bHasValue);
	TestEqual(TEXT("model guid recorded"), Second->GetModelGuid(), FString(TEXT("guid-2")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentOverrideStateOwnershipTest,
	"FragmentsUnreal.Overrides.State_Ownership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentOverrideStateOwnershipTest::RunTest(const FString& Parameters)
{
	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>();
	if (!Subsystem)
	{
		AddError(TEXT("No FragmentsImporterEditorSubsystem; cannot test ownership."));
		return false;
	}

	// A .frag that does not exist is the normal case for this test: Overrides::Load returns an empty set rather than failing, so state can be created for a model with no sidecar yet.
	const FString Guid = TEXT("ownership-test-guid");
	UFragmentOverrideState* First = Subsystem->GetOrCreateOverrideState(Guid, TEXT("C:/nonexistent/x.frag"));
	TestNotNull(TEXT("state created"), First);

	// IDENTITY, not equality: a second call must return the SAME object.
	UFragmentOverrideState* Second = Subsystem->GetOrCreateOverrideState(Guid, TEXT("C:/nonexistent/x.frag"));
	TestTrue(TEXT("same object returned on second call"), First == Second);

	// Find must not create.
	TestTrue(TEXT("find returns the same object"), Subsystem->FindOverrideState(Guid) == First);
	TestNull(TEXT("find does not create"), Subsystem->FindOverrideState(TEXT("never-created-guid")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
