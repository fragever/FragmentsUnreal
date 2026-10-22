// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Import/FragmentsSlateImportProgressSink.h"

#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "FragmentsImportProgress"

FSlateImportProgressSink::~FSlateImportProgressSink()
{
	// Defensive only -- the tracker is contracted to Cancel every entry on its own teardown, and ImportProgressTests.cpp pins that against a recording fake.
	if (FSlateApplication::IsInitialized())
	{
		for (const TPair<int32, FProgressNotificationHandle>& Pair : Handles)
		{
			FSlateNotificationManager::Get().CancelProgressNotification(Pair.Value);
		}
	}
	Handles.Empty();
}

int32 FSlateImportProgressSink::StartProgress(const FText& DisplayText, int32 TotalWorkToDo)
{
	// StartProgressNotification is a no-op unless an IProgressNotificationHandler is installed (UStatusBarSubsystem installs the only one), and it ignores any task with TotalWorkToDo <= 0.
	if (!FSlateApplication::IsInitialized() || TotalWorkToDo <= 0)
	{
		return 0;
	}

	const FProgressNotificationHandle Handle =
		FSlateNotificationManager::Get().StartProgressNotification(DisplayText, TotalWorkToDo);

	if (!Handle.IsValid())
	{
		// The progress handler is unavailable during early startup or non-Slate runs.
		return 0;
	}

	const int32 SinkId = ++NextSinkId;
	Handles.Add(SinkId, Handle);
	return SinkId;
}

void FSlateImportProgressSink::UpdateProgress(
	int32 SinkId, int32 TotalWorkDone, int32 UpdatedTotalWorkToDo, const FText& UpdatedDisplayText)
{
	if (const FProgressNotificationHandle* Handle = Handles.Find(SinkId))
	{
		// TotalWorkDone is absolute, not incremental -- the engine clamps it into [0, TotalWorkToDo] itself.
		FSlateNotificationManager::Get().UpdateProgressNotification(
			*Handle, TotalWorkDone, UpdatedTotalWorkToDo, UpdatedDisplayText);
	}
}

void FSlateImportProgressSink::CancelProgress(int32 SinkId)
{
	FProgressNotificationHandle Handle;
	if (Handles.RemoveAndCopyValue(SinkId, Handle))
	{
		// "Cancel" is the engine's only way to end a progress entry; it carries no outcome.
		FSlateNotificationManager::Get().CancelProgressNotification(Handle);
	}
}

void FSlateImportProgressSink::ShowFailure(const FText& Title, const FText& Detail)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(Title);
	Info.SubText = Detail;

	// bFireAndForget=false: a failure must not time out unread. The user dismisses it.
	Info.bFireAndForget = false;
	Info.bUseSuccessFailIcons = true;
	Info.bUseThrobber = false;

	// Set clipboard text before adding its hyperlink.
	Info.ShowCopyToClipboadHyperlink();

	// Build the dismiss button before adding the notification.
	TSharedRef<TWeakPtr<SNotificationItem>> WeakItemBox = MakeShared<TWeakPtr<SNotificationItem>>();

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("DismissImportFailure", "Dismiss"),
		LOCTEXT("DismissImportFailureTooltip", "Close this message."),
		FSimpleDelegate::CreateLambda([WeakItemBox]()
		{
			if (TSharedPtr<SNotificationItem> Pinned = WeakItemBox->Pin())
			{
				Pinned->SetFadeOutDuration(0.25f);
				Pinned->Fadeout();
			}
		}),
		// Visible only in CS_Fail, which is the state set immediately below.
		SNotificationItem::CS_Fail));

	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (!Item.IsValid())
	{
		return;
	}

	*WeakItemBox = Item;
	Item->SetCompletionState(SNotificationItem::CS_Fail);
}

void FSlateImportProgressSink::ShowNotice(const FText& Title, const FText& Detail)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	// The user asked for this, so it informs and goes: fire-and-forget, no failure styling.
	FNotificationInfo Info(Title);
	Info.SubText = Detail;
	Info.bFireAndForget = true;
	Info.ExpireDuration = 4.f;
	Info.bUseSuccessFailIcons = false;
	FSlateNotificationManager::Get().AddNotification(Info);
}

#undef LOCTEXT_NAMESPACE
