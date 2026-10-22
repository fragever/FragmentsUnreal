// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Tests panel actions, status, filtering, loading, and setting persistence.
#include "Misc/AutomationTest.h"
#include "Tabs/FragmentsPanelRules.h"
#include "Tabs/SFragmentsPanel.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelRowButtonTest,
	"FragmentsUnreal.Editor.Panel.RowButtonFollowsTheModelState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelRowButtonTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	TestTrue(TEXT("an import in progress is Importing, even for a loaded model (reimport)"),
		RowStateFor(true, true) == EModelRowState::Importing && RowStateFor(true, false) == EModelRowState::Importing);
	TestTrue(TEXT("loaded"), RowStateFor(false, true) == EModelRowState::Loaded);
	TestTrue(TEXT("not loaded"), RowStateFor(false, false) == EModelRowState::NotLoaded);

	auto Is = [](FPrimaryButton Button, EPrimaryAction Action, bool bEnabled)
	{
		return Button.Action == Action && Button.bEnabled == bEnabled;
	};
	TestTrue(TEXT("loaded: Edit"), Is(PrimaryButtonFor(EModelRowState::Loaded, true), EPrimaryAction::Edit, true));
	TestTrue(TEXT("loaded, file moved: still Edit"), Is(PrimaryButtonFor(EModelRowState::Loaded, false), EPrimaryAction::Edit, true));
	TestTrue(TEXT("not loaded: Load"), Is(PrimaryButtonFor(EModelRowState::NotLoaded, true), EPrimaryAction::Load, true));
	TestTrue(TEXT("not loaded, file moved: nothing to load"), Is(PrimaryButtonFor(EModelRowState::NotLoaded, false), EPrimaryAction::Load, false));
	TestTrue(TEXT("importing: the button cancels the import"),
		Is(PrimaryButtonFor(EModelRowState::Importing, true), EPrimaryAction::CancelImport, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelLoadAllTest,
	"FragmentsUnreal.Editor.Panel.LoadAllPicksOnlyLoadableModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelLoadAllTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;
	TestTrue(TEXT("a model not in the level, with its file, is loaded"), IsPickedByLoadAll(EModelRowState::NotLoaded, true));
	TestFalse(TEXT("a model whose file is gone is skipped"), IsPickedByLoadAll(EModelRowState::NotLoaded, false));
	TestFalse(TEXT("a loaded model is skipped"), IsPickedByLoadAll(EModelRowState::Loaded, true));
	TestFalse(TEXT("a model already importing is skipped"), IsPickedByLoadAll(EModelRowState::Importing, true));

	TestEqual(TEXT("the batch loading dialog counts from one and names the model"),
		MultiLoadProgressText(2, 47, TEXT("C:/Models/Building_A.frag")).ToString(),
		FString(TEXT("Loading 3 of 47: Building_A")));
	return true;
}

// Deleting a model in the viewport, or changing maps, never passes through the panel; the panel hears the unload and must leave a details page showing that model -- and only that model.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelDetailsPageTest,
	"FragmentsUnreal.Editor.Panel.DetailsPageLeavesOnlyForItsOwnModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelDetailsPageTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;
	TestTrue(TEXT("the page leaves when the model it shows unloads"), ShouldLeaveDetailsPage(true, TEXT("A"), TEXT("A")));
	TestFalse(TEXT("another model unloading leaves the page alone"), ShouldLeaveDetailsPage(true, TEXT("A"), TEXT("B")));
	TestFalse(TEXT("the list page has no model to leave"), ShouldLeaveDetailsPage(false, TEXT(""), TEXT("")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelStatusDotTest,
	"FragmentsUnreal.Editor.Panel.StatusDotBlinksYellowWhileImporting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelStatusDotTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	const FLinearColor Importing = StatusDotColor(EModelRowState::Importing);
	const FLinearColor Loaded = StatusDotColor(EModelRowState::Loaded);
	const FLinearColor NotLoaded = StatusDotColor(EModelRowState::NotLoaded);
	TestTrue(TEXT("importing is yellow: red and green high, blue low"), Importing.R > 0.5f && Importing.G > 0.3f && Importing.B < 0.2f);
	TestTrue(TEXT("loaded is green"), Loaded.G > Loaded.R && Loaded.G > Loaded.B);
	TestTrue(TEXT("not loaded is grey"), FMath::IsNearlyEqual(NotLoaded.R, NotLoaded.G, 0.05f) && FMath::IsNearlyEqual(NotLoaded.G, NotLoaded.B, 0.05f));

	float Lowest = 1.f, Highest = 0.f;
	for (int32 Step = 0; Step < 20; ++Step)
	{
		const float Opacity = StatusDotOpacity(EModelRowState::Importing, Step * 0.05);
		Lowest = FMath::Min(Lowest, Opacity);
		Highest = FMath::Max(Highest, Opacity);
		TestEqual(TEXT("loaded is steady"), StatusDotOpacity(EModelRowState::Loaded, Step * 0.05), 1.f);
		TestEqual(TEXT("not loaded is steady"), StatusDotOpacity(EModelRowState::NotLoaded, Step * 0.05), 1.f);
	}
	TestTrue(TEXT("importing blinks over a second"), Highest - Lowest > 0.4f);
	TestTrue(TEXT("and never vanishes"), Lowest >= 0.2f && Highest <= 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelListedModelsTest,
	"FragmentsUnreal.Editor.Panel.FirstImportShowsItsRowAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelListedModelsTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	auto Make = [](const TCHAR* Guid) { FFragmentsCachedModel Model; Model.Guid = Guid; return Model; };
	const TArray<FFragmentsCachedModel> Cached = { Make(TEXT("A")), Make(TEXT("B")) };
	const TArray<FFragmentsCachedModel> Importing = { Make(TEXT("B")), Make(TEXT("C")) };

	const TArray<FFragmentsCachedModel> Listed = ListedModels(Cached, Importing);
	TestEqual(TEXT("three rows: a reimport is not listed twice"), Listed.Num(), 3);
	TestTrue(TEXT("cached models first, then the new import"),
		Listed.Num() == 3 && Listed[0].Guid == TEXT("A") && Listed[1].Guid == TEXT("B") && Listed[2].Guid == TEXT("C"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelLoadedFirstTest,
	"FragmentsUnreal.Editor.Panel.LoadedModelsListFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelLoadedFirstTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	// A long list of buildings with two in the level: those two must be reachable without scrolling.
	auto Make = [](const TCHAR* Guid) { FFragmentsCachedModel Model; Model.Guid = Guid; return Model; };
	TArray<FFragmentsCachedModel> Models = { Make(TEXT("A")), Make(TEXT("B")), Make(TEXT("C")), Make(TEXT("D")), Make(TEXT("E")) };
	const TMap<FString, EModelRowState> States = {
		{ TEXT("A"), EModelRowState::NotLoaded }, { TEXT("B"), EModelRowState::Loaded },
		{ TEXT("C"), EModelRowState::Importing }, { TEXT("D"), EModelRowState::Loaded },
		{ TEXT("E"), EModelRowState::NotLoaded } };

	OrderLoadedFirst(Models, [&States](const FString& Guid) { return States.FindChecked(Guid); });

	FString Order;
	for (const FFragmentsCachedModel& Model : Models) { Order += Model.Guid; }
	TestEqual(TEXT("loaded first, then importing, then the rest, each in list order"), Order, FString(TEXT("BDCAE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelFilterCountTest,
	"FragmentsUnreal.Editor.Panel.FilterCountCountsEachToggle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelFilterCountTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	auto MakeRow = [](bool bHighlight, bool bIsolate)
	{
		TSharedPtr<FFragmentsMaterialReplacementRow> Row = MakeShared<FFragmentsMaterialReplacementRow>();
		Row->bIsHighlighted = bHighlight;
		Row->bIsIsolated = bIsolate;
		return Row;
	};

	TArray<TSharedPtr<FFragmentsMaterialReplacementRow>> Rows;
	TestEqual(TEXT("no rows, no filters"), CountActiveFilters(Rows), 0);

	Rows.Add(MakeRow(false, false));
	Rows.Add(MakeRow(true, false));
	TestEqual(TEXT("one highlight"), CountActiveFilters(Rows), 1);

	Rows.Add(MakeRow(false, true));
	Rows.Add(MakeRow(true, true));
	Rows.Add(nullptr);
	TestEqual(TEXT("two highlights and two isolates; an invalid row counts nothing"), CountActiveFilters(Rows), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelOriginConfigTest,
	"FragmentsUnreal.Editor.Panel.CoordinateSettingRoundTrips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelOriginConfigTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	// The stored strings are the enum's own names, so a config written before this used StaticEnum still reads back.
	TestEqual(TEXT("stored name for world origin"), OriginModeToConfig(EFragmentOriginMode::UseWorldOrigin), FString(TEXT("UseWorldOrigin")));
	TestEqual(TEXT("stored name for base coordinates"), OriginModeToConfig(EFragmentOriginMode::UseBaseCoordinates), FString(TEXT("UseBaseCoordinates")));

	for (const EFragmentOriginMode Mode : { EFragmentOriginMode::UseWorldOrigin, EFragmentOriginMode::UseBaseCoordinates })
	{
		EFragmentOriginMode Read = Mode == EFragmentOriginMode::UseWorldOrigin
			? EFragmentOriginMode::UseBaseCoordinates : EFragmentOriginMode::UseWorldOrigin;
		TestTrue(TEXT("a stored mode reads back"), OriginModeFromConfig(OriginModeToConfig(Mode), Read));
		TestEqual(TEXT("as the same mode"), Read, Mode);
	}

	EFragmentOriginMode Untouched = EFragmentOriginMode::UseBaseCoordinates;
	TestFalse(TEXT("an unknown value is rejected"), OriginModeFromConfig(TEXT("SomethingElse"), Untouched));
	TestEqual(TEXT("and leaves the setting alone"), Untouched, EFragmentOriginMode::UseBaseCoordinates);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelFooterLinkTest,
	"FragmentsUnreal.Editor.Panel.FooterLinksShowOnlyForWebAddresses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelFooterLinkTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;
	TestTrue(TEXT("https link shows"), IsWebLink(TEXT("https://fragmentsunreal.dev/docs")));
	TestTrue(TEXT("http link shows"), IsWebLink(TEXT("http://example.com")));
	TestTrue(TEXT("scheme case does not matter"), IsWebLink(TEXT("HTTPS://fragmentsunreal.dev")));
	TestFalse(TEXT("an empty field hides the button"), IsWebLink(FString()));
	TestFalse(TEXT("whitespace hides it"), IsWebLink(TEXT("   ")));
	TestFalse(TEXT("a bare scheme hides it"), IsWebLink(TEXT("https://")));
	TestFalse(TEXT("a non-web address hides it"), IsWebLink(TEXT("file:///C:/secret.txt")));
	TestFalse(TEXT("a host without a scheme hides it"), IsWebLink(TEXT("fragmentsunreal.dev/docs")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentPanelLoadingSkeletonTest,
	"FragmentsUnreal.Editor.Panel.OverrideListShowsASkeletonWhileDetailLoads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentPanelLoadingSkeletonTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::PanelRules;

	TestTrue(TEXT("the open model's detail is building for the edit"), IsMaterialListLoading(true, true, true));
	TestFalse(TEXT("built: the rows"), IsMaterialListLoading(true, true, false));
	TestFalse(TEXT("a build for a model the page is not editing"), IsMaterialListLoading(true, false, true));
	TestFalse(TEXT("no page open"), IsMaterialListLoading(false, false, true));

	TestEqual(TEXT("caption counts cells"), MaterialListLoadingCaption(37, 141).ToString(),
		FString(TEXT("Loading full-detail geometry for editing... 37 of 141 cells")));
	TestEqual(TEXT("caption before the count is known"), MaterialListLoadingCaption(0, 0).ToString(),
		FString(TEXT("Loading full-detail geometry for editing...")));

	float Lo = 1.f, Hi = 0.f;
	for (int32 Step = 0; Step < 200; ++Step)
	{
		const float O = SkeletonPulseOpacity(Step * 0.01);
		Lo = FMath::Min(Lo, O);
		Hi = FMath::Max(Hi, O);
	}
	TestTrue(TEXT("pulses between half and full"), FMath::IsNearlyEqual(Lo, 0.5f, 0.02f) && FMath::IsNearlyEqual(Hi, 1.f, 0.02f));
	TestTrue(TEXT("a two-second cycle"), FMath::IsNearlyEqual(SkeletonPulseOpacity(0.3), SkeletonPulseOpacity(2.3), 0.001f));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
