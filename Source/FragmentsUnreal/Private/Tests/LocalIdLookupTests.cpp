// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Utils/FragmentsUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

// Guards the O(N) load-phase lookup: BuildLocalIdToItemMap must index every node of an FFragmentItem hierarchy (root + nested children) by LocalId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLocalIdToItemMapTest,
	"FragmentsUnreal.Lookup.BuildLocalIdToItemMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLocalIdToItemMapTest::RunTest(const FString& Parameters)
{
	// Root(-1) -> A(10) -> B(20), Root -> C(30).
	FFragmentItem Root;
	Root.LocalId = -1;

	FFragmentItem* A = new FFragmentItem(); A->LocalId = 10;
	FFragmentItem* B = new FFragmentItem(); B->LocalId = 20;
	FFragmentItem* C = new FFragmentItem(); C->LocalId = 30;
	A->FragmentChildren.Add(B);
	Root.FragmentChildren.Add(A);
	Root.FragmentChildren.Add(C);

	TMap<int64, FFragmentItem*> Map;
	UFragmentsUtils::BuildLocalIdToItemMap(Root, Map);

	TestEqual(TEXT("map size"), Map.Num(), 4);
	TestTrue(TEXT("root indexed"), Map.FindRef(-1) == &Root);
	TestTrue(TEXT("child A"), Map.FindRef(10) == A);
	TestTrue(TEXT("nested child B"), Map.FindRef(20) == B);
	TestTrue(TEXT("child C"), Map.FindRef(30) == C);
	TestTrue(TEXT("miss returns null"), Map.FindRef(999) == nullptr);

	delete B; delete A; delete C;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
