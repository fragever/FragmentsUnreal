// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Utils/BudgetDrain.h"
#include "Utils/FrameBudgetCoordinator.h"
#include "Importer/FragmentsImporter.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergePacingDrainTest,
	"FragmentsUnreal.CellMergePacing.DrainBudgeted_MinOneAndBudgetStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergePacingDrainTest::RunTest(const FString& Parameters)
{
	// Min-1: with the frame already fully spent, the drain still advances exactly one item.
	{
		FFrameBudgetCoordinator B;
		B.TotalFrameBudgetMs = 4.0f;
		B.RecordWork(4.0f, /*Frame*/ 100); // exhaust frame 100
		int32 Cursor = 0, Processed = 0;
		const bool bDone = FragmentBudgetDrain::DrainBudgeted(Cursor, 10, B, 100,
			[&](int32) { ++Processed; });
		TestEqual(TEXT("min-1 advances exactly one under zero budget"), Cursor, 1);
		TestEqual(TEXT("did one unit of work"), Processed, 1);
		TestFalse(TEXT("not fully drained"), bDone);
	}

	// Budget-stop: 4 ms budget, each item records 1 ms -> exactly 4 items this frame.
	{
		FFrameBudgetCoordinator B;
		B.TotalFrameBudgetMs = 4.0f;
		int32 Cursor = 0;
		const bool bDone = FragmentBudgetDrain::DrainBudgeted(Cursor, 10, B, 200,
			[&](int32) { B.RecordWork(1.0f, 200); });
		TestEqual(TEXT("drains 4 items within a 4 ms budget"), Cursor, 4);
		TestFalse(TEXT("not fully drained"), bDone);
	}

	// Completion across frames: fresh budget each frame resumes the cursor to the end.
	{
		FFrameBudgetCoordinator B;
		B.TotalFrameBudgetMs = 4.0f;
		int32 Cursor = 0;
		FragmentBudgetDrain::DrainBudgeted(Cursor, 10, B, 301, [&](int32) { B.RecordWork(1.0f, 301); });
		FragmentBudgetDrain::DrainBudgeted(Cursor, 10, B, 302, [&](int32) { B.RecordWork(1.0f, 302); });
		const bool bDone = FragmentBudgetDrain::DrainBudgeted(Cursor, 10, B, 303,
			[&](int32) { B.RecordWork(1.0f, 303); });
		TestEqual(TEXT("cursor reaches the end"), Cursor, 10);
		TestTrue(TEXT("reports done"), bDone);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellMergeWorkListTest,
	"FragmentsUnreal.CellMergePacing.BuildWorkList_ShellFirstGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellMergeWorkListTest::RunTest(const FString& Parameters)
{
	const TArray<FCellId> Shell    = { FCellId(0, 0, 0), FCellId(0, 1, 0) };
	const TArray<FCellId> Interior = { FCellId(0, 2, 0) };

	// First pass: shells not yet enqueued -> shells (bShell=true) then interiors (false); flag flips.
	bool bShellEnqueued = false;
	TArray<TPair<FCellId, bool>> Work1;
	UFragmentsImporter::BuildCellMergeWorkList(Shell, Interior, bShellEnqueued, Work1);
	TestEqual(TEXT("first pass: 2 shell + 1 interior"), Work1.Num(), 3);
	TestTrue(TEXT("shell entries marked bShell"), Work1[0].Value && Work1[1].Value);
	TestFalse(TEXT("interior entry not bShell"), Work1[2].Value);
	TestTrue(TEXT("flag now set"), bShellEnqueued);

	// Second pass: shells already enqueued -> interiors only.
	TArray<TPair<FCellId, bool>> Work2;
	UFragmentsImporter::BuildCellMergeWorkList(Shell, Interior, bShellEnqueued, Work2);
	TestEqual(TEXT("second pass: interiors only"), Work2.Num(), 1);
	TestFalse(TEXT("interior entry not bShell"), Work2[0].Value);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOverrideApplyPacingTest,
	"FragmentsUnreal.CellMergePacing.OverrideApply_DrainsAcrossFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOverrideApplyPacingTest::RunTest(const FString& Parameters)
{
	// Same drain contract the override phase relies on: 6 items, 2 ms budget, 1 ms each -> 2 frames.
	FFrameBudgetCoordinator B;
	B.TotalFrameBudgetMs = 2.0f;
	int32 Cursor = 0;
	const bool bDone1 = FragmentBudgetDrain::DrainBudgeted(Cursor, 6, B, 400, [&](int32){ B.RecordWork(1.0f, 400); });
	TestEqual(TEXT("frame 1 drains 2"), Cursor, 2);
	TestFalse(TEXT("not done after frame 1"), bDone1);
	FragmentBudgetDrain::DrainBudgeted(Cursor, 6, B, 401, [&](int32){ B.RecordWork(1.0f, 401); });
	FragmentBudgetDrain::DrainBudgeted(Cursor, 6, B, 402, [&](int32){ B.RecordWork(1.0f, 402); });
	const bool bDone3 = FragmentBudgetDrain::DrainBudgeted(Cursor, 6, B, 403, [&](int32){ B.RecordWork(1.0f, 403); });
	TestEqual(TEXT("drains all 6"), Cursor, 6);
	TestTrue(TEXT("done"), bDone3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
