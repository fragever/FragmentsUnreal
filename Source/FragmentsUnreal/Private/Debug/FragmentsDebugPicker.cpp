// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Debug/FragmentsDebugPicker.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformFileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentsImporterSubsystem.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "Index/index_generated.h"

// Disambiguate from Unreal's UMaterial — FlatBuffers struct lives at root namespace.
using FBMaterial = ::Material;

AFragmentsDebugPicker::AFragmentsDebugPicker()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AFragmentsDebugPicker::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FragmentsDebugPicker] No PlayerController; binding skipped."));
		return;
	}

	if (bForceShowCursor)
	{
		PC->bShowMouseCursor = true;
		PC->bEnableClickEvents = true;
		PC->bEnableMouseOverEvents = true;
	}

	// EnableInput attaches our UInputComponent to the PC's input stack.
	EnableInput(PC);
	if (InputComponent)
	{
		InputComponent->BindKey(PickKey, IE_Pressed, this, &AFragmentsDebugPicker::OnPickPressed);
	}

	UE_LOG(LogTemp, Display, TEXT("[FragmentsDebugPicker] Press %s over a Fragments mesh. Output: %s"),
		*PickKey.ToString(), *GetOutputFilePath());
}

void AFragmentsDebugPicker::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UE_LOG(LogTemp, Display, TEXT("[FragmentsDebugPicker] Session ended: %d picks saved to %s"),
		ClickCounter, *CachedOutputPath);
	Super::EndPlay(EndPlayReason);
}

void AFragmentsDebugPicker::OnPickPressed()
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) return;

	FHitResult Hit;
	// ECC_Visibility hits anything whose collision response includes Visibility=Block, which HISMCs do by default.
	const bool bHit = PC->GetHitResultUnderCursor(ECC_Visibility, /*bTraceComplex=*/false, Hit);
	if (!bHit || !Hit.GetComponent())
	{
		UE_LOG(LogTemp, Warning, TEXT("[FragmentsDebugPicker] No hit under cursor."));
		return;
	}
	CapturePick(Hit);
}

void AFragmentsDebugPicker::CapturePick(const FHitResult& Hit)
{
	const UHierarchicalInstancedStaticMeshComponent* Hism =
		Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.GetComponent());
	if (!Hism)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FragmentsDebugPicker] Hit was not on a HISMC (Component=%s)."),
			*GetNameSafe(Hit.GetComponent()));
		return;
	}

	AFragment* Fragment = Cast<AFragment>(Hism->GetOwner());
	if (!Fragment)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FragmentsDebugPicker] HISMC owner is %s, not AFragment."),
			*GetNameSafe(Hism->GetOwner()));
		return;
	}

	// Hit.Item is the HISM instance index for instanced static mesh hits.
	int64 LocalId = INDEX_NONE;
	if (!Fragment->GetLocalIdForInstance(Hism, Hit.Item, LocalId) || LocalId == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FragmentsDebugPicker] No LocalId for Instance=%d."), Hit.Item);
		return;
	}

	// Resolve FFragmentItem via the subsystem. OK if null — we still write a partial record.
	const FFragmentItem* Item = nullptr;
	if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UFragmentsImporterSubsystem* Sub = GI->GetSubsystem<UFragmentsImporterSubsystem>())
		{
			Item = Sub->GetFragmentItemByLocalId(LocalId, Fragment->GetModelGuid());
		}
	}

	const FString Record = BuildRecord(Hit, Fragment, LocalId, Item, Hism);
	const FString Line = Record + LINE_TERMINATOR;

	// Append-mode write.
	if (!FFileHelper::SaveStringToFile(
			Line, *GetOutputFilePath(),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_Append))
	{
		UE_LOG(LogTemp, Error, TEXT("[FragmentsDebugPicker] Append failed: %s"), *CachedOutputPath);
		return;
	}

	++ClickCounter;
	UE_LOG(LogTemp, Display, TEXT("[FragmentsDebugPicker] #%d LocalId=%lld %s"),
		ClickCounter, LocalId, *Record.Left(160));
}

FString AFragmentsDebugPicker::GetOutputFilePath()
{
	if (!CachedOutputPath.IsEmpty()) return CachedOutputPath;

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("FragmentsDebug");
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);

	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Tail = OutputSuffix.IsEmpty() ? TEXT("") : (TEXT("_") + OutputSuffix);
	CachedOutputPath = Dir / FString::Printf(TEXT("material_clicks_%s%s.jsonl"), *Stamp, *Tail);
	return CachedOutputPath;
}

// Helpers for BuildRecord.

namespace
{
	// Resolve the source FlatBuffers Material* for one sample.
	const FBMaterial* ResolveSourceMaterial(
		const UFragmentsImporterSubsystem* Sub,
		const FString& ModelGuid,
		int64 MaterialIndex)
	{
		if (!Sub || MaterialIndex < 0) return nullptr;
		UFragmentsImporter* Imp = Sub->GetImporter();
		if (!Imp) return nullptr;
		const UFragmentModelWrapper* Wrapper = Imp->GetFragmentModel(ModelGuid);
		if (!Wrapper) return nullptr;
		const ::Model* Parsed = Wrapper->GetParsedModel();
		if (!Parsed || !Parsed->meshes() || !Parsed->meshes()->materials()) return nullptr;
		const auto* Materials = Parsed->meshes()->materials();
		if (static_cast<uint32>(MaterialIndex) >= Materials->size()) return nullptr;
		return Materials->Get(static_cast<uint32>(MaterialIndex));
	}

	TArray<TSharedPtr<FJsonValue>> Vec3ToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	TArray<TSharedPtr<FJsonValue>> RgbaFloatToJson(const FLinearColor& C)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(C.R));
		Arr.Add(MakeShared<FJsonValueNumber>(C.G));
		Arr.Add(MakeShared<FJsonValueNumber>(C.B));
		Arr.Add(MakeShared<FJsonValueNumber>(C.A));
		return Arr;
	}

	TArray<TSharedPtr<FJsonValue>> RgbaU8ToJson(uint8 R, uint8 G, uint8 B, uint8 A)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(R));
		Arr.Add(MakeShared<FJsonValueNumber>(G));
		Arr.Add(MakeShared<FJsonValueNumber>(B));
		Arr.Add(MakeShared<FJsonValueNumber>(A));
		return Arr;
	}
}

FString AFragmentsDebugPicker::BuildRecord(
	const FHitResult& Hit,
	const AFragment* Fragment,
	int64 LocalId,
	const FFragmentItem* Item,
	const UHierarchicalInstancedStaticMeshComponent* Hism) const
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

	// Identity
	Obj->SetStringField(TEXT("ts"), FDateTime::UtcNow().ToIso8601());
	Obj->SetNumberField(TEXT("click_index"), ClickCounter + 1);
	Obj->SetNumberField(TEXT("local_id"), static_cast<double>(LocalId));
	Obj->SetStringField(TEXT("model_guid"), Fragment ? Fragment->GetModelGuid() : FString());
	Obj->SetStringField(TEXT("category"), Item ? Item->Category : FString(TEXT("(unknown)")));

	// Spatial
	Obj->SetArrayField(TEXT("hit_world"), Vec3ToJson(Hit.ImpactPoint));
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		FVector CamLoc; FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		Obj->SetArrayField(TEXT("camera_loc"), Vec3ToJson(CamLoc));
	}

	// HISMC identity
	Obj->SetStringField(TEXT("hism_component"), GetNameSafe(Hism));
	Obj->SetNumberField(TEXT("instance_index"), Hit.Item);
	Obj->SetStringField(TEXT("hism_mesh"),
		GetNameSafe(Hism ? Hism->GetStaticMesh() : nullptr));

	// Applied material (what we actually rendered with)
	{
		TSharedRef<FJsonObject> Applied = MakeShared<FJsonObject>();
		UMaterialInterface* Mat = Hism ? Hism->GetMaterial(0) : nullptr;
		Applied->SetStringField(TEXT("material_name"), GetNameSafe(Mat));

		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mat))
		{
			UMaterialInterface* Base = MID->Parent;
			Applied->SetStringField(TEXT("base_material"), GetNameSafe(Base));
			// Heuristic: base material name containing "Glass" indicates the glass shader path fired.
			const bool bRoutedAsGlass = Base && Base->GetName().Contains(TEXT("Glass"));
			Applied->SetBoolField(TEXT("is_glass_routed"), bRoutedAsGlass);

			FLinearColor BaseColor;
			if (MID->GetVectorParameterValue(FMaterialParameterInfo(TEXT("BaseColor")), BaseColor))
			{
				Applied->SetArrayField(TEXT("base_color_rgba_float"), RgbaFloatToJson(BaseColor));
			}
			float Opacity = 0.f;
			if (MID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Opacity")), Opacity))
			{
				Applied->SetNumberField(TEXT("opacity"), Opacity);
			}
		}
		Obj->SetObjectField(TEXT("applied"), Applied);
	}

	// Source samples (what the .frag says this element should be)
	if (Item && Fragment)
	{
		UFragmentsImporterSubsystem* Sub =
			GetWorld() && GetWorld()->GetGameInstance()
				? GetWorld()->GetGameInstance()->GetSubsystem<UFragmentsImporterSubsystem>()
				: nullptr;

		TArray<TSharedPtr<FJsonValue>> SamplesJson;
		for (const FFragmentSample& Sample : Item->Samples)
		{
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetNumberField(TEXT("sample_index"), static_cast<double>(Sample.SampleIndex));
			S->SetNumberField(TEXT("representation_index"), static_cast<double>(Sample.RepresentationIndex));
			S->SetNumberField(TEXT("material_index"), static_cast<double>(Sample.MaterialIndex));

			if (const FBMaterial* Src = ResolveSourceMaterial(Sub, Fragment->GetModelGuid(), Sample.MaterialIndex))
			{
				S->SetArrayField(TEXT("src_rgba_u8"),
					RgbaU8ToJson(Src->r(), Src->g(), Src->b(), Src->a()));
				S->SetNumberField(TEXT("src_rendered_faces"), static_cast<int32>(Src->rendered_faces()));
				S->SetNumberField(TEXT("src_stroke"), static_cast<int32>(Src->stroke()));
				S->SetBoolField(TEXT("src_implies_glass"), Src->a() < 255);
			}
			SamplesJson.Add(MakeShared<FJsonValueObject>(S));
		}
		Obj->SetArrayField(TEXT("source_samples"), SamplesJson);
	}

	// Single-line JSON for JSONL output.
	FString Out;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}
