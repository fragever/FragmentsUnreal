// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystems/FragmentsEditorCacheSaveGame.h"
#include "Utils/FragmentsUtils.h"
#include "Widgets/Input/SComboBox.h"
#include "Tabs/FragmentsPanelRules.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SDockTab;
class SEditableTextBox;
class SScrollBox;
class STextBlock;
class SWidgetSwitcher;
class FAssetThumbnail;
class FAssetThumbnailPool;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;

struct FFragmentsMaterialReplacementRow
{
	TWeakObjectPtr<UMaterialInterface> SourceMaterial;
	TWeakObjectPtr<UMaterialInterface> ReplacementMaterial;
	TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Components;
	TSharedPtr<FAssetThumbnail> SourceThumbnail;
	TSharedPtr<FAssetThumbnail> ReplacementThumbnail;
	FString SourceKey;
	FString SourceAssetPath;
	FLinearColor SourceColor = FLinearColor::Transparent;
	int32 UsageCount = 0;
	bool bHasSourceColor = false;
	bool bSourceIsGlass = false;
	FString DisplayName;
	bool bIsHighlighted = false;
	bool bIsIsolated = false;
};

/** Main Fragments editor panel — the content that lives inside the dockable tab. */
class SFragmentsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFragmentsPanel) {}
		/** The tab hosting the panel. */
		SLATE_ARGUMENT(TSharedPtr<SDockTab>, OwnerTab)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SFragmentsPanel() override;

private:
	/** Multi-select file dialog; imports each chosen .frag in turn. */
	FReply OnImportFragmentModelsClicked();
	FReply OnBackClicked();
	FReply OnCachedFragmentDetailsClicked(FString ModelGuid);
	FReply OnReimportCachedFragmentClicked(FString ModelGuid);
	FReply OnRemoveCachedFragmentClicked(FString ModelGuid);
	FReply OnLoadCachedFragmentClicked(FString ModelGuid);
	/** The listed (search-filtered) models whose own Load would work. */
	TArray<FString> PathsForLoadAll() const;
	/** Cancels every import while any is running; otherwise loads PathsForLoadAll. */
	FReply OnLoadAllOrCancelImportsClicked();
	int32 GetImportingCount() const;
	/** Loads each file in turn; a batch gets one cancellable "Loading 3 of 47" dialog. */
	void LoadModelFiles(const TArray<FString>& Paths);
	bool IsPanelInForeground() const;
	UE::Fragments::PanelRules::EModelRowState GetRowState(const FString& ModelGuid) const;
	FText GetRowStatusTooltip(const FString& ModelGuid) const;
	/** The panel's own Select / Focus / Edit: selects (and frames) the model without the selection opening the override editor. */
	void SelectInViewport(FString ModelGuid, bool bFrameCamera);
	/** The row's "more" menu, built when it opens so its entries match the model's state then. */
	TSharedRef<SWidget> MakeRowMenu(FString ModelGuid, FString SourcePath, bool bSourceExists);
	/** The list's own menu, beside Load all: clear the level, or the list, in one go. */
	TSharedRef<SWidget> MakeListMenu();
	/** The popup behind the gear beside the import button. */
	TSharedRef<SWidget> MakeImportSettingsMenu();
	void HandleModelImportStarted(const FString& ModelGuid);
	int32 GetActiveFilterCount() const;
	void RefreshCachedFragments();
	void AddCachedFragmentRow(const FFragmentsCachedModel& CachedModel, bool bSourceExists);
	void ShowCachedModelDetails(const FFragmentsCachedModel& CachedModel);
	bool ShowCachedModelDetailsByGuid(const FString& ModelGuid);
	void RefreshDetailsView();
	void RefreshMaterialRows();

	/** Promote/clear the model under material edit to/from LOD0 so the panel shows the individual element's real material rather than a merged LOD1 cell's appearance. */
	void SyncEditPromotion(class UFragmentsImporter* Importer, const FString& DesiredGuid);

	/** The open model's edit-time detail build, while it runs: see GetLOD0BuildProgress. */
	bool GetEditLoadProgress(int32& OutCellsBuilt, int32& OutCellsTotal) const;
	bool IsMaterialListLoading() const;
	/** A placeholder shaped like an override row, for the loading skeleton. */
	TSharedRef<SWidget> MakeSkeletonMaterialRow() const;
	/** Keeps Slate repainting the skeleton's pulse while the list loads; stops itself after. */
	EActiveTimerReturnType TickMaterialListLoading(double InCurrentTime, float InDeltaTime);

	/** Refreshes each row's replacement material/thumbnail from the model's UFragmentOverrideState. */
	void DeriveRowOverridesFromState();
	void RebuildMaterialReplacementList();
	void AddMaterialReplacementRow(TSharedPtr<FFragmentsMaterialReplacementRow> Row);
	FString GetReplacementMaterialPath(TSharedPtr<FFragmentsMaterialReplacementRow> Row) const;
	FText GetMaterialSwatchLabel(TSharedPtr<FFragmentsMaterialReplacementRow> Row) const;
	void OnReplacementMaterialChanged(const struct FAssetData& AssetData, TSharedPtr<FFragmentsMaterialReplacementRow> Row);
	FReply OnUseSelectedReplacementMaterialClicked(TSharedPtr<FFragmentsMaterialReplacementRow> Row);
	FReply OnResetMaterialReplacementClicked(TSharedPtr<FFragmentsMaterialReplacementRow> Row);
	FReply OnResetAllMaterialReplacementsClicked();
	FReply OnResetVisibilityClicked();
	FReply OnSavePlacementClicked();
	FReply OnResetPlacementClicked();
	bool IsPlacementButtonEnabled() const;

	/** Placement fields. */
	bool GetLivePlacement(FTransform& OutPlacement) const;
	TOptional<FVector> GetPlacementLocation() const;
	TOptional<FRotator::FReal> GetPlacementRotation(int32 Component) const;
	/** Shift+right-click copies the Location (or Rotation) row, Shift+left-click pastes it -- the Details panel's gesture and clipboard format. */
	FReply OnPlacementRowMouseUp(const FGeometry& Geometry, const FPointerEvent& MouseEvent, bool bRotation);
	/** Field 0-2 = Location X/Y/Z, 3-5 = Rotation Roll/Pitch/Yaw (shown as X/Y/Z). */
	void OnPlacementFieldEdited(int32 Field, double NewValue, bool bCommitted);
	void SetLivePlacement(const FTransform& NewPlacement, bool bCommitted);
	void BeginPlacementDrag();
	void EndPlacementDrag();
	/** The live placement differs from the file: a move -- gizmo or fields -- nobody has saved yet. */
	bool IsPlacementUnsaved() const;
	/** There is a placement to revert: one is saved, or the model has been moved off its import pose. */
	bool CanRevertPlacement() const;
	/** Enable states for the override controls, so each offers itself only when it has something to act on. */
	bool HasAnyMaterialOverride() const;
	bool RowHasOverride(TSharedPtr<FFragmentsMaterialReplacementRow> Row) const;
	bool IsCachedFragmentLoaded(const FString& ModelGuid) const;
	void OnCachedFragmentsSearchChanged(const FText& NewText);
	/** Records the edit on the model's UFragmentOverrideState and lets ApplyToWorld reconcile components, sidecar and listeners. */
	void ApplyReplacementToRow(TSharedPtr<FFragmentsMaterialReplacementRow> Row, UMaterialInterface* ReplacementMaterial);
	void ApplyHighlightToRow(TSharedPtr<FFragmentsMaterialReplacementRow> Row, bool bHighlight);
	void ApplyIsolationState();
	void ClearAllHighlightsAndIsolation();
	TSharedPtr<FAssetThumbnail> MakeReplacementThumbnail(UMaterialInterface* Material) const;
	void OnAliasTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void HandleEditorSelectionChanged(UObject* NewSelection);
	void HandleProcessLoadedFragmentComplete(bool bSuccess, const FString& ErrorMessage, const FString& ModelGuid);
	/** Leaves the details page when the model it shows unloads -- including a viewport delete of its root and a map change, which never pass through the panel. */
	void HandleModelUnloaded(const FString& ModelGuid);

	/** Bound to UFragmentOverrideState::OnOverridesChanged(). */
	void HandleOverridesChanged(const FString& InModelGuid);

	TSharedRef<SWidget> GenerateOriginModeOption(TSharedPtr<EFragmentOriginMode> Option) const;
	void OnOriginModeChanged(TSharedPtr<EFragmentOriginMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurrentOriginModeLabel() const;
	EFragmentOriginMode GetSelectedOriginMode() const;
	static FText OriginModeToText(EFragmentOriginMode Mode);

	TSharedPtr<SWidgetSwitcher> PanelSwitcher;
	TWeakPtr<SDockTab> OwnerTab;

	/** The Fragments model selected in the level, highlighted in the list. */
	FString SelectedModelGuid;

	/** The models the list currently shows (search applied), and whether each one's file was found at the last refresh -- for the rows and Load all. */
	struct FListedModel
	{
		FFragmentsCachedModel Model;
		bool bSourceExists = false;
	};
	TArray<FListedModel> ListedModels;

	/** Set while SelectInViewport selects, so that selection does not open the override editor. */
	bool bSelectingInViewport = false;

	/** Loaded models, collected at most once per frame: every row asks, every frame. */
	mutable TSet<FString> LoadedModelGuids;
	mutable uint64 LoadedModelGuidsFrame = MAX_uint64;
	TSharedPtr<SScrollBox> CachedFragmentsScrollBox;
	TSharedPtr<SEditableTextBox> CachedFragmentsSearchBox;
	FString CachedFragmentsFilter;
	TSharedPtr<STextBlock> DetailNameText;
	TSharedPtr<STextBlock> DetailGuidText;
	TSharedPtr<SEditableTextBox> DetailAliasTextBox;
	TSharedPtr<SScrollBox> MaterialReplacementScrollBox;
	TSharedPtr<FAssetThumbnailPool> MaterialThumbnailPool;
	TArray<TSharedPtr<FFragmentsMaterialReplacementRow>> MaterialRows;
	FFragmentsCachedModel ActiveCachedModel;
	bool bHasActiveCachedModel = false;

	/** Model guid currently force-promoted to LOD0 for material editing, or empty. */
	FString PromotedForEditGuid;

	/** A placement field is being dragged: a transaction is open from BeginPlacementDrag until EndPlacementDrag, so the whole drag is one undo step. */
	bool bPlacementDragActive = false;
	bool bUpdatingDetailsView = false;
	FDelegateHandle ProcessLoadedDelegateHandle;
	FDelegateHandle ModelUnloadedDelegateHandle;
	FDelegateHandle ModelImportStartedDelegateHandle;
	FDelegateHandle SelectionChangedDelegateHandle;

	/** Subscription to UFragmentOverrideState::OnOverridesChanged. */
	FDelegateHandle OverridesChangedHandle;

	TSharedPtr<FActiveTimerHandle> MaterialListLoadingTimerHandle;

	TArray<TSharedPtr<EFragmentOriginMode>> OriginModeOptions;
	TSharedPtr<EFragmentOriginMode> CurrentOriginMode;
};
