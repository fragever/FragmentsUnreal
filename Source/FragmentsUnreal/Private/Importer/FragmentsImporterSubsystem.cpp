// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Importer/FragmentsImporterSubsystem.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "Utils/FragmentsLog.h"
#include "Index/index_generated.h"
#include "Spatial/FragmentRegistrationManager.h"

static FAutoConsoleCommandWithWorldAndArgs GFragmentsDumpCategoriesCmd(
    TEXT("Fragments.DumpCategories"),
    TEXT("Dump unique IFC categories from loaded fragment models. Optional arg: ModelGuid to filter to one model."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&UFragmentsImporterSubsystem::HandleDumpCategoriesCommand)
);

static FAutoConsoleCommandWithWorldAndArgs GFragmentsDumpSpaceAttributesCmd(
    TEXT("Fragments.DumpSpaceAttributes"),
    TEXT("Dump every IFCSPACE element's attributes from loaded fragment models. Optional arg: ModelGuid. Convenience wrapper around DumpAttributesByCategory."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&UFragmentsImporterSubsystem::HandleDumpSpaceAttributesCommand)
);

static FAutoConsoleCommandWithWorldAndArgs GFragmentsDumpAttributesByCategoryCmd(
    TEXT("Fragments.DumpAttributesByCategory"),
    TEXT("Dump every element of a given IFC category's attributes. Args: <Category> [ModelGuid]. Case-insensitive category match."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&UFragmentsImporterSubsystem::HandleDumpAttributesByCategoryCommand)
);


/** Scan attributes for a case-insensitive "Name" key; shared by GetElementInfo and GetElementDetail. */
static FString FindNameInAttributes(const TArray<FItemAttribute>& Attributes)
{
    for (const FItemAttribute& Attr : Attributes)
    {
        if (Attr.Key.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
        {
            return Attr.Value;
        }
    }
    return FString();
}

/** Look up the floor index for a LocalId via the model wrapper's LocalIdToFloor map. */
static int32 FindFloorIndex(UFragmentModelWrapper* Wrapper, int64 LocalId)
{
    if (!Wrapper) return INDEX_NONE;
    if (const int16* Found = Wrapper->LocalIdToFloor.Find(LocalId))
    {
        if (*Found >= 0)
        {
            return static_cast<int32>(*Found);
        }
    }
    return INDEX_NONE;
}

FString UFragmentsImporterSubsystem::LoadFragment(const FString& FragPath, const FFragmentLoadOptions& Options)
{
    check(Importer);

    FString ModelGuid = Importer->LoadFragment(FragPath, Options);
    FragmentModels = Importer->GetFragmentModels();

    return ModelGuid;
}

void UFragmentsImporterSubsystem::LoadFragmentAsync(const FString& FragPath,
    const FFragmentLoadOptions& Options,
    FOnFragmentLoadComplete OnComplete)
{
    check(Importer);

    TWeakObjectPtr<UFragmentsImporterSubsystem> WeakThis(this);
    // Bind to the NATIVE overload.
    Importer->LoadFragmentAsyncNative(FragPath, Options,
        [WeakThis, OnComplete](bool bSuccess, const FString& Error, const FString& Guid)
        {
            // Importer is nulled in Deinitialize while the subsystem UObject itself outlives it, so WeakThis.Get() succeeding does not imply Importer is still valid.
            if (UFragmentsImporterSubsystem* Self = WeakThis.Get())
            {
                if (Self->Importer)
                {
                    // Mirror the sync path: refresh the cached model map before notifying.
                    Self->FragmentModels = Self->Importer->GetFragmentModels();
                }
            }
            OnComplete.ExecuteIfBound(bSuccess, Error, Guid);
        });
}

void UFragmentsImporterSubsystem::UnloadFragment(const FString& ModelGuid)
{
    if (Importer)
        Importer->UnloadFragment(ModelGuid);

    if (UFragmentModelWrapper** Found = FragmentModels.Find(ModelGuid))
    {
        (*Found) = nullptr;
        FragmentModels.Remove(ModelGuid);
    }
}

void UFragmentsImporterSubsystem::ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh, FOnProcessLoadedFragmentComplete OnComplete,
    AFragment* BucketRoot)
{
    check(Importer);

    Importer->ProcessLoadedFragment(InModelGuid, InOwnerRef, bInSaveMesh, BucketRoot, OnComplete);
}

void UFragmentsImporterSubsystem::ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
    AFragment* BucketRoot)
{
    check(Importer);

    Importer->ProcessLoadedFragmentItem(InLocalId, InModelGuid, InOwnerRef, bInSaveMesh, BucketRoot);
}

TArray<int64> UFragmentsImporterSubsystem::GetElementsByCategory(const FString& InCategory, const FString& ModelGuid)
{
    check(Importer);

    return Importer->GetElementsByCategory(InCategory, ModelGuid);
}

AFragment* UFragmentsImporterSubsystem::GetItemByLocalId(int64 InLocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetItemByLocalId(InLocalId, InModelGuid);
}

FFragmentItem* UFragmentsImporterSubsystem::GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetFragmentItemByLocalId(InLocalId, InModelGuid);
}

void UFragmentsImporterSubsystem::GetItemData(FFragmentItem* InFragmentItem)
{
    check(Importer);
    Importer->GetItemData(InFragmentItem);
}

TArray<FItemAttribute> UFragmentsImporterSubsystem::GetItemPropertySets(int64 LocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetItemPropertySets(LocalId, InModelGuid);
}

TArray<FItemAttribute> UFragmentsImporterSubsystem::GetItemAttributes(int64 LocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetItemAttributes(LocalId, InModelGuid);
}

FFragmentElementInfo UFragmentsImporterSubsystem::GetElementInfo(int64 LocalId, const FString& ModelGuid)
{
    FFragmentItem* Item = GetFragmentItemByLocalId(LocalId, ModelGuid);
    if (!Item)
    {
        return FFragmentElementInfo();  // LocalId defaults to -1
    }

    // Name lives in IFC attributes (Key == "Name").
    GetItemData(Item);

    const FString Name = FindNameInAttributes(Item->Attributes);
    const int32 FloorIndex = FindFloorIndex(GetModelWrapper(ModelGuid), Item->LocalId);

    return FFragmentElementInfo(Item->LocalId, Item->Guid, Item->Category, Name, FloorIndex, Item->ModelGuid);
}

FString UFragmentsImporterSubsystem::GetModelMetadata(const FString& ModelGuid)
{
    if (UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuid))
    {
        if (*Found)
        {
            const Model* ParsedModel = (*Found)->GetParsedModel();
            if (ParsedModel && ParsedModel->metadata())
            {
                return UTF8_TO_TCHAR(ParsedModel->metadata()->c_str());
            }
        }
    }
    return FString();
}

UFragmentModelWrapper* UFragmentsImporterSubsystem::GetModelWrapper(const FString& ModelGuid) const
{
    if (UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuid))
    {
        return *Found;
    }
    return nullptr;
}

bool UFragmentsImporterSubsystem::PromoteCellAndRetryHit(const FHitResult& Hit)
{
    if (!Importer) return false;
    UFragmentRegistrationManager* RegMgr = Importer->GetRegistrationManager();
    return RegMgr ? RegMgr->PromoteCellAndRetryHit(Hit) : false;
}

FString UFragmentsImporterSubsystem::GetModelGuid(const FString& ModelGuid)
{
    if (UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuid))
    {
        if (*Found)
        {
            const Model* ParsedModel = (*Found)->GetParsedModel();
            if (ParsedModel && ParsedModel->guid())
            {
                return UTF8_TO_TCHAR(ParsedModel->guid()->c_str());
            }
        }
    }
    return FString();
}

FFragmentElementDetail UFragmentsImporterSubsystem::GetElementDetail(int64 LocalId, const FString& ModelGuid)
{
    FFragmentElementDetail Detail;

    FFragmentItem* Item = GetFragmentItemByLocalId(LocalId, ModelGuid);
    if (!Item)
    {
        return Detail;  // bValid defaults to false
    }

    Detail.bValid = true;
    Detail.LocalId = Item->LocalId;
    Detail.GlobalId = Item->Guid;
    Detail.Category = Item->Category;
    Detail.ModelGuid = Item->ModelGuid;
    Detail.Attributes = GetItemAttributes(LocalId, ModelGuid);
    Detail.PropertySets = GetItemPropertySets(LocalId, ModelGuid);

    // Pre-group property-set attributes by set name (first-seen order) so Blueprint can loop groups directly — BP TMap can't hold array values, so we group here.
    {
        TMap<FString, int32> GroupIndex;
        for (const FItemAttribute& Attr : Detail.PropertySets)
        {
            int32 Idx;
            if (const int32* Found = GroupIndex.Find(Attr.PropertySet))
            {
                Idx = *Found;
            }
            else
            {
                Idx = Detail.PropertySetGroups.Num();
                GroupIndex.Add(Attr.PropertySet, Idx);
                FFragmentPropertySetGroup NewGroup;
                NewGroup.Name = Attr.PropertySet;
                Detail.PropertySetGroups.Add(MoveTemp(NewGroup));
            }
            Detail.PropertySetGroups[Idx].Rows.Add(Attr);
        }
    }

    // Name lives in IFC attributes (Key == "Name"); reuse already-fetched attributes.
    Detail.Name = FindNameInAttributes(Detail.Attributes);

    // FloorIndex from the model wrapper's LocalId->Floor map.
    Detail.FloorIndex = FindFloorIndex(GetModelWrapper(ModelGuid), Item->LocalId);

    return Detail;
}

FFindResult UFragmentsImporterSubsystem::FindFragmentByLocalIdUnified(int64 LocalId, const FString& ModelGuid)
{
    check(Importer)
    return Importer->FindFragmentByLocalIdUnified(LocalId, ModelGuid);
}

AFragment* UFragmentsImporterSubsystem::GetModelFragment(const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetModelFragment(InModelGuid);
}

FTransform UFragmentsImporterSubsystem::GetBaseCoordinates()
{
    check(Importer);
    return Importer->GetBaseCoordinates();
}

void UFragmentsImporterSubsystem::ResetBaseCoordinates()
{
    check(Importer);
    Importer->ResetBaseCoordinates();
}

void UFragmentsImporterSubsystem::AbortAllProcessing()
{
    check(IsInGameThread());

    // Registrations first: UnregisterModel matches by BucketRoot pointer, so the actors must still be alive.
    if (UFragmentRegistrationManager* RegMgr = Importer ? Importer->GetRegistrationManager() : nullptr)
    {
        RegMgr->ResetAllRegistrations();
    }

    if (Importer)
    {
        Importer->ResetAll();
    }
}

void UFragmentsImporterSubsystem::ResetViewerSession()
{
    check(IsInGameThread());

    // The importer's live map, not our FragmentModels copy: that copy misses in-flight imports.
    TArray<FString> LoadedModelGuids;
    if (Importer)
    {
        Importer->GetFragmentModels().GetKeys(LoadedModelGuids);
    }

    if (FFragmentCellMergeManager* CellMergeManager = GetCellMergeManager())
    {
        for (const FString& ModelGuid : LoadedModelGuids)
        {
            CellMergeManager->CancelModelMerges(ModelGuid);
        }
    }

    // Releases every registration, then destroys every model's actors and caches.
    AbortAllProcessing();

    FragmentModels.Reset();

    UE_LOG(LogFragments, Log,
        TEXT("[Fragments] Viewer session reset; released %d model(s)."),
        LoadedModelGuids.Num());
}

void UFragmentsImporterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    Importer = NewObject<UFragmentsImporter>(this);
    // The cell-merge manager and its finalize sink moved onto UFragmentsImporter so the editor importer -- which has no GameInstance and so no instance of this subsystem -- has one too.
}

void UFragmentsImporterSubsystem::Deinitialize()
{
    // Drain pending cell merges. === Worker tasks may capture TWeakObjectPtr<UStaticMesh> pointing at meshes the importer owns; waiting first ensures all worker phases complete before mesh references are released.
    if (Importer)
    {
        Importer->ShutdownCellMerges();

        // Drain pending LOD2 proxy builds before releasing the Importer.
        Importer->WaitForAllProxyBuilds();
        Importer = nullptr;
    }

    Super::Deinitialize();
}

void UFragmentsImporterSubsystem::RefreshLevelSerializedRoot(AFragment* Root)
{
    if (!IsValid(Root) || Root->GetFragmentPath().IsEmpty())
    {
        return;
    }

    UWorld* World = Root->GetWorld();
    if (!World)
    {
        return;
    }

    const FString PathToLoad = Root->GetFragmentPath();
    const FString ModelGuidLocal = Root->GetModelGuid();

    UE_LOG(LogFragments, Verbose,
        TEXT("[PIE refresh] BucketRoot '%s' triggering runtime reload from '%s'"),
        *Root->GetName(), *PathToLoad);

    // Runtime chunked path doesn't spawn per-element AFragments; destroy carried-over editor copies.
    for (TActorIterator<AFragment> It(World); It; ++It)
    {
        AFragment* Frag = *It;
        if (IsValid(Frag) && Frag != Root && Frag->GetModelGuid() == ModelGuidLocal)
        {
            Frag->Destroy();
        }
    }

    // Root is about to be destroyed; transient owner keeps the chunked spawn's OwnerRef weak ptr valid across the multi-frame spawn.
    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags |= RF_Transient;
    AActor* TransientOwner = World->SpawnActor<AActor>(AActor::StaticClass(), Root->GetActorTransform(), SpawnParams);
    if (!TransientOwner)
    {
        UE_LOG(LogFragments, Warning, TEXT("[PIE refresh] Failed to spawn transient owner for runtime reload of '%s'"), *PathToLoad);
        return;
    }

    // Place the runtime copy the way the editor import was placed (issue: Play moved base-coordinate models to world origin).
    FFragmentLoadOptions Options;
    Options.OriginMode = Root->GetImportOriginMode();
    if (Options.OriginMode == EFragmentOriginMode::UseBaseCoordinates && Importer)
    {
        Importer->SeedBaseCoordinates(Root->GetImportBaseCoordinates());
    }

    const FString NewGuid = LoadFragment(PathToLoad, Options);
    if (NewGuid.IsEmpty())
    {
        UE_LOG(LogFragments, Warning, TEXT("[PIE refresh] LoadFragment returned empty guid for '%s'"), *PathToLoad);
        TransientOwner->Destroy();
        return;
    }

    ProcessLoadedFragment(NewGuid, TransientOwner, /*bSaveMesh=*/false,
        FOnProcessLoadedFragmentComplete(), /*BucketRoot=*/nullptr);

    Root->Destroy();
}

int32 UFragmentsImporterSubsystem::GetModelInstanceCount(const FString& ModelGuid) const
{
    UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuid);
    if (!Found || !*Found) return 0;

    AFragment* BucketRoot = (*Found)->GetSpawnedFragment();
    if (!IsValid(BucketRoot)) return 0;

    int32 Total = 0;
    for (const TPair<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Pair : BucketRoot->GetBuckets())
    {
        if (UHierarchicalInstancedStaticMeshComponent* HISMC = Pair.Value)
        {
            Total += HISMC->GetInstanceCount();
        }
    }
    return Total;
}

TArray<FString> UFragmentsImporterSubsystem::GetCategoriesForModel(const FString& ModelGuid) const
{
    TArray<FString> Out;
    UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuid);
    if (!Found || !*Found) return Out;

    TSet<FString> Unique;
    TArray<const FFragmentItem*> Stack;
    Stack.Reserve(256);
    Stack.Add(&(*Found)->GetModelItemRef());

    while (Stack.Num() > 0)
    {
        const FFragmentItem* Item = Stack.Pop();
        if (!Item) continue;
        if (!Item->Category.IsEmpty())
        {
            Unique.Add(Item->Category);
        }
        for (const FFragmentItem* Child : Item->FragmentChildren)
        {
            if (Child) Stack.Add(Child);
        }
    }

    Out = Unique.Array();
    Out.Sort();
    return Out;
}

void UFragmentsImporterSubsystem::SetFloorVisibilityRange(const FString& ModelGuid, int32 MinFloor, int32 MaxFloor)
{
    if (MinFloor > MaxFloor)
    {
        UE_LOG(LogFragments, Warning,
            TEXT("SetFloorVisibilityRange: MinFloor (%d) > MaxFloor (%d) for model %s — ignored"),
            MinFloor, MaxFloor, *ModelGuid);
        return;
    }

    UFragmentModelWrapper* Wrapper = GetModelWrapper(ModelGuid);
    if (!Wrapper)
    {
        UE_LOG(LogFragments, Warning,
            TEXT("SetFloorVisibilityRange: model %s is not loaded"), *ModelGuid);
        return;
    }

    AFragment* BucketRoot = Wrapper->GetSpawnedFragment();
    if (!IsValid(BucketRoot))
    {
        UE_LOG(LogFragments, Warning,
            TEXT("SetFloorVisibilityRange: model %s has no spawned BucketRoot"), *ModelGuid);
        return;
    }

    const TMap<int64, int16>& FloorMap = Wrapper->LocalIdToFloor;

    for (const TPair<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Pair : BucketRoot->GetBuckets())
    {
        UHierarchicalInstancedStaticMeshComponent* HISMC = Pair.Value;
        if (!IsValid(HISMC)) continue;

        TArray<int64> LocalIds;
        BucketRoot->GetAllLocalIdsForBucket(HISMC, LocalIds);
        if (LocalIds.Num() == 0) continue;

        for (int32 i = 0; i < LocalIds.Num(); i++)
        {
            const int64 LocalId = LocalIds[i];
            int32 FloorNumber = 0;
            if (const int16* FoundFloor = FloorMap.Find(LocalId))
            {
                FloorNumber = (*FoundFloor >= 0) ? static_cast<int32>(*FoundFloor) : 0;
            }

            const bool bHidden = (FloorNumber < MinFloor || FloorNumber > MaxFloor);
            HISMC->SetCustomDataValue(i, 1, bHidden ? 1.0f : 0.0f, /*bMarkRenderStateDirty=*/false);
        }

        HISMC->MarkRenderStateDirty();
    }

    // Propagate the filter to merged cells via the registration manager so LOD1/LOD2 proxies hide their contributions for out-of-range floors.
    if (UFragmentsImporter* Imp = GetImporter())
    {
        if (UFragmentRegistrationManager* RegMgr = Imp->GetRegistrationManager())
        {
            TSet<int64> KnownFloorIds;
            RegMgr->CollectModelFloors(ModelGuid, KnownFloorIds);
            for (int64 FloorIndex : KnownFloorIds)
            {
                const int32 Floor = static_cast<int32>(FloorIndex);
                const bool bVisible = (Floor >= MinFloor && Floor <= MaxFloor);
                RegMgr->SetFloorFilterForModel(ModelGuid, FloorIndex, bVisible);
            }
        }
    }
}

bool UFragmentsImporterSubsystem::TryGetFloorIndexForLocalId(const FString& ModelGuid, int64 LocalId, int32& OutFloorIndex) const
{
    OutFloorIndex = INDEX_NONE;

    const UFragmentModelWrapper* Wrapper = GetModelWrapper(ModelGuid);
    if (!Wrapper)
    {
        return false;
    }

    const int16* FoundFloor = Wrapper->LocalIdToFloor.Find(LocalId);
    if (!FoundFloor || *FoundFloor < 0)
    {
        return false;
    }

    OutFloorIndex = static_cast<int32>(*FoundFloor);
    return true;
}

bool UFragmentsImporterSubsystem::SetModelUserVisible(const FString& ModelGuid, bool bVisible)
{
    UFragmentRegistrationManager* RegMgr = Importer ? Importer->GetRegistrationManager() : nullptr;
    if (!RegMgr)
    {
        return false;
    }

    return RegMgr->SetModelUserVisible(ModelGuid, bVisible);
}

bool UFragmentsImporterSubsystem::IsModelUserVisible(const FString& ModelGuid) const
{
    UFragmentRegistrationManager* RegMgr = Importer ? Importer->GetRegistrationManager() : nullptr;
    // True on no manager, matching the manager's own answer for an unknown GUID.
    return RegMgr ? RegMgr->IsModelUserVisible(ModelGuid) : true;
}

void UFragmentsImporterSubsystem::SetCategoryFilter(const FString& ModelGuid, const TArray<FString>& VisibleCategories, float DimmedAlpha)
{
    UFragmentModelWrapper* Wrapper = GetModelWrapper(ModelGuid);
    if (!Wrapper)
    {
        UE_LOG(LogFragments, Warning,
            TEXT("[Fragments.Visibility] SetCategoryFilter: model %s is not loaded"), *ModelGuid);
        return;
    }

    AFragment* BucketRoot = Wrapper->GetSpawnedFragment();
    if (!IsValid(BucketRoot))
    {
        UE_LOG(LogFragments, Warning,
            TEXT("[Fragments.Visibility] SetCategoryFilter: model %s has no spawned BucketRoot"), *ModelGuid);
        return;
    }

    // O(1) membership probe vs TArray::Contains (O(N) per instance)
    TSet<FString> VisibleSet;
    VisibleSet.Reserve(VisibleCategories.Num());
    for (const FString& Cat : VisibleCategories)
    {
        VisibleSet.Add(Cat);
    }

    // Build LocalId -> Category once.
    TMap<int64, FString> LocalIdToCategory;
    {
        TArray<const FFragmentItem*> Stack;
        Stack.Reserve(256);
        Stack.Add(&Wrapper->GetModelItemRef());
        while (Stack.Num() > 0)
        {
            const FFragmentItem* Item = Stack.Pop();
            if (!Item) continue;
            if (Item->LocalId >= 0)
            {
                LocalIdToCategory.Add(Item->LocalId, Item->Category);
            }
            for (const FFragmentItem* Child : Item->FragmentChildren)
            {
                if (Child) Stack.Add(Child);
            }
        }
    }

    int32 HISMCount = 0;
    int32 VisibleCount = 0;
    int32 DimmedCount = 0;
    for (const TPair<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*>& Pair : BucketRoot->GetBuckets())
    {
        UHierarchicalInstancedStaticMeshComponent* HISMC = Pair.Value;
        if (!IsValid(HISMC)) continue;

        TArray<int64> LocalIds;
        BucketRoot->GetAllLocalIdsForBucket(HISMC, LocalIds);
        if (LocalIds.Num() == 0) continue;

        for (int32 i = 0; i < LocalIds.Num(); i++)
        {
            const int64 LocalId = LocalIds[i];
            const FString* CategoryPtr = LocalIdToCategory.Find(LocalId);
            const FString& Category = CategoryPtr ? *CategoryPtr : FString();

            const bool bVisible = VisibleSet.Contains(Category);
            const float SlotValue = bVisible ? 0.0f : DimmedAlpha;
            HISMC->SetCustomDataValue(i, 1, SlotValue, /*bMarkRenderStateDirty=*/false);
            if (bVisible) ++VisibleCount; else ++DimmedCount;
        }

        HISMC->MarkRenderStateDirty();
        ++HISMCount;
    }

    UE_LOG(LogFragments, Display,
        TEXT("[Fragments.Visibility] SetCategoryFilter for model %s: %d categories visible, %d HISMCs, %d visible / %d dimmed instances (dim=%.2f)"),
        *ModelGuid, VisibleCategories.Num(), HISMCount, VisibleCount, DimmedCount, DimmedAlpha);
}

void UFragmentsImporterSubsystem::DumpCategories(const FString& ModelGuidOrEmpty) const
{
    if (FragmentModels.Num() == 0)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpCategories] No models are currently loaded"));
        return;
    }

    // Stack-based walk avoids deep recursion on large hierarchies.
    auto DumpOne = [](const FString& Guid, const FFragmentItem& Root)
    {
        TMap<FString, int32> Counts;
        TArray<const FFragmentItem*> Stack;
        Stack.Reserve(256);
        Stack.Add(&Root);

        int32 TotalElements = 0;
        while (Stack.Num() > 0)
        {
            const FFragmentItem* Item = Stack.Pop();
            if (!Item) continue;

            ++TotalElements;
            if (!Item->Category.IsEmpty())
            {
                Counts.FindOrAdd(Item->Category) += 1;
            }

            for (const FFragmentItem* Child : Item->FragmentChildren)
            {
                if (Child) Stack.Add(Child);
            }
        }

        TArray<FString> Keys;
        Counts.GetKeys(Keys);
        Keys.Sort();

        UE_LOG(LogFragments, Display,
            TEXT("[Fragments.DumpCategories] Model %s - %d unique categories, %d total elements:"),
            *Guid, Keys.Num(), TotalElements);

        for (const FString& Key : Keys)
        {
            UE_LOG(LogFragments, Display, TEXT("  %s (%d)"), *Key, Counts[Key]);
        }
    };

    if (!ModelGuidOrEmpty.IsEmpty())
    {
        UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuidOrEmpty);
        if (!Found || !*Found)
        {
            UE_LOG(LogFragments, Warning,
                TEXT("[Fragments.DumpCategories] Model %s is not loaded"), *ModelGuidOrEmpty);
            return;
        }
        DumpOne(ModelGuidOrEmpty, (*Found)->GetModelItemRef());
        return;
    }

    for (const TPair<FString, UFragmentModelWrapper*>& Pair : FragmentModels)
    {
        if (!Pair.Value) continue;
        DumpOne(Pair.Key, Pair.Value->GetModelItemRef());
    }
}

void UFragmentsImporterSubsystem::HandleDumpCategoriesCommand(const TArray<FString>& Args, UWorld* World)
{
    if (!World) return;
    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpCategories] No GameInstance"));
        return;
    }
    UFragmentsImporterSubsystem* Sub = GI->GetSubsystem<UFragmentsImporterSubsystem>();
    if (!Sub)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpCategories] No subsystem"));
        return;
    }

    const FString TargetGuid = Args.Num() > 0 ? Args[0] : FString();
    Sub->DumpCategories(TargetGuid);
}

void UFragmentsImporterSubsystem::DumpAttributesByCategory(const FString& Category, const FString& ModelGuidOrEmpty)
{
    if (Category.IsEmpty())
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpAttributesByCategory] Category arg required"));
        return;
    }
    if (FragmentModels.Num() == 0)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpAttributesByCategory] No models are currently loaded"));
        return;
    }

    auto DumpOne = [this, &Category](const FString& Guid, FFragmentItem& Root)
    {
        TArray<FFragmentItem*> Stack;
        Stack.Reserve(256);
        Stack.Add(&Root);

        int32 MatchCount = 0;
        int32 MatchesWithAttributes = 0;
        TMap<FString, int32> KeyFrequency;

        while (Stack.Num() > 0)
        {
            FFragmentItem* Item = Stack.Pop();
            if (!Item) continue;

            for (FFragmentItem* Child : Item->FragmentChildren)
            {
                if (Child) Stack.Add(Child);
            }

            if (!Item->Category.Equals(Category, ESearchCase::IgnoreCase)) continue;
            ++MatchCount;

            GetItemData(Item);
            if (Item->Attributes.Num() == 0) continue;
            ++MatchesWithAttributes;

            UE_LOG(LogFragments, Display,
                TEXT("[Fragments.DumpAttributesByCategory] LocalId=%lld Guid=%s (%d attrs)"),
                Item->LocalId, *Item->Guid, Item->Attributes.Num());

            for (const FItemAttribute& Attr : Item->Attributes)
            {
                const FString Qualified = Attr.PropertySet.IsEmpty()
                    ? Attr.Key
                    : FString::Printf(TEXT("%s.%s"), *Attr.PropertySet, *Attr.Key);
                KeyFrequency.FindOrAdd(Qualified) += 1;
                UE_LOG(LogFragments, Display, TEXT("    %s = %s"), *Qualified, *Attr.Value);
            }
        }

        UE_LOG(LogFragments, Display,
            TEXT("[Fragments.DumpAttributesByCategory] Model %s, category %s — %d elements, %d with attributes, %d unique attr keys"),
            *Guid, *Category, MatchCount, MatchesWithAttributes, KeyFrequency.Num());

        if (KeyFrequency.Num() > 0)
        {
            TArray<FString> SortedKeys;
            KeyFrequency.GetKeys(SortedKeys);
            SortedKeys.Sort();
            UE_LOG(LogFragments, Display, TEXT("  Unique attribute keys (frequency / elements with attributes):"));
            for (const FString& K : SortedKeys)
            {
                UE_LOG(LogFragments, Display, TEXT("    %s (%d/%d)"),
                    *K, KeyFrequency[K], MatchesWithAttributes);
            }
        }

        if (MatchCount == 0)
        {
            UE_LOG(LogFragments, Warning,
                TEXT("[Fragments.DumpAttributesByCategory] Model %s has NO %s elements"),
                *Guid, *Category);
        }
        else if (MatchesWithAttributes == 0)
        {
            UE_LOG(LogFragments, Warning,
                TEXT("[Fragments.DumpAttributesByCategory] Model %s has %d %s elements but none carry attributes — export likely stripped Psets"),
                *Guid, MatchCount, *Category);
        }
    };

    if (!ModelGuidOrEmpty.IsEmpty())
    {
        UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuidOrEmpty);
        if (!Found || !*Found)
        {
            UE_LOG(LogFragments, Warning,
                TEXT("[Fragments.DumpAttributesByCategory] Model %s is not loaded"), *ModelGuidOrEmpty);
            return;
        }
        DumpOne(ModelGuidOrEmpty, (*Found)->GetModelItemRef());
        return;
    }

    for (const TPair<FString, UFragmentModelWrapper*>& Pair : FragmentModels)
    {
        if (!Pair.Value) continue;
        DumpOne(Pair.Key, Pair.Value->GetModelItemRef());
    }
}

void UFragmentsImporterSubsystem::DumpSpaceAttributes(const FString& ModelGuidOrEmpty)
{
    DumpAttributesByCategory(TEXT("IFCSPACE"), ModelGuidOrEmpty);
}

void UFragmentsImporterSubsystem::HandleDumpSpaceAttributesCommand(const TArray<FString>& Args, UWorld* World)
{
    if (!World) return;
    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpSpaceAttributes] No GameInstance"));
        return;
    }
    UFragmentsImporterSubsystem* Sub = GI->GetSubsystem<UFragmentsImporterSubsystem>();
    if (!Sub)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpSpaceAttributes] No subsystem"));
        return;
    }

    const FString TargetGuid = Args.Num() > 0 ? Args[0] : FString();
    Sub->DumpSpaceAttributes(TargetGuid);
}

void UFragmentsImporterSubsystem::HandleDumpAttributesByCategoryCommand(const TArray<FString>& Args, UWorld* World)
{
    if (!World) return;
    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpAttributesByCategory] No GameInstance"));
        return;
    }
    UFragmentsImporterSubsystem* Sub = GI->GetSubsystem<UFragmentsImporterSubsystem>();
    if (!Sub)
    {
        UE_LOG(LogFragments, Warning, TEXT("[Fragments.DumpAttributesByCategory] No subsystem"));
        return;
    }

    if (Args.Num() < 1)
    {
        UE_LOG(LogFragments, Warning, TEXT("Usage: Fragments.DumpAttributesByCategory <Category> [ModelGuid]"));
        return;
    }

    const FString Category = Args[0];
    const FString TargetGuid = Args.Num() > 1 ? Args[1] : FString();
    Sub->DumpAttributesByCategory(Category, TargetGuid);
}


