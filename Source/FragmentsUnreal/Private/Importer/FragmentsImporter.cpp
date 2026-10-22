// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Importer/FragmentsImporter.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CountersTrace.h"

// UE 5.4+ replaced bool bAllowShrinking with EAllowShrinking enum
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 4
#define FRAGMENTS_NO_SHRINKING EAllowShrinking::No
#else
#define FRAGMENTS_NO_SHRINKING false
#endif
#include "flatbuffers/flatbuffers.h"
#include "Serialization/ArchiveLoadCompressedProxy.h"
#include "zlib.h"
#include "ProceduralMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "StaticMeshDescription.h"
#include "StaticMeshAttributes.h"
#include "MeshDescription.h"
#include "Spatial/FragmentMeshAuthoring.h"
#include "Spatial/FragmentShellAuthoring.h"
#include "Spatial/FragmentViewProvider.h"
#include <StaticMeshOperations.h>
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "Spatial/FragmentProxyDecimate.h"
#include "Curve/PolygonIntersectionUtils.h"
#include "Utils/FragmentOcclusionClassifier.h"
#include "Utils/FragmentClassification.h"
#include "Utils/FragmentCellPartition.h"
#include "Utils/BudgetDrain.h"
#include "CompGeom/PolygonTriangulation.h"
#include "tesselator.h"
#include "Algo/Reverse.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentModelWrapper.h"
#include "UObject/SavePackage.h"
#include "Misc/ScopedSlowTask.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Spatial/FragmentCellMergeManager.h"
#include "Spatial/FragmentModelTier.h"   // DemoteDistanceCm (BuildCellSubLodSpecs' far-rung calibration)
#include "Spatial/FragmentProxyTiers.h"   // ViewCoeff/RadiusCutoffCm (LogCellBudgetSolve's OmitPx inverse)
#include "Debug/FragmentLODDebugComponent.h"
#include "Importer/FragmentsImporterSubsystem.h"
#include "Importer/FragmentOverrides.h"
#include "Utils/FragmentOcclusionClassifier.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Rendering/PositionVertexBuffer.h"
#include "RawIndexBuffer.h"
#include "GameFramework/PlayerController.h"
#include "Spatial/FragmentTriMeshCook.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h" // MAX_STATIC_MESH_LODS
#if WITH_EDITOR
#include "EngineUtils.h"
#endif


DEFINE_LOG_CATEGORY(LogFragments);

namespace
{
	/** Worker-safe POD snapshot of one accumulator section — the flat geometry (no UObject references) plus its material slot. */
	struct FProxyMeshSectionSnapshot
	{
		FragmentProxyDecimate::FDecimateSection Geo;
		int32 MaterialSlot = 0;
	};

	/** Sentinel FTierSpec::ScreenSize set by BuildProxyTierSpecs (which is bounds-free and cannot compute a real value) and overwritten by FillProxyTierScreenSizes once accumulated proxy bounds exist. */
	constexpr float kUnresolvedProxyTierScreenSize = -1.0f;

	/** Parses "100,10000,40000" into a float array, skipping empty tokens. */
	TArray<float> ParseNearEdgesCm(const FString& CommaSeparated)
	{
		TArray<float> NearEdges;
		TArray<FString> Parts;
		CommaSeparated.ParseIntoArray(Parts, TEXT(","), true);
		NearEdges.Reserve(Parts.Num());
		for (const FString& P : Parts) { NearEdges.Add(FCString::Atof(*P.TrimStartAndEnd())); }
		return NearEdges;
	}

	/** Looks up ModelGuid's registered bounds radius -- the SAME R FragmentModelTier's promote/demote math is calibrated against. */
	float ModelBoundsRadiusCmForGuid(UFragmentRegistrationManager* RegMgr, const FString& ModelGuid)
	{
		FBox Bounds(ForceInit);
		if (RegMgr && RegMgr->GetModelBounds(ModelGuid, Bounds) && Bounds.IsValid)
		{
			return static_cast<float>(FBoxSphereBounds(Bounds).SphereRadius);
		}
		return 0.0f;
	}
}

// Cell partition CVars Consulted at import time only — set via config/commandline before import begins.
static TAutoConsoleVariable<float> CVarCellSizeCm(
	TEXT("r.Fragments.CellSizeCm"), 2000.0f,
	TEXT("XY cell edge length in cm. Smaller = tighter bucket bounds, more buckets."),
	ECVF_ReadOnly);

// Proxy decimation CVars === Sentinel defaults (-1) defer to the importer UPROPERTYs; a valid value overrides.
static TAutoConsoleVariable<int32> CVarProxyDecimate(
	TEXT("r.Fragments.Proxy.Decimate"), -1,
	TEXT("Override proxy decimation on/off. -1 = use bDecimateProxyMesh UPROPERTY; 0 = off; 1 = on."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarProxyDecimateRatio(
	TEXT("r.Fragments.Proxy.DecimateRatio"), -1.0f,
	TEXT("Override proxy decimation kept-triangle fraction. <=0 = use ProxyDecimationRatio UPROPERTY; "
	     "else clamped to [0.05, 1.0]."),
	ECVF_Default);

// Collision-only decimation for the whole-building proxy's Chaos trimesh.
static TAutoConsoleVariable<float> CVarProxyCollisionDecimateRatio(
	TEXT("r.Fragments.Proxy.CollisionDecimateRatio"), 0.1f,
	TEXT("Fraction of tier-0 proxy triangles kept for the Chaos collision trimesh, clamped to [0.05, 1.0]. "
	     "Controls collision fidelity only -- independent of the visual tier's SimplifyRatio and does not "
	     "affect what is rendered. Read at import time only; must be set before import."),
	ECVF_ReadOnly);

// LOD2 proxy tiering CVars === Consulted at import time (game thread), via UFragmentsImporter::ResolveProxyTierSpecs.

// Number of LODs on the whole-building proxy. Read at import time only -- see ECVF_ReadOnly note below.
static TAutoConsoleVariable<int32> CVarProxyTiers(
	TEXT("r.Fragments.Proxy.Tiers"), 3,
	TEXT("Number of LOD tiers on the LOD2 proxy. 1 = single LOD. To reproduce pre-tier behaviour, combine "
	     "with r.Fragments.Proxy.Decimate 1. Read at import time only; console edits mid-session do nothing "
	     "-- must be set before import."),
	ECVF_ReadOnly);

// Screen size (px) below which an element may be omitted from a tier.
TAutoConsoleVariable<float> CVarProxyOmitPx(
	TEXT("r.Fragments.Proxy.OmitPx"), 4.0f,
	TEXT("Elements projecting below this many pixels at a tier's near edge are omitted from it. Read at "
	     "import time only; must be set before import."),
	ECVF_ReadOnly);

// Near-edge distance (cm) each tier's cutoff is calibrated for.
static TAutoConsoleVariable<FString> CVarProxyTierNearEdges(
	TEXT("r.Fragments.Proxy.TierNearEdgesCm"), TEXT("100,10000,40000"),
	TEXT("Comma-separated near-edge distances (cm), one per tier, ascending. Read at import time only; a "
	     "mid-import edit would calibrate MinRadiusCm against old edges and ScreenSize against new ones -- "
	     "must be set before import."),
	ECVF_ReadOnly);

// Proxy tier near edges as MULTIPLES of the model bounds radius, putting them in the same unit system as the model tier ladder (which promotes at GeoCoeff*Coeff/ThresholdPx = 1.980 * R).
static TAutoConsoleVariable<FString> CVarProxyTierNearEdgeRadiiMul(
	TEXT("r.Fragments.Proxy.TierNearEdgeRadiiMul"), TEXT(""),
	TEXT("Comma-separated near-edge multipliers of model bounds radius, one per tier, ascending. "
	     "Empty = use r.Fragments.Proxy.TierNearEdgesCm absolute values. Read at import time only."),
	ECVF_ReadOnly);

// Per-model prune of LEADING proxy render tiers that are provably unreachable, computed from this model's own numbers (UFragmentsImporter::CountUnreachableLeadingTiers) rather than applied as a blanket rule.
TAutoConsoleVariable<int32> CVarProxyPruneUnreachableTiers(
	TEXT("r.Fragments.Proxy.PruneUnreachableTiers"), 0,
	TEXT("0 = every proxy tier is built and kept in the render LOD list, even ones the model tier ")
	TEXT("ladder can never select for this model (default -- safe; see below for why 1 is not yet ")
	TEXT("recommended). 1 = per-model, prune a leading run of proxy render tiers whose engage() ")
	TEXT("distance is provably unreachable given this model's own PromoteCm; the last tier is never ")
	TEXT("pruned and collision still cooks from tier 0 unconditionally. GATED: the unreachability ")
	TEXT("proof covers only the MAIN render pass -- it does NOT establish that shadow depth passes ")
	TEXT("select the same LOD, and the proxy casts shadows whenever it is the coarsest resident ")
	TEXT("representation. Do not enable until a FAR capture (models in Proxy tier, default vs ")
	TEXT("r.ForceLODShadow 2) shows shadows do not depend on tiers 0/1."),
	ECVF_ReadOnly);

// LOD1 cell-merge decimation CVars === External linkage (no `static`) — read from FragmentCellMergeOps.cpp via extern decl, mirroring CVarCellCollapseMinInstances.
TAutoConsoleVariable<int32> CVarCellMergeDecimate(
	TEXT("r.Fragments.CellMerge.Decimate"), -1,
	TEXT("Override LOD1 cell-merge decimation on/off. -1 = use bDecimateCellMerge UPROPERTY; 0 = off; 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarCellMergeDecimateRatio(
	TEXT("r.Fragments.CellMerge.DecimateRatio"), -1.0f,
	TEXT("Override LOD1 cell-merge decimation kept-triangle fraction. <=0 = use CellMergeDecimationRatio "
	     "UPROPERTY; else clamped to [0.05, 1.0]."),
	ECVF_Default);

// Triangle floor for template-scoped simplification.
TAutoConsoleVariable<int32> CVarCellMergeMinSimplifyTris(
	TEXT("r.Fragments.CellMerge.MinSimplifyTris"), 64,
	TEXT("Templates with fewer triangles than this are never simplified during cell merge."),
	ECVF_ReadOnly);

// Coincident-vertex weld distance for the TEMPLATE-scoped simplify step only (FragmentCellMerge:: SimplifyTemplate).
TAutoConsoleVariable<float> CVarCellMergeWeldToleranceCm(
	TEXT("r.Fragments.CellMerge.WeldToleranceCm"), 0.01f,
	TEXT("Coincident-vertex weld distance (cm) applied before template-scoped cell-merge "
	     "simplification. 0 = disabled. Default 0.01 (0.1mm) -- just past float noise for "
	     "render-buffer-exact-coincident vertices. Section-scope (LOD2 proxy) callers are unaffected; "
	     "see FragmentProxyDecimate::DefaultWeldToleranceCm."),
	ECVF_ReadOnly);

// Collision cooks from a SEPARATELY decimated copy of sub-LOD 0, independent of the visual rung -- the r.Fragments.Proxy.CollisionDecimateRatio seam, applied to cells.
TAutoConsoleVariable<float> CVarCellMergeCollisionDecimateRatio(
	TEXT("r.Fragments.CellMerge.CollisionDecimateRatio"), 1.0f,
	TEXT("Ratio for the collision-only copy of LOD1 cell sub-LOD 0. 1.0 = full detail (default)."),
	ECVF_ReadOnly);

// Shell/interior simplification gate.
TAutoConsoleVariable<int32> CVarCellMergeSimplifyShell(
	TEXT("r.Fragments.CellMerge.SimplifyShell"), 0,
	TEXT("0 = shell templates are never simplified during cell merge, regardless of Decimate/DecimateRatio "
	     "(default; artifact guard against unwelded simplification tears on large shell surfaces). "
	     "1 = shell simplifies on the same terms as interiors."),
	ECVF_ReadOnly);

// External linkage (no `static`) — read from FragmentCellMergeOps.cpp via extern decl, mirroring CVarCellMergeDecimate.
TAutoConsoleVariable<int32> CVarCellMergeCensus(
	TEXT("r.Fragments.CellMerge.Census"), 0,
	TEXT("1 = accumulate a dry-run selection census during cell merge and write CSV to "
	     "Saved/Profiling/CellCensus-<timestamp>.csv via Frag.DumpCellCensus. Changes nothing about what "
	     "is built. 0 = off (default); a normal import pays nothing."),
	ECVF_Default);

// External linkage: also read from FragmentCellMergeOps.cpp via extern decl.
TAutoConsoleVariable<int32> CVarCellCollapseMinInstances(
	TEXT("r.Fragments.CellCollapseMinInstances"), 20,
	TEXT("Cells with fewer items than this get absorbed into a neighbor."),
	ECVF_ReadOnly);

// LOD CVars === External linkage (no `static`) — these are read from FragmentRegistrationManager.cpp via `extern` declarations.

TAutoConsoleVariable<float> CVarShellLOD0PromoteCm(
	TEXT("r.Fragments.Shell.LOD0PromoteCm"), 1500.0f,
	TEXT("Distance at which shell cells flip between Active_LOD0 (HISMC instances visible) "
	     "and Merged_LOD1 (merged cell mesh visible). Also sets the HISMC's InstanceEndCullDistance "
	     "for shell buckets so per-instance distance culling aligns with the LOD swap."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarInteriorLOD0PromoteCm(
	TEXT("r.Fragments.Interior.LOD0PromoteCm"), 1000.0f,
	TEXT("Same as Shell.LOD0PromoteCm but for interior cells. Default is tighter (1000 cm) "
	     "because interior detail is only meaningful when the user is close (e.g. first-person)."),
	ECVF_Default);

// ECVF_ReadOnly, matching the other criterion CVars: this value is BAKED into sub-LOD selection at import, so a mid-session edit would move nothing that is already built while appearing to work.
TAutoConsoleVariable<float> CVarInteriorOmitPx(
	TEXT("r.Fragments.Interior.OmitPx"), 32.0f,
	TEXT("Pixel budget for omitting small elements from INTERIOR merged cells. Default 32. "
	     "-1 = opt out and fall back to the shared r.Fragments.Proxy.OmitPx instead. Higher omits more."),
	ECVF_ReadOnly);

// Cell budget settings solve a cutoff from a target memory limit.
TAutoConsoleVariable<float> CVarMemoryCellBudgetMB(
	TEXT("r.Fragments.Memory.CellBudgetMB"), -1.0f,
	TEXT("Target resident MB for LOD1 cells + LOD0 templates. -1 = off (default). When set (and "
	     "BytesPerCellTriangle > 0), the census import (r.Fragments.CellMerge.Census 1) solves for "
	     "the selection cutoff that lands under it and logs the recommended r.Fragments.Interior."
	     "OmitPx -- two-pass: this import already built cells at whichever cutoff was already in "
	     "effect, so apply the recommendation on the NEXT import. Cannot go below the template "
	     "floor -- an impossible budget WARNS and is not silently approximated."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarMemoryBytesPerCellTriangle(
	TEXT("r.Fragments.Memory.BytesPerCellTriangle"), 208.0f,
	TEXT("Empirical bytes per merged-cell (LOD1) triangle, calibrated on "
	     "the Campus dataset: 25,807 MB of cell memory over ~130M baked "
	     "cell triangles. PROVISIONAL -- sub-LOD 1's cutoff is derived per model, so the ~130M "
	     "denominator is an estimate, not a count. <= 0 disables the budget solver: solving against "
	     "an uncalibrated constant produces a confident wrong answer. Calibrated with "
	     "bDecimateCellMerge OFF, which is NO LONGER the shipped default: the census runs on "
	     "unsimplified templates (see FragmentCellCensus.h), so for INTERIOR cells it OVER-counts "
	     "baked triangles by roughly 1.9x at ratio 0.5 with most interior geometry above the "
	     "64-triangle floor. Shell cells are unaffected (SimplifyShell defaults 0). The solver is "
	     "therefore conservative rather than wrong -- it over-predicts memory and picks a stricter "
	     "cutoff than needed -- but recalibrate this constant with simplify on before relying on it."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarMemoryTemplateFloorMB(
	TEXT("r.Fragments.Memory.TemplateFloorMB"), 0.0f,
	TEXT("Resident MB of LOD0 templates -- the floor r.Fragments.Memory.CellBudgetMB cannot reduce; "
	     "only the cell-merge cutoff moves. Per-dataset opt-in refinement -- default 0.0 means the "
	     "budget covers cells only, with no template floor accounted for. 3870 MB was measured for "
	     "Campus (34,900 MB total - 5,223 MB LOD2 proxies - "
	     "25,807 MB cells); that figure is Campus-specific, and defaulting to it would make the "
	     "solver confidently refuse legitimate budgets on a smaller dataset. Set explicitly per "
	     "dataset if this refinement is wanted."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarLOD0HysteresisCm(
	TEXT("r.Fragments.LOD0HysteresisCm"), 500.0f,
	TEXT("Distance band centered on `Shell.LOD0PromoteCm` or "
	     "`Interior.LOD0PromoteCm` (whichever applies to the cell) where the "
	     "cell holds its current state. Prevents thrashing at the threshold. "
	     "Clamped at read time to half the applicable PromoteCm so the band's "
	     "lower bound is always reachable."),
	ECVF_Default);

// Continuous model-LOD (LOD2 proxy <-> LOD1 cells).
TAutoConsoleVariable<float> CVarLODGeoCoeffCells(
	TEXT("r.Fragments.LOD.GeoCoeff.Cells"), 0.033f,
	TEXT("Screen-coverage coefficient for the LOD2 proxy -> LOD1 cells promotion. Derived so a "
	     "50m-radius building goes cell-resident at ~100m under the reference view: "
	     "ThresholdPx * D / (R * ViewCoeff) = 16 * 10000 / (5000 * 960.12). PROVISIONAL - "
	     "confirmed or corrected at the measurement gate."),
	ECVF_ReadOnly);

TAutoConsoleVariable<float> CVarLODSSEPixelError(
	TEXT("r.Fragments.LOD.SSEPixelError"), 16.0f,
	TEXT("Screen-space error refine threshold in pixels. Above this a model shows more detail."),
	ECVF_ReadOnly);

TAutoConsoleVariable<float> CVarLODCoarsenFactor(
	TEXT("r.Fragments.LOD.CoarsenFactor"), 0.75f,
	TEXT("Hysteresis band. A model demotes only below SSEPixelError * this; between the two it "
	     "holds its current tier. Must be in (0,1)."),
	ECVF_ReadOnly);

TAutoConsoleVariable<int32> CVarLODDebugAutoAttach(
	TEXT("r.Fragments.LODDebug.AutoAttach"), 0,
	TEXT("Debug: 1 = auto-attach UFragmentLODDebugComponent to every model's BucketRoot at spawn. "
	     "0 = disabled (default). The component itself defaults to Mode=Disabled; pick a mode in Details "
	     "to colorize. Mirrors Cesium's UCesiumDebugColorizeTilesRasterOverlay pattern."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarLODDebugForceAllToLOD1(
	TEXT("r.Fragments.LODDebug.ForceAllToLOD1"), 0,
	TEXT("Debug: 1 = force every cell with an available merged component to Merged_LOD1, "
	     "ignoring distance and frustum gates. Cells whose merge hasn't finalized yet still "
	     "fall back to Active_LOD0. Pairs with LOD1_Cells debug mode to visually inspect "
	     "cell partitioning without moving the camera. 0 = normal distance gating (default)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarEditorStreaming(
	TEXT("r.Fragments.EditorStreaming"), 1,
	TEXT("1 (default): drive cell LOD from the editor viewport cameras when there is no player ")
	TEXT("controller. 0: no editor streaming, cells stay as RegisterModel primed them."),
	ECVF_Default);

static float GFragmentsFinalizeMsPer1kTris = 0.05f;
static FAutoConsoleVariableRef CVarFragmentsFinalizeMsPer1kTris(
	TEXT("r.Fragments.Finalize.MsPer1kTris"),
	GFragmentsFinalizeMsPer1kTris,
	TEXT("Estimated game-thread BuildFromMeshDescriptions cost per 1000 triangles (ms). "
	     "Used by the budget-paced finalize drain to decide whether to start the next build."),
	ECVF_Default);

// Log slow mesh finalizations with their tier and geometry size.
static float GFragmentsFinalizeOutlierLogMs = 100.0f;
static FAutoConsoleVariableRef CVarFragmentsFinalizeOutlierLogMs(
	TEXT("r.Fragments.Finalize.OutlierLogMs"),
	GFragmentsFinalizeOutlierLogMs,
	TEXT("Log one [FinalizeOutlier] line for any mesh finalize exceeding this many milliseconds. "
	     "0 disables. Not ECVF_ReadOnly: this is a diagnostic that is useful to flip mid-session."),
	ECVF_Default);

static bool GFragmentsFinalizePaced = true;
static FAutoConsoleVariableRef CVarFragmentsFinalizePaced(
	TEXT("r.Fragments.Finalize.Paced"),
	GFragmentsFinalizePaced,
	TEXT("When true (default), mesh finalizes are queued and budget-paced. "
	     "When false, they build inline on worker completion (legacy fire-and-forget) for A/B testing."),
	ECVF_Default);

static bool GFragmentsCellMergeEnqueuePaced = true;
static FAutoConsoleVariableRef CVarFragmentsCellMergeEnqueuePaced(
	TEXT("r.Fragments.CellMergeEnqueue.Paced"),
	GFragmentsCellMergeEnqueuePaced,
	TEXT("When true (default), the bucket-finalize tail (cell-merge enqueue + override apply) is "
	     "budget-paced across frames. When false, it runs all-at-once (legacy) for A/B testing."),
	ECVF_Default);

// ProcessHISMSpawnChunk's spawn-phase drain and its HISM item loop share one FrameBudgetCoordinator budget for the tick.
static TAutoConsoleVariable<float> CVarFragmentsSpawnPhaseDrainBudgetShare(
	TEXT("r.Fragments.SpawnPhase.DrainBudgetShare"), 0.5f,
	TEXT("Fraction (clamped to [0.1, 0.95] at read time) of ProcessHISMSpawnChunk's per-tick "
	     "frame budget reserved for draining completed async shell builds (PendingMeshFinalizes) "
	     "before the HISM item loop runs. The remainder goes to the item loop. Since LOD0 "
	     "shell authoring moved to background workers, the item loop itself is cheap "
	     "(cache hit: a map lookup; new shell build: an async launch), so this likely wants "
	     "raising above the 0.5 default that was tuned when the loop still did the build "
	     "synchronously — sweep this live during a trace capture rather than guessing."),
	ECVF_Default);

// Rollback + measurement switch for the off-thread proxy flush.
static bool GFragmentsProxyFlushAsync = true;
static FAutoConsoleVariableRef CVarFragmentsProxyFlushAsync(
	TEXT("r.Fragments.ProxyFlush.Async"), GFragmentsProxyFlushAsync,
	TEXT("When true (default), proxy-contribution accumulation runs on a background "
	     "worker. When false, it runs synchronously on the game thread (legacy stall) "
	     "for A/B measurement."),
	ECVF_Default);

// Consecutive ProcessHISMSpawnChunk ticks the finalize barrier may defer bucket finalize before a stall warning is logged (~10s at the 16ms-ish chunked-spawn tick rate).
static constexpr int32 GFragmentsShellBuildBarrierStallLogThreshold = 600;

// Rough per-waiter game-thread cost for the async shell-build finalize gate, declared via FPendingMeshFinalize::ExtraFinalizeCostMs.
static constexpr float GFragmentsShellWaiterCostPerWaiterMs = 0.01f;

UFragmentsImporter::UFragmentsImporter()
{
}

void UFragmentsImporter::BeginDestroy()
{
	// Flush pending finalizers before GC so rooted meshes are released.
	FlushMeshFinalizes();
	Super::BeginDestroy();
}

EFinalizeGate UFragmentsImporter::DecideFinalizeGate(float RemainingMs, float EstMs, float FullBudgetMs)
{
	if (EstMs <= RemainingMs) { return EFinalizeGate::Run; }
	const bool bNeverFits  = EstMs > FullBudgetMs;
	const bool bFrameFresh = RemainingMs >= FullBudgetMs - KINDA_SMALL_NUMBER;
	if (bNeverFits && bFrameFresh) { return EFinalizeGate::RunAlone; }
	return EFinalizeGate::Defer;
}

float UFragmentsImporter::EstimateBuildMs(int32 TriangleCount)
{
	return (static_cast<float>(TriangleCount) / 1000.0f) * GFragmentsFinalizeMsPer1kTris;
}

int32 UFragmentsImporter::ResolveMaxInFlightShellBuilds(int32 NumWorkerThreads)
{
	// Clamp worker count before doubling to avoid overflow.
	const int32 SafeWorkerThreads = FMath::Clamp(NumWorkerThreads, 0, 64);
	return FMath::Clamp(SafeWorkerThreads * 2, 8, 128);
}

void UFragmentsImporter::EnqueueMeshFinalize(FPendingMeshFinalize&& Payload)
{
	check(IsInGameThread());
	PendingMeshFinalizes.Add(MoveTemp(Payload));
}

bool UFragmentsImporter::ShouldLogFinalizeOutlier(float ElapsedMs, float ThresholdMs)
{
	if (ThresholdMs <= 0.0f) { return false; }
	return ElapsedMs > ThresholdMs;
}

void UFragmentsImporter::RunMeshFinalize(FPendingMeshFinalize& Payload)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Frag_RunMeshFinalize);
	check(IsInGameThread());

	const double FinalizeStartSeconds = FPlatformTime::Seconds();

	UStaticMesh* M = Payload.WeakMesh.Get();
	if (!M)
	{
		// Model unloaded before the build ran. Finalizer must still fire with null.
		if (Payload.Finalizer) { Payload.Finalizer(nullptr); }
		return;
	}

	if (Payload.MeshDescriptions.Num() == 0 ||
	    Payload.MeshDescriptions[0] == nullptr ||
	    Payload.MeshDescriptions[0]->Triangles().Num() == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("[FinalizeDrain] Empty mesh description for %s"), *Payload.DebugLabel);
		M->RemoveFromRoot();
		if (Payload.Finalizer) { Payload.Finalizer(nullptr); }
		return;
	}

	// Runtime-assembled GPU-only HLOD stand-in: opt out of offline-built GPU features (distance field, nav data, ray-tracing proxy) that this mesh has no data for.
	M->bGenerateMeshDistanceField = false;
	M->bHasNavigationData = false;
	M->bSupportRayTracing = false;

	// Only LOD0 shell meshes need CPU access for cell-merge snapshots.
	M->bAllowCPUAccess = Payload.bNeedsCpuAccess;

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild             = true;
	Params.bBuildSimpleCollision  = false;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty      = false;
	Params.bUseHashAsGuid         = false;
	Params.bAllowCpuAccess        = Payload.bNeedsCpuAccess;

	TArray<const FMeshDescription*> LODDescriptions;
	LODDescriptions.Reserve(Payload.MeshDescriptions.Num());
	for (const TUniquePtr<FMeshDescription>& MD : Payload.MeshDescriptions)
	{
		if (MD.IsValid()) { LODDescriptions.Add(MD.Get()); }
	}

	if (!M->BuildFromMeshDescriptions(LODDescriptions, Params))
	{
		UE_LOG(LogFragments, Warning, TEXT("[FinalizeDrain] BuildFromMeshDescriptions failed for %s"), *Payload.DebugLabel);
		M->RemoveFromRoot();
		if (Payload.Finalizer) { Payload.Finalizer(nullptr); }
		return;
	}

	// Re-resolve each section's material index from the polygon group slot NAME we authored.
	if (FStaticMeshRenderData* RD = M->GetRenderData())
	{
		const TArray<FStaticMaterial>& Mats = M->GetStaticMaterials();
		const int32 NumMappedLODs = FMath::Min(RD->LODResources.Num(), LODDescriptions.Num());
		for (int32 i = 0; i < NumMappedLODs; ++i)
		{
			const FMeshDescription& MD = *LODDescriptions[i];
			FStaticMeshConstAttributes Attrs(MD);
			TPolygonGroupAttributesConstRef<FName> Names = Attrs.GetPolygonGroupMaterialSlotNames();
			FStaticMeshLODResources& LODRes = RD->LODResources[i];

			// Pairing section N with the Nth non-empty polygon group only holds while the engine's skip rule matches the one reimplemented here.
			int32 NumNonEmptyGroups = 0;
			for (FPolygonGroupID PG : MD.PolygonGroups().GetElementIDs())
			{
				if (MD.GetNumPolygonGroupPolygons(PG) > 0) { ++NumNonEmptyGroups; }
			}
			if (!ensureMsgf(NumNonEmptyGroups == LODRes.Sections.Num(),
				TEXT("[FinalizeDrain] %s LOD%d: %d non-empty polygon groups but %d built sections; ")
				TEXT("the engine's section/group correspondence changed -- leaving material indices ")
				TEXT("as built (far LODs may show shuffled materials)"),
				*Payload.DebugLabel, i, NumNonEmptyGroups, LODRes.Sections.Num()))
			{
				continue;
			}

			int32 SectionIdx = 0;
			for (FPolygonGroupID PG : MD.PolygonGroups().GetElementIDs())
			{
				if (MD.GetNumPolygonGroupPolygons(PG) == 0) { continue; }

				const FName SlotName = Names[PG];
				const int32 Slot = Mats.IndexOfByPredicate(
					[&SlotName](const FStaticMaterial& SM) { return SM.MaterialSlotName == SlotName; });

				// INDEX_NONE means the mesh description names a slot the mesh has no material for -- a producer bug, not something to paper over.
				if (Slot != INDEX_NONE)
				{
					LODRes.Sections[SectionIdx].MaterialIndex = Slot;
				}
				else
				{
					UE_LOG(LogFragments, Warning,
						TEXT("[FinalizeDrain] %s LOD%d group '%s' matches no material slot; ")
						TEXT("leaving engine-assigned index %d"),
						*Payload.DebugLabel, i, *SlotName.ToString(),
						LODRes.Sections[SectionIdx].MaterialIndex);
				}
				++SectionIdx;
			}
		}

		// The engine hardcodes 0.75^LOD (StaticMesh.cpp:7175, with a literal "@todo: some way of customizing LOD screen size"), which is unrelated to our tier thresholds.

		const int32 NumLODs = RD->LODResources.Num();
		for (int32 i = 0; i < Payload.LODScreenSizes.Num() && i < NumLODs; ++i)
		{
			RD->ScreenSize[i].Default = Payload.LODScreenSizes[i];
		}
	}

	UBodySetup* BodySetup = NewObject<UBodySetup>(M);
	BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	BodySetup->bDoubleSidedGeometry = false;
	BodySetup->InvalidatePhysicsData();
	if (Payload.TriMesh.IsValid())
	{
		Payload.TriMesh->SetDoCollide(false);
		BodySetup->TriMeshGeometries.Add(MoveTemp(Payload.TriMesh));
		BodySetup->bCreatedPhysicsMeshes = true;
	}
	M->SetBodySetup(BodySetup);

	UE_LOG(LogFragments, Verbose, TEXT("[FinalizeDrain] finalized %s cooked=%d"),
		*Payload.DebugLabel, BodySetup->TriMeshGeometries.Num());

	// Finalizer anchors the mesh (SetStaticMesh on a component) BEFORE RemoveFromRoot.
	if (Payload.Finalizer) { Payload.Finalizer(M); }

	// Outlier log.
	{
		const float ElapsedMs = static_cast<float>(
			(FPlatformTime::Seconds() - FinalizeStartSeconds) * 1000.0);
		if (ShouldLogFinalizeOutlier(ElapsedMs, GFragmentsFinalizeOutlierLogMs))
		{
			UE_LOG(LogFragments, Warning,
				TEXT("[FinalizeOutlier] label=%s tris=%d verts=%d corners=%d lods=%d sink=md ms=%.1f est=%.1f"),
				*Payload.DebugLabel,
				Payload.EstimatedTriangleCount,
				Payload.SourceVertexCount,
				Payload.CornerCount,
				Payload.MeshDescriptions.Num(),
				ElapsedMs,
				EstimateBuildMs(Payload.EstimatedTriangleCount) + Payload.ExtraFinalizeCostMs);
		}
	}

	M->RemoveFromRoot();
}

void UFragmentsImporter::DispatchMeshFinalize(FPendingMeshFinalize&& Payload)
{
	check(IsInGameThread());
	if (GFragmentsFinalizePaced) { EnqueueMeshFinalize(MoveTemp(Payload)); }
	else                         { RunMeshFinalize(Payload); }
}

void UFragmentsImporter::DrainMeshFinalizes(float MaxSliceMs)
{
	check(IsInGameThread());

	// A capped slice uses its share of the frame budget for finalize scheduling.
	const float FullBudget = FMath::Min(FrameBudgetCoordinator.TotalFrameBudgetMs, MaxSliceMs);

	float SpentThisCallMs = 0.f;

	while (PendingMeshFinalizes.Num() > 0)
	{
		const float CoordinatorRemaining = FrameBudgetCoordinator.AllocateRemainingMs();
		if (CoordinatorRemaining <= 0.f) { break; }

		// Bounded share: stop even if the shared coordinator still has room, once this call has spent its own allowance.
		const float Remaining = FMath::Min(CoordinatorRemaining, MaxSliceMs - SpentThisCallMs);
		if (Remaining <= 0.f) { break; }

		const FPendingMeshFinalize& Next = PendingMeshFinalizes[0];
		const float Est = EstimateBuildMs(Next.EstimatedTriangleCount) + Next.ExtraFinalizeCostMs;

		const EFinalizeGate Gate = DecideFinalizeGate(Remaining, Est, FullBudget);
		if (Gate == EFinalizeGate::Defer) { break; }

		// INVARIANT: RunMeshFinalize takes PendingMeshFinalizes[0] BY REFERENCE and invokes Payload.Finalizer(M) from inside it, which for a shell build chains into OnComplete -> CompleteShellBuild -> ProcessResolvedHISMItem.
		const double T0 = FPlatformTime::Seconds();
		RunMeshFinalize(PendingMeshFinalizes[0]);
		PendingMeshFinalizes.RemoveAt(0);
		const float ElapsedMs = static_cast<float>((FPlatformTime::Seconds() - T0) * 1000.0);
		FrameBudgetCoordinator.RecordWork(ElapsedMs);
		SpentThisCallMs += ElapsedMs;
	}
}

void UFragmentsImporter::FlushMeshFinalizes()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Frag_FlushMeshFinalizes);
	check(IsInGameThread());

	if (PendingMeshFinalizes.Num() == 0) { return; }

	UE_LOG(LogFragments, Log, TEXT("[FinalizeDrain] Flushing %d unbuilt mesh finalize(s)"),
		PendingMeshFinalizes.Num());

	for (FPendingMeshFinalize& Payload : PendingMeshFinalizes)
	{
		if (UStaticMesh* M = Payload.WeakMesh.Get()) { M->RemoveFromRoot(); }
		if (Payload.Finalizer) { Payload.Finalizer(nullptr); }
	}
	PendingMeshFinalizes.Empty();
}

void UFragmentsImporter::GetItemData(AFragment*& InFragment)
{
	if (!InFragment || InFragment->GetModelGuid().IsEmpty()) return;

	if (FragmentModels.Contains(InFragment->GetModelGuid()))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragment->GetModelGuid());
		const Model* InModel = Wrapper->GetParsedModel();
		
		int64 ItemIndex = Wrapper->GetFlatIndexForLocalId(InFragment->GetLocalId());
		if (ItemIndex == INDEX_NONE) return;
	
		// Attributes
		const auto* attribute = InModel->attributes()->Get(ItemIndex);
		TArray<FItemAttribute> ItemAttributes = UFragmentsUtils::ParseItemAttribute(attribute);
		InFragment->SetAttributes(ItemAttributes);

		// Category
		const auto* category = InModel->categories()->Get(ItemIndex);
		const char* RawCategory = category->c_str();
		FString CategorySty = UTF8_TO_TCHAR(RawCategory);
		InFragment->SetCategory(CategorySty);

		// Guids
		const auto* item_guid = InModel->guids()->Get(ItemIndex);
		const char* RawGuid = item_guid->c_str();
		FString GuidStr = UTF8_TO_TCHAR(RawGuid);
		InFragment->SetGuid(GuidStr);
	}
}

void UFragmentsImporter::GetItemData(FFragmentItem* InFragmentItem)
{
	if (InFragmentItem->ModelGuid.IsEmpty()) return;

	if (FragmentModels.Contains(InFragmentItem->ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragmentItem->ModelGuid);
		const Model* InModel = Wrapper->GetParsedModel();

		int64 ItemIndex = Wrapper->GetFlatIndexForLocalId(InFragmentItem->LocalId);
		flatbuffers::uoffset_t ii = static_cast<flatbuffers::uoffset_t>(ItemIndex);
		if (ItemIndex == INDEX_NONE) return;

		// Attributes
		if (ii < InModel->attributes()->size())
		{
			const auto* attribute = InModel->attributes()->Get(ItemIndex);
			TArray<FItemAttribute> ItemAttributes = UFragmentsUtils::ParseItemAttribute(attribute);
			InFragmentItem->Attributes = ItemAttributes;
		}

		// Category
		if (ii < InModel->categories()->size())
		{
			const auto* category = InModel->categories()->Get(ItemIndex);
			if (category)  // Null check for category
			{
				const char* RawCategory = category->c_str();
				FString CategorySty = UTF8_TO_TCHAR(RawCategory);
				InFragmentItem->Category = CategorySty;
			}
		}

		// Guids
		if (ii < InModel->guids()->size())
		{
			const auto* item_guid = InModel->guids()->Get(ItemIndex);
			if (item_guid)
			{
				const char* RawGuid = item_guid->c_str();
				FString GuidStr = UTF8_TO_TCHAR(RawGuid);
				InFragmentItem->Guid = GuidStr;
			}
		}
	}
}

TArray<FItemAttribute> UFragmentsImporter::GetItemPropertySets(AFragment* InFragment)
{
	TArray<FItemAttribute> CollectedAttributes;
	if (!InFragment || InFragment->GetModelGuid().IsEmpty()) return CollectedAttributes;
	if (!FragmentModels.Contains(InFragment->GetModelGuid())) return CollectedAttributes;

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragment->GetModelGuid());
	const Model* InModel = Wrapper->GetParsedModel();
	if (!InModel) return CollectedAttributes;

	TSet<int64> Visited;
	CollectPropertiesRecursive(InModel, InFragment->GetLocalId(), Visited, CollectedAttributes);

	return UFragmentsUtils::ParsePropertySets(CollectedAttributes);
}

TArray<FItemAttribute> UFragmentsImporter::GetItemPropertySets(FFragmentItem* InFragment)
{
	TArray<FItemAttribute> CollectedAttributes;
	if (!InFragment || InFragment->ModelGuid.IsEmpty()) return CollectedAttributes;
	if (!FragmentModels.Contains(InFragment->ModelGuid)) return CollectedAttributes;

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragment->ModelGuid);
	const Model* InModel = Wrapper->GetParsedModel();
	if (!InModel) return CollectedAttributes;

	TSet<int64> Visited;
	CollectPropertiesRecursive(InModel, InFragment->LocalId, Visited, CollectedAttributes);

	return UFragmentsUtils::ParsePropertySets(CollectedAttributes);
}

TArray<FItemAttribute> UFragmentsImporter::GetItemPropertySets(int64 LocalId, const FString& InModelGuid)
{
	TArray<FItemAttribute> CollectedAttributes;
	if (InModelGuid.IsEmpty()) return CollectedAttributes;
	if (!FragmentModels.Contains(InModelGuid)) return CollectedAttributes;

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
	if (!Wrapper) return CollectedAttributes;

	const Model* InModel = Wrapper->GetParsedModel();
	if (!InModel) return CollectedAttributes;

	TSet<int64> Visited;
	CollectPropertiesRecursive(InModel, LocalId, Visited, CollectedAttributes);

	return UFragmentsUtils::ParsePropertySets(CollectedAttributes);
}

TArray<FItemAttribute> UFragmentsImporter::GetItemAttributes(int64 LocalId, const FString& InModelGuid)
{
	TArray<FItemAttribute> CollectedAttributes;
	if (InModelGuid.IsEmpty()) return CollectedAttributes;
	if (!FragmentModels.Contains(InModelGuid)) return CollectedAttributes;

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
	if (!Wrapper) return CollectedAttributes;

	const Model* InModel = Wrapper->GetParsedModel();
	if (!InModel) return CollectedAttributes;

	int64 ItemIndex = Wrapper->GetFlatIndexForLocalId(LocalId);
	flatbuffers::uoffset_t ii = static_cast<flatbuffers::uoffset_t>(ItemIndex);
	if (ItemIndex == INDEX_NONE) return CollectedAttributes;

	if (ii < InModel->attributes()->size())
	{
		const auto* attribute = InModel->attributes()->Get(ii);
		CollectedAttributes = UFragmentsUtils::ParseItemAttribute(attribute);
	}
	return CollectedAttributes;
}

AFragment* UFragmentsImporter::GetModelFragment(const FString& ModelGuid)
{
	if (FragmentModels.Contains(ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(ModelGuid);
		return Wrapper->GetSpawnedFragment();
	}
	return nullptr;
}

void UFragmentsImporter::SeedBaseCoordinates(const FTransform& InBase)
{
	if (bBaseCoordinatesInitialized) { return; }
	// Same three fields the UseBaseCoordinates latch sets when the first model loads.
	BaseCoordinates = InBase;
	bBaseCoordinatesInitialized = true;
	SceneZeroZOffset = -BaseCoordinates.GetLocation();
}

void UFragmentsImporter::ResetBaseCoordinates()
{
	BaseCoordinates = FTransform::Identity;
	bBaseCoordinatesInitialized = false;
}

void UFragmentsImporter::ResetAll()
{
	// FIRST, before any actor is destroyed.
	ShutdownCellMerges();
	// Before the roots go: the registrations point at them and their components.
	if (RegistrationManager) { RegistrationManager->ResetAllRegistrations(); }

	// Destroy BucketRoot/SpawnedFragment actors, then release wrapper state
	for (auto& KV : FragmentModels)
	{
		if (KV.Value)
		{
			AFragment* SpawnedRoot = KV.Value->GetSpawnedFragment();
			if (IsValid(SpawnedRoot))
			{
				SpawnedRoot->Destroy();
			}
			KV.Value->ResetWrapper();
		}
	}

	FragmentModels.Empty();

	// Destroy per-actor fragments tracked in ModelFragmentsMap
	for (auto& MapKV : ModelFragmentsMap)
	{
		for (auto& FragKV : MapKV.Value.Fragments)
		{
			if (IsValid(FragKV.Value))
			{
				FragKV.Value->Destroy();
			}
		}
	}
	ModelFragmentsMap.Empty();
	MeshCache.Empty();
	RepresentationMeshCache.Empty();
	MaterialsCache.Empty();
	MaterialPool.Empty();
	PackagesToSave.Empty();

	OwnerRef = nullptr;
	bBaseCoordinatesInitialized = false;
	BaseCoordinates = FTransform::Identity;

	// Clear HISM chunked spawning state
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(HISMSpawnTimerHandle);
	}
	PendingHISMQueue.Empty();
	PendingBucketInstances.Empty();
	DeferredModelLoads.Empty();
	CurrentBucketRoot = nullptr;
	CurrentHISMWrapper = nullptr;
	CurrentMeshesRef = nullptr;
	bIsHISMChunkedSpawning = false;
	HISMItemsSpawned = 0;
	TotalHISMItems = 0;

	// Reset the bucket-finalize + paced tail pipeline so the next spawn starts clean.
	bIsFinalizingBuckets = false;
	FinalizeBucketKeys.Reset();
	FinalizeBucketCursor = 0;
	FinalizeTailPhase = EFinalizeTailPhase::None;
	CellMergeEnqueueQueue.Reset();
	CellMergeEnqueueCursor = 0;
	OverrideApplyQueue.Reset();
	OverrideApplyCursor = 0;
	bIsProcessingDeferredInteriors = false;

	// Clear any in-flight shell-build registry entries: an orphan here would permanently gate ShouldDeferBucketFinalize() open for the next import (see the finalize barrier in ProcessHISMSpawnChunk).
	InFlightShellBuilds.Empty();
	ConsecutiveBucketFinalizeDeferrals = 0;
}

void UFragmentsImporter::ReleaseRefToWorld(UWorld* World)
{
	if (!World) return;

	// World cleanup fires for EVERY world -- a PIE world on Stop, an asset editor's preview world when it closes -- and the editor importer is session-wide.
	if (UWorld* ModelWorld = ResolveModelWorld(); ModelWorld && ModelWorld != World)
	{
		return;
	}

	// 0) Stop world timers (if any)
	if (UWorld* AnyWorld = World)
	{
		AnyWorld->GetTimerManager().ClearTimer(RegistrationUpdateTimerHandle);
	}

	// 1) Drop all actor refs (DO NOT Destroy in cleanup; just sever references)
	OwnerRef = nullptr;
	// Release world-owned registrations before opening another level.
	if (RegistrationManager) { RegistrationManager->ReleaseWorld(World); }
	// Paired with the timer clear above: StreamingWorld is TickRegistrationStreaming's fallback world, so leaving it pointing at a world being torn down would outlive the timer it describes.
	if (StreamingWorld.Get() == World) { StreamingWorld = nullptr; }

	// 3) Reset wrappers and remove ALL models (editor importer is session-wide; models are per-world)
	for (auto& KVP : FragmentModels)
	{
		if (UFragmentModelWrapper* Wrapper = KVP.Value)
		{
			Wrapper->ResetWrapper(); // must null all actor/component refs inside wrapper
		}
	}
	FragmentModels.Empty();

	// 4) Also clear other per-world caches that may contain actors/components or world-outers
	ModelFragmentsMap.Empty();
	// Every other UPROPERTY that can point into the world (issue #19: each one alone keeps the old level from being collected when the editor opens another map).
	ModelBucketRoots.Empty();
	CurrentBucketRoot = nullptr;
	CurrentHISMWrapper = nullptr;
	PendingEditorHISMRegistrations.Empty();

	// (Keep mesh/material caches if they are transient-package assets, but if any are outered to map packages -> clear too) MeshCache.Empty(); MaterialsCache.Empty();.

	PackagesToSave.Empty();
	CurrentMeshesRef = nullptr;
	CurrentSpawningModelGuid.Reset();
}

bool UFragmentsImporter::IsFragmentLoaded(const FString& InModelGuid)
{
	return FragmentModels.Contains(InModelGuid);
}

bool UFragmentsImporter::IsPlacementCarrier(const FString& Cat)
{
	return Cat.Equals(TEXT("IFCSITE"), ESearchCase::IgnoreCase);
}

void UFragmentsImporter::ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
	AFragment* BucketRoot)
{
	FFragmentItem* Item = GetFragmentItemByLocalId(InLocalId, InModelGuid);

	if (!InOwnerRef || !Item) return;

	SetOwnerRef(InOwnerRef);

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
	const Model* ModelRef = Wrapper->GetParsedModel();

	BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
	BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));

	FDateTime StartTime = FDateTime::Now();
	AFragment* SpawnedFrag = SpawnFragmentModel(*Item, OwnerRef.Get(), ModelRef->meshes(), bInSaveMesh,
		Wrapper, BucketRoot);
	Wrapper->SetSpawnedFragment(SpawnedFrag);
	UE_LOG(LogFragments, Verbose, TEXT("Loaded fragment item in [%s]s -> %s"), *(FDateTime::Now() - StartTime).ToString(), *InModelGuid);

	if (PackagesToSave.Num() > 0)
	{
		DeferredSaveManager.AddPackagesToSave(PackagesToSave);
		PackagesToSave.Empty();
	}
}

AFragment* UFragmentsImporter::GetItemByLocalId(int64 LocalId, const FString& ModelGuid)
{
	if (ModelFragmentsMap.Contains(ModelGuid))
	{
		FFragmentLookup Lookup = *ModelFragmentsMap.Find(ModelGuid);

		if (Lookup.Fragments.Contains(LocalId))
		{
			return *Lookup.Fragments.Find(LocalId);
		}
	}
	return nullptr;
}

FFragmentItem* UFragmentsImporter::GetFragmentItemByLocalId(int64 LocalId, const FString& InModelGuid)
{
	if (FragmentModels.Contains(InModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
		return Wrapper->FindItemByLocalId(LocalId);
	}
	return nullptr;
}

bool UFragmentsImporter::ReadAndInflate(const FString& FragPath,
	TArray<uint8>& OutDecompressed,
	FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ReadAndInflate);

	OutDecompressed.Reset();
	OutError.Reset();

	TArray<uint8> CompressedData;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_ReadFile);
		if (!FFileHelper::LoadFileToArray(CompressedData, *FragPath))
		{
			OutError = FString::Printf(TEXT("Failed to load file: %s"), *FragPath);
			return false;
		}
	}
	TRACE_INT_VALUE(TEXT("FragLoad/CompressedBytes"), CompressedData.Num());

	const bool bTryDecompress = (CompressedData.Num() >= 2 && CompressedData[0] == 0x78);
	if (!bTryDecompress)
	{
		OutDecompressed = MoveTemp(CompressedData);
		TRACE_INT_VALUE(TEXT("FragLoad/DecompressedBytes"), OutDecompressed.Num());
		return true;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_Inflate);

	z_stream stream = {};
	stream.next_in = CompressedData.GetData();
	stream.avail_in = CompressedData.Num();

	int32 ret = inflateInit(&stream);
	if (ret != Z_OK)
	{
		OutError = FString::Printf(TEXT("zlib initialization failed: %d"), ret);
		return false;
	}

	const int32 ChunkSize = 1024 * 1024; // 1 MB
	const int64 MaxDecompressedBytes = 1024ll * 1024ll * 1024ll * 2ll; // 2 GB cap

	OutDecompressed.Reserve(FMath::Min<int64>(CompressedData.Num() * 4ll, 64ll * 1024ll * 1024ll));

	bool bStreamEnded = false;

	while (true)
	{
		const int32 OldSize = OutDecompressed.Num();

		if ((int64)OldSize + ChunkSize > MaxDecompressedBytes)
		{
			OutError = FString::Printf(
				TEXT("Decompression aborted: output exceeded limit (%lld bytes)."), MaxDecompressedBytes);
			ret = Z_MEM_ERROR;
			break;
		}

		OutDecompressed.AddUninitialized(ChunkSize);
		stream.next_out = OutDecompressed.GetData() + OldSize;
		stream.avail_out = ChunkSize;

		ret = inflate(&stream, Z_NO_FLUSH);

		if (ret == Z_STREAM_END)
		{
			const int32 TotalOut = OldSize + (ChunkSize - (int32)stream.avail_out);
			OutDecompressed.SetNum(TotalOut, FRAGMENTS_NO_SHRINKING);
			bStreamEnded = true;
			break;
		}

		if (ret == Z_OK)
		{
			continue; // need more output space
		}

		if (ret == Z_BUF_ERROR)
		{
			const int32 Produced = ChunkSize - (int32)stream.avail_out;
			if (Produced > 0)
			{
				continue;
			}
			OutError = TEXT("Decompression stalled (Z_BUF_ERROR) with no output produced.");
			break;
		}

		OutError = FString::Printf(TEXT("Decompression failed. zlib ret=%d msg=%s"),
			ret, stream.msg ? UTF8_TO_TCHAR(stream.msg) : TEXT("null"));

		const int32 ProducedSoFar = OldSize + (ChunkSize - (int32)stream.avail_out);
		OutDecompressed.SetNum(ProducedSoFar, FRAGMENTS_NO_SHRINKING);
		break;
	}

	const int32 endRet = inflateEnd(&stream);
	if (endRet != Z_OK)
	{
		UE_LOG(LogFragments, Warning, TEXT("inflateEnd returned %d"), endRet);
	}

	if (!bStreamEnded)
	{
		// Looked like zlib but is not.
		if (ret == Z_DATA_ERROR)
		{
			UE_LOG(LogFragments, Warning,
				TEXT("Data looked like zlib but is not (Z_DATA_ERROR). Falling back to raw data."));
			OutDecompressed = MoveTemp(CompressedData);
			OutError.Reset();
			TRACE_INT_VALUE(TEXT("FragLoad/DecompressedBytes"), OutDecompressed.Num());
			return true;
		}

		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Decompression failed (ret=%d)."), ret);
		}
		return false;
	}

	TRACE_INT_VALUE(TEXT("FragLoad/DecompressedBytes"), OutDecompressed.Num());
	return true;
}

FString UFragmentsImporter::LoadFragmentFromBuffer(TArray<uint8>&& Decompressed,
	const FString& FragPath,
	const FFragmentLoadOptions& Options)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_LoadFragmentFromBuffer);
	check(IsInGameThread());

	// r.Fragments.Memory.CellBudgetMB: clear the cell-merge census accumulator at the START of an import, not just at FlushCellCensus's end.
	CellCensusReport = FragmentCellCensus::FCensusReport();

	// LoadModel moves the buffer into a shared pointer (zero-copy) and verifies it before casting the FlatBuffers root -- a walk over the whole buffer, ~10-13 ms for a ~90 MB model.
	UFragmentModelWrapper* Wrapper = nullptr;
	const Model* ModelRef = nullptr;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_ParseModel);
		Wrapper = NewObject<UFragmentModelWrapper>(this);
		Wrapper->LoadModel(MoveTemp(Decompressed));
		Wrapper->FragmentPath = FragPath;
		ModelRef = Wrapper->GetParsedModel();
	}

	if (!ModelRef)
	{
		UE_LOG(LogFragments, Error,
			TEXT("'%s' is not a valid Fragments model (corrupt, truncated, or not a .frag); nothing was imported."),
			*FragPath);
		return FString();
	}

	const auto* guid = ModelRef->guid();
	const char* RawModelGuid = guid->c_str();
	FString ModelGuidStr = UTF8_TO_TCHAR(RawModelGuid);
	const auto* local_ids = ModelRef->local_ids();
	const auto* _meshes = ModelRef->meshes();

	// Root coordinate auto-detection: compute root transform from mesh coordinates Uses MakeTransform with bIsRootCoordinates=true to pick best XY vs XZ basis.
	const auto* spatial_structure = ModelRef->spatial_structure();

	// Walks the mesh coordinate set to pick the XY vs XZ basis.
	FTransform RootTransform;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_RootTransform);
		RootTransform = UFragmentsUtils::MakeTransform(_meshes->coordinates(), false, true);
	}
	FTransform RootWorld;

	// Sidecar Placement, when present, overrides Options.PlacementTransform (not composed).
	FTransform EffectivePlacement = Options.PlacementTransform;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_OverridesSidecar);
		FFragmentOverrides Parsed;
		if (UE::Fragments::Overrides::Load(FragPath, /*ModelGuid=*/TEXT(""), Parsed)
			&& Parsed.Placement.bHasValue)
		{
			EffectivePlacement = FTransform(Parsed.Placement.Rotation, Parsed.Placement.Location);
		}
	}

	switch (Options.OriginMode)
	{
	case EFragmentOriginMode::UseBaseCoordinates:
	{
		// The first model defines the base; later models retain their offset from it.
		if (!bBaseCoordinatesInitialized)
		{
			BaseCoordinates = RootTransform;
			bBaseCoordinatesInitialized = true;
			SceneZeroZOffset = -BaseCoordinates.GetLocation();
		}

		const FVector RootOffset = BaseCoordinates.GetLocation() - RootTransform.GetLocation();
		UE_LOG(LogFragments, Verbose, TEXT("[COORD]   RootOffset: %s"), *RootOffset.ToString());

		RootWorld = RootTransform;
		RootWorld.SetTranslation(RootOffset);
		break;
	}

	case EFragmentOriginMode::UseWorldOrigin:
	{
		// Neutralize the Revit file's own root — the model's natural origin lands at world zero.
		RootWorld = FTransform::Identity;
		break;
	}
	}

	// Post-placement nudge — applies in both modes. Identity = no-op.
	RootWorld = EffectivePlacement * RootWorld;
	Wrapper->SetBakedPlacement(EffectivePlacement);
	Wrapper->OriginMode = Options.OriginMode;
	Wrapper->OriginBase = BaseCoordinates;

	// Diagnostic logging for coordinate verification
	UE_LOG(LogFragments, Verbose, TEXT("[COORD] ModelGuid: %s"), *ModelGuidStr);
	UE_LOG(LogFragments, Verbose, TEXT("[COORD]   RootTransform Location: %s  Rotation: %s"),
		*RootTransform.GetLocation().ToString(), *RootTransform.GetRotation().Rotator().ToString());
	UE_LOG(LogFragments, Verbose, TEXT("[COORD]   BaseCoordinates Location: %s  Rotation: %s"),
		*BaseCoordinates.GetLocation().ToString(), *BaseCoordinates.GetRotation().Rotator().ToString());
	UE_LOG(LogFragments, Verbose, TEXT("[COORD]   SceneZeroZOffset: %s"), *SceneZeroZOffset.ToString());
	UE_LOG(LogFragments, Verbose, TEXT("[COORD]   OriginMode: %d  PlacementLocation: %s"),
		static_cast<int32>(Options.OriginMode), *EffectivePlacement.GetLocation().ToString());
	UE_LOG(LogFragments, Verbose, TEXT("[COORD]   Final RootWorld Location: %s  Rotation: %s"),
		*RootWorld.GetLocation().ToString(), *RootWorld.GetRotation().Rotator().ToString());

	FFragmentItem FragmentItem;
	FragmentItem.Guid = ModelGuidStr;
	FragmentItem.ModelGuid = ModelGuidStr;
	FragmentItem.GlobalTransform = RootWorld;

	// The spatial-hierarchy walk.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_MapModelStructure);
		UFragmentsUtils::MapModelStructureToData(spatial_structure, FragmentItem, TEXT(""));
	}

	Wrapper->SetModelItem(FragmentItem);

	if (FragmentModels.Contains(ModelGuidStr))
	{
		UE_LOG(LogFragments, Verbose, TEXT("LoadFragment: reimport detected for %s — purging stale state"), *ModelGuidStr);
		PurgeModelState(ModelGuidStr, /*bRemoveFromMap=*/false);
	}
	FragmentModels.Add(ModelGuidStr, Wrapper);

	// Loop through samples and spawn meshes
	if (_meshes)
	{
		const auto* samples = _meshes->samples();
		const auto* representations = _meshes->representations();
		const auto* coordinates = _meshes->coordinates();
		const auto* meshes_items = _meshes->meshes_items();
		const auto* materials = _meshes->materials();
		const auto* cirle_extrusions = _meshes->circle_extrusions();
		const auto* shells = _meshes->shells();
		const auto* local_tranforms = _meshes->local_transforms();
		const auto* global_transforms = _meshes->global_transforms();

		// Grouping samples by Item ID.
		TMap<int64, TArray<const Sample*>> SamplesByItem;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_GroupSamplesByItem);
			for (flatbuffers::uoffset_t i = 0; i < samples->size(); i++)
			{
				const auto* sample = samples->Get(i);
				SamplesByItem.FindOrAdd(sample->item()).Add(sample);
			}
			TRACE_INT_VALUE(TEXT("FragLoad/SampleCount"), (int32)samples->size());
			TRACE_INT_VALUE(TEXT("FragLoad/ItemCount"), SamplesByItem.Num());
		}

		// Per-item walk: FindItemByLocalId + GetItemData + a transform compose each.
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_Load_BuildItemSamples);

		for (const auto& Item : SamplesByItem)
		{
			int64 ItemId = Item.Key;

			const TArray<const Sample*> ItemSamples = Item.Value;

			const auto mesh = meshes_items->Get(ItemId);
			const auto local_id = local_ids->Get(ItemId);

			FFragmentItem* FoundFragmentItem = Wrapper->FindItemByLocalId(local_id);
			if (!FoundFragmentItem)
			{
				continue;
			}

			GetItemData(FoundFragmentItem);

			const auto* global_transform = global_transforms->Get(mesh);
			FTransform ItemGlobal = UFragmentsUtils::MakeTransform(global_transform);
			FTransform ItemWorld = ItemGlobal * RootWorld;
			FoundFragmentItem->GlobalTransform = ItemWorld;

			for (int32 i = 0; i < ItemSamples.Num(); i++)
			{
				const Sample* sample = ItemSamples[i];
				const auto* material = materials->Get(sample->material());
				const auto* representation = representations->Get(sample->representation());
				const auto* local_transform = local_tranforms->Get(sample->local_transform());

				FFragmentSample SampleInfo;
				SampleInfo.SampleIndex = i;
				SampleInfo.LocalTransformIndex = sample->local_transform();
				SampleInfo.RepresentationIndex = sample->representation();
				SampleInfo.MaterialIndex = sample->material();

				FoundFragmentItem->Samples.Add(SampleInfo);
			}

		}

	}
	ModelFragmentsMap.Add(ModelGuidStr, FFragmentLookup());

	return ModelGuidStr;
}

FString UFragmentsImporter::LoadFragment(const FString& FragPath, const FFragmentLoadOptions& Options)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_LoadFragment);

	// One load at a time across BOTH entry points, not just async-vs-async.
	if (bAsyncLoadInFlight)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("LoadFragment: rejected '%s' - an async load is already in flight. Wait for "
			     "its OnComplete before loading again, or use LoadFragmentAsync."),
			*FragPath);
		return FString();
	}

	TArray<uint8> Decompressed;
	FString Error;
	if (!ReadAndInflate(FragPath, Decompressed, Error))
	{
		UE_LOG(LogFragments, Error, TEXT("LoadFragment: %s"), *Error);
		return FString();
	}

	return LoadFragmentFromBuffer(MoveTemp(Decompressed), FragPath, Options);
}

void UFragmentsImporter::LoadFragmentAsyncNative(const FString& FragPath,
	const FFragmentLoadOptions& Options,
	TFunction<void(bool, const FString&, const FString&)> OnComplete)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_LoadFragmentAsync);
	check(IsInGameThread());

	// One load per importer while concurrency is deferred.
	if (bAsyncLoadInFlight)
	{
		const FString Error = FString::Printf(
			TEXT("LoadFragmentAsync rejected: a load is already in flight (requested '%s')."),
			*FragPath);
		UE_LOG(LogFragments, Warning, TEXT("%s"), *Error);
		OnComplete(false, Error, FString());
		return;
	}

	bAsyncLoadInFlight = true;

	TWeakObjectPtr<UFragmentsImporter> WeakThis(this);
	const FString PathCopy = FragPath;

	AsyncLoadTask = UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[WeakThis, PathCopy, Options, OnComplete = MoveTemp(OnComplete)]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_LoadFragmentOnWorker);

			// Worker half: no UObject touched.
			TArray<uint8> Decompressed;
			FString Error;
			const bool bReadOk = UFragmentsImporter::ReadAndInflate(PathCopy, Decompressed, Error);

			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, PathCopy, Options, OnComplete = MoveTemp(OnComplete),
				 Decompressed = MoveTemp(Decompressed), Error, bReadOk]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_LoadFragmentFinalize);
					check(IsInGameThread());

					UFragmentsImporter* ImporterPtr = WeakThis.Get();
					if (!ImporterPtr)
					{
						// Importer torn down between worker completion and this hop.
						return;
					}

					// Clear the in-flight flag before reentrant completion callbacks.
					ImporterPtr->bAsyncLoadInFlight = false;

					if (!bReadOk)
					{
						UE_LOG(LogFragments, Error, TEXT("LoadFragmentAsync: %s"), *Error);
						OnComplete(false, Error, FString());
						return;
					}

					const FString Guid = ImporterPtr->LoadFragmentFromBuffer(
						MoveTemp(Decompressed), PathCopy, Options);

					if (Guid.IsEmpty())
					{
						const FString BuildError = FString::Printf(
							TEXT("Failed to build model from '%s'."), *PathCopy);
						OnComplete(false, BuildError, FString());
						return;
					}

					OnComplete(true, FString(), Guid);
				});
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);
}

void UFragmentsImporter::ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
	AFragment* BucketRoot, FOnProcessLoadedFragmentComplete OnComplete)
{
	UE_LOG(LogFragments, Verbose, TEXT("ProcessLoadedFragment START - ModelGuid: %s, Owner: %p"), *InModelGuid, InOwnerRef);

	// Check if model exists
	if (!FragmentModels.Contains(InModelGuid))
	{
		UE_LOG(LogFragments, Error, TEXT("ProcessLoadedFragment: Model not in FragmentModels!"));
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(false, TEXT("ProcessLoadedFragment: Model not in FragmentModels!"), TEXT(""));
		}
		return;
	}

	if (!InOwnerRef)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("ProcessLoadedFragment: InOwnerRef is null for model %s — aborting. Pass any valid AActor (e.g. self from a level Blueprint) on the In Owner Ref pin."),
			*InModelGuid);
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(false, TEXT("ProcessLoadedFragment: InOwnerRef is nullptr!"), TEXT(""));
		}
		return;
	}

	// Ensure RegistrationManager exists before FinalizeHISMBuckets, which now enqueues cell merges and needs to resolve RegMgr.
	if (!RegistrationManager)
	{
		RegistrationManager = NewObject<UFragmentRegistrationManager>(this);
		RegistrationManager->OnInteriorLoadRequested.AddDynamic(
			this, &UFragmentsImporter::HandleInteriorLoadRequested);
	}

	// If a chunked HISM spawn is already in progress, defer this load
	if (bIsHISMChunkedSpawning)
	{
		UE_LOG(LogFragments, Verbose, TEXT("[HISM Chunked] Spawn in progress - deferring model %s"), *InModelGuid);
		FDeferredModelLoad Deferred;
		Deferred.ModelGuid = InModelGuid;
		Deferred.Owner = InOwnerRef;
		Deferred.bSaveMeshes = bInSaveMesh;
		Deferred.BucketRoot = BucketRoot;
		Deferred.OnCompleteCallback = OnComplete;
		DeferredModelLoads.Add(MoveTemp(Deferred));
		return;
	}

	// Bind Delegate
	OnProcessLoadedFragmentComplete = OnComplete;

	SetOwnerRef(InOwnerRef);

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
	const Model* ModelRef = Wrapper->GetParsedModel();

	BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
	BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));

	// Resolve the world from the OWNER, not from this importer.
	UWorld* World = OwnerRef.IsValid() ? OwnerRef->GetWorld() : nullptr;
	if (!World)
	{
		World = GetWorld();
	}
	UE_LOG(LogFragments, Display,
		TEXT("[Import] Model %s -> %s path (owner=%s, world=%s)"),
		*InModelGuid,
		World ? TEXT("chunked/cell-partitioned") : TEXT("synchronous/UNPARTITIONED"),
		OwnerRef.IsValid() ? *OwnerRef->GetName() : TEXT("none"),
		World ? *World->GetName() : TEXT("none"));

	if (!World)
	{
		LoadOverridesIntoWrapper(Wrapper, InModelGuid);

		FDateTime StartTime = FDateTime::Now();
		AFragment* SpawnedRoot = SpawnFragmentModel(Wrapper->GetModelItem(), OwnerRef.Get(),
			ModelRef->meshes(), bInSaveMesh, Wrapper, BucketRoot);
		Wrapper->SetSpawnedFragment(SpawnedRoot);
		UE_LOG(LogFragments, Verbose, TEXT("Editor direct: Loaded model in [%s]s -> %s"),
			*(FDateTime::Now() - StartTime).ToString(), *InModelGuid);

		if (SpawnedRoot)
		{
			for (auto& KV : SpawnedRoot->Buckets)
			{
				UHierarchicalInstancedStaticMeshComponent* HISM = KV.Value;
				if (IsValid(HISM) && !HISM->IsRegistered())
				{
					HISM->RegisterComponent();
				}
			}

			// Also apply overrides on the editor-direct (non-chunked) path.
			ApplyMaterialOverrides(SpawnedRoot, Wrapper);
		}

		if (PackagesToSave.Num() > 0)
		{
			DeferredSaveManager.AddPackagesToSave(PackagesToSave);
			PackagesToSave.Empty();
		}

		// Unbind before Execute to avoid killing a re-entrant rebind.
		if (OnProcessLoadedFragmentComplete.IsBound())
		{
			FOnProcessLoadedFragmentComplete Local = OnProcessLoadedFragmentComplete;
			OnProcessLoadedFragmentComplete.Unbind();
			Local.Execute(true, TEXT(""), InModelGuid);
		}
		return;
	}

	// HISM PATH: Chunked frame-budgeted spawning.
	{
		if (!bIsHISMChunkedSpawning)
		{
			SuppressImportOverhead();
		}

		FFragmentItem& RootItem = Wrapper->GetModelItemRef();

		// Create BucketRoot actor
		AFragment* BucketRootActor = OwnerRef->GetWorld()->SpawnActor<AFragment>();
		BucketRootActor->SetAsBucketRoot(true);
		BucketRootActor->SetHISMEnabled(true);
		BucketRootActor->SetData(RootItem);
		// RootWorld (GlobalTransform) is baked into the instances below, so Save Placement needs it and the placement it contains to turn this actor's transform back into an absolute one.
		BucketRootActor->SetBakedFrame(RootItem.GlobalTransform, Wrapper->GetBakedPlacement());
		// ROOT COMPONENT, and it must exist BEFORE the first bucket is created.
		EnsureRootComponent(*BucketRootActor);
		// FragmentPath drives the BeginPlay refresh, so tag ONLY a root spawned outside play (an editor import).
		if (!OwnerRef->GetWorld()->IsGameWorld())
		{
			BucketRootActor->SetFragmentPath(Wrapper->FragmentPath);
			BucketRootActor->SetImportOrigin(Wrapper->OriginMode, Wrapper->OriginBase);
		}
		Wrapper->SetSpawnedFragment(BucketRootActor);
		CurrentBucketRoot = BucketRootActor;

		// Debug: optionally auto-attach the LOD colorize component (Cesium-style).
		if (CVarLODDebugAutoAttach.GetValueOnGameThread() != 0)
		{
			UFragmentLODDebugComponent* DebugComp =
				NewObject<UFragmentLODDebugComponent>(BucketRootActor);
			DebugComp->RegisterComponent();
			BucketRootActor->AddInstanceComponent(DebugComp);
		}

		// Store references for chunk processing
		CurrentMeshesRef = ModelRef->meshes();
		bCurrentSaveMeshes = bInSaveMesh;
		CurrentSpawningModelGuid = InModelGuid;
		CurrentHISMWrapper = Wrapper;

		LoadOverridesIntoWrapper(Wrapper, InModelGuid);

		// Build LocalId → Floor map for cell partition pass.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_BuildFloorMap);
			UE::Fragments::CellPartition::BuildLocalIdToFloorMap(
				CurrentHISMWrapper->GetModelItemRef(),
				CurrentHISMWrapper->LocalIdToFloor);

			int16 MaxFloor = 0;
			for (const TPair<int64, int16>& Pair : CurrentHISMWrapper->LocalIdToFloor)
			{
				MaxFloor = FMath::Max(MaxFloor, Pair.Value);
			}
			UE_LOG(LogFragments, Verbose,
				TEXT("[FloorMap] %d items mapped, %d floors detected"),
				CurrentHISMWrapper->LocalIdToFloor.Num(),
				MaxFloor + 1);
		}

		// Build flat spawn queue from hierarchy (fast — no mesh creation)
		PendingHISMQueue.Empty();
		PendingBucketInstances.Empty();
		// Reset before the (single) root call: BuildHISMSpawnQueue recurses into children internally, and NextHISMQueueIndex is a member specifically so it keeps counting across that recursion instead of restarting per level.
		NextHISMQueueIndex = 0;
		BuildHISMSpawnQueue(RootItem, FTransform::Identity, INDEX_NONE);

		// Compute ModelAnchor from all queued items (shell + deferred interior).
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ComputeModelAnchor);
			FBox AccumulatedBounds(ForceInit);
			for (const FPendingHISMItem& Item : PendingHISMQueue)
			{
				AccumulatedBounds += Item.WorldTransform.GetLocation();
			}
			// Include deferred interior items so interior cell coordinates align with shell cells.
			if (const TArray<FPendingHISMItem>* Deferred = DeferredInteriorQueues.Find(CurrentSpawningModelGuid))
			{
				for (const FPendingHISMItem& Item : *Deferred)
				{
					AccumulatedBounds += Item.WorldTransform.GetLocation();
				}
			}
			CurrentHISMWrapper->ModelAnchor = AccumulatedBounds.IsValid ? AccumulatedBounds.Min : FVector::ZeroVector;
			UE_LOG(LogFragments, Verbose,
				TEXT("[Anchor] ModelAnchor=(%.0f, %.0f, %.0f)"),
				CurrentHISMWrapper->ModelAnchor.X,
				CurrentHISMWrapper->ModelAnchor.Y,
				CurrentHISMWrapper->ModelAnchor.Z);
		}

		TotalHISMItems = PendingHISMQueue.Num();
		HISMItemsSpawned = 0;
		bIsHISMChunkedSpawning = true;

		UE_LOG(LogFragments, Verbose, TEXT("[HISM Chunked] Queued %d items for frame-budgeted spawning."), TotalHISMItems);

		if (bEnableShellFirstStreaming)
		{
			const TArray<FPendingHISMItem>* DeferredForModel = DeferredInteriorQueues.Find(CurrentSpawningModelGuid);
			const int32 DeferredCount = DeferredForModel ? DeferredForModel->Num() : 0;
			UE_LOG(LogFragments, Verbose, TEXT("[Shell-First] Shell queue: %d items, Deferred interior: %d items"),
				PendingHISMQueue.Num(), DeferredCount);
		}

		// Start frame-budgeted timer
		if (World)
		{
			World->GetTimerManager().SetTimer(
				HISMSpawnTimerHandle, this,
				&UFragmentsImporter::ProcessHISMSpawnChunk,
				0.016f, true);
		}

		return;
	}
}


TArray<int64> UFragmentsImporter::GetElementsByCategory(const FString& InCategory, const FString& ModelGuid)
{
	TArray<int64> LocalIds;

	if (FragmentModels.Contains(ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(ModelGuid);
		const Model* InModel = Wrapper->GetParsedModel();

		if (!InModel)
			return LocalIds;

		const auto* categories = InModel->categories();
		const auto* local_ids = InModel->local_ids();

		if (!categories || !local_ids)
			return LocalIds;

		for (flatbuffers::uoffset_t i = 0; i < categories->size(); i++)
		{
			const auto* category = categories->Get(i);
			FString CategoryName = UTF8_TO_TCHAR(category->c_str());

			if (CategoryName.Equals(InCategory, ESearchCase::IgnoreCase))
			{
				int64 LocalId = local_ids->Get(i);
				LocalIds.Add(LocalId);
			}
		}
	}
	return LocalIds;
}

void UFragmentsImporter::PurgeInFlightShellBuildsForModel(const FString& ModelGuid)
{
	check(IsInGameThread());

	// In-flight shell builds are shared by SamplePath across resident models.
	for (auto It = InFlightShellBuilds.CreateIterator(); It; ++It)
	{
		TArray<FPendingHISMItem>& Waiters = It.Value().Waiters;
		Waiters.RemoveAll([&ModelGuid](const FPendingHISMItem& Waiter)
		{
			return Waiter.ModelGuid == ModelGuid;
		});

		if (Waiters.Num() == 0)
		{
			// Nothing left to serve for this SamplePath.
			It.RemoveCurrent();
		}
	}
}

void UFragmentsImporter::PurgeModelState(const FString& ModelGuid, bool bRemoveFromMap)
{
	CancelProxyBuilds(ModelGuid);

	// Keep the shared finalize queue when purging only one model.

	DeferredInteriorQueues.Remove(ModelGuid);
	ModelBucketRoots.Remove(ModelGuid);
	ProxyAccumulators.Remove(ModelGuid);
	// Purge buffered proxy contributions on unload to prevent duplicate geometry after reimport.
	PendingProxyContributions.Remove(ModelGuid);

	// Invalidate any in-flight proxy-flush worker for this model.
	ProxyFlushEpoch.FindOrAdd(ModelGuid)++;

	// Selectively drop this model's waiters from the in-flight shell-build registry.
	PurgeInFlightShellBuildsForModel(ModelGuid);

	PendingHISMQueue.RemoveAll([&ModelGuid](const FPendingHISMItem& Item)
	{
		return Item.ModelGuid == ModelGuid;
	});

	const bool bAbortedSpawn = bIsHISMChunkedSpawning && CurrentSpawningModelGuid == ModelGuid;
	if (bAbortedSpawn)
	{
		if (OwnerRef.IsValid())
		{
			if (UWorld* World = OwnerRef->GetWorld())
			{
				World->GetTimerManager().ClearTimer(HISMSpawnTimerHandle);
			}
		}
		PendingHISMQueue.Empty();
		PendingBucketInstances.Empty();
		bIsHISMChunkedSpawning = false;
		bIsProcessingDeferredInteriors = false;
		HISMItemsSpawned = 0;
		TotalHISMItems = 0;
		CurrentBucketRoot = nullptr;
		CurrentHISMWrapper = nullptr;
		CurrentMeshesRef = nullptr;

		// Reset the bucket-finalize + paced tail pipeline so a reimport starts clean.
		bIsFinalizingBuckets = false;
		FinalizeBucketKeys.Reset();
		FinalizeBucketCursor = 0;
		FinalizeTailPhase = EFinalizeTailPhase::None;
		CellMergeEnqueueQueue.Reset();
		CellMergeEnqueueCursor = 0;
		OverrideApplyQueue.Reset();
		OverrideApplyCursor = 0;

		// InFlightShellBuilds itself is already purged unconditionally above, before this branch — only reset the stall-log counter here, since it is meaningful only for the spawn session actually being aborted.
		ConsecutiveBucketFinalizeDeferrals = 0;

		UE_LOG(LogFragments, Verbose, TEXT("PurgeModelState: Aborted in-progress HISM spawn for %s"), *ModelGuid);
	}

	if (UFragmentModelWrapper** WrapperPtr = FragmentModels.Find(ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *WrapperPtr;
		if (Wrapper)
		{
			AFragment* SpawnedRoot = Wrapper->GetSpawnedFragment();
			if (IsValid(SpawnedRoot))
			{
				SpawnedRoot->Destroy();
				UE_LOG(LogFragments, Verbose, TEXT("PurgeModelState: Destroyed BucketRoot/SpawnedFragment for %s"), *ModelGuid);
			}
			Wrapper->ResetWrapper();
		}

		if (bRemoveFromMap)
		{
			FragmentModels.Remove(ModelGuid);
		}
	}

	if (FFragmentLookup* Lookup = ModelFragmentsMap.Find(ModelGuid))
	{
		for (TPair<int64, AFragment*>& Obj : Lookup->Fragments)
		{
			if (IsValid(Obj.Value))
			{
				Obj.Value->Destroy();
			}
		}
		ModelFragmentsMap.Remove(ModelGuid);
	}

	const FString GuidPrefix = FString::Printf(TEXT("/Game/Buildings/%s/"), *ModelGuid);
	for (auto It = MeshCache.CreateIterator(); It; ++It)
	{
		if (It.Key().StartsWith(GuidPrefix))
		{
			It.RemoveCurrent();
		}
	}

	RepresentationMeshCache.Empty();

#if WITH_EDITOR
	if (GIsEditor && GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType != EWorldType::Editor) continue;
			UWorld* EditorWorld = Ctx.World();
			if (!EditorWorld) continue;
			for (TActorIterator<AFragment> It(EditorWorld); It; ++It)
			{
				AFragment* Fragment = *It;
				if (IsValid(Fragment) && Fragment->GetModelGuid() == ModelGuid)
				{
					Fragment->Destroy();
				}
			}
		}
	}
#endif

	// Drain parked loads after fully purging an aborted spawn.
	if (bAbortedSpawn)
	{
		DrainNextDeferredModelLoad();
	}
}

void UFragmentsImporter::UnloadFragment(const FString& ModelGuid)
{
	PurgeModelState(ModelGuid, /*bRemoveFromMap=*/true);
}

void UFragmentsImporter::CollectPropertiesRecursive(
	const Model* InModel,
	int64 StartLocalId,
	TSet<int64>& Visited,
	TArray<FItemAttribute>& OutAttributes)
{
	if (!InModel || Visited.Contains(StartLocalId)) return;
	Visited.Add(StartLocalId);

	const auto* relations = InModel->relations();
	const auto* attributes = InModel->attributes();
	const auto* relations_items = InModel->relations_items();

	for (flatbuffers::uoffset_t i = 0; i < relations_items->size(); i++)
	{
		if (relations_items->Get(i) != StartLocalId) continue;

		const auto* Relation = relations->Get(i);
		if (!Relation || !Relation->data()) continue;

		for (flatbuffers::uoffset_t j = 0; j < Relation->data()->size(); j++)
		{
			const char* RawStr = Relation->data()->Get(j)->c_str();
			FString Cleaned = UTF8_TO_TCHAR(RawStr);

			TArray<FString> Tokens;
			Cleaned.Replace(TEXT("["), TEXT(""))
				.Replace(TEXT("]"), TEXT(""))
				.ParseIntoArray(Tokens, TEXT(","), true);

			if (Tokens.Num() < 2) continue;

			FString RelationName = Tokens[0].TrimStartAndEnd().Replace(TEXT("\""), TEXT(""));

			// Follow property-related and classification relations (skip purely spatial/geometric ones)
			static const TSet<FString> AllowedRelations = {
				TEXT("IsDefinedBy"), TEXT("HasProperties"), TEXT("DefinesType"),
				TEXT("HasAssociations"), TEXT("HasAssignments"), TEXT("IsTypedBy"),
				TEXT("HasContext"), TEXT("Declares"), TEXT("Nests"),
				TEXT("HasOpenings"), TEXT("FillsVoids")
			};
			if (!AllowedRelations.Contains(RelationName))
				continue;

			for (int32 k = 1; k < Tokens.Num(); ++k)
			{
				int64 RelatedLocalId = FCString::Atoi64(*Tokens[k].TrimStartAndEnd());
				if (Visited.Contains(RelatedLocalId)) continue;

				// Try resolving RelatedLocalId to attribute
				flatbuffers::uoffset_t AttrIndex = UFragmentsUtils::GetIndexForLocalId(InModel, RelatedLocalId);
				if (AttrIndex != INDEX_NONE && attributes && AttrIndex < attributes->size())
				{
					const auto* Attr = attributes->Get(AttrIndex);
					if (Attr)
					{
						TArray<FItemAttribute> Props = UFragmentsUtils::ParseItemAttribute(Attr);
						OutAttributes.Append(Props);
					}
				}

				// Recurse
				CollectPropertiesRecursive(InModel, RelatedLocalId, Visited, OutAttributes);
			}
		}
	}
}


void UFragmentsImporter::SpawnStaticMesh(UStaticMesh* StaticMesh,const Transform* LocalTransform, const Transform* GlobalTransform, AActor* Owner, FName OptionalTag)
{
	if (!StaticMesh || !LocalTransform || !GlobalTransform || !Owner) return;


	// Convert Fragments transform → Unreal FTransform
	FVector Local(LocalTransform->position().x() * 100, LocalTransform->position().z() * 100, LocalTransform->position().y() * 100); // Fix z-up and Unreal units
	FVector XLocal(LocalTransform->x_direction().x(), LocalTransform->x_direction().z(), LocalTransform->x_direction().y());
	FVector YLocal(LocalTransform->y_direction().x(), LocalTransform->y_direction().z(), LocalTransform->y_direction().y());
	FVector ZLocal = FVector::CrossProduct(XLocal, YLocal);

	FVector Global(GlobalTransform->position().x() * 100, GlobalTransform->position().z() * 100, GlobalTransform->position().y() * 100); // Fix z-up and Unreal units
	FVector XGlobal(GlobalTransform->x_direction().x(), GlobalTransform->x_direction().z(), GlobalTransform->x_direction().y());
	FVector YGlobal(GlobalTransform->y_direction().x(), GlobalTransform->y_direction().z(), GlobalTransform->y_direction().y());
	FVector ZGlobal = FVector::CrossProduct(XGlobal, YGlobal);

	FVector Pos = Global + Local;

	FMatrix GlobalMatrix(XGlobal, YGlobal, ZGlobal, FVector::ZeroVector);  // Global rotation matrix
	FMatrix LocalMatrix(XLocal, YLocal, ZLocal, FVector::ZeroVector);  // Local rotation matrix



	FMatrix FinalRotationMatrix = LocalMatrix.Inverse() * GlobalMatrix;



	FRotator Rot = FinalRotationMatrix.Rotator();
	FVector FinalScale = FVector(1.0f, 1.0f, 1.0f);

	FTransform Transform(Rot, Pos, FinalScale);

	// Step 1: Spawn StaticMeshActor
	AStaticMeshActor* MeshActor = Owner->GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform);

	if (MeshActor == nullptr)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to spawn StaticMeshActor."));
		return;
	}

	// Step 2: Set Mesh Actor's mobility to movable (so it can be adjusted in runtime)
	MeshActor->GetRootComponent()->SetMobility(EComponentMobility::Movable);

	// Step 3: Ensure the mesh component is available (create one if needed)
	UStaticMeshComponent* MeshComponent = MeshActor->GetStaticMeshComponent();
	if (!MeshComponent)
	{
		// If no mesh component exists, create and register one
		MeshComponent = NewObject<UStaticMeshComponent>(MeshActor);
		MeshComponent->SetupAttachment(MeshActor->GetRootComponent());
		MeshComponent->RegisterComponent();
	}

	// Step 4: Set the static mesh
	MeshComponent->SetStaticMesh(StaticMesh);

	// Step 5: Mark the actor as needing an update to refresh its mesh
	MeshActor->MarkComponentsRenderStateDirty();
	MeshActor->SetActorTransform(Transform); // Apply the final transform

	// Step 6: Save the StaticMesh used


	// Ensure mesh is added and visible
	MeshComponent->SetVisibility(true);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	// Add Optional tag
	MeshActor->Tags.Add(OptionalTag);
}

void UFragmentsImporter::ExtractShellGeometry(
	const Shell* ShellRef,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs)
{
	if (!ShellRef || !ShellRef->points())
	{
		return;
	}

	const auto* Points = ShellRef->points();

	// Extract vertices (convert from FlatBuffers FloatVector to FVector)
	for (flatbuffers::uoffset_t i = 0; i < Points->size(); i++)
	{
		const auto& P = *Points->Get(i);
		// Convert: m to cm (×100), swap Y and Z for Unreal coordinate system
		FVector Vertex(P.x() * 100.0f, P.z() * 100.0f, P.y() * 100.0f);
		OutVertices.Add(Vertex);
	}

	// Build hole map (profile_id → hole indices)
	const auto* Holes = ShellRef->holes();
	TMap<int32, TArray<TArray<int32>>> ProfileHolesMap;

	if (Holes)
	{
		for (flatbuffers::uoffset_t j = 0; j < Holes->size(); j++)
		{
			const auto* Hole = Holes->Get(j);
			const auto* HoleIndices = Hole->indices();
			const auto Profile_id = Hole->profile_id();

			TArray<int32> HoleIdx;
			for (flatbuffers::uoffset_t k = 0; k < HoleIndices->size(); k++)
			{
				HoleIdx.Add(HoleIndices->Get(k));
			}

			if (ProfileHolesMap.Contains(Profile_id))
			{
				ProfileHolesMap[Profile_id].Add(HoleIdx);
			}
			else
			{
				TArray<TArray<int32>> HolesForProfile;
				HolesForProfile.Add(HoleIdx);
				ProfileHolesMap.Add(Profile_id, HolesForProfile);
			}
		}
	}

	// Process profiles
	const auto* Profiles = ShellRef->profiles();
	if (!Profiles)
	{
		return;
	}

	for (flatbuffers::uoffset_t i = 0; i < Profiles->size(); i++)
	{
		const ShellProfile* Profile = Profiles->Get(i);
		const auto* Indices = Profile->indices();

		if (!Indices || Indices->size() < 3)
		{
			continue;
		}

		// Check if this profile has holes
		bool bHasHoles = ProfileHolesMap.Contains(i);

		if (bHasHoles)
		{
			// Extract contour indices
			TArray<int32> ContourIndices;
			for (flatbuffers::uoffset_t j = 0; j < Indices->size(); j++)
			{
				ContourIndices.Add(Indices->Get(j));
			}

			// Triangulate polygon with holes This creates NEW vertices and indices (not reusing OutVertices!).
			TArray<FVector> TriangulatedVertices;
			TArray<int32> TriangulatedIndices;

			if (FragmentShellAuthoring::TriangulatePolygonWithHoles(
				OutVertices,           // All vertices (for lookup)
				ContourIndices,        // Contour as indices
				ProfileHolesMap[i],    // Holes
				TriangulatedVertices,  // Output: new vertices
				TriangulatedIndices    // Output: new indices
			))
			{
				// Add triangulated vertices to output
				int32 VertexOffset = OutVertices.Num();
				OutVertices.Append(TriangulatedVertices);

				// Adjust indices and add to output
				for (int32 Idx : TriangulatedIndices)
				{
					OutTriangles.Add(VertexOffset + Idx);
				}
			}
			else
			{
				UE_LOG(LogFragments, Verbose, TEXT("Triangulation failed for profile %d"), i);
			}
		}
		else
		{
			// Simple fan triangulation for convex polygons (no holes)
			if (Indices->size() >= 3)
			{
				int32 V0 = Indices->Get(0);

				for (flatbuffers::uoffset_t j = 1; j < Indices->size() - 1; j++)
				{
					int32 V1 = Indices->Get(j);
					int32 V2 = Indices->Get(j + 1);

					OutTriangles.Add(V0);
					OutTriangles.Add(V1);
					OutTriangles.Add(V2);
				}
			}
		}
	}

	// Calculate normals (per-face)
	OutNormals.SetNum(OutVertices.Num());
	for (int32 i = 0; i < OutTriangles.Num(); i += 3)
	{
		if (i + 2 < OutTriangles.Num())
		{
			int32 Idx0 = OutTriangles[i + 0];
			int32 Idx1 = OutTriangles[i + 1];
			int32 Idx2 = OutTriangles[i + 2];

			if (Idx0 < OutVertices.Num() && Idx1 < OutVertices.Num() && Idx2 < OutVertices.Num())
			{
				FVector V0 = OutVertices[Idx0];
				FVector V1 = OutVertices[Idx1];
				FVector V2 = OutVertices[Idx2];

				FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();

				OutNormals[Idx0] = Normal;
				OutNormals[Idx1] = Normal;
				OutNormals[Idx2] = Normal;
			}
		}
	}

	// Simple planar UV projection
	OutUVs.SetNum(OutVertices.Num());
	for (int32 i = 0; i < OutVertices.Num(); i++)
	{
		OutUVs[i] = FVector2D(OutVertices[i].X * 0.01f, OutVertices[i].Y * 0.01f);
	}
}

AFragment* UFragmentsImporter::SpawnFragmentModel(FFragmentItem InFragmentItem, AActor* InParent, const Meshes* MeshesRef,
	bool bSaveMeshes, UFragmentModelWrapper* InWrapperRef,
	AFragment* BucketRoot, int64 ParentFloorKey)
{
	UE_LOG(LogFragments, Verbose, TEXT("SpawnFragmentModel Start - LocalId=%lld, Category=%s, Children=%d, Samples=%d, BucketRoot=%p"),
		InFragmentItem.LocalId, *InFragmentItem.Category, InFragmentItem.FragmentChildren.Num(),
		InFragmentItem.Samples.Num(), BucketRoot);

	if (!InParent)
	{
		UE_LOG(LogFragments, Error, TEXT("SpawnFragmentModel: InParent is NULL! Early return. "));
		if (OnProcessLoadedFragmentComplete.IsBound())
		{
			FOnProcessLoadedFragmentComplete Local = OnProcessLoadedFragmentComplete;
			OnProcessLoadedFragmentComplete.Unbind();
			Local.Execute(false, TEXT("[EDITOR] SpawnFragmentModel: InParent is NULL! Early return. "), TEXT(""));
		}
		return nullptr;
	}

	// Create AFragment

	AFragment* FragmentModel = OwnerRef->GetWorld()->SpawnActor<AFragment>(
		AFragment::StaticClass(), InFragmentItem.GlobalTransform);

	if (!FragmentModel)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to spawn FragmentModel actor!"));
		if (OnProcessLoadedFragmentComplete.IsBound())
		{
			FOnProcessLoadedFragmentComplete Local = OnProcessLoadedFragmentComplete;
			OnProcessLoadedFragmentComplete.Unbind();
			Local.Execute(false, TEXT("[EDITOR] SpawnFragmentModel: Failed to spawn FragmentModel actor!"), TEXT(""));
		}
		return nullptr;
	}

	// Hide per-element children from the outliner; only the model root stays listed.
	FragmentModel->SetShouldShowInOutliner(Cast<AFragment>(InParent) == nullptr);

	UE_LOG(LogFragments, Verbose, TEXT("Spawned FragmentModel: %s at %s"), *FragmentModel->GetName(),*InFragmentItem.GlobalTransform.ToString());
	

	// Root Component.
	EnsureRootComponent(*FragmentModel);

	// Set Transform and Info
	FTransform RootTransform = UFragmentsUtils::MakeTransform(MeshesRef->coordinates());
	FragmentModel->SetActorTransform(InFragmentItem.GlobalTransform);
	FragmentModel->SetData(InFragmentItem);

	// Verifies for Fragments that represents Categories such as IFCBUILDING, IFCSITE, IFCPROJECT that might affect the rotation of the IFC tree elements.
	const FString Cat = FragmentModel->GetCategory();
	if (IsPlacementCarrier(Cat))
	{
		FTransform T = FragmentModel->GetActorTransform();  // current (world) transform

		// Keep translation, cancel rotation
		T.SetRotation(FQuat::Identity);
		T.SetScale3D(FVector(1.0f));

		FragmentModel->SetActorTransform(T);
	}

	FragmentModel->AttachToActor(InParent, FAttachmentTransformRules::KeepWorldTransform);


#if WITH_EDITOR
	if (!FragmentModel->GetCategory().IsEmpty())
		FragmentModel->SetActorLabel(FragmentModel->GetCategory());
#endif

	// Auto-initialize HISM bucket root if needed
	if (!BucketRoot)
	{
		BucketRoot = FragmentModel;
		BucketRoot->SetAsBucketRoot(true);
		BucketRoot->SetHISMEnabled(true);
		if (InWrapperRef)
		{
			BucketRoot->SetFragmentPath(InWrapperRef->FragmentPath);
			BucketRoot->SetImportOrigin(InWrapperRef->OriginMode, InWrapperRef->OriginBase);
		}
	}

	// Collects the Current Floor key for later mapping in HISM
	int64 CurrentFloorKey = ParentFloorKey;
	if (FragmentModel->GetCategory().Equals(TEXT("IFCBUILDINGSTOREY"), ESearchCase::IgnoreCase))
	{
		CurrentFloorKey = FragmentModel->GetLocalId();
	}

	// Create Meshes If Sample Exists
	const TArray<FFragmentSample>& Samples = FragmentModel->GetSamples();
	UE_LOG(LogFragments, Verbose, TEXT("Processing %d samples for FragmentModel"), Samples.Num());

	if (Samples.Num() > 0)
	{
		for (int32 i = 0; i < Samples.Num(); i++)
		{
			const FFragmentSample& Sample = Samples[i];

			const Material* material = MeshesRef->materials()->Get(Sample.MaterialIndex);
			const Representation* representation = MeshesRef->representations()->Get(Sample.RepresentationIndex);
			const Transform* local_transform = MeshesRef->local_transforms()->Get(Sample.LocalTransformIndex);

			const int64 repId = representation->id();
			const uint8 R = material->r(), G = material->g(), B = material->b(), A = material->a();

			FString MeshName = FString::Printf(TEXT("rep_%lld"), repId);
			FString PackagePath = TEXT("/Game/Buildings") / FragmentModel->GetModelGuid() / MeshName;
			const FString SamplePath = PackagePath + TEXT(".") + MeshName;

			FString UniquePackageName = FPackageName::ObjectPathToPackageName(PackagePath);
			FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());

			FTransform LocalTransform = UFragmentsUtils::MakeTransform(local_transform);

			UStaticMesh* Mesh = nullptr;
			if (MeshCache.Contains(SamplePath))
			{
				Mesh = MeshCache[SamplePath];
			}
			else
			{
				// Only probe for a persistent asset when saving meshes; runtime path creates RF_Transient meshes that never live at /Game/Buildings/...
				if (bSaveMeshes)
				{
					Mesh = LoadObject<UStaticMesh>(nullptr, *SamplePath);
				}

				// Runtime mesh creation — works in both editor and packaged builds.
				if (!Mesh)
				{
#if WITH_EDITOR
					UPackage* MeshPackage = CreatePackage(*PackagePath);
#else
					UPackage* MeshPackage = GetTransientPackage();
#endif
					if (representation->representation_class() == RepresentationClass::RepresentationClass_SHELL)
					{
						const auto* shell = MeshesRef->shells()->Get(representation->id());
						FFragmentMeshData MeshData = BuildShellMeshDataFromFlatBuffers(shell, material);
						if (MeshData.bIsValid)
						{
							uint8 MR = material ? material->r() : 255;
							uint8 MG = material ? material->g() : 255;
							uint8 MB = material ? material->b() : 255;
							uint8 MA = material ? material->a() : 255;
							bool bMIsGlass = (MA < 255);  // Glass determined by transparency
							UMaterialInterface* SlotMaterial = GetPooledMaterial(MR, MG, MB, MA, bMIsGlass);
							Mesh = CommitToMeshDescription(MeshData, GetTransientPackage(), SlotMaterial, InFragmentItem.ModelGuid);
						}
					}
					else if (representation->representation_class() == RepresentationClass_CIRCLE_EXTRUSION)
					{
						const auto* circleExtrusion = MeshesRef->circle_extrusions()->Get(representation->id());
						Mesh = CreateStaticMeshFromCircleExtrusion(circleExtrusion, material, *MeshName, MeshPackage, FragmentModel->GetModelGuid());
					}

#if WITH_EDITOR
					if (Mesh && !FPaths::FileExists(PackageFileName) && bSaveMeshes)
					{
						MeshPackage->FullyLoad();
						Mesh->Rename(*MeshName, MeshPackage);
						Mesh->SetFlags(RF_Public | RF_Standalone);
						MeshPackage->MarkPackageDirty();
						FAssetRegistryModule::AssetCreated(Mesh);

						FSavePackageArgs SaveArgs;
						SaveArgs.SaveFlags = RF_Public | RF_Standalone;
						PackagesToSave.Add(MeshPackage);
						UPackage::SavePackage(MeshPackage, Mesh, *PackageFileName, SaveArgs);
					}
#endif
				}
				MeshCache.Add(SamplePath, Mesh);
			}

			if (!Mesh) continue;

			// HISM INSTANCING PATH (bucket system).
			UMaterialInterface* MatIF = nullptr;
#if WITH_EDITOR
			// Editor-built static meshes already carry a slot 0 material via AddMaterialToMesh
			MatIF = Mesh->GetMaterial(0);
#else
			// Runtime: reuse/create a MID per material index on the wrapper
			if (UMaterialInstanceDynamic** FoundMid = InWrapperRef->GetMaterialsMap().Find(Sample.MaterialIndex))
			{
				MatIF = *FoundMid;
			}
			else
			{
				UMaterialInterface* BaseMat = (A < 255) ? BaseGlassMaterial.Get() : BaseMaterial.Get();
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, BucketRoot);
				MID->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor(R / 255.f, G / 255.f, B / 255.f, A / 255.f));
				if (A < 255) MID->SetScalarParameterValue(TEXT("Opacity"), A / 255.f);
				InWrapperRef->GetMaterialsMap().Add(Sample.MaterialIndex, MID);
				MatIF = MID;
			}
#endif
			const FTransform WorldXf = LocalTransform * FragmentModel->GetActorTransform();
			const bool bItemIsShell = IsShellCategory(FragmentModel->GetCategory());
			BucketRoot->AddHISMInstance(Mesh, MatIF, WorldXf, InFragmentItem.LocalId, CurrentFloorKey, bItemIsShell);

			// Tag the HISMC with its BIM source RGBA so the editor panel and override apply pass can identify it.
			if (UHierarchicalInstancedStaticMeshComponent* HISM = BucketRoot->GetOrCreateBucket(Mesh, MatIF, bItemIsShell))
			{
				if (!BucketRoot->BIMSourceColors.Contains(HISM))
				{
					BucketRoot->BIMSourceColors.Add(HISM, FBIMSourceColor{ R, G, B, A, A < 255 });
				}
			}
		}
	}

	if (ModelFragmentsMap.Contains(InFragmentItem.ModelGuid))
	{
		ModelFragmentsMap[InFragmentItem.ModelGuid].Fragments.Add(InFragmentItem.LocalId, FragmentModel);
	}

	// Recursively spawn child fragments
	for (FFragmentItem* Child : InFragmentItem.FragmentChildren)
	{
		SpawnFragmentModel(*Child, FragmentModel, MeshesRef, bSaveMeshes, InWrapperRef, BucketRoot, CurrentFloorKey);
	}

	return FragmentModel;
}

USceneComponent* UFragmentsImporter::EnsureRootComponent(AFragment& Root)
{
	if (USceneComponent* Existing = Root.GetRootComponent())
	{
		return Existing;
	}

	// RF_Transactional, or moving this actor in the viewport is not undoable.
	USceneComponent* RootScene = NewObject<USceneComponent>(&Root, NAME_None, RF_Transactional);
	RootScene->SetMobility(EComponentMobility::Movable);
	RootScene->RegisterComponent();
	Root.SetRootComponent(RootScene);
	return RootScene;
}

void UFragmentsImporter::EnsureRegistrationStreamingTicking(UWorld* InWorld)
{
	if (!InWorld) return;

	// A world change means the previous world's timer manager is either gone with it or about to be; clearing keeps RegistrationUpdateTimerHandle describing exactly one live timer, since one handle cannot track two.
	if (UWorld* Prev = StreamingWorld.Get(); Prev && Prev != InWorld)
	{
		Prev->GetTimerManager().ClearTimer(RegistrationUpdateTimerHandle);
	}
	StreamingWorld = InWorld;

	FTimerManager& TimerManager = InWorld->GetTimerManager();
	if (!TimerManager.IsTimerActive(RegistrationUpdateTimerHandle))
	{
		TimerManager.SetTimer(
			RegistrationUpdateTimerHandle, this,
			&UFragmentsImporter::TickRegistrationStreaming,
			0.016f, true);
	}
}

UWorld* UFragmentsImporter::ResolveModelWorld() const
{
	// OwnerRef first: the runtime and import paths must resolve exactly as they always have.
	if (OwnerRef.IsValid())
	{
		if (UWorld* OwnerWorld = OwnerRef->GetWorld()) { return OwnerWorld; }
	}
	// Then the world the streaming tick was started on.
	if (UWorld* Streaming = StreamingWorld.Get()) { return Streaming; }
	// Then this importer's own world, which answers only under PIE.
	return const_cast<UFragmentsImporter*>(this)->GetWorld();
}

#if WITH_AUTOMATION_TESTS
bool UFragmentsImporter::IsRegistrationStreamingTickingForTesting() const
{
	UWorld* World = ResolveModelWorld();
	return World && World->GetTimerManager().IsTimerActive(RegistrationUpdateTimerHandle);
}
#endif

FFragmentCellMergeManager* UFragmentsImporter::GetCellMergeManager()
{
	if (!CellMergeManager)
	{
		// The CDO owns no session state and must never launch worker tasks.
		if (HasAnyFlags(RF_ClassDefaultObject)) { return nullptr; }

		CellMergeManager = MakeUnique<FFragmentCellMergeManager>();

		// Verbatim the sink UFragmentsImporterSubsystem::Initialize used to install, with the weak importer pointer now pointing at the manager's own owner rather than at a sibling.
		TWeakObjectPtr<UFragmentsImporter> WeakImporter(this);
		CellMergeManager->SetFinalizeSink(
			[WeakImporter](FPendingMeshFinalize&& Payload)
			{
				check(IsInGameThread());
				UFragmentsImporter* ImporterPtr = WeakImporter.Get();
				if (!ImporterPtr)
				{
					// Importer torn down before this deferred finalize ran; drop it, unrooting the mesh to avoid a leak (it was rooted at EnqueueMerge).
					if (UStaticMesh* M = Payload.WeakMesh.Get()) { M->RemoveFromRoot(); }
					return;
				}
				ImporterPtr->DispatchMeshFinalize(MoveTemp(Payload));
			});
	}

	// Refreshed on every access rather than set once at creation: the manager outlives a map change, and EnqueueMerge outers each merged mesh to this world.
	CellMergeManager->SetOuterWorld(ResolveModelWorld());

	return CellMergeManager.Get();
}

void UFragmentsImporter::ShutdownCellMerges()
{
	if (CellMergeManager)
	{
		CellMergeManager->WaitForAllMerges();
		CellMergeManager.Reset();
	}
}

void UFragmentsImporter::TickRegistrationStreaming()
{
	// Per-frame tick for the registration streaming path.
	NewMeshCreationsThisFrame = 0;

	// Sync coordinator settings from UPROPERTY values
	FrameBudgetCoordinator.TotalFrameBudgetMs = TotalFrameBudgetMs;
	FrameBudgetCoordinator.bEnableAdaptiveBudget = bEnableAdaptiveBudget;

	FrameBudgetCoordinator.BeginFrame();

	// Drain finalizes first (highest priority), on this persistent tick rather than ProcessHISMSpawnChunk's self-clearing timer, so a proxy enqueued after HISM spawn completes doesn't get stranded.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Frag_DrainMeshFinalizes);
		DrainMeshFinalizes();
	}

	// Update registration streaming
	if (RegistrationManager)
	{
		UWorld* World = ResolveModelWorld();
		if (World)
		{
			TArray<FFragmentViewInfo, TInlineAllocator<4>> Views;
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				// Runtime path: always runs regardless of r.Fragments.EditorStreaming.
				RegistrationManager->ExtractViewInfo(PC, Views.AddDefaulted_GetRef());
			}
			else if (CVarEditorStreaming.GetValueOnGameThread() != 0)
			{
				TArray<FFragmentViewInfo> Gathered;
				UE::Fragments::ViewProvider::Gather(World, Gathered);
				Views.Append(MoveTemp(Gathered));
			}

			// Empty means no camera had an opinion -- skip, never cull.
			if (Views.Num() > 0)
			{
				const float CookSliceMs = FrameBudgetCoordinator.AllocateRemainingMs();
				const double CookStart = FPlatformTime::Seconds();
				RegistrationManager->UpdateRegistration(Views, World, CookSliceMs);
				FrameBudgetCoordinator.RecordWork(
					static_cast<float>((FPlatformTime::Seconds() - CookStart) * 1000.0));
			}
		}
	}

	FrameBudgetCoordinator.EndFrame();
}

// HISM CHUNKED SPAWNING.

void UFragmentsImporter::BuildHISMSpawnQueue(const FFragmentItem& InItem, const FTransform& ParentWorldTransform, int64 ParentFloorKey)
{
	// Track floor changes for bucket keying
	int64 CurrentFloorKey = ParentFloorKey;
	if (InItem.Category.Equals(TEXT("IFCBUILDINGSTOREY"), ESearchCase::IgnoreCase))
	{
		CurrentFloorKey = InItem.LocalId;
	}

	// Compute this item's world transform (mirrors SpawnFragmentModel actor transform logic)
	FTransform ItemWorldTransform = InItem.GlobalTransform;
	if (IsPlacementCarrier(InItem.Category))
	{
		ItemWorldTransform.SetRotation(FQuat::Identity);
		ItemWorldTransform.SetScale3D(FVector(1.0f));
	}

	// Queue each sample as a pending item
	for (int32 i = 0; i < InItem.Samples.Num(); i++)
	{
		const FFragmentSample& Sample = InItem.Samples[i];

		// Pre-extract material data from FlatBuffers
		const Material* Mat = CurrentMeshesRef->materials()->Get(Sample.MaterialIndex);
		const Transform* LocalTf = CurrentMeshesRef->local_transforms()->Get(Sample.LocalTransformIndex);

		FTransform SampleLocalTransform = UFragmentsUtils::MakeTransform(LocalTf);
		FTransform SampleWorldTransform = SampleLocalTransform * ItemWorldTransform;

		FPendingHISMItem PendingItem;
		PendingItem.RepresentationIndex = Sample.RepresentationIndex;
		PendingItem.MaterialIndex = Sample.MaterialIndex;
		PendingItem.LocalTransformIndex = Sample.LocalTransformIndex;
		PendingItem.LocalId = InItem.LocalId;
		PendingItem.FloorKey = CurrentFloorKey;
		PendingItem.WorldTransform = SampleWorldTransform;
		PendingItem.Category = InItem.Category;
		PendingItem.ModelGuid = InItem.ModelGuid;
		PendingItem.R = Mat->r();
		PendingItem.G = Mat->g();
		PendingItem.B = Mat->b();
		PendingItem.A = Mat->a();
		PendingItem.bIsGlass = (Mat->a() < 255);

		// One increment per sample, regardless of which branch below it takes, so the shell/interior split cannot perturb the traversal-order numbering.
		PendingItem.QueueIndex = NextHISMQueueIndex++;

		if (bEnableShellFirstStreaming && !IsShellCategory(PendingItem.Category))
		{
			DeferredInteriorQueues.FindOrAdd(PendingItem.ModelGuid).Add(MoveTemp(PendingItem));
		}
		else
		{
			PendingHISMQueue.Add(MoveTemp(PendingItem));
		}
	}

	// Recurse into children
	for (FFragmentItem* Child : InItem.FragmentChildren)
	{
		if (Child)
		{
			BuildHISMSpawnQueue(*Child, ItemWorldTransform, CurrentFloorKey);
		}
	}
}

void UFragmentsImporter::BeginFinalizeHISMBuckets()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_BeginFinalizeBuckets);

	// Rebuilds PendingBucketInstances via orphan remap; must run before drain.
	FinalizePartitionCells();
	LogCategoryFrequencyTable();

	// Snapshot keys now; PendingBucketInstances stays populated through the chunked drain so EnqueueEagerCellMerges -> SnapshotCellMergeSources can still read per-cell items in EndFinalizeHISMBuckets.
	FinalizeBucketKeys.Reset(PendingBucketInstances.Num());
	for (const auto& Pair : PendingBucketInstances)
	{
		FinalizeBucketKeys.Add(Pair.Key);
	}
	FinalizeBucketCursor = 0;
	bIsFinalizingBuckets = true;
}

bool UFragmentsImporter::ProcessFinalizeBucketsChunk()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_FinalizeBucketsChunk);

	const int32 Budget = FMath::Max(1, BucketsPerChunk);
	const int32 End = FMath::Min(FinalizeBucketCursor + Budget, FinalizeBucketKeys.Num());

	for (int32 i = FinalizeBucketCursor; i < End; ++i)
	{
		const FFragBucketKey& Key = FinalizeBucketKeys[i];
		if (const TArray<FPendingHISMItem>* Items = PendingBucketInstances.Find(Key))
		{
			if (Items->Num() > 0)
			{
				FinalizeSingleHISMBucket(Key, *Items);
			}
		}
	}
	FinalizeBucketCursor = End;

	return FinalizeBucketCursor >= FinalizeBucketKeys.Num();
}

void UFragmentsImporter::EndFinalizeHISMBuckets()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_EndFinalizeBuckets);

	// Cleanup only.
	PendingBucketInstances.Empty();
	FinalizeBucketKeys.Reset();
	FinalizeBucketCursor = 0;
	bIsFinalizingBuckets = false;
	CellMergeEnqueueQueue.Reset();
	CellMergeEnqueueCursor = 0;
	OverrideApplyQueue.Reset();
	OverrideApplyCursor = 0;
	FinalizeTailPhase = EFinalizeTailPhase::None;
}

void UFragmentsImporter::ApplyMaterialOverrides(AFragment* BucketRoot, UFragmentModelWrapper* Wrapper)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ApplyMaterialOverrides);

	if (!IsValid(BucketRoot) || !IsValid(Wrapper)) return;
	if (Wrapper->MaterialOverrides.Num() == 0) return;

	int32 Applied = 0;
	for (const TPair<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor>& Pair : BucketRoot->GetBIMSourceColors())
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Key;
		if (!IsValid(HISM)) continue;

		const FBIMSourceColor& Src = Pair.Value;
		UMaterialInterface* Mat = Wrapper->ResolveOverrideMaterial(Src.R, Src.G, Src.B, Src.A, Src.bIsGlass);
		if (!Mat) continue;

		HISM->SetMaterial(0, Mat);
		HISM->MarkRenderStateDirty();
		++Applied;
	}

	UE_LOG(LogFragments, Verbose,
		TEXT("[fragmentOverrides] applied %d overrides across %d HISMCs"),
		Applied, BucketRoot->GetBIMSourceColors().Num());
}

void UFragmentsImporter::BeginApplyOverrides()
{
	OverrideApplyQueue.Reset();
	OverrideApplyCursor = 0;

	if (IsValid(CurrentBucketRoot) && IsValid(CurrentHISMWrapper) &&
	    CurrentHISMWrapper->MaterialOverrides.Num() > 0)
	{
		for (const TPair<UHierarchicalInstancedStaticMeshComponent*, FBIMSourceColor>& Pair :
		     CurrentBucketRoot->GetBIMSourceColors())
		{
			OverrideApplyQueue.Emplace(Pair.Key, Pair.Value);
		}
	}

	FinalizeTailPhase = EFinalizeTailPhase::ApplyOverrides;
}

bool UFragmentsImporter::ProcessApplyOverridesChunk()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ApplyOverridesChunk);

	UFragmentModelWrapper* Wrapper = CurrentHISMWrapper;
	if (!IsValid(Wrapper))
	{
		OverrideApplyCursor = OverrideApplyQueue.Num();
		return true;
	}

	return FragmentBudgetDrain::DrainBudgeted(
		OverrideApplyCursor, OverrideApplyQueue.Num(),
		FrameBudgetCoordinator, GFrameCounter,
		[&](int32 i)
		{
			const double T0 = FPlatformTime::Seconds();
			UHierarchicalInstancedStaticMeshComponent* HISM = OverrideApplyQueue[i].Key;
			const FBIMSourceColor& Src = OverrideApplyQueue[i].Value;
			if (IsValid(HISM))
			{
				UMaterialInterface* Mat = Wrapper->ResolveOverrideMaterial(Src.R, Src.G, Src.B, Src.A, Src.bIsGlass);
				if (Mat)
				{
					HISM->SetMaterial(0, Mat);
					HISM->MarkRenderStateDirty();
				}
			}
			FrameBudgetCoordinator.RecordWork(
				static_cast<float>((FPlatformTime::Seconds() - T0) * 1000.0), GFrameCounter);
		});
}

void UFragmentsImporter::FinalizeSingleHISMBucket(const FFragBucketKey& Key, const TArray<FPendingHISMItem>& Items)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_FinalizeBucket);
	if (!CurrentBucketRoot || Items.Num() == 0) return;

	// Check if a bucket HISMC already exists for this key.
	UHierarchicalInstancedStaticMeshComponent** ExistingPtr = CurrentBucketRoot->Buckets.Find(Key);
	UHierarchicalInstancedStaticMeshComponent* HISM = ExistingPtr ? *ExistingPtr : nullptr;
	const bool bAppending = (HISM != nullptr);

	if (!bAppending)
	{
		// CREATE NEW HISMC
		HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(CurrentBucketRoot);
		HISM->SetStaticMesh(Key.Mesh);
		HISM->SetMaterial(0, Key.Material);
		HISM->SetMobility(EComponentMobility::Stationary); // Stationary skips velocity pass (~7ms/frame) while allowing late registration
		HISM->NumCustomDataFloats = 2; // [0]=highlight, [1]=dimmer 0..1 (0=visible, 1=hidden, intermediate=X-ray dim)
		HISM->bAffectDistanceFieldLighting = false;
		HISM->bAffectDynamicIndirectLighting = false;

		// Shadow and Occlusion Classification == Use the first item's category + alpha as representative for the bucket.
		const FPendingHISMItem& FirstItem = Items[0];
		const EOcclusionRole Role = UFragmentOcclusionClassifier::ClassifyFragment(
			FirstItem.Category, FirstItem.A);

		// Determine if this bucket is structural (for shadow gating and tagging).
		static const TSet<FString> StructuralPrefixes = {
			TEXT("IfcWall"), TEXT("IfcSlab"), TEXT("IfcRoof"),
			TEXT("IfcBeam"), TEXT("IfcColumn"), TEXT("IfcStair")
		};
		bool bIsStructural = false;
		for (const FString& Prefix : StructuralPrefixes)
		{
			if (FirstItem.Category.Contains(Prefix, ESearchCase::IgnoreCase))
			{
				bIsStructural = true;
				break;
			}
		}

		// Tag structural HISMCs so the RegistrationManager can detect them at RegisterModel time.
		if (bIsStructural)
		{
			HISM->ComponentTags.AddUnique(FName(TEXT("Structural")));
		}

		switch (Role)
		{
		case EOcclusionRole::Occluder:
			// Shadows owned by merged LOD1 (bCastHiddenShadow); HISMCs never cast.
			HISM->bUseAsOccluder = true;
			HISM->SetCastShadow(false);
			HISM->bCastDynamicShadow = false;
			HISM->bCastStaticShadow = false;
			break;

		case EOcclusionRole::Occludee:
			// No shadow for any non-structural HISMC.
			HISM->bUseAsOccluder = false;
			HISM->SetCastShadow(false);
			HISM->bCastDynamicShadow = false;
			HISM->bCastStaticShadow = false;
			break;

		case EOcclusionRole::NonOccluder:
			// Glass + transparent materials -> no shadow, no occlusion.
			HISM->bUseAsOccluder = false;
			HISM->SetCastShadow(false);
			HISM->bCastDynamicShadow = false;
			HISM->bCastStaticShadow = false;
			break;
		}

		// IFCPLATE glass: no shadows, and tag for the registration manager's distance-gated occluder flip.
		if (FragmentClassification::IsOccludableGlass(FirstItem))
		{
			HISM->SetCastShadow(false);
			HISM->bCastDynamicShadow = false;
			HISM->bCastStaticShadow = false;
			HISM->ComponentTags.AddUnique(FName(TEXT("GlassCurtainWall")));
		}

		// Each HISMC is scoped to one cell; ApplyCellTransition drives visibility.
		UE_LOG(LogFragments, Verbose, TEXT("[HISM Classification] Category=%s Role=%d Instances=%d CastShadow=%d Cell=(F%d,X%d,Y%d) bShell=%d PromoteCm=%.0f"),
			*FirstItem.Category, (int32)Role, Items.Num(), HISM->CastShadow,
			Key.Cell.Floor, Key.Cell.GridX, Key.Cell.GridY, Key.bShell,
			(Key.bShell != 0)
				? CVarShellLOD0PromoteCm.GetValueOnGameThread()
				: CVarInteriorLOD0PromoteCm.GetValueOnGameThread());

		// Configure collision for line traces (e.g. click-to-select).
		HISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);

		// Start hidden so the cell state machine drives initial visibility, avoiding a one-frame full-LOD0 flash before far cells demote to merged.
		HISM->SetVisibility(false);

#if WITH_EDITOR
		HISM->bHasPerInstanceHitProxies = true;
#endif

		// Attach but do NOT register yet — we register after adding instances below.
		HISM->SetupAttachment(CurrentBucketRoot->GetRootComponent());
		HISM->SetRelativeTransform(FTransform::Identity);

		CurrentBucketRoot->BIMSourceColors.Add(HISM,
			FBIMSourceColor{ FirstItem.R, FirstItem.G, FirstItem.B, FirstItem.A, FirstItem.bIsGlass });
	}

	// ADD INSTANCES (shared path for both new and existing HISMCs)

	// Collect transforms relative to bucket root (mirrors AddHISMInstance)
	const FTransform BucketWorld = CurrentBucketRoot->GetActorTransform();
	TArray<FTransform> Transforms;
	Transforms.Reserve(Items.Num());
	for (const FPendingHISMItem& Item : Items)
	{
		Transforms.Add(Item.WorldTransform.GetRelativeTransform(BucketWorld));
	}

	// Batch add instances
	TArray<int32> InstanceIndices;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_BatchAddInstances);
		InstanceIndices = HISM->AddInstances(Transforms, /*bShouldReturnIndices=*/true, /*bWorldSpace=*/false);
	}

	// UPDATE LUT

	if (bAppending)
	{
		// Grow existing LUT to accommodate new instance indices
		FFragLocalids* LUTPtr = CurrentBucketRoot->InstanceLocalIds.Find(HISM);
		if (LUTPtr)
		{
			// Instance indices from AddInstances on an existing HISMC continue from the previous count, so the highest new index may exceed the LUT size.
			const int32 RequiredSize = InstanceIndices.Last() + 1;
			if (RequiredSize > LUTPtr->LocalIds.Num())
			{
				LUTPtr->LocalIds.SetNum(RequiredSize);
				LUTPtr->FloorKeys.SetNum(RequiredSize);
			}

			for (int32 i = 0; i < Items.Num(); i++)
			{
				const FPendingHISMItem& Item = Items[i];
				const int32 Idx = InstanceIndices[i];

				HISM->SetCustomDataValue(Idx, 0, 0.f, false); // highlight slot
				HISM->SetCustomDataValue(Idx, 1, 0.f, false); // dimmer slot (0=visible, 1=hidden, intermediate=X-ray dim)

				LUTPtr->LocalIds[Idx] = Item.LocalId;
				LUTPtr->FloorKeys[Idx] = Item.FloorKey;
				CurrentBucketRoot->LocalIdToInstance.FindOrAdd(Item.LocalId).Items.Add(
					FFragInstanceRef{HISM, Idx});
			}
		}
	}
	else
	{
		// New LUT for a fresh HISMC
		FFragLocalids LUT;
		LUT.LocalIds.SetNum(Items.Num());
		LUT.FloorKeys.SetNum(Items.Num());

		for (int32 i = 0; i < Items.Num(); i++)
		{
			const FPendingHISMItem& Item = Items[i];
			const int32 Idx = InstanceIndices[i];

			// Custom data: default to no highlight, full visibility
			HISM->SetCustomDataValue(Idx, 0, 0.f, false); // highlight slot
			HISM->SetCustomDataValue(Idx, 1, 0.f, false); // dimmer slot (0=visible, 1=hidden, intermediate=X-ray dim)

			// Populate lookup maps for interaction API
			LUT.LocalIds[Idx] = Item.LocalId;
			LUT.FloorKeys[Idx] = Item.FloorKey;
			CurrentBucketRoot->LocalIdToInstance.FindOrAdd(Item.LocalId).Items.Add(
				FFragInstanceRef{HISM, Idx});
		}

		CurrentBucketRoot->InstanceLocalIds.Add(HISM, MoveTemp(LUT));
	}

	// Single render state update for entire HISMC
	HISM->MarkRenderStateDirty();

	if (!bAppending)
	{
		// Register immediately so HISMC is visible this frame (progressive loading).
		if (!HISM->IsRegistered())
		{
			HISM->RegisterComponent();
		}

		// Store in Buckets map (only for new HISMCs — existing ones are already there)
		CurrentBucketRoot->Buckets.Add(Key, HISM);
	}

	UE_LOG(LogFragments, Verbose, TEXT("[HISM Batch] %s bucket with %d instances (Mesh=%s, total=%d)"),
		bAppending ? TEXT("Appended to") : TEXT("Finalized new"),
		Items.Num(), *Key.Mesh->GetName(), HISM->GetInstanceCount());
}

bool UFragmentsImporter::RegisterShellBuildWaiter(const FString& SamplePath, FPendingHISMItem&& Item)
{
	check(IsInGameThread());

	if (FPendingShellBuild* Existing = InFlightShellBuilds.Find(SamplePath))
	{
		Existing->Waiters.Add(MoveTemp(Item));
		return false;
	}

	FPendingShellBuild& Entry = InFlightShellBuilds.Add(SamplePath);
	Entry.Waiters.Add(MoveTemp(Item));
	return true;
}

void UFragmentsImporter::CompleteShellBuild(const FString& SamplePath, FShellBuildResult&& Result)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_CompleteShellBuild);
	check(IsInGameThread());

	// Guard first: a completion with no matching registry entry (spurious or double-fired) must be a clean no-op, not a cache stomp.
	FPendingShellBuild Entry;
	if (!InFlightShellBuilds.RemoveAndCopyValue(SamplePath, Entry))
	{
		return;
	}

	// Cache even a null mesh: prevents re-launching a build that already failed.
	MeshCache.Add(SamplePath, Result.Mesh);

	if (Result.Mesh && Result.ProxyData.bIsValid)
	{
		ProxyMeshDataCache.Add(
			SamplePath,
			MakeShared<const FFragmentMeshData, ESPMode::ThreadSafe>(MoveTemp(Result.ProxyData)));
	}

	if (!Result.Mesh)
	{
		// MeshCache above already recorded the failure, so waiters are dropped and no rebuild will be attempted for this SamplePath.
		return;
	}

	for (FPendingHISMItem& Waiter : Entry.Waiters)
	{
		UMaterialInterface* MatIF = GetPooledMaterial(Waiter.R, Waiter.G, Waiter.B, Waiter.A, Waiter.bIsGlass);
		if (!MatIF)
		{
			UE_LOG(LogFragments, Verbose,
				TEXT("[HISM async] Dropping waiter LocalId=%lld ModelGuid=%s for %s - material pool "
				     "returned null during async completion drain"),
				Waiter.LocalId, *Waiter.ModelGuid, *SamplePath);
			continue;
		}
		ProcessResolvedHISMItem(MoveTemp(Waiter), Result.Mesh, MatIF);
	}
}

void UFragmentsImporter::ProcessResolvedHISMItem(FPendingHISMItem&& Item, UStaticMesh* Mesh, UMaterialInterface* MatIF)
{
	check(IsInGameThread());

	// Resolve THIS item's own model wrapper — never CurrentHISMWrapper.
	UFragmentModelWrapper* Wrapper = GetFragmentModel(Item.ModelGuid);
	if (!Wrapper)
	{
		UE_LOG(LogFragments, Verbose,
			TEXT("[HISM async] Dropping item LocalId=%lld ModelGuid=%s - model wrapper not found (model likely unloaded during an in-flight shell build)"),
			Item.LocalId, *Item.ModelGuid);
		return;
	}

	// HLOD Proxy Accumulation (deferred for deterministic ordering)
	if (bGenerateProxyMesh && IsShellCategory(Item.Category))
	{
		const uint32 MatCRC = HashMaterialProperties(Item.R, Item.G, Item.B, Item.A, Item.bIsGlass);
		UMaterialInterface* ResolvedMat = Wrapper->ResolveOverrideMaterial(Item.R, Item.G, Item.B, Item.A, Item.bIsGlass);
		UMaterialInterface* ProxyMat = ResolvedMat ? ResolvedMat : MatIF;
		// QueueIndex, not LocalId: LocalId is shared by every sample of one element, so it is not a total order (see FPendingHISMItem::QueueIndex doc).
		RecordProxyContribution(Item.ModelGuid, Item.QueueIndex, Item.SamplePath,
			Item.WorldTransform, MatCRC, ProxyMat);
	}

	// Compute cell for this item
	{
		Item.bShell = IsShellCategory(Item.Category);

		int16 Floor = 0;
		if (const int16* FoundFloor = Wrapper->LocalIdToFloor.Find(Item.LocalId))
		{
			Floor = (*FoundFloor >= 0) ? *FoundFloor : 0;
		}

		int16 GX = 0, GY = 0;
		const float CellSizeCm = CVarCellSizeCm.GetValueOnGameThread();
		UE::Fragments::CellPartition::ComputeGridXY(
			Item.WorldTransform.GetLocation(),
			Wrapper->ModelAnchor.X,
			Wrapper->ModelAnchor.Y,
			CellSizeCm, GX, GY);

		Item.Cell = FCellId(Floor, GX, GY);

		TMap<FCellId, FCellMetadata>& TargetTable =
			Item.bShell ? Wrapper->ShellCells : Wrapper->InteriorCells;
		FCellMetadata& CellMeta = TargetTable.FindOrAdd(Item.Cell);
		CellMeta.ItemCount++;
		CellMeta.WorldBounds += Item.WorldTransform.GetLocation();
	}

	// Bucket key
	FFragBucketKey Key;
	Key.Mesh = Mesh;
	Key.Material = MatIF;
	Key.bShell = Item.bShell ? 1 : 0;
	Key.Cell = Item.Cell;

	PendingBucketInstances.FindOrAdd(Key).Add(MoveTemp(Item));
	HISMItemsSpawned++;
}

void UFragmentsImporter::ProcessHISMSpawnChunk()
{
	// Defer GC every spawn tick — prevents IncrementalPurgeGarbage from slicing into the frame (measured: 334 ms x 3 passes).
	if (GEngine) { GEngine->DelayGarbageCollection(); }

	// Async LOD0 builds complete during this phase, but the registration-streaming tick that normally drains finalizes does not start until spawning ends.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Frag_DrainMeshFinalizes_Spawn);
		const float DrainBudgetShare = FMath::Clamp(
			CVarFragmentsSpawnPhaseDrainBudgetShare.GetValueOnGameThread(), 0.1f, 0.95f);
		DrainMeshFinalizes(FrameBudgetCoordinator.TotalFrameBudgetMs * DrainBudgetShare);
	}

	// Progress phases follow the queue, mesh wait, and finalize-tail states.
	if (PendingHISMQueue.Num() == 0)
	{
		// Barrier: every shell build must land before bucket finalize, or buckets and the proxy accumulator would be missing their waiters' contributions.
		if (ShouldDeferBucketFinalize())
		{
			++ConsecutiveBucketFinalizeDeferrals;
			if ((ConsecutiveBucketFinalizeDeferrals % GFragmentsShellBuildBarrierStallLogThreshold) == 0)
			{
				TArray<FString> StuckSamplePaths;
				InFlightShellBuilds.GetKeys(StuckSamplePaths);
				UE_LOG(LogFragments, Warning,
					TEXT("[HISM async] Bucket finalize has been deferred for %d consecutive spawn ticks waiting on %d in-flight shell build(s): %s"),
					ConsecutiveBucketFinalizeDeferrals, StuckSamplePaths.Num(), *FString::Join(StuckSamplePaths, TEXT(", ")));
			}
			return; // timer refires; the drain above keeps making progress
		}
		ConsecutiveBucketFinalizeDeferrals = 0;

		// Queue exhausted — chunk-finalize buckets across multiple ticks so the 115ms AddInstances/HISMC creation burst doesn't land on a single tick.
		if (!bIsFinalizingBuckets)
		{
			BeginFinalizeHISMBuckets();
		}
		if (FinalizeTailPhase == EFinalizeTailPhase::None)
		{
			if (!ProcessFinalizeBucketsChunk())
			{
				return; // more buckets next tick
			}
			// Buckets done.
			if (CurrentBucketRoot)
			{
				if (!bIsProcessingDeferredInteriors)
				{
					if (!RegistrationManager)
					{
						RegistrationManager = NewObject<UFragmentRegistrationManager>(this);
						RegistrationManager->OnInteriorLoadRequested.AddDynamic(
							this, &UFragmentsImporter::HandleInteriorLoadRequested);
					}

					const bool bHasDeferred = bEnableShellFirstStreaming
						&& DeferredInteriorQueues.Contains(CurrentSpawningModelGuid);

					// Register now with a nullptr proxy; the async LOD2 proxy build kicked off below (in the completion branch) attaches it later via RegistrationManager->AttachProxyComponent when that build finishes.
					RegistrationManager->RegisterModel(CurrentBucketRoot, bHasDeferred, /*ProxyComponent=*/ nullptr,
						CurrentHISMWrapper);
					ModelBucketRoots.Add(CurrentSpawningModelGuid, CurrentBucketRoot);
				}
				else if (RegistrationManager)
				{
					RegistrationManager->RegisterNewComponents(CurrentBucketRoot, CurrentHISMWrapper);
				}
			}

			// Run the tail: paced across ticks, or legacy all-at-once.
			if (!GFragmentsCellMergeEnqueuePaced)
			{
				EnqueueEagerCellMerges();
				ApplyMaterialOverrides(CurrentBucketRoot, CurrentHISMWrapper);
				EndFinalizeHISMBuckets();
			}
			else
			{
				BeginCellMergeEnqueue(); // -> FinalizeTailPhase == EnqueueCellMerges
			}
		}

		// Phase B: paced cell-merge enqueue.
		if (FinalizeTailPhase == EFinalizeTailPhase::EnqueueCellMerges)
		{
			if (!ProcessCellMergeEnqueueChunk())
			{
				return; // more cells next tick
			}
			BeginApplyOverrides(); // -> FinalizeTailPhase == ApplyOverrides
		}

		// Phase C: paced override apply.
		if (FinalizeTailPhase == EFinalizeTailPhase::ApplyOverrides)
		{
			if (!ProcessApplyOverridesChunk())
			{
				return; // more overrides next tick
			}
			EndFinalizeHISMBuckets(); // cleanup + resets FinalizeTailPhase to None
		}

		// Stop timer
		if (UWorld* World = OwnerRef->GetWorld())
		{
			World->GetTimerManager().ClearTimer(HISMSpawnTimerHandle);
		}

		bIsHISMChunkedSpawning = false;

		// Diagnostic counts
		int32 BucketCount = 0;
		int32 TotalInstances = 0;
		if (CurrentBucketRoot)
		{
			const auto& Buckets = CurrentBucketRoot->GetBuckets();
			BucketCount = Buckets.Num();
			for (const auto& Pair : Buckets)
			{
				if (Pair.Value) TotalInstances += Pair.Value->GetInstanceCount();
			}
		}

		UE_LOG(LogFragments, Log, TEXT("[HISM Chunked] Complete. Spawned %d items into %d buckets (%d total instances)."),
			HISMItemsSpawned, BucketCount, TotalInstances);

		if (bIsProcessingDeferredInteriors)
		{
			// Deferred interior spawn completed.
			bIsProcessingDeferredInteriors = false;
			if (RegistrationManager && CurrentBucketRoot)
			{
				RegistrationManager->RegisterNewComponents(CurrentBucketRoot, CurrentHISMWrapper);
			}

			// Wait for interior merges to drain before ending the import trace.
			if (AActor* Owner = OwnerRef.Get())
			{
				if (UWorld* W = Owner->GetWorld())
				{
					W->GetTimerManager().ClearTimer(ImportTraceStopPollHandle);
					W->GetTimerManager().SetTimer(ImportTraceStopPollHandle,
						FTimerDelegate::CreateLambda([this]()
						{
							AActor* InnerOwner = OwnerRef.Get();
							UWorld* InnerW = InnerOwner ? InnerOwner->GetWorld() : nullptr;
							if (!InnerW) return;
							// Was a World -> GameInstance -> subsystem walk, null in an editor world, so the clear below was unreachable there and this 10 Hz looping timer stayed installed for the life of the world, one per editor import.
							FFragmentCellMergeManager* CMM = GetCellMergeManager();
							if (!CMM || CMM->GetInFlightMergeCount() == 0)
							{
								InnerW->GetTimerManager().ClearTimer(ImportTraceStopPollHandle);
							}
						}),
						0.1f, /*bLoop=*/ true);
				}
			}

			// Restart the per-frame registration update timer (if not running)
			EnsureRegistrationStreamingTicking(OwnerRef->GetWorld());

			// A model load can have been parked while THIS interior spawn was running (ProcessLoadedFragment defers whenever bIsHISMChunkedSpawning is set).
			switch (DecidePostSpawnAction(DeferredModelLoads.Num(), PendingInteriorActivations.Num()))
			{
			case EPostSpawnAction::DrainDeferredModel:
				DrainNextDeferredModelLoad();
				break;

			case EPostSpawnAction::ActivateInterior:
			{
				const FString NextGuid = PendingInteriorActivations[0];
				PendingInteriorActivations.RemoveAt(0);
				HandleInteriorLoadRequested(NextGuid);
				break;
			}

			case EPostSpawnAction::Idle:
				break;
			}
		}
		else
		{
			// Initial shell import completed.
			if (CurrentBucketRoot && bGenerateProxyMesh)
			{
				// Hand the model's buffered LOD2 contributions to a background worker.
				LaunchProxyContributionFlush(CurrentSpawningModelGuid, CurrentBucketRoot);
			}

			// Start per-frame timer for registration streaming updates
			EnsureRegistrationStreamingTicking(OwnerRef->GetWorld());

			// Unbind before Execute: a BP callback chaining the next model re-enters ProcessLoadedFragment and rebinds OnProcessLoadedFragmentComplete; an Unbind-after would kill the new model's callback (batch import stops at 2).
			if (OnProcessLoadedFragmentComplete.IsBound())
			{
				FOnProcessLoadedFragmentComplete Local = OnProcessLoadedFragmentComplete;
				OnProcessLoadedFragmentComplete.Unbind();
				Local.Execute(true, TEXT(""), CurrentSpawningModelGuid);
			}

			// Same decision as the deferred-interior branch above — one rule, both paths.
			switch (DecidePostSpawnAction(DeferredModelLoads.Num(), PendingInteriorActivations.Num()))
			{
			case EPostSpawnAction::DrainDeferredModel:
				if (DrainNextDeferredModelLoad())
				{
					break;
				}
				// Every parked entry had a dead owner, so nothing was started.
				RestoreImportOverhead();
				break;

			case EPostSpawnAction::ActivateInterior:
			{
				RestoreImportOverhead();

				// All initial model imports done — drain any interior activations that were deferred because they arrived during shell spawning.
				UE_LOG(LogFragments, Verbose, TEXT("[Shell-First] All models imported. Processing %d deferred interior activations."),
					PendingInteriorActivations.Num());
				// Activate the first one; each subsequent activation will be triggered by the registration manager on the next UpdateRegistration pass (bInteriorsTriggered is already set, so we call the handler directly).
				const FString FirstGuid = PendingInteriorActivations[0];
				PendingInteriorActivations.RemoveAt(0);
				HandleInteriorLoadRequested(FirstGuid);
				break;
			}

			case EPostSpawnAction::Idle:
				RestoreImportOverhead();
				break;
			}
		}
		return;
	}

	const float SpawnSliceMs = FrameBudgetCoordinator.AllocateRemainingMs();
	const double FrameStartTime = FPlatformTime::Seconds();
	const double FrameBudgetSec = SpawnSliceMs / 1000.0;
	int32 ProcessedThisFrame = 0;

	// Ensure base materials are loaded
	if (!BaseMaterial)
	{
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));
	}
	if (!BaseGlassMaterial)
	{
		BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
	}

	// Loop-invariant: worker thread count doesn't change mid-loop, so resolve the backpressure cap once rather than every iteration.
	const int32 MaxInFlightShellBuilds = ResolveMaxInFlightShellBuilds(
		FTaskGraphInterface::Get().GetNumWorkerThreads());

	while (PendingHISMQueue.Num() > 0)
	{
		// Check frame budget
		const double Elapsed = FPlatformTime::Seconds() - FrameStartTime;
		if (Elapsed >= FrameBudgetSec && ProcessedThisFrame > 0)
		{
			break;
		}

		// Pop next item (swap with last for O(1) removal)
		FPendingHISMItem Item = MoveTemp(PendingHISMQueue.Last());
		PendingHISMQueue.RemoveAt(PendingHISMQueue.Num() - 1, 1, FRAGMENTS_NO_SHRINKING);

		// Resolve BucketRoot for this item (may differ from CurrentBucketRoot when deferred interiors from multiple models are in the queue).
		if (Item.ModelGuid != CurrentSpawningModelGuid)
		{
			AFragment* ItemBucketRoot = GetBucketRootForModel(Item.ModelGuid);
			if (!ItemBucketRoot)
			{
				UE_LOG(LogFragments, Verbose, TEXT("[Shell-First] Skipping item from unloaded model %s"), *Item.ModelGuid);
				ProcessedThisFrame++;
				continue;
			}
			CurrentBucketRoot = ItemBucketRoot;
			CurrentSpawningModelGuid = Item.ModelGuid;

			// Also update MeshesRef for this model
			if (UFragmentModelWrapper* Wrapper = GetFragmentModel(Item.ModelGuid))
			{
				CurrentMeshesRef = Wrapper->GetParsedModel()->meshes();
				CurrentHISMWrapper = Wrapper;
			}
		}

		// RESOLVE MESH (mirrors SpawnFragmentModel lines 1370-1424).
		TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ResolveMesh);
		const Representation* Rep = CurrentMeshesRef->representations()->Get(Item.RepresentationIndex);
		const int64 RepId = Rep->id();
		const Material* MatRef = CurrentMeshesRef->materials()->Get(Item.MaterialIndex);

		FString MeshName = FString::Printf(TEXT("rep_%lld"), RepId);
		FString PackagePath = TEXT("/Game/Buildings") / Item.ModelGuid / MeshName;
		const FString SamplePath = PackagePath + TEXT(".") + MeshName;
		// Keep in lockstep with SamplePath above — ProcessResolvedHISMItem reads Item.SamplePath instead of rebuilding this string (see FragmentsUtils.h).
		Item.SamplePath = SamplePath;

		// Resolve or launch the mesh build
		UStaticMesh* Mesh = nullptr;
		if (UStaticMesh** CachedMesh = MeshCache.Find(SamplePath))
		{
			Mesh = *CachedMesh;
			if (!Mesh)
			{
				// A previous build failed for this representation; skip quietly.
				ProcessedThisFrame++;
				continue;
			}
		}
		else if (Rep->representation_class() == RepresentationClass::RepresentationClass_SHELL)
		{
			// Backpressure: cap concurrent worker builds so the (now cheap) loop cannot launch thousands of tasks and balloon PendingMeshFinalizes.
			const bool bAlreadyInFlight = InFlightShellBuilds.Contains(SamplePath);
			if (!bAlreadyInFlight && InFlightShellBuilds.Num() >= MaxInFlightShellBuilds)
			{
				// Item has already been popped from PendingHISMQueue — push it back before yielding so no work is lost.
				PendingHISMQueue.Add(MoveTemp(Item));
				break; // yield the frame; the next tick drains finalizes first
			}

			// BuildHISMSpawnQueue already read these off the same Material this item's MaterialIndex resolves to; re-deriving them here would be a second definition of "is glass" free to drift from the item's.
			const FColor VertexColor(Item.R, Item.G, Item.B, Item.A);
			UMaterialInterface* SlotMaterial = GetPooledMaterial(Item.R, Item.G, Item.B, Item.A, Item.bIsGlass);

			// Capture ShellRef and its buffer pin together before launching the worker.
			const Shell* ShellRef = CurrentMeshesRef->shells()->Get(Rep->id());
			FModelBufferPin BufferPin = CurrentHISMWrapper ? CurrentHISMWrapper->GetBufferPin() : FModelBufferPin();
			// Unconditional, deliberately.
			const bool bNeedsProxyData = bGenerateProxyMesh;

			// Async path: author on a worker, finalize budget-paced on the game thread.
			const bool bStartBuild = RegisterShellBuildWaiter(SamplePath, MoveTemp(Item));
			if (bStartBuild)
			{
				TWeakObjectPtr<UFragmentsImporter> WeakThis(this);
				const FString SamplePathCopy = SamplePath;
				EnqueueShellMeshBuild(
					SamplePath, ShellRef, VertexColor, SlotMaterial, bNeedsProxyData,
					MoveTemp(BufferPin),
					[WeakThis, SamplePathCopy](FShellBuildResult&& Result)
					{
						if (UFragmentsImporter* Importer = WeakThis.Get())
						{
							Importer->CompleteShellBuild(SamplePathCopy, MoveTemp(Result));
						}
					});
			}

			// Either way this item is now a waiter — its downstream work runs at completion.
			ProcessedThisFrame++;
			continue;
		}
		else if (Rep->representation_class() == RepresentationClass_CIRCLE_EXTRUSION)
		{
			// Circle extrusions stay synchronous (not the hitch source, and the editor build path needs the game thread).
#if WITH_EDITOR
			UPackage* MeshPackage = CreatePackage(*PackagePath);
#else
			UPackage* MeshPackage = GetTransientPackage();
#endif
			const auto* circleExtrusion = CurrentMeshesRef->circle_extrusions()->Get(Rep->id());
			Mesh = CreateStaticMeshFromCircleExtrusion(circleExtrusion, MatRef, *MeshName, MeshPackage, Item.ModelGuid);
			MeshCache.Add(SamplePath, Mesh);
		}

		if (!Mesh)
		{
			ProcessedThisFrame++;
			continue;
		}

		// RESOLVE MATERIAL + DOWNSTREAM (cache-hit / circle-extrusion path).
		UMaterialInterface* MatIF = GetPooledMaterial(Item.R, Item.G, Item.B, Item.A, Item.bIsGlass);
		if (!MatIF)
		{
			ProcessedThisFrame++;
			continue;
		}

		ProcessResolvedHISMItem(MoveTemp(Item), Mesh, MatIF);
		ProcessedThisFrame++;
	}

	FrameBudgetCoordinator.RecordWork(static_cast<float>((FPlatformTime::Seconds() - FrameStartTime) * 1000.0));

	// Bucket keys are stable at construction (Mesh + Material + bShell), so no post-hoc rewrite is needed.
}

// Static mesh creation records census data through a static counter.
static TMap<FString, int32> GStaticSiteMeshCounts;

UStaticMesh* UFragmentsImporter::CommitToRenderData(
	const FFragmentMeshData& MeshData,
	UObject* Outer,
	UMaterialInterface* Material,
	const FString& ModelGuid)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_CommitToRenderData);
	if (!MeshData.bIsValid || MeshData.Vertices.Num() == 0 || MeshData.Indices.Num() < 3)
	{
		UE_LOG(LogFragments, Warning, TEXT("CommitToRenderData: Invalid mesh data for %s"), *MeshData.AssetName);
		return nullptr;
	}

	// 1. Create the UStaticMesh — RF_Transient excludes it from save scanning and GC package walks
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Outer, FName(*MeshData.AssetName), RF_Transient);

	// No census counter here (removed): CommitToRenderData has zero callers repo-wide (production or test -- see the "LEGACY" comment on the declaration), so a counter here can never be non-zero.

	// 2. Add material slot — must happen before SetRenderData so the section's MaterialIndex is valid
	if (Material)
	{
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Fragment")));
	}

	// 3. Create render data with a single LOD
	TUniquePtr<FStaticMeshRenderData> RenderData = MakeUnique<FStaticMeshRenderData>();
	RenderData->AllocateLODResources(1);
	FStaticMeshLODResources& LOD = RenderData->LODResources[0];

	const int32 NumVerts = MeshData.Vertices.Num();
	const int32 NumIndices = MeshData.Indices.Num();

	// 4. Position vertex buffer — build a TArray<FVector3f> for the bulk Init overload
	{
		TArray<FVector3f> Positions;
		Positions.SetNumUninitialized(NumVerts);
		for (int32 i = 0; i < NumVerts; i++)
		{
			Positions[i] = MeshData.Vertices[i].Position;
		}
		// SnapshotCellMergeSources reads PosBuf.VertexPosition(i) at runtime.
		LOD.VertexBuffers.PositionVertexBuffer.Init(Positions, /*bInNeedsCPUAccess=*/ true);
	}

	// 5. Static mesh vertex buffer (normals, tangents, UVs) — 1 UV channel
	LOD.VertexBuffers.StaticMeshVertexBuffer.Init(NumVerts, /*NumTexCoords=*/ 1, /*bNeedsCPUAccess=*/ true);
	for (int32 i = 0; i < NumVerts; i++)
	{
		const FFragmentMeshVertex& V = MeshData.Vertices[i];

		// Reconstruct bitangent from normal, tangent, and sign for SetVertexTangents
		const FVector3f BitangentY = FVector3f::CrossProduct(V.Normal, V.TangentX) * V.TangentYSign;

		LOD.VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, V.TangentX, BitangentY, V.Normal);
		LOD.VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(i, 0, V.UV0);
	}

	// 6. Color vertex buffer
	LOD.VertexBuffers.ColorVertexBuffer.Init(NumVerts, /*bNeedsCPUAccess=*/ true);
	for (int32 i = 0; i < NumVerts; i++)
	{
		LOD.VertexBuffers.ColorVertexBuffer.VertexColor(i) = MeshData.Vertices[i].Color;
	}

	// 7. Index buffer — always 32-bit (meshes can exceed 65K vertices)
	LOD.IndexBuffer.SetIndices(MeshData.Indices, EIndexBufferStride::Force32Bit);

	// SnapshotCellMergeSources requires CPU access to the index buffer.
	LOD.IndexBuffer.TrySetAllowCPUAccess(true);

	// 8. Mesh section — maps index range to material slot
	FStaticMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
	Section.MaterialIndex = 0;
	Section.FirstIndex = 0;
	Section.NumTriangles = NumIndices / 3;
	Section.MinVertexIndex = 0;
	Section.MaxVertexIndex = NumVerts - 1;
	Section.bEnableCollision = true;   // Required for ComplexAsSimple collision (line traces)
	Section.bCastShadow = true;        // Overridden per-HISMC by occlusion classifier

	// Inline the render buffers.
	LOD.bBuffersInlined = true;

	// 10. Bounds — FBox has an explicit constructor from TBox<float> (FBox3f)
	RenderData->Bounds = FBoxSphereBounds(FBox(MeshData.BoundingBox));

	// Disable ray tracing before installing render data.
	StaticMesh->bSupportRayTracing = false;
	StaticMesh->SetRenderData(MoveTemp(RenderData));

	// SnapshotCellMergeSources requires CPU access to vertex/index buffers.
	StaticMesh->bAllowCPUAccess = true;

	StaticMesh->InitResources();          // Enqueues GPU upload on render thread (non-blocking)
	StaticMesh->CalculateExtendedBounds();

	// 12. Custom runtime complex collision (shared with CommitToMeshDescription).
	AttachRuntimeComplexCollision(StaticMesh, MeshData, ModelGuid);

	return StaticMesh;
}

void UFragmentsImporter::AttachRuntimeComplexCollision(
	UStaticMesh* StaticMesh,
	const FFragmentMeshData& MeshData,
	const FString& ModelGuid)
{
	// Create triangle-mesh collision for HISMC line traces / click-to-select.
	TArray<FVector3f> CollisionPositions;
	CollisionPositions.Reserve(MeshData.Vertices.Num());
	for (const FFragmentMeshVertex& V : MeshData.Vertices)
	{
		CollisionPositions.Add(V.Position);
	}

	TArray<uint32> CollisionIndices = MeshData.Indices;

	Chaos::FTriangleMeshImplicitObjectPtr TriMesh =
		FragmentTriMeshCook::BuildTriMeshOnWorker(
			MoveTemp(CollisionPositions),
			MoveTemp(CollisionIndices));

	UBodySetup* BodySetup = NewObject<UBodySetup>(StaticMesh);
	BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	BodySetup->bDoubleSidedGeometry = true;
	BodySetup->InvalidatePhysicsData();

	if (TriMesh.IsValid())
	{
		TriMesh->SetDoCollide(false);
		BodySetup->TriMeshGeometries.Add(MoveTemp(TriMesh));
		BodySetup->bCreatedPhysicsMeshes = true;
	}

	StaticMesh->SetBodySetup(BodySetup);

	// Reparent to StaticMesh after cooking (StaticMesh owns the BodySetup long-term)
	BodySetup->Rename(nullptr, StaticMesh);
	StaticMesh->SetBodySetup(BodySetup);
}

UStaticMesh* UFragmentsImporter::CommitToMeshDescription(
	const FFragmentMeshData& MeshData,
	UObject* Outer,
	UMaterialInterface* Material,
	const FString& ModelGuid)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_CommitToMeshDescription);
	check(IsInGameThread());

	if (!MeshData.bIsValid || MeshData.Vertices.Num() == 0 || MeshData.Indices.Num() < 3)
	{
		UE_LOG(LogFragments, Warning, TEXT("CommitToMeshDescription: Invalid mesh data for %s"), *MeshData.AssetName);
		return nullptr;
	}

	// Create the UStaticMesh — RF_Transient excludes it from save scanning and GC package walks.
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Outer, FName(*MeshData.AssetName), RF_Transient);

	// Key this single-element mesh census by AssetName on cache misses.
	if (CVarCellMergeCensus.GetValueOnGameThread() != 0)
	{
		++GStaticSiteMeshCounts.FindOrAdd(TEXT("CommitToMeshDescription"), 0);
	}

	StaticMesh->bGenerateMeshDistanceField = false;
	StaticMesh->bHasNavigationData = false;
	// Do NOT call InitResources() here — BuildFromMeshDescriptions() does it internally.

	// Material slot 0.
	if (Material)
	{
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Fragment")));
	}

	// Author a mesh description from the pre-triangulated FFragmentMeshData.
	UStaticMeshDescription* SMDesc = StaticMesh->CreateStaticMeshDescription(Outer);
	FMeshDescription& MD = SMDesc->GetMeshDescription();
	FStaticMeshAttributes Attributes(MD);
	MD.SetNumUVChannels(1);

	// Reserve up front — counts are known (one vertex instance + polygon per triangle).
	const int32 NumVerts = MeshData.Vertices.Num();
	const int32 NumTris  = MeshData.Indices.Num() / 3;
	MD.ReserveNewVertices(NumVerts);
	MD.ReserveNewVertexInstances(NumTris * 3);
	MD.ReserveNewPolygons(NumTris);
	MD.ReserveNewTriangles(NumTris);

	TVertexAttributesRef<FVector3f>         VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstNormals  = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float>     InstBiSign   = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> InstColors   = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstUVs      = Attributes.GetVertexInstanceUVs();

	// One vertex per unique source vertex (positions are shared/indexed).
	TArray<FVertexID> VertexIDs;
	VertexIDs.SetNumUninitialized(NumVerts);
	for (int32 i = 0; i < NumVerts; ++i)
	{
		const FVertexID VID = MD.CreateVertex();
		VertexPositions[VID] = MeshData.Vertices[i].Position;
		VertexIDs[i] = VID;
	}

	// One polygon group for the single material slot.
	const FPolygonGroupID PG = MD.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[PG] = TEXT("Fragment");

	// One triangle per 3 indices; one vertex instance per corner carrying the authored normal/tangent/uv/color so output matches the SetRenderData path.
	for (int32 t = 0; t + 2 < MeshData.Indices.Num(); t += 3)
	{
		TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
		bool bValid = true;
		for (int32 k = 0; k < 3; ++k)
		{
			const int32 SrcIdx = (int32)MeshData.Indices[t + k];
			if (!MeshData.Vertices.IsValidIndex(SrcIdx)) { bValid = false; break; }
			const FFragmentMeshVertex& V = MeshData.Vertices[SrcIdx];
			const FVertexInstanceID VIID = MD.CreateVertexInstance(VertexIDs[SrcIdx]);
			InstNormals[VIID]  = V.Normal;
			InstTangents[VIID] = V.TangentX;
			InstBiSign[VIID]   = V.TangentYSign;
			InstUVs.Set(VIID, 0, V.UV0);
			InstColors[VIID]   = FVector4f(FLinearColor(V.Color));
			Corners.Add(VIID);
		}
		if (bValid)
		{
			MD.CreatePolygon(PG, Corners);
		}
	}

	if (MD.Polygons().Num() == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("CommitToMeshDescription: no polygons for %s"), *MeshData.AssetName);
		StaticMesh->MarkAsGarbage();
		return nullptr;
	}

	// Build. bFastBuild = runtime-fast; custom collision path owns collision.
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild             = true;
	Params.bBuildSimpleCollision  = false;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty      = false;
	Params.bUseHashAsGuid         = false;

	// CPU access for SnapshotCellMergeSources.
	Params.bAllowCpuAccess = true;
	StaticMesh->bAllowCPUAccess = true;

	// No RayTracingProxy on runtime-built meshes — disable RT before the build.
	StaticMesh->bSupportRayTracing = false;

	if (!StaticMesh->BuildFromMeshDescriptions(TArray<const FMeshDescription*>{ &MD }, Params))
	{
		UE_LOG(LogFragments, Warning, TEXT("CommitToMeshDescription: BuildFromMeshDescriptions failed for %s"), *MeshData.AssetName);
		StaticMesh->MarkAsGarbage();
		return nullptr;
	}

	// Custom runtime complex collision (shared with CommitToRenderData).
	AttachRuntimeComplexCollision(StaticMesh, MeshData, ModelGuid);

	return StaticMesh;
}

void UFragmentsImporter::CancelProxyBuilds(const FString& ModelGuid)
{
	check(IsInGameThread());

	FScopeLock Lock(&PendingProxyBuildsLock);
	PendingProxyBuilds.Remove(ModelGuid);
	// UE::Tasks (5.3) doesn't support cancellation; in-flight workers run to completion, and their fire-and-forget AsyncTask(GameThread) finalizes bail on stale WeakMesh.
}

void UFragmentsImporter::WaitForAllProxyBuilds()
{
	check(IsInGameThread());

	// The async fragment load is a separate handle from PendingProxyBuilds, but it has the same hazard: its game-thread completion is only ENQUEUED when the task ends.
	if (AsyncLoadTask.IsValid())
	{
		AsyncLoadTask.Wait();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}

	// A registered task's game-thread completion only needs to be *enqueued* (via AsyncTask(GameThread, ...)) for Task.Wait() below to return -- not to have run.
	constexpr int32 kMaxDrainPasses = 8;

	TArray<UE::Tasks::FTask> AlreadyWaited;
	bool bConverged = false;

	for (int32 Pass = 0; Pass < kMaxDrainPasses; ++Pass)
	{
		TArray<UE::Tasks::FTask> Snapshot;
		{
			FScopeLock Lock(&PendingProxyBuildsLock);
			for (auto& Pair : PendingProxyBuilds)
			{
				for (UE::Tasks::FTask& Task : Pair.Value)
				{
					if (Task.IsValid())
					{
						Snapshot.Add(Task);
					}
				}
			}
		}

		TArray<UE::Tasks::FTask> NewTasks;
		for (UE::Tasks::FTask& Task : Snapshot)
		{
			if (!AlreadyWaited.Contains(Task))
			{
				NewTasks.Add(Task);
			}
		}

		if (NewTasks.Num() == 0)
		{
			bConverged = true;
			break;
		}

		for (UE::Tasks::FTask& Task : NewTasks)
		{
			Task.Wait();
		}
		AlreadyWaited.Append(NewTasks);

		// Run queued game-thread work (including the AsyncTask(GameThread, ...) hops the tasks above just unblocked) so any new registrations they trigger are visible to the next pass's snapshot.
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}

	if (!bConverged)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("WaitForAllProxyBuilds: hit the %d-pass drain cap; proxy builds may still be registering new work. Proceeding with teardown regardless."),
			kMaxDrainPasses);
	}

	FScopeLock Lock(&PendingProxyBuildsLock);
	PendingProxyBuilds.Empty();
}

void UFragmentsImporter::AppendCompactedTierLOD(
	int32 TierIndex,
	TUniquePtr<FMeshDescription> MD,
	float ScreenSize,
	TArray<TUniquePtr<FMeshDescription>>& OutDescriptions,
	TArray<float>& OutScreenSizes,
	bool bForceAppend)
{
	// Always append the first surviving tier, even when its mesh is null.
	if (TierIndex > 0 && !bForceAppend && !MD.IsValid())
	{
		return;
	}
	OutDescriptions.Add(MoveTemp(MD));
	OutScreenSizes.Add(ScreenSize);
}

void UFragmentsImporter::PruneTrailingEmptyTiers(TArray<FProxyMeshAccumulator>& Tiers)
{
	// A tier whose cutoff dropped everything would otherwise produce a degenerate LOD.
	while (Tiers.Num() > 1 && Tiers.Last().IsEmpty())
	{
		Tiers.Pop();
	}
}

void UFragmentsImporter::EnqueueProxyMeshBuild(
	const FString& ModelGuid,
	TArray<FProxyMeshAccumulator>& Tiers,
	TConstArrayView<FragmentProxyTiers::FTierSpec> TierSpecs,
	float ModelBoundsRadiusCm,
	TFunction<void(UStaticMesh*)> OnComplete)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_CommitProxyToRenderData);

	if (Tiers.Num() == 0 || Tiers[0].IsEmpty())
	{
		UE_LOG(LogFragments, Warning, TEXT("EnqueueProxyMeshBuild: Empty accumulator"));
		if (OnComplete) OnComplete(nullptr);
		return;
	}

	// Tier 0 is guaranteed non-empty by the check above -- see PruneTrailingEmptyTiers' doc comment.
	PruneTrailingEmptyTiers(Tiers);

	// Tally and bail early on degenerate input — keeps the worker hop only for real work.
	int32 TotalVerts = 0;
	int32 TotalIndices = 0;
	for (const auto& [CRC, Section] : Tiers[0].Sections)
	{
		TotalVerts += Section.VertexCount();
		TotalIndices += Section.Indices.Num();
	}
	if (TotalVerts == 0 || TotalIndices < 3)
	{
		UE_LOG(LogFragments, Warning, TEXT("EnqueueProxyMeshBuild: No geometry (%d verts, %d indices)"), TotalVerts, TotalIndices);
		if (OnComplete) OnComplete(nullptr);
		return;
	}

	// The proxy needs a world the models live in.
	UWorld* GameWorld = ResolveModelWorld();
	if (!GameWorld && GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.World() && Ctx.World()->IsGameWorld())
			{
				GameWorld = Ctx.World();
				break;
			}
		}
	}
	if (!GameWorld)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("EnqueueProxyMeshBuild: no world for the proxy mesh, skipping the build"));
		if (OnComplete) OnComplete(nullptr);
		return;
	}

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	// Keep alive across the worker hop; the completion lambda hands ownership to the proxy component via SetStaticMesh, then unroots.
	StaticMesh->AddToRoot();

	// Material slots come from tier 0's CRC set only, sorted for determinism (TMap::GetKeys has no defined order).
	TArray<uint32> SectionOrder;
	Tiers[0].Sections.GetKeys(SectionOrder);
	SectionOrder.Sort();

	for (int32 SlotIdx = 0; SlotIdx < SectionOrder.Num(); SlotIdx++)
	{
		const FProxyMeshSection& Section = Tiers[0].Sections[SectionOrder[SlotIdx]];
		UMaterialInterface* Mat = Section.Material;
		FName SlotName = *FString::Printf(TEXT("ProxySlot_%d"), SlotIdx);
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(Mat, SlotName));
	}

	// Snapshot section data into worker-safe POD, per tier, mapped onto the shared slot indices above.
	TArray<TArray<FProxyMeshSectionSnapshot>> TierSnapshots;
	TierSnapshots.SetNum(Tiers.Num());
	for (int32 t = 0; t < Tiers.Num(); ++t)
	{
		for (int32 SlotIdx = 0; SlotIdx < SectionOrder.Num(); SlotIdx++)
		{
			FProxyMeshSection* Section = Tiers[t].Sections.Find(SectionOrder[SlotIdx]);
			if (!Section) { continue; }   // dropped from this tier — legal, slot index unchanged

			FProxyMeshSectionSnapshot& Snap = TierSnapshots[t].AddDefaulted_GetRef();
			// Move geometry into the snapshot — the accumulator is dropped right after.
			Snap.Geo.Positions     = MoveTemp(Section->Positions);
			Snap.Geo.Normals       = MoveTemp(Section->Normals);
			Snap.Geo.TangentsX     = MoveTemp(Section->TangentsX);
			Snap.Geo.TangentYSigns = MoveTemp(Section->TangentYSigns);
			Snap.Geo.UV0           = MoveTemp(Section->UV0);
			Snap.Geo.VertexColors  = MoveTemp(Section->VertexColors);
			Snap.Geo.Indices       = MoveTemp(Section->Indices);
			Snap.MaterialSlot      = SlotIdx;
		}
	}

	// Per-tier simplify ratios, parallel to TierSnapshots.
	TArray<float> TierSimplifyRatios;
	TierSimplifyRatios.SetNum(TierSnapshots.Num());
	for (int32 t = 0; t < TierSnapshots.Num(); ++t)
	{
		TierSimplifyRatios[t] = TierSpecs.IsValidIndex(t) ? TierSpecs[t].SimplifyRatio : 1.0f;
	}

	// Compute ScreenSize once accumulated proxy bounds are available.
	TArray<FragmentProxyTiers::FTierSpec> ScreenSizeSpecs;
	ScreenSizeSpecs.Reserve(TierSnapshots.Num());
	for (int32 t = 0; t < TierSnapshots.Num(); ++t)
	{
		ScreenSizeSpecs.Add(TierSpecs.IsValidIndex(t) ? TierSpecs[t] : FragmentProxyTiers::FTierSpec{});
	}
	const float ProxyBoundsRadiusCm = Tiers[0].BoundingBox.GetExtent().Size();
	const TArray<float> NearEdgesCm = ResolveNearEdgesCm(ModelBoundsRadiusCm);
	FillProxyTierScreenSizes(ScreenSizeSpecs, NearEdgesCm, ProxyBoundsRadiusCm);

	TArray<float> TierScreenSizes;
	TierScreenSizes.SetNum(TierSnapshots.Num());
	for (int32 t = 0; t < TierSnapshots.Num(); ++t)
	{
		TierScreenSizes[t] = ScreenSizeSpecs[t].ScreenSize;
	}

	TWeakObjectPtr<UStaticMesh> WeakMesh(StaticMesh);
	TWeakObjectPtr<UFragmentsImporter> WeakThis(this);

	// Read on the game thread, captured by value -- worker-safety: no CVar reads on the worker.
	const float CollisionDecimateRatio =
		FMath::Clamp(CVarProxyCollisionDecimateRatio.GetValueOnGameThread(), 0.05f, 1.0f);

	// Per-model leading-tier prune (see CVarProxyPruneUnreachableTiers' doc comment for why this ships default-off).
	int32 UnreachableLeadingTierCount = 0;
	if (CVarProxyPruneUnreachableTiers.GetValueOnGameThread() != 0)
	{
		const float GeoCoeffCells = CVarLODGeoCoeffCells.GetValueOnGameThread();
		const float ThresholdPx   = CVarLODSSEPixelError.GetValueOnGameThread();
		const float RefViewCoeff  = FragmentProxyTiers::ViewCoeff(
			FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);

		// Model tier ladder's own Proxy->Cells promote distance -- the conservative (smaller, keeps-more-tiers) choice vs. the demote distance.
		const float PromoteCm = (ThresholdPx > 0.0f)
			? GeoCoeffCells * ModelBoundsRadiusCm * RefViewCoeff / ThresholdPx
			: 0.0f;

		UnreachableLeadingTierCount =
			CountUnreachableLeadingTiers(TierScreenSizes, ProxyBoundsRadiusCm, PromoteCm);

		if (UnreachableLeadingTierCount > 0)
		{
			const float FailedEngageCm = FragmentProxyTiers::kRefProjM11 * ProxyBoundsRadiusCm /
				TierScreenSizes[UnreachableLeadingTierCount];
			UE_LOG(LogFragments, Display,
				TEXT("[ProxyTiers] Model %s: pruning %d leading unreachable render tier(s) -- ")
				TEXT("R_model=%.1fcm R_proxy=%.1fcm PromoteCm=%.1f engage(%d)=%.1f <= PromoteCm"),
				*ModelGuid, UnreachableLeadingTierCount, ModelBoundsRadiusCm, ProxyBoundsRadiusCm,
				PromoteCm, UnreachableLeadingTierCount, FailedEngageCm);
		}
	}

	UE::Tasks::FTask BuildTask = UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[
			WeakThis,
			AllTierSnaps = MoveTemp(TierSnapshots),
			WeakMesh,
			TierSimplifyRatios = MoveTemp(TierSimplifyRatios),
			TierScreenSizesIn = MoveTemp(TierScreenSizes),
			CollisionDecimateRatio,
			UnreachableLeadingTierCount,
			OnComplete = MoveTemp(OnComplete)
		]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ProxyBuildOnWorker);

			// Keep each compacted mesh description aligned with its ScreenSize.
			TArray<TUniquePtr<FMeshDescription>> CompactedDescriptions;
			TArray<float> CompactedScreenSizes;
			CompactedDescriptions.Reserve(AllTierSnaps.Num());
			CompactedScreenSizes.Reserve(AllTierSnaps.Num());

			// Collision (tier 0 geometry only) cooks from a SEPARATELY decimated copy of tier 0's sections, at r.Fragments.Proxy.CollisionDecimateRatio -- independent of the visual tier's SimplifyRatio.
			TArray<FProxyMeshSectionSnapshot> CollisionSnaps = AllTierSnaps[0];
			if (CollisionDecimateRatio < 1.0f)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_DecimateProxyCollisionOnWorker);
				for (FProxyMeshSectionSnapshot& Snap : CollisionSnaps)
				{
					FragmentProxyDecimate::DecimateSection(Snap.Geo, CollisionDecimateRatio);
				}
			}
			TArray<FragmentMeshAuthoring::FAuthoringSection> CollisionAuthorSections;
			CollisionAuthorSections.Reserve(CollisionSnaps.Num());
			for (const FProxyMeshSectionSnapshot& Snap : CollisionSnaps)
			{
				FragmentMeshAuthoring::FAuthoringSection& Sec = CollisionAuthorSections.AddDefaulted_GetRef();
				Sec.MaterialSlot  = Snap.MaterialSlot;
				Sec.Positions     = Snap.Geo.Positions;
				Sec.Normals       = Snap.Geo.Normals;
				Sec.TangentsX     = Snap.Geo.TangentsX;
				Sec.TangentYSigns = Snap.Geo.TangentYSigns;
				Sec.UV0           = Snap.Geo.UV0;
				Sec.Colors        = Snap.Geo.VertexColors;
				Sec.Indices       = Snap.Geo.Indices;
			}
			// Collision-only authoring pass; the resulting FMeshDescription is discarded -- only the flat world positions/indices out-params feed the trimesh cook below.
			FMeshDescription CollisionOnlyMD;
			TArray<FVector3f> CollisionPositions;
			TArray<uint32> CollisionIndices;
			FragmentMeshAuthoring::AuthorMeshDescription(
				CollisionAuthorSections, TEXT("ProxyCollision_"), CollisionOnlyMD,
				CollisionPositions, CollisionIndices);

			int32 TotalTris = 0;

			// Tracks whether any tier has reached AppendCompactedTierLOD yet this build.
			bool bAppendedAnyTierThisBuild = false;

			for (int32 t = 0; t < AllTierSnaps.Num(); ++t)
			{
				TArray<FProxyMeshSectionSnapshot>& Snaps = AllTierSnaps[t];
				const float SimplifyRatio = TierSimplifyRatios[t];

				// Decimate each section in place (worker thread), preserving UV0 + tangents.
				if (SimplifyRatio < 1.0f)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_DecimateProxyOnWorker);
					for (FProxyMeshSectionSnapshot& Snap : Snaps)
					{
						FragmentProxyDecimate::DecimateSection(Snap.Geo, SimplifyRatio);
					}
				}

				// Phase B: author an FMeshDescription for this tier.
				TUniquePtr<FMeshDescription> MD = MakeUnique<FMeshDescription>();

				// FAuthoringSection fields are views borrowing Snaps (alive through AuthorMeshDescription); positions are already world-space, so no per-section instance transform.
				TArray<FragmentMeshAuthoring::FAuthoringSection> Sections;
				Sections.Reserve(Snaps.Num());
				for (const FProxyMeshSectionSnapshot& Snap : Snaps)
				{
					FragmentMeshAuthoring::FAuthoringSection& Sec = Sections.AddDefaulted_GetRef();
					Sec.MaterialSlot  = Snap.MaterialSlot;
					Sec.Positions     = Snap.Geo.Positions;
					Sec.Normals       = Snap.Geo.Normals;
					Sec.TangentsX     = Snap.Geo.TangentsX;
					Sec.TangentYSigns = Snap.Geo.TangentYSigns;
					Sec.UV0           = Snap.Geo.UV0;
					Sec.Colors        = Snap.Geo.VertexColors;
					Sec.Indices       = Snap.Geo.Indices;
				}

				// Collision geometry is cooked separately above (from CollisionSnaps); every tier here passes throwaway out-params.
				TArray<FVector3f> ThisCollisionPositions;
				TArray<uint32> ThisCollisionIndices;
				const bool bAuthored = FragmentMeshAuthoring::AuthorMeshDescription(
					Sections, TEXT("ProxySlot_"), *MD,
					ThisCollisionPositions, ThisCollisionIndices);

				// This tier is one of a per-model LEADING run that CountUnreachableLeadingTiers proved unreachable (r.Fragments.Proxy.PruneUnreachableTiers, default off -- see its doc comment).
				const bool bTierPrunedFromRenderList = (t < UnreachableLeadingTierCount);

				if (!bAuthored)
				{
					MD.Reset();
				}
				else if (!bTierPrunedFromRenderList)
				{
					// Charge drain work only for authored tiers retained in the payload.
					for (const FProxyMeshSectionSnapshot& Snap : Snaps)
					{
						TotalTris += Snap.Geo.Indices.Num() / 3;
					}
				}

				if (!bTierPrunedFromRenderList)
				{
					// Force-append the first tier remaining after pruning.
					const bool bForceAppend = !bAppendedAnyTierThisBuild;
					UFragmentsImporter::AppendCompactedTierLOD(
						t, MoveTemp(MD), TierScreenSizesIn[t], CompactedDescriptions, CompactedScreenSizes,
						bForceAppend);
					bAppendedAnyTierThisBuild = true;
				}
			}

			// Cook the trimesh on this worker from the collision-decimated positions + indices.
			Chaos::FTriangleMeshImplicitObjectPtr TriMesh;
			if (CollisionIndices.Num() >= 3)
			{
				TriMesh = FragmentTriMeshCook::BuildTriMeshOnWorker(MoveTemp(CollisionPositions), MoveTemp(CollisionIndices));
			}

			// Phase C: hop to game thread to attach UObject state.
			AsyncTask(ENamedThreads::GameThread,
				[
					WeakThis,
					WeakMesh,
					CompactedDescriptions = MoveTemp(CompactedDescriptions),
					CompactedScreenSizes = MoveTemp(CompactedScreenSizes),
					TriMesh = MoveTemp(TriMesh),
					TotalTris,
					OnComplete = MoveTemp(OnComplete)
				]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ProxyEnqueue);
					check(IsInGameThread());

					UFragmentsImporter* ImporterPtr = WeakThis.Get();
					if (!ImporterPtr)
					{
						// Importer torn down mid-hop; drop the build.
						if (UStaticMesh* M = WeakMesh.Get()) { M->RemoveFromRoot(); }
						return;
					}

					FPendingMeshFinalize Payload;
					Payload.WeakMesh          = WeakMesh;
					Payload.MeshDescriptions  = MoveTemp(CompactedDescriptions);
					Payload.LODScreenSizes    = MoveTemp(CompactedScreenSizes);
					Payload.TriMesh           = MoveTemp(TriMesh);
					Payload.Finalizer         = MoveTemp(OnComplete);
					Payload.DebugLabel        = TEXT("ProxyMesh");
					Payload.EstimatedTriangleCount = TotalTris;

					ImporterPtr->DispatchMeshFinalize(MoveTemp(Payload));
				});
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);

	// Track for cancellation / drain.
	{
		FScopeLock Lock(&PendingProxyBuildsLock);
		PendingProxyBuilds.FindOrAdd(ModelGuid).Add(BuildTask);
	}
}

void UFragmentsImporter::EnqueueShellMeshBuild(
	const FString& SamplePath,
	const Shell* ShellRef,
	const FColor& VertexColor,
	UMaterialInterface* SlotMaterial,
	bool bNeedsProxyData,
	FModelBufferPin BufferPin,
	TFunction<void(FShellBuildResult&&)> OnComplete)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_EnqueueShellMeshBuild);
	check(IsInGameThread());

	if (!ShellRef || !BufferPin.IsValid())
	{
		FShellBuildResult Failed;
		if (OnComplete) { OnComplete(MoveTemp(Failed)); }
		return;
	}

	// Game thread: create and root the mesh shell.
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);

	// Census: this is the primary LOD0 shell-template mesh-creation site, the one essentially every LOD0 template mesh is created at.
	if (CVarCellMergeCensus.GetValueOnGameThread() != 0)
	{
		++CellCensusReport.MeshesBuiltBySite.FindOrAdd(TEXT("EnqueueShellMeshBuild"), 0);
		++CellCensusReport.DistinctSamplePaths.FindOrAdd(SamplePath, 0);
	}

	StaticMesh->bGenerateMeshDistanceField = false;
	StaticMesh->bHasNavigationData = false;
	StaticMesh->bSupportRayTracing = false;
	if (SlotMaterial)
	{
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(SlotMaterial, TEXT("Fragment")));
	}
	StaticMesh->AddToRoot();

	TWeakObjectPtr<UStaticMesh> WeakMesh(StaticMesh);
	TWeakObjectPtr<UFragmentsImporter> WeakThis(this);

	UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[WeakThis, WeakMesh, ShellRef, VertexColor, bNeedsProxyData,
		 BufferPin, SamplePath, OnComplete = MoveTemp(OnComplete)]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ShellBuildOnWorker);

			// BufferPin keeps ShellRef's backing bytes alive for this read.
			TUniquePtr<FMeshDescription> MD = MakeUnique<FMeshDescription>();
			TArray<FVector3f> CollisionPositions;
			TArray<uint32> CollisionIndices;
			FFragmentMeshData ProxyData;

			FragmentShellAuthoring::FShellSource Source;
			bool bAuthored = FragmentShellAuthoring::ReadShellSource(ShellRef, Source);
			if (bAuthored)
			{
				bAuthored = FragmentShellAuthoring::AuthorShellMeshDescription(
					Source, VertexColor, *MD, CollisionPositions, CollisionIndices);
			}

			Chaos::FTriangleMeshImplicitObjectPtr TriMesh;
			if (!bAuthored || CollisionIndices.Num() < 3)
			{
				// Leave MD null; still hop back so WeakMesh is unrooted (avoids a leak).
				MD.Reset();
			}
			else
			{
				if (bNeedsProxyData)
				{
					FragmentShellAuthoring::ExtractMeshData(*MD, VertexColor, ProxyData);
				}
				TriMesh = FragmentTriMeshCook::BuildTriMeshOnWorker(
					MoveTemp(CollisionPositions), MoveTemp(CollisionIndices));
			}

			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, WeakMesh, SamplePath, MD = MoveTemp(MD), TriMesh = MoveTemp(TriMesh),
				 ProxyData = MoveTemp(ProxyData), OnComplete = MoveTemp(OnComplete)]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ShellEnqueue);
					check(IsInGameThread());

					UFragmentsImporter* ImporterPtr = WeakThis.Get();
					if (!ImporterPtr)
					{
						// Importer torn down mid-hop: unroot and drop.
						if (UStaticMesh* M = WeakMesh.Get()) { M->RemoveFromRoot(); }
						return;
					}

					FPendingMeshFinalize Payload;
					Payload.WeakMesh        = WeakMesh;
					Payload.MeshDescriptions.Add(MoveTemp(MD));
					Payload.TriMesh         = MoveTemp(TriMesh);
					Payload.DebugLabel      = FString::Printf(TEXT("ShellMesh:%s"), *SamplePath);
					Payload.EstimatedTriangleCount =
						Payload.MeshDescriptions[0].IsValid() ? Payload.MeshDescriptions[0]->Triangles().Num() : 0;
					// LOD0 per-element shell mesh: SnapshotCellMergeSources reads this mesh's index/vertex buffers back on the CPU when it later merges this element into a cell mesh.
					Payload.bNeedsCpuAccess = true;

					// Declare the waiter-drain cost this payload's own Finalizer triggers via CompleteShellBuild, so the budget gate isn't blind to it.
					if (const FPendingShellBuild* Entry = ImporterPtr->InFlightShellBuilds.Find(SamplePath))
					{
						Payload.ExtraFinalizeCostMs =
							Entry->Waiters.Num() * GFragmentsShellWaiterCostPerWaiterMs;
					}
					Payload.Finalizer =
						[ProxyData = MoveTemp(ProxyData), OnComplete = MoveTemp(OnComplete)]
						(UStaticMesh* BuiltMesh) mutable
						{
							FShellBuildResult Result;
							Result.Mesh = BuiltMesh;
							if (BuiltMesh) { Result.ProxyData = MoveTemp(ProxyData); }
							if (OnComplete) { OnComplete(MoveTemp(Result)); }
						};

					ImporterPtr->DispatchMeshFinalize(MoveTemp(Payload));
				});
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);
	// Per-element tasks cannot use the per-model PendingProxyBuilds registry.
}

void UFragmentsImporter::RecordProxyContribution(
	const FString& ModelGuid, int64 OrderKey, const FString& SamplePath,
	const FTransform& WorldTransform, uint32 MatCRC, UMaterialInterface* Material)
{
	check(IsInGameThread());

	FProxyContribution& C = PendingProxyContributions.FindOrAdd(ModelGuid).AddDefaulted_GetRef();
	C.OrderKey        = OrderKey;
	C.SamplePath      = SamplePath;
	C.WorldTransform  = WorldTransform;
	C.MatCRC          = MatCRC;
	C.Material        = Material;
}

#if WITH_DEV_AUTOMATION_TESTS
TArray<int64> UFragmentsImporter::GetSortedProxyContributionKeysForTests(const FString& ModelGuid) const
{
	TArray<int64> Keys;
	if (const TArray<FProxyContribution>* Found = PendingProxyContributions.Find(ModelGuid))
	{
		Keys.Reserve(Found->Num());
		for (const FProxyContribution& C : *Found) { Keys.Add(C.OrderKey); }
		Keys.Sort();
	}
	return Keys;
}
#endif // WITH_DEV_AUTOMATION_TESTS

TArray<UFragmentsImporter::FResolvedProxyContribution>
UFragmentsImporter::ResolveProxyContributions(
	const TArray<FProxyContribution>& SortedContributions) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ResolveProxyContributions);
	check(IsInGameThread());

	TArray<FResolvedProxyContribution> Payload;
	Payload.Reserve(SortedContributions.Num());

	int32 MissingStash = 0;
	for (const FProxyContribution& C : SortedContributions)
	{
		if (const TSharedPtr<const FFragmentMeshData, ESPMode::ThreadSafe>* Stashed =
				ProxyMeshDataCache.Find(C.SamplePath))
		{
			Payload.Add(FResolvedProxyContribution{ *Stashed, C.WorldTransform, C.MatCRC, C.Material });
		}
		else
		{
			++MissingStash;
		}
	}

	// A miss means an element that WILL be in the building contributes nothing to its LOD2 proxy: a hole in the far silhouette with no other symptom.
	UE_CLOG(MissingStash > 0, LogFragments, Warning,
		TEXT("[HLOD Proxy] %d of %d contributions had no stashed proxy mesh data and were dropped. "
		     "Expect missing geometry in the far LOD."),
		MissingStash, SortedContributions.Num());

	return Payload;
}

TArray<float> UFragmentsImporter::ResolveNearEdgesCm(
	float ModelBoundsRadiusCm, const FString& MulSpec, const FString& AbsSpec)
{
	// Pure and CVar-free: takes the two spec strings as arguments instead of reading r.Fragments.Proxy.{TierNearEdgeRadiiMul,TierNearEdgesCm} itself, so it is directly testable without touching either ECVF_ReadOnly CVar.
	const TArray<float> Muls = ParseNearEdgesCm(MulSpec);
	if (Muls.Num() == 0 || ModelBoundsRadiusCm <= 0.0f)
	{
		// Empty multiplier spec (the CVar default) or a not-yet-known radius: fall back to the pre-existing absolute ladder unchanged.
		if (Muls.Num() > 0)
		{
			UE_LOG(LogFragments, Warning,
				TEXT("[ProxyTiers] r.Fragments.Proxy.TierNearEdgeRadiiMul is set, but "
				     "ModelBoundsRadiusCm=%.2f is non-positive -- falling back to the absolute "
				     "r.Fragments.Proxy.TierNearEdgesCm ladder for this model. If the model's bounds "
				     "radius has not been registered yet, its proxy tiers will re-strand on the "
				     "absolute ladder with no other symptom until a memory gate."),
				ModelBoundsRadiusCm);
		}
		return ParseNearEdgesCm(AbsSpec);
	}

	TArray<float> NearEdges;
	NearEdges.Reserve(Muls.Num());
	for (const float Mul : Muls) { NearEdges.Add(Mul * ModelBoundsRadiusCm); }
	return NearEdges;
}

TArray<float> UFragmentsImporter::ResolveNearEdgesCm(float ModelBoundsRadiusCm)
{
	check(IsInGameThread());   // CVar reads are game-thread only

	return ResolveNearEdgesCm(
		ModelBoundsRadiusCm,
		CVarProxyTierNearEdgeRadiiMul.GetValueOnGameThread(),
		CVarProxyTierNearEdges.GetValueOnGameThread());
}

TArray<FragmentProxyTiers::FTierSpec> UFragmentsImporter::BuildProxyTierSpecs(
	int32 NumTiers, float OmitPx, TConstArrayView<float> NearEdgesCm, float SimplifyRatio)
{
	using namespace FragmentProxyTiers;

	TArray<FTierSpec> Specs;
	// Upper-clamped too: UE cannot represent more than MAX_STATIC_MESH_LODS LODs on one static mesh, so anything past it could never be selected at runtime regardless of how it was derived.
	NumTiers = FMath::Clamp(NumTiers, 1, MAX_STATIC_MESH_LODS);
	Specs.Reserve(NumTiers);

	// Reference view for the cutoff derivation.
	const float Coeff = ViewCoeff(kRefViewportHeightPx, kRefProjM11);

	// Keep deeper tier cutoffs at least as large as shallower cutoffs.
	float PrevCutoff = 0.0f;
	for (int32 t = 0; t < NumTiers; ++t)
	{
		FTierSpec Spec;
		const float Cutoff = (t == 0 || !NearEdgesCm.IsValidIndex(t))
			? PrevCutoff
			: FMath::Max(PrevCutoff, RadiusCutoffCm(OmitPx, NearEdgesCm[t], Coeff));
		Spec.MinRadiusCm = PrevCutoff = Cutoff;
		Spec.SimplifyRatio = SimplifyRatio;
		// Left unresolved here -- see FillProxyTierScreenSizes and this function's header doc comment for why ScreenSize cannot be derived until the proxy's accumulated bounds exist.
		Spec.ScreenSize = kUnresolvedProxyTierScreenSize;
		Specs.Add(Spec);
	}

	// Check tier nesting without aborting a development build.
	if (!ensureMsgf(AreTiersNested(Specs),
		TEXT("BuildProxyTierSpecs produced non-nested tiers -- this should be structurally impossible; ")
		TEXT("proxy LOD selection may flicker as the camera moves")))
	{
		FString NearEdgesStr;
		for (int32 i = 0; i < NearEdgesCm.Num(); ++i)
		{
			NearEdgesStr += (i > 0 ? TEXT(",") : TEXT("")) + FString::SanitizeFloat(NearEdgesCm[i]);
		}
		UE_LOG(LogFragments, Warning,
			TEXT("[HLOD Proxy] Non-nested tier specs from r.Fragments.Proxy.TierNearEdgesCm=\"%s\" ")
			TEXT("(NumTiers=%d, OmitPx=%.2f) -- check that CVar's value"),
			*NearEdgesStr, NumTiers, OmitPx);
	}
	return Specs;
}

void UFragmentsImporter::FillProxyTierScreenSizes(
	TArray<FragmentProxyTiers::FTierSpec>& Specs, TConstArrayView<float> NearEdgesCm, float ProxyBoundsRadiusCm)
{
	// Same M11 reference view as BuildProxyTierSpecs' cutoff derivation, so ScreenSize and MinRadiusCm are two expressions of the same calibration and cannot drift apart.
	const float M11 = FragmentProxyTiers::kRefProjM11;

	float PrevScreenSize = 1.0f;   // clamp ceiling; tier 0 never needs the strictly-decreasing nudge
	for (int32 t = 0; t < Specs.Num(); ++t)
	{
		float ScreenSize;
		if (NearEdgesCm.IsValidIndex(t) && NearEdgesCm[t] > 0.0f)
		{
			ScreenSize = FMath::Clamp(ProxyBoundsRadiusCm * M11 / NearEdgesCm[t], 0.0f, 1.0f);
		}
		else
		{
			// An uncalibrated tier inherits the prior tier’s assigned ScreenSize.
			ScreenSize = (t == 0) ? 1.0f : PrevScreenSize;
		}

		// Force strictly decreasing: UE's LOD selection cannot distinguish two tiers tied at the same ScreenSize, and duplicate/out-of-order near edges (or the inherited-value branch above) can tie them.
		if (t > 0 && ScreenSize >= PrevScreenSize)
		{
			ScreenSize = PrevScreenSize * 0.999f;
		}

		Specs[t].ScreenSize = ScreenSize;
		PrevScreenSize = ScreenSize;
	}
}

int32 UFragmentsImporter::CountUnreachableLeadingTiers(
	TConstArrayView<float> TierScreenSizes, float ProxyBoundsRadiusCm, float PromoteCm)
{
	const int32 NumTiers = TierScreenSizes.Num();
	int32 UnreachableCount = 0;

	// t only ranges over tiers that HAVE a successor -- the last tier is never tested, so it can never be counted (see the declaration's doc comment for why that matters).
	for (int32 t = 0; t < NumTiers - 1; ++t)
	{
		const float NextScreenSize = TierScreenSizes[t + 1];
		if (NextScreenSize <= 0.0f)
		{
			// Cannot evaluate engage() for a degenerate entry -- stop the leading run here rather than divide by zero or treat a zero/negative entry as vacuously unreachable.
			break;
		}

		const float EngageNextCm = FragmentProxyTiers::kRefProjM11 * ProxyBoundsRadiusCm / NextScreenSize;
		if (EngageNextCm > PromoteCm)
		{
			// First reachable tier -- the leading run stops here so surviving tiers are never renumbered.
			break;
		}

		++UnreachableCount;
	}
	return UnreachableCount;
}

TArray<FCellSubLodSpec> UFragmentsImporter::BuildCellSubLodSpecs(
	float NearEdgeCm, float ModelBoundsRadiusCm, float CellBoundsRadiusCm,
	float OmitPx, float GeoCoeffCells, float ThresholdPx, float CoarsenFactor)
{
	using namespace FragmentProxyTiers;

	// Build-time reference view, NOT the live one: baked selection must stay deterministic and view-free.
	const float Coeff = ViewCoeff(kRefViewportHeightPx, kRefProjM11);

	// Sub-LOD 1's near edge is where THIS model demotes to its LOD2 proxy.
	const float DemoteCm = FragmentModelTier::DemoteDistanceCm(
		GeoCoeffCells, ModelBoundsRadiusCm, Coeff, ThresholdPx, CoarsenFactor);

	const float NearEdges[2] = { NearEdgeCm, FMath::Max(DemoteCm, NearEdgeCm) };

	TArray<FCellSubLodSpec> Specs;
	Specs.SetNum(2);

	// Far cell LOD cutoffs cannot fall below near cutoffs.
	float PrevCutoff = 0.0f;
	for (int32 i = 0; i < 2; ++i)
	{
		PrevCutoff = FMath::Max(PrevCutoff, RadiusCutoffCm(OmitPx, NearEdges[i], Coeff));
		Specs[i].MinRadiusCm = PrevCutoff;
	}

	// ScreenSize: reuse the proxy derivation verbatim so there is one implementation of UE's M11 * R / D convention.
	TArray<FTierSpec> Tiers;
	Tiers.SetNum(2);
	FillProxyTierScreenSizes(Tiers, MakeArrayView(NearEdges, 2), CellBoundsRadiusCm);
	Specs[0].ScreenSize = Tiers[0].ScreenSize;
	Specs[1].ScreenSize = Tiers[1].ScreenSize;

	return Specs;
}

float UFragmentsImporter::ResolveCellOmitPx(bool bShellSide, float SharedOmitPx, float InteriorOmitPx)
{
	// Pure and CVar-free: takes both budgets as arguments instead of reading r.Fragments.{Proxy,Interior}.OmitPx itself, so it is directly testable without touching either ECVF_ReadOnly CVar.
	if (bShellSide) { return SharedOmitPx; }

	return (InteriorOmitPx >= 0.0f) ? InteriorOmitPx : SharedOmitPx;
}

float UFragmentsImporter::ResolveCellOmitPx(bool bShellSide)
{
	check(IsInGameThread());   // CVar reads are game-thread only

	return ResolveCellOmitPx(
		bShellSide,
		CVarProxyOmitPx.GetValueOnGameThread(),
		CVarInteriorOmitPx.GetValueOnGameThread());
}

TArray<FragmentProxyTiers::FTierSpec> UFragmentsImporter::ResolveProxyTierSpecs(
	float ModelBoundsRadiusCm) const
{
	check(IsInGameThread());   // CVar reads and the ratio property are game-thread only

	// ModelBoundsRadiusCm must be the SAME value EnqueueProxyMeshBuild's ScreenSizeSpecs re-resolve passes to this same resolver -- see that call site's comment.
	const TArray<float> NearEdges = ResolveNearEdgesCm(ModelBoundsRadiusCm);

	const FragmentProxyDecimate::FDecimateSettings Settings =
		FragmentProxyDecimate::ResolveSettings(
			CVarProxyDecimate.GetValueOnGameThread(),
			CVarProxyDecimateRatio.GetValueOnGameThread(),
			bDecimateProxyMesh,
			ProxyDecimationRatio);

	return BuildProxyTierSpecs(
		CVarProxyTiers.GetValueOnGameThread(),
		CVarProxyOmitPx.GetValueOnGameThread(),
		NearEdges,
		Settings.bEnabled ? Settings.Ratio : 1.0f);
}

void UFragmentsImporter::AccumulateResolvedContributions(
	const TArray<FResolvedProxyContribution>& Payload,
	TConstArrayView<FragmentProxyTiers::FTierSpec> Tiers,
	TArray<FProxyMeshAccumulator>& OutTiers)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_AccumulateResolvedContributions);

	checkSlow(FragmentProxyTiers::AreTiersNested(Tiers));

	// APPEND when OutTiers is already the right shape (see header doc comment) -- only reset/resize on a mismatch, e.g. the first call for a model where OutTiers starts empty.
	const int32 WantedNum = FMath::Max(1, Tiers.Num());
	if (OutTiers.Num() != WantedNum)
	{
		OutTiers.Reset();
		OutTiers.SetNum(WantedNum);
	}

	// Compute one radius per unique mesh template, not per instance.
	TMap<const FFragmentMeshData*, float> LocalRadiusCache;

	// Strict index order -- this loop is the byte-identical guarantee.
	for (const FResolvedProxyContribution& C : Payload)
	{
		if (!C.MeshData.IsValid()) { continue; }

		const FFragmentMeshData* CacheKey = C.MeshData.Get();
		const float* Found = LocalRadiusCache.Find(CacheKey);
		const float LocalRadius = Found
			? *Found
			: LocalRadiusCache.Add(CacheKey, FragmentProxyTiers::LocalRadiusCm(*C.MeshData));

		const float WorldRadius = FragmentProxyTiers::WorldRadiusCm(LocalRadius, C.WorldTransform);

		for (int32 t = 0; t < OutTiers.Num(); ++t)
		{
			const float MinRadiusCm = Tiers.IsValidIndex(t) ? Tiers[t].MinRadiusCm : 0.0f;
			if (WorldRadius >= MinRadiusCm)
			{
				OutTiers[t].AddGeometry(*C.MeshData, C.WorldTransform, C.MatCRC, C.Material);
			}
		}
	}
}

void UFragmentsImporter::AccumulateResolvedContributions(
	const TArray<FResolvedProxyContribution>& Payload,
	FProxyMeshAccumulator& OutAccumulator)
{
	// One all-inclusive tier reproduces the pre-tier behaviour exactly.
	const FragmentProxyTiers::FTierSpec KeepEverything{ 0.0f, 1.0f, 1.0f };
	TArray<FProxyMeshAccumulator> Tiers;
	AccumulateResolvedContributions(Payload, MakeArrayView(&KeepEverything, 1), Tiers);
	if (Tiers.Num() > 0)
	{
		OutAccumulator = MoveTemp(Tiers[0]);
	}
}

/** The one replay order. */
static void SortProxyContributions(TArray<UFragmentsImporter::FProxyContribution>& Contributions)
{
	Contributions.Sort([](const UFragmentsImporter::FProxyContribution& A,
	                      const UFragmentsImporter::FProxyContribution& B)
	{
		return A.OrderKey < B.OrderKey;
	});
}

void UFragmentsImporter::FlushProxyContributions(const FString& ModelGuid)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_FlushProxyContributions);
	check(IsInGameThread());

	TArray<FProxyContribution> Contributions;
	if (!PendingProxyContributions.RemoveAndCopyValue(ModelGuid, Contributions))
	{
		return;
	}

	// Stable replay order regardless of which worker finished first.
	SortProxyContributions(Contributions);

	// Same resolve + strict-index-order replay the async path runs, so the synchronous fallback and the worker cannot drift apart.
	const TArray<FResolvedProxyContribution> Payload = ResolveProxyContributions(Contributions);

	// Create a proxy accumulator only after finding geometry.
	if (Payload.Num() > 0)
	{
		// Same R FragmentModelTier's promote/demote math uses for this model -- see ModelBoundsRadiusCmForGuid's comment.
		const float ModelBoundsRadiusCm = ModelBoundsRadiusCmForGuid(RegistrationManager, ModelGuid);
		AccumulateResolvedContributions(
			Payload, ResolveProxyTierSpecs(ModelBoundsRadiusCm), ProxyAccumulators.FindOrAdd(ModelGuid));
	}
}

// Flush proxy contributions off the game thread.

void UFragmentsImporter::LaunchProxyContributionFlush(const FString& ModelGuid, AFragment* BucketRoot)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_LaunchProxyContributionFlush);
	check(IsInGameThread());

	TArray<FProxyContribution> Contributions;
	PendingProxyContributions.RemoveAndCopyValue(ModelGuid, Contributions);

	SortProxyContributions(Contributions);

	TArray<FResolvedProxyContribution> Payload = ResolveProxyContributions(Contributions);

	// Nothing to accumulate: skip the worker hop entirely and finalize inline, so the existing "no accumulated geometry" path still reports.
	if (Payload.Num() == 0)
	{
		FinalizeProxyMesh(ModelGuid, BucketRoot);
		return;
	}

	// Same lookup FlushProxyContributions makes -- see ModelBoundsRadiusCmForGuid's comment.
	const float ModelBoundsRadiusCm = ModelBoundsRadiusCmForGuid(RegistrationManager, ModelGuid);
	const TArray<FragmentProxyTiers::FTierSpec> Tiers = ResolveProxyTierSpecs(ModelBoundsRadiusCm);

	if (!GFragmentsProxyFlushAsync)
	{
		// Rollback / A-B path: accumulate synchronously on the game thread.
		TArray<FProxyMeshAccumulator> SyncTiers;
		AccumulateResolvedContributions(Payload, Tiers, SyncTiers);
		if (SyncTiers.Num() > 0 && !SyncTiers[0].IsEmpty())
		{
			ProxyAccumulators.Add(ModelGuid, MoveTemp(SyncTiers));
		}
		FinalizeProxyMesh(ModelGuid, BucketRoot);
		return;
	}

	const uint32 LaunchEpoch = ProxyFlushEpoch.FindOrAdd(ModelGuid);
	TWeakObjectPtr<UFragmentsImporter> WeakThis(this);
	TWeakObjectPtr<AFragment> WeakBucketRoot(BucketRoot);
	const FString ModelGuidCopy = ModelGuid;

	UE::Tasks::FTask FlushTask = UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[WeakThis, WeakBucketRoot, ModelGuidCopy, LaunchEpoch, Work = MoveTemp(Payload), Tiers]() mutable
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ProxyFlushOnWorker);

			TArray<FProxyMeshAccumulator> AccumTiers;
			AccumulateResolvedContributions(Work, Tiers, AccumTiers);

			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, WeakBucketRoot, ModelGuidCopy, LaunchEpoch, AccumTiers = MoveTemp(AccumTiers)]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_ProxyFlushFinalize);
					check(IsInGameThread());

					UFragmentsImporter* ImporterPtr = WeakThis.Get();
					if (!ImporterPtr)
					{
						return; // importer torn down between worker completion and this hop
					}

					// Stale epoch => this model was purged (or reimported) while the worker ran.
					if (ImporterPtr->ProxyFlushEpoch.FindOrAdd(ModelGuidCopy) != LaunchEpoch)
					{
						UE_LOG(LogFragments, Verbose,
							TEXT("[HLOD Proxy] Discarding stale proxy flush for model %s"),
							*ModelGuidCopy);
						return;
					}

					// Only publish a non-empty accumulator: an unconditional Add would leave a stray empty entry that FinalizeProxyMesh's IsEmpty() early-out never removes.
					if (AccumTiers.Num() > 0 && !AccumTiers[0].IsEmpty())
					{
						ImporterPtr->ProxyAccumulators.Add(ModelGuidCopy, MoveTemp(AccumTiers));
#if WITH_DEV_AUTOMATION_TESTS
						// TEST-ONLY.
						++ImporterPtr->ProxyFlushPublishCountForTests;
#endif
					}

					ImporterPtr->FinalizeProxyMesh(ModelGuidCopy, WeakBucketRoot.Get());
				});
		},
		// Run proxy accumulation at background priority behind shell builds.
		UE::Tasks::ETaskPriority::BackgroundNormal);

	{
		FScopeLock Lock(&PendingProxyBuildsLock);
		PendingProxyBuilds.FindOrAdd(ModelGuid).Add(FlushTask);
	}
}


// FinalizeProxyMesh launches the async LOD2 proxy build.

void UFragmentsImporter::FinalizeProxyMesh(const FString& ModelGuid, AFragment* BucketRoot)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_FinalizeProxyMesh);

	// Always a no-op now, not the primary path.
	FlushProxyContributions(ModelGuid);

	// Runtime proxy tier selection uses each tier’s ScreenSize.
	TArray<FProxyMeshAccumulator>* AccumulatorTiers = ProxyAccumulators.Find(ModelGuid);
	FProxyMeshAccumulator* Accumulator = (AccumulatorTiers && AccumulatorTiers->Num() > 0)
		? &(*AccumulatorTiers)[0]
		: nullptr;
	if (!Accumulator || Accumulator->IsEmpty())
	{
		UE_LOG(LogFragments, Warning, TEXT("[HLOD Proxy] No accumulated geometry for model %s"), *ModelGuid);
		return;
	}

	const int32 PreDecimVerts = Accumulator->TotalVertexCount();
	const int32 PreDecimTris = Accumulator->TotalTriangleCount();

	UE_LOG(LogFragments, Verbose, TEXT("[HLOD Proxy] Finalizing proxy for model %s: %d sections, %d verts, %d tris"),
		*ModelGuid, Accumulator->Sections.Num(), PreDecimVerts, PreDecimTris);

	// Capture data needed by the completion lambda (we may have moved on by then).
	TWeakObjectPtr<AFragment> WeakBucketRoot(BucketRoot);
	const FString ModelGuidCopy = ModelGuid;
	// Weak ref: WaitForAllProxyBuilds drains the outer worker FTask but not the fire-and-forget AsyncTask(GameThread) finalize that follows.
	TWeakObjectPtr<UFragmentRegistrationManager> WeakRegMgr(RegistrationManager);

	// Re-resolve proxy settings on the game thread using the same model bounds radius.
	const float ModelBoundsRadiusCm = ModelBoundsRadiusCmForGuid(RegistrationManager, ModelGuid);
	const TArray<FragmentProxyTiers::FTierSpec> TierSpecs = ResolveProxyTierSpecs(ModelBoundsRadiusCm);

	EnqueueProxyMeshBuild(ModelGuid, *AccumulatorTiers, TierSpecs, ModelBoundsRadiusCm,
		[WeakBucketRoot, ModelGuidCopy, WeakRegMgr](UStaticMesh* ProxyMesh)
		{
			check(IsInGameThread());

			AFragment* Root = WeakBucketRoot.Get();
			if (!Root || !ProxyMesh)
			{
				if (!ProxyMesh)
				{
					UE_LOG(LogFragments, Warning,
						TEXT("[HLOD Proxy] Async build returned null for model %s; skipping component attach"),
						*ModelGuidCopy);
				}
				return;
			}

			// Skip component creation if registration ended before finalization.
			UFragmentRegistrationManager* RegMgr = WeakRegMgr.Get();
			if (!RegMgr)
			{
				UE_LOG(LogFragments, Verbose,
					TEXT("[HLOD Proxy] RegistrationManager gone for model %s; skipping component build"),
					*ModelGuidCopy);
				return;
			}

			// Build and attach the proxy component (was previously inline in FinalizeProxyMesh).
			UStaticMeshComponent* ProxyComp = NewObject<UStaticMeshComponent>(Root);
			ProxyComp->SetStaticMesh(ProxyMesh);
			ProxyComp->SetMobility(EComponentMobility::Stationary);
			ProxyComp->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
			ProxyComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			ProxyComp->SetCollisionResponseToAllChannels(ECR_Overlap);
			ProxyComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			ProxyComp->SetCastShadow(true);
			ProxyComp->bAffectDistanceFieldLighting = false;
			ProxyComp->ComponentTags.Add(FName(TEXT("HLODProxy")));
			ProxyComp->AttachToComponent(Root->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
			ProxyComp->RegisterComponent();

			const int32 NumMaterials = ProxyComp->GetNumMaterials();
			for (int32 SlotIdx = 0; SlotIdx < NumMaterials; SlotIdx++)
			{
				ProxyComp->CreateAndSetMaterialInstanceDynamic(SlotIdx);
			}

			ProxyComp->RecreatePhysicsState();

			if (ProxyComp->GetBodyInstance() && ProxyComp->GetBodyInstance()->IsValidBodyInstance())
			{
				UE_LOG(LogFragments, Verbose, TEXT("[HLOD Proxy] Collision body active for model %s"), *ModelGuidCopy);
			}

			// Hand the proxy to the registration manager for per-frame streaming.
			RegMgr->AttachProxyComponent(Root, ProxyComp);
		});

	// Accumulator is consumed; remove from the per-model map so a re-import can repopulate from scratch.
	ProxyAccumulators.Remove(ModelGuid);
}

// Build shell mesh data for the single-element load path.

FFragmentMeshData UFragmentsImporter::BuildShellMeshDataFromFlatBuffers(
	const Shell* ShellRef,
	const Material* RefMaterial)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragImporter_BuildShellMeshData);
	FFragmentMeshData Result;

	FragmentShellAuthoring::FShellSource Source;
	if (!FragmentShellAuthoring::ReadShellSource(ShellRef, Source))
	{
		UE_LOG(LogFragments, Warning,
			TEXT("BuildShellMeshDataFromFlatBuffers: null shell, or shell with no points/profiles"));
		return Result;
	}

	// Untextured elements keep the neutral grey this path has always defaulted to; the streaming path never hits this case (its material is resolved up front).
	const FColor VertexColor = RefMaterial
		? FColor(RefMaterial->r(), RefMaterial->g(), RefMaterial->b(), RefMaterial->a())
		: FColor(200, 200, 200, 255);

	// Collision geometry is discarded here: this path feeds FFragmentMeshData into the SetRenderData commit, which cooks no trimesh (only the async shell build does, via EnqueueShellMeshBuild).
	FMeshDescription MD;
	TArray<FVector3f> UnusedCollisionPositions;
	TArray<uint32> UnusedCollisionIndices;
	FragmentShellAuthoring::FShellAuthorStats Stats;
	const bool bAuthored = FragmentShellAuthoring::AuthorShellMeshDescription(
		Source, VertexColor, MD, UnusedCollisionPositions, UnusedCollisionIndices, &Stats);

	if (Stats.FailedProfiles > 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("BuildShellMeshDataFromFlatBuffers: TESS_SUMMARY failed=%d/%d profiles"),
			Stats.FailedProfiles, Stats.TotalProfiles);
	}
	if (Stats.SkippedDegenerateTriangles > 0)
	{
		UE_LOG(LogFragments, Verbose, TEXT("BuildShellMeshDataFromFlatBuffers: skipped %d degenerate tris"),
			Stats.SkippedDegenerateTriangles);
	}
	if (!bAuthored)
	{
		UE_LOG(LogFragments, Warning, TEXT("BuildShellMeshDataFromFlatBuffers: ALL profiles failed"));
		return Result;
	}

	FragmentShellAuthoring::ExtractMeshData(MD, VertexColor, Result);
	return Result;
}

UStaticMesh* UFragmentsImporter::CreateStaticMeshFromCircleExtrusion(const CircleExtrusion* CircleExtrusion, const Material* RefMaterial, const FString& AssetName, UObject* OuterRef, const FString& InModelGuid)
{
	if (!CircleExtrusion || !CircleExtrusion->axes() || CircleExtrusion->axes()->size() == 0)
		return nullptr;

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(OuterRef, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
	StaticMesh->bGenerateMeshDistanceField = false;
	StaticMesh->bHasNavigationData = false;
	StaticMesh->InitResources();
	StaticMesh->SetLightingGuid();

	TArray<const FMeshDescription*> MeshDescriptionPtrs;

	// Build Settings
#if WITH_EDITOR
	{
		FStaticMeshSourceModel& SrcModel = StaticMesh->AddSourceModel();
		SrcModel.BuildSettings.bRecomputeNormals = true;
		SrcModel.BuildSettings.bRecomputeTangents = true;
		SrcModel.BuildSettings.bRemoveDegenerates = true;
		SrcModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
		SrcModel.BuildSettings.bBuildReversedIndexBuffer = true;
		SrcModel.BuildSettings.bUseFullPrecisionUVs = false;
		SrcModel.BuildSettings.bGenerateLightmapUVs = false;
		SrcModel.BuildSettings.SrcLightmapIndex = 0;
		SrcModel.BuildSettings.DstLightmapIndex = 1;
		SrcModel.BuildSettings.MinLightmapResolution = 64;
		SrcModel.BuildSettings.DistanceFieldResolutionScale = 0.0f;
		SrcModel.BuildSettings.MaxLumenMeshCards = 0;
		SrcModel.BuildSettings.bComputeWeightedNormals = true;
	}
#endif

	// LOD0 – Full circle extrusion
	UStaticMeshDescription* LOD0Desc = StaticMesh->CreateStaticMeshDescription(OuterRef);
	BuildFullCircleExtrusion(*LOD0Desc, CircleExtrusion, RefMaterial, StaticMesh);
	MeshDescriptionPtrs.Add(&LOD0Desc->GetMeshDescription());

	{
	}

	// Build mesh
	UStaticMesh::FBuildMeshDescriptionsParams MeshParams;
	MeshParams.bBuildSimpleCollision = false;
	MeshParams.bCommitMeshDescription = true;
	MeshParams.bMarkPackageDirty = true;
	MeshParams.bUseHashAsGuid = false;
	// bFastBuild intentionally NOT set — it skips collision cooking and normal/tangent processing, breaking selection and visual quality.

	StaticMesh->BuildFromMeshDescriptions(MeshDescriptionPtrs, MeshParams);

	return StaticMesh;
}

FName UFragmentsImporter::AddMaterialToMesh(UStaticMesh*& CreatedMesh, const Material* RefMaterial, const FString& InModelGuid)
{
	if (!RefMaterial || !CreatedMesh) return FName();

	float R = RefMaterial->r() / 255.f;
	float G = RefMaterial->g() / 255.f;
	float B = RefMaterial->b() / 255.f;
	float A = RefMaterial->a() / 255.f;
	bool bHasTransparency = A < 1.0f;

	UMaterialInterface* BaseMat = bHasTransparency ? BaseGlassMaterial.Get() : BaseMaterial.Get();
	if (!BaseMat)
	{
		UE_LOG(LogFragments, Error, TEXT("Unable to load Base Material"));
		return FName();
	}

#if WITH_EDITOR
	// Editor + bSaveMeshes: create UMaterialInstanceConstant for disk persistence
	if (bCurrentSaveMeshes)
	{
		FString MaterialName = FString::Printf(TEXT("mat_%d_%d_%d_%d"),
			FMath::RoundToInt(R * 255), FMath::RoundToInt(G * 255),
			FMath::RoundToInt(B * 255), FMath::RoundToInt(A * 255));
		FString PackagePath = TEXT("/Game/Buildings") / MaterialName;
		FString UniquePackageName = FPackageName::ObjectPathToPackageName(PackagePath);
		FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());
		const FString SamplePath = PackagePath + TEXT(".") + MaterialName;

		UMaterialInstanceConstant* MaterialInstance = nullptr;
		if (MaterialsCache.Contains(SamplePath))
		{
			MaterialInstance = MaterialsCache[SamplePath];
		}
		else if (FPaths::FileExists(PackageFileName))
		{
			UPackage* ExistingPackage = LoadPackage(nullptr, *PackagePath, LOAD_None);
			if (ExistingPackage)
			{
				MaterialInstance = FindObject<UMaterialInstanceConstant>(ExistingPackage, *MaterialName);
			}
		}
		else
		{
			UPackage* MaterialPackage = CreatePackage(*PackagePath);
			MaterialInstance = NewObject<UMaterialInstanceConstant>(UMaterialInstanceConstant::StaticClass());
			MaterialInstance->SetParentEditorOnly(BaseMat);
			MaterialInstance->SetVectorParameterValueEditorOnly(TEXT("BaseColor"), FLinearColor(R, G, B, A));
			if (bHasTransparency)
			{
				MaterialInstance->SetScalarParameterValueEditorOnly(TEXT("Opacity"), A);
			}

			if (!FPaths::FileExists(PackageFileName))
			{
				MaterialPackage->FullyLoad();
				MaterialInstance->Rename(*MaterialName, MaterialPackage);
				MaterialInstance->SetFlags(RF_Public | RF_Standalone);
				MaterialPackage->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(MaterialInstance);
				PackagesToSave.Add(MaterialPackage);
			}

			MaterialsCache.Add(SamplePath, MaterialInstance);
		}

		return CreatedMesh->AddMaterial(MaterialInstance);
	}
#endif

	// Dynamic material path: used at runtime and in editor when bSaveMeshes=false
	UMaterialInstanceDynamic* DynamicMaterial = GetPooledMaterial(
		RefMaterial->r(), RefMaterial->g(), RefMaterial->b(), RefMaterial->a(), bHasTransparency);
	if (!DynamicMaterial)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to get pooled material"));
		return FName();
	}

	return CreatedMesh->AddMaterial(DynamicMaterial);
}

void UFragmentsImporter::BuildFullCircleExtrusion(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion, const Material* RefMaterial, UStaticMesh* StaticMesh)
{
	FStaticMeshAttributes Attributes(StaticMeshDescription.GetMeshDescription());
	Attributes.Register();

	FName MaterialSlotName = AddMaterialToMesh(StaticMesh, RefMaterial);
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription.CreatePolygonGroup();
	StaticMeshDescription.SetPolygonGroupMaterialSlotName(PolygonGroupId, MaterialSlotName);

	FMeshDescription& MeshDescription = StaticMeshDescription.GetMeshDescription();

	const auto* Axes = CircleExtrusion->axes();
	const auto* Radii = CircleExtrusion->radius();
	int32 SegmentCount = 16;

	for (flatbuffers::uoffset_t axisIndex = 0; axisIndex < Axes->size(); ++axisIndex)
	{
		const auto* Axis = Axes->Get(axisIndex);
		const auto* Orders = Axis->order();
		const auto* Parts = Axis->parts();
		const auto* Wires = Axis->wires();
		const auto* WireSets = Axis->wire_sets();

		for (flatbuffers::uoffset_t i = 0; i < Orders->size(); i++)
		{
			int32 OrderIndex = Orders->Get(i);
			int32 PartIndex = Parts->Get(i);

			TArray<TArray<FVertexID>> RingVertexIDs;

			// Handle CIRCLE_CURVE
			if (Axis->circle_curves() && PartIndex == (int)AxisPartClass::AxisPartClass_CIRCLE_CURVE)
			{
				const auto* Curves = Axis->circle_curves();
				const float Radius = Radii->Get(axisIndex) * 100.0f;

				TArray<FVector> ArcCenters;
				TArray<FVector> ArcTangents;

				// Sample arc
				for (flatbuffers::uoffset_t c = 0; c < Curves->size(); ++c)
				{
					const auto* Circle = Curves->Get(c);
					FVector Center = FVector(Circle->position().x(), Circle->position().z(), Circle->position().y()) * 100.0f;
					FVector XDir = FVector(Circle->x_direction().x(), Circle->x_direction().z(), Circle->x_direction().y());
					FVector YDir = FVector(Circle->y_direction().x(), Circle->y_direction().z(), Circle->y_direction().y());
					float ApertureRad = FMath::DegreesToRadians(Circle->aperture());
					float ArcRadius = Circle->radius() * 100.0f;

					int32 ArcDivs = FMath::Clamp(FMath::RoundToInt(ApertureRad * ArcRadius * 0.05f), 4, 32);
					for (int32 j = 0; j <= ArcDivs; ++j)
					{
						float t = static_cast<float>(j) / ArcDivs;
						float angle = -ApertureRad / 2.0f + t * ApertureRad;
						FVector Pos = Center + ArcRadius * (FMath::Cos(angle) * XDir + FMath::Sin(angle) * YDir);
						ArcCenters.Add(Pos);
					}
				}

				// Compute tangents
				for (int32 j = 0; j < ArcCenters.Num(); ++j)
				{
					if (j == 0)
						ArcTangents.Add((ArcCenters[1] - ArcCenters[0]).GetSafeNormal());
					else if (j == ArcCenters.Num() - 1)
						ArcTangents.Add((ArcCenters.Last() - ArcCenters[j - 1]).GetSafeNormal());
					else
						ArcTangents.Add((ArcCenters[j + 1] - ArcCenters[j - 1]).GetSafeNormal());
				}

				// Initial frame from first tangent
				FVector PrevTangent = ArcTangents[0];
				FVector PrevX, PrevY;
				PrevTangent.FindBestAxisVectors(PrevX, PrevY);

				TArray<TArray<FVertexID>> AllRings;

				for (int32 k = 0; k < ArcCenters.Num(); ++k)
				{
					const FVector& Tangent = ArcTangents[k];
					FQuat AlignQuat = FQuat::FindBetweenNormals(PrevTangent, Tangent);
					FVector CurrX = AlignQuat.RotateVector(PrevX);
					FVector CurrY = AlignQuat.RotateVector(PrevY);

					TArray<FVertexID> Ring;
					for (int32 j = 0; j < SegmentCount; ++j)
					{
						float Angle = 2.0f * PI * j / SegmentCount;
						FVector Offset = FMath::Cos(Angle) * CurrX + FMath::Sin(Angle) * CurrY;
						FVector Pos = ArcCenters[k] + Offset * Radius;

						FVertexID V = StaticMeshDescription.CreateVertex();
						StaticMeshDescription.SetVertexPosition(V, Pos);
						Ring.Add(V);
					}

					AllRings.Add(Ring);
					PrevTangent = Tangent;
					PrevX = CurrX;
					PrevY = CurrY;
				}

				// Stitch rings
				for (int32 k = 0; k < AllRings.Num() - 1; ++k)
				{
					const auto& RingA = AllRings[k];
					const auto& RingB = AllRings[k + 1];

					for (int32 j = 0; j < SegmentCount; ++j)
					{
						int32 Next = (j + 1) % SegmentCount;

						FVertexID V00 = RingA[j];
						FVertexID V01 = RingB[j];
						FVertexID V10 = RingA[Next];
						FVertexID V11 = RingB[Next];

						TArray<FVertexInstanceID> Tri1 = {
							MeshDescription.CreateVertexInstance(V00),
							MeshDescription.CreateVertexInstance(V01),
							MeshDescription.CreateVertexInstance(V10)
						};
						TArray<FVertexInstanceID> Tri2 = {
							MeshDescription.CreateVertexInstance(V10),
							MeshDescription.CreateVertexInstance(V01),
							MeshDescription.CreateVertexInstance(V11)
						};

						MeshDescription.CreatePolygon(PolygonGroupId, Tri1);
						MeshDescription.CreatePolygon(PolygonGroupId, Tri2);
					}
				}
			}

			else if (Wires && PartIndex == (int)AxisPartClass::AxisPartClass_WIRE)
			{
				// Handle Wire
				const auto* Wire = Wires->Get(OrderIndex);
				FVector P1 = FVector(Wire->p1().x(), Wire->p1().z(), Wire->p1().y()) * 100.0f;
				FVector P2 = FVector(Wire->p2().x(), Wire->p2().z(), Wire->p2().y()) * 100.0f;

				FVector Direction = (P2 - P1).GetSafeNormal();
				FVector XDir, YDir;
				Direction.FindBestAxisVectors(XDir, YDir);

				TArray<FVertexID> Ring1, Ring2;

				for (int32 j = 0; j < SegmentCount; j++)
				{
					float Angle = 2.0f * PI * j / SegmentCount;
					FVector Offset = FMath::Cos(Angle) * XDir + FMath::Sin(Angle) * YDir;
					FVector V1 = P1 + Offset * Radii->Get(OrderIndex) * 100.0f;
					FVector V2 = P2 + Offset * Radii->Get(OrderIndex) * 100.0f;

					FVertexID ID1 = StaticMeshDescription.CreateVertex();
					FVertexID ID2 = StaticMeshDescription.CreateVertex();

					StaticMeshDescription.SetVertexPosition(ID1, V1);
					StaticMeshDescription.SetVertexPosition(ID2, V2);

					Ring1.Add(ID1);
					Ring2.Add(ID2);
				}

				for (int32 j = 0; j < SegmentCount; ++j)
				{
					int32 Next = (j + 1) % SegmentCount;

					FVertexID V00 = Ring1[j];
					FVertexID V01 = Ring2[j];
					FVertexID V10 = Ring1[Next];
					FVertexID V11 = Ring2[Next];

					TArray<FVertexInstanceID> Tri1 = {
						StaticMeshDescription.CreateVertexInstance(V00),
						StaticMeshDescription.CreateVertexInstance(V01),
						StaticMeshDescription.CreateVertexInstance(V10)
					};

					TArray<FVertexInstanceID> Tri2 = {
						StaticMeshDescription.CreateVertexInstance(V10),
						StaticMeshDescription.CreateVertexInstance(V01),
						StaticMeshDescription.CreateVertexInstance(V11)
					};

					StaticMeshDescription.GetMeshDescription().CreatePolygon(PolygonGroupId, Tri1);
					StaticMeshDescription.GetMeshDescription().CreatePolygon(PolygonGroupId, Tri2);
				}

				return;
			}

			else if (WireSets && PartIndex == (int)AxisPartClass::AxisPartClass_WIRE_SET)
			{
				const auto* WSet = WireSets->Get(OrderIndex);
				const auto* Points = WSet->ps();

				if (!Points || Points->size() < 2)
					continue;

				TArray<TArray<FVertexID>> Rings;

				for (flatbuffers::uoffset_t p = 0; p < Points->size(); p++)
				{
					const auto& Pt = Points->Get(p);
					FVector Pos = FVector(Pt->x(), Pt->z(), Pt->y()) * 100.0f;

					// compute Tangent along polyline
					FVector Tangent;
					if (p == 0)
					{
						const auto& Next = Points->Get(p + 1);
						Tangent = (FVector(Next->x(), Next->z(), Next->y()) * 100.0f - Pos).GetSafeNormal();
					}
					else if (p == Points->size() - 1)
					{
						const auto& Prev = Points->Get(p - 1);
						Tangent = (Pos - FVector(Prev->x(), Prev->z(), Prev->y()) * 100.0f).GetSafeNormal();
					}
					else
					{
						const auto& Prev = Points->Get(p - 1);
						const auto& Next = Points->Get(p + 1);
						Tangent = (FVector(Next->x(), Next->z(), Next->y()) * 100.0f - FVector(Prev->x(), Prev->z(), Prev->y()) * 100.0f).GetSafeNormal();
					}

					// Frame
					FVector XDir, YDir;
					Tangent.FindBestAxisVectors(XDir, YDir);

					TArray<FVertexID> Ring;
					for (int32 j = 0; j < SegmentCount; j++)
					{
						float Angle = 2.0f * PI * j / SegmentCount;
						FVector Offset = FMath::Cos(Angle) * XDir + FMath::Sin(Angle) * YDir;
						FVector RingPos = Pos + Offset * Radii->Get(OrderIndex) * 100.0f;

						FVertexID Vtx = StaticMeshDescription.CreateVertex();
						StaticMeshDescription.SetVertexPosition(Vtx, RingPos);
						Ring.Add(Vtx);
					}

					// Connect rings
					for (int32 k = 0; k < Rings.Num() - 1; ++k)
					{
						const auto& RingA = Rings[k];
						const auto& RingB = Rings[k + 1];

						for (int32 j = 0; j < SegmentCount; ++j)
						{
							int32 Next = (j + 1) % SegmentCount;

							FVertexID V00 = RingA[j];
							FVertexID V01 = RingB[j];
							FVertexID V10 = RingA[Next];
							FVertexID V11 = RingB[Next];

							TArray<FVertexInstanceID> Tri1 = {
								MeshDescription.CreateVertexInstance(V00),
								MeshDescription.CreateVertexInstance(V01),
								MeshDescription.CreateVertexInstance(V10)
							};

							TArray<FVertexInstanceID> Tri2 = {
								MeshDescription.CreateVertexInstance(V10),
								MeshDescription.CreateVertexInstance(V01),
								MeshDescription.CreateVertexInstance(V11)
							};

							MeshDescription.CreatePolygon(PolygonGroupId, Tri1);
							MeshDescription.CreatePolygon(PolygonGroupId, Tri2);
						}
					}
				}
			}
		}
	}

	FragmentShellAuthoring::ComputePerFacePlanarUVs(StaticMeshDescription.GetMeshDescription(), 0.01f);
	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(StaticMeshDescription.GetMeshDescription());
	FStaticMeshOperations::ComputeTangentsAndNormals(StaticMeshDescription.GetMeshDescription(), EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
}

void UFragmentsImporter::BuildLineOnlyMesh(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion)
{
	FStaticMeshAttributes Attributes(StaticMeshDescription.GetMeshDescription());
	Attributes.Register();
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription.CreatePolygonGroup();
	FMeshDescription& MeshDescription = StaticMeshDescription.GetMeshDescription();

	const auto* Axes = CircleExtrusion->axes();
	if (!Axes) return;

	for (auto Axis : *Axes)
	{
		const auto* Orders = Axis->order();
		const auto* Wires = Axis->wires();

		for (uint32 i = 0; i < Orders->size(); ++i)
		{
			const auto* Wire = Wires->Get(Orders->Get(i));
			FVector P1 = FVector(Wire->p1().x(), Wire->p1().z(), Wire->p1().y()) * 100.0f;
			FVector P2 = FVector(Wire->p2().x(), Wire->p2().z(), Wire->p2().y()) * 100.0f;

			FVertexID V0 = StaticMeshDescription.CreateVertex();
			FVertexID V1 = StaticMeshDescription.CreateVertex();
			StaticMeshDescription.SetVertexPosition(V0, P1);
			StaticMeshDescription.SetVertexPosition(V1, P2);

			TArray<FVertexInstanceID> Tri;
			FVertexInstanceID I0 = StaticMeshDescription.CreateVertexInstance(V0);
			FVertexInstanceID I1 = StaticMeshDescription.CreateVertexInstance(V1);
			FVertexInstanceID I2 = StaticMeshDescription.CreateVertexInstance(V1); // degenerate triangle
			Tri.Add(I0);
			Tri.Add(I1);
			Tri.Add(I2);

			StaticMeshDescription.GetMeshDescription().CreatePolygon(PolygonGroupId, Tri,{});
		}
	}
}

TArray<FVector> UFragmentsImporter::SampleRingPoints(const FVector& Center, const FVector XDir, const FVector& YDir, float Radius, int SegmentCount, float ApertureRadians)
{
	TArray<FVector> Ring;
	for (int32 i = 0; i <= SegmentCount; i++)
	{
		float t = (float)i / SegmentCount;
		float angle = -ApertureRadians / 2.0f + t * ApertureRadians;
		FVector Point = Center + Radius * (FMath::Cos(angle) * XDir + FMath::Sin(angle) * YDir);
		Ring.Add(Point);
	}
	return Ring;
}

void UFragmentsImporter::SavePackagesWithProgress(const TArray<UPackage*>& InPackagesToSave)
{
#if WITH_EDITOR
	if (PackagesToSave.Num() == 0)
		return;

	// Create a slow task with progress bar and allow cancel
	FScopedSlowTask SlowTask((float)PackagesToSave.Num(), FText::FromString(TEXT("Saving Static Meshes...")));
	SlowTask.MakeDialog(true);

	for (UPackage* Package : PackagesToSave)
	{
		if (SlowTask.ShouldCancel())
		{
			UE_LOG(LogFragments, Warning, TEXT("User canceled saving packages."));
			break;
		}

		FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.SaveFlags = RF_Public | RF_Standalone;

		bool bSuccess = UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs);
		if (!bSuccess)
		{
			UE_LOG(LogFragments, Error, TEXT("Failed to save package: %s"), *Package->GetName());
		}
		else
		{
			UE_LOG(LogFragments, Log, TEXT("Saved package: %s"), *Package->GetName());
		}

		SlowTask.EnterProgressFrame(1.f);
	}
#else
	// Runtime: do not save packages, just log
	UE_LOG(LogFragments, Log, TEXT("Skipping package saving in runtime environment."));
#endif
}

// Material pooling.

uint32 UFragmentsImporter::HashMaterialProperties(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass) const
{
	uint32 Hash = 0;
	Hash = HashCombine(Hash, GetTypeHash(R));
	Hash = HashCombine(Hash, GetTypeHash(G));
	Hash = HashCombine(Hash, GetTypeHash(B));
	Hash = HashCombine(Hash, GetTypeHash(A));
	Hash = HashCombine(Hash, GetTypeHash(bIsGlass));
	return Hash;
}

UMaterialInstanceDynamic* UFragmentsImporter::GetPooledMaterial(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass)
{
	uint32 Hash = HashMaterialProperties(R, G, B, A, bIsGlass);

	if (UMaterialInstanceDynamic** Found = MaterialPool.Find(Hash))
	{
		return *Found;
	}

	// Create new material instance
	UMaterialInterface* BaseMat = bIsGlass ? BaseGlassMaterial.Get() : BaseMaterial.Get();
	if (!BaseMat)
	{
		// Load materials if not already loaded
		BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));
		BaseMat = bIsGlass ? BaseGlassMaterial.Get() : BaseMaterial.Get();
	}

	if (!BaseMat)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to load base material for pooling"));
		return nullptr;
	}

	UMaterialInstanceDynamic* NewMat = UMaterialInstanceDynamic::Create(BaseMat, this);
	if (!NewMat)
	{
		return nullptr;
	}

	float Rf = R / 255.f;
	float Gf = G / 255.f;
	float Bf = B / 255.f;
	float Af = A / 255.f;

	if (Af < 1.0f)
	{
		NewMat->SetScalarParameterValue(TEXT("Opacity"), Af);
	}
	NewMat->SetVectorParameterValue(TEXT("BaseColor"), FVector4(Rf, Gf, Bf, Af));

	MaterialPool.Add(Hash, NewMat);
	return NewMat;
}

void UFragmentsImporter::SuppressImportOverhead()
{
	auto SetCVar = [](const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			CVar->Set(Value, ECVF_SetByCode);
		}
	};

	// Do NOT toggle r.GenerateMeshDistanceFields/r.DynamicGlobalIlluminationMethod: flipping them mid-import races the DFAO pass (Global Distance Field teardown mid-frame); DF cost is suppressed per-mesh/component instead.
	SetCVar(TEXT("r.Lumen.Reflections.Allow"), 0);
	SetCVar(TEXT("gc.TimeBetweenPurgingPendingKillObjects"), 300);

	// Belt: one-shot timer reset ensures GC can't fire even if a spawn tick is skipped.
	if (GEngine) { GEngine->SetTimeUntilNextGarbageCollection(3600.f); }

	UE_LOG(LogFragments, Verbose, TEXT("[Perf] Suppressed GC + Lumen reflections during import"));
}

UFragmentsImporter::EPostSpawnAction UFragmentsImporter::DecidePostSpawnAction(
	int32 NumDeferredModels, int32 NumPendingInteriors)
{
	if (NumDeferredModels > 0)   { return EPostSpawnAction::DrainDeferredModel; }
	if (NumPendingInteriors > 0) { return EPostSpawnAction::ActivateInterior; }
	return EPostSpawnAction::Idle;
}

bool UFragmentsImporter::DrainNextDeferredModelLoad()
{
	check(IsInGameThread());

	while (DeferredModelLoads.Num() > 0)
	{
		FDeferredModelLoad Next = MoveTemp(DeferredModelLoads[0]);
		DeferredModelLoads.RemoveAt(0);

		if (!Next.Owner.IsValid())
		{
			// Owner destroyed while this load sat parked.
			UE_LOG(LogFragments, Warning,
				TEXT("[HISM Chunked] Dropping deferred model %s - its owner was destroyed while queued"),
				*Next.ModelGuid);
			continue;
		}

		UE_LOG(LogFragments, Verbose, TEXT("[HISM Chunked] Processing deferred model: %s"), *Next.ModelGuid);
		ProcessLoadedFragment(Next.ModelGuid, Next.Owner.Get(), Next.bSaveMeshes,
			Next.BucketRoot, Next.OnCompleteCallback);
		return true;
	}

	return false;
}

void UFragmentsImporter::RestoreImportOverhead()
{
	auto SetCVar = [](const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			CVar->Set(Value, ECVF_SetByCode);
		}
	};

	// See SuppressImportOverhead for why r.GenerateMeshDistanceFields/r.DynamicGlobalIlluminationMethod are left untouched.
	SetCVar(TEXT("r.Lumen.Reflections.Allow"), 1);
	SetCVar(TEXT("gc.TimeBetweenPurgingPendingKillObjects"), 60);

	// Snap the next-GC timer back so the 3600s offset from Suppress doesn't leave GC dormant for ~40 minutes post-import.
	if (GEngine) { GEngine->SetTimeUntilNextGarbageCollection(60.f); }

	UE_LOG(LogFragments, Verbose, TEXT("[Perf] Restored subsystems after import"));
}

void UFragmentsImporter::LoadOverridesIntoWrapper(UFragmentModelWrapper* Wrapper, const FString& InModelGuid)
{
	FFragmentOverrides Parsed;
	if (UE::Fragments::Overrides::Load(Wrapper->FragmentPath, InModelGuid, Parsed))
	{
		Wrapper->MaterialOverrides.Reset();
		Wrapper->ResolvedOverrideMaterials.Reset();
		for (const FFragmentMaterialOverrideEntry& Entry : Parsed.MaterialOverrides)
		{
			if (!Entry.SourceKey.IsEmpty() && !Entry.ReplacementMaterialPath.IsEmpty())
			{
				Wrapper->MaterialOverrides.Add(Entry.SourceKey, Entry.ReplacementMaterialPath);
			}
		}
		UE_LOG(LogFragments, Verbose,
			TEXT("[fragmentOverrides] loaded %d entries for model %s"),
			Wrapper->MaterialOverrides.Num(), *InModelGuid);
	}
}

FFindResult UFragmentsImporter::FindFragmentByLocalIdUnified(int64 LocalId, const FString& ModelGuid)
{
	if (FFragmentLookup* Lookup = ModelFragmentsMap.Find(ModelGuid))
	{
		if (AFragment** Actor = Lookup->Fragments.Find(LocalId))
		{
			if (*Actor)
			{
				return FFindResult::FromActor(*Actor);
			}
		}
	}

	return FFindResult::NotFound();
}

// Shell-First Streaming helpers.

AFragment* UFragmentsImporter::GetBucketRootForModel(const FString& ModelGuid) const
{
	AFragment* const* Found = ModelBucketRoots.Find(ModelGuid);
	return Found ? *Found : nullptr;
}

// EDITOR ONLY.
#if WITH_EDITOR
FFragmentImportProgressSnapshot UFragmentsImporter::GetImportProgressSnapshot(
	const FString& ForModelGuid) const
{
	FFragmentImportProgressSnapshot Out;

	// Report spawning only for the model currently using the pipeline.
	Out.bIsSpawning = bIsHISMChunkedSpawning && CurrentSpawningModelGuid == ForModelGuid;

	Out.bIsQueuedForSpawn = DeferredModelLoads.ContainsByPredicate(
		[&ForModelGuid](const FDeferredModelLoad& Load) { return Load.ModelGuid == ForModelGuid; });

	// These counters describe only the model currently using the spawn pipeline.
	if (Out.bIsSpawning)
	{
		Out.bIsDeferredInterior = bIsProcessingDeferredInteriors;
		Out.ItemsSpawned = HISMItemsSpawned;
		Out.TotalItems = TotalHISMItems;
		Out.InFlightMeshBuilds = InFlightShellBuilds.Num();

		// PHASE LADDER -- MIRRORS ProcessHISMSpawnChunk's OWN BRANCHING, IN THE SAME ORDER.
		if (PendingHISMQueue.Num() > 0)
		{
			Out.Phase = EFragmentImportPhase::Spawning;
		}
		else if (ShouldDeferBucketFinalize())
		{
			// The barrier, checked BEFORE the finalize phases exactly as ProcessHISMSpawnChunk checks it.
			Out.Phase = EFragmentImportPhase::WaitingOnMeshBuilds;
		}
		else
		{
			auto CursorFraction = [](int32 Cursor, int32 Num) -> float
			{
				return Num > 0 ? FMath::Clamp((float)Cursor / (float)Num, 0.f, 1.f) : 0.f;
			};

			switch (FinalizeTailPhase)
			{
			case EFinalizeTailPhase::EnqueueCellMerges:
				Out.Phase = EFragmentImportPhase::MergingCells;
				Out.PhaseFraction = CursorFraction(CellMergeEnqueueCursor, CellMergeEnqueueQueue.Num());
				break;
			case EFinalizeTailPhase::ApplyOverrides:
				Out.Phase = EFragmentImportPhase::ApplyingOverrides;
				Out.PhaseFraction = CursorFraction(OverrideApplyCursor, OverrideApplyQueue.Num());
				break;
			case EFinalizeTailPhase::None:
			default:
				// None means Phase A has not handed off yet -- either it is still draining buckets (bIsFinalizingBuckets true) or BeginFinalizeHISMBuckets is about to run this tick.
				Out.Phase = EFragmentImportPhase::BuildingBuckets;
				Out.PhaseFraction = bIsFinalizingBuckets
					? CursorFraction(FinalizeBucketCursor, FinalizeBucketKeys.Num())
					: 0.f;
				break;
			}
		}
	}

	return Out;
}
#endif // WITH_EDITOR

void UFragmentsImporter::HandleInteriorLoadRequested(const FString& ModelGuid)
{
	// Guard: if a shell spawn is in progress, defer this activation until it completes.
	if (bIsHISMChunkedSpawning && !bIsProcessingDeferredInteriors)
	{
		UE_LOG(LogFragments, Verbose, TEXT("[Shell-First] Shell spawn in progress, deferring interior activation for %s"), *ModelGuid);
		PendingInteriorActivations.AddUnique(ModelGuid);
		return;
	}

	TArray<FPendingHISMItem>* Deferred = DeferredInteriorQueues.Find(ModelGuid);
	if (!Deferred || Deferred->Num() == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("[Shell-First] Interior load requested for %s but no deferred items found"), *ModelGuid);
		return;
	}

	AFragment* BucketRoot = GetBucketRootForModel(ModelGuid);
	if (!BucketRoot)
	{
		UE_LOG(LogFragments, Warning, TEXT("[Shell-First] Interior load requested for %s but BucketRoot not found"), *ModelGuid);
		return;
	}

	const int32 Count = Deferred->Num();

	// Priority sort: nearest items to camera spawn first
	FVector CameraLoc = FVector::ZeroVector;
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FRotator CamRot;
			PC->GetPlayerViewPoint(CameraLoc, CamRot);
		}
	}
	// Sort farthest-first so that back-pop (PendingHISMQueue.Last()) yields nearest-first
	Deferred->Sort([&CameraLoc](const FPendingHISMItem& A, const FPendingHISMItem& B)
	{
		return FVector::DistSquared(A.WorldTransform.GetLocation(), CameraLoc)
			 > FVector::DistSquared(B.WorldTransform.GetLocation(), CameraLoc);
	});

	// Move into active queue
	PendingHISMQueue.Append(MoveTemp(*Deferred));
	DeferredInteriorQueues.Remove(ModelGuid);

	// Set context for the spawn pipeline
	CurrentBucketRoot = BucketRoot;
	CurrentSpawningModelGuid = ModelGuid;

	// Restore MeshesRef from the model wrapper (needed for mesh creation in ProcessHISMSpawnChunk)
	if (UFragmentModelWrapper* Wrapper = GetFragmentModel(ModelGuid))
	{
		CurrentMeshesRef = Wrapper->GetParsedModel()->meshes();
		CurrentHISMWrapper = Wrapper;
	}

	UE_LOG(LogFragments, Verbose, TEXT("[Shell-First] Activating %d deferred interior items for model %s"),
		Count, *ModelGuid);

	// Start HISM spawn timer if not already running
	bIsHISMChunkedSpawning = true;
	bIsProcessingDeferredInteriors = true;
	HISMItemsSpawned = 0;
	TotalHISMItems = PendingHISMQueue.Num();

	if (UWorld* World = GetWorld())
	{
		if (!World->GetTimerManager().IsTimerActive(HISMSpawnTimerHandle))
		{
			World->GetTimerManager().SetTimer(
				HISMSpawnTimerHandle, this,
				&UFragmentsImporter::ProcessHISMSpawnChunk,
				0.016f, true);
		}
	}
}

// r.Fragments.Memory.CellBudgetMB: logs the campus-wide budget solve at census-flush time -- the only point a full campus FCellCensus exists, since cells are merged incrementally as HISM chunks are spawned.
static void LogCellBudgetSolve(const FragmentCellCensus::FCellCensus& InteriorCensus, int64 ShellTotalTris)
{
	const float BudgetMB      = CVarMemoryCellBudgetMB.GetValueOnGameThread();
	const float BytesPerTri   = CVarMemoryBytesPerCellTriangle.GetValueOnGameThread();
	const float TemplateFloorMB = CVarMemoryTemplateFloorMB.GetValueOnGameThread();

	if (BudgetMB <= 0.0f) { return; }   // -1 sentinel (default): solver off.

	if (BytesPerTri <= 0.0f)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[CellBudget] r.Fragments.Memory.CellBudgetMB=%.1f is set, but ")
			TEXT("r.Fragments.Memory.BytesPerCellTriangle=%.2f is unset/invalid -- the budget solver ")
			TEXT("stays disabled rather than solving against an uncalibrated constant."),
			BudgetMB, BytesPerTri);
		return;
	}

	// The shell cutoff is fixed in this cell-budget solve.
	const double ShellMB = (double)ShellTotalTris * (double)BytesPerTri / (1024.0 * 1024.0);
	const double FixedFloorMB = (double)TemplateFloorMB + ShellMB;

	const double PredictedAtNoCutoffMB = (double)InteriorCensus.TotalTris * (double)BytesPerTri
		/ (1024.0 * 1024.0) + FixedFloorMB;

	const FragmentCellCensus::FBudgetResult R = FragmentCellCensus::SolveCutoffForBudget(
		InteriorCensus, BudgetMB, BytesPerTri, FixedFloorMB);

	if (!R.bReachable)
	{
		UE_LOG(LogFragments, Warning,
			TEXT("[CellBudget] Budget %.1f MB is UNREACHABLE: the fixed floor this knob cannot move ")
			TEXT("-- %.1f MB LOD0 templates (r.Fragments.Memory.TemplateFloorMB) + %.1f MB shell cells ")
			TEXT("(%lld shell tris, shell has its own independent cutoff) -- alone is %.1f MB, and the ")
			TEXT("interior cutoff has nothing left to give. Raise the budget, or lower the floor by ")
			TEXT("re-measuring it."),
			BudgetMB, TemplateFloorMB, ShellMB, ShellTotalTris, FixedFloorMB);
		return;
	}

	// Approximate inverse of FragmentProxyTiers::RadiusCutoffCm (OmitPx = Cutoff * Coeff / NearEdge), using the same reference-view Coeff BuildCellSubLodSpecs bakes with.
	const float Coeff = FragmentProxyTiers::ViewCoeff(
		FragmentProxyTiers::kRefViewportHeightPx, FragmentProxyTiers::kRefProjM11);
	const float InteriorNearEdgeCm = CVarInteriorLOD0PromoteCm.GetValueOnGameThread();
	const float ApproxOmitPx = (InteriorNearEdgeCm > 0.0f)
		? (R.CutoffCm * Coeff / InteriorNearEdgeCm)
		: 0.0f;

	UE_LOG(LogFragments, Log,
		TEXT("[CellBudget] TWO-PASS WORKFLOW -- this import already built cells at the cutoff already ")
		TEXT("in effect; the recommendation below applies to the NEXT import only. Interior-only solve: ")
		TEXT("shell (%lld tris / %.1f MB) keeps its own cutoff and is folded into the fixed floor below, ")
		TEXT("not reduced by this recommendation. ")
		TEXT("PREDICTED at recommended interior cutoff %.2f cm: %lld interior tris / %.1f MB total ")
		TEXT("(includes %.1f MB fixed floor = %.1f MB templates + %.1f MB shell). ")
		TEXT("PREDICTED at no cutoff (cutoff 0, i.e. unconstrained interior): %lld interior tris / ")
		TEXT("%.1f MB total. Both numbers use the SAME census and the SAME BytesPerCellTriangle ")
		TEXT("constant, so they cannot reveal drift against reality -- for a true predicted-vs-measured ")
		TEXT("check, compare against a `memreport` capture of the resulting import; the importer ")
		TEXT("cannot run that itself. ")
		TEXT("Set r.Fragments.Interior.OmitPx=%.3f (approx -- per-model DemoteCm not included) and ")
		TEXT("re-import to apply."),
		ShellTotalTris, ShellMB,
		R.CutoffCm, R.PredictedTris, R.PredictedMB, FixedFloorMB, (double)TemplateFloorMB, ShellMB,
		InteriorCensus.TotalTris, PredictedAtNoCutoffMB,
		ApproxOmitPx);
}

void UFragmentsImporter::FlushCellCensus()
{
	// Include static mesh-creation counters before checking whether census data is empty.
	for (const TPair<FString, int32>& Pair : GStaticSiteMeshCounts)
	{
		CellCensusReport.MeshesBuiltBySite.FindOrAdd(Pair.Key, 0) += Pair.Value;
	}
	GStaticSiteMeshCounts.Reset();

	if (CellCensusReport.Cells.Num() == 0 && CellCensusReport.MeshesBuiltBySite.Num() == 0) { return; }

	const FString Path = FPaths::Combine(
		FPaths::ProfilingDir(),
		FString::Printf(TEXT("CellCensus-%s.csv"), *FDateTime::Now().ToString()));

	FragmentCellCensus::WriteCensusCsv(CellCensusReport, Path);

	// r.Fragments.Memory.CellBudgetMB: roll INTERIOR cells only up to a campus-wide interior total via the existing public MergeCensus -- no dedicated roll-up API, this is it.
	FragmentCellCensus::FCellCensus InteriorCensus;
	int64 ShellTotalTris = 0;
	for (const FragmentCellCensus::FCellCensus& Cell : CellCensusReport.Cells)
	{
		if (Cell.bShell)
		{
			ShellTotalTris += Cell.TotalTris;
		}
		else
		{
			FragmentCellCensus::MergeCensus(InteriorCensus, Cell);
		}
	}
	LogCellBudgetSolve(InteriorCensus, ShellTotalTris);

	CellCensusReport = FragmentCellCensus::FCensusReport();
}

// Console command registration
#if !UE_BUILD_SHIPPING
namespace FragImporterConsole
{
	static UFragmentsImporter* FindImporter(UWorld* World)
	{
		if (!World) return nullptr;
		UGameInstance* GI = World->GetGameInstance();
		if (!GI) return nullptr;
		UFragmentsImporterSubsystem* Sub = GI->GetSubsystem<UFragmentsImporterSubsystem>();
		if (!Sub) return nullptr;
		return Sub->GetImporter();
	}
}

// The census is a pure observer (r.Fragments.CellMerge.Census, ECVF_Default) with no import-end auto-flush hook: FlushCellCensus is called manually here, exactly as the user already runs Frag.DumpProxyLOD after import.
static FAutoConsoleCommandWithWorld GFragDumpCellCensusCmd(
	TEXT("Frag.DumpCellCensus"),
	TEXT("Write the accumulated cell-merge selection census (r.Fragments.CellMerge.Census=1) to "
	     "Saved/Profiling/CellCensus-<timestamp>.csv and clear the accumulator."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UFragmentsImporter* Imp = FragImporterConsole::FindImporter(World);
		if (!Imp)
		{
			UE_LOG(LogFragments, Warning, TEXT("Frag.DumpCellCensus: No active importer."));
			return;
		}
		Imp->FlushCellCensus();
	}));

#endif

namespace UE::Fragments::ImporterResolver
{
	FResolveEditorImporter& GetEditorResolver()
	{
		static FResolveEditorImporter Delegate;
		return Delegate;
	}

	UFragmentsImporter* Resolve(UObject* WorldContext)
	{
		if (!WorldContext) { return nullptr; }
		UWorld* World = WorldContext->GetWorld();

		if (World)
		{
			if (UGameInstance* GI = World->GetGameInstance())
			{
				if (UFragmentsImporterSubsystem* Subsys = GI->GetSubsystem<UFragmentsImporterSubsystem>())
				{
					// A live subsystem owns the answer once found -- ALWAYS return here, even when GetImporter() is null.
					return Subsys->GetImporter();
				}
			}
		}

		// No GameInstance is the NORMAL case in the editor, not an error.
		if (GetEditorResolver().IsBound())
		{
			return GetEditorResolver().Execute(World);
		}
		return nullptr;
	}
}
