// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Debug/FragmentLODDebugComponent.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragLODDebug, Log, All);

static TAutoConsoleVariable<int32> CVarLODDebugVerbose(
	TEXT("r.Fragments.LODDebug.Verbose"), 0,
	TEXT("Per-instance HISMC coloring diagnostics. 1 = log per-HISMC state and "
	     "first few instance writes during ApplyHISMCColors."),
	ECVF_Default);

/** Sink for r.Fragments.LODDebug.Mode — drive every live debug component at once. */
static void ApplyLODDebugModeCVar(IConsoleVariable* Var)
{
	if (!Var) { return; }

	const int32 Raw = Var->GetInt();
	if (Raw < 0)
	{
		// Negative = "no global override": leave each component's own Mode alone.
		return;
	}

	const int32 MaxMode = static_cast<int32>(EFragmentLODDebugMode::AllLayers);
	const EFragmentLODDebugMode NewMode =
		static_cast<EFragmentLODDebugMode>(FMath::Clamp(Raw, 0, MaxMode));

	int32 Applied = 0;
	for (TObjectIterator<UFragmentLODDebugComponent> It; It; ++It)
	{
		UFragmentLODDebugComponent* Comp = *It;
		// Skip the class default object and anything mid-teardown — SetMode walks the owner's components and swaps materials, which neither can service.
		if (!IsValid(Comp) || Comp->IsTemplate()) { continue; }
		if (!Comp->GetOwner()) { continue; }

		Comp->SetMode(NewMode);
		++Applied;
	}

	// Loud on zero: the overwhelmingly likely cause is that AutoAttach was not set before the import, so no components exist to drive.
	if (Applied == 0)
	{
		UE_LOG(LogFragLODDebug, Warning,
			TEXT("r.Fragments.LODDebug.Mode=%d applied to 0 components. Set "
			     "r.Fragments.LODDebug.AutoAttach 1 BEFORE importing — the component is "
			     "attached at model spawn, so toggling AutoAttach afterwards does nothing "
			     "for models that are already loaded."), Raw);
	}
	else
	{
		UE_LOG(LogFragLODDebug, Log,
			TEXT("r.Fragments.LODDebug.Mode=%d applied to %d component(s)."), Raw, Applied);
	}
}

static TAutoConsoleVariable<int32> CVarLODDebugMode(
	TEXT("r.Fragments.LODDebug.Mode"), -1,
	TEXT("Set EVERY attached UFragmentLODDebugComponent's mode at once, and re-color "
	     "immediately. Requires r.Fragments.LODDebug.AutoAttach 1 at import time.\n"
	     "  -1 = no global override (default; each component keeps its own Details setting)\n"
	     "   0 = Disabled (restores original materials — this is the 'off' switch)\n"
	     "   1 = LOD2 proxy only (uniform yellow)\n"
	     "   2 = LOD1 cells only (per-cell colors)\n"
	     "   3 = LOD0 HISMCs only (per-instance colors)\n"
	     "   4 = All layers — proxy + cells + HISMCs, distinguishable by saturation/value,\n"
	     "       so an LOD2 -> LOD1 -> LOD0 transition is visible as the camera approaches.\n"
	     "Re-issue the command after loading more models: components that attach later are "
	     "not retro-colored. Pairs with r.Fragments.LODDebug.ForceAllToLOD1."),
	FConsoleVariableDelegate::CreateStatic(&ApplyLODDebugModeCVar),
	ECVF_Default);

namespace
{
	const TCHAR* const ProxyTag         = TEXT("HLODProxy");
	const TCHAR* const CellMergeTag     = TEXT("FragCellMerge");
	const TCHAR* const BaseMaterialPath = TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial");

	// Per-instance debug material — author manually in the editor before LOD0_HISMCs or AllLayers modes will tint individual instances.
	const TCHAR* const PerInstanceMaterialPath = TEXT("/FragmentsUnreal/Materials/M_FragmentDebugPerInstanceColor.M_FragmentDebugPerInstanceColor");
}

UFragmentLODDebugComponent::UFragmentLODDebugComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
}

void UFragmentLODDebugComponent::SetMode(EFragmentLODDebugMode NewMode)
{
	Mode = NewMode;
	Apply();
}

void UFragmentLODDebugComponent::Apply()
{
	Clear();

	if (Mode == EFragmentLODDebugMode::Disabled) return;

	if (!BaseDebugMaterial)
	{
		BaseDebugMaterial = LoadObject<UMaterialInterface>(nullptr, BaseMaterialPath);
		if (!BaseDebugMaterial)
		{
			UE_LOG(LogFragLODDebug, Warning,
				TEXT("Apply: failed to load base material %s - debug mode inactive"),
				BaseMaterialPath);
			return;
		}
	}

	const bool bDoLOD2 = Mode == EFragmentLODDebugMode::LOD2_Proxy
	                   || Mode == EFragmentLODDebugMode::AllLayers;
	const bool bDoLOD1 = Mode == EFragmentLODDebugMode::LOD1_Cells
	                   || Mode == EFragmentLODDebugMode::AllLayers;
	const bool bDoLOD0 = Mode == EFragmentLODDebugMode::LOD0_HISMCs
	                   || Mode == EFragmentLODDebugMode::AllLayers;

	if (bDoLOD2) ApplyProxyColor();
	if (bDoLOD1) ApplyCellColors();
	if (bDoLOD0) ApplyHISMCColors();

	UE_LOG(LogFragLODDebug, Log,
		TEXT("Apply: mode=%d touched %d components"),
		static_cast<int32>(Mode), Backups.Num());
}

void UFragmentLODDebugComponent::Clear()
{
	for (FMaterialBackup& B : Backups)
	{
		UPrimitiveComponent* C = B.Component.Get();
		if (!IsValid(C)) continue;

		if (B.bModifiedCustomData)
		{
			// SetNumCustomDataFloats reallocates and zeroes the per-instance buffer, so restore the count first, then copy the saved floats back into the exposed property.
			if (UInstancedStaticMeshComponent* ISMC = Cast<UInstancedStaticMeshComponent>(C))
			{
				ISMC->SetNumCustomDataFloats(B.OriginalNumCustomDataFloats);
				ISMC->PerInstanceSMCustomData = B.OriginalPerInstanceCustomData;
			}
		}

		for (int32 SlotIdx = 0; SlotIdx < B.OriginalMaterials.Num(); ++SlotIdx)
		{
			C->SetMaterial(SlotIdx, B.OriginalMaterials[SlotIdx]);
		}
		C->MarkRenderStateDirty();
	}
	Backups.Reset();
}

void UFragmentLODDebugComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	Clear();
	// Always drop the global override flag when the component goes away — otherwise the state machine keeps forcing LOD1 with no debug component owning the choice.
	if (bForceAllCellsToLOD1)
	{
		bForceAllCellsToLOD1 = false;
		SyncForceLOD1CVar();
	}
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

#if WITH_EDITOR
void UFragmentLODDebugComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PropName = PropertyChangedEvent.GetPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(UFragmentLODDebugComponent, Mode))
	{
		Apply();
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(UFragmentLODDebugComponent, bForceAllCellsToLOD1))
	{
		SyncForceLOD1CVar();
	}
}
#endif

void UFragmentLODDebugComponent::SyncForceLOD1CVar() const
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.Fragments.LODDebug.ForceAllToLOD1")))
	{
		CVar->Set(bForceAllCellsToLOD1 ? 1 : 0);
	}
	else
	{
		UE_LOG(LogFragLODDebug, Warning,
			TEXT("SyncForceLOD1CVar: CVar r.Fragments.LODDebug.ForceAllToLOD1 not found "
			     "(plugin module not loaded?)"));
	}
}

void UFragmentLODDebugComponent::ApplyProxyColor()
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	TArray<UStaticMeshComponent*> StaticMeshComps;
	Owner->GetComponents<UStaticMeshComponent>(StaticMeshComps);
	for (UStaticMeshComponent* Comp : StaticMeshComps)
	{
		if (!IsValid(Comp)) continue;
		// HISMC is a UStaticMeshComponent subclass — exclude.
		if (Comp->IsA<UHierarchicalInstancedStaticMeshComponent>()) continue;
		// LOD1 cells share the StaticMeshComponent type — exclude.
		if (Comp->ComponentHasTag(FName(CellMergeTag))) continue;
		// Only the proxy.
		if (!Comp->ComponentHasTag(FName(ProxyTag))) continue;

		SwapToColored(Comp, ProxyColor());
	}
}

void UFragmentLODDebugComponent::ApplyCellColors()
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	TArray<UStaticMeshComponent*> StaticMeshComps;
	Owner->GetComponents<UStaticMeshComponent>(StaticMeshComps);
	for (UStaticMeshComponent* Comp : StaticMeshComps)
	{
		if (!IsValid(Comp)) continue;
		if (Comp->IsA<UHierarchicalInstancedStaticMeshComponent>()) continue;
		if (!Comp->ComponentHasTag(FName(CellMergeTag))) continue;

		// Hash on component name — stable across frames, distinct per cell.
		const uint32 H = GetTypeHash(Comp->GetFName());
		SwapToColored(Comp, ColorFromHash(H, /*S*/ 230, /*V*/ 245));
	}
}

void UFragmentLODDebugComponent::ApplyHISMCColors()
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	// Lazy-load the per-instance debug material.
	if (!PerInstanceColorMaterial)
	{
		PerInstanceColorMaterial = LoadObject<UMaterialInterface>(nullptr, PerInstanceMaterialPath);
		if (!PerInstanceColorMaterial)
		{
			UE_LOG(LogFragLODDebug, Warning,
				TEXT("ApplyHISMCColors: %s not found - falling back to per-bucket "
				     "HISMC coloring. Author the material in the editor (see header "
				     "of this file for the graph) to enable per-instance colors."),
				PerInstanceMaterialPath);
		}
	}

	const bool bVerbose = CVarLODDebugVerbose.GetValueOnGameThread() != 0;

	TArray<UHierarchicalInstancedStaticMeshComponent*> HISMCs;
	Owner->GetComponents<UHierarchicalInstancedStaticMeshComponent>(HISMCs);

	if (bVerbose)
	{
		UE_LOG(LogFragLODDebug, Log,
			TEXT("DIAG: Owner=%s | HISMCs=%d | PerInstanceColorMaterial=%s"),
			*Owner->GetName(), HISMCs.Num(),
			PerInstanceColorMaterial ? *PerInstanceColorMaterial->GetPathName() : TEXT("<NULL - fallback path>"));
	}

	int32 HismcIdx = 0;
	int32 TotalInstancesTouched = 0;
	int32 TotalWriteFailures    = 0;
	for (UHierarchicalInstancedStaticMeshComponent* HISMC : HISMCs)
	{
		if (!IsValid(HISMC)) continue;
		++HismcIdx;

		if (!PerInstanceColorMaterial)
		{
			// Fallback: per-bucket coloring (pre-PerInstanceCustomData behavior).
			const uint32 H = GetTypeHash(HISMC->GetFName());
			SwapToColored(HISMC, ColorFromHash(H, /*S*/ 130, /*V*/ 190));
			continue;
		}

		// Per-instance path.
		const int32 NumSlots         = HISMC->GetNumMaterials();
		const int32 NumInstances     = HISMC->GetInstanceCount();
		const int32 RawInstDataNum   = HISMC->PerInstanceSMData.Num();
		const int32 NCDFBefore       = HISMC->NumCustomDataFloats;
		const int32 CustomDataBefore = HISMC->PerInstanceSMCustomData.Num();

		if (bVerbose && HismcIdx <= 5)
		{
			UE_LOG(LogFragLODDebug, Log,
				TEXT("DIAG: HISMC[%d] '%s' BEFORE: NumSlots=%d GetInstanceCount=%d PerInstanceSMData.Num=%d NumCustomDataFloats=%d PerInstanceSMCustomData.Num=%d"),
				HismcIdx, *HISMC->GetName(), NumSlots, NumInstances, RawInstDataNum, NCDFBefore, CustomDataBefore);
		}

		if (NumSlots == 0) continue;

		FMaterialBackup Backup;
		Backup.Component = HISMC;
		Backup.OriginalMaterials.SetNum(NumSlots);
		for (int32 i = 0; i < NumSlots; ++i)
		{
			Backup.OriginalMaterials[i] = HISMC->GetMaterial(i);
		}

		Backup.OriginalNumCustomDataFloats   = HISMC->NumCustomDataFloats;
		Backup.OriginalPerInstanceCustomData = HISMC->PerInstanceSMCustomData;
		Backup.bModifiedCustomData           = true;

		HISMC->SetNumCustomDataFloats(3);

		if (bVerbose && HismcIdx <= 5)
		{
			UE_LOG(LogFragLODDebug, Log,
				TEXT("DIAG: HISMC[%d] AFTER SetNumCustomDataFloats(3): NumCustomDataFloats=%d PerInstanceSMCustomData.Num=%d (expected=%d)"),
				HismcIdx, HISMC->NumCustomDataFloats, HISMC->PerInstanceSMCustomData.Num(), RawInstDataNum * 3);
		}

		const uint32 NameHash = GetTypeHash(HISMC->GetFName());
		int32 LocalWriteFailures = 0;
		for (int32 i = 0; i < NumInstances; ++i)
		{
			const uint32 InstHash = HashCombine(NameHash, GetTypeHash(i));
			const FLinearColor C  = ColorFromHash(InstHash, /*S*/ 200, /*V*/ 220);
			const bool b0 = HISMC->SetCustomDataValue(i, 0, C.R, /*bMarkRenderStateDirty*/ false);
			const bool b1 = HISMC->SetCustomDataValue(i, 1, C.G, /*bMarkRenderStateDirty*/ false);
			const bool b2 = HISMC->SetCustomDataValue(i, 2, C.B, /*bMarkRenderStateDirty*/ false);
			if (!(b0 && b1 && b2)) ++LocalWriteFailures;

			if (bVerbose && HismcIdx <= 3 && i < 3)
			{
				UE_LOG(LogFragLODDebug, Log,
					TEXT("DIAG: HISMC[%d] inst[%d] wrote (R=%.3f G=%.3f B=%.3f) results=(%d,%d,%d) hash=0x%08x"),
					HismcIdx, i, C.R, C.G, C.B, (int)b0, (int)b1, (int)b2, InstHash);
			}
		}
		TotalInstancesTouched += NumInstances;
		TotalWriteFailures    += LocalWriteFailures;

		if (bVerbose && HismcIdx <= 3 && HISMC->PerInstanceSMCustomData.Num() >= 9)
		{
			const TArray<float>& D = HISMC->PerInstanceSMCustomData;
			UE_LOG(LogFragLODDebug, Log,
				TEXT("DIAG: HISMC[%d] PerInstanceSMCustomData[0..8] = %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f"),
				HismcIdx, D[0], D[1], D[2], D[3], D[4], D[5], D[6], D[7], D[8]);
		}

		for (int32 i = 0; i < NumSlots; ++i)
		{
			HISMC->SetMaterial(i, PerInstanceColorMaterial);
		}
		HISMC->MarkRenderStateDirty();

		Backups.Add(MoveTemp(Backup));
	}

	if (bVerbose)
	{
		UE_LOG(LogFragLODDebug, Log,
			TEXT("DIAG: ApplyHISMCColors complete | HISMCsTouched=%d InstancesTouched=%d WriteFailures=%d"),
			HismcIdx, TotalInstancesTouched, TotalWriteFailures);
	}
}

void UFragmentLODDebugComponent::SwapToColored(UPrimitiveComponent* Comp, const FLinearColor& Color)
{
	if (!IsValid(Comp) || !BaseDebugMaterial) return;

	const int32 NumSlots = Comp->GetNumMaterials();
	if (NumSlots == 0) return;

	FMaterialBackup Backup;
	Backup.Component = Comp;
	Backup.OriginalMaterials.SetNum(NumSlots);
	for (int32 i = 0; i < NumSlots; ++i)
	{
		Backup.OriginalMaterials[i] = Comp->GetMaterial(i);
	}

	for (int32 i = 0; i < NumSlots; ++i)
	{
		UMaterialInstanceDynamic* SlotMID = UMaterialInstanceDynamic::Create(BaseDebugMaterial, Comp);
		if (!SlotMID) continue;
		SlotMID->SetVectorParameterValue(TEXT("BaseColor"), Color);
		SlotMID->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
		Comp->SetMaterial(i, SlotMID);
	}
	Comp->MarkRenderStateDirty();

	Backups.Add(MoveTemp(Backup));
}

FLinearColor UFragmentLODDebugComponent::ColorFromHash(uint32 H, uint8 Saturation, uint8 Value)
{
	// Knuth multiplicative hash with the 32-bit golden-ratio constant.
	constexpr uint32 GoldenRatio32 = 0x9E3779B9u;
	const uint8 H8 = static_cast<uint8>((H * GoldenRatio32) >> 24);
	return FLinearColor::MakeFromHSV8(H8, Saturation, Value);
}
