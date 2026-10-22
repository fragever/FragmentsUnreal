// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "Engine/StaticMesh.h"
#include "Async/TaskGraphInterfaces.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ShellBuildRegistryTestsPrivate
{
	FPendingHISMItem MakeItem(int64 LocalId)
	{
		FPendingHISMItem Item;
		Item.LocalId = LocalId;
		Item.Category = TEXT("IfcWall");     // shell category
		Item.ModelGuid = TEXT("TestModel");
		Item.WorldTransform = FTransform::Identity;
		return Item;
	}

	FPendingHISMItem MakeItemForModel(int64 LocalId, const FString& ModelGuid)
	{
		FPendingHISMItem Item = MakeItem(LocalId);
		Item.ModelGuid = ModelGuid;
		return Item;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellBuildDedupTest,
	"FragmentsUnreal.ShellBuild.Registry_DedupsWaitersOntoOneBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellBuildDedupTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	// Resolve each item against its own model wrapper before cell processing.
	UFragmentModelWrapper* Wrapper = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	Wrapper->AddToRoot();
	Importer->GetFragmentModels_Mutable().Add(TEXT("TestModel"), Wrapper);

	const FString SamplePath = TEXT("/Game/Buildings/TestModel/rep_7.rep_7");

	// Three items share one representation: one registry entry, three waiters.
	TestTrue(TEXT("first item starts a build"), Importer->RegisterShellBuildWaiter(SamplePath, MakeItem(1)));
	TestFalse(TEXT("second item only waits"), Importer->RegisterShellBuildWaiter(SamplePath, MakeItem(2)));
	TestFalse(TEXT("third item only waits"), Importer->RegisterShellBuildWaiter(SamplePath, MakeItem(3)));
	TestEqual(TEXT("one build in flight"), Importer->NumInFlightShellBuilds(), 1);

	// Completion caches the mesh and clears the registry entry.
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	UFragmentsImporter::FShellBuildResult Result;
	Result.Mesh = Mesh;
	Importer->CompleteShellBuild(SamplePath, MoveTemp(Result));

	TestEqual(TEXT("registry drained"), Importer->NumInFlightShellBuilds(), 0);
	TestEqual(TEXT("mesh cached for reuse"), Importer->FindCachedMesh(SamplePath), Mesh);
	TestEqual(TEXT("all three waiters reached the bucket table"), Importer->NumPendingBucketInstances(), 3);

	Importer->RemoveFromRoot();
	Wrapper->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellBuildNullMeshTest,
	"FragmentsUnreal.ShellBuild.Registry_HandlesFailedBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellBuildNullMeshTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString SamplePath = TEXT("/Game/Buildings/TestModel/rep_9.rep_9");
	Importer->RegisterShellBuildWaiter(SamplePath, MakeItem(1));
	Importer->RegisterShellBuildWaiter(SamplePath, MakeItem(2));

	// A failed author delivers a null mesh: registry must still drain cleanly.
	UFragmentsImporter::FShellBuildResult Failed;
	Importer->CompleteShellBuild(SamplePath, MoveTemp(Failed));

	TestEqual(TEXT("registry drained on failure"), Importer->NumInFlightShellBuilds(), 0);
	TestNull(TEXT("null cached so retries do not loop"), Importer->FindCachedMesh(SamplePath));
	TestEqual(TEXT("waiters dropped, not queued, on a null mesh"), Importer->NumPendingBucketInstances(), 0);

	Importer->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellBuildBackpressureTest,
	"FragmentsUnreal.ShellBuild.Backpressure_CapScalesWithWorkers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellBuildBackpressureTest::RunTest(const FString& Parameters)
{
	// 2x workers, clamped to [8, 128].
	TestEqual(TEXT("typical 12 workers -> 24"), UFragmentsImporter::ResolveMaxInFlightShellBuilds(12), 24);
	TestEqual(TEXT("tiny machine clamps to floor"), UFragmentsImporter::ResolveMaxInFlightShellBuilds(1), 8);
	TestEqual(TEXT("zero workers clamps to floor"), UFragmentsImporter::ResolveMaxInFlightShellBuilds(0), 8);
	TestEqual(TEXT("huge machine clamps to ceiling"), UFragmentsImporter::ResolveMaxInFlightShellBuilds(256), 128);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellBuildFinalizeBarrierTest,
	"FragmentsUnreal.ShellBuild.Barrier_DefersBucketFinalizeUntilBuildsDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellBuildFinalizeBarrierTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	// Resolve each waiter against its own model wrapper.
	UFragmentModelWrapper* Wrapper = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	Wrapper->AddToRoot();
	Importer->GetFragmentModels_Mutable().Add(TEXT("TestModel"), Wrapper);

	TestFalse(TEXT("no builds -> no deferral"), Importer->ShouldDeferBucketFinalize());

	const FString SamplePath = TEXT("/Game/Buildings/TestModel/rep_3.rep_3");
	Importer->RegisterShellBuildWaiter(SamplePath, MakeItem(1));
	TestTrue(TEXT("in-flight build defers finalize"), Importer->ShouldDeferBucketFinalize());

	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	UFragmentsImporter::FShellBuildResult Result;
	Result.Mesh = Mesh;
	Importer->CompleteShellBuild(SamplePath, MoveTemp(Result));

	TestFalse(TEXT("drained -> finalize allowed"), Importer->ShouldDeferBucketFinalize());

	Importer->RemoveFromRoot();
	Wrapper->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyContributionOrderTest,
	"FragmentsUnreal.ShellBuild.ProxyContributions_FlushInDeterministicOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyContributionOrderTest::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString ModelGuid = TEXT("TestModel");

	// Record contributions out of order (simulating async completion order).
	Importer->RecordProxyContribution(ModelGuid, /*OrderKey=*/ 30, TEXT("pathC"), FTransform::Identity, 3, nullptr);
	Importer->RecordProxyContribution(ModelGuid, /*OrderKey=*/ 10, TEXT("pathA"), FTransform::Identity, 1, nullptr);
	Importer->RecordProxyContribution(ModelGuid, /*OrderKey=*/ 20, TEXT("pathB"), FTransform::Identity, 2, nullptr);

	const TArray<int64> Sorted = Importer->GetSortedProxyContributionKeysForTests(ModelGuid);
	TestEqual(TEXT("three contributions"), Sorted.Num(), 3);
	if (Sorted.Num() == 3)
	{
		TestEqual(TEXT("sorted 0"), Sorted[0], (int64)10);
		TestEqual(TEXT("sorted 1"), Sorted[1], (int64)20);
		TestEqual(TEXT("sorted 2"), Sorted[2], (int64)30);
	}

	Importer->RemoveFromRoot();
	return true;
}

namespace ShellBuildRegistryTestsPrivate
{
	// One distinguishable vertex — Position.X is the only thing this test reads back, used purely to tell "which contribution's geometry landed first" in the flushed section's Positions array.
	FFragmentMeshData MakeSingleVertexMeshData(float DistinguishingX)
	{
		FFragmentMeshData Data;
		FFragmentMeshVertex V;
		V.Position = FVector3f(DistinguishingX, 0.f, 0.f);
		V.Normal = FVector3f::ZAxisVector;
		V.TangentX = FVector3f::XAxisVector;
		V.TangentYSign = 1.f;
		V.UV0 = FVector2f::ZeroVector;
		V.Color = FColor::White;
		Data.Vertices.Add(V);
		Data.bIsValid = true;
		return Data;
	}
}

// QueueIndex gives each sample a stable order even when LocalId is shared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyContributionFlushOrderIndependentTest,
	"FragmentsUnreal.ShellBuild.ProxyContributions_FlushIsOrderIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyContributionFlushOrderIndependentTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString PathX = TEXT("pathX");
	const FString PathY = TEXT("pathY");
	constexpr float XVertexX = 1.0f;
	constexpr float YVertexX = 2.0f;
	constexpr uint32 SharedMatCRC = 77;   // shared -> both land in one section

	Importer->StashProxyMeshDataForTests(PathX, MakeSingleVertexMeshData(XVertexX));
	Importer->StashProxyMeshDataForTests(PathY, MakeSingleVertexMeshData(YVertexX));

	// Run A: record in ascending-OrderKey order (X's OrderKey=100 recorded first).
	const FString ModelGuidA = TEXT("TestModel_OrderA");
	Importer->RecordProxyContribution(ModelGuidA, /*OrderKey=*/ 100, PathX, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->RecordProxyContribution(ModelGuidA, /*OrderKey=*/ 200, PathY, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->FlushProxyContributions(ModelGuidA);

	// Run B: record in descending-OrderKey order (Y recorded first) — simulates Y's async build completing before X's, which is exactly the case a naive tie-prone key (like the old LocalId) could not survive.
	const FString ModelGuidB = TEXT("TestModel_OrderB");
	Importer->RecordProxyContribution(ModelGuidB, /*OrderKey=*/ 200, PathY, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->RecordProxyContribution(ModelGuidB, /*OrderKey=*/ 100, PathX, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->FlushProxyContributions(ModelGuidB);

	const FProxyMeshAccumulator* AccumA = Importer->GetProxyAccumulatorForTests(ModelGuidA);
	const FProxyMeshAccumulator* AccumB = Importer->GetProxyAccumulatorForTests(ModelGuidB);

	const bool bHaveBothAccumulators = TestNotNull(TEXT("run A accumulator exists"), AccumA)
		& TestNotNull(TEXT("run B accumulator exists"), AccumB);
	if (bHaveBothAccumulators)
	{
		const FProxyMeshSection* SectionA = AccumA->Sections.Find(SharedMatCRC);
		const FProxyMeshSection* SectionB = AccumB->Sections.Find(SharedMatCRC);

		const bool bHaveBothSections = TestNotNull(TEXT("run A section exists"), SectionA)
			& TestNotNull(TEXT("run B section exists"), SectionB);
		if (bHaveBothSections)
		{
			TestEqual(TEXT("run A has both vertices"), SectionA->Positions.Num(), 2);
			TestEqual(TEXT("run B has both vertices"), SectionB->Positions.Num(), 2);

			if (SectionA->Positions.Num() == 2 && SectionB->Positions.Num() == 2)
			{
				// Both runs must replay X before Y (OrderKey 100 < 200), regardless of which order they were recorded in.
				TestEqual(TEXT("run A: X first"), SectionA->Positions[0].X, XVertexX);
				TestEqual(TEXT("run A: Y second"), SectionA->Positions[1].X, YVertexX);
				TestEqual(TEXT("run B: X first"), SectionB->Positions[0].X, XVertexX);
				TestEqual(TEXT("run B: Y second"), SectionB->Positions[1].X, YVertexX);

				// And the two runs must match each other byte-for-byte, not just each independently happen to start with X.
				TestEqual(TEXT("both runs agree on vertex 0"), SectionA->Positions[0], SectionB->Positions[0]);
				TestEqual(TEXT("both runs agree on vertex 1"), SectionA->Positions[1], SectionB->Positions[1]);
			}
		}
	}

	Importer->RemoveFromRoot();
	return true;
}

// Determinism is a HARD requirement for the proxy accumulator: moving accumulation off the game thread must not perturb vertex order by even one element.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyContributionResolvedAccumulateMatchesSingleShotTest,
	"FragmentsUnreal.ShellBuild.ProxyContributions_ResolvedAccumulateMatchesSingleShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyContributionResolvedAccumulateMatchesSingleShotTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString PathX = TEXT("resolvedPathX");
	const FString PathY = TEXT("resolvedPathY");
	const FString PathZ = TEXT("resolvedPathZ");
	constexpr float XVertexX = 10.0f;
	constexpr float YVertexX = 20.0f;
	constexpr float ZVertexX = 30.0f;
	constexpr uint32 SharedMatCRC = 99; // shared -> all three land in one section

	Importer->StashProxyMeshDataForTests(PathX, MakeSingleVertexMeshData(XVertexX));
	Importer->StashProxyMeshDataForTests(PathY, MakeSingleVertexMeshData(YVertexX));
	Importer->StashProxyMeshDataForTests(PathZ, MakeSingleVertexMeshData(ZVertexX));

	// Reference path: single-shot FlushProxyContributions, recorded out of order.
	const FString ModelSingleShot = TEXT("TestModel_SingleShot");
	Importer->RecordProxyContribution(ModelSingleShot, /*OrderKey=*/ 30, PathZ, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->RecordProxyContribution(ModelSingleShot, /*OrderKey=*/ 10, PathX, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->RecordProxyContribution(ModelSingleShot, /*OrderKey=*/ 20, PathY, FTransform::Identity, SharedMatCRC, nullptr);
	Importer->FlushProxyContributions(ModelSingleShot);

	// Worker path: same contributions, sorted then resolved then accumulated.
	TArray<UFragmentsImporter::FProxyContribution> Sorted;
	Sorted.Add({ /*OrderKey=*/ 30, PathZ, FTransform::Identity, SharedMatCRC, nullptr });
	Sorted.Add({ /*OrderKey=*/ 10, PathX, FTransform::Identity, SharedMatCRC, nullptr });
	Sorted.Add({ /*OrderKey=*/ 20, PathY, FTransform::Identity, SharedMatCRC, nullptr });
	Sorted.Sort([](const UFragmentsImporter::FProxyContribution& A,
	               const UFragmentsImporter::FProxyContribution& B)
	{
		return A.OrderKey < B.OrderKey;
	});

	const TArray<UFragmentsImporter::FResolvedProxyContribution> Payload =
		Importer->ResolveProxyContributionsForTests(Sorted);
	TestEqual(TEXT("all three contributions resolved"), Payload.Num(), 3);

	FProxyMeshAccumulator WorkerAccum;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, WorkerAccum);

	const FProxyMeshAccumulator* AccumSingleShot = Importer->GetProxyAccumulatorForTests(ModelSingleShot);
	if (TestNotNull(TEXT("single-shot accumulator exists"), AccumSingleShot))
	{
		const FProxyMeshSection* SectionSingleShot = AccumSingleShot->Sections.Find(SharedMatCRC);
		const FProxyMeshSection* SectionWorker = WorkerAccum.Sections.Find(SharedMatCRC);

		const bool bHaveBoth = TestNotNull(TEXT("single-shot section exists"), SectionSingleShot)
			& TestNotNull(TEXT("worker section exists"), SectionWorker);
		if (bHaveBoth)
		{
			const bool bBothHaveThree =
				TestEqual(TEXT("single-shot has all three vertices"), SectionSingleShot->Positions.Num(), 3)
				& TestEqual(TEXT("worker has all three vertices"), SectionWorker->Positions.Num(), 3);

			if (bBothHaveThree)
			{
				TestEqual(TEXT("vertex 0 matches"), SectionSingleShot->Positions[0], SectionWorker->Positions[0]);
				TestEqual(TEXT("vertex 1 matches"), SectionSingleShot->Positions[1], SectionWorker->Positions[1]);
				TestEqual(TEXT("vertex 2 matches"), SectionSingleShot->Positions[2], SectionWorker->Positions[2]);

				TestEqual(TEXT("vertex 0 is X (OrderKey 10)"), SectionWorker->Positions[0].X, XVertexX);
				TestEqual(TEXT("vertex 1 is Y (OrderKey 20)"), SectionWorker->Positions[1].X, YVertexX);
				TestEqual(TEXT("vertex 2 is Z (OrderKey 30)"), SectionWorker->Positions[2].X, ZVertexX);
			}
		}
	}

	Importer->RemoveFromRoot();
	return true;
}

// A contribution whose SamplePath is absent from ProxyMeshDataCache must be dropped during resolve, on the game thread -- never handed to a worker as a null MeshData for the accumulate loop to branch on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyContributionResolveDropsCacheMissTest,
	"FragmentsUnreal.ShellBuild.ProxyContributions_ResolveDropsCacheMiss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyContributionResolveDropsCacheMissTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString PresentPath = TEXT("presentPath");
	Importer->StashProxyMeshDataForTests(PresentPath, MakeSingleVertexMeshData(1.0f));

	TArray<UFragmentsImporter::FProxyContribution> Sorted;
	Sorted.Add({ /*OrderKey=*/ 10, PresentPath,            FTransform::Identity, /*MatCRC=*/ 7, nullptr });
	Sorted.Add({ /*OrderKey=*/ 20, TEXT("missingPath"),    FTransform::Identity, /*MatCRC=*/ 7, nullptr });

	const TArray<UFragmentsImporter::FResolvedProxyContribution> Payload =
		Importer->ResolveProxyContributionsForTests(Sorted);

	TestEqual(TEXT("cache miss dropped at resolve"), Payload.Num(), 1);
	if (Payload.Num() == 1)
	{
		TestTrue(TEXT("surviving payload entry has valid mesh data"), Payload[0].MeshData.IsValid());
	}

	Importer->RemoveFromRoot();
	return true;
}

// Regression test for the cross-model purge bug: PurgeModelState used to call InFlightShellBuilds.Empty(), which is a single flat registry shared by every resident model (keyed by SamplePath, not ModelGuid).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShellBuildSelectivePurgeTest,
	"FragmentsUnreal.ShellBuild.PurgeIsSelectiveToOneModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShellBuildSelectivePurgeTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString ModelA = TEXT("ModelA");
	const FString ModelB = TEXT("ModelB");

	// Both models get a live wrapper so ProcessResolvedHISMItem (which resolves the item's own wrapper via GetFragmentModel(Item.ModelGuid), never CurrentHISMWrapper) can succeed for either.
	UFragmentModelWrapper* WrapperA = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	WrapperA->AddToRoot();
	Importer->GetFragmentModels_Mutable().Add(ModelA, WrapperA);

	UFragmentModelWrapper* WrapperB = NewObject<UFragmentModelWrapper>(GetTransientPackage());
	WrapperB->AddToRoot();
	Importer->GetFragmentModels_Mutable().Add(ModelB, WrapperB);

	// Purging ModelA removes its own waiters and keeps ModelB’s.
	const FString PathShared = TEXT("/Game/Buildings/Shared/rep_1.rep_1");
	const FString PathAOnly = TEXT("/Game/Buildings/ModelA/rep_2.rep_2");
	const FString PathBOnly = TEXT("/Game/Buildings/ModelB/rep_3.rep_3");

	TestTrue(TEXT("PathShared starts a build (ModelA waiter)"),
		Importer->RegisterShellBuildWaiter(PathShared, MakeItemForModel(1, ModelA)));
	TestFalse(TEXT("PathShared gains a second waiter (ModelB)"),
		Importer->RegisterShellBuildWaiter(PathShared, MakeItemForModel(2, ModelB)));
	TestTrue(TEXT("PathAOnly starts a build (ModelA waiter)"),
		Importer->RegisterShellBuildWaiter(PathAOnly, MakeItemForModel(3, ModelA)));
	TestTrue(TEXT("PathBOnly starts a build (ModelB waiter)"),
		Importer->RegisterShellBuildWaiter(PathBOnly, MakeItemForModel(4, ModelB)));

	const bool bStartedWithThreeEntries =
		TestEqual(TEXT("three registry entries before purge"), Importer->NumInFlightShellBuilds(), 3);
	if (!bStartedWithThreeEntries)
	{
		Importer->RemoveFromRoot();
		WrapperA->RemoveFromRoot();
		WrapperB->RemoveFromRoot();
		return false;
	}

	// Purge ModelA only.
	Importer->PurgeInFlightShellBuildsForModelForTests(ModelA);

	// PathAOnly's sole waiter was ModelA -> entry removed entirely.
	const bool bTwoEntriesRemain =
		TestEqual(TEXT("PathAOnly entry removed, PathShared+PathBOnly remain"),
			Importer->NumInFlightShellBuilds(), 2);
	if (!bTwoEntriesRemain)
	{
		Importer->RemoveFromRoot();
		WrapperA->RemoveFromRoot();
		WrapperB->RemoveFromRoot();
		return false;
	}

	// Completing the removed entry must be a clean no-op: no crash, no cache write, no change to the in-flight count (RemoveAndCopyValue finds nothing).
	{
		UStaticMesh* OrphanMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UFragmentsImporter::FShellBuildResult OrphanResult;
		OrphanResult.Mesh = OrphanMesh;
		Importer->CompleteShellBuild(PathAOnly, MoveTemp(OrphanResult));
	}
	TestEqual(TEXT("completing a purged entry is a no-op"), Importer->NumInFlightShellBuilds(), 2);
	TestNull(TEXT("purged entry never wrote its mesh to the cache"), Importer->FindCachedMesh(PathAOnly));

	// Complete PathShared: only ModelB's waiter should still be registered, so exactly one item reaches PendingBucketInstances from this entry.
	{
		UStaticMesh* SharedMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UFragmentsImporter::FShellBuildResult SharedResult;
		SharedResult.Mesh = SharedMesh;
		Importer->CompleteShellBuild(PathShared, MoveTemp(SharedResult));
	}
	TestEqual(TEXT("PathShared's surviving ModelB waiter reached the bucket table"),
		Importer->NumPendingBucketInstances(), 1);

	// Complete PathBOnly: fully untouched by the purge, so its ModelB waiter must still drain normally.
	{
		UStaticMesh* BOnlyMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UFragmentsImporter::FShellBuildResult BOnlyResult;
		BOnlyResult.Mesh = BOnlyMesh;
		Importer->CompleteShellBuild(PathBOnly, MoveTemp(BOnlyResult));
	}
	TestEqual(TEXT("PathBOnly's untouched ModelB waiter also reached the bucket table"),
		Importer->NumPendingBucketInstances(), 2);

	// Both surviving entries have now completed -- registry fully drained.
	TestEqual(TEXT("registry fully drained after both survivors complete"),
		Importer->NumInFlightShellBuilds(), 0);

	Importer->RemoveFromRoot();
	WrapperA->RemoveFromRoot();
	WrapperB->RemoveFromRoot();
	return true;
}

// Purging a model mid-flush must not let the worker's completion resurrect its geometry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyFlushPurgeMidFlightTest,
	"FragmentsUnreal.ShellBuild.ProxyFlush_PurgeMidFlightDiscardsResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyFlushPurgeMidFlightTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString ModelGuid = TEXT("TestModel_PurgedMidFlush");
	const FString SamplePath = TEXT("purgeMidFlightPath");
	Importer->StashProxyMeshDataForTests(SamplePath, MakeSingleVertexMeshData(1.0f));
	Importer->RecordProxyContribution(
		ModelGuid, /*OrderKey=*/ 10, SamplePath, FTransform::Identity, /*MatCRC=*/ 1, nullptr);

	// Non-empty payload -> takes the real async worker path, not the empty-payload inline finalize.
	Importer->LaunchProxyContributionFlushForTests(ModelGuid, /*BucketRoot=*/ nullptr);

	// Keep a worker handle before purge removes its registry entry.
	const TArray<UE::Tasks::FTask> FlushTasks = Importer->GetPendingProxyBuildTasksForTests(ModelGuid);
	TestEqual(TEXT("worker task registered for the non-empty flush"), FlushTasks.Num(), 1);

	// Purge now, before anything has pumped the game thread: guaranteed to bump the epoch strictly before the completion's epoch check can possibly run.
	Importer->PurgeModelStateForTests(ModelGuid, /*bRemoveFromMap=*/ false);

	// Wait on the saved worker handle after purging the registry entry.
	for (const UE::Tasks::FTask& Task : FlushTasks)
	{
		Task.Wait();
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);

	// Check the publish counter because both stale and completed builds remove the accumulator.
	TestNull(TEXT("stale completion must not leave a resurrected accumulator behind"),
		Importer->GetProxyAccumulatorForTests(ModelGuid));
	TestEqual(TEXT("stale completion must never publish into ProxyAccumulators"),
		Importer->ProxyFlushPublishCountForTests, 0);

	Importer->RemoveFromRoot();
	return true;
}

// The positive twin of ProxyFlush_PurgeMidFlightDiscardsResult above: with no purge in the way, a non-empty flush's worker completion must actually publish.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyFlushCurrentEpochPublishesTest,
	"FragmentsUnreal.ShellBuild.ProxyFlush_CurrentEpochPublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyFlushCurrentEpochPublishesTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString ModelGuid = TEXT("TestModel_CurrentEpoch");
	const FString SamplePath = TEXT("currentEpochPath");
	Importer->StashProxyMeshDataForTests(SamplePath, MakeSingleVertexMeshData(1.0f));
	Importer->RecordProxyContribution(
		ModelGuid, /*OrderKey=*/ 10, SamplePath, FTransform::Identity, /*MatCRC=*/ 1, nullptr);

	// Non-empty payload -> real async worker path, not the empty-payload inline finalize.
	Importer->LaunchProxyContributionFlushForTests(ModelGuid, /*BucketRoot=*/ nullptr);

	const TArray<UE::Tasks::FTask> FlushTasks = Importer->GetPendingProxyBuildTasksForTests(ModelGuid);
	TestEqual(TEXT("worker task registered for the non-empty flush"), FlushTasks.Num(), 1);

	// No purge here -- the epoch stays current, so the guard must let the result through.
	for (const UE::Tasks::FTask& Task : FlushTasks)
	{
		Task.Wait();
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);

	// The publish counter distinguishes a completed build from an unpublished one.
	TestEqual(TEXT("current-epoch completion publishes exactly once"),
		Importer->ProxyFlushPublishCountForTests, 1);

	Importer->RemoveFromRoot();
	return true;
}

// A resolved payload must survive mutation of ProxyMeshDataCache.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyFlushPayloadSurvivesCacheRehashTest,
	"FragmentsUnreal.ShellBuild.ProxyFlush_PayloadSurvivesCacheRehash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyFlushPayloadSurvivesCacheRehashTest::RunTest(const FString& Parameters)
{
	using namespace ShellBuildRegistryTestsPrivate;

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	constexpr float TargetVertexX = 42.0f;
	constexpr uint32 MatCRC = 5;
	const FString TargetPath = TEXT("rehashTargetPath");
	Importer->StashProxyMeshDataForTests(TargetPath, MakeSingleVertexMeshData(TargetVertexX));

	TArray<UFragmentsImporter::FProxyContribution> Sorted;
	Sorted.Add({ /*OrderKey=*/ 10, TargetPath, FTransform::Identity, MatCRC, nullptr });

	const TArray<UFragmentsImporter::FResolvedProxyContribution> Payload =
		Importer->ResolveProxyContributionsForTests(Sorted);
	TestEqual(TEXT("target contribution resolved"), Payload.Num(), 1);

	// Force rehash: enough inserts to grow the map well past its resolve-time capacity.
	for (int32 i = 0; i < 512; ++i)
	{
		Importer->StashProxyMeshDataForTests(
			FString::Printf(TEXT("rehashFiller_%d"), i),
			MakeSingleVertexMeshData(static_cast<float>(i)));
	}

	FProxyMeshAccumulator Accum;
	UFragmentsImporter::AccumulateResolvedContributions(Payload, Accum);

	const FProxyMeshSection* Section = Accum.Sections.Find(MatCRC);
	if (TestNotNull(TEXT("section exists after rehash"), Section))
	{
		if (TestEqual(TEXT("one vertex accumulated"), Section->Positions.Num(), 1))
		{
			TestEqual(TEXT("vertex data intact after cache rehash"),
				Section->Positions[0].X, TargetVertexX);
		}
	}

	Importer->RemoveFromRoot();
	return true;
}

// An empty proxy payload still reaches FinalizeProxyMesh and reports its result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyFlushEmptyPayloadCreatesNoAccumulatorTest,
	"FragmentsUnreal.ShellBuild.ProxyFlush_EmptyPayloadCreatesNoAccumulator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyFlushEmptyPayloadCreatesNoAccumulatorTest::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	const FString ModelGuid = TEXT("TestModel_NoContributions");

	// No contributions recorded, so resolve yields an empty payload.
	TArray<UFragmentsImporter::FProxyContribution> Empty;
	const TArray<UFragmentsImporter::FResolvedProxyContribution> Payload =
		Importer->ResolveProxyContributionsForTests(Empty);
	TestEqual(TEXT("empty input resolves to empty payload"), Payload.Num(), 0);

	// Launching with nothing buffered must not create an accumulator entry.
	Importer->LaunchProxyContributionFlushForTests(ModelGuid, /*BucketRoot=*/ nullptr);
	TestNull(TEXT("no accumulator created for an empty flush"),
		Importer->GetProxyAccumulatorForTests(ModelGuid));

	Importer->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
