// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"

/** The editor subsystem's decisions about whether an imported model is still present, kept apart so they can be tested without an import. */
namespace UE::Fragments::ModelPresence
{
	/** A tick after a model's current bucket root was deleted: unload the model -- as the panel's Remove would -- only if the importer still holds the SAME entry it held at the deletion. */
	FRAGMENTSEDITOR_API bool ShouldUnloadAfterRootDeletion(const UObject* ImporterEntryNow, FObjectKey EntryAtDeletion);

	/** Reimport an undo-restored root when its source is available and not already loaded. */
	FRAGMENTSEDITOR_API bool ShouldReimportAfterUndo(bool bImporterHasModel, const FString& SourcePath);

	/** Reimport saved roots on map load when their source is available. */
	FRAGMENTSEDITOR_API bool ShouldReimportOnMapOpen(bool bIsBucketRoot, const FString& SourcePath, bool bImporterHasModel);

	/** The model's .frag is still where it was imported from. */
	FRAGMENTSEDITOR_API bool SourceFileExists(const FString& SourcePath);
}
