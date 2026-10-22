// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Importer/FragmentsImporter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace FragmentLoadAsyncTestsPrivate
{
	/** Writes Bytes to a fixed-per-Suffix temp file (FragLoadTest_<Suffix>.bin, NOT uniquely named -- callers with the same Suffix overwrite each other's file) and returns its path. */
	static FString WriteTempFile(const FString& Suffix, const TArray<uint8>& Bytes)
	{
		const FString Path = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			FString::Printf(TEXT("FragLoadTest_%s.bin"), *Suffix));
		if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
		{
			return FString();
		}
		return Path;
	}

	static void DeleteTempFile(const FString& Path)
	{
		FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path);
	}

	/** The plugin ships a small real .frag at Content/Resources/small_test.frag. */
	static FString SampleFragPath()
	{
		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(TEXT("FragmentsUnreal"));
		return Plugin.IsValid()
			? FPaths::Combine(Plugin->GetContentDir(), TEXT("Resources"), TEXT("small_test.frag"))
			: FString();
	}

	static TArray<uint8> MakeBytes(const ANSICHAR* Text)
	{
		TArray<uint8> Out;
		for (const ANSICHAR* P = Text; *P; ++P) { Out.Add(static_cast<uint8>(*P)); }
		return Out;
	}
}

// ReadAndInflate is the half that moves to a worker.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadAndInflateUncompressedPassthroughTest,
	"FragmentsUnreal.LoadAsync.ReadAndInflate_UncompressedPassthrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReadAndInflateUncompressedPassthroughTest::RunTest(const FString& Parameters)
{
	using namespace FragmentLoadAsyncTestsPrivate;

	const TArray<uint8> Raw = MakeBytes("not compressed at all");
	const FString Path = WriteTempFile(TEXT("plain"), Raw);
	if (!TestTrue(TEXT("temp file setup succeeded"), !Path.IsEmpty()))
	{
		return false;
	}

	TArray<uint8> Out;
	FString Error;
	const bool bOk = UFragmentsImporter::ReadAndInflate(Path, Out, Error);

	TestTrue(TEXT("uncompressed file reads successfully"), bOk);
	TestEqual(TEXT("error is empty on success"), Error, FString());
	TestEqual(TEXT("bytes pass through unchanged"), Out.Num(), Raw.Num());
	if (Out.Num() == Raw.Num() && Raw.Num() > 0)
	{
		TestEqual(TEXT("first byte matches"), Out[0], Raw[0]);
		TestEqual(TEXT("last byte matches"), Out[Raw.Num() - 1], Raw[Raw.Num() - 1]);
	}

	DeleteTempFile(Path);
	return true;
}

// Missing files return an error through the async completion delegate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadAndInflateMissingFileTest,
	"FragmentsUnreal.LoadAsync.ReadAndInflate_MissingFileFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReadAndInflateMissingFileTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Out;
	FString Error;
	const bool bOk = UFragmentsImporter::ReadAndInflate(
		TEXT("C:/definitely/not/a/real/path/nope.frag"), Out, Error);

	TestFalse(TEXT("missing file fails"), bOk);
	TestTrue(TEXT("error message is populated"), !Error.IsEmpty());
	TestEqual(TEXT("no bytes produced"), Out.Num(), 0);
	return true;
}

// REGRESSION GUARD: a file whose first byte is 0x78 looks like a zlib header, so the existing code attempts inflate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadAndInflateFalseZlibHeaderTest,
	"FragmentsUnreal.LoadAsync.ReadAndInflate_FalseZlibHeaderFallsBackToRaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReadAndInflateFalseZlibHeaderTest::RunTest(const FString& Parameters)
{
	using namespace FragmentLoadAsyncTestsPrivate;

	// 0x78 first byte, but the remainder is not a valid zlib stream.
	TArray<uint8> Raw;
	Raw.Add(0x78);
	Raw.Append(MakeBytes("this is not really zlib data, just plain bytes"));
	const FString Path = WriteTempFile(TEXT("falsezlib"), Raw);
	if (!TestTrue(TEXT("temp file setup succeeded"), !Path.IsEmpty()))
	{
		return false;
	}

	TArray<uint8> Out;
	FString Error;
	const bool bOk = UFragmentsImporter::ReadAndInflate(Path, Out, Error);

	TestTrue(TEXT("false zlib header still succeeds via raw fallback"), bOk);
	TestEqual(TEXT("raw bytes returned unchanged"), Out.Num(), Raw.Num());
	if (Out.Num() == Raw.Num())
	{
		TestEqual(TEXT("first byte is still 0x78"), Out[0], static_cast<uint8>(0x78));
	}

	DeleteTempFile(Path);
	return true;
}

// The composed load path preserves the original ModelGuid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadFragmentComposedEqualsMonolithicTest,
	"FragmentsUnreal.LoadAsync.ComposedPathMatchesSync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoadFragmentComposedEqualsMonolithicTest::RunTest(const FString& Parameters)
{
	// Ships with the plugin.
	const FString SamplePath = FragmentLoadAsyncTestsPrivate::SampleFragPath();
	if (!TestTrue(TEXT("plugin sample .frag exists (Content/Resources/small_test.frag)"),
		!SamplePath.IsEmpty()
		&& FPlatformFileManager::Get().GetPlatformFile().FileExists(*SamplePath)))
	{
		return false;
	}

	UFragmentsImporter* ImporterA = NewObject<UFragmentsImporter>(GetTransientPackage());
	ImporterA->AddToRoot();
	UFragmentsImporter* ImporterB = NewObject<UFragmentsImporter>(GetTransientPackage());
	ImporterB->AddToRoot();

	const FFragmentLoadOptions Options;

	// Path 1: the public synchronous entry point.
	const FString GuidSync = ImporterA->LoadFragment(SamplePath, Options);

	// Path 2: the two halves composed by hand, as LoadFragmentAsync does across threads.
	TArray<uint8> Buffer;
	FString Error;
	const bool bOk = UFragmentsImporter::ReadAndInflate(SamplePath, Buffer, Error);
	TestTrue(TEXT("ReadAndInflate succeeds on the sample"), bOk);
	const FString GuidComposed = bOk
		? ImporterB->LoadFragmentFromBuffer(MoveTemp(Buffer), SamplePath, Options)
		: FString();

	TestTrue(TEXT("sync path produced a guid"), !GuidSync.IsEmpty());
	TestEqual(TEXT("composed path produces the identical ModelGuid"), GuidComposed, GuidSync);

	ImporterA->RemoveFromRoot();
	ImporterB->RemoveFromRoot();
	return true;
}

// A failed async load must REPORT.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadFragmentAsyncMissingFileFiresDelegateTest,
	"FragmentsUnreal.LoadAsync.AsyncMissingFileFiresFailureDelegate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoadFragmentAsyncMissingFileFiresDelegateTest::RunTest(const FString& Parameters)
{
	// Whitelist the expected error log from a missing-file test.
	AddExpectedErrorPlain(TEXT("Failed to load file"), EAutomationExpectedErrorFlags::Contains, 1);

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	TestTrue(TEXT("async load of a missing file was attempted"),
		Importer->LoadFragmentAsyncBlockingForTests(
			TEXT("C:/definitely/not/a/real/path/nope.frag")));

	TestFalse(TEXT("delegate reported failure"), Importer->LastAsyncSuccessForTests());
	TestTrue(TEXT("delegate carried an error message"),
		!Importer->LastAsyncErrorForTests().IsEmpty());
	TestTrue(TEXT("no guid on failure"), Importer->LastAsyncGuidForTests().IsEmpty());

	Importer->RemoveFromRoot();
	return true;
}

// The happy path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadFragmentAsyncSucceedsTest,
	"FragmentsUnreal.LoadAsync.AsyncLoadSucceedsAndReportsGuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoadFragmentAsyncSucceedsTest::RunTest(const FString& Parameters)
{
	// See the equivalence test: fail, do not skip.
	const FString SamplePath = FragmentLoadAsyncTestsPrivate::SampleFragPath();
	if (!TestTrue(TEXT("plugin sample .frag exists (Content/Resources/small_test.frag)"),
		!SamplePath.IsEmpty()
		&& FPlatformFileManager::Get().GetPlatformFile().FileExists(*SamplePath)))
	{
		return false;
	}

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	TestTrue(TEXT("completion fired"), Importer->LoadFragmentAsyncBlockingForTests(SamplePath));
	TestTrue(TEXT("async load reported success"), Importer->LastAsyncSuccessForTests());
	TestTrue(TEXT("no error on success"), Importer->LastAsyncErrorForTests().IsEmpty());
	TestTrue(TEXT("a ModelGuid was returned"), !Importer->LastAsyncGuidForTests().IsEmpty());

	// The in-flight flag must be cleared by the completion, or every later load is rejected by the guard.
	TestTrue(TEXT("a second load is accepted after the first completes"),
		Importer->LoadFragmentAsyncBlockingForTests(SamplePath));
	TestTrue(TEXT("second load actually succeeded, not rejected"),
		Importer->LastAsyncSuccessForTests());

	Importer->RemoveFromRoot();
	return true;
}

// The in-flight guard makes the deferred-concurrency decision explicit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadFragmentAsyncInFlightGuardTest,
	"FragmentsUnreal.LoadAsync.SecondConcurrentCallIsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoadFragmentAsyncInFlightGuardTest::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	Importer->SetAsyncLoadInFlightForTests(true);

	// Keep the completion state alive if the callback runs after this test scope.
	TSharedRef<bool> bCallbackFired = MakeShared<bool>(false);
	TSharedRef<bool> bReportedSuccess = MakeShared<bool>(true);
	TSharedRef<FString> ReportedError = MakeShared<FString>();

	Importer->LoadFragmentAsyncWithHandlerForTests(
		TEXT("C:/some/path.frag"),
		[bCallbackFired, bReportedSuccess, ReportedError](bool bSuccess, const FString& Error, const FString& Guid)
		{
			*bCallbackFired = true;
			*bReportedSuccess = bSuccess;
			*ReportedError = Error;
		});

	TestTrue(TEXT("rejection fires the delegate rather than returning silently"), *bCallbackFired);
	TestFalse(TEXT("rejection reports failure"), *bReportedSuccess);
	TestTrue(TEXT("rejection explains why"), !ReportedError->IsEmpty());

	Importer->SetAsyncLoadInFlightForTests(false);
	Importer->RemoveFromRoot();
	return true;
}

// Teardown waits for async load completion before releasing the importer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWaitForAllProxyBuildsDrainsAsyncLoadTest,
	"FragmentsUnreal.LoadAsync.TeardownDrainRunsCompletionEvenWithNoProxyBuilds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWaitForAllProxyBuildsDrainsAsyncLoadTest::RunTest(const FString& Parameters)
{
	const FString SamplePath = FragmentLoadAsyncTestsPrivate::SampleFragPath();
	if (!TestTrue(TEXT("plugin sample .frag exists (Content/Resources/small_test.frag)"),
		!SamplePath.IsEmpty()
		&& FPlatformFileManager::Get().GetPlatformFile().FileExists(*SamplePath)))
	{
		return false;
	}

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	// Shared (not stack-captured): if the drain regresses, this handler stays queued past RunTest's return and a later pump -- from an unrelated test -- would run it.
	TSharedRef<bool> bFired = MakeShared<bool>(false);
	Importer->LoadFragmentAsyncWithHandlerForTests(SamplePath,
		[bFired](bool, const FString&, const FString&)
		{
			*bFired = true;
		});

	// Deliberately do NOT pump here ourselves -- WaitForAllProxyBuilds must be sufficient on its own, exactly as it needs to be when Deinitialize calls it with no other pump forthcoming afterward.
	Importer->WaitForAllProxyBuilds();

	TestTrue(TEXT("the async completion ran during the teardown drain, not after it"), *bFired);

	Importer->RemoveFromRoot();
	return true;
}

// The synchronous entry point is still Blueprint-callable (deprecated, not removed), so it can now be reached WHILE an async load is in flight -- a path that did not exist while it was C++ only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadFragmentSyncRejectedWhileAsyncInFlightTest,
	"FragmentsUnreal.LoadAsync.SyncLoadRejectedWhileAsyncInFlight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoadFragmentSyncRejectedWhileAsyncInFlightTest::RunTest(const FString& Parameters)
{
	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();

	Importer->SetAsyncLoadInFlightForTests(true);

	const FString Guid = Importer->LoadFragment(
		TEXT("C:/definitely/not/a/real/path/nope.frag"), FFragmentLoadOptions());

	TestTrue(TEXT("sync load rejected while an async load is in flight"), Guid.IsEmpty());
	TestEqual(TEXT("the rejected load registered no model"),
		Importer->GetFragmentModels().Num(), 0);

	// Clearing the flag must let it through again -- otherwise this would pass even if the guard were stuck on, which would break the editor panel and PIE refresh.
	AddExpectedErrorPlain(TEXT("Failed to load file"), EAutomationExpectedErrorFlags::Contains, 1);
	Importer->SetAsyncLoadInFlightForTests(false);
	TestTrue(TEXT("sync load reaches the file read once no async load is in flight"),
		Importer->LoadFragment(
			TEXT("C:/definitely/not/a/real/path/nope.frag"), FFragmentLoadOptions()).IsEmpty());

	Importer->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
