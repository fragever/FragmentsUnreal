// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// The floor filter's per-bucket pass: slot 1 is written only where it changes, so a floor change touches the buckets
// (and instances) whose visibility actually flips instead of every piece of the building.
#include "Misc/AutomationTest.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Importer/FragmentFloorFilter.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace FloorFilterTestsPrivate
{
	UHierarchicalInstancedStaticMeshComponent* MakeBucket(int32 Instances, int32 CustomFloats)
	{
		UHierarchicalInstancedStaticMeshComponent* H =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		H->SetNumCustomDataFloats(CustomFloats);
		for (int32 i = 0; i < Instances; ++i)
		{
			H->AddInstance(FTransform(FVector(100.0 * i, 0, 0)));
		}
		return H;
	}

	float Slot1(const UHierarchicalInstancedStaticMeshComponent* H, int32 Instance)
	{
		return H->PerInstanceSMCustomData[Instance * H->NumCustomDataFloats + 1];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorFilterSlotValueTest, "FragmentsUnreal.FloorFilter.SlotValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloorFilterSlotValueTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::FloorFilter;
	const int16 Two = 2, Minus = -1;
	TestEqual(TEXT("an element with no floor reads as floor 0, shown when 0 is in range"), SlotValue(nullptr, 0, 3), 0.f);
	TestEqual(TEXT("an element with no floor is hidden when floor 0 is cut"), SlotValue(nullptr, 1, 3), 1.f);
	TestEqual(TEXT("a negative floor reads as floor 0"), SlotValue(&Minus, 1, 3), 1.f);
	TestEqual(TEXT("in range"), SlotValue(&Two, 2, 2), 0.f);
	TestEqual(TEXT("above the range"), SlotValue(&Two, 0, 1), 1.f);
	TestEqual(TEXT("below the range"), SlotValue(&Two, 3, 9), 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorFilterWritesOnlyChangesTest, "FragmentsUnreal.FloorFilter.WritesOnlyChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloorFilterWritesOnlyChangesTest::RunTest(const FString& Parameters)
{
	using namespace FloorFilterTestsPrivate;
	using namespace UE::Fragments::FloorFilter;

	UHierarchicalInstancedStaticMeshComponent* H = MakeBucket(4, 2);
	H->AddToRoot();
	ON_SCOPE_EXIT { H->RemoveFromRoot(); };

	// Elements 10..12 on floors 0..2, element 13 with no floor (reads as floor 0).
	const TArray<int64> Ids = { 10, 11, 12, 13 };
	const TMap<int64, int16> Floors = { { 10, 0 }, { 11, 1 }, { 12, 2 } };

	TestEqual(TEXT("cutting floor 0 changes the two floor-0 elements"), ApplyToBucket(*H, Ids, Floors, 1, 2), 2);
	TestEqual(TEXT("floor 0 hidden"), Slot1(H, 0), 1.f);
	TestEqual(TEXT("floor 1 shown"), Slot1(H, 1), 0.f);
	TestEqual(TEXT("floor 2 shown"), Slot1(H, 2), 0.f);
	TestEqual(TEXT("no floor hidden with floor 0"), Slot1(H, 3), 1.f);

	TestEqual(TEXT("the same range again changes nothing"), ApplyToBucket(*H, Ids, Floors, 1, 2), 0);
	TestEqual(TEXT("moving the top down changes only floor 2"), ApplyToBucket(*H, Ids, Floors, 1, 1), 1);
	TestEqual(TEXT("floor 2 now hidden"), Slot1(H, 2), 1.f);
	TestEqual(TEXT("everything back changes the three hidden ones"), ApplyToBucket(*H, Ids, Floors, 0, 2), 3);
	for (int32 i = 0; i < 4; ++i)
	{
		TestEqual(*FString::Printf(TEXT("instance %d shown"), i), Slot1(H, i), 0.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorFilterBucketEdgesTest, "FragmentsUnreal.FloorFilter.BucketEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloorFilterBucketEdgesTest::RunTest(const FString& Parameters)
{
	using namespace FloorFilterTestsPrivate;
	using namespace UE::Fragments::FloorFilter;

	const TMap<int64, int16> Floors = { { 1, 5 }, { 2, 5 } };

	// A bucket without slot 1 has nothing to write.
	UHierarchicalInstancedStaticMeshComponent* NoSlot = MakeBucket(2, 1);
	NoSlot->AddToRoot();
	ON_SCOPE_EXIT { NoSlot->RemoveFromRoot(); };
	TestEqual(TEXT("one custom float: no slot 1, nothing written"), ApplyToBucket(*NoSlot, { 1, 2 }, Floors, 0, 1), 0);

	// More local ids than instances (or fewer): only the instances that exist and have an id.
	UHierarchicalInstancedStaticMeshComponent* H = MakeBucket(2, 3);
	H->AddToRoot();
	ON_SCOPE_EXIT { H->RemoveFromRoot(); };
	TestEqual(TEXT("ids past the instance count are ignored"), ApplyToBucket(*H, { 1, 2, 3 }, Floors, 0, 1), 2);
	TestEqual(TEXT("slot 1 written at stride 3"), Slot1(H, 1), 1.f);
	TestEqual(TEXT("slot 2 untouched"), H->PerInstanceSMCustomData[1 * 3 + 2], 0.f);
	TestEqual(TEXT("a shorter id list leaves the rest alone"), ApplyToBucket(*H, { 1 }, Floors, 5, 5), 1);
	TestEqual(TEXT("instance 1 still hidden"), Slot1(H, 1), 1.f);
	return true;
}

// The floor pass owns "hidden by floor" (1) only: a category dim on a showing floor survives a floor change, so floor
// and category filters stop undoing each other (each floor step used to reset every dim to 0 for the category pass to
// write back: thousands of instance updates per step instead of one floor's worth).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorFilterKeepsDimTest, "FragmentsUnreal.FloorFilter.KeepsCategoryDim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloorFilterKeepsDimTest::RunTest(const FString& Parameters)
{
	using namespace FloorFilterTestsPrivate;
	using namespace UE::Fragments::FloorFilter;

	UHierarchicalInstancedStaticMeshComponent* H = MakeBucket(3, 2);
	H->AddToRoot();
	ON_SCOPE_EXIT { H->RemoveFromRoot(); };
	const TArray<int64> Ids = { 1, 2, 3 };
	const TMap<int64, int16> Floors = { { 1, 0 }, { 2, 0 }, { 3, 1 } };
	// A category view dims instances 1 and 2 (the second on floor 1).
	const float Dims[] = { 0.f, 0.3f, 0.3f };
	WriteSlot(*H, 3, [&](int32 i) { return Dims[i]; });

	TestEqual(TEXT("cutting floor 1 changes only its instance"), ApplyToBucket(*H, Ids, Floors, 0, 0), 1);
	TestEqual(TEXT("the dim on the showing floor survives"), Slot1(H, 1), 0.3f);
	TestEqual(TEXT("the cut floor is hidden"), Slot1(H, 2), 1.f);
	TestEqual(TEXT("the same range again: nothing"), ApplyToBucket(*H, Ids, Floors, 0, 0), 0);
	TestEqual(TEXT("bringing floor 1 back changes only its instance"), ApplyToBucket(*H, Ids, Floors, 0, 1), 1);
	TestEqual(TEXT("a floor coming back is shown (its dim is the category pass's to restore)"), Slot1(H, 2), 0.f);
	TestEqual(TEXT("the other dim still there"), Slot1(H, 1), 0.3f);
	return true;
}

// The category filter writes a dim value through the same pass: only changed instances, any value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorFilterWriteSlotDimTest, "FragmentsUnreal.FloorFilter.WriteSlotDim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloorFilterWriteSlotDimTest::RunTest(const FString& Parameters)
{
	using namespace FloorFilterTestsPrivate;
	using namespace UE::Fragments::FloorFilter;

	UHierarchicalInstancedStaticMeshComponent* H = MakeBucket(3, 2);
	H->AddToRoot();
	ON_SCOPE_EXIT { H->RemoveFromRoot(); };
	const float Dims[] = { 0.f, 0.3f, 0.3f };
	TestEqual(TEXT("two instances dimmed"), WriteSlot(*H, 3, [&](int32 i) { return Dims[i]; }), 2);
	TestEqual(TEXT("dim value written"), Slot1(H, 1), 0.3f);
	TestEqual(TEXT("the same values again: nothing written"), WriteSlot(*H, 3, [&](int32 i) { return Dims[i]; }), 0);
	TestEqual(TEXT("a count past the instances is clamped"), WriteSlot(*H, 9, [](int32) { return 0.f; }), 2);
	return true;
}

#endif
