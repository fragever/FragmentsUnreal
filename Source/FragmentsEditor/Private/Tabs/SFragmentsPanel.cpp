// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Tabs/SFragmentsPanel.h"

#include "Importer/FragmentOverrides.h"
#include "AssetRegistry/AssetData.h"
#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "EditorSubsystems/FragmentModelPresence.h"
#include "Interfaces/IPluginManager.h"
#include "Tabs/FragmentsPanelRules.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SOverlay.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "EngineUtils.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDesktopPlatform.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Overrides/FragmentMaterialDrop.h"
#include "Overrides/FragmentOverrideState.h"
#include "Overrides/FragmentPlacementMath.h"
#include "PropertyCustomizationHelpers.h"
#include "SAssetDropTarget.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Style/FragmentsStyle.h"
#include "UObject/SoftObjectPath.h"
#include "Utils/FragmentsLog.h"
#include "IContentBrowserSingleton.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SRotatorInputBox.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	UFragmentsImporterEditorSubsystem* ImporterSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;
	}
}

#define LOCTEXT_NAMESPACE "SFragmentsPanel"

void SFragmentsPanel::Construct(const FArguments& InArgs)
{
	OwnerTab = InArgs._OwnerTab;

	OriginModeOptions.Reset();
	OriginModeOptions.Add(MakeShared<EFragmentOriginMode>(EFragmentOriginMode::UseWorldOrigin));
	OriginModeOptions.Add(MakeShared<EFragmentOriginMode>(EFragmentOriginMode::UseBaseCoordinates));
	CurrentOriginMode = OriginModeOptions[0];

	// The coordinate setting is remembered per project; it used to reset every time the panel opened.
	FString StoredOriginMode;
	EFragmentOriginMode StoredMode;
	if (GConfig->GetString(TEXT("FragmentsEditor"), TEXT("ImportOriginMode"), StoredOriginMode, GEditorPerProjectIni)
		&& UE::Fragments::PanelRules::OriginModeFromConfig(StoredOriginMode, StoredMode))
	{
		for (const TSharedPtr<EFragmentOriginMode>& Option : OriginModeOptions)
		{
			if (*Option == StoredMode) { CurrentOriginMode = Option; }
		}
	}

	// The footer's name, version and links all come from FragmentsUnreal.uplugin, the same place the Plugins window reads them from.
	FText FooterText = LOCTEXT("FooterName", "FragmentsUnreal");
	FString DocsUrl, SupportUrl;
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FragmentsUnreal")))
	{
		const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
		FooterText = FText::Format(LOCTEXT("FooterNameVersion", "FragmentsUnreal {0}"), FText::FromString(Descriptor.VersionName));
		DocsUrl = Descriptor.DocsURL.TrimStartAndEnd();
		SupportUrl = Descriptor.SupportURL.TrimStartAndEnd();
	}
	auto FooterLink = [](const FString& Url, const FName Icon, const FText& ToolTip) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.Padding(FMargin(6.f, 0.f, 0.f, 0.f))
			.Visibility(UE::Fragments::PanelRules::IsWebLink(Url) ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(SButton)
				.ButtonStyle(&FFragmentsStyle::Get().GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
				.ContentPadding(FMargin(3.f))
				.ToolTipText(FText::Format(LOCTEXT("FooterLinkTip", "{0}\n{1}"), ToolTip, FText::FromString(Url)))
				.OnClicked_Lambda([Url]()
				{
					FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.WidthOverride(14.f)
					.HeightOverride(14.f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(Icon))
					]
				]
			];
	};

	const ISlateStyle& Style = FFragmentsStyle::Get();
	MaterialThumbnailPool = MakeShared<FAssetThumbnailPool>(64);
	if (GEditor)
	{
		if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
		{
			ProcessLoadedDelegateHandle = Subsystem->OnProcessLoadedFragmentComplete().AddSP(
				this,
				&SFragmentsPanel::HandleProcessLoadedFragmentComplete);
			ModelUnloadedDelegateHandle = Subsystem->OnModelUnloaded().AddSP(
				this,
				&SFragmentsPanel::HandleModelUnloaded);
			ModelImportStartedDelegateHandle = Subsystem->OnModelImportStarted().AddSP(
					this,
					&SFragmentsPanel::HandleModelImportStarted);
		}

		SelectionChangedDelegateHandle = USelection::SelectionChangedEvent.AddSP(
			this,
			&SFragmentsPanel::HandleEditorSelectionChanged);
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(Style.GetBrush("Fragments.Background.Primary"))
		.Padding(FMargin(12.f, 8.f))
		[
			SAssignNew(PanelSwitcher, SWidgetSwitcher)
			.WidgetIndex(0)

			+ SWidgetSwitcher::Slot()
			[
				SNew(SVerticalBox)

				// Import: the page's one primary action, settings behind the gear beside it.
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 12.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.HeightOverride(28.f)
						[
							SNew(SButton)
							.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button"))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.ContentPadding(FMargin(8.f, 0.f))
							.ToolTipText(LOCTEXT("ImportModelsTooltip", "Choose one or more .frag files to import"))
							.OnClicked(this, &SFragmentsPanel::OnImportFragmentModelsClicked)
							[
								SNew(STextBlock)
								.TextStyle(Style, "Fragments.Text.Button")
								.Text(LOCTEXT("ImportModelsButton", "Import Fragment Models..."))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.f, 0.f, 0.f, 0.f)
					[
						SNew(SBox)
						.WidthOverride(28.f)
						.HeightOverride(28.f)
						[
							SNew(SComboButton)
							.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
							.HasDownArrow(false)
							.ContentPadding(FMargin(0.f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.ToolTipText(LOCTEXT("ImportSettingsTooltip", "Import settings"))
							.OnGetMenuContent(this, &SFragmentsPanel::MakeImportSettingsMenu)
							.ButtonContent()
							[
								SNew(SBox)
								.WidthOverride(16.f)
								.HeightOverride(16.f)
								[
									SNew(SImage)
									.Image(FAppStyle::GetBrush("Icons.Settings"))
								]
							]
						]
					]
				]

				// Saved Models: a subtitle, then search across the width with Load all beside it
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock)
					.TextStyle(Style, "Fragments.Text.Body")
					.Text(LOCTEXT("SavedModelsTitle", "Saved Models"))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						SAssignNew(CachedFragmentsSearchBox, SEditableTextBox)
						.Style(&Style.GetWidgetStyle<FEditableTextBoxStyle>("Fragments.Input"))
						.HintText(LOCTEXT("CachedFragmentsSearchHint", "Search..."))
						.OnTextChanged(this, &SFragmentsPanel::OnCachedFragmentsSearchChanged)
					]

					// Load all, or -- while anything imports -- Cancel imports.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
						.ContentPadding(FMargin(4.f))
						.IsEnabled_Lambda([this]() { return GetImportingCount() > 0 || PathsForLoadAll().Num() > 0; })
						.ToolTipText_Lambda([this]()
						{
							const int32 Importing = GetImportingCount();
							return Importing > 0
								? FText::Format(LOCTEXT("CancelImportsTooltip",
									"Cancel imports ({0}): stop everything still importing; finished models stay"), FText::AsNumber(Importing))
								: FText::Format(LOCTEXT("LoadAllTooltip",
									"Load all: load every model in this list that is not in the level ({0})"), FText::AsNumber(PathsForLoadAll().Num()));
						})
						.OnClicked(this, &SFragmentsPanel::OnLoadAllOrCancelImportsClicked)
						[
							SNew(SBox)
							.WidthOverride(16.f)
							.HeightOverride(16.f)
							[
								SNew(SImage)
								.Image_Lambda([this, &Style]()
								{
									return Style.GetBrush(GetImportingCount() > 0 ? "Fragments.Icon.Close" : "Fragments.Icon.Reimport");
								})
							]
						]
					]

					// Clear the level, or the list, without a click per model.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.f, 0.f, 0.f, 0.f)
					[
						SNew(SComboButton)
						.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
						.HasDownArrow(false)
						.ContentPadding(FMargin(9.f, 4.f))
						.ToolTipText(LOCTEXT("ListMoreTip", "Clear the level or the list"))
						.OnGetMenuContent(this, &SFragmentsPanel::MakeListMenu)
						.ButtonContent()
						[
							SNew(SBox)
							.WidthOverride(4.f)
							.HeightOverride(16.f)
							[
								SNew(SImage)
								.Image(Style.GetBrush("Fragments.Icon.More"))
							]
						]
					]
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.f)
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SBorder)
					.BorderImage(Style.GetBrush("Fragments.Background.Secondary"))
					.Padding(FMargin(6.f))
					[
						SAssignNew(CachedFragmentsScrollBox, SScrollBox)
					]
				]

				// Footer: logo, name, version
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.f, 0.f, 6.f, 0.f)
					[
						SNew(SBox)
						.WidthOverride(20.f)
						.HeightOverride(20.f)
						[
							SNew(SImage)
							.Image(Style.GetBrush("Fragments.Icon.Logo"))
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.TextStyle(Style, "Fragments.Text.Small")
						.Text(FooterText)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.f, 0.f, 0.f, 0.f)
					[
						FooterLink(DocsUrl, "Icons.Documentation", LOCTEXT("DocsLinkTip", "Documentation"))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						FooterLink(SupportUrl, "Icons.Heart", LOCTEXT("SupportLinkTip", "Support FragmentsUnreal -- it is open source and runs on donations"))
					]
				]
			]

			+ SWidgetSwitcher::Slot()
			[
				// Details page.
				SNew(SVerticalBox)

				// Title: back button, model name, GUID
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.f, 0.f, 10.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
						.ContentPadding(FMargin(0.f))
						.OnClicked(this, &SFragmentsPanel::OnBackClicked)
						[
							SNew(SBox)
							.WidthOverride(32.f)
							.HeightOverride(32.f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.TextStyle(Style, "Fragments.Text.Button")
								.Text(LOCTEXT("DetailsBackButton", "<"))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							// The model's own name, or its alias when one is set -- the same name the model list shows.
							SAssignNew(DetailNameText, STextBlock)
							.TextStyle(Style, "Fragments.Text.Heading")
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							.Text_Lambda([this]()
							{
								if (!bHasActiveCachedModel) return FText::GetEmpty();
								return FText::FromString(ActiveCachedModel.Alias.IsEmpty()
									? FPaths::GetBaseFilename(ActiveCachedModel.Path)
									: ActiveCachedModel.Alias);
							})
							.ToolTipText_Lambda([this]()
							{
								return bHasActiveCachedModel
									? FText::FromString(ActiveCachedModel.Path)
									: FText::GetEmpty();
							})
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.f, 2.f, 0.f, 0.f)
						[
							SAssignNew(DetailGuidText, STextBlock)
							.TextStyle(Style, "Fragments.Text.Small")
							.ColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.55f))
						]
					]
				]

				// Alias
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 6.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.f, 0.f, 10.f, 0.f)
					[
						SNew(STextBlock)
						.TextStyle(Style, "Fragments.Text.Small")
						.Text(LOCTEXT("DetailsAliasLabel", "Alias"))
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						SAssignNew(DetailAliasTextBox, SEditableTextBox)
						.Style(&Style.GetWidgetStyle<FEditableTextBoxStyle>("Fragments.Input"))
						.OnTextCommitted(this, &SFragmentsPanel::OnAliasTextCommitted)
					]
				]

				// Placement: its own section, title line with Save / Revert, then the fields
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SBorder)
					.BorderImage(Style.GetBrush("Fragments.Background.Secondary"))
					.Padding(FMargin(8.f))
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.FillWidth(1.f)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.TextStyle(Style, "Fragments.Text.Body")
								.Text(LOCTEXT("PlacementSectionTitle", "Placement"))
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.f, 0.f, 6.f, 0.f)
							[
								// Only lit when the model has moved since the last save, whether by gizmo, typed value, drag or paste -- none of those write the file themselves.
								SNew(SButton)
								.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
								.ContentPadding(FMargin(4.f))
								.IsEnabled_Lambda([this]() { return IsPlacementButtonEnabled() && IsPlacementUnsaved(); })
								.ToolTipText(LOCTEXT("SavePlacementTooltip", "Save the model's current position to the overrides file"))
								.OnClicked(this, &SFragmentsPanel::OnSavePlacementClicked)
								[
									SNew(SBox)
									.WidthOverride(16.f)
									.HeightOverride(16.f)
									[
										SNew(SImage)
										.Image(FAppStyle::Get().GetBrush("Icons.Save"))
									]
								]
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SButton)
								.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
								.ContentPadding(FMargin(4.f))
								.IsEnabled_Lambda([this]() { return IsPlacementButtonEnabled() && CanRevertPlacement(); })
								.ToolTipText(LOCTEXT("RevertPlacementTooltip", "Revert to the imported position (undoable)"))
								.OnClicked(this, &SFragmentsPanel::OnResetPlacementClicked)
								[
									SNew(SBox)
									.WidthOverride(16.f)
									.HeightOverride(16.f)
									[
										SNew(SImage)
										.Image(Style.GetBrush("Fragments.Icon.Revert"))
									]
								]
							]
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.f, 8.f, 0.f, 0.f)
						[
							// Shift+right-click copies the row, Shift+left-click pastes -- the Details panel's gesture, handled on the row the same way (mouse-up that bubbles).
							SNew(SBorder)
							.BorderImage(FAppStyle::Get().GetBrush("NoBorder"))
							.Padding(0.f)
							.OnMouseButtonUp(this, &SFragmentsPanel::OnPlacementRowMouseUp, false)
							[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SBox)
								.WidthOverride(60.f)
								[
									SNew(STextBlock)
									.TextStyle(Style, "Fragments.Text.Small")
									.Text(LOCTEXT("PlacementLocationLabel", "Location"))
									.ToolTipText(LOCTEXT("PlacementCopyPasteTip", "Shift+right-click to copy, Shift+left-click to paste"))
								]
							]

							+ SHorizontalBox::Slot()
							.FillWidth(1.f)
							.VAlign(VAlign_Center)
							[
								SNew(SNumericVectorInputBox<FVector::FReal>)
								.IsEnabled(this, &SFragmentsPanel::IsPlacementButtonEnabled)
								.bColorAxisLabels(true)
								.AllowSpin(true)
								.Vector(this, &SFragmentsPanel::GetPlacementLocation)
								// Bind per-axis callbacks because UE 5.4 drops vector edits without ConstrainVector.
								.OnXChanged_Lambda([this](FVector::FReal V) { OnPlacementFieldEdited(0, V, false); })
								.OnYChanged_Lambda([this](FVector::FReal V) { OnPlacementFieldEdited(1, V, false); })
								.OnZChanged_Lambda([this](FVector::FReal V) { OnPlacementFieldEdited(2, V, false); })
								.OnXCommitted_Lambda([this](FVector::FReal V, ETextCommit::Type) { OnPlacementFieldEdited(0, V, true); })
								.OnYCommitted_Lambda([this](FVector::FReal V, ETextCommit::Type) { OnPlacementFieldEdited(1, V, true); })
								.OnZCommitted_Lambda([this](FVector::FReal V, ETextCommit::Type) { OnPlacementFieldEdited(2, V, true); })
								.OnBeginSliderMovement(this, &SFragmentsPanel::BeginPlacementDrag)
								.OnEndSliderMovement_Lambda([this](FVector::FReal) { EndPlacementDrag(); })
							]
							]
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.f, 4.f, 0.f, 0.f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::Get().GetBrush("NoBorder"))
							.Padding(0.f)
							.OnMouseButtonUp(this, &SFragmentsPanel::OnPlacementRowMouseUp, true)
							[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SBox)
								.WidthOverride(60.f)
								[
									SNew(STextBlock)
									.TextStyle(Style, "Fragments.Text.Small")
									.Text(LOCTEXT("PlacementRotationLabel", "Rotation"))
									.ToolTipText(LOCTEXT("PlacementCopyPasteTip", "Shift+right-click to copy, Shift+left-click to paste"))
								]
							]

							+ SHorizontalBox::Slot()
							.FillWidth(1.f)
							.VAlign(VAlign_Center)
							[
								// Roll / Pitch / Yaw, shown as X / Y / Z like the editor's own Details panel.
								SNew(SNumericRotatorInputBox<FRotator::FReal>)
								.IsEnabled(this, &SFragmentsPanel::IsPlacementButtonEnabled)
								.bColorAxisLabels(true)
								.AllowSpin(true)
								.Roll(this, &SFragmentsPanel::GetPlacementRotation, 0)
								.Pitch(this, &SFragmentsPanel::GetPlacementRotation, 1)
								.Yaw(this, &SFragmentsPanel::GetPlacementRotation, 2)
								.OnRollChanged_Lambda([this](FRotator::FReal V) { OnPlacementFieldEdited(3, V, false); })
								.OnPitchChanged_Lambda([this](FRotator::FReal V) { OnPlacementFieldEdited(4, V, false); })
								.OnYawChanged_Lambda([this](FRotator::FReal V) { OnPlacementFieldEdited(5, V, false); })
								.OnRollCommitted_Lambda([this](FRotator::FReal V, ETextCommit::Type) { OnPlacementFieldEdited(3, V, true); })
								.OnPitchCommitted_Lambda([this](FRotator::FReal V, ETextCommit::Type) { OnPlacementFieldEdited(4, V, true); })
								.OnYawCommitted_Lambda([this](FRotator::FReal V, ETextCommit::Type) { OnPlacementFieldEdited(5, V, true); })
								// Per-axis drag events, NOT OnBeginSliderMovement/OnEndSliderMovement: in UE 5.4 the rotator box wires the widget-wide pair to Pitch only, so a Roll or Yaw drag never opened the edit (SRotatorInputBox.h, Construct).
								.OnRollBeginSliderMovement(this, &SFragmentsPanel::BeginPlacementDrag)
								.OnPitchBeginSliderMovement(this, &SFragmentsPanel::BeginPlacementDrag)
								.OnYawBeginSliderMovement(this, &SFragmentsPanel::BeginPlacementDrag)
								.OnRollEndSliderMovement_Lambda([this](FRotator::FReal) { EndPlacementDrag(); })
								.OnPitchEndSliderMovement_Lambda([this](FRotator::FReal) { EndPlacementDrag(); })
								.OnYawEndSliderMovement_Lambda([this](FRotator::FReal) { EndPlacementDrag(); })
							]
							]
						]
					]
				]

				// Material overrides: title line with its actions, then the list
				+ SVerticalBox::Slot()
				.FillHeight(1.f)
				[
					SNew(SBorder)
					.BorderImage(Style.GetBrush("Fragments.Background.Secondary"))
					.Padding(FMargin(8.f))
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.FillWidth(1.f)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.TextStyle(Style, "Fragments.Text.Body")
								.Text(LOCTEXT("MaterialOverridesTitle", "Material Overrides"))
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.f, 0.f, 6.f, 0.f)
							[
								// Icon-only, so the tooltip carries the name. "Clear" for the view filters, "Revert" for anything that goes back to the imported state -- the two buttons on this line do very different things.
								SNew(SButton)
								.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
								.ContentPadding(FMargin(4.f))
								.ToolTipText_Lambda([this]()
								{
									return FText::Format(LOCTEXT("ClearFiltersTooltip",
										"Clear Filters: turn off every Highlight and Isolate ({0} active)"),
										FText::AsNumber(GetActiveFilterCount()));
								})
								.IsEnabled_Lambda([this]() { return GetActiveFilterCount() > 0; })
								.OnClicked(this, &SFragmentsPanel::OnResetVisibilityClicked)
								[
									// A funnel with the active-filter count in a circle on its top-right corner; the badge is hidden while nothing is filtered.
									SNew(SBox)
									.WidthOverride(22.f)
									.HeightOverride(20.f)
									[
										SNew(SOverlay)

										+ SOverlay::Slot()
										.HAlign(HAlign_Left)
										.VAlign(VAlign_Bottom)
										[
											SNew(SBox)
											.WidthOverride(16.f)
											.HeightOverride(16.f)
											[
												SNew(SImage)
												.Image(FAppStyle::Get().GetBrush("Icons.Filter"))
											]
										]

										+ SOverlay::Slot()
										.HAlign(HAlign_Right)
										.VAlign(VAlign_Top)
										[
											SNew(SBorder)
											.BorderImage(Style.GetBrush("Fragments.Badge"))
											.Padding(FMargin(3.f, 0.f))
											.HAlign(HAlign_Center)
											.Visibility_Lambda([this]()
											{
												return GetActiveFilterCount() > 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
											})
											[
												SNew(STextBlock)
												.Font(FCoreStyle::GetDefaultFontStyle("Bold", 7))
												.ColorAndOpacity(FLinearColor::White)
												.Text_Lambda([this]() { return FText::AsNumber(GetActiveFilterCount()); })
											]
										]
									]
								]
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SButton)
								.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
								.ContentPadding(FMargin(4.f))
								.ToolTipText(LOCTEXT("RevertOverridesTooltip", "Revert Overrides: put every material on this model back to the imported one (undoable)"))
								.IsEnabled(this, &SFragmentsPanel::HasAnyMaterialOverride)
								.OnClicked(this, &SFragmentsPanel::OnResetAllMaterialReplacementsClicked)
								[
									SNew(SBox)
									.WidthOverride(16.f)
									.HeightOverride(16.f)
									[
										SNew(SImage)
										.Image(Style.GetBrush("Fragments.Icon.Revert"))
									]
								]
							]
						]

						+ SVerticalBox::Slot()
						.FillHeight(1.f)
						.Padding(0.f, 8.f, 0.f, 0.f)
						[
							SNew(SBorder)
							.BorderImage(Style.GetBrush("Fragments.Background.Tertiary"))
							.Padding(FMargin(6.f))
							[
								// A model whose detail is built cell by cell before the rows can be listed shows a pulsing skeleton, with a cell count, rather than an empty box that reads as broken.
								SNew(SWidgetSwitcher)
								.WidgetIndex_Lambda([this]() { return IsMaterialListLoading() ? 1 : 0; })

								+ SWidgetSwitcher::Slot()
								[
									SAssignNew(MaterialReplacementScrollBox, SScrollBox)
								]

								+ SWidgetSwitcher::Slot()
								[
									SNew(SVerticalBox)

									+ SVerticalBox::Slot()
									.AutoHeight()
									.Padding(2.f, 0.f, 2.f, 8.f)
									[
										SNew(STextBlock)
										.TextStyle(Style, "Fragments.Text.Small")
										.AutoWrapText(true)
										.Text_Lambda([this]()
										{
											int32 Built = 0, Total = 0;
											GetEditLoadProgress(Built, Total);
											return UE::Fragments::PanelRules::MaterialListLoadingCaption(Built, Total);
										})
									]

									+ SVerticalBox::Slot()
									.FillHeight(1.f)
									[
										SNew(SBorder)
										.BorderImage(FAppStyle::Get().GetBrush("NoBorder"))
										.Padding(0.f)
										.ColorAndOpacity_Lambda([]()
										{
											return FLinearColor(1.f, 1.f, 1.f,
												UE::Fragments::PanelRules::SkeletonPulseOpacity(FPlatformTime::Seconds()));
										})
										[
											SNew(SVerticalBox)
											+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f) [ MakeSkeletonMaterialRow() ]
											+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f) [ MakeSkeletonMaterialRow() ]
											+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f) [ MakeSkeletonMaterialRow() ]
											+ SVerticalBox::Slot().AutoHeight() [ MakeSkeletonMaterialRow() ]
										]
									]
								]
							]
						]
					]
				]
			]
		]
	];

	RefreshCachedFragments();

	// One refresh path for both a normal edit and an undo/redo: ApplyToWorld broadcasts at the end of both.
	OverridesChangedHandle = UFragmentOverrideState::OnOverridesChanged().AddSP(
		this, &SFragmentsPanel::HandleOverridesChanged);
}

SFragmentsPanel::~SFragmentsPanel()
{
	// A static delegate outlives the widget.
	UFragmentOverrideState::OnOverridesChanged().Remove(OverridesChangedHandle);

	// Closed mid-drag on a placement field: the drag's transaction was opened in BeginPlacementDrag and would otherwise never be closed.
	EndPlacementDrag();

	ClearAllHighlightsAndIsolation();

	// Closing the panel ends material editing and clears promotion.
	if (!PromotedForEditGuid.IsEmpty() && GEditor)
	{
		if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
		{
			if (UFragmentsImporter* Importer = Subsystem->GetImporter())
			{
				if (UFragmentRegistrationManager* RegMgr = Importer->GetRegistrationManager())
				{
					RegMgr->ClearHotPromoteOverridesForModel(PromotedForEditGuid);
				}
			}
		}
		PromotedForEditGuid.Reset();
	}

	if (GEditor && ProcessLoadedDelegateHandle.IsValid())
	{
		if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
		{
			Subsystem->OnProcessLoadedFragmentComplete().Remove(ProcessLoadedDelegateHandle);
			Subsystem->OnModelUnloaded().Remove(ModelUnloadedDelegateHandle);
			Subsystem->OnModelImportStarted().Remove(ModelImportStartedDelegateHandle);
		}
	}

	if (SelectionChangedDelegateHandle.IsValid())
	{
		USelection::SelectionChangedEvent.Remove(SelectionChangedDelegateHandle);
	}
}

FReply SFragmentsPanel::OnImportFragmentModelsClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	const void* ParentWindowHandle = nullptr;
	const TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindBestParentWindowForDialogs(nullptr);
	if (ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid())
	{
		ParentWindowHandle = ParentWindow->GetNativeWindow()->GetOSWindowHandle();
	}

	FString DefaultPath = FPaths::ProjectContentDir();
	GConfig->GetString(TEXT("FragmentsEditor"), TEXT("LastFragmentDirectory"), DefaultPath, GEditorPerProjectIni);

	TArray<FString> Files;
	const bool bSelected = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		LOCTEXT("ImportModelsDialogTitle", "Import Fragment Models").ToString(),
		DefaultPath,
		TEXT(""),
		TEXT("Fragment files (*.frag)|*.frag|All files (*.*)|*.*"),
		EFileDialogFlags::Multiple,
		Files);
	if (!bSelected || Files.Num() == 0)
	{
		return FReply::Handled();
	}

	GConfig->SetString(TEXT("FragmentsEditor"), TEXT("LastFragmentDirectory"),
		*FPaths::GetPath(Files[0]), GEditorPerProjectIni);

	LoadModelFiles(Files);
	return FReply::Handled();
}

FReply SFragmentsPanel::OnBackClicked()
{
	// Leaving the details page ends the edit.
	ActiveCachedModel = FFragmentsCachedModel();
	bHasActiveCachedModel = false;
	RefreshDetailsView();

	if (PanelSwitcher.IsValid())
	{
		PanelSwitcher->SetActiveWidgetIndex(0);
	}

	return FReply::Handled();
}

FReply SFragmentsPanel::OnCachedFragmentDetailsClicked(FString ModelGuid)
{
	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	FFragmentsCachedModel CachedModel;
	if (!Subsystem->GetCachedModelByGuid(ModelGuid, CachedModel))
	{
		return FReply::Handled();
	}

	AFragment* LiveRoot = Subsystem->GetModelFragment(ModelGuid);
	if (IsValid(LiveRoot) && LiveRoot->IsHISMEnabled() && LiveRoot->GetBuckets().Num() > 0)
	{
		ShowCachedModelDetails(CachedModel);
		return FReply::Handled();
	}

	if (CachedModel.Path.IsEmpty() || !FPaths::FileExists(CachedModel.Path))
	{
		UE_LOG(LogFragments, Warning, TEXT("Fragments cached model cannot be loaded. Missing file for '%s': %s"),
			*ModelGuid, *CachedModel.Path);
		Subsystem->ReportImportFailure(FString::Printf(
			TEXT("The fragment file for this model was not found:\n%s"), *CachedModel.Path));
		return FReply::Handled();
	}

	FString ErrorMessage;
	Subsystem->LoadAndProcessFragmentForEditor(CachedModel.Path, GetSelectedOriginMode(), ErrorMessage);

	return FReply::Handled();
}

FReply SFragmentsPanel::OnReimportCachedFragmentClicked(FString ModelGuid)
{
	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	FFragmentsCachedModel CachedModel;
	if (!Subsystem->GetCachedModelByGuid(ModelGuid, CachedModel))
	{
		return FReply::Handled();
	}

	if (CachedModel.Path.IsEmpty() || !FPaths::FileExists(CachedModel.Path))
	{
		UE_LOG(LogFragments, Warning, TEXT("Fragments reimport: missing file for '%s': %s"),
			*ModelGuid, *CachedModel.Path);
		Subsystem->ReportImportFailure(FString::Printf(
			TEXT("Cannot reimport -- the fragment file for this model was not found:\n%s"), *CachedModel.Path));
		return FReply::Handled();
	}

	Subsystem->UnloadFragment(ModelGuid);

	FString ErrorMessage;
	Subsystem->LoadAndProcessFragmentForEditor(CachedModel.Path, GetSelectedOriginMode(), ErrorMessage);

	return FReply::Handled();
}

FReply SFragmentsPanel::OnRemoveCachedFragmentClicked(FString ModelGuid)
{
	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	Subsystem->UnloadFragment(ModelGuid);
	return FReply::Handled();
}

FReply SFragmentsPanel::OnLoadCachedFragmentClicked(FString ModelGuid)
{
	FFragmentsCachedModel CachedModel;
	if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem(); Subsystem && Subsystem->GetCachedModelByGuid(ModelGuid, CachedModel))
	{
		LoadModelFiles({ CachedModel.Path });
	}
	return FReply::Handled();
}

TArray<FString> SFragmentsPanel::PathsForLoadAll() const
{
	TArray<FString> Paths;
	for (const FListedModel& Listed : ListedModels)
	{
		if (UE::Fragments::PanelRules::IsPickedByLoadAll(GetRowState(Listed.Model.Guid), Listed.bSourceExists))
		{
			Paths.Add(Listed.Model.Path);
		}
	}
	return Paths;
}

FReply SFragmentsPanel::OnLoadAllOrCancelImportsClicked()
{
	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (Subsystem && Subsystem->GetImportingCount() > 0)
	{
		Subsystem->CancelAllImports();
		RefreshCachedFragments();   // first imports' rows go away
	}
	else
	{
		LoadModelFiles(PathsForLoadAll());
	}
	return FReply::Handled();
}

void SFragmentsPanel::LoadModelFiles(const TArray<FString>& Paths)
{
	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem || Paths.Num() == 0)
	{
		return;
	}

	// Each file is parsed here, blocking, before it streams in over the following frames.
	TOptional<FScopedSlowTask> Batch;
	if (Paths.Num() > 1)
	{
		Batch.Emplace(static_cast<float>(Paths.Num()), LOCTEXT("LoadingModelsTask", "Loading models"));
		Batch->MakeDialog(/*bShowCancelButton=*/true);
	}

	for (int32 Index = 0; Index < Paths.Num(); ++Index)
	{
		if (Batch.IsSet())
		{
			if (Batch->ShouldCancel())
			{
				Subsystem->ReportLoadingStopped(Paths.Num() - Index);
				break;
			}
			Batch->EnterProgressFrame(1.f, UE::Fragments::PanelRules::MultiLoadProgressText(Index, Paths.Num(), Paths[Index]));
		}

		// A failure raises its own notification and does not stop the rest.
		FString ErrorMessage;
		Subsystem->LoadAndProcessFragmentForEditor(Paths[Index], GetSelectedOriginMode(), ErrorMessage);
	}
}

int32 SFragmentsPanel::GetImportingCount() const
{
	const UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	return Subsystem ? Subsystem->GetImportingCount() : 0;
}

void SFragmentsPanel::RefreshCachedFragments()
{
	if (!CachedFragmentsScrollBox.IsValid())
	{
		return;
	}

	CachedFragmentsScrollBox->ClearChildren();
	ListedModels.Reset();

	if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
	{
		const FString FilterLower = CachedFragmentsFilter.ToLower();
		TArray<FFragmentsCachedModel> Importing;
		Subsystem->GetImportingModels(Importing);
		// Refreshes run from load-complete and unload handlers, possibly after the rows already filled this frame's loaded cache; order by the level as it is now.
		LoadedModelGuidsFrame = MAX_uint64;
		TArray<FFragmentsCachedModel> Models = UE::Fragments::PanelRules::ListedModels(Subsystem->GetCachedModels(), Importing);
		UE::Fragments::PanelRules::OrderLoadedFirst(Models, [this](const FString& Guid) { return GetRowState(Guid); });
		for (const FFragmentsCachedModel& CachedModel : Models)
		{
			if (!FilterLower.IsEmpty())
			{
				const FString AliasLower = CachedModel.Alias.ToLower();
				const FString FileLower = FPaths::GetBaseFilename(CachedModel.Path).ToLower();
				if (!AliasLower.Contains(FilterLower) && !FileLower.Contains(FilterLower))
				{
					continue;
				}
			}
			// The file is checked here, once per refresh, not by the rows and Load all every frame: a file moving under an open panel is rare, and the list refreshes on every import, removal and search.
			const FListedModel& Listed = ListedModels.Add_GetRef({ CachedModel, UE::Fragments::ModelPresence::SourceFileExists(CachedModel.Path) });
			AddCachedFragmentRow(Listed.Model, Listed.bSourceExists);
		}
	}
}

void SFragmentsPanel::OnCachedFragmentsSearchChanged(const FText& NewText)
{
	CachedFragmentsFilter = NewText.ToString();
	RefreshCachedFragments();
}

void SFragmentsPanel::AddCachedFragmentRow(const FFragmentsCachedModel& CachedModel, bool bSourceExists)
{
	using namespace UE::Fragments::PanelRules;

	const ISlateStyle& Style = FFragmentsStyle::Get();
	const FString DisplayName = CachedModel.Alias.IsEmpty()
		? FPaths::GetBaseFilename(CachedModel.Path)
		: CachedModel.Alias;
	const FString Guid = CachedModel.Guid;
	const FString Path = CachedModel.Path;

	auto Primary = [this, Guid, bSourceExists]() { return PrimaryButtonFor(GetRowState(Guid), bSourceExists); };

	// A sample is one placed piece of geometry, which becomes one mesh instance in the level.
	const FText Details = bSourceExists
		? FText::Format(LOCTEXT("CachedFragmentInstances", "{0} instances"), FText::AsNumber(CachedModel.SamplesCount))
		: FText::Format(LOCTEXT("CachedFragmentInstancesMissing", "{0} instances - file not found"), FText::AsNumber(CachedModel.SamplesCount));

	CachedFragmentsScrollBox->AddSlot()
	.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
	[
		SNew(SBorder)
		// The model selected in the level is highlighted, whether or not the selection opened it.
		.BorderImage_Lambda([this, Guid, &Style]()
		{
			return Style.GetBrush(Guid == SelectedModelGuid ? "Fragments.Background.Elevated" : "Fragments.Background.Tertiary");
		})
		.Padding(FMargin(10.f, 6.f))
		[
			SNew(SHorizontalBox)

			// Status: grey not loaded, blinking yellow importing, green loaded.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SImage)
				.Image(Style.GetBrush("Fragments.Dot"))
				.ColorAndOpacity_Lambda([this, Guid]()
				{
					const EModelRowState State = GetRowState(Guid);
					FLinearColor Color = StatusDotColor(State);
					Color.A = StatusDotOpacity(State, FPlatformTime::Seconds());
					return FSlateColor(Color);
				})
				.ToolTipText_Lambda([this, Guid]() { return GetRowStatusTooltip(Guid); })
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.TextStyle(Style, "Fragments.Text.Body")
					.Text(FText::FromString(DisplayName))
					.ToolTipText(FText::FromString(Path))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.TextStyle(Style, "Fragments.Text.Small")
					.Text(Details)
				]
			]

			// The one button: Edit for a model in the level, Load for one that is not.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
				.ContentPadding(FMargin(4.f))
				.IsEnabled_Lambda([Primary]() { return Primary().bEnabled; })
				.ToolTipText_Lambda([Primary]()
				{
					switch (Primary().Action)
					{
					case EPrimaryAction::Edit:         return LOCTEXT("EditTip", "Open the override editor and focus the model in the viewport");
					case EPrimaryAction::CancelImport: return LOCTEXT("CancelImportTip", "Cancel this import; the model is unloaded");
					default:                           return LOCTEXT("LoadTip", "Load this model into the level");
					}
				})
				.OnClicked_Lambda([this, Guid, Primary]()
				{
					switch (Primary().Action)
					{
					case EPrimaryAction::Load:
						return OnLoadCachedFragmentClicked(Guid);
					case EPrimaryAction::CancelImport:
						if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem()) { Subsystem->CancelImport(Guid); }
						RefreshCachedFragments();   // a first import's row goes away
						return FReply::Handled();
					default:
						// Edit is where the model comes into focus -- an import no longer moves the camera.
						const FReply Reply = OnCachedFragmentDetailsClicked(Guid);
						SelectInViewport(Guid, /*bFrameCamera=*/true);
						return Reply;
					}
				})
				[
					SNew(SBox)
					.WidthOverride(16.f)
					.HeightOverride(16.f)
					[
						SNew(SImage)
						.Image_Lambda([Primary, &Style]()
						{
							switch (Primary().Action)
							{
							case EPrimaryAction::Edit:         return Style.GetBrush("Fragments.Icon.Edit");
							case EPrimaryAction::CancelImport: return Style.GetBrush("Fragments.Icon.Close");
							default:                           return Style.GetBrush("Fragments.Icon.Reimport");
							}
						})
					]
				]
			]

			// Everything else.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.f, 0.f, 0.f, 0.f)
			[
				SNew(SComboButton)
				.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
				.HasDownArrow(false)
				.ContentPadding(FMargin(9.f, 4.f))
				.ToolTipText(LOCTEXT("MoreTip", "More options"))
				.IsEnabled_Lambda([this, Guid]() { return GetRowState(Guid) != EModelRowState::Importing; })
				.OnGetMenuContent_Lambda([this, Guid, Path, bSourceExists]() { return MakeRowMenu(Guid, Path, bSourceExists); })
				.ButtonContent()
				[
					SNew(SBox)
					.WidthOverride(4.f)
					.HeightOverride(16.f)
					[
						SNew(SImage)
						.Image(Style.GetBrush("Fragments.Icon.More"))
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SFragmentsPanel::MakeListMenu()
{
	// Counted as the menu opens, so the labels and the confirmations agree with what will happen.
	TSet<FString> InLevel;
	int32 NotInLevel = 0;
	if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
	{
		Subsystem->CollectLoadedModelGuids(InLevel);   // bare roots a saved level brought back count too
		TArray<FFragmentsCachedModel> Importing;
		Subsystem->GetImportingModels(Importing);
		for (const FFragmentsCachedModel& Model : Importing) { InLevel.Add(Model.Guid); }
		for (const FFragmentsCachedModel& Model : Subsystem->GetCachedModels())
		{
			NotInLevel += InLevel.Contains(Model.Guid) ? 0 : 1;
		}
	}
	const int32 InLevelCount = InLevel.Num();

	const FName StyleSet = FFragmentsStyle::GetStyleSetName();
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("MenuClearScene", "Clear Scene Fragment Models ({0})"), FText::AsNumber(InLevelCount)),
		LOCTEXT("MenuClearSceneTip", "Remove every Fragments model from the level, including any still importing; the list is kept"),
		FSlateIcon(StyleSet, "Fragments.Icon.Delete"),
		FUIAction(
			FExecuteAction::CreateLambda([this, InLevelCount]()
			{
				const FText Question = FText::Format(LOCTEXT("ClearSceneConfirm",
					"Remove all {0} Fragments models from the level?\n\nThey stay in the list, so any of them can be loaded again."),
					FText::AsNumber(InLevelCount));
				if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes) { return; }
				if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
				{
					Subsystem->ClearLevelModels();
					RefreshCachedFragments();
				}
			}),
			FCanExecuteAction::CreateLambda([InLevelCount]() { return InLevelCount > 0; })));

	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("MenuForgetAll", "Remove All Models from List ({0})"), FText::AsNumber(NotInLevel)),
		LOCTEXT("MenuForgetAllTip", "Remove every model that is not in the level from this list; models in the level stay listed, and no .frag file is touched"),
		FSlateIcon(StyleSet, "Fragments.Icon.Close"),
		FUIAction(
			FExecuteAction::CreateLambda([this, NotInLevel]()
			{
				const FText Question = FText::Format(LOCTEXT("ForgetAllConfirm",
					"Remove {0} models from the list?\n\nModels in the level stay listed. No .frag file is touched."),
					FText::AsNumber(NotInLevel));
				if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes) { return; }
				if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
				{
					Subsystem->ForgetAllCachedModels();
					RefreshCachedFragments();
				}
			}),
			FCanExecuteAction::CreateLambda([NotInLevel]() { return NotInLevel > 0; })));

	return Menu.MakeWidget();
}

TSharedRef<SWidget> SFragmentsPanel::MakeRowMenu(FString ModelGuid, FString SourcePath, bool bSourceExists)
{
	using namespace UE::Fragments::PanelRules;

	const FName StyleSet = FFragmentsStyle::GetStyleSetName();
	const FName AppStyleSet = FAppStyle::GetAppStyleSetName();
	const FCanExecuteAction IfSourceExists = FCanExecuteAction::CreateLambda([bSourceExists]() { return bSourceExists; });
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

	// The menu waits while the model imports, so a model here is either in the level or not.
	if (GetRowState(ModelGuid) == EModelRowState::Loaded)
	{
		Menu.AddMenuEntry(LOCTEXT("MenuSelect", "Select in Viewport"), LOCTEXT("MenuSelectTip", "Select this model's actor in the level"),
			FSlateIcon(AppStyleSet, "Icons.SelectInViewport"),
			FUIAction(FExecuteAction::CreateSP(this, &SFragmentsPanel::SelectInViewport, ModelGuid, /*bFrameCamera=*/false)));
		Menu.AddMenuEntry(LOCTEXT("MenuFocus", "Focus in Viewport"), LOCTEXT("MenuFocusTip", "Select this model and move the viewport cameras to it"),
			FSlateIcon(AppStyleSet, "Icons.Search"),
			FUIAction(FExecuteAction::CreateSP(this, &SFragmentsPanel::SelectInViewport, ModelGuid, /*bFrameCamera=*/true)));
		Menu.AddMenuSeparator();
		Menu.AddMenuEntry(LOCTEXT("MenuReimport", "Reimport"), LOCTEXT("MenuReimportTip", "Unload and import this model again from its file"),
			FSlateIcon(StyleSet, "Fragments.Icon.Reimport"),
			FUIAction(FExecuteAction::CreateLambda([this, ModelGuid]() { OnReimportCachedFragmentClicked(ModelGuid); }), IfSourceExists));
		Menu.AddMenuEntry(LOCTEXT("MenuRemove", "Remove"), LOCTEXT("MenuRemoveTip", "Remove from the level; the model stays in this list"),
			FSlateIcon(StyleSet, "Fragments.Icon.Delete"),
			FUIAction(FExecuteAction::CreateLambda([this, ModelGuid]() { OnRemoveCachedFragmentClicked(ModelGuid); })));
	}

	Menu.AddMenuEntry(LOCTEXT("MenuShowInExplorer", "Show in Explorer"), FText::FromString(SourcePath),
		FSlateIcon(AppStyleSet, "Icons.FolderOpen"),
		FUIAction(FExecuteAction::CreateLambda([SourcePath]()
		{
			// Opens the containing folder with the file selected.
			FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(SourcePath));
		}), IfSourceExists));

	if (GetRowState(ModelGuid) == EModelRowState::NotLoaded)
	{
		Menu.AddMenuEntry(LOCTEXT("MenuForget", "Remove from list"), LOCTEXT("MenuForgetTip", "Remove this model from the list; the .frag file is not touched"),
			FSlateIcon(StyleSet, "Fragments.Icon.Close"),
			FUIAction(FExecuteAction::CreateLambda([this, ModelGuid]()
			{
				UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
				if (Subsystem && Subsystem->ForgetCachedModel(ModelGuid))
				{
					RefreshCachedFragments();
				}
			})));
	}

	return Menu.MakeWidget();
}

TSharedRef<SWidget> SFragmentsPanel::MakeImportSettingsMenu()
{
	const ISlateStyle& Style = FFragmentsStyle::Get();

	return SNew(SBorder)
		.BorderImage(Style.GetBrush("Fragments.Background.Elevated"))
		.Padding(FMargin(12.f, 10.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(STextBlock)
				.TextStyle(Style, "Fragments.Text.Small")
				.Text(LOCTEXT("ImportSettingsTitle", "Import Settings"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 12.f, 0.f)
				[
					SNew(STextBlock)
					.TextStyle(Style, "Fragments.Text.Body")
					.Text(LOCTEXT("CoordinatesLabel", "Coordinates"))
					.ToolTipText(LOCTEXT("CoordinatesTooltip",
						"Use World Origin: each model is placed on its own.\nUse Base Coordinates: every model is placed relative to the first one imported, so a campus lines up."))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.MinDesiredWidth(170.f)
					[
						SNew(SComboBox<TSharedPtr<EFragmentOriginMode>>)
						.OptionsSource(&OriginModeOptions)
						.OnGenerateWidget(this, &SFragmentsPanel::GenerateOriginModeOption)
						.OnSelectionChanged(this, &SFragmentsPanel::OnOriginModeChanged)
						.InitiallySelectedItem(CurrentOriginMode)
						[
							SNew(STextBlock)
							.TextStyle(Style, "Fragments.Text.Body")
							.Text(this, &SFragmentsPanel::GetCurrentOriginModeLabel)
						]
					]
				]
			]
		];
}

void SFragmentsPanel::ShowCachedModelDetails(const FFragmentsCachedModel& CachedModel)
{
	ActiveCachedModel = CachedModel;
	bHasActiveCachedModel = true;
	RefreshDetailsView();

	if (PanelSwitcher.IsValid())
	{
		PanelSwitcher->SetActiveWidgetIndex(1);
	}
}

bool SFragmentsPanel::ShowCachedModelDetailsByGuid(const FString& ModelGuid)
{
	if (!GEditor)
	{
		return false;
	}

	if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
	{
		FFragmentsCachedModel CachedModel;
		if (Subsystem->GetCachedModelByGuid(ModelGuid, CachedModel))
		{
			ShowCachedModelDetails(CachedModel);
			return true;
		}
	}

	return false;
}

void SFragmentsPanel::RefreshDetailsView()
{
	bUpdatingDetailsView = true;

	const FString Guid = bHasActiveCachedModel ? ActiveCachedModel.Guid : FString();
	const FString Alias = bHasActiveCachedModel ? ActiveCachedModel.Alias : FString();

	// DetailNameText is not set here: it is bound to the active model in Construct, and a SetText would replace that binding with a fixed string that an alias edit could no longer update.

	if (DetailGuidText.IsValid())
	{
		DetailGuidText->SetText(FText::FromString(Guid));
	}

	if (DetailAliasTextBox.IsValid())
	{
		DetailAliasTextBox->SetText(FText::FromString(Alias));
	}

	bUpdatingDetailsView = false;
	RefreshMaterialRows();
}

void SFragmentsPanel::SyncEditPromotion(UFragmentsImporter* Importer, const FString& DesiredGuid)
{
	// Editing materials against a merged LOD1 cell shows the MERGED material, not the one being edited.
	UFragmentRegistrationManager* RegMgr = Importer ? Importer->GetRegistrationManager() : nullptr;
	if (!RegMgr) { return; }

	// Already promoted only if the REGISTRATION still says so.
	if (PromotedForEditGuid == DesiredGuid
		&& (DesiredGuid.IsEmpty() || RegMgr->HasModelPromoteOverride(DesiredGuid)))
	{
		return;
	}

	if (!PromotedForEditGuid.IsEmpty())
	{
		RegMgr->ClearHotPromoteOverridesForModel(PromotedForEditGuid);
		PromotedForEditGuid.Reset();
	}

	if (!DesiredGuid.IsEmpty())
	{
		RegMgr->PromoteAllCellsForModel(DesiredGuid);
		PromotedForEditGuid = DesiredGuid;

		if (IsMaterialListLoading() && !MaterialListLoadingTimerHandle.IsValid())
		{
			MaterialListLoadingTimerHandle = RegisterActiveTimer(
				0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SFragmentsPanel::TickMaterialListLoading));
		}
	}
}

void SFragmentsPanel::RefreshMaterialRows()
{
	ClearAllHighlightsAndIsolation();
	MaterialRows.Empty();

	if (!bHasActiveCachedModel || !GEditor)
	{
		UFragmentsImporterEditorSubsystem* FallbackSubsystem = GEditor
			? ImporterSubsystem()
			: nullptr;
		SyncEditPromotion(FallbackSubsystem ? FallbackSubsystem->GetImporter() : nullptr, FString());
		RebuildMaterialReplacementList();
		return;
	}

	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem)
	{
		SyncEditPromotion(nullptr, FString());
		RebuildMaterialReplacementList();
		return;
	}

	AFragment* ModelFragment = Subsystem->GetModelFragment(ActiveCachedModel.Guid);
	if (!ModelFragment)
	{
		SyncEditPromotion(Subsystem->GetImporter(), FString());
		RebuildMaterialReplacementList();
		return;
	}

	TMap<FString, TSharedPtr<FFragmentsMaterialReplacementRow>> RowsBySourceKey;
	TSet<UHierarchicalInstancedStaticMeshComponent*> SeenComponents;

	UFragmentsImporter* Importer = Subsystem->GetImporter();
	SyncEditPromotion(Importer, ActiveCachedModel.Guid);

	AFragment* BucketRoot = ModelFragment->GetBucketRoot();
	if (!IsValid(BucketRoot)) BucketRoot = ModelFragment;

	const TMap<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor>& SrcMap =
		BucketRoot->GetBIMSourceColors();

	auto AddMaterialComponent = [&](UHierarchicalInstancedStaticMeshComponent* Component)
	{
		if (!IsValid(Component) || SeenComponents.Contains(Component)) return;
		SeenComponents.Add(Component);

		const FBIMSourceColor* SrcPtr = SrcMap.Find(Component);
		if (!SrcPtr) return;
		const FBIMSourceColor& Src = *SrcPtr;

		const FString Key = UE::Fragments::Overrides::BuildSourceKey(
			Src.R, Src.G, Src.B, Src.A, Src.bIsGlass);

		TSharedPtr<FFragmentsMaterialReplacementRow>& Row = RowsBySourceKey.FindOrAdd(Key);
		if (!Row.IsValid())
		{
			Row = MakeShared<FFragmentsMaterialReplacementRow>();
			Row->SourceKey = Key;
			Row->bHasSourceColor = true;
			Row->SourceColor = FLinearColor(Src.R / 255.f, Src.G / 255.f, Src.B / 255.f, Src.A / 255.f);
			Row->bSourceIsGlass = Src.bIsGlass;
			Row->DisplayName = FString::Printf(TEXT("mat_%d_%d_%d_%d"), Src.R, Src.G, Src.B, Src.A);

			UMaterialInstanceDynamic* SourceMID = Importer
				? Importer->GetPooledMaterial(Src.R, Src.G, Src.B, Src.A, Src.bIsGlass)
				: nullptr;
			Row->SourceMaterial = SourceMID;
			Row->SourceAssetPath = SourceMID ? SourceMID->GetPathName() : FString();

			if (SourceMID && MaterialThumbnailPool.IsValid())
			{
				Row->SourceThumbnail = MakeShared<FAssetThumbnail>(
					FAssetData(SourceMID), 96, 96, MaterialThumbnailPool);
			}

			// Deliberately NOT seeded from Component->GetMaterial(0) here.
		}

		Row->Components.Add(Component);
		Row->UsageCount += Component->GetInstanceCount();
	};

	const TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*> Buckets = BucketRoot->GetBuckets();
	for (const TPair<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Pair : Buckets)
	{
		AddMaterialComponent(Pair.Value);
	}

	if (UWorld* World = BucketRoot->GetWorld())
	{
		for (TActorIterator<AFragment> It(World); It; ++It)
		{
			AFragment* Fragment = *It;
			if (!IsValid(Fragment) || Fragment->GetModelGuid() != ActiveCachedModel.Guid) continue;

			TArray<UHierarchicalInstancedStaticMeshComponent*> Components;
			Fragment->GetComponents<UHierarchicalInstancedStaticMeshComponent>(Components);
			for (UHierarchicalInstancedStaticMeshComponent* Component : Components)
			{
				AddMaterialComponent(Component);
			}
		}
	}

	RowsBySourceKey.GenerateValueArray(MaterialRows);
	MaterialRows.Sort([](const TSharedPtr<FFragmentsMaterialReplacementRow>& A, const TSharedPtr<FFragmentsMaterialReplacementRow>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid();
		}
		return A->UsageCount > B->UsageCount;
	});

	// Reconcile the WORLD too, not just the rows.
	if (UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(ActiveCachedModel.Guid, ActiveCachedModel.Path))
	{
		State->ApplyToComponents();
	}

	DeriveRowOverridesFromState();
	RebuildMaterialReplacementList();
}

void SFragmentsPanel::DeriveRowOverridesFromState()
{
	if (!bHasActiveCachedModel || !GEditor)
	{
		return;
	}

	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	UFragmentOverrideState* State = Subsystem
		? Subsystem->GetOrCreateOverrideState(ActiveCachedModel.Guid, ActiveCachedModel.Path)
		: nullptr;

	// Rows are a VIEW of (live components x state).
	for (const TSharedPtr<FFragmentsMaterialReplacementRow>& Row : MaterialRows)
	{
		if (!Row.IsValid())
		{
			continue;
		}

		const FFragmentMaterialOverrideEntry* Entry = State ? State->FindOverride(Row->SourceKey) : nullptr;
		const FString Path = Entry ? Entry->ReplacementMaterialPath : FString();

		Row->ReplacementMaterial = Path.IsEmpty()
			? nullptr
			: Cast<UMaterialInterface>(FSoftObjectPath(Path).TryLoad());

		// Show saved overrides even when their replacement asset cannot load.
		if (!Path.IsEmpty() && !Row->ReplacementMaterial.IsValid())
		{
			UE_LOG(LogFragments, Warning,
				TEXT("Fragment override row for model '%s' key '%s' references '%s', which failed to "
				     "load. The row will read as 'no override' while the entry is still recorded."),
				*ActiveCachedModel.Guid, *Row->SourceKey, *Path);
		}

		Row->ReplacementThumbnail = MakeReplacementThumbnail(Row->ReplacementMaterial.Get());
	}
}

void SFragmentsPanel::RebuildMaterialReplacementList()
{
	if (!MaterialReplacementScrollBox.IsValid())
	{
		return;
	}

	MaterialReplacementScrollBox->ClearChildren();

	for (const TSharedPtr<FFragmentsMaterialReplacementRow>& Row : MaterialRows)
	{
		AddMaterialReplacementRow(Row);
	}
}

void SFragmentsPanel::HandleOverridesChanged(const FString& InModelGuid)
{
	// The row rebuild is filtered to the model on screen: rebuilding rows for a model the panel is not showing would replace the visible list with another model's.
	if (!bHasActiveCachedModel || InModelGuid != ActiveCachedModel.Guid)
	{
		return;
	}

	// RebuildMaterialReplacementList() alone is NOT enough.
	DeriveRowOverridesFromState();
	RebuildMaterialReplacementList();
}

void SFragmentsPanel::AddMaterialReplacementRow(TSharedPtr<FFragmentsMaterialReplacementRow> Row)
{
	if (!Row.IsValid() || !MaterialReplacementScrollBox.IsValid())
	{
		return;
	}

	const ISlateStyle& Style = FFragmentsStyle::Get();
	const TSharedRef<SWidget> SourcePreviewWidget = Row->SourceThumbnail.IsValid()
		? Row->SourceThumbnail->MakeThumbnailWidget()
		: StaticCastSharedRef<SWidget>(
			SNew(SImage)
			.Image(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.ColorAndOpacity(FLinearColor(0.72f, 0.72f, 0.72f, 1.f)));

	// Compact layout: the view filters in a narrow left column; on the right, the thumbnail pair with its name and count beside it, and the picker line underneath ending in the material revert.
	constexpr float ThumbnailSize = 64.f;

	auto MakeThumbnail = [&Style, ThumbnailSize](const TSharedRef<SWidget>& Content)
	{
		return SNew(SBox)
			.WidthOverride(ThumbnailSize)
			.HeightOverride(ThumbnailSize)
			[
				SNew(SBorder)
				.BorderImage(Style.GetBrush("Fragments.Material.ThumbnailFrame"))
				.Padding(FMargin(4.f))
				[
					Content
				]
			];
	};

	auto MakeToggle = [&Style](const FText& Label, TFunction<ECheckBoxState()> IsChecked,
		TFunction<void(ECheckBoxState)> OnChanged)
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda(MoveTemp(IsChecked))
			.OnCheckStateChanged_Lambda(MoveTemp(OnChanged))
			[
				SNew(STextBlock)
				.TextStyle(Style, "Fragments.Text.Small")
				.Text(Label)
			];
	};

	MaterialReplacementScrollBox->AddSlot()
	.Padding(FMargin(0.f, 0.f, 0.f, 6.f))
	[
		SNew(SBorder)
		.BorderImage(Style.GetBrush("Fragments.Background.Elevated"))
		.Padding(FMargin(8.f))
		[
			SNew(SHorizontalBox)

			// Left column: the view filters, Highlight and Isolate ---- Only the filters live here, stacked at the top.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 4.f)
				[
					MakeToggle(LOCTEXT("HighlightLabel", "Highlight"),
						[Row]() {
							return Row.IsValid() && Row->bIsHighlighted ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						},
						[this, Row](ECheckBoxState NewState) {
							ApplyHighlightToRow(Row, NewState == ECheckBoxState::Checked);
						})
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					MakeToggle(LOCTEXT("IsolateLabel", "Isolate"),
						[Row]() {
							return Row.IsValid() && Row->bIsIsolated ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						},
						[this, Row](ECheckBoxState NewState) {
							if (!Row.IsValid()) return;
							const bool bNewIsolated = (NewState == ECheckBoxState::Checked);
							if (Row->bIsIsolated == bNewIsolated) return;
							Row->bIsIsolated = bNewIsolated;
							ApplyIsolationState();
						})
				]
			]

			// Right side: thumbnails + name/count, picker underneath
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						// The whole thumbnail pair is one drop target: the artist is looking at the thumbnails, not the picker, and dropping on either one means the same thing.
						SNew(SAssetDropTarget)
						.ToolTipText(LOCTEXT("MaterialDropTooltip", "Drop a material here to override this one"))
						.OnAreAssetsAcceptableForDrop_Lambda([](TArrayView<FAssetData> Assets)
						{
							return UE::Fragments::Overrides::IsAcceptableOverrideDrop(Assets);
						})
						.OnAssetsDropped_Lambda([this, Row](const FDragDropEvent&, TArrayView<FAssetData> Assets)
						{
							if (UE::Fragments::Overrides::IsAcceptableOverrideDrop(Assets))
							{
								OnReplacementMaterialChanged(Assets[0], Row);
							}
						})
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								MakeThumbnail(SourcePreviewWidget)
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(6.f, 0.f)
							[
								SNew(STextBlock)
								.TextStyle(Style, "Fragments.Text.Heading")
								.Text(FText::FromString(TEXT("→")))
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								// Rebuild the thumbnail widget each refresh without storing a stale row pointer.
								MakeThumbnail(Row->ReplacementThumbnail.IsValid()
									? Row->ReplacementThumbnail->MakeThumbnailWidget()
									: SNullWidget::NullWidget)
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					.Padding(10.f, 0.f, 0.f, 0.f)
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.TextStyle(Style, "Fragments.Text.Small")
							.Text(this, &SFragmentsPanel::GetMaterialSwatchLabel, Row)
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.f, 3.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.TextStyle(Style, "Fragments.Text.Small")
							.Text(FText::Format(LOCTEXT("MaterialUsageCount", "{0} instances"), FText::AsNumber(Row->UsageCount)))
						]
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.f, 0.f, 6.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
						.ContentPadding(FMargin(4.f))
						.ToolTipText(LOCTEXT("UseSelectedReplacementMaterialTooltip", "Use selected material"))
						.OnClicked(this, &SFragmentsPanel::OnUseSelectedReplacementMaterialClicked, Row)
						[
							SNew(SBox)
							.WidthOverride(16.f)
							.HeightOverride(16.f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(SImage)
								.Image(Style.GetBrush("Fragments.Icon.ArrowCircleBrokenLeft"))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						// DisplayBrowse is the "Go to" for the override: it syncs the Content Browser to the material, as every other asset field does.
						SNew(SObjectPropertyEntryBox)
						.AllowedClass(UMaterialInterface::StaticClass())
						.AllowClear(true)
						.DisplayUseSelected(false)
						.DisplayBrowse(true)
						.ObjectPath(this, &SFragmentsPanel::GetReplacementMaterialPath, Row)
						.OnObjectChanged(this, &SFragmentsPanel::OnReplacementMaterialChanged, Row)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.f, 0.f, 0.f, 0.f)
					[
						// Greyed out while the row has no override, so it only offers itself when there is something to revert.
						SNew(SButton)
						.ButtonStyle(&Style.GetWidgetStyle<FButtonStyle>("Fragments.Button.WithIcon"))
						.ContentPadding(FMargin(4.f))
						.IsEnabled(this, &SFragmentsPanel::RowHasOverride, Row)
						.OnClicked(this, &SFragmentsPanel::OnResetMaterialReplacementClicked, Row)
						.ToolTipText(LOCTEXT("RevertMaterialTooltip", "Revert to the imported material"))
						[
							SNew(SBox)
							.WidthOverride(16.f)
							.HeightOverride(16.f)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(SImage)
								.Image(Style.GetBrush("Fragments.Icon.Revert"))
							]
						]
					]
				]
			]
		]
	];
}

FText SFragmentsPanel::GetMaterialSwatchLabel(TSharedPtr<FFragmentsMaterialReplacementRow> Row) const
{
	if (!Row.IsValid() || !Row->bHasSourceColor)
	{
		return LOCTEXT("DefaultMaterialSwatchLabel", "mat_default");
	}

	const int32 R = FMath::RoundToInt(FMath::Clamp(Row->SourceColor.R, 0.0f, 1.0f) * 255.0f);
	const int32 G = FMath::RoundToInt(FMath::Clamp(Row->SourceColor.G, 0.0f, 1.0f) * 255.0f);
	const int32 B = FMath::RoundToInt(FMath::Clamp(Row->SourceColor.B, 0.0f, 1.0f) * 255.0f);
	const int32 A = FMath::RoundToInt(FMath::Clamp(Row->SourceColor.A, 0.0f, 1.0f) * 255.0f);
	return FText::FromString(FString::Printf(TEXT("mat_%d_%d_%d_%d"), R, G, B, A));
}

FString SFragmentsPanel::GetReplacementMaterialPath(TSharedPtr<FFragmentsMaterialReplacementRow> Row) const
{
	if (!Row.IsValid() || !Row->ReplacementMaterial.IsValid())
	{
		return FString();
	}

	return Row->ReplacementMaterial->GetPathName();
}

void SFragmentsPanel::OnReplacementMaterialChanged(const FAssetData& AssetData, TSharedPtr<FFragmentsMaterialReplacementRow> Row)
{
	if (!Row.IsValid())
	{
		return;
	}

	UMaterialInterface* ReplacementMaterial = Cast<UMaterialInterface>(AssetData.GetAsset());

	// One picker change is one user gesture, so it opens exactly one transaction here rather than inside ApplyReplacementToRow -- a multi-row gesture reuses that helper and must stay a single undo step.
	FScopedTransaction Tx(LOCTEXT("SetFragmentMaterialOverride", "Set Fragment Material Override"));
	ApplyReplacementToRow(Row, ReplacementMaterial);
}

FReply SFragmentsPanel::OnUseSelectedReplacementMaterialClicked(TSharedPtr<FFragmentsMaterialReplacementRow> Row)
{
	if (!Row.IsValid())
	{
		return FReply::Handled();
	}

	TArray<FAssetData> SelectedAssets;
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (Cast<UMaterialInterface>(AssetData.GetAsset()))
		{
			OnReplacementMaterialChanged(AssetData, Row);
			break;
		}
	}

	return FReply::Handled();
}

FReply SFragmentsPanel::OnResetMaterialReplacementClicked(TSharedPtr<FFragmentsMaterialReplacementRow> Row)
{
	if (Row.IsValid())
	{
		// Calls the helper directly rather than routing through OnReplacementMaterialChanged, so the undo entry the artist sees is named "Reset", not "Set".
		FScopedTransaction Tx(LOCTEXT("ResetFragmentMaterialOverride", "Reset Fragment Material Override"));
		ApplyReplacementToRow(Row, nullptr);
	}

	return FReply::Handled();
}

FReply SFragmentsPanel::OnResetAllMaterialReplacementsClicked()
{
	if (!bHasActiveCachedModel)
	{
		return FReply::Handled();
	}

	UFragmentsImporterEditorSubsystem* Subsystem =
		ImporterSubsystem();
	UFragmentOverrideState* State = Subsystem
		? Subsystem->GetOrCreateOverrideState(ActiveCachedModel.Guid, ActiveCachedModel.Path)
		: nullptr;
	if (!State)
	{
		return FReply::Handled();
	}

	// Nothing to reset: bail BEFORE opening a transaction, so a stray click does not push an empty undo step in front of whatever the artist actually wants to undo, and costs no component pass.
	if (State->GetOverrides().MaterialOverrides.Num() == 0)
	{
		return FReply::Handled();
	}

	// ONE transaction, ONE clear, ONE apply -- deliberately not a loop over rows.
	FScopedTransaction Tx(LOCTEXT("ResetAllFragmentMaterialOverrides", "Reset All Fragment Material Overrides"));

	// Modify() snapshots the state as it is RIGHT NOW into the open transaction -- that snapshot is what undo restores.
	State->Modify();
	State->ClearAllOverrides();
	State->ApplyToWorld();

	return FReply::Handled();
}

FReply SFragmentsPanel::OnResetVisibilityClicked()
{
	ClearAllHighlightsAndIsolation();
	return FReply::Handled();
}

void SFragmentsPanel::ApplyReplacementToRow(TSharedPtr<FFragmentsMaterialReplacementRow> Row,
	UMaterialInterface* ReplacementMaterial)
{
	if (!Row.IsValid() || !bHasActiveCachedModel)
	{
		return;
	}

	UFragmentsImporterEditorSubsystem* Subsystem =
		ImporterSubsystem();
	UFragmentOverrideState* State = Subsystem
		? Subsystem->GetOrCreateOverrideState(ActiveCachedModel.Guid, ActiveCachedModel.Path)
		: nullptr;
	if (!State)
	{
		return;
	}

	// The row carries the display metadata the sidecar records alongside the override; the state object owns the override itself.
	FFragmentMaterialOverrideEntry Meta;
	Meta.SourceKey           = Row->SourceKey;
	Meta.SourceMaterialName  = Row->DisplayName;
	Meta.SourceMaterialAsset = Row->SourceAssetPath;
	Meta.UsageCount          = Row->UsageCount;
	Meta.bSourceIsGlass      = Row->bSourceIsGlass;
	Meta.bHasSourceColor     = Row->bHasSourceColor;
	Meta.SourceColor         = Row->SourceColor;

	const FString NewPath = ReplacementMaterial ? ReplacementMaterial->GetPathName() : FString();

	// No FScopedTransaction here on purpose: the CALLER opens one, and names it for the gesture the artist performed ("Set" vs "Reset"), which this helper cannot tell apart.
	State->Modify();
	State->SetOverride(Row->SourceKey, NewPath, Meta);

	// Components, sidecar and listeners all reconcile from the state in one place, so an undo can never produce a different world or file than the edit it reverses.
	State->ApplyToWorld();
}

void SFragmentsPanel::ApplyHighlightToRow(TSharedPtr<FFragmentsMaterialReplacementRow> Row, bool bHighlight)
{
	if (!Row.IsValid()) return;

	Row->bIsHighlighted = bHighlight;
	const float Value = bHighlight ? 1.0f : 0.0f;

	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& WeakComp : Row->Components)
	{
		UHierarchicalInstancedStaticMeshComponent* Comp = WeakComp.Get();
		if (!IsValid(Comp)) continue;

		const int32 Count = Comp->GetInstanceCount();
		const int32 Stride = Comp->NumCustomDataFloats;
		if (Stride < 2) continue;

		const TArray<float>& Data = Comp->PerInstanceSMCustomData;
		for (int32 i = 0; i < Count; ++i)
		{
			const int32 HiddenIdx = i * Stride + 1;
			if (bHighlight && Data.IsValidIndex(HiddenIdx) && Data[HiddenIdx] > 0.5f) continue;
			Comp->SetCustomDataValue(i, 0, Value, /*bMarkRenderStateDirty=*/false);
		}
		Comp->MarkRenderStateDirty();
	}
}

void SFragmentsPanel::ApplyIsolationState()
{
	TSet<UHierarchicalInstancedStaticMeshComponent*> KeptComps;
	TSet<UHierarchicalInstancedStaticMeshComponent*> AllComps;

	for (const TSharedPtr<FFragmentsMaterialReplacementRow>& Row : MaterialRows)
	{
		if (!Row.IsValid()) continue;
		for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& WeakComp : Row->Components)
		{
			if (UHierarchicalInstancedStaticMeshComponent* Comp = WeakComp.Get())
			{
				AllComps.Add(Comp);
				if (Row->bIsIsolated) KeptComps.Add(Comp);
			}
		}
	}

	const bool bAnyIsolated = !KeptComps.IsEmpty();
	for (UHierarchicalInstancedStaticMeshComponent* Comp : AllComps)
	{
		if (!IsValid(Comp) || Comp->NumCustomDataFloats < 2) continue;
		const float Value = (bAnyIsolated && !KeptComps.Contains(Comp)) ? 1.0f : 0.0f;
		const int32 Count = Comp->GetInstanceCount();
		for (int32 i = 0; i < Count; ++i)
		{
			Comp->SetCustomDataValue(i, 1, Value, /*bMarkRenderStateDirty=*/false);
		}
		Comp->MarkRenderStateDirty();
	}
}

void SFragmentsPanel::ClearAllHighlightsAndIsolation()
{
	for (const TSharedPtr<FFragmentsMaterialReplacementRow>& Row : MaterialRows)
	{
		if (!Row.IsValid()) continue;
		Row->bIsHighlighted = false;
		Row->bIsIsolated = false;
		for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& WeakComp : Row->Components)
		{
			UHierarchicalInstancedStaticMeshComponent* Comp = WeakComp.Get();
			if (!IsValid(Comp) || Comp->NumCustomDataFloats < 2) continue;
			const int32 Count = Comp->GetInstanceCount();
			for (int32 i = 0; i < Count; ++i)
			{
				Comp->SetCustomDataValue(i, 0, 0.0f, /*bMarkRenderStateDirty=*/false);
				Comp->SetCustomDataValue(i, 1, 0.0f, /*bMarkRenderStateDirty=*/false);
			}
			Comp->MarkRenderStateDirty();
		}
	}
}

TSharedPtr<FAssetThumbnail> SFragmentsPanel::MakeReplacementThumbnail(UMaterialInterface* Material) const
{
	if (!MaterialThumbnailPool.IsValid()) return nullptr;
	const FAssetData AssetData = Material ? FAssetData(Material) : FAssetData();
	return MakeShared<FAssetThumbnail>(AssetData, 96, 96, MaterialThumbnailPool);
}

void SFragmentsPanel::OnAliasTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	if (bUpdatingDetailsView || !bHasActiveCachedModel || !GEditor)
	{
		return;
	}

	ActiveCachedModel.Alias = NewText.ToString();

	if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
	{
		if (Subsystem->UpdateCachedModelAlias(ActiveCachedModel.Guid, ActiveCachedModel.Alias))
		{
			RefreshCachedFragments();
		}
	}
}

void SFragmentsPanel::HandleEditorSelectionChanged(UObject* NewSelection)
{
	if (!GEditor)
	{
		return;
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		return;
	}

	FString Guid;
	for (FSelectionIterator It(*SelectedActors); It; ++It)
	{
		if (AFragment* Fragment = Cast<AFragment>(*It))
		{
			Guid = Fragment->GetModelGuid();
			break;
		}
	}
	SelectedModelGuid = Guid;
	if (Guid.IsEmpty())
	{
		return;
	}

	// Opening the override editor promotes the model to LOD0, so only a selection the user made in the level, with this tab in front, opens it -- not the panel's own Select / Focus / Edit.
	if (!bSelectingInViewport && IsPanelInForeground())
	{
		ShowCachedModelDetailsByGuid(Guid);
	}
}

void SFragmentsPanel::HandleProcessLoadedFragmentComplete(bool bSuccess, const FString& ErrorMessage, const FString& ModelGuid)
{
	if (bSuccess)
	{
		// The list only: opening the override editor would promote the new model to LOD0, which a multi-file import would do to every building in turn.
		RefreshCachedFragments();
		return;
	}

	// LogFragments, not LogTemp, so this async-side failure lands on the same channel as the synchronous one FailImport logs (FragmentsImporterEditorSubsystem.cpp) -- one place to look for either kind of import failure.
	UE_LOG(LogFragments, Warning, TEXT("Fragments process failed for '%s': %s"), *ModelGuid, *ErrorMessage);
}

void SFragmentsPanel::HandleModelUnloaded(const FString& ModelGuid)
{
	if (UE::Fragments::PanelRules::ShouldLeaveDetailsPage(bHasActiveCachedModel, ActiveCachedModel.Guid, ModelGuid))
	{
		OnBackClicked();
	}
	// An unloaded model drops out of the loaded group at the top of the list.
	RefreshCachedFragments();
}

FText SFragmentsPanel::OriginModeToText(EFragmentOriginMode Mode)
{
	switch (Mode)
	{
	case EFragmentOriginMode::UseWorldOrigin:
		return LOCTEXT("OriginModeUseWorldOrigin", "Use World Origin");
	case EFragmentOriginMode::UseBaseCoordinates:
		return LOCTEXT("OriginModeUseBaseCoordinates", "Use Base Coordinates");
	default:
		return LOCTEXT("OriginModeUnknown", "Unknown");
	}
}

TSharedRef<SWidget> SFragmentsPanel::GenerateOriginModeOption(TSharedPtr<EFragmentOriginMode> Option) const
{
	return SNew(STextBlock)
		.TextStyle(FFragmentsStyle::Get(), "Fragments.Text.Body")
		.Text(Option.IsValid() ? OriginModeToText(*Option) : FText::GetEmpty());
}

void SFragmentsPanel::OnOriginModeChanged(TSharedPtr<EFragmentOriginMode> NewSelection, ESelectInfo::Type)
{
	if (NewSelection.IsValid())
	{
		CurrentOriginMode = NewSelection;
		GConfig->SetString(TEXT("FragmentsEditor"), TEXT("ImportOriginMode"),
			*UE::Fragments::PanelRules::OriginModeToConfig(*NewSelection), GEditorPerProjectIni);
	}
}

FText SFragmentsPanel::GetCurrentOriginModeLabel() const
{
	return CurrentOriginMode.IsValid() ? OriginModeToText(*CurrentOriginMode) : FText::GetEmpty();
}

EFragmentOriginMode SFragmentsPanel::GetSelectedOriginMode() const
{
	return CurrentOriginMode.IsValid() ? *CurrentOriginMode : EFragmentOriginMode::UseWorldOrigin;
}

bool SFragmentsPanel::IsPlacementButtonEnabled() const
{
	if (!bHasActiveCachedModel) return false;
	return IsCachedFragmentLoaded(ActiveCachedModel.Guid);
}

bool SFragmentsPanel::HasAnyMaterialOverride() const
{
	// FindOverrideState, never GetOrCreate: these run every frame from IsEnabled, and creating a state is not something a button's enable check may do as a side effect.
	const UFragmentsImporterEditorSubsystem* Subsystem =
		bHasActiveCachedModel ? ImporterSubsystem() : nullptr;
	const UFragmentOverrideState* State = Subsystem ? Subsystem->FindOverrideState(ActiveCachedModel.Guid) : nullptr;
	return State && State->GetOverrides().MaterialOverrides.Num() > 0;
}

bool SFragmentsPanel::RowHasOverride(TSharedPtr<FFragmentsMaterialReplacementRow> Row) const
{
	// Asks the state, not Row->ReplacementMaterial: an override whose material no longer loads shows an empty picker but is still recorded, and must stay revertable.
	const UFragmentsImporterEditorSubsystem* Subsystem =
		(Row.IsValid() && bHasActiveCachedModel) ? ImporterSubsystem() : nullptr;
	const UFragmentOverrideState* State = Subsystem ? Subsystem->FindOverrideState(ActiveCachedModel.Guid) : nullptr;
	return State && State->FindOverride(Row->SourceKey) != nullptr;
}

bool SFragmentsPanel::IsCachedFragmentLoaded(const FString& ModelGuid) const
{
	if (!GEditor || ModelGuid.IsEmpty()) return false;

	// Every row asks, several times, every frame; collect once per frame instead of searching the level per call.
	if (LoadedModelGuidsFrame != GFrameCounter)
	{
		LoadedModelGuidsFrame = GFrameCounter;
		LoadedModelGuids.Reset();
		if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
		{
			Subsystem->CollectLoadedModelGuids(LoadedModelGuids);
		}
	}
	return LoadedModelGuids.Contains(ModelGuid);
}

UE::Fragments::PanelRules::EModelRowState SFragmentsPanel::GetRowState(const FString& ModelGuid) const
{
	UFragmentsImporterEditorSubsystem* Subsystem =
		ImporterSubsystem();
	return UE::Fragments::PanelRules::RowStateFor(
		Subsystem && Subsystem->IsModelImporting(ModelGuid), IsCachedFragmentLoaded(ModelGuid));
}

void SFragmentsPanel::HandleModelImportStarted(const FString& ModelGuid)
{
	// A first import has no cached entry yet; refreshing shows its row, blinking, right away.
	if (!ListedModels.ContainsByPredicate([&ModelGuid](const FListedModel& Listed) { return Listed.Model.Guid == ModelGuid; }))
	{
		RefreshCachedFragments();
	}
}

void SFragmentsPanel::SelectInViewport(FString ModelGuid, bool bFrameCamera)
{
	TGuardValue<bool> Guard(bSelectingInViewport, true);
	if (UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem())
	{
		Subsystem->SelectModelInViewport(ModelGuid, bFrameCamera);
	}
}

FText SFragmentsPanel::GetRowStatusTooltip(const FString& ModelGuid) const
{
	switch (GetRowState(ModelGuid))
	{
	case UE::Fragments::PanelRules::EModelRowState::Importing:
	{
		const UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
		return Subsystem
			? FText::Format(LOCTEXT("ModelImportingStageTip", "Importing - {0}"), Subsystem->GetImportStageText(ModelGuid))
			: LOCTEXT("ModelImportingTip", "Importing...");
	}
	case UE::Fragments::PanelRules::EModelRowState::Loaded:
		return LOCTEXT("ModelLoadedTip", "Loaded in the level");
	default:
		return LOCTEXT("ModelNotLoadedTip", "Not loaded");
	}
}

bool SFragmentsPanel::IsPanelInForeground() const
{
	// No owning tab means the panel was embedded somewhere other than its own tab: it is visible wherever it is.
	const TSharedPtr<SDockTab> Tab = OwnerTab.Pin();
	return !Tab.IsValid() || Tab->IsForeground();
}

int32 SFragmentsPanel::GetActiveFilterCount() const
{
	return UE::Fragments::PanelRules::CountActiveFilters(MaterialRows);
}

FReply SFragmentsPanel::OnSavePlacementClicked()
{
	if (!bHasActiveCachedModel || !GEditor)
	{
		return FReply::Handled();
	}

	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	FTransform ActorXform;
	if (!GetLivePlacement(ActorXform))
	{
		UE_LOG(LogTemp, Warning, TEXT("Save Placement: no AFragment found for model '%s'"), *ActiveCachedModel.Guid);
		return FReply::Handled();
	}

	UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(ActiveCachedModel.Guid, ActiveCachedModel.Path);
	if (!State)
	{
		return FReply::Handled();
	}

	// Goes through the state instead of writing the sidecar directly.
	FScopedTransaction Tx(LOCTEXT("SaveFragmentPlacement", "Save Fragment Placement"));
	State->Modify();

	FFragmentPlacement& Placement = State->GetOverridesMutable().Placement;
	Placement.Location = ActorXform.GetLocation();
	Placement.Rotation = ActorXform.Rotator();
	Placement.bHasValue = true;

	// Logged BEFORE the write and phrased as intent, not as an outcome.
	UE_LOG(LogFragments, Log,
		TEXT("Saving placement for '%s' (model %s): Location=%s Rotation=%s"),
		*(ActiveCachedModel.Alias.IsEmpty() ? ActiveCachedModel.Guid : ActiveCachedModel.Alias),
		*ActiveCachedModel.Guid,
		*ActorXform.GetLocation().ToString(),
		*ActorXform.Rotator().ToString());

	State->ApplyToWorld();

	return FReply::Handled();
}

FReply SFragmentsPanel::OnResetPlacementClicked()
{
	if (!bHasActiveCachedModel || !GEditor)
	{
		return FReply::Handled();
	}

	UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	UFragmentOverrideState* State =
		Subsystem->GetOrCreateOverrideState(ActiveCachedModel.Guid, ActiveCachedModel.Path);
	if (!State)
	{
		return FReply::Handled();
	}

	// One gesture, one transaction -- and the ACTOR's transform belongs inside it too.
	FScopedTransaction Tx(LOCTEXT("ResetFragmentPlacement", "Reset Fragment Placement"));

	if (AFragment* ModelFragment = Subsystem->GetModelFragment(ActiveCachedModel.Guid); IsValid(ModelFragment))
	{
		// Modify() before the move, not after: it snapshots the pre-move transform, which is the only thing undo has to restore from.
		ModelFragment->Modify();
		// For a chunked-import root, identity would leave the geometry at its baked placement until the next reimport; this delta shows the unplaced pose now.
		ModelFragment->SetActorTransform(ModelFragment->HasBakedFrame()
			? UE::Fragments::Placement::ActorDeltaForNoPlacement(
				ModelFragment->GetBakedFrame(), ModelFragment->GetBakedPlacement())
			: FTransform::Identity);
	}

	State->Modify();
	State->GetOverridesMutable().Placement = FFragmentPlacement{};
	State->ApplyToWorld();

	UE_LOG(LogTemp, Log, TEXT("Reset placement for '%s'"),
		*(ActiveCachedModel.Alias.IsEmpty() ? ActiveCachedModel.Guid : ActiveCachedModel.Alias));

	return FReply::Handled();
}

namespace
{
	// Far below anything typed or dragged in cm / degrees, far above float noise from composing the transform back and forth.
	constexpr float PlacementTolerance = 1.e-3f;

	FTransform SavedPlacementOf(const UFragmentOverrideState* State)
	{
		const FFragmentPlacement* Saved = State ? &State->GetOverrides().Placement : nullptr;
		return (Saved && Saved->bHasValue) ? FTransform(Saved->Rotation, Saved->Location) : FTransform::Identity;
	}
}

bool SFragmentsPanel::GetLivePlacement(FTransform& OutPlacement) const
{
	UFragmentsImporterEditorSubsystem* Subsystem =
		bHasActiveCachedModel ? ImporterSubsystem() : nullptr;
	const AFragment* Fragment = Subsystem ? Subsystem->GetModelFragment(ActiveCachedModel.Guid) : nullptr;
	if (!IsValid(Fragment))
	{
		return false;
	}

	// A chunked-import root sits at identity with its placement baked into the instances, so its actor transform is only a delta; compose it back into the absolute placement the file stores.
	OutPlacement = Fragment->HasBakedFrame()
		? UE::Fragments::Placement::ComposeSavedPlacement(
			Fragment->GetBakedFrame(), Fragment->GetBakedPlacement(), Fragment->GetActorTransform())
		: Fragment->GetActorTransform();
	return true;
}

TOptional<FVector> SFragmentsPanel::GetPlacementLocation() const
{
	FTransform Live;
	return GetLivePlacement(Live) ? TOptional<FVector>(Live.GetLocation()) : TOptional<FVector>();
}

TOptional<FRotator::FReal> SFragmentsPanel::GetPlacementRotation(int32 Component) const
{
	FTransform Live;
	if (!GetLivePlacement(Live))
	{
		return TOptional<FRotator::FReal>();
	}
	const FRotator Rot = Live.Rotator();
	return Component == 0 ? Rot.Roll : (Component == 1 ? Rot.Pitch : Rot.Yaw);
}

FReply SFragmentsPanel::OnPlacementRowMouseUp(const FGeometry&, const FPointerEvent& MouseEvent, bool bRotation)
{
	FTransform Live;
	if (!MouseEvent.GetModifierKeys().IsShiftDown() || !IsPlacementButtonEnabled() || !GetLivePlacement(Live))
	{
		return FReply::Unhandled();
	}

	using namespace UE::Fragments::Placement;
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		FPlatformApplicationMisc::ClipboardCopy(*(bRotation
			? FormatRotationForClipboard(Live.Rotator())
			: FormatLocationForClipboard(Live.GetLocation())));
		return FReply::Handled();
	}

	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		// Anything on the clipboard that is not a complete Location / Rotation pastes nothing.
		FString Text;
		FPlatformApplicationMisc::ClipboardPaste(Text);
		if (bRotation)
		{
			FRotator Rotation;
			if (!ParseRotationFromClipboard(Text, Rotation)) return FReply::Handled();
			Live.SetRotation(Rotation.Quaternion());
		}
		else
		{
			FVector Location;
			if (!ParseLocationFromClipboard(Text, Location)) return FReply::Handled();
			Live.SetLocation(Location);
		}
		SetLivePlacement(Live, /*bCommitted=*/true);
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

void SFragmentsPanel::OnPlacementFieldEdited(int32 Field, double NewValue, bool bCommitted)
{
	FTransform Live;
	if (!GetLivePlacement(Live))
	{
		return;
	}

	if (Field < 3)
	{
		FVector Location = Live.GetLocation();
		Location[Field] = NewValue;
		Live.SetLocation(Location);
	}
	else
	{
		FRotator Rot = Live.Rotator();
		(Field == 3 ? Rot.Roll : (Field == 4 ? Rot.Pitch : Rot.Yaw)) = NewValue;
		Live.SetRotation(Rot.Quaternion());
	}
	SetLivePlacement(Live, bCommitted);
}

void SFragmentsPanel::SetLivePlacement(const FTransform& NewPlacement, bool bCommitted)
{
	// A field reports a change on every step of a drag and a commit when it is typed.
	if (!bPlacementDragActive && !bCommitted)
	{
		return;
	}

	FTransform Live;
	UFragmentsImporterEditorSubsystem* Subsystem =
		bHasActiveCachedModel ? ImporterSubsystem() : nullptr;
	AFragment* Fragment = Subsystem ? Subsystem->GetModelFragment(ActiveCachedModel.Guid) : nullptr;
	if (!IsValid(Fragment) || !GetLivePlacement(Live))
	{
		return;
	}

	// A field also commits when it merely loses focus.
	if (NewPlacement.Equals(Live, PlacementTolerance))
	{
		return;
	}

	const FTransform ActorDelta = Fragment->HasBakedFrame()
		? UE::Fragments::Placement::ActorDeltaForPlacement(
			Fragment->GetBakedFrame(), Fragment->GetBakedPlacement(), NewPlacement)
		: NewPlacement;

	// The fields only MOVE the model, exactly like the gizmo; nothing reaches the overrides file until Save.
	if (bPlacementDragActive)
	{
		// BeginPlacementDrag already opened the transaction and snapshotted the actor.
		Fragment->SetActorTransform(ActorDelta);
		return;
	}

	// A typed or pasted value: one undo step.
	FScopedTransaction Tx(LOCTEXT("SetFragmentPlacement", "Move Fragment"));
	Fragment->Modify();
	Fragment->SetActorTransform(ActorDelta);
}

void SFragmentsPanel::BeginPlacementDrag()
{
	UFragmentsImporterEditorSubsystem* Subsystem =
		bHasActiveCachedModel ? ImporterSubsystem() : nullptr;
	AFragment* Fragment = Subsystem ? Subsystem->GetModelFragment(ActiveCachedModel.Guid) : nullptr;
	if (bPlacementDragActive || !IsValid(Fragment))
	{
		return;
	}

	// A drag spans many frames, so a scoped transaction cannot cover it: open one here and close it in EndPlacementDrag, the way the editor's own details panel handles a dragged value.
	GEditor->BeginTransaction(LOCTEXT("DragFragmentPlacement", "Move Fragment"));
	Fragment->Modify();
	bPlacementDragActive = true;
}

void SFragmentsPanel::EndPlacementDrag()
{
	if (!bPlacementDragActive)
	{
		return;
	}
	bPlacementDragActive = false;

	if (GEditor)
	{
		GEditor->EndTransaction();
	}
}

bool SFragmentsPanel::IsPlacementUnsaved() const
{
	FTransform Live;
	if (!GetLivePlacement(Live))
	{
		return false;
	}
	const UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	const UFragmentOverrideState* State = Subsystem ? Subsystem->FindOverrideState(ActiveCachedModel.Guid) : nullptr;
	return !Live.Equals(SavedPlacementOf(State), PlacementTolerance);
}

bool SFragmentsPanel::CanRevertPlacement() const
{
	FTransform Live;
	if (!GetLivePlacement(Live))
	{
		return false;
	}
	const UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	const UFragmentOverrideState* State = Subsystem ? Subsystem->FindOverrideState(ActiveCachedModel.Guid) : nullptr;
	const bool bHasSaved = State && State->GetOverrides().Placement.bHasValue;
	return bHasSaved || !Live.Equals(FTransform::Identity, PlacementTolerance);
}

bool SFragmentsPanel::GetEditLoadProgress(int32& OutCellsBuilt, int32& OutCellsTotal) const
{
	OutCellsBuilt = 0;
	OutCellsTotal = 0;
	if (!bHasActiveCachedModel || PromotedForEditGuid != ActiveCachedModel.Guid)
	{
		return false;
	}
	const UFragmentsImporterEditorSubsystem* Subsystem = ImporterSubsystem();
	const UFragmentsImporter* Importer = Subsystem ? Subsystem->GetImporter() : nullptr;
	const UFragmentRegistrationManager* RegMgr = Importer ? Importer->GetRegistrationManager() : nullptr;
	return RegMgr && RegMgr->GetLOD0BuildProgress(ActiveCachedModel.Guid, OutCellsBuilt, OutCellsTotal);
}

bool SFragmentsPanel::IsMaterialListLoading() const
{
	int32 Built = 0, Total = 0;
	const bool bPending = GetEditLoadProgress(Built, Total);
	return UE::Fragments::PanelRules::IsMaterialListLoading(
		bHasActiveCachedModel, bHasActiveCachedModel && PromotedForEditGuid == ActiveCachedModel.Guid, bPending);
}

TSharedRef<SWidget> SFragmentsPanel::MakeSkeletonMaterialRow() const
{
	// The bones are faint rounded boxes laid out like an override row: the two toggles, the source and replacement thumbnails with the arrow between, the name and count lines, the picker bar.
	static const FSlateRoundedBoxBrush Bone(FLinearColor(1.f, 1.f, 1.f, 0.07f), 4.f);
	const ISlateStyle& Style = FFragmentsStyle::Get();
	auto Box = [](float W, float H) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(W).HeightOverride(H)[ SNew(SImage).Image(&Bone) ];
	};

	return SNew(SBorder)
		.BorderImage(Style.GetBrush("Fragments.Background.Secondary"))
		.Padding(FMargin(10.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 0.f, 12.f, 0.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f) [ Box(70.f, 14.f) ]
					+ SVerticalBox::Slot().AutoHeight() [ Box(56.f, 14.f) ]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ Box(56.f, 56.f) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f) [ Box(12.f, 12.f) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f) [ Box(56.f, 56.f) ]

				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.f, 0.f, 0.f, 6.f) [ Box(120.f, 12.f) ]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left) [ Box(80.f, 10.f) ]
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 10.f, 0.f, 0.f)
			[
				SNew(SBox).HeightOverride(18.f)[ SNew(SImage).Image(&Bone) ]
			]
		];
}

EActiveTimerReturnType SFragmentsPanel::TickMaterialListLoading(double InCurrentTime, float InDeltaTime)
{
	// Nothing to do but keep the pulse repainting until the detail build finishes.
	if (IsMaterialListLoading())
	{
		return EActiveTimerReturnType::Continue;
	}
	MaterialListLoadingTimerHandle.Reset();
	return EActiveTimerReturnType::Stop;
}

#undef LOCTEXT_NAMESPACE
