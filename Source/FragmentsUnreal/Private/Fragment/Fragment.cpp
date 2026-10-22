// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#include "Fragment/Fragment.h"
#include "Engine/GameInstance.h"
#include "Importer/FragmentsImporterSubsystem.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentRegistrationManager.h"
#include "Utils/FragmentsLog.h"


AFragment::AFragment()
{
	// Keep tick disabled — with 1000+ fragment actors, empty Tick wastes CPU
	PrimaryActorTick.bCanEverTick = false;
}

void AFragment::BeginPlay()
{
	Super::BeginPlay();

	if (!bBucketRoot || FragmentPath.IsEmpty()) return;

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UFragmentsImporterSubsystem* Subsystem = GI->GetSubsystem<UFragmentsImporterSubsystem>())
		{
			Subsystem->RefreshLevelSerializedRoot(this);
		}
	}
}

TArray<struct FItemAttribute> AFragment::GetAttributes()
{
	if (UWorld* W = GEngine->GetWorldFromContextObjectChecked(this))
	{
		if (auto* Sub = W->GetGameInstance()->GetSubsystem<UFragmentsImporterSubsystem>())
		{
			FFragmentItem* InItem = Sub->GetFragmentItemByLocalId(LocalId, ModelGuid);
			if (!InItem) return TArray<FItemAttribute>();

			if (!InItem->ModelGuid.IsEmpty())
			{
				Sub->GetItemData(InItem);
				return InItem->Attributes;
			}
		}
	}
	return TArray<FItemAttribute>();
}

AFragment* AFragment::FindFragmentByLocalId(int64 InLocalId)
{
	if (InLocalId == LocalId) return this;

	AFragment* FoundFragment = nullptr;
	for (AFragment*& F : FragmentChildren)
	{
		if (F->GetLocalId() == InLocalId) return F;

		FoundFragment = F->FindFragmentByLocalId(InLocalId);
	}
	return FoundFragment;
}

void AFragment::SetData(FFragmentItem InFragmentItem)
{
	Data = InFragmentItem;
	ModelGuid = InFragmentItem.ModelGuid;
	Guid = InFragmentItem.Guid;
	GlobalTransform = InFragmentItem.GlobalTransform;
	SetActorTransform(GlobalTransform);
	LocalId = InFragmentItem.LocalId;
	Category = InFragmentItem.Category;
	Samples = InFragmentItem.Samples;
}

// HISM API Implementation.

bool AFragment::SetHighlightedByLocalId(int64 InLocalId, bool bHighlighted)
{
	AFragment* Root = GetBucketRoot();
	if (!Root) return false;

	FFragInstanceArray* Arr = Root->LocalIdToInstance.Find(InLocalId);
	if (!Arr) return false;

	const float Value = bHighlighted ? 1.f : 0.f;

	TSet<UHierarchicalInstancedStaticMeshComponent*> DirtyOnce;
	for (const FFragInstanceRef& Ref : Arr->Items)
	{
		if (!Ref.Comp || Ref.InstanceIndex == INDEX_NONE) continue;

		// Skip highlight on hidden-by-floor instances Read custom data slot [1] (hidden flag) directly from the data array Index = InstanceIndex * NumCustomDataFloats + SlotIndex.
		const int32 HiddenIdx = Ref.InstanceIndex * Ref.Comp->NumCustomDataFloats + 1;
		if (bHighlighted && HiddenIdx < Ref.Comp->PerInstanceSMCustomData.Num()
			&& Ref.Comp->PerInstanceSMCustomData[HiddenIdx] > 0.5f) continue;

		Ref.Comp->SetCustomDataValue(Ref.InstanceIndex, 0, Value, /*bMarkRenderStateDirty=*/false);
		DirtyOnce.Add(Ref.Comp.Get());
	}
	for (UHierarchicalInstancedStaticMeshComponent* C : DirtyOnce)
	{
		if (IsValid(C)) C->MarkRenderStateDirty();
	}
	if (bHighlighted)
	{
		Root->HighlightedLocalIds.Add(InLocalId);
	}
	else
	{
		Root->HighlightedLocalIds.Remove(InLocalId);
	}
	return Arr->Items.Num() > 0;
}

void AFragment::ClearAllHISMHighlights()
{
	TSet<UHierarchicalInstancedStaticMeshComponent*> DirtyOnce;

	for (int64 HighlightedId : HighlightedLocalIds)
	{
		if (FFragInstanceArray* Found = LocalIdToInstance.Find(HighlightedId))
		{
			for (const FFragInstanceRef& Ref : Found->Items)
			{
				if (!Ref.Comp || Ref.InstanceIndex == INDEX_NONE) continue;
				Ref.Comp->SetCustomDataValue(Ref.InstanceIndex, 0, 0.0f, false);
				DirtyOnce.Add(Ref.Comp.Get());
			}
		}
	}
	for (UHierarchicalInstancedStaticMeshComponent* C : DirtyOnce)
	{
		if (IsValid(C)) C->MarkRenderStateDirty();
	}
}

bool AFragment::SetHiddenByLocalId(int64 InLocalId, bool bIsHidden)
{
	AFragment* Root = GetBucketRoot();
	if (!Root) return false;

	FFragInstanceArray* Arr = Root->LocalIdToInstance.Find(InLocalId);
	if (!Arr) return false;

	const float Value = bIsHidden ? 1.0f : 0.0f;

	TSet<UHierarchicalInstancedStaticMeshComponent*> DirtyOnce;
	for (const FFragInstanceRef& Ref : Arr->Items)
	{
		if (!Ref.Comp || Ref.InstanceIndex == INDEX_NONE) continue;
		Ref.Comp->SetCustomDataValue(Ref.InstanceIndex, 1, Value, false);
		DirtyOnce.Add(Ref.Comp.Get());
	}
	for (UHierarchicalInstancedStaticMeshComponent* C : DirtyOnce)
	{
		if (IsValid(C)) C->MarkRenderStateDirty();
	}

	if (bIsHidden)
	{
		Root->HiddenElementLocalIds.Add(InLocalId);
	}
	else
	{
		Root->HiddenElementLocalIds.Remove(InLocalId);
	}

	return Arr->Items.Num() > 0;
}

void AFragment::ClearAllHISMHidden()
{
	TSet<UHierarchicalInstancedStaticMeshComponent*> DirtyOnce;

	for (int64 HiddenId : HiddenElementLocalIds)
	{
		if (FFragInstanceArray* Found = LocalIdToInstance.Find(HiddenId))
		{
			for (const FFragInstanceRef& Ref : Found->Items)
			{
				if (!Ref.Comp || Ref.InstanceIndex == INDEX_NONE) continue;
				Ref.Comp->SetCustomDataValue(Ref.InstanceIndex, 1, 0.0f, false);
				DirtyOnce.Add(Ref.Comp.Get());
			}
		}
	}

	for (UHierarchicalInstancedStaticMeshComponent* C : DirtyOnce)
	{
		if (IsValid(C)) C->MarkRenderStateDirty();
	}

	HiddenElementLocalIds.Empty();
}

void AFragment::ShowOnlyFloor(int64 InFloorId)
{
	HiddenElementLocalIds.Empty();

	for (auto& [Key, HISMC] : Buckets)
	{
		if (!IsValid(HISMC)) continue;

		const FFragLocalids* LUT = InstanceLocalIds.Find(HISMC);
		if (!LUT) continue;

		for (int32 i = 0; i < LUT->FloorKeys.Num(); i++)
		{
			const bool bFloorHidden = (LUT->FloorKeys[i] != InFloorId);
			// Custom data slot [1] = hidden flag (1.0 = hidden, 0.0 = visible).
			HISMC->SetCustomDataValue(i, 1, bFloorHidden ? 1.0f : 0.0f, /*bMarkRenderStateDirty=*/false);

			if (bFloorHidden && LUT->LocalIds.IsValidIndex(i))
			{
				HiddenElementLocalIds.Add(LUT->LocalIds[i]);
			}
		}
		// One dirty mark per HISMC, not per instance.
		HISMC->MarkRenderStateDirty();
	}

	ActiveFloorId = InFloorId;
	bFloorFilterActive = true;

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UFragmentsImporterSubsystem* Subsys = GI->GetSubsystem<UFragmentsImporterSubsystem>())
			{
				if (UFragmentsImporter* Imp = Subsys->GetImporter())
				{
					if (UFragmentRegistrationManager* RegMgr = Imp->GetRegistrationManager())
					{
						TSet<int64> KnownFloors;
						RegMgr->CollectModelFloors(ModelGuid, KnownFloors);
						for (int64 F : KnownFloors)
						{
							RegMgr->SetFloorFilterForModel(ModelGuid, F, F == InFloorId);
						}
					}
				}
			}
		}
	}
}

void AFragment::SetFloorVisible(int64 InFloorId, bool bVisible)
{
	const float HiddenValue = bVisible ? 0.0f : 1.0f;

	for (auto& [Key, HISMC] : Buckets)
	{
		if (!IsValid(HISMC)) continue;

		const FFragLocalids* LUT = InstanceLocalIds.Find(HISMC);
		if (!LUT) continue;

		for (int32 i = 0; i < LUT->FloorKeys.Num(); i++)
		{
			if (LUT->FloorKeys[i] == InFloorId)
			{
				HISMC->SetCustomDataValue(i, 1, HiddenValue, false);
			}
		}
		HISMC->MarkRenderStateDirty();
	}

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UFragmentsImporterSubsystem* Subsys = GI->GetSubsystem<UFragmentsImporterSubsystem>())
			{
				if (UFragmentsImporter* Imp = Subsys->GetImporter())
				{
					if (UFragmentRegistrationManager* RegMgr = Imp->GetRegistrationManager())
					{
						RegMgr->SetFloorFilterForModel(ModelGuid, InFloorId, bVisible);
					}
				}
			}
		}
	}
}

void AFragment::ShowAllFloors()
{
	for (auto& [Key, HISMC] : Buckets)
	{
		if (!IsValid(HISMC)) continue;

		const FFragLocalids* LUT = InstanceLocalIds.Find(HISMC);
		if (!LUT) continue;

		for (int32 i = 0; i < LUT->FloorKeys.Num(); i++)
		{
			HISMC->SetCustomDataValue(i, 1, 0.0f, /*bMarkRenderStateDirty=*/false);
		}
		HISMC->MarkRenderStateDirty();
	}

	HiddenElementLocalIds.Empty();
	bFloorFilterActive = false;

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UFragmentsImporterSubsystem* Subsys = GI->GetSubsystem<UFragmentsImporterSubsystem>())
			{
				if (UFragmentsImporter* Imp = Subsys->GetImporter())
				{
					if (UFragmentRegistrationManager* RegMgr = Imp->GetRegistrationManager())
					{
						RegMgr->ClearFloorFilterForModel(ModelGuid);
					}
				}
			}
		}
	}
}

bool AFragment::IsFloorVisible(int64 InFloorId) const
{
	if (!bFloorFilterActive) return true;
	return (InFloorId == ActiveFloorId);
}

TArray<int64> AFragment::GetAvailableFloorIds() const
{
	TArray<int64> Result;

	UWorld* World = GetWorld();
	if (!World) return Result;

	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return Result;

	UFragmentsImporterSubsystem* Subsys = GI->GetSubsystem<UFragmentsImporterSubsystem>();
	if (!Subsys) return Result;

	UFragmentsImporter* Imp = Subsys->GetImporter();
	if (!Imp) return Result;

	UFragmentRegistrationManager* RegMgr = Imp->GetRegistrationManager();
	if (!RegMgr) return Result;

	TSet<int64> Floors;
	RegMgr->CollectModelFloors(ModelGuid, Floors);
	Result = Floors.Array();
	Result.Sort();
	return Result;
}

void AFragment::EnableProximityCulling(float StartFadeCm, float EndCullCm, bool bAlsoApplytoNonHISM)
{
	StartFadeCm = FMath::Max(0.f, StartFadeCm);
	EndCullCm = FMath::Max(StartFadeCm + 1.f, EndCullCm);

	for (const auto& Pair : Buckets)
	{
		if (auto* HISM = Pair.Value)
		{
#if ENGINE_MAJOR_VERSION >= 5
			HISM->SetCullDistances((int32)StartFadeCm, (int32)EndCullCm);
#else
			HISM->InstanceStartCullDistance = (int32)StartFadeCm;
			HISM->InstanceEndCullDistance = (int32)EndCullCm;
			HISM->MarkRenderStateDirty();
#endif
			HISM->bCastContactShadow = false;
			HISM->SetReceivesDecals(false);
			HISM->SetCanEverAffectNavigation(false);
			HISM->SetGenerateOverlapEvents(false);
			HISM->SetRenderInDepthPass(true);
		}
	}

	if (!bAlsoApplytoNonHISM) return;

	TInlineComponentArray<UStaticMeshComponent*> SMs(this);
	for (UStaticMeshComponent* C : SMs)
	{
		if (!IsValid(C) || Cast<UInstancedStaticMeshComponent>(C)) continue;
		C->LDMaxDrawDistance = EndCullCm;
		C->SetReceivesDecals(false);
		C->SetGenerateOverlapEvents(false);
	}
}

bool AFragment::GetLocalIdForInstance(const UHierarchicalInstancedStaticMeshComponent* HISM, int32 InstanceIndex, int64& OutLocalId) const
{
	const AFragment* Root = const_cast<AFragment*>(this)->GetBucketRoot();
	if (!Root) return false;

	if (const FFragLocalids* Arr = Root->InstanceLocalIds.Find(const_cast<UHierarchicalInstancedStaticMeshComponent*>(HISM)))
	{
		if (Arr->LocalIds.IsValidIndex(InstanceIndex))
		{
			OutLocalId = Arr->LocalIds[InstanceIndex];
			return true;
		}
	}
	return false;
}

void AFragment::GetAllLocalIdsForBucket(const UHierarchicalInstancedStaticMeshComponent* HISM, TArray<int64>& OutLocalIds) const
{
	OutLocalIds.Reset();
	const AFragment* Root = const_cast<AFragment*>(this)->GetBucketRoot();
	if (!Root) return;

	if (const FFragLocalids* Arr = Root->InstanceLocalIds.Find(const_cast<UHierarchicalInstancedStaticMeshComponent*>(HISM)))
	{
		OutLocalIds.Append(Arr->LocalIds);
	}
}

#if WITH_EDITOR
void AFragment::GetSelectedLocalIdsforBucket(const UHierarchicalInstancedStaticMeshComponent* HISM, TArray<int64>& OutLocalIds) const
{
	OutLocalIds.Reset();

	const AFragment* Root = const_cast<AFragment*>(this)->GetBucketRoot();
	if (!Root) return;

	const FFragLocalids* Arr = Root->InstanceLocalIds.Find(const_cast<UHierarchicalInstancedStaticMeshComponent*>(HISM));
	if (!Arr) return;

	const int32 Count = HISM->GetInstanceCount();
	for (int32 i = 0; i < Count; ++i)
	{
		if (!HISM->IsInstanceSelected(i)) continue;
		if (Arr->LocalIds.IsValidIndex(i))
		{
			OutLocalIds.Add(Arr->LocalIds[i]);
		}
	}
}
#endif

AFragment* AFragment::GetBucketRoot()
{
	AActor* Cur = this;
	while (Cur)
	{
		if (AFragment* Frag = Cast<AFragment>(Cur))
		{
			if (Frag->bBucketRoot) return Frag;
		}
		Cur = Cur->GetAttachParentActor();
	}
	return this;
}

int32 AFragment::AddHISMInstance(UStaticMesh* Mesh, UMaterialInterface* Mat, const FTransform& WorldXd, int64 InLocalId, int64 InFloorId, bool bShell)
{
	AFragment* Root = GetBucketRoot();
	if (!Root || !Root->bUseHISM) return INDEX_NONE;
	UHierarchicalInstancedStaticMeshComponent* HISM = Root->GetOrCreateBucket(Mesh, Mat, bShell);

	const FTransform BucketWorld = Root->GetActorTransform();
	const FTransform LocalInst = WorldXd.GetRelativeTransform(BucketWorld);

	const int32 NewIdx = HISM->AddInstance(LocalInst);
	auto& LUT = Root->InstanceLocalIds.FindChecked(HISM);
	if (LUT.LocalIds.Num() <= NewIdx) LUT.LocalIds.SetNum(NewIdx + 1);
	LUT.LocalIds[NewIdx] = InLocalId;

	Root->LocalIdToInstance.FindOrAdd(InLocalId).Items.Add({ HISM, NewIdx });

	if (HISM->NumCustomDataFloats < 2) HISM->NumCustomDataFloats = 2;
	HISM->SetCustomDataValue(NewIdx, 0, 0.0f, false);
	HISM->SetCustomDataValue(NewIdx, 1, 0.0f, false);
	return NewIdx;
}

bool AFragment::ResolveHitToLocalId(const FHitResult& Hit, int64& OutLocalId) const
{
	const auto* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.Component.Get());
	if (!HISM) return false;
	const int32 Idx = Hit.Item;
	if (Idx == INDEX_NONE) return false;

	const AFragment* Root = Cast<AFragment>(const_cast<AFragment*>(this)->GetBucketRoot());
	if (!Root) return false;
	if (const auto* Arr = Root->InstanceLocalIds.Find(HISM))
	{
		if (Arr->LocalIds.IsValidIndex(Idx))
		{
			OutLocalId = Arr->LocalIds[Idx];
			if (Root->HiddenElementLocalIds.Contains(OutLocalId)) return false;
			return true;
		}
	}
	return false;
}

bool AFragment::TryPromoteModelFromHit(const FHitResult& Hit)
{
	if (!Hit.Component.IsValid()) return false;
	if (!Hit.Component->ComponentHasTag(FName(TEXT("HLODProxy")))) return false;

	UGameInstance* GI = GetGameInstance();
	if (!GI) return false;

	UFragmentsImporterSubsystem* Subsystem = GI->GetSubsystem<UFragmentsImporterSubsystem>();
	if (!Subsystem) return false;

	UFragmentsImporter* Imp = Subsystem->GetImporter();
	if (!Imp) return false;

	UFragmentRegistrationManager* RegMgr = Imp->GetRegistrationManager();
	if (!RegMgr) return false;

	return RegMgr->PromoteModelFromProxyHit(Hit);
}

bool AFragment::TryPromoteCellFromHit(const FHitResult& Hit)
{
	if (!Hit.Component.IsValid()) return false;
	if (!Hit.Component->ComponentHasTag(FName(TEXT("FragCellMerge")))) return false;

	UGameInstance* GI = GetGameInstance();
	if (!GI) return false;

	UFragmentsImporterSubsystem* Subsys = GI->GetSubsystem<UFragmentsImporterSubsystem>();
	if (!Subsys) return false;

	return Subsys->PromoteCellAndRetryHit(Hit);
}

FBox AFragment::GetBucketBounds(bool bOnlyVisible) const
{
	FBox OutBox(ForceInit);

	if (!bBucketRoot)
	{
		return OutBox;
	}

	for (const TPair<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Pair : Buckets)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
		if (!IsValid(HISM))
		{
			continue;
		}

		if (bOnlyVisible)
		{
			if (!HISM->IsVisible() || HISM->GetInstanceCount() <= 0)
			{
				continue;
			}
		}
		else
		{
			if (HISM->GetInstanceCount() <= 0)
			{
				continue;
			}
		}

		OutBox += HISM->UnbuiltInstanceBounds;
	}

	return OutBox;
}

bool AFragment::GetInstancesForLocalId(
	int64 InLocalId,
	TArray<UHierarchicalInstancedStaticMeshComponent*>& OutComps,
	TArray<int32>& OutIndices
) const
{
	const AFragment* Root = const_cast<AFragment*>(this)->GetBucketRoot();
	if (!Root) return false;
	const FFragInstanceArray* Arr = Root->LocalIdToInstance.Find(InLocalId);
	if (!Arr) return false;
	for (const FFragInstanceRef& Ref : Arr->Items)
	{
		if (Ref.Comp && Ref.InstanceIndex != INDEX_NONE)
		{
			OutComps.Add(Ref.Comp.Get());
			OutIndices.Add(Ref.InstanceIndex);
		}
	}
	return OutComps.Num() > 0;
}

bool AFragment::GetWorldTransformByLocalId(
	int64 InLocalId,
	FTransform& OutWorldTransform
) const
{
	const AFragment* Root = const_cast<AFragment*>(this)->GetBucketRoot();
	if (!Root)
	{
		return false;
	}

	const FFragInstanceArray* Arr = Root->LocalIdToInstance.Find(InLocalId);
	if (!Arr || Arr->Items.Num() == 0)
	{
		return false;
	}

	for (const FFragInstanceRef& Ref : Arr->Items)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Ref.Comp.Get();

		if (!IsValid(HISM))
		{
			continue;
		}

		if (Ref.InstanceIndex == INDEX_NONE)
		{
			continue;
		}

		if (Ref.InstanceIndex >= HISM->GetInstanceCount())
		{
			continue;
		}

		// true = return world-space transform
		if (HISM->GetInstanceTransform(
			Ref.InstanceIndex,
			OutWorldTransform,
			/*bWorldSpace=*/true))
		{
			return true;
		}
	}

	return false;
}

bool AFragment::GetWorldBoundsByLocalId(int64 InLocalId, FBox& OutWorldBounds) const
{
	OutWorldBounds.Init();
	const AFragment* Root = const_cast<AFragment*>(this)->GetBucketRoot();
	if (!Root) return false;

	const FFragInstanceArray* Instances = Root->LocalIdToInstance.Find(InLocalId);
	if (!Instances) return false;

	for (const FFragInstanceRef& Ref : Instances->Items)
	{
		UHierarchicalInstancedStaticMeshComponent* Hism = Ref.Comp.Get();
		if (!IsValid(Hism) || Ref.InstanceIndex == INDEX_NONE || Ref.InstanceIndex >= Hism->GetInstanceCount()) continue;

		UStaticMesh* Mesh = Hism->GetStaticMesh();
		FTransform WorldTransform;
		if (!Mesh || !Hism->GetInstanceTransform(Ref.InstanceIndex, WorldTransform, true)) continue;
		OutWorldBounds += Mesh->GetBoundingBox().TransformBy(WorldTransform);
	}

	return OutWorldBounds.IsValid != 0;
}

UHierarchicalInstancedStaticMeshComponent* AFragment::GetOrCreateBucket(UStaticMesh* Mesh, UMaterialInterface* Mat, bool bShell)
{
	check(bBucketRoot && "Buckets should only be created on the root fragment");
	// bShell must be set explicitly by the caller.
	FFragBucketKey Key;
	Key.Mesh = Mesh;
	Key.Material = Mat;
	Key.bShell = bShell ? 1 : 0;
	if (auto* Found = Buckets.FindRef(Key)) return Found;

	auto* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
	HISM->SetupAttachment(GetRootComponent());
	HISM->SetMobility(EComponentMobility::Movable);
	HISM->SetRelativeTransform(FTransform::Identity);
	HISM->SetStaticMesh(Mesh);
	HISM->SetMaterial(0, Mat);
	HISM->bAffectDistanceFieldLighting = false;

	HISM->NumCustomDataFloats = FMath::Max<int32>(HISM->NumCustomDataFloats, 2);

#if WITH_EDITOR
	HISM->bHasPerInstanceHitProxies = true;
#endif

	// Do NOT register here — RegistrationManager will register based on camera distance after all buckets are finalized.

	Buckets.Add(Key, HISM);
	InstanceLocalIds.Add(HISM, FFragLocalids());

	return HISM;
}
