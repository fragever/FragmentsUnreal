// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
// Dropping assets onto a material row's thumbnails sets that row's override.
#include "Misc/AutomationTest.h"
#include "Overrides/FragmentMaterialDrop.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Built from a class path, not a loaded object: that is what a Content Browser drag carries for an asset that has not been loaded yet, and the check must not need to load it.
	FAssetData MakeAsset(const TCHAR* Name, const UClass* Class)
	{
		return FAssetData(
			FName(FString::Printf(TEXT("/Game/DropTest/%s"), Name)),
			FName(TEXT("/Game/DropTest")),
			FName(Name),
			Class->GetClassPathName());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFragmentMaterialDropTest,
	"FragmentsUnreal.Overrides.MaterialDrop_AcceptsExactlyOneMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFragmentMaterialDropTest::RunTest(const FString& Parameters)
{
	using namespace UE::Fragments::Overrides;

	const FAssetData Material = MakeAsset(TEXT("M_Brick"), UMaterial::StaticClass());
	const FAssetData Instance = MakeAsset(TEXT("MI_Brick"), UMaterialInstanceConstant::StaticClass());
	const FAssetData Texture  = MakeAsset(TEXT("T_Brick"), UTexture2D::StaticClass());

	TestTrue(TEXT("one material is accepted"), IsAcceptableOverrideDrop({ Material }));
	TestTrue(TEXT("one material instance is accepted"), IsAcceptableOverrideDrop({ Instance }));

	TestFalse(TEXT("a texture is refused"), IsAcceptableOverrideDrop({ Texture }));
	TestFalse(TEXT("two materials are refused -- a row takes one override"),
		IsAcceptableOverrideDrop({ Material, Instance }));
	TestFalse(TEXT("an empty drag is refused"), IsAcceptableOverrideDrop({}));
	TestFalse(TEXT("an invalid asset is refused"), IsAcceptableOverrideDrop({ FAssetData() }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
