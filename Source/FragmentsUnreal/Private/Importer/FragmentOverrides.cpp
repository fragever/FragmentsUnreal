// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Importer/FragmentOverrides.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentOverrides, Log, All);

static bool LoadOverridesFromFile(const FString& FilePath, FFragmentOverrides& Out)
{
    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        UE_LOG(LogFragmentOverrides, Warning, TEXT("fragmentOverrides JSON failed to parse: %s"), *FilePath);
        return false;
    }

    Out = FFragmentOverrides();
    RootObject->TryGetNumberField(TEXT("Version"), Out.Version);
    RootObject->TryGetStringField(TEXT("ModelGuid"), Out.ModelGuid);
    RootObject->TryGetStringField(TEXT("FragmentPath"), Out.FragmentPath);

    if (Out.Version > 3)
    {
        UE_LOG(LogFragmentOverrides, Warning,
            TEXT("fragmentOverrides version %d is newer than supported version 3; ignoring overrides for '%s'"),
            Out.Version, *Out.ModelGuid);
        Out = FFragmentOverrides();
        return false;
    }

	const TArray<TSharedPtr<FJsonValue>>* OverridesArray = nullptr;

	const TSharedPtr<FJsonObject>* MaterialsObj = nullptr;
	if (RootObject->TryGetObjectField(TEXT("Materials"), MaterialsObj) && MaterialsObj && MaterialsObj->IsValid())
	{
		(*MaterialsObj)->TryGetArrayField(TEXT("Overrides"), OverridesArray);
	}
	else
	{
		RootObject->TryGetArrayField(TEXT("Overrides"), OverridesArray);
	}

	if (!OverridesArray)
	{
		return true;
	}

    for (const TSharedPtr<FJsonValue>& EntryValue : *OverridesArray)
    {
        const TSharedPtr<FJsonObject> EntryObject = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
        if (!EntryObject.IsValid()) continue;

        FFragmentMaterialOverrideEntry Entry;
        EntryObject->TryGetStringField(TEXT("SourceKey"), Entry.SourceKey);
        EntryObject->TryGetStringField(TEXT("ReplacementMaterial"), Entry.ReplacementMaterialPath);
        EntryObject->TryGetStringField(TEXT("SourceMaterialName"), Entry.SourceMaterialName);
        EntryObject->TryGetStringField(TEXT("SourceMaterialAsset"), Entry.SourceMaterialAsset);
        EntryObject->TryGetNumberField(TEXT("UsageCount"), Entry.UsageCount);
        EntryObject->TryGetBoolField(TEXT("SourceIsGlass"), Entry.bSourceIsGlass);

        const TSharedPtr<FJsonObject>* ColorObj = nullptr;
        if (EntryObject->TryGetObjectField(TEXT("SourceColor"), ColorObj) && ColorObj && ColorObj->IsValid())
        {
            double R = 0, G = 0, B = 0, A = 1;
            (*ColorObj)->TryGetNumberField(TEXT("R"), R);
            (*ColorObj)->TryGetNumberField(TEXT("G"), G);
            (*ColorObj)->TryGetNumberField(TEXT("B"), B);
            (*ColorObj)->TryGetNumberField(TEXT("A"), A);
            Entry.SourceColor = FLinearColor((float)R, (float)G, (float)B, (float)A);
            Entry.bHasSourceColor = true;
        }

        if (Entry.SourceKey.IsEmpty()) continue;
        Out.MaterialOverrides.Add(MoveTemp(Entry));
    }

    // Placement block (v3+). Absent in v1/v2 — Out.Placement.bHasValue stays false.
    const TSharedPtr<FJsonObject>* PlacementObj = nullptr;
    if (RootObject->TryGetObjectField(TEXT("Placement"), PlacementObj)
        && PlacementObj && PlacementObj->IsValid())
    {
        const TSharedPtr<FJsonObject>* LocObj = nullptr;
        const TSharedPtr<FJsonObject>* RotObj = nullptr;
        if ((*PlacementObj)->TryGetObjectField(TEXT("Location"), LocObj)
            && LocObj && LocObj->IsValid()
            && (*PlacementObj)->TryGetObjectField(TEXT("Rotation"), RotObj)
            && RotObj && RotObj->IsValid())
        {
            double X = 0.0, Y = 0.0, Z = 0.0, Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
            (*LocObj)->TryGetNumberField(TEXT("X"), X);
            (*LocObj)->TryGetNumberField(TEXT("Y"), Y);
            (*LocObj)->TryGetNumberField(TEXT("Z"), Z);
            (*RotObj)->TryGetNumberField(TEXT("Pitch"), Pitch);
            (*RotObj)->TryGetNumberField(TEXT("Yaw"), Yaw);
            (*RotObj)->TryGetNumberField(TEXT("Roll"), Roll);
            Out.Placement.Location = FVector(X, Y, Z);
            Out.Placement.Rotation = FRotator(Pitch, Yaw, Roll);
            Out.Placement.bHasValue = true;
        }
    }

    return true;
}

namespace UE::Fragments::Overrides
{
    FString GetSidecarPath(const FString& FragPath)
    {
        return FragPath + TEXT(".fragmentOverrides.json");
    }

    FString BuildSourceKey(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass)
    {
        return FString::Printf(TEXT("rgba_%d_%d_%d_%d_glass_%d"), R, G, B, A, bIsGlass ? 1 : 0);
    }

    bool Load(const FString& FragPath, const FString& ModelGuid, FFragmentOverrides& Out)
    {
        Out = FFragmentOverrides();

        const FString SidecarPath = GetSidecarPath(FragPath);
        if (FPaths::FileExists(SidecarPath))
        {
            return LoadOverridesFromFile(SidecarPath, Out);
        }

        if (!ModelGuid.IsEmpty())
        {
            const FString LegacyPath = FPaths::ProjectContentDir()
                / TEXT("Buildings") / ModelGuid / TEXT("FragmentsMaterialOverrides.json");
            if (FPaths::FileExists(LegacyPath))
            {
                const bool bOk = LoadOverridesFromFile(LegacyPath, Out);
                if (bOk)
                {
                    UE_LOG(LogFragmentOverrides, Log,
                        TEXT("fragmentOverrides loaded from legacy path '%s'; will migrate on next edit"),
                        *LegacyPath);
                }
                return bOk;
            }
        }

        return false;
    }

    bool Save(const FString& FragPath, const FFragmentOverrides& In)
    {
        const FString FilePath = GetSidecarPath(FragPath);

        const FString DirPath = FPaths::GetPath(FilePath);
        if (!DirPath.IsEmpty() && !FPaths::DirectoryExists(DirPath))
        {
            if (!IFileManager::Get().MakeDirectory(*DirPath, true))
            {
                UE_LOG(LogFragmentOverrides, Warning, TEXT("fragmentOverrides: could not create directory '%s'"), *DirPath);
                return false;
            }
        }

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("Version"), 3);
        Root->SetStringField(TEXT("ModelGuid"), In.ModelGuid);
        Root->SetStringField(TEXT("FragmentPath"), In.FragmentPath);

        TSharedRef<FJsonObject> Materials = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> EntriesArray;
        EntriesArray.Reserve(In.MaterialOverrides.Num());

        for (const FFragmentMaterialOverrideEntry& Entry : In.MaterialOverrides)
        {
            if (Entry.SourceKey.IsEmpty() || Entry.ReplacementMaterialPath.IsEmpty()) continue;

            TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
            EntryObj->SetStringField(TEXT("SourceKey"), Entry.SourceKey);
            EntryObj->SetStringField(TEXT("SourceMaterialName"), Entry.SourceMaterialName);
            EntryObj->SetStringField(TEXT("SourceMaterialAsset"), Entry.SourceMaterialAsset);
            EntryObj->SetStringField(TEXT("ReplacementMaterial"), Entry.ReplacementMaterialPath);
            EntryObj->SetNumberField(TEXT("UsageCount"), Entry.UsageCount);
            EntryObj->SetBoolField(TEXT("SourceIsGlass"), Entry.bSourceIsGlass);

            if (Entry.bHasSourceColor)
            {
                TSharedRef<FJsonObject> ColorObj = MakeShared<FJsonObject>();
                ColorObj->SetNumberField(TEXT("R"), Entry.SourceColor.R);
                ColorObj->SetNumberField(TEXT("G"), Entry.SourceColor.G);
                ColorObj->SetNumberField(TEXT("B"), Entry.SourceColor.B);
                ColorObj->SetNumberField(TEXT("A"), Entry.SourceColor.A);
                EntryObj->SetObjectField(TEXT("SourceColor"), ColorObj);
            }

            EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
        }

        Materials->SetArrayField(TEXT("Overrides"), EntriesArray);
        Root->SetObjectField(TEXT("Materials"), Materials);

        if (In.Placement.bHasValue)
        {
            TSharedRef<FJsonObject> PlacementObj = MakeShared<FJsonObject>();

            TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
            LocObj->SetNumberField(TEXT("X"), In.Placement.Location.X);
            LocObj->SetNumberField(TEXT("Y"), In.Placement.Location.Y);
            LocObj->SetNumberField(TEXT("Z"), In.Placement.Location.Z);
            PlacementObj->SetObjectField(TEXT("Location"), LocObj);

            TSharedRef<FJsonObject> RotObj = MakeShared<FJsonObject>();
            RotObj->SetNumberField(TEXT("Pitch"), In.Placement.Rotation.Pitch);
            RotObj->SetNumberField(TEXT("Yaw"), In.Placement.Rotation.Yaw);
            RotObj->SetNumberField(TEXT("Roll"), In.Placement.Rotation.Roll);
            PlacementObj->SetObjectField(TEXT("Rotation"), RotObj);

            Root->SetObjectField(TEXT("Placement"), PlacementObj);
        }

        FString OutText;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            UE_LOG(LogFragmentOverrides, Warning, TEXT("fragmentOverrides: serialization failed for '%s'"), *FilePath);
            return false;
        }

        if (!FFileHelper::SaveStringToFile(OutText, *FilePath))
        {
            UE_LOG(LogFragmentOverrides, Warning, TEXT("fragmentOverrides: write failed for '%s'"), *FilePath);
            return false;
        }

        return true;
    }

    bool Update(const FString& FragPath, const FString& ModelGuid,
        TFunctionRef<void(FFragmentOverrides&)> Mutator)
    {
        FFragmentOverrides Existing;
        Load(FragPath, ModelGuid, Existing);
        Existing.ModelGuid = ModelGuid;
        Existing.FragmentPath = FragPath;
        Mutator(Existing);
        return Save(FragPath, Existing);
    }
}  // namespace UE::Fragments::Overrides
