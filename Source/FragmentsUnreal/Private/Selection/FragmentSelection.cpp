// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant


#include "Selection/FragmentSelection.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"

namespace
{
	bool DeprojectScreenToRay(APlayerController* PC, FVector2D ScreenPos, float TraceLength,
	                          FVector& OutStart, FVector& OutEnd)
	{
		if (!PC) return false;
		FVector WorldOrigin;
		FVector WorldDirection;
		if (!PC->DeprojectScreenPositionToWorld(ScreenPos.X, ScreenPos.Y, WorldOrigin, WorldDirection))
		{
			return false;
		}
		OutStart = WorldOrigin;
		OutEnd = WorldOrigin + WorldDirection * TraceLength;
		return true;
	}

	UFragmentRegistrationManager* GetRegMgr(UObject* WorldContext)
	{
		// Was: World -> GameInstance -> subsystem -> importer.
		UFragmentsImporter* Imp = UE::Fragments::ImporterResolver::Resolve(WorldContext);
		return Imp ? Imp->GetRegistrationManager() : nullptr;
	}

	// Slot 1 is the per-instance "filter delta": 0.0 = original appearance, >0 = x-ray dimmed (DimmedAlpha) or hidden (1.0).
	constexpr float kFilterEpsilon = 0.01f;   // at-rest is exactly 0.0; any nonzero filter trips this
	constexpr float kAdvanceEps    = 1.0f;    // cm to step past a skipped surface point
	constexpr int32 kMaxWalkSteps  = 32;      // safety cap; real scenes step past a handful of layers

	/** True if this hit is a fragment HISMC instance that has been filtered out (slot 1 > epsilon) and should be clicked through. */
	bool IsHitFilteredOut(const FHitResult& Hit)
	{
		const UHierarchicalInstancedStaticMeshComponent* H =
			Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.GetComponent());
		if (!H || Hit.Item == INDEX_NONE) return false;
		const int32 Idx = Hit.Item * H->NumCustomDataFloats + 1;
		if (Idx < 0 || Idx >= H->PerInstanceSMCustomData.Num()) return false;
		return H->PerInstanceSMCustomData[Idx] > kFilterEpsilon;
	}
}

static void HandleTraceVisibleAtCursor(const TArray<FString>& Args, UWorld* World)
{
	if (!World) { UE_LOG(LogTemp, Warning, TEXT("[TraceVisible] no world")); return; }
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("[TraceVisible] no player controller / camera"));
		return;
	}

	const FVector Start = PC->PlayerCameraManager->GetCameraLocation();
	const FVector Dir   = PC->PlayerCameraManager->GetCameraRotation().Vector();
	const FVector End   = Start + Dir * 100000.f;

	// Instrumented walk: log every collider the ray crosses, its custom-data slot-1 value, and whether the predicate (slot1 > 0.01) would skip it.
	const int32 kDiagSteps = 80;
	FVector Cur = Start;
	FCollisionQueryParams P(SCENE_QUERY_STAT(TraceVisibleDiag), /*bTraceComplex=*/true);
	bool bSelected = false;
	int32 Step = 0;
	for (; Step < kDiagSteps; ++Step)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, Cur, End, ECC_Visibility, P))
		{
			UE_LOG(LogTemp, Display, TEXT("[TraceVisible] step %d: ray exited, no more colliders"), Step);
			break;
		}

		const UHierarchicalInstancedStaticMeshComponent* H =
			Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.GetComponent());
		float Slot1 = -999.f;
		int32 NCDF  = -1;
		if (H && Hit.Item != INDEX_NONE)
		{
			NCDF = H->NumCustomDataFloats;
			const int32 Idx = Hit.Item * NCDF + 1;
			if (Idx >= 0 && Idx < H->PerInstanceSMCustomData.Num()) Slot1 = H->PerInstanceSMCustomData[Idx];
		}
		const bool bFiltered = (H != nullptr) && (Slot1 > kFilterEpsilon);

		UE_LOG(LogTemp, Display,
			TEXT("[TraceVisible] step %2d: comp=%s item=%d ncdf=%d slot1=%.3f filtered=%d%s"),
			Step, *GetNameSafe(Hit.GetComponent()), Hit.Item, NCDF, Slot1, bFiltered ? 1 : 0,
			(Step >= kMaxWalkSteps) ? TEXT("  <-- PAST real cap (32)") : TEXT(""));

		if (bFiltered)
		{
			Cur = Hit.Location + Dir * 1.0f;
			continue;
		}

		UE_LOG(LogTemp, Display,
			TEXT("[TraceVisible] => SELECT at step %d: comp=%s item=%d slot1=%.3f loc=%s"),
			Step, *GetNameSafe(Hit.GetComponent()), Hit.Item, Slot1, *Hit.Location.ToCompactString());
		bSelected = true;
		break;
	}
	if (!bSelected && Step >= kDiagSteps)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[TraceVisible] reached diagnostic cap %d without finding a full-opacity element"), kDiagSteps);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GFragmentsTraceVisibleCmd(
	TEXT("Fragments.TraceVisibleAtCursor"),
	TEXT("Trace from the player camera forward and log the raw first hit vs the first NON-ghosted hit (slot-1 skip walk)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleTraceVisibleAtCursor));

// UFragmentSelectAtScreenPos.

UFragmentSelectAtScreenPos* UFragmentSelectAtScreenPos::SelectAtScreenPos(
	UObject* WorldContextObject,
	APlayerController* PC,
	FVector2D ScreenPos,
	float TraceLength,
	ECollisionChannel TraceChannel,
	int32 MaxRetries)
{
	UFragmentSelectAtScreenPos* Action = NewObject<UFragmentSelectAtScreenPos>();
	Action->WorldContext = WorldContextObject;
	Action->CachedPC = PC;
	Action->CachedScreenPos = ScreenPos;
	Action->CachedTraceLength = TraceLength;
	Action->CachedChannel = TraceChannel;
	Action->RemainingRetries = FMath::Max(0, MaxRetries);
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

UWorld* UFragmentSelectAtScreenPos::GetWorld() const
{
	if (CachedPC.IsValid())   return CachedPC->GetWorld();
	if (WorldContext.IsValid()) return WorldContext->GetWorld();
	return nullptr;
}

void UFragmentSelectAtScreenPos::Activate()
{
	if (!CachedPC.IsValid())
	{
		FireFailed(EFragmentSelectFailReason::InvalidContext);
		return;
	}
	if (!DeprojectScreenToRay(CachedPC.Get(), CachedScreenPos, CachedTraceLength,
	                          CachedRayStart, CachedRayEnd))
	{
		FireFailed(EFragmentSelectFailReason::InvalidContext);
		return;
	}
	DoTrace();
}

void UFragmentSelectAtScreenPos::DoTrace()
{
	if (bCompleted) return;

	UWorld* W = GetWorld();
	if (!W)
	{
		FireFailed(EFragmentSelectFailReason::InvalidContext);
		return;
	}

	FHitResult Hit;
	if (!UFragmentSelectionStatics::TraceFirstVisibleHit(
			WorldContext.Get(), CachedRayStart, CachedRayEnd, Hit, CachedChannel,
			/*bTraceComplex=*/ true))
	{
		FireFailed(EFragmentSelectFailReason::NoTrace);
		return;
	}

	UPrimitiveComponent* HitComp = Hit.GetComponent();
	if (!HitComp)
	{
		FireFailed(EFragmentSelectFailReason::NoFragment);
		return;
	}
	AFragment* HitFragment = Cast<AFragment>(Hit.GetActor());

	// Direct HISMC hit → resolve immediately.
	if (Cast<UHierarchicalInstancedStaticMeshComponent>(HitComp))
	{
		TryResolve(Hit);
		return;
	}

	// LOD1 merged proxy → promote + retry next ~tick.
	if (HitComp->ComponentHasTag(FName(TEXT("FragCellMerge"))))
	{
		if (!HitFragment)          { FireFailed(EFragmentSelectFailReason::NoFragment); return; }
		if (RemainingRetries <= 0) { FireFailed(EFragmentSelectFailReason::Timeout);    return; }
		--RemainingRetries;
		if (HitFragment->TryPromoteCellFromHit(Hit))
		{
			ScheduleRetrace();
			return;
		}
		FireFailed(EFragmentSelectFailReason::NoFragment);
		return;
	}

	// LOD2 / model-level HLOD proxy → set the model promote override + retry.
	if (HitComp->ComponentHasTag(FName(TEXT("HLODProxy"))))
	{
		if (!HitFragment)             { FireFailed(EFragmentSelectFailReason::NoFragment); return; }
		if (RemainingRetries <= 0)    { FireFailed(EFragmentSelectFailReason::Timeout);    return; }
		--RemainingRetries;
		if (HitFragment->TryPromoteModelFromHit(Hit))
		{
			ScheduleRetrace();
			return;
		}
		FireFailed(EFragmentSelectFailReason::NoFragment);
		return;
	}

	FireFailed(EFragmentSelectFailReason::NoFragment);
}

void UFragmentSelectAtScreenPos::ScheduleRetrace()
{
	UWorld* W = GetWorld();
	if (!W)
	{
		FireFailed(EFragmentSelectFailReason::InvalidContext);
		return;
	}
	// 50ms covers one full UpdateRegistration tick at any reasonable framerate so the state machine has applied the LOD1->LOD0 transition before we retrace.
	W->GetTimerManager().SetTimer(
		RetraceTimer, this, &UFragmentSelectAtScreenPos::DoTrace, 0.05f, /*bLoop=*/ false);
}

void UFragmentSelectAtScreenPos::TryResolve(const FHitResult& Hit)
{
	AFragment* HitFragment = Cast<AFragment>(Hit.GetActor());
	if (!HitFragment)
	{
		FireFailed(EFragmentSelectFailReason::NoFragment);
		return;
	}

	int64 LocalId = 0;
	if (!HitFragment->ResolveHitToLocalId(Hit, LocalId))
	{
		FireFailed(EFragmentSelectFailReason::NotResolvable);
		return;
	}

	AFragment* Root = HitFragment->GetBucketRoot();
	if (!Root) Root = HitFragment;

	FFragmentCellHandle Handle;
	if (UFragmentRegistrationManager* RegMgr = GetRegMgr(WorldContext.Get()))
	{
		if (auto* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.GetComponent()))
		{
			Handle = RegMgr->MakeHandleForHISMC(HISM);
		}
	}

	FireResolved(LocalId, Root, Hit.Location, Handle);
}

void UFragmentSelectAtScreenPos::FireResolved(int64 LocalId, AFragment* OwningFragment,
                                              const FVector& HitLoc, const FFragmentCellHandle& Handle)
{
	if (bCompleted) return;
	bCompleted = true;
	const FString ModelGuid = OwningFragment ? OwningFragment->GetModelGuid() : FString();
	Resolved.Broadcast(LocalId, ModelGuid, OwningFragment, HitLoc, Handle);
	SetReadyToDestroy();
}

void UFragmentSelectAtScreenPos::FireFailed(EFragmentSelectFailReason Reason)
{
	if (bCompleted) return;
	bCompleted = true;
	Failed.Broadcast(Reason);
	SetReadyToDestroy();
}

// UFragmentSelectionStatics.

bool UFragmentSelectionStatics::TraceFirstVisibleHit(
	UObject* WorldContextObject,
	FVector Start,
	FVector End,
	FHitResult& OutHit,
	ECollisionChannel TraceChannel,
	bool bTraceComplex)
{
	OutHit = FHitResult();
	if (!WorldContextObject) return false;
	UWorld* W = WorldContextObject->GetWorld();
	if (!W) return false;

	const FVector Dir = (End - Start).GetSafeNormal();
	if (Dir.IsNearlyZero()) return false;

	FVector Cur = Start;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FragmentVisibleTrace), bTraceComplex);

	for (int32 Step = 0; Step < kMaxWalkSteps; ++Step)
	{
		FHitResult Hit;
		if (!W->LineTraceSingleByChannel(Hit, Cur, End, TraceChannel, Params))
		{
			return false; // nothing left along the ray
		}
		if (IsHitFilteredOut(Hit))
		{
			Cur = Hit.Location + Dir * kAdvanceEps; // always advance — front OR back face
			continue;
		}
		OutHit = Hit;
		return true;
	}
	return false; // safety cap reached
}

bool UFragmentSelectionStatics::TraceFirstVisibleHitByObjectTypes(
	UObject* WorldContextObject,
	FVector Start,
	FVector End,
	FHitResult& OutHit,
	const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes,
	bool bTraceComplex)
{
	OutHit = FHitResult();
	if (!WorldContextObject) return false;
	UWorld* W = WorldContextObject->GetWorld();
	if (!W) return false;

	const FVector Dir = (End - Start).GetSafeNormal();
	if (Dir.IsNearlyZero()) return false;

	FCollisionObjectQueryParams ObjParams;
	for (const TEnumAsByte<EObjectTypeQuery>& OT : ObjectTypes)
	{
		ObjParams.AddObjectTypesToQuery(UEngineTypes::ConvertToCollisionChannel(OT));
	}

	FVector Cur = Start;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FragmentVisibleTraceObj), bTraceComplex);

	for (int32 Step = 0; Step < kMaxWalkSteps; ++Step)
	{
		FHitResult Hit;
		if (!W->LineTraceSingleByObjectType(Hit, Cur, End, ObjParams, Params))
		{
			return false;
		}
		if (IsHitFilteredOut(Hit))
		{
			Cur = Hit.Location + Dir * kAdvanceEps;
			continue;
		}
		OutHit = Hit;
		return true;
	}
	return false;
}

bool UFragmentSelectionStatics::PeekAtScreenPos(
	UObject* WorldContextObject,
	APlayerController* PC,
	FVector2D ScreenPos,
	float TraceLength,
	ECollisionChannel TraceChannel,
	int64& OutLocalId,
	FString& OutModelGuid,
	AFragment*& OutOwningFragment,
	FVector& OutHitLocation,
	FFragmentCellHandle& OutCellHandle)
{
	OutLocalId = 0;
	OutModelGuid.Reset();
	OutOwningFragment = nullptr;
	OutHitLocation = FVector::ZeroVector;
	OutCellHandle = FFragmentCellHandle();

	if (!WorldContextObject || !PC) return false;

	FVector Start, End;
	if (!DeprojectScreenToRay(PC, ScreenPos, TraceLength, Start, End)) return false;

	FHitResult Hit;
	if (!TraceFirstVisibleHit(WorldContextObject, Start, End, Hit, TraceChannel,
			/*bTraceComplex=*/ true)) return false;

	UHierarchicalInstancedStaticMeshComponent* HISM =
		Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.GetComponent());
	if (!HISM) return false;

	AFragment* HitFragment = Cast<AFragment>(Hit.GetActor());
	if (!HitFragment) return false;

	int64 LocalId = 0;
	if (!HitFragment->ResolveHitToLocalId(Hit, LocalId)) return false;

	AFragment* Root = HitFragment->GetBucketRoot();
	if (!Root) Root = HitFragment;

	OutLocalId = LocalId;
	OutModelGuid = Root->GetModelGuid();
	OutOwningFragment = Root;
	OutHitLocation = Hit.Location;
	if (UFragmentRegistrationManager* RegMgr = GetRegMgr(WorldContextObject))
	{
		OutCellHandle = RegMgr->MakeHandleForHISMC(HISM);
	}
	return true;
}

void UFragmentSelectionStatics::ClearAllOverrides(UObject* WorldContextObject)
{
	if (UFragmentRegistrationManager* RegMgr = GetRegMgr(WorldContextObject))
	{
		RegMgr->ClearAllHotPromoteOverrides();
	}
}

void UFragmentSelectionStatics::PromoteAllCellsForModel(UObject* WorldContextObject, const FString& ModelGuid)
{
	if (UFragmentRegistrationManager* RegMgr = GetRegMgr(WorldContextObject))
	{
		RegMgr->PromoteAllCellsForModel(ModelGuid);
	}
}

bool UFragmentSelectionStatics::DemoteCell(UObject* WorldContextObject, const FFragmentCellHandle& Handle)
{
	if (UFragmentRegistrationManager* RegMgr = GetRegMgr(WorldContextObject))
	{
		return RegMgr->DemoteCellByHandle(Handle);
	}
	return false;
}
