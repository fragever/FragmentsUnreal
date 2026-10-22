// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Tabs/FragmentsPanelRules.h"
#include "Algo/StableSort.h"

#include "Tabs/SFragmentsPanel.h"
#include "Misc/Paths.h"

namespace UE::Fragments::PanelRules
{
	EModelRowState RowStateFor(bool bImporting, bool bLoaded)
	{
		if (bImporting) { return EModelRowState::Importing; }
		return bLoaded ? EModelRowState::Loaded : EModelRowState::NotLoaded;
	}

	FPrimaryButton PrimaryButtonFor(EModelRowState State, bool bSourceExists)
	{
		switch (State)
		{
		case EModelRowState::Loaded:    return { EPrimaryAction::Edit, true };
		case EModelRowState::Importing: return { EPrimaryAction::CancelImport, true };
		default:                        return { EPrimaryAction::Load, bSourceExists };
		}
	}

	bool IsPickedByLoadAll(EModelRowState State, bool bSourceExists)
	{
		return State == EModelRowState::NotLoaded && bSourceExists;
	}

	bool ShouldLeaveDetailsPage(bool bHasActiveModel, const FString& ActiveModelGuid, const FString& UnloadedModelGuid)
	{
		return bHasActiveModel && !UnloadedModelGuid.IsEmpty() && ActiveModelGuid == UnloadedModelGuid;
	}

	FText MultiLoadProgressText(int32 Index, int32 Count, const FString& FilePath)
	{
		return FText::Format(NSLOCTEXT("FragmentsPanel", "LoadingModelOfCount", "Loading {0} of {1}: {2}"),
			FText::AsNumber(Index + 1), FText::AsNumber(Count), FText::FromString(FPaths::GetBaseFilename(FilePath)));
	}

	FLinearColor StatusDotColor(EModelRowState State)
	{
		switch (State)
		{
		case EModelRowState::Importing: return FLinearColor(FColor(0xF2, 0xC2, 0x3A));
		case EModelRowState::Loaded:    return FLinearColor(FColor(0x4C, 0xC3, 0x6B));
		default:                        return FLinearColor(FColor(0x5A, 0x5A, 0x5E));
		}
	}

	float StatusDotOpacity(EModelRowState State, double TimeSeconds)
	{
		if (State != EModelRowState::Importing) { return 1.f; }
		// One gentle pulse per second, between 25% and full.
		const float Wave = 0.5f + 0.5f * FMath::Cos(static_cast<float>(TimeSeconds * 2.0 * UE_DOUBLE_PI));
		return FMath::Lerp(0.25f, 1.f, Wave);
	}

	TArray<FFragmentsCachedModel> ListedModels(
		const TArray<FFragmentsCachedModel>& Cached, const TArray<FFragmentsCachedModel>& Importing)
	{
		TArray<FFragmentsCachedModel> Listed = Cached;
		for (const FFragmentsCachedModel& Model : Importing)
		{
			if (!Listed.ContainsByPredicate([&Model](const FFragmentsCachedModel& Existing) { return Existing.Guid == Model.Guid; }))
			{
				Listed.Add(Model);
			}
		}
		return Listed;
	}

	void OrderLoadedFirst(
		TArray<FFragmentsCachedModel>& Models, TFunctionRef<EModelRowState(const FString& Guid)> StateOf)
	{
		auto Rank = [&StateOf](const FFragmentsCachedModel& Model)
		{
			switch (StateOf(Model.Guid))
			{
			case EModelRowState::Loaded:    return 0;
			case EModelRowState::Importing: return 1;
			default:                        return 2;
			}
		};
		Algo::StableSortBy(Models, Rank);
	}

	int32 CountActiveFilters(TConstArrayView<TSharedPtr<FFragmentsMaterialReplacementRow>> Rows)
	{
		int32 Count = 0;
		for (const TSharedPtr<FFragmentsMaterialReplacementRow>& Row : Rows)
		{
			if (!Row.IsValid()) { continue; }
			Count += (Row->bIsHighlighted ? 1 : 0) + (Row->bIsIsolated ? 1 : 0);
		}
		return Count;
	}

	bool IsWebLink(const FString& Url)
	{
		const FString Trimmed = Url.TrimStartAndEnd();
		for (const TCHAR* Scheme : { TEXT("https://"), TEXT("http://") })
		{
			if (Trimmed.StartsWith(Scheme, ESearchCase::IgnoreCase) && Trimmed.Len() > FCString::Strlen(Scheme))
			{
				return true;
			}
		}
		return false;
	}

	FString OriginModeToConfig(EFragmentOriginMode Mode)
	{
		return StaticEnum<EFragmentOriginMode>()->GetNameStringByValue(static_cast<int64>(Mode));
	}

	bool OriginModeFromConfig(const FString& Value, EFragmentOriginMode& Out)
	{
		const int64 Found = StaticEnum<EFragmentOriginMode>()->GetValueByNameString(Value);
		if (Found == INDEX_NONE) { return false; }
		Out = static_cast<EFragmentOriginMode>(Found);
		return true;
	}
	bool IsMaterialListLoading(bool bHasActiveModel, bool bPromotedForActiveModel, bool bLOD0BuildPending)
	{
		return bHasActiveModel && bPromotedForActiveModel && bLOD0BuildPending;
	}

	FText MaterialListLoadingCaption(int32 CellsBuilt, int32 CellsTotal)
	{
		return CellsTotal > 0
			? FText::Format(NSLOCTEXT("FragmentsPanel", "MaterialListLoadingCount", "Loading full-detail geometry for editing... {0} of {1} cells"),
				FText::AsNumber(CellsBuilt), FText::AsNumber(CellsTotal))
			: NSLOCTEXT("FragmentsPanel", "MaterialListLoading", "Loading full-detail geometry for editing...");
	}

	float SkeletonPulseOpacity(double TimeSeconds)
	{
		// A cosine between 1 and 0.5 over two seconds -- the soft pulse a skeleton placeholder uses.
		const float Wave = 0.5f + 0.5f * FMath::Cos(static_cast<float>(TimeSeconds * UE_DOUBLE_PI));
		return FMath::Lerp(0.5f, 1.f, Wave);
	}
}
