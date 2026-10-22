// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Utils/FragmentCellPartition.h"

#if WITH_DEV_AUTOMATION_TESTS

using UE::Fragments::CellPartition::CollapseSparseCells;

// CollapseSparseCells produces the same partition for any cell insertion order.

namespace
{
	/** A row of cells on floor 0 at (0, X, 0), inserted in the caller's order. */
	TMap<FCellId, FCellMetadata> MakeRow(const TArray<int16>& InsertOrder, const TArray<int32>& Counts)
	{
		check(InsertOrder.Num() == Counts.Num());
		TMap<FCellId, FCellMetadata> Cells;
		for (int32 i = 0; i < InsertOrder.Num(); ++i)
		{
			const int16 X = InsertOrder[i];
			FCellMetadata& Meta = Cells.Add(FCellId(0, X, 0));
			Meta.ItemCount = Counts[i];
			Meta.WorldBounds = FBox(
				FVector(X * 100.0, 0.0, 0.0),
				FVector(X * 100.0 + 100.0, 100.0, 100.0));
		}
		return Cells;
	}

	/** Every cell at the same count, in the caller's insertion order. */
	TMap<FCellId, FCellMetadata> MakeUniformRow(const TArray<int16>& InsertOrder, int32 CountEach)
	{
		TArray<int32> Counts;
		Counts.Init(CountEach, InsertOrder.Num());
		return MakeRow(InsertOrder, Counts);
	}

	/** The partition as a single ordered string, so a failure names both sides. */
	FString Describe(const TMap<FCellId, FCellMetadata>& Cells)
	{
		TArray<FCellId> Keys;
		Cells.GetKeys(Keys);
		Keys.Sort();
		FString Out;
		for (const FCellId& Key : Keys)
		{
			Out += FString::Printf(TEXT("(%d,%d,%d)=%d "),
				Key.Floor, Key.GridX, Key.GridY, Cells[Key].ItemCount);
		}
		return Out;
	}
}

// The candidate SORT: equal-count sparse cells must be processed in a fixed order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellPartitionCollapseIgnoresInsertionOrder,
	"FragmentsUnreal.CellPartition.CollapseDeterminism.IgnoresInsertionOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellPartitionCollapseIgnoresInsertionOrder::RunTest(const FString& Parameters)
{
	// Three adjacent, equally sparse cells -- the smallest set where the PROCESSING order changes the answer.
	const int32 MinInstances = 10;
	const int32 MaxRadius    = 1;

	TMap<FCellId, FCellMetadata> Ascending  = MakeUniformRow({ 0, 1, 2 }, 5);
	TMap<FCellId, FCellMetadata> MiddleFirst = MakeUniformRow({ 1, 0, 2 }, 5);
	TMap<FCellId, FCellMetadata> Descending = MakeUniformRow({ 2, 1, 0 }, 5);

	CollapseSparseCells(Ascending,   MinInstances, MaxRadius);
	CollapseSparseCells(MiddleFirst, MinInstances, MaxRadius);
	CollapseSparseCells(Descending,  MinInstances, MaxRadius);

	// The order the fix pins: lowest FCellId first among equal counts. (0,0,0) is absorbed into (0,1,0), which is then above threshold, and (0,2,0) joins it.
	TestEqual(TEXT("ascending insertion order collapses to the lowest-id-first partition"),
		Describe(Ascending), FString(TEXT("(0,1,0)=15 ")));
	TestEqual(TEXT("middle-first insertion order produces the same partition"),
		Describe(MiddleFirst), Describe(Ascending));
	TestEqual(TEXT("descending insertion order produces the same partition"),
		Describe(Descending), Describe(Ascending));

	return true;
}

// The NEIGHBOUR search: equal-count absorbing neighbours must be decided the same way every time.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellPartitionAbsorbingNeighbourIsDeterministic,
	"FragmentsUnreal.CellPartition.CollapseDeterminism.AbsorbingNeighbourIsDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellPartitionAbsorbingNeighbourIsDeterministic::RunTest(const FString& Parameters)
{
	// One sparse cell between two dense neighbours of IDENTICAL count.
	const int32 MinInstances = 10;
	const int32 MaxRadius    = 1;

	TMap<FCellId, FCellMetadata> LeftFirst  = MakeRow({ 0, 1, 2 }, { 20, 5, 20 });
	TMap<FCellId, FCellMetadata> RightFirst = MakeRow({ 2, 1, 0 }, { 20, 5, 20 });

	CollapseSparseCells(LeftFirst,  MinInstances, MaxRadius);
	CollapseSparseCells(RightFirst, MinInstances, MaxRadius);

	TestEqual(TEXT("the lowest-id neighbour absorbs the sparse cell"),
		Describe(LeftFirst), FString(TEXT("(0,0,0)=25 (0,2,0)=20 ")));
	TestEqual(TEXT("the reversed insertion order absorbs into the same neighbour"),
		Describe(RightFirst), Describe(LeftFirst));

	return true;
}

// A wider row, three fill orders.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellPartitionCollapseIsStableOverAWideRow,
	"FragmentsUnreal.CellPartition.CollapseDeterminism.StableOverAWideRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellPartitionCollapseIsStableOverAWideRow::RunTest(const FString& Parameters)
{
	const TArray<int16> Ascending   = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	const TArray<int16> Descending  = { 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
	const TArray<int16> Interleaved = { 5, 0, 11, 3, 8, 1, 9, 2, 7, 4, 10, 6 };

	TMap<FCellId, FCellMetadata> A = MakeUniformRow(Ascending, 5);
	TMap<FCellId, FCellMetadata> D = MakeUniformRow(Descending, 5);
	TMap<FCellId, FCellMetadata> I = MakeUniformRow(Interleaved, 5);

	CollapseSparseCells(A, 10, 1);
	CollapseSparseCells(D, 10, 1);
	CollapseSparseCells(I, 10, 1);

	TestEqual(TEXT("descending fill order gives the ascending partition"), Describe(D), Describe(A));
	TestEqual(TEXT("interleaved fill order gives the ascending partition"), Describe(I), Describe(A));

	// Sanity: the pass did something and conserved every item. 12 cells of 5 is 60 items whatever the grouping.
	TestTrue(TEXT("the collapse pass actually merged cells"), A.Num() < 12);
	int32 Total = 0;
	for (const TPair<FCellId, FCellMetadata>& Pair : A) { Total += Pair.Value.ItemCount; }
	TestEqual(TEXT("no items were lost or duplicated by the collapse"), Total, 60);

	return true;
}

// The ordering itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCellIdOrdersLexicographically,
	"FragmentsUnreal.CellPartition.CellId.OrdersLexicographically",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCellIdOrdersLexicographically::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("floor is the most significant field"),
		FCellId(0, 99, 99) < FCellId(1, 0, 0));
	TestTrue(TEXT("grid X outranks grid Y"),
		FCellId(0, 1, 99) < FCellId(0, 2, 0));
	TestTrue(TEXT("grid Y breaks the last tie"),
		FCellId(0, 1, 1) < FCellId(0, 1, 2));
	TestFalse(TEXT("the order is strict: a cell does not precede itself"),
		FCellId(3, 4, 5) < FCellId(3, 4, 5));
	TestTrue(TEXT("negative coordinates order below positive ones"),
		FCellId(-1, -1, -1) < FCellId(0, 0, 0));

	// Sorting a shuffled array must land in exactly one arrangement.
	TArray<FCellId> Shuffled = {
		FCellId(1, 0, 0), FCellId(0, 2, 1), FCellId(0, 2, 0), FCellId(0, 1, 5), FCellId(-1, 9, 9),
	};
	Shuffled.Sort();
	const TArray<FCellId> Expected = {
		FCellId(-1, 9, 9), FCellId(0, 1, 5), FCellId(0, 2, 0), FCellId(0, 2, 1), FCellId(1, 0, 0),
	};
	bool bMatches = (Shuffled.Num() == Expected.Num());
	for (int32 i = 0; bMatches && i < Shuffled.Num(); ++i)
	{
		bMatches = (Shuffled[i] == Expected[i]);
	}
	TestTrue(TEXT("TArray::Sort with the default predicate lands lexicographically"), bMatches);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
