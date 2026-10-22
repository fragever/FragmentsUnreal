// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

// This header stays private because it depends on private Slate modules.
#include "CoreMinimal.h"
#include "Import/FragmentsImportProgressSink.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

/** The real sink. */
class FSlateImportProgressSink : public IFragmentsImportProgressSink
{
public:
	/** Defensive teardown: cancels any engine progress rows still open so a module unload or a mid-session sink swap never leaves a permanently stuck status-bar entry. */
	virtual ~FSlateImportProgressSink() override;

	virtual int32 StartProgress(const FText& DisplayText, int32 TotalWorkToDo) override;
	virtual void UpdateProgress(int32 SinkId, int32 TotalWorkDone, int32 UpdatedTotalWorkToDo,
		const FText& UpdatedDisplayText) override;
	virtual void CancelProgress(int32 SinkId) override;
	virtual void ShowFailure(const FText& Title, const FText& Detail) override;
	virtual void ShowNotice(const FText& Title, const FText& Detail) override;

private:
	/** Our opaque ids -> engine handles. */
	TMap<int32, FProgressNotificationHandle> Handles;
	int32 NextSinkId = 0;
};
