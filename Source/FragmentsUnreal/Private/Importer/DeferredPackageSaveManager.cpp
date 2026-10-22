// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant




#include "Importer/DeferredPackageSaveManager.h"
#include "Utils/FragmentsLog.h"
#include "Misc/PackageName.h"
#include "Serialization/ArchiveSaveCompressedProxy.h"
#include "UObject/SavePackage.h"
#if WITH_EDITOR
#include "Misc/ScopedSlowTask.h"
#endif


FDeferredPackageSaveManager::FDeferredPackageSaveManager()
	:bIsSaving(false)
{
}

FDeferredPackageSaveManager::~FDeferredPackageSaveManager()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FDeferredPackageSaveManager::AddPackagesToSave(const TArray<UPackage*>& InPackages)
{
	for (UPackage* Pkg : InPackages)
	{
		if (Pkg) PackagesToSave.Enqueue(Pkg);
	}
	if (!bIsSaving)
	{
		bIsSaving = true;

#if WITH_EDITOR
        SlowTask = MakeUnique<FScopedSlowTask>(InPackages.Num(), FText::FromString(TEXT("Saving Static Mesh Packages...")));
        SlowTask->MakeDialog(true);
#endif
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FDeferredPackageSaveManager::ThickSave));
	}
}

bool FDeferredPackageSaveManager::ThickSave(float DeltaTime)
{
    UPackage* Package = nullptr;
    if (PackagesToSave.Dequeue(Package) && Package)
    {
        FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.SaveFlags = RF_Public | RF_Standalone;

        bool bSuccess = UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs);
        if (!bSuccess)
        {
            UE_LOG(LogFragments, Error, TEXT("Failed to save package: %s"), *Package->GetName());
        }
#if WITH_EDITOR
        if (SlowTask.IsValid())
        {
            SlowTask->EnterProgressFrame(1);

            if (SlowTask->ShouldCancel())
            {
                // User cancelled the save process
                PackagesToSave.Empty();
                bIsSaving = false;
                TickerHandle.Reset();
                SlowTask.Reset();
                return false;
            }
        }
#endif
    }

    // Stop ticking when queue is empty
    if (PackagesToSave.IsEmpty())
    {
        bIsSaving = false;
#if WITH_EDITOR
        SlowTask.Reset();
#endif
        TickerHandle.Reset();
        return false; // unregister ticker
    }

    return true; // keep ticking
}
