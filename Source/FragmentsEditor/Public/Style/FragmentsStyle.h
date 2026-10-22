// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

class FFragmentsStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	// Brand colors (linear space, converted from sRGB hex)
	static const FLinearColor Primary;    // #101014
	static const FLinearColor Secondary;  // #16161C
	static const FLinearColor Tertiary;   // #1C1C24
	static const FLinearColor Elevated;   // #24242E
	static const FLinearColor Border;     // #2C2C36
	static const FLinearColor ButtonBlue; // #5B8CFF
	static const FLinearColor White;

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
