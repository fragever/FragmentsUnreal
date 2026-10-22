// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"

/** The five UI operations import progress needs, with NO Slate types in the signatures. */
class IFragmentsImportProgressSink
{
public:
	virtual ~IFragmentsImportProgressSink() = default;

	/** @return an opaque non-zero id, or 0 if no notification could be created. */
	virtual int32 StartProgress(const FText& DisplayText, int32 TotalWorkToDo) = 0;

	/** TotalWorkDone is absolute, not incremental. */
	virtual void UpdateProgress(int32 SinkId, int32 TotalWorkDone, int32 UpdatedTotalWorkToDo,
		const FText& UpdatedDisplayText) = 0;

	/** Ends the entry with no visible outcome. */
	virtual void CancelProgress(int32 SinkId) = 0;

	/** A persistent failure notification, independent of any progress entry -- it must work for the synchronous failures that never opened one. */
	virtual void ShowFailure(const FText& Title, const FText& Detail) = 0;

	/** A short informational notification that dismisses itself -- e.g. "3 imports cancelled". */
	virtual void ShowNotice(const FText& Title, const FText& Detail) = 0;
};
