// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Spatial/FragmentRegistrationManager.h"
#include "Fragment/Fragment.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Utils/FragmentsLog.h"
#include "GameFramework/PlayerController.h"
#include "Engine/LocalPlayer.h"
#include "SceneView.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Importer/FragmentsImporterSubsystem.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Spatial/FragmentProxyTiers.h"
#include "Spatial/FragmentScreenSpaceError.h"
#include "Spatial/FragmentCellLodCombine.h"
#include "Engine/GameInstance.h"
#include "Engine/HitResult.h"
#include "ProfilingDebugging/CountersTrace.h"

// Defined in FragmentsImporter.cpp.
extern TAutoConsoleVariable<float> CVarShellLOD0PromoteCm;
extern TAutoConsoleVariable<float> CVarInteriorLOD0PromoteCm;
extern TAutoConsoleVariable<float> CVarLOD0HysteresisCm;

// Debug override — when 1, force every cell with an available merged component to Merged_LOD1 regardless of distance/frustum.
extern TAutoConsoleVariable<int32> CVarLODDebugForceAllToLOD1;

// Defined in FragmentsImporter.cpp. Continuous model-LOD promote criterion.
extern TAutoConsoleVariable<float> CVarLODGeoCoeffCells;
extern TAutoConsoleVariable<float> CVarLODSSEPixelError;
extern TAutoConsoleVariable<float> CVarLODCoarsenFactor;

static TAutoConsoleVariable<int32> CVarCellTransitionsPerTick(
	TEXT("fragments.CellTransitionsPerTick"), -1,
	TEXT("Override MaxCellTransitionsPerTick (-1 = use UPROPERTY value)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarModelTierTransitionsPerTick(
	TEXT("fragments.ModelTierTransitionsPerTick"), -1,
	TEXT("Override MaxModelTierTransitionsPerTick (-1 = use the UPROPERTY value). Runtime-settable: "
	     "pure pacing with no baked consequence, unlike the r.Fragments.LOD.* criterion CVars."),
	ECVF_Default);

TRACE_DECLARE_INT_COUNTER(FragRegMgr_CellTransitionsAppliedThisTick,
	TEXT("FragRegMgr/CellTransitionsAppliedThisTick"));

namespace
{
	/** Compute the target ECellLodState for a cell given camera position, frustum, and the promote distance. */
	ECellLodState DecideCellTargetState(
		const FCellRegistrationData& Cell,
		const FVector& CameraLocation,
		const FConvexVolume& ViewFrustum,
		bool bDoFrustum,
		float PromoteCm,
		float HysteresisCm)
	{
		// Debug override: force every cell with an available merged component into Merged_LOD1, bypassing frustum + distance gates.
		if (CVarLODDebugForceAllToLOD1.GetValueOnGameThread() != 0)
		{
			const bool bHasMerged = IsValid(Cell.MergedComponent) && !Cell.bMergeInFlight;
			return bHasMerged ? ECellLodState::Merged_LOD1 : ECellLodState::Active_LOD0;
		}

		// User-driven floor filter wins over distance and HotPromoteOverride.
		if (Cell.bFilterHidden)
		{
			return ECellLodState::Skipped;
		}

		// Defensive clamp: if a user sets HysteresisCm >= PromoteCm, the lower band edge (PromoteCm - HysteresisCm) goes ≤0 and a Merged_LOD1 cell in that range can never demote back to Active_LOD0 (distance is always ≥0).
		const float ClampedHysteresisCm = FMath::Min(HysteresisCm, PromoteCm * 0.5f);

		// Reject cells outside the view frustum.
		if (bDoFrustum && Cell.WorldBounds.IsValid &&
			!ViewFrustum.IntersectBox(
				Cell.WorldBounds.GetCenter(), Cell.WorldBounds.GetExtent()))
		{
			return ECellLodState::Skipped;
		}

		// Keep explicitly promoted cells at full detail.
		if (Cell.HotPromoteOverride)
		{
			return ECellLodState::Active_LOD0;
		}

		// Apply the distance gate with hysteresis.
		const double DistSq = Cell.WorldBounds.IsValid
			? Cell.WorldBounds.ComputeSquaredDistanceToPoint(CameraLocation)
			: 0.0;
		const double Dist = FMath::Sqrt(DistSq);

		const float UpperBound = PromoteCm + ClampedHysteresisCm;
		const float LowerBound = PromoteCm - ClampedHysteresisCm;

		// No merged mesh available → pin at Active_LOD0 regardless of distance.
		const bool bHasMerged = IsValid(Cell.MergedComponent) && !Cell.bMergeInFlight;
		if (!bHasMerged)
		{
			return ECellLodState::Active_LOD0;
		}

		return UE::Fragments::CellLod::ResolveDistanceBand(Cell.LodState, Dist, LowerBound, UpperBound);
	}

	/** Toggles every HISMC owned by a cell in lockstep with the cell's merged proxy. */
	void SetCellHISMCsVisible(FCellRegistrationData& Cell, bool bVisible)
	{
		// DIAGNOSTIC: SetVisibility marks render state dirty per component, so a cell with many HISMCs enqueues that many render-state updates in one frame.
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_SetCellHISMCsVisible);

		int32 Toggled = 0;
		for (TObjectPtr<UHierarchicalInstancedStaticMeshComponent>& H : Cell.HISMCs)
		{
			if (IsValid(H))
			{
				H->SetVisibility(bVisible, false);
				++Toggled;
			}
		}
		TRACE_INT_VALUE(TEXT("FragRegMgr/CellHISMCsToggled"), Toggled);
	}

	/** Set the merged proxy's render visibility AND trace collision together. */
	void SetMergedShown(UStaticMeshComponent* Merged, bool bShown)
	{
		if (!IsValid(Merged)) return;
		Merged->SetVisibility(bShown, /*bPropagateToChildren=*/ false);
		Merged->SetCollisionEnabled(bShown
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision);
	}

	/** Toggle all three shadow-casting flags atomically. bCastHiddenShadow keeps the LOD1 in the shadow gather while SetVisibility(false). */
	void SetMergedShadowEnabled(UStaticMeshComponent* Merged, bool bEnabled)
	{
		if (!IsValid(Merged)) return;
		Merged->bCastHiddenShadow  = bEnabled;
		Merged->bCastDynamicShadow = bEnabled;
		Merged->SetCastShadow(bEnabled);
	}

	/** Cook a HISMC's StaticMesh body setup synchronously so line trace queries will actually hit it. */
	void CookHISMCBodyIfNeeded(UHierarchicalInstancedStaticMeshComponent* HISM, UWorld* World)
	{
		if (!IsValid(HISM)) return;
		UStaticMesh* Mesh = HISM->GetStaticMesh();
		if (!Mesh || !Mesh->GetBodySetup()) return;
		UBodySetup* BS = Mesh->GetBodySetup();
		if (BS->bCreatedPhysicsMeshes) return;

		UObject* OrigOuter = BS->GetOuter();
		if (World) BS->Rename(nullptr, World, REN_DontCreateRedirectors);
		BS->InvalidatePhysicsData();
		BS->CreatePhysicsMeshes();
		if (OrigOuter && OrigOuter != World) BS->Rename(nullptr, OrigOuter, REN_DontCreateRedirectors);

		HISM->RecreatePhysicsState();
	}

	/** Register any not-yet-registered HISMCs in this cell with QueryOnly collision. */
	void EnsureCellHISMCsRegistered(
		FCellRegistrationData& Cell,
		FModelRegistrationData& Model)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_EnsureCellHISMCsRegistered);

		// DIAGNOSTIC: separates the two costs this function can incur, which the enclosing CellPromote_LOD0 scope cannot distinguish: ScanIters -- the inner linear search over Model.Components.
		int32 ScanIters = 0;
		int32 Registered = 0;

		for (UHierarchicalInstancedStaticMeshComponent* H : Cell.HISMCs)
		{
			if (!IsValid(H)) continue;
			for (FHISMCRegistrationState& S : Model.Components)
			{
				++ScanIters;
				if (S.Component.Get() == H)
				{
					if (!S.bIsRegistered)
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_HISMCRegisterComponent);
						H->RegisterComponent();
						H->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
						S.bIsRegistered = true;
						++Registered;
					}
					break;
				}
			}
		}

		TRACE_INT_VALUE(TEXT("FragRegMgr/EnsureRegistered_ScanIters"), ScanIters);
		TRACE_INT_VALUE(TEXT("FragRegMgr/EnsureRegistered_Registered"), Registered);
	}

	/** Apply the target state. */
	bool ApplyCellTransition(
		FCellRegistrationData& Cell,
		FModelRegistrationData& Model,
		ECellLodState Target)
	{
		if (Target == Cell.LodState) return false;

		// A user-hidden model takes no transitions; LodState stays put so unhide can restore it.
		if (!Model.bUserVisible) return false;

		const ECellLodState Prev = Cell.LodState;
		bool bDidWork = false;

		// Active_LOD0 → Merged_LOD1
		if (Prev == ECellLodState::Active_LOD0 && Target == ECellLodState::Merged_LOD1)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CellDemote_LOD1);
			SetMergedShown(Cell.MergedComponent, true);
			SetCellHISMCsVisible(Cell, false);
			Cell.LodState = ECellLodState::Merged_LOD1;
			bDidWork = true;
		}
		// Merged_LOD1 → Active_LOD0
		else if (Prev == ECellLodState::Merged_LOD1 && Target == ECellLodState::Active_LOD0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CellPromote_LOD0);
			EnsureCellHISMCsRegistered(Cell, Model);
			SetMergedShown(Cell.MergedComponent, false);
			SetCellHISMCsVisible(Cell, true);
			Cell.LodState = ECellLodState::Active_LOD0;
			bDidWork = true;
		}
		// Active_LOD0 → Skipped
		else if (Prev == ECellLodState::Active_LOD0 && Target == ECellLodState::Skipped)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CellSkipFrustum);
			SetMergedShown(Cell.MergedComponent, false);
			SetCellHISMCsVisible(Cell, false);
			Cell.LodState = ECellLodState::Skipped;
			bDidWork = true;
		}
		// Merged_LOD1 → Skipped
		else if (Prev == ECellLodState::Merged_LOD1 && Target == ECellLodState::Skipped)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CellSkipFrustum);
			SetMergedShown(Cell.MergedComponent, false);
			SetCellHISMCsVisible(Cell, false);
			Cell.LodState = ECellLodState::Skipped;
			bDidWork = true;
		}
		// Skipped → Active_LOD0
		else if (Prev == ECellLodState::Skipped && Target == ECellLodState::Active_LOD0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CellPromote_LOD0);
			EnsureCellHISMCsRegistered(Cell, Model);
			SetMergedShown(Cell.MergedComponent, false);
			SetCellHISMCsVisible(Cell, true);
			Cell.LodState = ECellLodState::Active_LOD0;
			bDidWork = true;
		}
		// Skipped → Merged_LOD1
		else if (Prev == ECellLodState::Skipped && Target == ECellLodState::Merged_LOD1)
		{
			if (IsValid(Cell.MergedComponent))
			{
				SetMergedShown(Cell.MergedComponent, true);
				SetCellHISMCsVisible(Cell, false);
				Cell.LodState = ECellLodState::Merged_LOD1;
				bDidWork = true;
			}
			// else: DecideCellTargetState would have returned Active_LOD0; this branch is only hit if MergedComponent went null between decide and apply (race with model unload).
		}

		return bDidWork;
	}
}

void UFragmentRegistrationManager::RebuildCellHISMCIndex(FModelRegistrationData& ModelData)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_BuildCellHISMCIndex);

	for (FHISMCRegistrationState& State : ModelData.Components)
	{
		UHierarchicalInstancedStaticMeshComponent* H = State.Component.Get();
		if (!IsValid(H)) continue;

		ensureMsgf(!State.CellId.IsSentinel(),
			TEXT("RebuildCellHISMCIndex: HISMC %s has sentinel CellId; RemapOrphans should have caught this."),
			*GetNameSafe(H));

		TArray<FCellRegistrationData>& Cells = State.bIsShell
			? ModelData.ShellCells : ModelData.InteriorCells;
		FCellRegistrationData* Match = Cells.FindByPredicate(
			[&State](const FCellRegistrationData& C) { return C.Cell == State.CellId; });
		if (Match)
		{
			Match->HISMCs.AddUnique(H);
			// The frustum and distance gates test the cell's bounds, so they must cover every owned component.
			if (H->GetInstanceCount() > 0)
			{
				Match->WorldBounds += H->CalcBounds(H->GetComponentTransform()).GetBox();
			}
		}
		else
		{
			UE_LOG(LogFragments, Error,
				TEXT("[CellSubBucket] HISMC %s Cell=(F%d,X%d,Y%d) bShell=%d has no matching FCellRegistrationData."),
				*GetNameSafe(H), State.CellId.Floor, State.CellId.GridX, State.CellId.GridY,
				State.bIsShell ? 1 : 0);
			ensureMsgf(false, TEXT("RebuildCellHISMCIndex: HISMC has no matching cell; see preceding error log."));
		}
	}

	// Prime visibility from LodState.
	for (FCellRegistrationData& C : ModelData.ShellCells)
	{
		SetCellHISMCsVisible(C, ModelData.bUserVisible && C.LodState == ECellLodState::Active_LOD0);
	}
	for (FCellRegistrationData& C : ModelData.InteriorCells)
	{
		SetCellHISMCsVisible(C, ModelData.bUserVisible && C.LodState == ECellLodState::Active_LOD0);
	}

	int32 ShellCellsWithHISMCs = 0, InteriorCellsWithHISMCs = 0;
	for (const FCellRegistrationData& C : ModelData.ShellCells)    if (C.HISMCs.Num() > 0) ShellCellsWithHISMCs++;
	for (const FCellRegistrationData& C : ModelData.InteriorCells) if (C.HISMCs.Num() > 0) InteriorCellsWithHISMCs++;
	UE_LOG(LogFragments, Verbose,
		TEXT("[CellSubBucket] %s: cells with HISMCs: shell=%d/%d interior=%d/%d"),
		*ModelData.ModelGuid,
		ShellCellsWithHISMCs, ModelData.ShellCells.Num(),
		InteriorCellsWithHISMCs, ModelData.InteriorCells.Num());
}

void UFragmentRegistrationManager::RegisterModel(AFragment* BucketRoot, bool bHasDeferredInteriors, UStaticMeshComponent* InProxyComponent, UFragmentModelWrapper* InWrapper)
{
	if (!BucketRoot || !BucketRoot->bBucketRoot)
	{
		return;
	}

	// Check for duplicates
	for (const FModelRegistrationData& Existing : RegisteredModels)
	{
		if (Existing.BucketRoot == BucketRoot) return;
	}

	FModelRegistrationData ModelData;
	ModelData.BucketRoot = BucketRoot;

	// CullDistance is the LOD0 promote distance, used by EvaluateOneHISMC for glass-flip and collision-gating thresholds.
	const float ShellPromoteCm    = CVarShellLOD0PromoteCm.GetValueOnGameThread();
	const float InteriorPromoteCm = CVarInteriorLOD0PromoteCm.GetValueOnGameThread();

	int32 CompIdx = 0;
	for (auto& [Key, HISMC] : BucketRoot->Buckets)
	{
		if (!IsValid(HISMC)) continue;   // do NOT bump CompIdx for skipped entries

		FHISMCRegistrationState State;
		State.Component = HISMC;
		State.CellId = Key.Cell;
		State.bIsShell = (Key.bShell != 0);
		State.CullDistance = State.bIsShell ? ShellPromoteCm : InteriorPromoteCm;
		State.bIsRegistered = HISMC->IsRegistered();

		// Pick up the importer's glass tag (see FragmentClassification::IsOccludableGlass).
		if (HISMC->ComponentHasTag(FName(TEXT("GlassCurtainWall"))))
		{
			State.bIsOccludableGlass = true;
			State.OccluderState = EGlassOccluderState::Transparent;
		}

		// Pick up the importer's structural tag for shadow gating decisions.
		State.bIsStructural = HISMC->ComponentHasTag(FName(TEXT("Structural")));

		// Cache bounds once — HISMC instances are set once and never move, and the bucket root actor is stationary.
		const FBoxSphereBounds Bounds = HISMC->CalcBounds(HISMC->GetComponentTransform());
		State.CachedBoundsOrigin = Bounds.Origin;
		State.CachedBoundsRadius = Bounds.SphereRadius;

		const int32 ThisCompIdx = CompIdx;  // capture pre-add index
		ModelData.Components.Add(MoveTemp(State));
		CompIdx++;

		ModelData.AllHISMCs.Add(ThisCompIdx);
	}

	// Compute tight AABB from all HISMCs' actual box bounds (not sphere-reconstructed).
	if (ModelData.Components.Num() > 0)
	{
		// Seed with first HISMC's actual box bounds
		const FBoxSphereBounds FirstBounds = ModelData.Components[0].Component->CalcBounds(
			ModelData.Components[0].Component->GetComponentTransform());
		FBox UnionBox(FirstBounds.Origin - FirstBounds.BoxExtent,
			FirstBounds.Origin + FirstBounds.BoxExtent);

		for (int32 i = 1; i < ModelData.Components.Num(); i++)
		{
			if (!IsValid(ModelData.Components[i].Component)) continue;
			const FBoxSphereBounds CompBounds = ModelData.Components[i].Component->CalcBounds(
				ModelData.Components[i].Component->GetComponentTransform());
			const FBox CompBox(CompBounds.Origin - CompBounds.BoxExtent,
				CompBounds.Origin + CompBounds.BoxExtent);
			UnionBox += CompBox;  // FBox union = min/max per axis
		}

		const FBoxSphereBounds UnionBounds(UnionBox);
		ModelData.ModelBoundsOrigin = UnionBounds.Origin;
		ModelData.ModelBoundsExtent = UnionBounds.BoxExtent;
		ModelData.ModelBoundsRadius = UnionBounds.SphereRadius;
	}

	ModelData.bInteriorsPending = bHasDeferredInteriors;

	// Store model GUID for reverse lookup
	ModelData.ModelGuid = BucketRoot->GetModelGuid();

	// Step 6 Phase A: populate cell WorldBounds from the wrapper's tables.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_PartitionCells);
		// Caller-supplied wrapper wins.
		UFragmentModelWrapper* Wrapper = InWrapper;
		if (!Wrapper)
		{
			UWorld* RegWorld = BucketRoot->GetWorld();
			UGameInstance* GI = RegWorld ? RegWorld->GetGameInstance() : nullptr;
			UFragmentsImporterSubsystem* Subsys = GI ? GI->GetSubsystem<UFragmentsImporterSubsystem>() : nullptr;
			Wrapper = Subsys ? Subsys->GetModelWrapper(ModelData.ModelGuid) : nullptr;
		}

		// Pre-populate cell tables from wrapper data so AssignBounds has records to fill in.
		if (Wrapper)
		{
			for (const TPair<FCellId, FCellMetadata>& Pair : Wrapper->ShellCells)
			{
				FCellRegistrationData Record;
				Record.Cell = Pair.Key;
				ModelData.ShellCells.Add(MoveTemp(Record));
			}
			for (const TPair<FCellId, FCellMetadata>& Pair : Wrapper->InteriorCells)
			{
				FCellRegistrationData Record;
				Record.Cell = Pair.Key;
				ModelData.InteriorCells.Add(MoveTemp(Record));
			}
		}

		if (Wrapper)
		{
			auto AssignBounds = [](
				TArray<FCellRegistrationData>& Records,
				const TMap<FCellId, FCellMetadata>& Source)
			{
				for (FCellRegistrationData& CRD : Records)
				{
					if (const FCellMetadata* Meta = Source.Find(CRD.Cell))
					{
						CRD.WorldBounds = Meta->WorldBounds;
					}
					else
					{
						// Cells without wrapper metadata get an empty box; the state machine's frustum/distance gates treat this as "always skip" until the wrapper publishes real bounds.
						CRD.WorldBounds = FBox(ForceInit);
					}
				}
			};
			AssignBounds(ModelData.ShellCells,    Wrapper->ShellCells);
			AssignBounds(ModelData.InteriorCells, Wrapper->InteriorCells);
		}
		else
		{
			UE_LOG(LogFragments, Warning,
				TEXT("[RegisterModel] No wrapper for ModelGuid=%s; cells will use fallback bounds."),
				*ModelData.ModelGuid);
		}

		// Mark all cells as in-flight.
		for (FCellRegistrationData& CRD : ModelData.ShellCells)
		{
			CRD.bMergeInFlight = true;
			CRD.LodState = ECellLodState::Merged_LOD1;
		}
		for (FCellRegistrationData& CRD : ModelData.InteriorCells)
		{
			CRD.bMergeInFlight = true;
			CRD.LodState = ECellLodState::Merged_LOD1;
		}

		UE_LOG(LogFragments, Verbose,
			TEXT("[RegisterModel] %s: ShellCells=%d InteriorCells=%d HISMCs=%d"),
			*ModelData.ModelGuid,
			ModelData.ShellCells.Num(),
			ModelData.InteriorCells.Num(),
			ModelData.AllHISMCs.Num());
	}

	// Shell pass. RegisterNewComponents calls this again to pick up interiors.
	RebuildCellHISMCIndex(ModelData);

	// Proxy component handling
	if (InProxyComponent)
	{
		ModelData.ProxyComponent = InProxyComponent;
	}
	// Models always start in the Proxy tier; the first UpdateRegistration pass promotes any model the camera is already close to.
	ModelData.Tier = FragmentModelTier::ETier::Proxy;

	int32 GlassCount = 0;
	int32 StructuralCount = 0;
	for (const FHISMCRegistrationState& S : ModelData.Components)
	{
		if (S.bIsOccludableGlass) GlassCount++;
		if (S.bIsStructural) StructuralCount++;
	}
	UE_LOG(LogFragments, Log, TEXT("RegistrationManager: Registered model with %d HISMCs (%d structural, %d glass). Bounds: origin=(%.0f, %.0f, %.0f) radius=%.0f cm. InteriorsPending=%s"),
		ModelData.Components.Num(), StructuralCount, GlassCount,
		ModelData.ModelBoundsOrigin.X, ModelData.ModelBoundsOrigin.Y, ModelData.ModelBoundsOrigin.Z,
		ModelData.ModelBoundsRadius,
		ModelData.bInteriorsPending ? TEXT("true") : TEXT("false"));

	RegisteredModels.Add(MoveTemp(ModelData));

	// Force one update pass so the initial registration sweep runs even if the camera is still.
	bForceNextUpdate = true;
}

// DIAGNOSTIC SCOPE: the LOD2 proxy's arrival on the game thread, fired from the async proxy-build completion.
void UFragmentRegistrationManager::AttachProxyComponent(AFragment* BucketRoot, UStaticMeshComponent* ProxyComp)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_AttachProxyComponent);

	if (!BucketRoot || !ProxyComp)
	{
		return;
	}

	for (FModelRegistrationData& Data : RegisteredModels)
	{
		if (Data.BucketRoot == BucketRoot)
		{
			if (Data.ProxyComponent != nullptr)
			{
				UE_LOG(LogFragments, Warning,
					TEXT("[AttachProxyComponent] BucketRoot %s already has a proxy attached; ignoring double-attach."),
					*BucketRoot->GetName());
				return;
			}
			Data.ProxyComponent = ProxyComp;

			// The tier is a continuous function of the view, so the late-arriving proxy just adopts whatever tier the model already holds — there is no activation state to consult, and no HISMC registration to undo.
			if (Data.Tier == FragmentModelTier::ETier::Cells)
			{
				// The camera is already close enough for cells; the proxy must not draw over them.
				SetProxyShown(ProxyComp, false);
				UE_LOG(LogFragments, Verbose,
					TEXT("[AttachProxyComponent] Model %s is cell-resident; hid late-arriving proxy."),
					*BucketRoot->GetName());
			}
			else
			{
				// UpdateRegistration skips per-cell iteration for Proxy-tier models, so any cell left visible would render full detail underneath the proxy.
				ResetCellsForProxyMode(Data);
				// A user-hidden model adopts Proxy-tier bookkeeping but stays dark until SetModelUserVisible(true) reveals it.
				SetProxyShown(ProxyComp, Data.bUserVisible);
				UE_LOG(LogFragments, Verbose,
					TEXT("[AttachProxyComponent] Attached proxy to %s; Proxy tier (user-visible=%d)."),
					*BucketRoot->GetName(), Data.bUserVisible ? 1 : 0);
			}

			// The proxy's arrival flips FDecisionInputs::bProxyAvailable false -> true, so the tier decision must re-run.
			bForceNextUpdate = true;

			// A model already promoted to Cells before its async proxy build finished must not have that proxy arrive casting.
			ApplyModelShadowOwnership(Data, Data.Tier);
			return;
		}
	}

	// Model was unregistered between completion-lambda dispatch and now.
	if (ProxyComp->IsRegistered())
	{
		ProxyComp->DestroyComponent();
	}
	UE_LOG(LogFragments, Verbose,
		TEXT("[AttachProxyComponent] No registered model for BucketRoot %s; destroyed orphan proxy."),
		*BucketRoot->GetName());
}

bool UFragmentRegistrationManager::GetModelBounds(const FString& ModelGuid, FBox& OutBounds) const
{
	OutBounds.Init();

	for (const FModelRegistrationData& ModelData : RegisteredModels)
	{
		if (ModelData.ModelGuid != ModelGuid)
		{
			continue;
		}

		if (IsValid(ModelData.ProxyComponent))
		{
			const FBoxSphereBounds ProxyBounds = ModelData.ProxyComponent->CalcBounds(
				ModelData.ProxyComponent->GetComponentTransform());

			OutBounds = FBox(
				ProxyBounds.Origin - ProxyBounds.BoxExtent,
				ProxyBounds.Origin + ProxyBounds.BoxExtent);

			if (OutBounds.IsValid != 0)
			{
				return true;
			}
		}

		if (!ModelData.ModelBoundsExtent.IsNearlyZero())
		{
			OutBounds = FBox(
				ModelData.ModelBoundsOrigin - ModelData.ModelBoundsExtent,
				ModelData.ModelBoundsOrigin + ModelData.ModelBoundsExtent);

			return OutBounds.IsValid != 0;
		}

		if (IsValid(ModelData.BucketRoot))
		{
			OutBounds = ModelData.BucketRoot->GetBucketBounds(false);
			return OutBounds.IsValid != 0;
		}

		return false;
	}

	return false;
}

bool UFragmentRegistrationManager::GetRegisteredModelsBounds(FBox& OutBounds) const
{
	OutBounds.Init();

	for (const FModelRegistrationData& ModelData : RegisteredModels)
	{
		FBox ModelBox(ForceInit);

		if (IsValid(ModelData.ProxyComponent))
		{
			const FBoxSphereBounds ProxyBounds = ModelData.ProxyComponent->CalcBounds(
				ModelData.ProxyComponent->GetComponentTransform());

			ModelBox = FBox(
				ProxyBounds.Origin - ProxyBounds.BoxExtent,
				ProxyBounds.Origin + ProxyBounds.BoxExtent);
		}
		else if (!ModelData.ModelBoundsExtent.IsNearlyZero())
		{
			ModelBox = FBox(
				ModelData.ModelBoundsOrigin - ModelData.ModelBoundsExtent,
				ModelData.ModelBoundsOrigin + ModelData.ModelBoundsExtent);
		}
		else if (IsValid(ModelData.BucketRoot))
		{
			ModelBox = ModelData.BucketRoot->GetBucketBounds(false);
		}

		if (ModelBox.IsValid)
		{
			OutBounds += ModelBox;
		}
	}

	return OutBounds.IsValid != 0;
}

void UFragmentRegistrationManager::SetProxyShown(UStaticMeshComponent* ProxyComp, bool bShown)
{
	if (!IsValid(ProxyComp)) return;
	ProxyComp->SetVisibility(bShown, /*bPropagateToChildren=*/ false);
	ProxyComp->SetHiddenInGame(!bShown);
	// Invariant mirrors SetMergedShown: an invisible primitive never keeps QueryOnly collision, or a click would still hit the proxy after the model went cell-resident and the retry trace would never reach a cell.
	ProxyComp->SetCollisionEnabled(bShown
		? ECollisionEnabled::QueryOnly
		: ECollisionEnabled::NoCollision);
}

void UFragmentRegistrationManager::ResetCellsForProxyMode(FModelRegistrationData& ModelData)
{
	// DIAGNOSTIC: walks every cell on demotion. Cost tracks total cell count, not proximity.
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_ResetCellsForProxyMode);

	auto ResetTable = [](TArray<FCellRegistrationData>& Cells)
	{
		for (FCellRegistrationData& Cell : Cells)
		{
			if (Cell.bFilterHidden) continue;
			SetMergedShown(Cell.MergedComponent, false);
			// false, NOT true.
			SetCellHISMCsVisible(Cell, false);
			Cell.LodState = ECellLodState::Merged_LOD1;
		}
	};
	ResetTable(ModelData.ShellCells);
	ResetTable(ModelData.InteriorCells);
}

bool UFragmentRegistrationManager::ApplyModelTierTransition(
	FModelRegistrationData& ModelData, FragmentModelTier::ETier Target)
{
	using FragmentModelTier::ETier;

	if (Target == ModelData.Tier) return false;

	if (Target == ETier::Cells)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_ModelPromote_Cells);

		SetProxyShown(ModelData.ProxyComponent, false);

		// Baseline every cell to Merged_LOD1 with its merged mesh shown.
		auto ShowMerged = [](TArray<FCellRegistrationData>& Cells)
		{
			for (FCellRegistrationData& Cell : Cells)
			{
				if (Cell.bFilterHidden) continue;
				SetMergedShown(Cell.MergedComponent, true);
				Cell.LodState = ECellLodState::Merged_LOD1;
			}
		};
		ShowMerged(ModelData.ShellCells);
		ShowMerged(ModelData.InteriorCells);

		ModelData.Tier = ETier::Cells;
	}
	else
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_ModelDemote_Proxy);

		// Hides merged cells AND their HISMCs, and resets LodState.
		ResetCellsForProxyMode(ModelData);
		SetProxyShown(ModelData.ProxyComponent, true);

		// Collision teardown, mirroring what the deleted DeactivateModel did unconditionally.
		for (FHISMCRegistrationState& State : ModelData.Components)
		{
			State.bForceCollision = false;
			if (!IsValid(State.Component)) continue;
			if (State.Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			{
				State.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
		}

		// Paired with the bForceCollision clear above: the per-cell override is what would ask IterateCells to re-promote, and IterateCells does not run at Proxy tier either.
		auto ClearCellHotPromotes = [](TArray<FCellRegistrationData>& Cells)
		{
			for (FCellRegistrationData& Cell : Cells) { Cell.HotPromoteOverride = false; }
		};
		ClearCellHotPromotes(ModelData.ShellCells);
		ClearCellHotPromotes(ModelData.InteriorCells);

		ModelData.Tier = ETier::Proxy;
	}

	ApplyModelShadowOwnership(ModelData, ModelData.Tier);
	// Deliberately no bForceNextUpdate here.
	return true;
}

void UFragmentRegistrationManager::ApplyModelShadowOwnership(
	FModelRegistrationData& ModelData, FragmentModelTier::ETier Tier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_ApplyModelShadowOwnership);

	// A user-hidden model casts nothing in any tier.
	const bool bUserVisible = ModelData.bUserVisible;
	const bool bCellsCast = bUserVisible && FragmentModelTier::ShouldCellsCast(Tier);

	if (IsValid(ModelData.ProxyComponent))
	{
		// Two flags here vs.
		const bool bProxyCasts = bUserVisible && FragmentModelTier::ShouldProxyCast(Tier);
		ModelData.ProxyComponent->SetCastShadow(bProxyCasts);
		ModelData.ProxyComponent->bCastDynamicShadow = bProxyCasts;
	}

	auto ApplyToTable = [bCellsCast](TArray<FCellRegistrationData>& Cells)
	{
		for (FCellRegistrationData& Cell : Cells)
		{
			// A floor-filtered cell stays dark regardless of tier: the user hid it.
			SetMergedShadowEnabled(Cell.MergedComponent, bCellsCast && !Cell.bFilterHidden);
		}
	};
	ApplyToTable(ModelData.ShellCells);
	ApplyToTable(ModelData.InteriorCells);
}

void UFragmentRegistrationManager::UnregisterModel(AFragment* BucketRoot)
{
	const FString Guid = BucketRoot ? BucketRoot->GetModelGuid() : TEXT("");

	RegisteredModels.RemoveAll([&](FModelRegistrationData& Data)
	{
		if (Data.BucketRoot != BucketRoot) return false;

		// Destroy proxy component
		if (IsValid(Data.ProxyComponent))
		{
			Data.ProxyComponent->DestroyComponent();
		}

		return true;
	});
}

void UFragmentRegistrationManager::ResetAllRegistrations()
{
	// Collect first: UnregisterModel mutates RegisteredModels, so this cannot iterate it.
	TArray<AFragment*> Roots;
	Roots.Reserve(RegisteredModels.Num());
	for (const FModelRegistrationData& Data : RegisteredModels)
	{
		if (IsValid(Data.BucketRoot))
		{
			Roots.Add(Data.BucketRoot);
		}
	}

	for (AFragment* Root : Roots)
	{
		UnregisterModel(Root);
	}

	// Anything left held an already-destroyed BucketRoot, so UnregisterModel could not match it.
	RegisteredModels.Reset();
	// Raw component pointers.
	PendingGlassFlips.Reset();

	UE_LOG(LogFragments, Log, TEXT("[Registration] ResetAllRegistrations: released %d model(s)"),
		Roots.Num());
}

void UFragmentRegistrationManager::ReleaseWorld(UWorld* World)
{
	// Severs references only.
	const int32 Before = RegisteredModels.Num();
	RegisteredModels.RemoveAll([World](const FModelRegistrationData& Data)
	{
		return !IsValid(Data.BucketRoot) || Data.BucketRoot->GetWorld() == World;
	});
	PendingGlassFlips.RemoveAll([World](const FPendingGlassFlip& Flip)
	{
		return !IsValid(Flip.Component) || Flip.Component->GetWorld() == World;
	});

	UE_LOG(LogFragments, Log, TEXT("[Registration] ReleaseWorld: released %d model(s)"),
		Before - RegisteredModels.Num());
}

void UFragmentRegistrationManager::RegisterNewComponents(AFragment* BucketRoot, UFragmentModelWrapper* InWrapper)
{
	if (!BucketRoot || !BucketRoot->bBucketRoot) return;

	// Find existing model data
	FModelRegistrationData* ModelData = nullptr;
	for (FModelRegistrationData& Existing : RegisteredModels)
	{
		if (Existing.BucketRoot == BucketRoot)
		{
			ModelData = &Existing;
			break;
		}
	}

	if (!ModelData)
	{
		UE_LOG(LogFragments, Warning, TEXT("RegisterNewComponents: model not found for BucketRoot %s"),
			*BucketRoot->GetName());
		return;
	}

	// Build set of already-tracked HISMCs for O(1) lookup
	TSet<UHierarchicalInstancedStaticMeshComponent*> ExistingComponents;
	ExistingComponents.Reserve(ModelData->Components.Num());
	for (const FHISMCRegistrationState& State : ModelData->Components)
	{
		if (State.Component) ExistingComponents.Add(State.Component.Get());
	}

	int32 NewCount = 0;

	const float ShellPromoteCm    = CVarShellLOD0PromoteCm.GetValueOnGameThread();
	const float InteriorPromoteCm = CVarInteriorLOD0PromoteCm.GetValueOnGameThread();

	for (auto& [Key, HISMC] : BucketRoot->Buckets)
	{
		if (!IsValid(HISMC)) continue;
		if (ExistingComponents.Contains(HISMC)) continue;  // Already tracked

		FHISMCRegistrationState State;
		State.Component = HISMC;
		State.CellId = Key.Cell;
		State.bIsShell = (Key.bShell != 0);
		State.CullDistance = State.bIsShell ? ShellPromoteCm : InteriorPromoteCm;
		State.bIsRegistered = HISMC->IsRegistered();

		if (HISMC->ComponentHasTag(FName(TEXT("GlassCurtainWall"))))
		{
			State.bIsOccludableGlass = true;
			State.OccluderState = EGlassOccluderState::Transparent;
		}
		State.bIsStructural = HISMC->ComponentHasTag(FName(TEXT("Structural")));

		const FBoxSphereBounds Bounds = HISMC->CalcBounds(HISMC->GetComponentTransform());
		State.CachedBoundsOrigin = Bounds.Origin;
		State.CachedBoundsRadius = Bounds.SphereRadius;

		const int32 ThisCompIdx = ModelData->Components.Num();  // index for the about-to-add entry
		ModelData->Components.Add(MoveTemp(State));
		NewCount++;

		ModelData->AllHISMCs.Add(ThisCompIdx);
	}

	if (NewCount > 0)
	{
		// Refresh interior cell bounds after they load.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_PartitionCells);
			// Caller-supplied wrapper wins; see RegisterModel for why the GameInstance fallback is empty-handed in the editor.
			UFragmentModelWrapper* Wrapper = InWrapper;
			if (!Wrapper)
			{
				UWorld* RegWorld = BucketRoot->GetWorld();
				UGameInstance* GI = RegWorld ? RegWorld->GetGameInstance() : nullptr;
				UFragmentsImporterSubsystem* Subsys = GI ? GI->GetSubsystem<UFragmentsImporterSubsystem>() : nullptr;
				Wrapper = Subsys ? Subsys->GetModelWrapper(ModelData->ModelGuid) : nullptr;
			}

			// Pre-populate cell tables from wrapper data for any cells the wrapper has gained since the initial RegisterModel call (typically interior cells streamed in after the shell-first trigger).
			if (Wrapper)
			{
				TSet<FCellId> KnownShell;
				KnownShell.Reserve(ModelData->ShellCells.Num());
				for (const FCellRegistrationData& CRD : ModelData->ShellCells)
				{
					KnownShell.Add(CRD.Cell);
				}
				for (const TPair<FCellId, FCellMetadata>& Pair : Wrapper->ShellCells)
				{
					if (!KnownShell.Contains(Pair.Key))
					{
						FCellRegistrationData Record;
						Record.Cell = Pair.Key;
						ModelData->ShellCells.Add(MoveTemp(Record));
					}
				}

				TSet<FCellId> KnownInterior;
				KnownInterior.Reserve(ModelData->InteriorCells.Num());
				for (const FCellRegistrationData& CRD : ModelData->InteriorCells)
				{
					KnownInterior.Add(CRD.Cell);
				}
				for (const TPair<FCellId, FCellMetadata>& Pair : Wrapper->InteriorCells)
				{
					if (!KnownInterior.Contains(Pair.Key))
					{
						FCellRegistrationData Record;
						Record.Cell = Pair.Key;
						ModelData->InteriorCells.Add(MoveTemp(Record));
					}
				}
			}

			if (Wrapper)
			{
				auto AssignBounds = [](
					TArray<FCellRegistrationData>& Records,
					const TMap<FCellId, FCellMetadata>& Source)
				{
					for (FCellRegistrationData& CRD : Records)
					{
						if (const FCellMetadata* Meta = Source.Find(CRD.Cell))
						{
							CRD.WorldBounds = Meta->WorldBounds;
						}
						else
						{
							CRD.WorldBounds = FBox(ForceInit);
						}
					}
				};
				AssignBounds(ModelData->ShellCells,    Wrapper->ShellCells);
				AssignBounds(ModelData->InteriorCells, Wrapper->InteriorCells);
			}
			else
			{
				UE_LOG(LogFragments, Warning,
					TEXT("[RegisterNewComponents] No wrapper for ModelGuid=%s; cells will use fallback bounds."),
					*ModelData->ModelGuid);
			}
		}

		// Mirror RegisterModel's init for newly-added interior cells.
		for (FCellRegistrationData& CRD : ModelData->ShellCells)
		{
			if (!CRD.MergedComponent && !CRD.bMergeInFlight)
			{
				CRD.bMergeInFlight = true;
				CRD.LodState = ECellLodState::Merged_LOD1;
			}
		}
		for (FCellRegistrationData& CRD : ModelData->InteriorCells)
		{
			if (!CRD.MergedComponent && !CRD.bMergeInFlight)
			{
				CRD.bMergeInFlight = true;
				CRD.LodState = ECellLodState::Merged_LOD1;
			}
		}

		// Interior pass. Re-walks shell HISMCs (no-op via AddUnique).
		RebuildCellHISMCIndex(*ModelData);

		UE_LOG(LogFragments, Verbose, TEXT("RegisterNewComponents: Added %d new HISMCs for model %s (total: %d; ShellCells=%d InteriorCells=%d HISMCs=%d)"),
			NewCount, *BucketRoot->GetModelGuid(), ModelData->Components.Num(),
			ModelData->ShellCells.Num(), ModelData->InteriorCells.Num(), ModelData->AllHISMCs.Num());
		bForceNextUpdate = true;
	}
}

FModelRegistrationData* UFragmentRegistrationManager::FindModelByGuid(const FString& ModelGuid)
{
	for (FModelRegistrationData& Data : RegisteredModels)
	{
		// Skip orphans.
		if (!IsValid(Data.BucketRoot)) continue;
		if (Data.ModelGuid == ModelGuid) return &Data;
	}
	return nullptr;
}

bool UFragmentRegistrationManager::PromoteCellAndRetryHit(const FHitResult& Hit)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_PromoteCellAndRetry);

	if (!Hit.Component.IsValid()) return false;

	UStaticMeshComponent* HitMerged = Cast<UStaticMeshComponent>(Hit.Component.Get());
	if (!HitMerged) return false;
	if (!HitMerged->ComponentHasTag(FName(TEXT("FragCellMerge")))) return false;

	// Find the cell that owns this merged component.
	for (FModelRegistrationData& Model : RegisteredModels)
	{
		auto FindAndPromote = [&, HitMerged](TArray<FCellRegistrationData>& Table) -> bool
		{
			for (FCellRegistrationData& CRD : Table)
			{
				if (CRD.MergedComponent.Get() == HitMerged)
				{
					// Force trace collision on promoted HISMCs for the next-frame retry.
					UWorld* WorldForCook = GetWorld();
					for (UHierarchicalInstancedStaticMeshComponent* H : CRD.HISMCs)
					{
						if (!IsValid(H)) continue;
						CookHISMCBodyIfNeeded(H, WorldForCook);
						for (FHISMCRegistrationState& S : Model.Components)
						{
							if (S.Component.Get() == H)
							{
								S.bForceCollision = true;
								if (S.Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
								{
									S.Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
								}
								break;
							}
						}
					}

					// Do NOT pre-mutate Cell.LodState — ApplyCellTransition's "Target == Cell.LodState" early-return would then skip the proper transition branch and leave the merged mesh visible.
					CRD.HotPromoteOverride = true;
					bForceNextUpdate = true;
					return true;
				}
			}
			return false;
		};

		if (FindAndPromote(Model.ShellCells))    return true;
		if (FindAndPromote(Model.InteriorCells)) return true;
	}

	return false;
}

bool UFragmentRegistrationManager::PromoteModelFromProxyHit(const FHitResult& Hit)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_PromoteModelFromProxyHit);

	if (!Hit.Component.IsValid()) return false;
	if (!Hit.Component->ComponentHasTag(FName(TEXT("HLODProxy")))) return false;

	const AFragment* HitRoot = Cast<AFragment>(Hit.Component->GetOwner());
	if (!HitRoot) return false;

	FModelRegistrationData* Model = FindModelByGuid(HitRoot->GetModelGuid());
	if (!Model) return false;

	Model->bPromoteOverride = true;
	bForceNextUpdate = true;   // bypass the still-camera early-out; the click IS the motion
	return true;
}

namespace
{
	/** Clear HotPromoteOverride + bForceCollision on every HISMC in `Cells` that the model knows about. */
	void ClearForceCollisionForCells(
		TArray<FCellRegistrationData>& Cells,
		FModelRegistrationData& Model)
	{
		for (FCellRegistrationData& Cell : Cells)
		{
			Cell.HotPromoteOverride = false;
			for (UHierarchicalInstancedStaticMeshComponent* H : Cell.HISMCs)
			{
				if (!IsValid(H)) continue;
				for (FHISMCRegistrationState& S : Model.Components)
				{
					if (S.Component.Get() == H)
					{
						S.bForceCollision = false;
						break;
					}
				}
			}
		}
	}
}

void UFragmentRegistrationManager::ClearAllHotPromoteOverrides()
{
	for (FModelRegistrationData& Model : RegisteredModels)
	{
		ClearForceCollisionForCells(Model.ShellCells,    Model);
		ClearForceCollisionForCells(Model.InteriorCells, Model);
		Model.bPromoteOverride = false;
	}
	bForceNextUpdate = true;
}

bool UFragmentRegistrationManager::GetLOD0BuildProgress(
	const FString& ModelGuid, int32& OutCellsBuilt, int32& OutCellsTotal) const
{
	OutCellsBuilt = 0;
	OutCellsTotal = 0;
	return false;
}

bool UFragmentRegistrationManager::HasModelPromoteOverride(const FString& ModelGuid) const
{
	const FModelRegistrationData* Model =
		const_cast<UFragmentRegistrationManager*>(this)->FindModelByGuid(ModelGuid);
	return Model && Model->bPromoteOverride;
}

void UFragmentRegistrationManager::PromoteAllCellsForModel(const FString& ModelGuid)
{
	FModelRegistrationData* Model = FindModelByGuid(ModelGuid);
	if (!Model) return;

	UWorld* WorldForCook = GetWorld();

	// Promote to LOD0 AND force trace collision on every cell.
	Model->bPromoteOverride = true;

	auto PromoteCells = [&](TArray<FCellRegistrationData>& Table)
	{
		for (FCellRegistrationData& Cell : Table)
		{
			Cell.HotPromoteOverride = true;
			for (UHierarchicalInstancedStaticMeshComponent* H : Cell.HISMCs)
			{
				if (!IsValid(H)) continue;
				CookHISMCBodyIfNeeded(H, WorldForCook);
				for (FHISMCRegistrationState& S : Model->Components)
				{
					if (S.Component.Get() == H)
					{
						S.bForceCollision = true;
						if (S.Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
						{
							S.Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
						}
						break;
					}
				}
			}
		}
	};

	PromoteCells(Model->ShellCells);
	PromoteCells(Model->InteriorCells);
	bForceNextUpdate = true;
}

void UFragmentRegistrationManager::ClearHotPromoteOverridesForModel(const FString& ModelGuid)
{
	FModelRegistrationData* Model = FindModelByGuid(ModelGuid);
	if (!Model) return;

	// Clear only this model’s cell and proxy promotion overrides.
	ClearForceCollisionForCells(Model->ShellCells,    *Model);
	ClearForceCollisionForCells(Model->InteriorCells, *Model);
	Model->bPromoteOverride = false;

	// The next pass has to re-decide every cell -- their target states just changed without the camera moving, which is exactly what the still-camera early-out would otherwise skip.
	bForceNextUpdate = true;
}

bool UFragmentRegistrationManager::DemoteCellByHandle(const FFragmentCellHandle& Handle)
{
	if (!Handle.IsValid()) return false;

	for (FModelRegistrationData& Model : RegisteredModels)
	{
		if (!IsValid(Model.BucketRoot)) continue;
		if (Model.BucketRoot->GetModelGuid() != Handle.ModelGuid) continue;

		TArray<FCellRegistrationData>& Cells = Handle.bIsShell ? Model.ShellCells : Model.InteriorCells;
		for (FCellRegistrationData& Cell : Cells)
		{
			if (Cell.Cell.Floor != Handle.Floor) continue;
			if (Cell.Cell.GridX != Handle.GridX) continue;
			if (Cell.Cell.GridY != Handle.GridY) continue;

			// Same tier-dependent teardown split as ClearForceCollisionForCells: clearing bForceCollision is enough at Cells tier, where EvaluateOneHISMC drops the collision on the next tick.
			Cell.HotPromoteOverride = false;
			for (UHierarchicalInstancedStaticMeshComponent* H : Cell.HISMCs)
			{
				if (!IsValid(H)) continue;
				for (FHISMCRegistrationState& S : Model.Components)
				{
					if (S.Component.Get() == H)
					{
						S.bForceCollision = false;
						break;
					}
				}
			}
			bForceNextUpdate = true;
			return true;
		}
	}
	return false;
}

FFragmentCellHandle UFragmentRegistrationManager::MakeHandleForHISMC(
	UHierarchicalInstancedStaticMeshComponent* HISM) const
{
	FFragmentCellHandle Handle;
	if (!IsValid(HISM)) return Handle;

	for (const FModelRegistrationData& Model : RegisteredModels)
	{
		// Skip models mid-teardown so we never return a partial handle (Floor/GridX/etc. populated but ModelGuid empty) that would IsValid()==false yet confuse a caller who naively reads its fields.
		if (!IsValid(Model.BucketRoot)) continue;

		for (const FHISMCRegistrationState& S : Model.Components)
		{
			if (S.Component.Get() == HISM)
			{
				Handle.ModelGuid = Model.BucketRoot->GetModelGuid();
				Handle.Floor    = S.CellId.Floor;
				Handle.GridX    = S.CellId.GridX;
				Handle.GridY    = S.CellId.GridY;
				Handle.bIsShell = S.bIsShell;
				return Handle;
			}
		}
	}
	return Handle;
}

void UFragmentRegistrationManager::UpdateRegistration(
	TConstArrayView<FFragmentViewInfo> Views, UWorld* World, float CollisionCookBudgetOverrideMs)
{
	// Empty is a no-op, NOT a cull-everything pass. See FragmentCellLodCombine.h.
	if (Views.IsEmpty() || !World)
	{
		return;
	}

	const bool bForcedThisTick = bForceNextUpdate;
	bForceNextUpdate = false;

	if (ShouldSkipForStillCamera(Views, bForcedThisTick))
	{
		return;
	}

	const float NowSeconds = World->GetTimeSeconds();

	int32 CollisionUncookBudgetThisFrame = 0;
	// Time-based budget for chunked collision cooking (shared across all models)
	double CollisionCookTimeSpentMs = 0.0;
	// Non-negative override caps the fixed budget so cooking shares the per-frame ceiling with finalize drain and HISM spawn.
	const float EffectiveCookBudgetMs =
		(CollisionCookBudgetOverrideMs >= 0.0f)
			? FMath::Min(CollisionCookBudgetMs, CollisionCookBudgetOverrideMs)
			: CollisionCookBudgetMs;

	const float ShellPromoteCm    = CVarShellLOD0PromoteCm.GetValueOnGameThread();
	const float InteriorPromoteCm = CVarInteriorLOD0PromoteCm.GetValueOnGameThread();
	const float HysteresisCm      = CVarLOD0HysteresisCm.GetValueOnGameThread();

	// Continuous model-tier criterion (LOD2 proxy <-> LOD1 cells).
	const float GeoCoeffCells = CVarLODGeoCoeffCells.GetValueOnGameThread();
	const float SseThreshold  = CVarLODSSEPixelError.GetValueOnGameThread();
	const float CoarsenFactor = CVarLODCoarsenFactor.GetValueOnGameThread();

	const int32 ModelCapOverride = CVarModelTierTransitionsPerTick.GetValueOnGameThread();
	const int32 ModelTierCap = FMath::Max(
		(ModelCapOverride >= 0) ? ModelCapOverride : MaxModelTierTransitionsPerTick, 1);
	int32 ModelTierTransitionsApplied = 0;

	// Global per-tick cap; HotPromote + bBypassTransitionCapNextTick bypass.
	const int32 CVarOverride = CVarCellTransitionsPerTick.GetValueOnGameThread();
	const int32 RawCap = (CVarOverride >= 0) ? CVarOverride : MaxCellTransitionsPerTick;
	const bool bBypassCapThisTick = bBypassTransitionCapNextTick;
	bBypassTransitionCapNextTick = false;
	const int32 TransitionCap = bBypassCapThisTick ? TNumericLimits<int32>::Max() : FMath::Max(RawCap, 1);
	int32 TransitionsApplied = 0;
	bool bAnyCellDeferred = false;

	auto IterateCells = [&](FModelRegistrationData& Model, TArray<FCellRegistrationData>& Cells, float PromoteCm)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CellLoop);
		TArray<ECellLodState, TInlineAllocator<4>> PerView;
		for (FCellRegistrationData& Cell : Cells)
		{
			PerView.Reset();
			for (const FFragmentViewInfo& V : Views)
			{
				PerView.Add(DecideCellTargetState(
					Cell, V.CameraLocation, V.ViewFrustum,
					bEnableFrustumCulling && V.bHaveFrustum, PromoteCm, HysteresisCm));
			}

			ECellLodState Target;
			if (!UE::Fragments::CellLod::CombineTargetStates(PerView, Target)) continue;

			if (Target == Cell.LodState) continue;

			const bool bIsHotPromote = Cell.HotPromoteOverride;
			if (!bIsHotPromote && TransitionsApplied >= TransitionCap)
			{
				bAnyCellDeferred = true;
				continue;
			}

			if (ApplyCellTransition(Cell, Model, Target) && !bIsHotPromote)
			{
				++TransitionsApplied;
			}
		}
	};

	for (FModelRegistrationData& ModelData : RegisteredModels)
	{
		if (!IsValid(ModelData.BucketRoot)) continue;

		// Nearest view wins for every per-model decision below: it is the most demanding, which matches the most-detailed-wins rule CombineTargetStates applies to cells.
		int32 NearestViewIdx = 0;
		float DistToBox = TNumericLimits<float>::Max();
		float SmallestExtent = 0.0f;
		if (ModelData.ModelBoundsRadius > 0.0f)
		{
			const FVector BoxMin = ModelData.ModelBoundsOrigin - ModelData.ModelBoundsExtent;
			const FVector BoxMax = ModelData.ModelBoundsOrigin + ModelData.ModelBoundsExtent;
			for (int32 i = 0; i < Views.Num(); ++i)
			{
				const FVector ClosestPoint =
					Views[i].CameraLocation.ComponentMax(BoxMin).ComponentMin(BoxMax);
				const float D = FVector::Dist(Views[i].CameraLocation, ClosestPoint);
				if (D < DistToBox) { DistToBox = D; NearestViewIdx = i; }
			}
			SmallestExtent = FMath::Min(ModelData.ModelBoundsExtent.X, ModelData.ModelBoundsExtent.Y);
		}
		const FFragmentViewInfo& ModelView = Views[NearestViewIdx];

		// Continuous model tier: LOD2 proxy <-> LOD1 cells --- Frozen while user-hidden: every transition shows its target tier, so deciding here would re-show a hidden model.
		if (ModelData.bUserVisible && ModelData.ModelBoundsRadius > 0.0f)
		{
			FragmentModelTier::FDecisionInputs In;
			In.ModelBoundsRadiusCm     = ModelData.ModelBoundsRadius;
			In.DistToBoxCm             = DistToBox;
			In.SseCoeff                = ModelView.SseCoeff;
			In.GeoCoeff                = GeoCoeffCells;
			In.ThresholdPx             = SseThreshold;
			In.CoarsenFactor           = CoarsenFactor;
			In.bAnyCellFloorFiltered   = (ModelData.FloorFilteredCellCount > 0);
			In.bPromoteOverride        = ModelData.bPromoteOverride;
			In.bProxyAvailable         = IsValid(ModelData.ProxyComponent);
			In.Current                 = ModelData.Tier;

			const FragmentModelTier::ETier Target = FragmentModelTier::Decide(In);
			if (Target != ModelData.Tier)
			{
				// The click override bypasses the cap: a user waiting on their own click must not queue behind camera-driven promotions.
				if (ModelData.bPromoteOverride || ModelTierTransitionsApplied < ModelTierCap)
				{
					if (ApplyModelTierTransition(ModelData, Target) && !ModelData.bPromoteOverride)
					{
						++ModelTierTransitionsApplied;
					}
				}
				else
				{
					// Deferred — force next tick so a still camera doesn't strand it.
					bAnyCellDeferred = true;
				}
			}
		}

		// Deferred collision cooking trigger --- ABOVE the Proxy-tier early-out on purpose.
		if (ModelData.ModelBoundsRadius > 0.0f)
		{
			const float CookPadding = SmallestExtent * CollisionTriggerMultiplier;
			const float UncookPadding = CookPadding * CollisionUncookHysteresis;

			if (!ModelData.bCollisionCooked && DistToBox < CookPadding)
			{
				// Start chunked cooking if not already in progress
				if (ModelData.CollisionCookProgress < 0)
				{
					ModelData.CollisionCookProgress = 0;
					ModelData.CollisionUncookEligibleTime = 0.0f;
					const FString ModelGuid = ModelData.BucketRoot
						? ModelData.BucketRoot->GetModelGuid() : TEXT("unknown");
					UE_LOG(LogFragments, Verbose,
						TEXT("[Collision] Starting chunked cook for model %s (%d HISMCs, dist=%.0f cm, padding=%.0f cm)"),
						*ModelGuid, ModelData.Components.Num(), DistToBox, CookPadding);
				}
			}

			// Chunked collision cooking — process HISMCs within time budget
			if (ModelData.CollisionCookProgress >= 0
				&& ModelData.CollisionCookProgress < ModelData.Components.Num()
				&& CollisionCookTimeSpentMs < EffectiveCookBudgetMs)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_CookCollision);
				const double ChunkStartSeconds = FPlatformTime::Seconds();

				while (ModelData.CollisionCookProgress < ModelData.Components.Num()
					&& CollisionCookTimeSpentMs < EffectiveCookBudgetMs)
				{
					FHISMCRegistrationState& State = ModelData.Components[ModelData.CollisionCookProgress];
					ModelData.CollisionCookProgress++;

					if (!IsValid(State.Component)) continue;
					UStaticMesh* Mesh = State.Component->GetStaticMesh();
					if (!Mesh || !Mesh->GetBodySetup()) continue;

					UBodySetup* BS = Mesh->GetBodySetup();

					// Skip meshes pre-cooked at import time.
					if (BS->bCreatedPhysicsMeshes)
					{
						continue;
					}

					// Fallback path (unchanged): runtime cook on game thread.
					UObject* OrigOuter = BS->GetOuter();
					if (World)
					{
						BS->Rename(nullptr, World);
					}
					BS->InvalidatePhysicsData();
					BS->CreatePhysicsMeshes();
					if (OrigOuter && OrigOuter != World)
					{
						BS->Rename(nullptr, OrigOuter);
					}

					// Update time spent (check after each HISMC to stay within budget)
					CollisionCookTimeSpentMs = (FPlatformTime::Seconds() - ChunkStartSeconds) * 1000.0;
				}

				// Check if cooking is complete for this model
				if (ModelData.CollisionCookProgress >= ModelData.Components.Num())
				{
					ModelData.bCollisionCooked = true;
					ModelData.CollisionCookProgress = -1;
					const FString ModelGuid = ModelData.BucketRoot
						? ModelData.BucketRoot->GetModelGuid() : TEXT("unknown");
					UE_LOG(LogFragments, Verbose,
						TEXT("[Collision] Finished cooking physics for model %s (%d HISMCs)"),
						*ModelGuid, ModelData.Components.Num());
				}
			}
			else if (!ModelData.bCollisionCooked && ModelData.CollisionCookProgress >= 0
				&& DistToBox >= CookPadding)
			{
				// Camera left cook zone while cooking was in progress — cancel
				ModelData.CollisionCookProgress = -1;
				UE_LOG(LogFragments, Verbose, TEXT("[Collision] Cancelled in-progress cook (camera left cook zone)"));
			}
			else if (ModelData.bCollisionCooked && DistToBox > UncookPadding)
			{
				// Camera exited uncook zone — start or check cooldown
				if (ModelData.CollisionUncookEligibleTime == 0.0f)
				{
					// Start cooldown timer
					ModelData.CollisionUncookEligibleTime = NowSeconds + CollisionCooldownSeconds;
				}
				else if (NowSeconds >= ModelData.CollisionUncookEligibleTime)
				{
					// Cooldown expired — uncook if budget allows
					if (CollisionUncookBudgetThisFrame < MaxCollisionUncooksPerFrame)
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_UncookCollision);

						// Build set of meshes still needed by other cooked models
						TSet<UStaticMesh*> ProtectedMeshes;
						for (const FModelRegistrationData& OtherModel : RegisteredModels)
						{
							if (&OtherModel == &ModelData) continue;
							if (!OtherModel.bCollisionCooked) continue;
							for (const FHISMCRegistrationState& OtherState : OtherModel.Components)
							{
								if (!IsValid(OtherState.Component)) continue;
								if (UStaticMesh* OtherMesh = OtherState.Component->GetStaticMesh())
								{
									ProtectedMeshes.Add(OtherMesh);
								}
							}
						}

						for (FHISMCRegistrationState& State : ModelData.Components)
						{
							if (!IsValid(State.Component)) continue;
							State.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
							UStaticMesh* Mesh = State.Component->GetStaticMesh();
							if (Mesh && Mesh->GetBodySetup() && !ProtectedMeshes.Contains(Mesh))
							{
								Mesh->GetBodySetup()->InvalidatePhysicsData();
							}
						}
						ModelData.bCollisionCooked = false;
						ModelData.CollisionCookProgress = -1;
						ModelData.CollisionUncookEligibleTime = 0.0f;
						CollisionUncookBudgetThisFrame++;

						const FString ModelGuid = ModelData.BucketRoot
							? ModelData.BucketRoot->GetModelGuid() : TEXT("unknown");
						UE_LOG(LogFragments, Verbose,
							TEXT("[Collision] Uncooked physics for model %s (%d HISMCs, %d meshes protected by other models)."),
							*ModelGuid, ModelData.Components.Num(), ProtectedMeshes.Num());
					}
				}
			}
			else if (ModelData.bCollisionCooked && DistToBox <= UncookPadding
				&& ModelData.CollisionUncookEligibleTime != 0.0f)
			{
				// Camera returned to uncook zone — cancel pending uncook
				ModelData.CollisionUncookEligibleTime = 0.0f;
			}
		}

		// Proxy-tier and user-hidden models need no per-cell or per-HISMC work --- Hidden: IterateCells would re-show cells and EvaluateOneHISMC would re-arm LOD0 collision.
		if (!ModelData.bUserVisible || ModelData.Tier == FragmentModelTier::ETier::Proxy)
		{
			continue;
		}

		// Shell-first: interior loading trigger
		if (ModelData.bInteriorsPending && !ModelData.bInteriorsTriggered && ModelData.ModelBoundsRadius > 0.0f)
		{
			const float TriggerPadding = SmallestExtent * InteriorTriggerRadiusMultiplier;

			if (DistToBox < TriggerPadding)
			{
				ModelData.bInteriorsTriggered = true;
				const FString TriggerModelGuid = (ModelData.BucketRoot != nullptr)
					? ModelData.BucketRoot->GetModelGuid() : TEXT("unknown");
				UE_LOG(LogFragments, Verbose, TEXT("[Shell-First] Camera within trigger box for model %s (dist=%.0f cm from box, padding=%.0f cm, extent=(%.0f, %.0f, %.0f)). Requesting interior load."),
					*TriggerModelGuid, DistToBox, TriggerPadding,
					ModelData.ModelBoundsExtent.X, ModelData.ModelBoundsExtent.Y, ModelData.ModelBoundsExtent.Z);
				OnInteriorLoadRequested.Broadcast(TriggerModelGuid);
			}
		}

		// Cell-first iteration with per-cell LOD state machine. === Shell and interior cells use independent promote distances, since interior detail only matters up close while shells stay readable from far.
		IterateCells(ModelData, ModelData.ShellCells,    ShellPromoteCm);
		IterateCells(ModelData, ModelData.InteriorCells, InteriorPromoteCm);

		// Per-HISMC maintenance loop — glass-occluder flipping + spatial collision gating only.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_HISMCMaintain);
			for (int32 CompIdx : ModelData.AllHISMCs)
			{
				FHISMCRegistrationState& State = ModelData.Components[CompIdx];
				if (!IsValid(State.Component)) continue;
				EvaluateOneHISMC(State, ModelData, CompIdx, ModelView.CameraLocation);
			}
		}
	}

	// Force next tick so still-camera skip doesn't strand deferred transitions.
	if (bAnyCellDeferred)
	{
		bForceNextUpdate = true;
	}

	TRACE_COUNTER_SET(FragRegMgr_CellTransitionsAppliedThisTick, TransitionsApplied);

	// Apply queued glass occluder flips (rate-limited).
	DrainPendingGlassFlips();
}

bool UFragmentRegistrationManager::ShouldSkipForStillCamera(
	TConstArrayView<FFragmentViewInfo> Views, bool bForcedThisTick)
{
	// A viewport opening or closing changes what is on screen even if no camera moved.
	bool bCameraMoved = !bHasLastCameraState || (LastViewStates.Num() != Views.Num());

	if (!bCameraMoved)
	{
		for (int32 i = 0; i < Views.Num(); ++i)
		{
			const float PosDelta = FVector::Dist(Views[i].CameraLocation, LastViewStates[i].Location);
			const float YawDelta = FMath::Abs(FRotator::NormalizeAxis(
				Views[i].CameraRotation.Yaw - LastViewStates[i].Rotation.Yaw));
			const float PitchDelta = FMath::Abs(FRotator::NormalizeAxis(
				Views[i].CameraRotation.Pitch - LastViewStates[i].Rotation.Pitch));
			const float RollDelta = FMath::Abs(FRotator::NormalizeAxis(
				Views[i].CameraRotation.Roll - LastViewStates[i].Rotation.Roll));
			const float MaxRotDelta = FMath::Max3(YawDelta, PitchDelta, RollDelta);

			// ANY view moving is movement -- the pass serves all of them.
			if (PosDelta >= CameraStillPositionEpsilonCm || MaxRotDelta >= CameraStillRotationEpsilonDeg)
			{
				bCameraMoved = true;
				break;
			}
		}
	}

	// Skip the entire update if no camera moved. Avoids HZB-noise shadow popping on a still camera.
	if (bSkipUpdateWhenCameraStill && !bForcedThisTick && !bCameraMoved)
	{
		return true;
	}

	LastViewStates.Reset(Views.Num());
	for (const FFragmentViewInfo& V : Views)
	{
		LastViewStates.Add({ V.CameraLocation, V.CameraRotation });
	}
	bHasLastCameraState = true;
	return false;
}

void UFragmentRegistrationManager::ExtractViewInfo(APlayerController* PC, FFragmentViewInfo& OutView) const
{
	// Pull the engine's actual view frustum and projection directly from the local player.
	OutView.CameraLocation = FVector::ZeroVector;
	OutView.bHaveFrustum = false;

	// Fallback ~1080p @ 90deg FOV: proj[1][1] = 1/tan(45deg) = 1, height = 1080.
	OutView.SseCoeff = FragmentSSE::SseCoeff(1080.0f, 1.0f);

	if (ULocalPlayer* LP = Cast<ULocalPlayer>(PC->Player))
	{
		FSceneViewProjectionData ProjectionData;
		if (LP->ViewportClient && LP->GetProjectionData(LP->ViewportClient->Viewport, ProjectionData))
		{
			OutView.CameraLocation = ProjectionData.ViewOrigin;
			const FMatrix ViewProj = ProjectionData.ComputeViewProjectionMatrix();
			GetViewFrustumBounds(OutView.ViewFrustum, ViewProj, /*bUseNearPlane=*/false);
			OutView.bHaveFrustum = true;

			const FIntRect Rect = ProjectionData.GetConstrainedViewRect();
			const float ViewportHeightPx = static_cast<float>(Rect.Height());
			const float ProjMatrix11 = static_cast<float>(ProjectionData.ProjectionMatrix.M[1][1]);
			if (ViewportHeightPx > 0.0f && ProjMatrix11 > 0.0f)
			{
				OutView.SseCoeff = FragmentSSE::SseCoeff(ViewportHeightPx, ProjMatrix11);
			}
		}
	}

	if (!OutView.bHaveFrustum)
	{
		// Fallback: distance-only decisions; SseCoeff already holds the default above.
		PC->GetPlayerViewPoint(OutView.CameraLocation, OutView.CameraRotation);
	}
	else
	{
		// Read camera rotation each tick so in-place turns refresh frustum culling.
		FVector UnusedLocation;
		PC->GetPlayerViewPoint(UnusedLocation, OutView.CameraRotation);
	}
}

void UFragmentRegistrationManager::EvaluateOneHISMC(
	FHISMCRegistrationState& State,
	FModelRegistrationData& ModelData,
	int32 CompIdx,
	const FVector& CameraLocation)
{
	// Distance from camera to bounds surface — drives both glass-flip threshold and spatial collision gating.
	const float Distance = FVector::Dist(CameraLocation, State.CachedBoundsOrigin) - State.CachedBoundsRadius;
	const float ClampedDistance = FMath::Max(0.0f, Distance);

	const bool bHasCullDistance = State.CullDistance > 0.0f;

	// Enable glass HZB occlusion beyond the distance threshold.
	if (bEnableGlassOccluderFlip
		&& State.OccluderState != EGlassOccluderState::NotGlass)
	{
		EvaluateGlassFlip(State, ClampedDistance);
	}

	// Spatial collision gating: enable QueryOnly collision only when the camera is within interaction distance.
	const bool bThisHISMCCooked = ModelData.bCollisionCooked
		|| (ModelData.CollisionCookProgress > CompIdx);
	if (bHasCullDistance)
	{
		const float InteractDistance = State.CullDistance * CollisionActivationFraction;
		// bForceCollision overrides the distance gate for HISMCs owned by a hot-promoted cell so the click-retry trace can hit them at range.
		const bool bShouldHaveCollision =
			(ClampedDistance <= InteractDistance) || State.bForceCollision;
		const ECollisionEnabled::Type Current = State.Component->GetCollisionEnabled();

		// Activation requires a cooked body — otherwise SetCollisionEnabled flips a flag with no physics shape behind it.
		if (bShouldHaveCollision && bThisHISMCCooked && Current == ECollisionEnabled::NoCollision)
		{
			State.Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		}
		else if (!bShouldHaveCollision && Current != ECollisionEnabled::NoCollision)
		{
			State.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	// Shadow policy is baked once at construction (FragmentsImporter::FinalizeSingleHISMBucket) — no per-frame toggling.
}

void UFragmentRegistrationManager::EvaluateGlassFlip(FHISMCRegistrationState& State, float DistanceToCamera)
{
	if (State.CullDistance <= 0.0f)
	{
		return;
	}

	const float EnableThreshold  = State.CullDistance * GlassOccluderEnableFraction;
	const float DisableThreshold = State.CullDistance * GlassOccluderDisableFraction;

	EGlassOccluderState DesiredState = State.OccluderState;
	float DistanceFromThreshold = 0.0f;

	// Hysteresis: flip to occluding past EnableThreshold, flip back inside DisableThreshold.
	if (State.OccluderState == EGlassOccluderState::Occluding)
	{
		if (DistanceToCamera < DisableThreshold)
		{
			DesiredState = EGlassOccluderState::Transparent;
			DistanceFromThreshold = DisableThreshold - DistanceToCamera;
		}
	}
	else
	{
		if (DistanceToCamera > EnableThreshold)
		{
			DesiredState = EGlassOccluderState::Occluding;
			DistanceFromThreshold = DistanceToCamera - EnableThreshold;
		}
	}

	if (DesiredState != State.OccluderState)
	{
		FPendingGlassFlip Pending;
		Pending.Component = State.Component;
		Pending.DesiredState = DesiredState;
		Pending.DistanceFromThreshold = DistanceFromThreshold;
		PendingGlassFlips.Add(Pending);
	}
}

void UFragmentRegistrationManager::DrainPendingGlassFlips()
{
	if (PendingGlassFlips.Num() == 0) return;

	// Largest distance-from-threshold first so the most overdue flips win the budget.
	PendingGlassFlips.Sort([](const FPendingGlassFlip& A, const FPendingGlassFlip& B)
	{
		return A.DistanceFromThreshold > B.DistanceFromThreshold;
	});

	const int32 FlipsThisFrame = FMath::Min(PendingGlassFlips.Num(), MaxGlassFlipsPerFrame);

	int32 AppliedFlips = 0;
	for (int32 i = 0; i < FlipsThisFrame; i++)
	{
		UHierarchicalInstancedStaticMeshComponent* Comp = PendingGlassFlips[i].Component;
		if (!IsValid(Comp)) continue;

		const bool bDesiredOccluding = (PendingGlassFlips[i].DesiredState == EGlassOccluderState::Occluding);
		if (Comp->bUseAsOccluder != bDesiredOccluding)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_GlassFlip);
			Comp->bUseAsOccluder = bDesiredOccluding;
			Comp->MarkRenderStateDirty();
		}

		// Find the state entry by Component* and update its OccluderState.
		for (FModelRegistrationData& ModelData : RegisteredModels)
		{
			for (FHISMCRegistrationState& State : ModelData.Components)
			{
				if (State.Component == Comp)
				{
					State.OccluderState = PendingGlassFlips[i].DesiredState;
					goto NextFlip;
				}
			}
		}
		NextFlip:;
		AppliedFlips++;
	}

	UE_LOG(LogFragments, Verbose,
		TEXT("GlassFlip: applied %d/%d (budget %d)"),
		AppliedFlips, PendingGlassFlips.Num(), MaxGlassFlipsPerFrame);

	PendingGlassFlips.Reset();
}

bool UFragmentRegistrationManager::SetModelUserVisible(const FString& ModelGuid, bool bVisible)
{
	FModelRegistrationData* ModelData = FindModelByGuid(ModelGuid);
	if (!ModelData || !IsValid(ModelData->BucketRoot))
	{
		// Not registered yet (still loading): the request is discarded, not recorded.
		UE_LOG(LogFragments, Verbose,
			TEXT("[Visibility] SetModelUserVisible(%s, %d) ignored: model not registered"),
			*ModelGuid, bVisible ? 1 : 0);
		return false;
	}

	ModelData->bUserVisible = bVisible;

	// Actor-level flags are the game-world backstop: owner-hidden primitives do not render and owner-collision-disabled primitives do not trace.
	ModelData->BucketRoot->SetActorHiddenInGame(!bVisible);
	ModelData->BucketRoot->SetActorEnableCollision(bVisible);

	if (!bVisible)
	{
		// Every tier at once, whatever the current one is.
		SetProxyShown(ModelData->ProxyComponent, false);

		auto HideCells = [](TArray<FCellRegistrationData>& Cells)
		{
			for (FCellRegistrationData& Cell : Cells)
			{
				SetCellHISMCsVisible(Cell, false);
				SetMergedShown(Cell.MergedComponent, false);
			}
		};
		HideCells(ModelData->ShellCells);
		HideCells(ModelData->InteriorCells);

		// Components is the model's authoritative HISMC set, including cells whose merge is still in flight or failed.
		for (FHISMCRegistrationState& State : ModelData->Components)
		{
			if (!IsValid(State.Component)) continue;
			State.Component->SetVisibility(false, false);
			State.Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}

		// With bUserVisible false this darkens every caster: proxy and merged cells alike.
		ApplyModelShadowOwnership(*ModelData, ModelData->Tier);

		UE_LOG(LogFragments, Verbose,
			TEXT("[Visibility] Model %s hidden in every tier; tier frozen"), *ModelGuid);
		return true;
	}

	ShowModelAtCurrentTier(*ModelData);

	// The tier was frozen while hidden and may be stale for the current view.
	bForceNextUpdate = true;
	bBypassTransitionCapNextTick = true;

	UE_LOG(LogFragments, Verbose,
		TEXT("[Visibility] Model %s shown at frozen tier %s; re-derive requested"), *ModelGuid,
		ModelData->Tier == FragmentModelTier::ETier::Cells ? TEXT("Cells") : TEXT("Proxy"));
	return true;
}

bool UFragmentRegistrationManager::IsModelUserVisible(const FString& ModelGuid) const
{
	// FindModelByGuid skips registrations whose bucket root is gone, so a reload of an unloaded GUID answers from its live entry, the same one SetModelUserVisible writes.
	const FModelRegistrationData* ModelData =
		const_cast<UFragmentRegistrationManager*>(this)->FindModelByGuid(ModelGuid);

	// Matches bUserVisible's default: nothing has hidden a model that is not registered yet.
	return ModelData ? ModelData->bUserVisible : true;
}

void UFragmentRegistrationManager::ShowModelAtCurrentTier(FModelRegistrationData& ModelData)
{
	check(ModelData.bUserVisible);

	if (ModelData.Tier == FragmentModelTier::ETier::Proxy)
	{
		// Cells stay dark at Proxy tier.
		ResetCellsForProxyMode(ModelData);
		SetProxyShown(ModelData.ProxyComponent, true);
	}
	else
	{
		SetProxyShown(ModelData.ProxyComponent, false);

		// Replay each cell's frozen LodState.
		auto RestoreCells = [&ModelData](TArray<FCellRegistrationData>& Cells)
		{
			for (FCellRegistrationData& Cell : Cells)
			{
				if (Cell.bFilterHidden || Cell.LodState == ECellLodState::Skipped)
				{
					SetCellHISMCsVisible(Cell, false);
					SetMergedShown(Cell.MergedComponent, false);
				}
				else if (Cell.LodState == ECellLodState::Active_LOD0)
				{
					EnsureCellHISMCsRegistered(Cell, ModelData);
					SetMergedShown(Cell.MergedComponent, false);
					SetCellHISMCsVisible(Cell, true);
				}
				else // Merged_LOD1
				{
					SetCellHISMCsVisible(Cell, false);
					SetMergedShown(Cell.MergedComponent, true);
				}
			}
		};
		RestoreCells(ModelData.ShellCells);
		RestoreCells(ModelData.InteriorCells);
	}

	ApplyModelShadowOwnership(ModelData, ModelData.Tier);
}

#if !UE_BUILD_SHIPPING
void UFragmentRegistrationManager::DumpVisibilitySnapshot()
{
	UE_LOG(LogFragments, Display, TEXT("=== Frag.DumpVisibility snapshot ==="));

	if (RegisteredModels.Num() == 0)
	{
		UE_LOG(LogFragments, Display, TEXT("(No models registered)"));
		return;
	}

	// Pick a UWorld from any model's BucketRoot for timestamps.
	UWorld* World = nullptr;
	for (const FModelRegistrationData& MD : RegisteredModels)
	{
		if (MD.BucketRoot && MD.BucketRoot->GetWorld()) { World = MD.BucketRoot->GetWorld(); break; }
	}
	const float NowSeconds = World ? World->GetTimeSeconds() : 0.0f;

	int32 GrandTotalHISMCs = 0;
	int32 GrandTotalRegistered = 0;
	int32 GrandTotalRendered = 0;
	int64 GrandInstancesReg = 0;
	int64 GrandInstancesRendered = 0;

	for (const FModelRegistrationData& ModelData : RegisteredModels)
	{
		const int32 Total = ModelData.Components.Num();
		int32 Registered = 0;
		int32 Rendered = 0;
		int32 RenderedStructural = 0;
		int32 RenderedGlass = 0;
		int32 RenderedOther = 0;
		int64 InstancesReg = 0;
		int64 InstancesRendered = 0;

		for (const FHISMCRegistrationState& State : ModelData.Components)
		{
			if (!IsValid(State.Component)) continue;
			const int32 InstCount = State.Component->GetInstanceCount();
			if (State.bIsRegistered)
			{
				Registered++;
				InstancesReg += InstCount;

				const float RenderTime = State.Component->GetLastRenderTimeOnScreen();
				if (RenderTime > NowSeconds - RecentlyRenderedThresholdSeconds)
				{
					Rendered++;
					InstancesRendered += InstCount;
					if (State.bIsStructural)            RenderedStructural++;
					else if (State.bIsOccludableGlass)  RenderedGlass++;
					else                                RenderedOther++;
				}
			}
		}

		GrandTotalHISMCs += Total;
		GrandTotalRegistered += Registered;
		GrandTotalRendered += Rendered;
		GrandInstancesReg += InstancesReg;
		GrandInstancesRendered += InstancesRendered;

		UE_LOG(LogFragments, Display, TEXT("Model: %s"), *ModelData.ModelGuid);
		UE_LOG(LogFragments, Display, TEXT("  Total HISMCs:        %d"), Total);
		UE_LOG(LogFragments, Display, TEXT("  Registered:          %d  (%.1f%%)"),
			Registered, Total > 0 ? (100.0f * Registered / Total) : 0.0f);
		UE_LOG(LogFragments, Display, TEXT("  Recently rendered:   %d  (%.1f%% of total, %.1f%% of registered)"),
			Rendered,
			Total > 0 ? (100.0f * Rendered / Total) : 0.0f,
			Registered > 0 ? (100.0f * Rendered / Registered) : 0.0f);
		UE_LOG(LogFragments, Display, TEXT("  Rendered tag breakdown:"));
		UE_LOG(LogFragments, Display, TEXT("    Structural:        %d  (red)"), RenderedStructural);
		UE_LOG(LogFragments, Display, TEXT("    GlassCurtainWall:  %d  (cyan)"), RenderedGlass);
		UE_LOG(LogFragments, Display, TEXT("    Other:             %d  (gray)"), RenderedOther);
		UE_LOG(LogFragments, Display, TEXT("  Instances in registered HISMCs:  %lld"), InstancesReg);
		UE_LOG(LogFragments, Display, TEXT("  Instances in rendered HISMCs:    %lld"), InstancesRendered);
	}

	if (RegisteredModels.Num() > 1)
	{
		UE_LOG(LogFragments, Display, TEXT("--- Aggregate ---"));
		UE_LOG(LogFragments, Display, TEXT("  Total HISMCs:        %d"), GrandTotalHISMCs);
		UE_LOG(LogFragments, Display, TEXT("  Registered:          %d"), GrandTotalRegistered);
		UE_LOG(LogFragments, Display, TEXT("  Recently rendered:   %d"), GrandTotalRendered);
		UE_LOG(LogFragments, Display, TEXT("  Instances in registered HISMCs:  %lld"), GrandInstancesReg);
		UE_LOG(LogFragments, Display, TEXT("  Instances in rendered HISMCs:    %lld"), GrandInstancesRendered);
	}

	UE_LOG(LogFragments, Display, TEXT("(Cross-reference with `stat scenerendering` for engine-side counts.)"));
}
#endif

void UFragmentRegistrationManager::RestoreProxyInteractionState(const FString& ModelGuid)
{
	FModelRegistrationData* ModelData = FindModelByGuid(ModelGuid);
	if (!ModelData || !IsValid(ModelData->BucketRoot))
	{
		return;
	}

	// This helper is only meant for a model already in the Proxy tier.
	if (ModelData->Tier != FragmentModelTier::ETier::Proxy)
	{
		return;
	}

	// Nor may it un-hide a user-hidden model.
	if (!ModelData->bUserVisible)
	{
		return;
	}

	if (!IsValid(ModelData->ProxyComponent))
	{
		return;
	}

	if (!ModelData->ProxyComponent->IsRegistered())
	{
		ModelData->ProxyComponent->RegisterComponent();
	}

	ModelData->ProxyComponent->SetHiddenInGame(false);
	ModelData->ProxyComponent->SetVisibility(true);

	// Reapply the exact proxy collision setup used when the proxy is first created.
	ModelData->ProxyComponent->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	ModelData->ProxyComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ModelData->ProxyComponent->SetCollisionResponseToAllChannels(ECR_Overlap);
	ModelData->ProxyComponent->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	// Force next registration tick so any proxy/full visibility bookkeeping stays aligned.
	bForceNextUpdate = true;
}

// Cell-merge finalize callbacks.

// DIAGNOSTIC SCOPE on the whole function: called once per cell as each merge lands, so a burst of interior-cell commits shows up as a cluster of these in one frame.
void UFragmentRegistrationManager::AttachMergedComponent(
	const FString& ModelGuid, const FCellId& Cell, bool bShellSide, UStaticMesh* MergedMesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_AttachMergedComponent);
	check(IsInGameThread());
	if (!IsValid(MergedMesh)) return;

	FModelRegistrationData* Model = FindModelByGuid(ModelGuid);
	if (!Model || !IsValid(Model->BucketRoot))
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[AttachMerged] No registered model for guid=%s - discarding merged mesh"),
			*ModelGuid);
		return;
	}

	// Find the destination cell record.
	TArray<FCellRegistrationData>& Table = bShellSide ? Model->ShellCells : Model->InteriorCells;
	FCellRegistrationData* Dest = nullptr;
	for (FCellRegistrationData& CRD : Table)
	{
		if (CRD.Cell == Cell) { Dest = &CRD; break; }
	}
	if (!Dest)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[AttachMerged] Cell F%dX%dY%d not in %s table for model %s"),
			Cell.Floor, Cell.GridX, Cell.GridY,
			bShellSide ? TEXT("Shell") : TEXT("Interior"),
			*ModelGuid);
		return;
	}

	// Construct the component.
	UStaticMeshComponent* MergedComp = NewObject<UStaticMeshComponent>(
		Model->BucketRoot, NAME_None, RF_Transient);
	MergedComp->SetStaticMesh(MergedMesh);
	MergedComp->SetMobility(EComponentMobility::Stationary);

	// Collision channel/response setup only — SetMergedShown below owns the enabled/disabled state.
	MergedComp->SetCollisionObjectType(ECC_WorldStatic);
	MergedComp->SetCollisionResponseToAllChannels(ECR_Block);

	MergedComp->ComponentTags.Add(FName(TEXT("FragCellMerge")));   // for click resolution
	// Stamp the cell's floor index as a component tag so per-floor visibility filters (which already walk ComponentTags on AFragment children) can include merged cell components alongside HISMCs.
	MergedComp->ComponentTags.Add(FName(*FString::Printf(TEXT("Floor:%d"), Cell.Floor)));
	MergedComp->bAffectDistanceFieldLighting = false;               // match HISMC Lumen suppression
	MergedComp->bAffectDynamicIndirectLighting = false;
	// Pre-attach race: bFilterHidden or a user hide may have landed while MergedComponent was null.
	SetMergedShadowEnabled(MergedComp,
		Model->bUserVisible && FragmentModelTier::ShouldCellsCast(Model->Tier) && !Dest->bFilterHidden);

	MergedComp->AttachToComponent(
		Model->BucketRoot->GetRootComponent(),
		FAttachmentTransformRules::KeepWorldTransform);
	// Land hidden AND trace-inert, in lockstep.
	SetMergedShown(MergedComp, false);
	{
		// DIAGNOSTIC: the LOD1 cell's render-state creation.
		TRACE_CPUPROFILER_EVENT_SCOPE(FragRegMgr_MergedCompRegisterComponent);
		MergedComp->RegisterComponent();   // sticky-register; never unregistered during normal ops
	}

	Dest->MergedComponent = MergedComp;
	Dest->bMergeInFlight = false;
	// The component landed hidden; make the next pass (even on a still camera) show what the cell needs.
	Dest->LodState = UE::Fragments::CellLod::StateWhenMergeSettles(Dest->LodState);
	bForceNextUpdate = true;

	UE_LOG(LogFragments, Verbose,
		TEXT("[AttachMerged] model=%s cell=F%dX%dY%d shell=%d  attached + hidden"),
		*ModelGuid, Cell.Floor, Cell.GridX, Cell.GridY, bShellSide ? 1 : 0);
}

void UFragmentRegistrationManager::OnCellMergeFailed(
	const FString& ModelGuid, const FCellId& Cell, bool bShellSide)
{
	check(IsInGameThread());

	FModelRegistrationData* Model = FindModelByGuid(ModelGuid);
	if (!Model) return;

	TArray<FCellRegistrationData>& Table = bShellSide ? Model->ShellCells : Model->InteriorCells;
	for (FCellRegistrationData& CRD : Table)
	{
		if (CRD.Cell == Cell)
		{
			CRD.bMergeInFlight = false;
			// The state machine reads MergedComponent==null and bMergeInFlight==false as "no LOD1 available, pin at LOD0".
			CRD.LodState = UE::Fragments::CellLod::StateWhenMergeSettles(CRD.LodState);
			bForceNextUpdate = true;
			return;
		}
	}
}

void UFragmentRegistrationManager::SetFloorFilterForModel(
	const FString& ModelGuid, int64 FloorId, bool bVisible)
{
	FModelRegistrationData* Model = FindModelByGuid(ModelGuid);
	if (!Model) return;

	auto ApplyToTable = [&](TArray<FCellRegistrationData>& Table)
		{
			for (FCellRegistrationData& Cell : Table)
			{
				if (Cell.Cell.Floor == FloorId)
				{
					// Delta on TRANSITION only.
					const bool bNewHidden = !bVisible;
					if (Cell.bFilterHidden != bNewHidden)
					{
						Model->FloorFilteredCellCount += bNewHidden ? 1 : -1;
						Cell.bFilterHidden = bNewHidden;
					}
					SetMergedShadowEnabled(Cell.MergedComponent,
						Model->bUserVisible && FragmentModelTier::ShouldCellsCast(Model->Tier) && bVisible);
				}
			}
		};
	ApplyToTable(Model->ShellCells);
	ApplyToTable(Model->InteriorCells);

	bForceNextUpdate = true;
	bBypassTransitionCapNextTick = true;

	UE_LOG(LogFragments, Verbose,
		TEXT("[FloorFilter] SetFloorFilterForModel guid=%s floor=%lld visible=%d"),
		*ModelGuid, FloorId, bVisible ? 1 : 0);
}
void UFragmentRegistrationManager::CollectModelFloors(
	const FString& ModelGuid, TSet<int64>& OutFloors) const
{
	for (const FModelRegistrationData& Model : RegisteredModels)
	{
		if (Model.ModelGuid != ModelGuid) continue;
		for (const FCellRegistrationData& Cell : Model.ShellCells)
		{
			OutFloors.Add(Cell.Cell.Floor);
		}
		for (const FCellRegistrationData& Cell : Model.InteriorCells)
		{
			OutFloors.Add(Cell.Cell.Floor);
		}
		return;
	}
}
void UFragmentRegistrationManager::ClearFloorFilterForModel(const FString& ModelGuid)
{
	FModelRegistrationData* Model = FindModelByGuid(ModelGuid);
	if (!Model) return;

	auto ClearTable = [Model](TArray<FCellRegistrationData>& Table)
		{
			for (FCellRegistrationData& Cell : Table)
			{
				Cell.bFilterHidden = false;
				SetMergedShadowEnabled(Cell.MergedComponent,
					Model->bUserVisible && FragmentModelTier::ShouldCellsCast(Model->Tier));
			}
		};
	ClearTable(Model->ShellCells);
	ClearTable(Model->InteriorCells);
	Model->FloorFilteredCellCount = 0;

	bForceNextUpdate = true;
	bBypassTransitionCapNextTick = true;

	UE_LOG(LogFragments, Verbose,
		TEXT("[FloorFilter] ClearFloorFilterForModel guid=%s"), *ModelGuid);
}

#if !UE_BUILD_SHIPPING
void UFragmentRegistrationManager::DumpProxyLODState() const
{
	UWorld* World = GetWorld();

	// Camera + viewport define the projection UE actually selects LODs with.
	FVector CamLoc = FVector::ZeroVector;
	FVector CamFwd = FVector::ForwardVector;
	float   FOVDeg = 90.0f;
	if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		if (PC->PlayerCameraManager)
		{
			CamLoc = PC->PlayerCameraManager->GetCameraLocation();
			CamFwd = PC->PlayerCameraManager->GetCameraRotation().Vector();
			FOVDeg = PC->PlayerCameraManager->GetFOVAngle();
		}
	}

	FIntPoint ViewSize(0, 0);
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		ViewSize = GEngine->GameViewport->Viewport->GetSizeXY();
	}

	// Mirrors FPerspectiveMatrix: M00 = 1/tan(halfFOVx), M11 = M00 * aspect.
	const float RefM11   = FragmentProxyTiers::kRefProjM11;
	const float Aspect   = (ViewSize.Y > 0) ? (float)ViewSize.X / (float)ViewSize.Y : (RefM11 / 1.0f);
	const float M00      = 1.0f / FMath::Tan(FMath::DegreesToRadians(FOVDeg * 0.5f));
	const float M11      = M00 * Aspect;
	const float ScreenMul = FMath::Max(M00, M11);

	UE_LOG(LogFragments, Display,
		TEXT("[ProxyLOD] view: viewport=%dx%d aspect=%.4f fov=%.2fdeg M00=%.4f M11=%.4f -> ScreenMul=%.4f ")
		TEXT("(ScreenSizes baked against %.0fp / M11=%.3f; ratio=%.3f)"),
		ViewSize.X, ViewSize.Y, Aspect, FOVDeg, M00, M11, ScreenMul,
		FragmentProxyTiers::kRefViewportHeightPx, RefM11, ScreenMul / RefM11);
	UE_LOG(LogFragments, Display, TEXT("[ProxyLOD] camera=(%.0f, %.0f, %.0f)"),
		CamLoc.X, CamLoc.Y, CamLoc.Z);

	// Rank by how centred each proxy is, so "the building in the middle of the screen" comes first.
	struct FProxyEntry { const FModelRegistrationData* Model; float Dist; float Centredness; };
	TArray<FProxyEntry> Entries;
	for (const FModelRegistrationData& M : RegisteredModels)
	{
		if (!IsValid(M.ProxyComponent) || !M.ProxyComponent->GetStaticMesh()) { continue; }
		const FVector Origin = M.ProxyComponent->Bounds.Origin;
		const FVector ToProxy = Origin - CamLoc;
		const float Dist = (float)ToProxy.Size();
		const float Centredness = (Dist > UE_KINDA_SMALL_NUMBER)
			? (float)FVector::DotProduct(ToProxy / Dist, CamFwd) : -1.0f;
		Entries.Add({ &M, Dist, Centredness });
	}
	Entries.Sort([](const FProxyEntry& A, const FProxyEntry& B) { return A.Centredness > B.Centredness; });

	if (Entries.Num() == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("[ProxyLOD] No registered models have a proxy component."));
		return;
	}

	const int32 NumToDump = FMath::Min(3, Entries.Num());
	for (int32 e = 0; e < NumToDump; ++e)
	{
		const FProxyEntry& E = Entries[e];
		const UStaticMeshComponent* Comp = E.Model->ProxyComponent;
		const UStaticMesh* Mesh = Comp->GetStaticMesh();
		const FStaticMeshRenderData* RD = Mesh->GetRenderData();
		if (!RD) { continue; }

		const float R = (float)Comp->Bounds.SphereRadius;
		const float D = FMath::Max(1.0f, E.Dist);
		const float CurScreenSize = ScreenMul * R / D;   // what the view produces right now

		UE_LOG(LogFragments, Display,
			TEXT("[ProxyLOD] --- model %s (visible=%d, tier=%s) ---"),
			*E.Model->ModelGuid, Comp->IsVisible() ? 1 : 0,
			E.Model->Tier == FragmentModelTier::ETier::Proxy ? TEXT("Proxy") : TEXT("Cells"));
		UE_LOG(LogFragments, Display,
			TEXT("[ProxyLOD]   boundsRadius=%.0fcm  cameraDist=%.0fcm  currentScreenSize=%.4f  numLODs=%d"),
			R, E.Dist, CurScreenSize, RD->LODResources.Num());

		// Material slots are shared across every LOD; a far tier missing one contributes no section for it rather than renumbering (slots are matched by name at build time).
		const TArray<FStaticMaterial>& Mats = Mesh->GetStaticMaterials();
		for (int32 m = 0; m < Mats.Num(); ++m)
		{
			UE_LOG(LogFragments, Display, TEXT("[ProxyLOD]   slot[%d] name=%s material=%s"),
				m, *Mats[m].MaterialSlotName.ToString(),
				Mats[m].MaterialInterface ? *Mats[m].MaterialInterface->GetName() : TEXT("<null>"));
		}

		// UE walks LODs backwards and takes the highest index whose ScreenSize still exceeds the current one (ComputeStaticMeshLOD).
		int32 SelectedLOD = 0;
		for (int32 i = RD->LODResources.Num() - 1; i >= 0; --i)
		{
			if (RD->ScreenSize[i].GetValue() > CurScreenSize) { SelectedLOD = i; break; }
		}

		for (int32 i = 0; i < RD->LODResources.Num(); ++i)
		{
			const float SS = RD->ScreenSize[i].GetValue();
			// Distance at which this LOD takes over, under the CURRENT view.
			const float SwitchDistCm = (SS > UE_KINDA_SMALL_NUMBER) ? (ScreenMul * R / SS) : -1.0f;

			FString SectionDesc;
			const FStaticMeshLODResources& LODRes = RD->LODResources[i];
			for (int32 s = 0; s < LODRes.Sections.Num(); ++s)
			{
				SectionDesc += FString::Printf(TEXT("%s%d:%dtris"),
					s > 0 ? TEXT(" ") : TEXT(""),
					LODRes.Sections[s].MaterialIndex, LODRes.Sections[s].NumTriangles);
			}

			UE_LOG(LogFragments, Display,
				TEXT("[ProxyLOD]   LOD%d%s screenSize=%.4f switchesAt=%.0fcm tris=%d sections=%d [matIdx:tris %s]"),
				i, (i == SelectedLOD) ? TEXT(" <== SELECTED NOW") : TEXT(""),
				SS, SwitchDistCm, LODRes.GetNumTriangles(), LODRes.Sections.Num(), *SectionDesc);
		}
	}
}
#endif

// Console command registration
#if !UE_BUILD_SHIPPING
namespace FragRegMgrConsole
{
	static UFragmentRegistrationManager* FindManager(UWorld* World)
	{
		if (!World) return nullptr;
		UGameInstance* GI = World->GetGameInstance();
		if (!GI) return nullptr;
		UFragmentsImporterSubsystem* Sub = GI->GetSubsystem<UFragmentsImporterSubsystem>();
		if (!Sub) return nullptr;
		UFragmentsImporter* Imp = Sub->GetImporter();
		if (!Imp) return nullptr;
		return Imp->GetRegistrationManager();
	}
}

static FAutoConsoleCommandWithWorld GFragDumpVisibilityCmd(
	TEXT("Frag.DumpVisibility"),
	TEXT("Dump visibility funnel snapshot to LogFragments."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UFragmentRegistrationManager* Mgr = FragRegMgrConsole::FindManager(World);
		if (!Mgr)
		{
			UE_LOG(LogFragments, Warning, TEXT("Frag.DumpVisibility: No active registration manager."));
			return;
		}
		Mgr->DumpVisibilitySnapshot();
	}));

// Diagnostic only.
static FAutoConsoleCommandWithWorld GFragDumpProxyLODCmd(
	TEXT("Frag.DumpProxyLOD"),
	TEXT("Dump proxy LOD screen sizes, switch distances, current selection and per-LOD material indices."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UFragmentRegistrationManager* Mgr = FragRegMgrConsole::FindManager(World);
		if (!Mgr)
		{
			UE_LOG(LogFragments, Warning, TEXT("Frag.DumpProxyLOD: No active registration manager."));
			return;
		}
		Mgr->DumpProxyLODState();
	}));

#endif
