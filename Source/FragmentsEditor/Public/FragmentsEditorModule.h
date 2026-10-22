// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FFragmentsEditor : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static const FName FragmentsPanelTabName;

private:
	TSharedRef<class SDockTab> SpawnFragmentsPanelTab(const class FSpawnTabArgs& Args);
};
