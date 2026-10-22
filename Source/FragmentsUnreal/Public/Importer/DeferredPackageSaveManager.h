// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#pragma once

#include "CoreMinimal.h"

class FDeferredPackageSaveManager
{
public:
	FDeferredPackageSaveManager();
	~FDeferredPackageSaveManager();

	void AddPackagesToSave(const TArray<UPackage*>& InPackages);
	bool IsSaving() const { return bIsSaving; }

private:
	bool ThickSave(float DeltaTime);
	TQueue<UPackage*> PackagesToSave;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bIsSaving = false;
#if WITH_EDITOR
	TUniquePtr<FScopedSlowTask> SlowTask;
#endif
};
