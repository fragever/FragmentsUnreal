// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "FragmentsEditorModule.h"

#include "Style/FragmentsStyle.h"
#include "Tabs/SFragmentsPanel.h"
#include "Spatial/FragmentViewProvider.h"
#include "EditorSubsystems/FragmentsEditorViewProvider.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "Importer/FragmentsImporter.h"
#include "Editor.h"

#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FragmentsEditor"

IMPLEMENT_MODULE(FFragmentsEditor, FragmentsEditor);

const FName FFragmentsEditor::FragmentsPanelTabName(TEXT("FragmentsPanel"));

void FFragmentsEditor::StartupModule()
{
	FFragmentsStyle::Initialize();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		FragmentsPanelTabName,
		FOnSpawnTab::CreateRaw(this, &FFragmentsEditor::SpawnFragmentsPanelTab))
		.SetDisplayName(LOCTEXT("FragmentsPanelTabTitle", "Fragments Viewer"))
		.SetTooltipText(LOCTEXT("FragmentsPanelTabTooltip", "Open the Fragments BIM viewer"))
		.SetIcon(FSlateIcon(FFragmentsStyle::GetStyleSetName(), "Fragments.Icon.Logo"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory());

	// Lets the runtime streaming pass see editor viewport cameras.
	UE::Fragments::ViewProvider::Get().BindStatic(
		&FragmentsEditorViewProvider::GatherFromLevelViewports);

	// Lets FragmentSelection's resolver find an importer in the editor, where there is no GameInstance.
	UE::Fragments::ImporterResolver::GetEditorResolver().BindLambda(
		[](UWorld*) -> UFragmentsImporter*
		{
			if (GEditor)
			{
				if (UFragmentsImporterEditorSubsystem* Subsys =
						GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>())
				{
					return Subsys->GetImporter();
				}
			}
			return nullptr;
		});
}

void FFragmentsEditor::ShutdownModule()
{
	UE::Fragments::ImporterResolver::GetEditorResolver().Unbind();

	UE::Fragments::ViewProvider::Get().Unbind();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FragmentsPanelTabName);

	FFragmentsStyle::Shutdown();
}

TSharedRef<SDockTab> FFragmentsEditor::SpawnFragmentsPanelTab(const FSpawnTabArgs& /*Args*/)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	Tab->SetContent(SNew(SFragmentsPanel).OwnerTab(Tab));
	return Tab;
}

#undef LOCTEXT_NAMESPACE
