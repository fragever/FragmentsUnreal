// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FragmentLODDebugComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPrimitiveComponent;

UENUM(BlueprintType)
enum class EFragmentLODDebugMode : uint8
{
	Disabled       UMETA(DisplayName = "Disabled"),
	LOD2_Proxy     UMETA(DisplayName = "LOD2 only (proxy, uniform)"),
	LOD1_Cells     UMETA(DisplayName = "LOD1 only (per-cell colors)"),
	LOD0_HISMCs    UMETA(DisplayName = "LOD0 only (per-HISMC colors)"),
	AllLayers      UMETA(DisplayName = "All layers (proxy + cells + HISMCs)"),
};

/** Debug component that recolors a model's components by LOD layer. */
UCLASS(ClassGroup = (FragmentsDebug), meta = (BlueprintSpawnableComponent))
class FRAGMENTSUNREAL_API UFragmentLODDebugComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFragmentLODDebugComponent();

	/** What to colorize. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD Debug")
	EFragmentLODDebugMode Mode = EFragmentLODDebugMode::Disabled;

	/** Visual inspection toggle: force every cell with a built merged mesh to Merged_LOD1 regardless of camera distance or frustum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD Debug")
	bool bForceAllCellsToLOD1 = false;

	/** Set Mode and re-color in one call. */
	UFUNCTION(BlueprintCallable, Category = "LOD Debug",
		meta = (DisplayName = "Set Mode (and apply)"))
	void SetMode(EFragmentLODDebugMode NewMode);

	/** Re-apply the current mode. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "LOD Debug",
		meta = (DisplayName = "Apply (re-color now)"))
	void Apply();

	/** Restore original materials on every previously-touched component. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "LOD Debug",
		meta = (DisplayName = "Clear (restore originals)"))
	void Clear();

protected:
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	struct FMaterialBackup
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		TArray<TObjectPtr<UMaterialInterface>> OriginalMaterials;

		// Populated only for HISMCs we touched on the per-instance path.
		int32 OriginalNumCustomDataFloats = 0;
		TArray<float> OriginalPerInstanceCustomData;
		bool bModifiedCustomData = false;
	};

	/** Per-component material snapshots — drained by Clear(). */
	TArray<FMaterialBackup> Backups;

	/** M_BaseFragmentMaterial — has BaseColor + Opacity parameters. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BaseDebugMaterial;

	/** M_FragmentDebugPerInstanceColor — when present, ApplyHISMCColors uses PerInstanceCustomData[0..2] for per-instance RGB. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PerInstanceColorMaterial;

	void ApplyProxyColor();
	void ApplyCellColors();
	void ApplyHISMCColors();

	/** Snapshot Comp's current materials and replace every slot with a MID of BaseDebugMaterial tinted to Color. */
	void SwapToColored(UPrimitiveComponent* Comp, const FLinearColor& Color);

	/** Drive the r.Fragments.LODDebug.ForceAllToLOD1 CVar from bForceAllCellsToLOD1. */
	void SyncForceLOD1CVar() const;

	/** Hash → hue with golden-ratio rotation; S/V picked per LOD layer so layers are visually distinguishable when AllLayers is active. */
	static FLinearColor ColorFromHash(uint32 H, uint8 Saturation, uint8 Value);

	static FLinearColor ProxyColor() { return FLinearColor(1.0f, 0.85f, 0.1f); }
};
