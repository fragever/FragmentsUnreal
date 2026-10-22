// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Misc/AutomationTest.h"
#include "Utils/FrameBudgetCoordinator.h"
#include "Importer/FragmentsImporter.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBudgetCoordinatorAccountingTest,
	"FragmentsUnreal.Budget.Coordinator_AccumulatesAndResetsPerFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBudgetCoordinatorAccountingTest::RunTest(const FString& Parameters)
{
	FFrameBudgetCoordinator C;
	C.TotalFrameBudgetMs = 4.0f;

	// Fresh frame 100: full budget available.
	TestEqual(TEXT("fresh frame full budget"), C.AllocateRemainingMs(100), 4.0f);

	// Spend 1.5ms in frame 100; remaining shrinks.
	C.RecordWork(1.5f, 100);
	TestEqual(TEXT("after 1.5ms remaining"), C.AllocateRemainingMs(100), 2.5f);

	// Spend 3.0ms more in frame 100; clamps at 0 (never negative).
	C.RecordWork(3.0f, 100);
	TestEqual(TEXT("overspend clamps to zero"), C.AllocateRemainingMs(100), 0.0f);

	// New frame 101: resets to full budget.
	TestEqual(TEXT("new frame resets"), C.AllocateRemainingMs(101), 4.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalizeGateDecisionTest,
	"FragmentsUnreal.Budget.Finalize_GateDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFinalizeGateDecisionTest::RunTest(const FString& Parameters)
{
	const float Full = 4.0f;

	// Fits the remaining budget -> Run.
	TestEqual(TEXT("fits remaining"),
		(int)UFragmentsImporter::DecideFinalizeGate(4.0f, 2.0f, Full), (int)EFinalizeGate::Run);

	// Exactly equal -> Run (<= boundary).
	TestEqual(TEXT("equal boundary runs"),
		(int)UFragmentsImporter::DecideFinalizeGate(2.0f, 2.0f, Full), (int)EFinalizeGate::Run);

	// Too big for remaining but fits a fresh full budget -> Defer.
	TestEqual(TEXT("defer to fresh slice"),
		(int)UFragmentsImporter::DecideFinalizeGate(1.0f, 3.0f, Full), (int)EFinalizeGate::Defer);

	// Un-fittable (est > full) on a fresh frame -> RunAlone.
	TestEqual(TEXT("oversized fresh runs alone"),
		(int)UFragmentsImporter::DecideFinalizeGate(4.0f, 6.0f, Full), (int)EFinalizeGate::RunAlone);

	// Un-fittable but frame already partly spent -> Defer (wait for fresh frame).
	TestEqual(TEXT("oversized partial defers"),
		(int)UFragmentsImporter::DecideFinalizeGate(2.5f, 6.0f, Full), (int)EFinalizeGate::Defer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalizeQueueDrainOrderTest,
	"FragmentsUnreal.Budget.Finalize_QueueDrainsFIFOAndFiresFinalizer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFinalizeQueueDrainOrderTest::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	TArray<int32> FiredOrder;
	for (int32 i = 0; i < 3; ++i)
	{
		FPendingMeshFinalize P;
		P.DebugLabel = FString::Printf(TEXT("item%d"), i);
		P.EstimatedTriangleCount = 0;   // 0ms estimate -> always fits the gate
		P.Finalizer = [&FiredOrder, i](UStaticMesh*) { FiredOrder.Add(i); };
		Importer->EnqueueMeshFinalize(MoveTemp(P));   // WeakMesh left null on purpose
	}

	TestEqual(TEXT("three queued"), Importer->NumPendingMeshFinalizes(), 3);

	Importer->DrainMeshFinalizes();   // full budget, 0ms items -> drains all three

	TestEqual(TEXT("queue emptied"), Importer->NumPendingMeshFinalizes(), 0);
	TestEqual(TEXT("all three fired"), FiredOrder.Num(), 3);
	if (FiredOrder.Num() == 3)
	{
		TestEqual(TEXT("FIFO order 0"), FiredOrder[0], 0);
		TestEqual(TEXT("FIFO order 1"), FiredOrder[1], 1);
		TestEqual(TEXT("FIFO order 2"), FiredOrder[2], 2);
	}

	Importer->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFinalizeQueueFlushTest,
	"FragmentsUnreal.Budget.Finalize_FlushFiresNullAndUnroots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFinalizeQueueFlushTest::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	// One payload with a real rooted mesh (verifies FlushMeshFinalizes unroots it), two with a null WeakMesh (the RunMeshFinalize-style "already gone" case).
	UStaticMesh* RootedMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	RootedMesh->AddToRoot();

	TArray<UStaticMesh*> FiredWith;
	for (int32 i = 0; i < 3; ++i)
	{
		FPendingMeshFinalize P;
		P.DebugLabel = FString::Printf(TEXT("flush%d"), i);
		P.EstimatedTriangleCount = 0;
		if (i == 0) { P.WeakMesh = RootedMesh; }
		P.Finalizer = [&FiredWith](UStaticMesh* M) { FiredWith.Add(M); };
		Importer->EnqueueMeshFinalize(MoveTemp(P));
	}

	TestEqual(TEXT("three queued before flush"), Importer->NumPendingMeshFinalizes(), 3);

	Importer->FlushMeshFinalizes();

	TestEqual(TEXT("queue emptied by flush"), Importer->NumPendingMeshFinalizes(), 0);
	TestEqual(TEXT("all three finalizers fired"), FiredWith.Num(), 3);
	for (int32 i = 0; i < FiredWith.Num(); ++i)
	{
		TestNull(*FString::Printf(TEXT("finalize %d fired with null mesh"), i), FiredWith[i]);
	}
	TestFalse(TEXT("rooted mesh was unrooted by flush"), RootedMesh->IsRooted());

	// RootedMesh is no longer rooted; it will be GC'd normally. Don't touch it further.
	Importer->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
