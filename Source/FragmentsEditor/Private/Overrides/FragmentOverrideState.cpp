// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Overrides/FragmentOverrideState.h"

#include "Editor.h"
#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "Fragment/Fragment.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"
#include "Importer/FragmentsImporter.h"
#include "Utils/FragmentsLog.h"

UFragmentOverrideState::UFragmentOverrideState()
{
	// RF_Transactional is what makes this object visible to GEditor's transaction buffer.
	SetFlags(RF_Transactional);
}

void UFragmentOverrideState::Initialise(const FString& InModelGuid, const FString& InFragPath,
	const FFragmentOverrides& InLoaded)
{
	ModelGuid = InModelGuid;
	FragPath  = InFragPath;
	Overrides = InLoaded;
	Overrides.ModelGuid    = InModelGuid;
	Overrides.FragmentPath = InFragPath;
}

void UFragmentOverrideState::SetOverride(const FString& SourceKey, const FString& ReplacementPath,
	const FFragmentMaterialOverrideEntry& Meta)
{
	if (SourceKey.IsEmpty())
	{
		return;
	}

	if (ReplacementPath.IsEmpty())
	{
		ClearOverride(SourceKey);
		return;
	}

	FFragmentMaterialOverrideEntry Entry = Meta;
	Entry.SourceKey = SourceKey;
	Entry.ReplacementMaterialPath = ReplacementPath;

	// Replace in place rather than append.
	for (FFragmentMaterialOverrideEntry& Existing : Overrides.MaterialOverrides)
	{
		if (Existing.SourceKey == SourceKey)
		{
			Existing = MoveTemp(Entry);
			return;
		}
	}
	Overrides.MaterialOverrides.Add(MoveTemp(Entry));
}

bool UFragmentOverrideState::ClearOverride(const FString& SourceKey)
{
	return Overrides.MaterialOverrides.RemoveAll(
		[&SourceKey](const FFragmentMaterialOverrideEntry& E) { return E.SourceKey == SourceKey; }) > 0;
}

int32 UFragmentOverrideState::ClearAllOverrides()
{
	const int32 Removed = Overrides.MaterialOverrides.Num();
	Overrides.MaterialOverrides.Reset();
	return Removed;
}

const FFragmentMaterialOverrideEntry* UFragmentOverrideState::FindOverride(const FString& SourceKey) const
{
	return Overrides.MaterialOverrides.FindByPredicate(
		[&SourceKey](const FFragmentMaterialOverrideEntry& E) { return E.SourceKey == SourceKey; });
}

FOnFragmentOverridesChanged& UFragmentOverrideState::OnOverridesChanged()
{
	static FOnFragmentOverridesChanged Delegate;
	return Delegate;
}

void UFragmentOverrideState::PostEditUndo()
{
	Super::PostEditUndo();
	ApplyToWorld();
}

void UFragmentOverrideState::ApplyToComponents()
{
	// Absent actor is NORMAL, not an error: the panel builds its rows from the sidecar and can list a model that is not in the level.
	UFragmentsImporterEditorSubsystem* Subsystem =
		GEditor ? GEditor->GetEditorSubsystem<UFragmentsImporterEditorSubsystem>() : nullptr;

	if (AFragment* ModelFragment = Subsystem ? Subsystem->GetModelFragment(ModelGuid) : nullptr)
	{
		AFragment* BucketRoot = ModelFragment->GetBucketRoot();
		if (!IsValid(BucketRoot)) { BucketRoot = ModelFragment; }

		for (const TPair<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor>& Pair :
		     BucketRoot->GetBIMSourceColors())
		{
			UHierarchicalInstancedStaticMeshComponent* Component = Pair.Key;
			if (!IsValid(Component)) { continue; }

			const FBIMSourceColor& Src = Pair.Value;
			const FString Key = UE::Fragments::Overrides::BuildSourceKey(Src.R, Src.G, Src.B, Src.A, Src.bIsGlass);

			UMaterialInterface* Replacement = nullptr;
			if (const FFragmentMaterialOverrideEntry* Entry = FindOverride(Key))
			{
				Replacement = Cast<UMaterialInterface>(
					FSoftObjectPath(Entry->ReplacementMaterialPath).TryLoad());

				// Keep a missing replacement in state so the panel can still show the saved override.
				if (!Replacement && !Entry->ReplacementMaterialPath.IsEmpty())
				{
					UE_LOG(LogFragments, Warning,
						TEXT("Fragment override for model '%s' key '%s' points at '%s', which "
						     "failed to load. Reverting that bucket to its source material; the "
						     "override entry is still recorded and will re-apply if the asset returns."),
						*ModelGuid, *Key, *Entry->ReplacementMaterialPath);
				}
			}

			// A null Replacement is the RESET case and must still be written: leaving the previous override on the component is exactly what makes an undo look like it did nothing.
			UMaterialInterface* ToApply = Replacement;
			if (!ToApply && Subsystem->GetImporter())
			{
				ToApply = Subsystem->GetImporter()->GetPooledMaterial(Src.R, Src.G, Src.B, Src.A, Src.bIsGlass);
			}

			Component->SetMaterial(0, ToApply);
			Component->MarkRenderStateDirty();
		}
	}
}

void UFragmentOverrideState::ApplyToWorld()
{
	// Apply the override to loaded components.
	ApplyToComponents();

	// Persist the override in the sidecar.
	const bool bWritten = UE::Fragments::Overrides::Update(FragPath, ModelGuid,
		[this](FFragmentOverrides& Out)
		{
			Out.MaterialOverrides = Overrides.MaterialOverrides;
			Out.Placement         = Overrides.Placement;
		});

	// A failed write is NOT cosmetic and must not be swallowed.
	if (!bWritten)
	{
		UE_LOG(LogFragments, Error,
			TEXT("Fragment overrides for model '%s' could NOT be written to '%s'. The world and the "
			     "panel now show an edit that is not on disk; it will be lost on reload."),
			*ModelGuid, *UE::Fragments::Overrides::GetSidecarPath(FragPath));
	}

	// Notify listeners after the world and sidecar are updated.
	OnOverridesChanged().Broadcast(ModelGuid);
}
