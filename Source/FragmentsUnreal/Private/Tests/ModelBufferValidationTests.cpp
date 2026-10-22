// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Reject malformed FlatBuffers before accessing model fields.
#include "Misc/AutomationTest.h"
#include "Importer/FragmentModelWrapper.h"
#include "Importer/FragmentsImporter.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString SampleFragPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FragmentsUnreal"));
		return Plugin.IsValid()
			? FPaths::Combine(Plugin->GetContentDir(), TEXT("Resources"), TEXT("small_test.frag"))
			: FString();
	}

	TArray<uint8> TextBytes(const ANSICHAR* Text)
	{
		TArray<uint8> Out;
		Out.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentModelBufferValidationTest,
	"FragmentsUnreal.Importer.ModelBuffer.AcceptsOnlyASoundModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentModelBufferValidationTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Sample;
	FString Error;
	if (!TestTrue(TEXT("plugin sample .frag inflates (Content/Resources/small_test.frag)"),
		UFragmentsImporter::ReadAndInflate(SampleFragPath(), Sample, Error))) { return false; }

	// A real file must still pass -- a check stricter than the files we ship would reject every import.
	TestTrue(TEXT("the shipped sample is a sound model"), UFragmentModelWrapper::IsValidModelBuffer(Sample));

	TestFalse(TEXT("text is not a model"), UFragmentModelWrapper::IsValidModelBuffer(TextBytes("not a fragment")));
	TestFalse(TEXT("an empty buffer is not a model"), UFragmentModelWrapper::IsValidModelBuffer(TArray<uint8>()));

	TArray<uint8> Truncated(Sample.GetData(), Sample.Num() / 2);
	TestFalse(TEXT("a truncated download is not a model"), UFragmentModelWrapper::IsValidModelBuffer(Truncated));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentCorruptFileImportTest,
	"FragmentsUnreal.Importer.ModelBuffer.CorruptFileFailsTheImportInsteadOfCrashing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentCorruptFileImportTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Sample;
	FString Error;
	if (!TestTrue(TEXT("plugin sample .frag inflates"),
		UFragmentsImporter::ReadAndInflate(SampleFragPath(), Sample, Error))) { return false; }

	AddExpectedError(TEXT("is not a valid Fragments model"), EAutomationExpectedErrorFlags::Contains, 2);

	UFragmentsImporter* Importer = NewObject<UFragmentsImporter>(GetTransientPackage());
	Importer->AddToRoot();
	const FFragmentLoadOptions Options;

	// Before the fix, this first call crashed the editor in Model::guid().
	TestTrue(TEXT("a non-.frag file imports nothing"),
		Importer->LoadFragmentFromBuffer(TextBytes("not a fragment"), TEXT("C:/corrupt/not-a-fragment.frag"), Options).IsEmpty());

	TArray<uint8> Truncated(Sample.GetData(), Sample.Num() / 2);
	TestTrue(TEXT("a truncated download imports nothing"),
		Importer->LoadFragmentFromBuffer(MoveTemp(Truncated), TEXT("C:/corrupt/truncated.frag"), Options).IsEmpty());

	TestEqual(TEXT("no model is left behind"), Importer->GetFragmentModels().Num(), 0);
	Importer->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
