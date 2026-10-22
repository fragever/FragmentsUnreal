// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Style/FragmentsStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/Texture2D.h"
#include "UObject/GCObject.h"

// GC guard — FSlateStyleSet is plain C++, so the GC never walks it.
class FFragmentsStyleGCGuard : public FGCObject
{
public:
	TArray<TObjectPtr<UObject>> ResourceObjects;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObjects(ResourceObjects);
	}

	virtual FString GetReferencerName() const override
	{
		return TEXT("FFragmentsStyle");
	}
};

static TUniquePtr<FFragmentsStyleGCGuard> GFontGCGuard;

// Icon brush — Slate resolves asset paths starting with '/' via the content system at runtime, so this works even if the texture is added after cook.
#define FRAGMENTS_ICON_BRUSH(AssetName, Size) \
	MakeIconBrush(TEXT("/FragmentsUnreal/Resources/UI/") AssetName TEXT(".") AssetName, Size)

// Color constants (FColor RGBA -> FLinearColor applies sRGB->linear).
const FLinearColor FFragmentsStyle::Primary    = FLinearColor(FColor(0x10, 0x10, 0x14));
const FLinearColor FFragmentsStyle::Secondary  = FLinearColor(FColor(0x16, 0x16, 0x1C));
const FLinearColor FFragmentsStyle::Tertiary   = FLinearColor(FColor(0x1C, 0x1C, 0x24));
const FLinearColor FFragmentsStyle::Elevated   = FLinearColor(FColor(0x24, 0x24, 0x2E));
const FLinearColor FFragmentsStyle::Border     = FLinearColor(FColor(0x2C, 0x2C, 0x36));
const FLinearColor FFragmentsStyle::ButtonBlue = FLinearColor(FColor(0x5B, 0x8C, 0xFF));
const FLinearColor FFragmentsStyle::White      = FLinearColor::White;

TSharedPtr<FSlateStyleSet> FFragmentsStyle::StyleInstance = nullptr;

void FFragmentsStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		GFontGCGuard = MakeUnique<FFragmentsStyleGCGuard>();
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FFragmentsStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		ensure(StyleInstance.IsUnique());
		StyleInstance.Reset();
	}

	// Releasing the guard lets GC reclaim the font face objects normally
	GFontGCGuard.Reset();
}

const ISlateStyle& FFragmentsStyle::Get()         { return *StyleInstance; }
FName              FFragmentsStyle::GetStyleSetName()
{
	static FName Name(TEXT("FragmentsStyle"));
	return Name;
}

// Load a UFontFace asset by content path, register it with the GC guard, and return a FSlateFontInfo wrapping it.
static FSlateFontInfo MakeFontInfo(const TCHAR* FontAssetPath, const TCHAR* FontFaceAssetPath, int32 Size)
{
	UObject* FontObject = LoadObject<UFont>(nullptr, FontAssetPath);
	if (!FontObject)
	{
		FontObject = LoadObject<UFontFace>(nullptr, FontFaceAssetPath);
	}

	if (FontObject)
	{
		GFontGCGuard->ResourceObjects.AddUnique(FontObject);
		return FSlateFontInfo(FontObject, Size);
	}

	UE_LOG(LogTemp, Warning,
		TEXT("FFragmentsStyle: failed to load font assets '%s' and '%s', falling back to engine default."),
		FontAssetPath,
		FontFaceAssetPath);
	return FCoreStyle::GetDefaultFontStyle("Regular", Size);
}

static FSlateBrush* MakeIconBrush(const TCHAR* TextureAssetPath, const FVector2D& Size)
{
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, TextureAssetPath);
	if (Texture)
	{
		GFontGCGuard->ResourceObjects.AddUnique(Texture);
		return new FSlateImageBrush(Texture, Size);
	}

	UE_LOG(LogTemp, Warning,
		TEXT("FFragmentsStyle: failed to load icon texture '%s'."),
		TextureAssetPath);
	return new FSlateColorBrush(FLinearColor::Transparent);
}

static FSlateBrush* MakeOptionalIconBrush(const TCHAR* TextureAssetPath, const FVector2D& Size)
{
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, TextureAssetPath);
	if (Texture)
	{
		GFontGCGuard->ResourceObjects.AddUnique(Texture);
		return new FSlateImageBrush(Texture, Size);
	}

	return new FSlateColorBrush(FLinearColor::Transparent);
}

TSharedRef<FSlateStyleSet> FFragmentsStyle::Create()
{
	TSharedRef<FSlateStyleSet> StyleSet = MakeShareable(new FSlateStyleSet(GetStyleSetName()));
	StyleSet->SetContentRoot(TEXT("/FragmentsUnreal/Resources/UI"));

	// Fonts (UFontFace assets imported into the plugin)
	const FSlateFontInfo HeadingFont = MakeFontInfo(
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Bold_Font.FiraCode-Bold_Font"),
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Bold.FiraCode-Bold"),
		14);
	const FSlateFontInfo BodyFont = MakeFontInfo(
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Regular_Font.FiraCode-Regular_Font"),
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Regular.FiraCode-Regular"),
		10);
	const FSlateFontInfo ButtonFont = MakeFontInfo(
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-SemiBold_Font.FiraCode-SemiBold_Font"),
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-SemiBold.FiraCode-SemiBold"),
		10);
	const FSlateFontInfo DescriptionFont = MakeFontInfo(
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Regular_Font.FiraCode-Regular_Font"),
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Regular.FiraCode-Regular"),
		9);
	const FSlateFontInfo SmallFont = MakeFontInfo(
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Regular_Font.FiraCode-Regular_Font"),
		TEXT("/FragmentsUnreal/Resources/Fonts/FiraCode-Regular.FiraCode-Regular"),
		8);

	// Register fonts in the FontInfos map so ISlateStyle::GetFontStyle() can find them by name.
	StyleSet->Set("Fragments.Text.Heading",     HeadingFont);
	StyleSet->Set("Fragments.Text.Body",        BodyFont);
	StyleSet->Set("Fragments.Text.Button",      ButtonFont);
	StyleSet->Set("Fragments.Text.Description", DescriptionFont);
	StyleSet->Set("Fragments.Text.Small",       SmallFont);

	// Text block styles (WidgetStyleValues map) -------------------------- STextBlock::TextStyle() reads from this map and applies font + color in one call — no need to set .Font() separately on every text block.
	const FSlateColor TextWhite(White);

	StyleSet->Set("Fragments.Text.Heading",
		FTextBlockStyle().SetFont(HeadingFont).SetColorAndOpacity(TextWhite));

	StyleSet->Set("Fragments.Text.Body",
		FTextBlockStyle().SetFont(BodyFont).SetColorAndOpacity(TextWhite));

	StyleSet->Set("Fragments.Text.Button",
		FTextBlockStyle().SetFont(ButtonFont).SetColorAndOpacity(TextWhite));

	StyleSet->Set("Fragments.Text.Description",
		FTextBlockStyle().SetFont(DescriptionFont).SetColorAndOpacity(TextWhite));

	StyleSet->Set("Fragments.Text.Small",
		FTextBlockStyle().SetFont(SmallFont).SetColorAndOpacity(TextWhite));

	// Background color brushes
	StyleSet->Set("Fragments.Background.Primary",   new FSlateColorBrush(Primary));
	StyleSet->Set("Fragments.Background.Secondary", new FSlateColorBrush(Secondary));
	StyleSet->Set("Fragments.Background.Tertiary",  new FSlateColorBrush(Tertiary));
	StyleSet->Set("Fragments.Background.Elevated",  new FSlateColorBrush(Elevated));
	StyleSet->Set("Fragments.Background.Border",    new FSlateColorBrush(Border));

	// Model-row status dot (white, tinted per state) and the count badge on the filter funnel.
	StyleSet->Set("Fragments.Dot",   new FSlateRoundedBoxBrush(FLinearColor::White, 4.f, FVector2D(8.f, 8.f)));
	StyleSet->Set("Fragments.Badge", new FSlateRoundedBoxBrush(ButtonBlue, 6.f));

	// The engine's own vertical "more options" dots, so the row menu reads like UE's.
	StyleSet->Set("Fragments.Icon.More", new FSlateVectorImageBrush(
		FPaths::EngineContentDir() / TEXT("Slate/Starship/Common/ellipsis-vertical-narrow.svg"), FVector2D(6.f, 24.f)));
	StyleSet->Set("Fragments.Material.ThumbnailFrame", new FSlateRoundedBoxBrush(Elevated, 64.f, Border, 2.f));

	// Text input style
	{
		const FSlateBrush InputBg = FSlateColorBrush(Tertiary);

		FEditableTextBoxStyle InputStyle;
		InputStyle.SetFont(BodyFont);
		InputStyle.SetForegroundColor(TextWhite);
		InputStyle.SetBackgroundImageNormal  (InputBg);
		InputStyle.SetBackgroundImageHovered (InputBg);
		InputStyle.SetBackgroundImageFocused (InputBg);
		InputStyle.SetBackgroundImageReadOnly(InputBg);
		InputStyle.SetPadding(FMargin(8.f, 4.f));

		StyleSet->Set("Fragments.Input", InputStyle);
	}

	// Button style
	{
		const FLinearColor BtnHover(
			FMath::Min(ButtonBlue.R + 0.08f, 1.f),
			FMath::Min(ButtonBlue.G + 0.08f, 1.f),
			FMath::Min(ButtonBlue.B + 0.08f, 1.f));
		const FLinearColor BtnPressed(ButtonBlue.R * 0.85f, ButtonBlue.G * 0.85f, ButtonBlue.B * 0.85f);

		FButtonStyle ButtonStyle;
		ButtonStyle.SetNormal  (FSlateColorBrush(ButtonBlue));
		ButtonStyle.SetHovered (FSlateColorBrush(BtnHover));
		ButtonStyle.SetPressed (FSlateColorBrush(BtnPressed));
		ButtonStyle.SetDisabled(FSlateColorBrush(FLinearColor(Border)));
		ButtonStyle.SetNormalPadding (FMargin(12.f, 6.f));
		ButtonStyle.SetPressedPadding(FMargin(12.f, 7.f, 12.f, 5.f));
		ButtonStyle.NormalForeground  = TextWhite;
		ButtonStyle.HoveredForeground = TextWhite;
		ButtonStyle.PressedForeground = TextWhite;

		StyleSet->Set("Fragments.Button", ButtonStyle);
	}

	// Button-with-icon style
	{
		const FLinearColor BtnHover(
			FMath::Min(ButtonBlue.R + 0.08f, 1.f),
			FMath::Min(ButtonBlue.G + 0.08f, 1.f),
			FMath::Min(ButtonBlue.B + 0.08f, 1.f));
		const FLinearColor BtnPressed(ButtonBlue.R * 0.85f, ButtonBlue.G * 0.85f, ButtonBlue.B * 0.85f);

		FButtonStyle ButtonIconStyle;
		ButtonIconStyle.SetNormal  (FSlateColorBrush(ButtonBlue));
		ButtonIconStyle.SetHovered (FSlateColorBrush(BtnHover));
		ButtonIconStyle.SetPressed (FSlateColorBrush(BtnPressed));
		ButtonIconStyle.SetDisabled(FSlateColorBrush(FLinearColor(Border)));
		ButtonIconStyle.SetNormalPadding (FMargin(0.f));
		ButtonIconStyle.SetPressedPadding(FMargin(0.f));
		ButtonIconStyle.NormalForeground  = TextWhite;
		ButtonIconStyle.HoveredForeground = TextWhite;
		ButtonIconStyle.PressedForeground = TextWhite;

		StyleSet->Set("Fragments.Button.WithIcon", ButtonIconStyle);
	}

	// Icons (UTexture2D assets in Content/Resources/UI/)
	const FVector2D Icon16(16.f, 16.f);
	const FVector2D Icon24(24.f, 24.f);
	const FVector2D Icon32(32.f, 32.f);

	StyleSet->Set("Fragments.Icon.Add",    FRAGMENTS_ICON_BRUSH("cube-01",    Icon16));
	StyleSet->Set("Fragments.Icon.Delete", FRAGMENTS_ICON_BRUSH("trash-01", Icon16));
	StyleSet->Set("Fragments.Icon.Edit",   FRAGMENTS_ICON_BRUSH("edit-02",   Icon16));
	StyleSet->Set("Fragments.Icon.Import", FRAGMENTS_ICON_BRUSH("plus-square", Icon24));
	StyleSet->Set("Fragments.Icon.Reimport", FRAGMENTS_ICON_BRUSH("import-icon", Icon16));
	StyleSet->Set("Fragments.Icon.Revert",   FRAGMENTS_ICON_BRUSH("revert-icon", Icon16));
	StyleSet->Set("Fragments.Icon.Close",  FRAGMENTS_ICON_BRUSH("x-close",  Icon16));
	StyleSet->Set("Fragments.Icon.Logo",   FRAGMENTS_ICON_BRUSH("logo",   Icon32));
	StyleSet->Set("Fragments.Icon.ArrowCircleBrokenLeft",
		MakeOptionalIconBrush(TEXT("/FragmentsUnreal/Resources/UI/arrow-circle-broken-left.arrow-circle-broken-left"), Icon16));
	StyleSet->Set("Fragments.Icon.ChevronDown",
		MakeOptionalIconBrush(TEXT("/FragmentsUnreal/Resources/UI/chevron-down.chevron-down"), Icon16));

	return StyleSet;
}

#undef FRAGMENTS_ICON_BRUSH
