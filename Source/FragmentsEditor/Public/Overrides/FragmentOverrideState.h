// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// UFragmentOverrideState -- the transactable owner of one model's material overrides and placement.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Importer/FragmentOverrides.h"
#include "FragmentOverrideState.generated.h"

/** Broadcast after any change -- edit, undo or redo -- with the model GUID that changed. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnFragmentOverridesChanged, const FString& /*ModelGuid*/);

UCLASS()
class FRAGMENTSEDITOR_API UFragmentOverrideState : public UObject
{
	GENERATED_BODY()

public:
	UFragmentOverrideState();

	/** Seeds the object from a sidecar load. Records identity; does not touch the world or disk. */
	void Initialise(const FString& InModelGuid, const FString& InFragPath, const FFragmentOverrides& InLoaded);

	/** Sets or replaces one override. */
	void SetOverride(const FString& SourceKey, const FString& ReplacementPath,
		const FFragmentMaterialOverrideEntry& Meta);

	/** Returns true if an entry was actually removed -- an assertable answer for tests and callers that want to log what happened, NOT a signal to decide whether a transaction was needed. */
	bool ClearOverride(const FString& SourceKey);

	/** Returns how many entries were removed, with the same caveat: it arrives too late to gate a transaction on. */
	int32 ClearAllOverrides();

	const FFragmentMaterialOverrideEntry* FindOverride(const FString& SourceKey) const;

	const FFragmentOverrides& GetOverrides() const { return Overrides; }
	const FString& GetModelGuid() const { return ModelGuid; }
	const FString& GetFragPath() const { return FragPath; }

	/** Mutable access for callers that already hold a transaction and need to edit Placement. */
	FFragmentOverrides& GetOverridesMutable() { return Overrides; }

	/** The ONE reconciliation routine. */
	void ApplyToWorld();

	/** Step 1 of ApplyToWorld on its own: push the current overrides onto every bucket component this model has RIGHT NOW, and nothing else. */
	void ApplyToComponents();

	/** Fires after any change -- edit, undo or redo. */
	static FOnFragmentOverridesChanged& OnOverridesChanged();

	/** Fires on exactly the object a transaction restored, for both undo and redo. */
	virtual void PostEditUndo() override;

private:
	UPROPERTY()
	FFragmentOverrides Overrides;

	UPROPERTY()
	FString ModelGuid;

	UPROPERTY()
	FString FragPath;
};
