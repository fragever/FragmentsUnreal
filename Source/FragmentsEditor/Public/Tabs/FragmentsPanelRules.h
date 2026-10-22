// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystems/FragmentsEditorCacheSaveGame.h"
#include "Utils/FragmentsUtils.h"

struct FFragmentsMaterialReplacementRow;

/** Testable rules for panel rows, filters, loading, and settings. */
namespace UE::Fragments::PanelRules
{
	/** Where a listed model is. Importing wins: a reimport is in the level and importing at once. */
	enum class EModelRowState : uint8
	{
		NotLoaded,
		Importing,
		Loaded,
	};
	FRAGMENTSEDITOR_API EModelRowState RowStateFor(bool bImporting, bool bLoaded);

	/** A model row is one button and a "more" menu. */
	enum class EPrimaryAction : uint8
	{
		Load,
		Edit,
		CancelImport,
	};
	struct FPrimaryButton
	{
		EPrimaryAction Action = EPrimaryAction::Load;
		bool bEnabled = false;
	};
	FRAGMENTSEDITOR_API FPrimaryButton PrimaryButtonFor(EModelRowState State, bool bSourceExists);

	/** Load all loads a listed model only if its own Load button would: not in the level, not importing, and its file still there. */
	FRAGMENTSEDITOR_API bool IsPickedByLoadAll(EModelRowState State, bool bSourceExists);

	/** The panel leaves a model's details page when that model unloads, and only then. */
	FRAGMENTSEDITOR_API bool ShouldLeaveDetailsPage(
		bool bHasActiveModel, const FString& ActiveModelGuid, const FString& UnloadedModelGuid);

	/** A multi-file import's loading dialog line, e.g. "Loading 3 of 47: Building_A". */
	FRAGMENTSEDITOR_API FText MultiLoadProgressText(int32 Index, int32 Count, const FString& FilePath);

	/** The row's status dot: grey when not loaded, green when loaded, yellow while importing. */
	FRAGMENTSEDITOR_API FLinearColor StatusDotColor(EModelRowState State);
	/** The importing dot blinks; the others are steady. */
	FRAGMENTSEDITOR_API float StatusDotOpacity(EModelRowState State, double TimeSeconds);

	/** The models the list shows: every cached model, then each model whose first import is still running -- it is not cached until it finishes, but its row (and blinking dot) shows at once. */
	FRAGMENTSEDITOR_API TArray<FFragmentsCachedModel> ListedModels(
		const TArray<FFragmentsCachedModel>& Cached, const TArray<FFragmentsCachedModel>& Importing);

	/** Models in the level float to the top of a long list, so they are one glance away to edit or unload: loaded first, then importing, then the rest. */
	FRAGMENTSEDITOR_API void OrderLoadedFirst(
		TArray<FFragmentsCachedModel>& Models, TFunctionRef<EModelRowState(const FString& Guid)> StateOf);

	/** Active view filters on the override editor: each Highlight and each Isolate counts once. */
	FRAGMENTSEDITOR_API int32 CountActiveFilters(TConstArrayView<TSharedPtr<FFragmentsMaterialReplacementRow>> Rows);

	/** The footer's documentation and support buttons read their URLs from FragmentsUnreal.uplugin (DocsURL, SupportURL). */
	FRAGMENTSEDITOR_API bool IsWebLink(const FString& Url);

	/** The coordinate-origin import setting as stored in the editor's per-project config. */
	FRAGMENTSEDITOR_API FString OriginModeToConfig(EFragmentOriginMode Mode);
	/** False, leaving Out untouched, for a value this version does not know. */
	FRAGMENTSEDITOR_API bool OriginModeFromConfig(const FString& Value, EFragmentOriginMode& Out);

	/** Opening a model for editing can build its full-detail geometry cell by cell first (a cooked model's LOD0), and the override list cannot be filled until that is done. */
	FRAGMENTSEDITOR_API bool IsMaterialListLoading(bool bHasActiveModel, bool bPromotedForActiveModel, bool bLOD0BuildPending);
	FRAGMENTSEDITOR_API FText MaterialListLoadingCaption(int32 CellsBuilt, int32 CellsTotal);
	/** The skeleton's pulse: opacity between 0.5 and 1, one cycle every two seconds. */
	FRAGMENTSEDITOR_API float SkeletonPulseOpacity(double TimeSeconds);
}
