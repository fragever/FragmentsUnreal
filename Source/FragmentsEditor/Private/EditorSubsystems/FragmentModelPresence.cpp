// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "EditorSubsystems/FragmentModelPresence.h"

#include "Misc/Paths.h"

namespace UE::Fragments::ModelPresence
{
	bool ShouldUnloadAfterRootDeletion(const UObject* ImporterEntryNow, FObjectKey EntryAtDeletion)
	{
		return ImporterEntryNow && EntryAtDeletion != FObjectKey() && FObjectKey(ImporterEntryNow) == EntryAtDeletion;
	}

	bool ShouldReimportAfterUndo(bool bImporterHasModel, const FString& SourcePath)
	{
		return !bImporterHasModel && SourceFileExists(SourcePath);
	}

	bool ShouldReimportOnMapOpen(bool bIsBucketRoot, const FString& SourcePath, bool bImporterHasModel)
	{
		return bIsBucketRoot && !bImporterHasModel && SourceFileExists(SourcePath);
	}

	bool SourceFileExists(const FString& SourcePath)
	{
		return !SourcePath.IsEmpty() && FPaths::FileExists(SourcePath);
	}
}
