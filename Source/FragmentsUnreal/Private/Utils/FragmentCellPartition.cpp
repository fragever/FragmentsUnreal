// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Utils/FragmentCellPartition.h"
#include "Utils/FragmentsUtils.h"

namespace UE::Fragments::CellPartition
{

namespace
{
    /** Returns the same-floor neighbor with the highest ItemCount within radius, or nullptr if none found. */
    const TPair<FCellId, FCellMetadata>* FindBestNeighbor(
        const TMap<FCellId, FCellMetadata>& Cells,
        const FCellId& Seed,
        int32 MaxRadius)
    {
        const TPair<FCellId, FCellMetadata>* Best = nullptr;
        int32 BestCount = -1;
        for (const TPair<FCellId, FCellMetadata>& Pair : Cells)
        {
            if (Pair.Key == Seed) continue;
            if (Pair.Key.Floor != Seed.Floor) continue;
            const int32 DX = FMath::Abs<int32>(Pair.Key.GridX - Seed.GridX);
            const int32 DY = FMath::Abs<int32>(Pair.Key.GridY - Seed.GridY);
            if (DX + DY > MaxRadius) continue;
            // TIEBREAK ON THE CELL, NOT ON THE WALK.
            if (Pair.Value.ItemCount > BestCount
                || (Pair.Value.ItemCount == BestCount && Best && Pair.Key < Best->Key))
            {
                Best = &Pair;
                BestCount = Pair.Value.ItemCount;
            }
        }
        return Best;
    }
}

void CollapseSparseCells(
    TMap<FCellId, FCellMetadata>& Cells,
    int32 MinInstances,
    int32 MaxRadius)
{
    const int32 MaxPasses = 3;
    for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
    {
        bool bChanged = false;
        TArray<FCellId> Candidates;
        for (const TPair<FCellId, FCellMetadata>& Pair : Cells)
        {
            if (Pair.Value.ItemCount < MinInstances)
            {
                Candidates.Add(Pair.Key);
            }
        }
        // Break equal item counts deterministically because each absorption changes later choices.
        Candidates.Sort([&Cells](const FCellId& A, const FCellId& B)
        {
            const int32 CountA = Cells[A].ItemCount;
            const int32 CountB = Cells[B].ItemCount;
            if (CountA != CountB) { return CountA < CountB; }
            return A < B;
        });

        for (const FCellId& Seed : Candidates)
        {
            if (!Cells.Contains(Seed)) continue;
            // Earlier absorptions in this pass may have bumped this Seed above threshold.
            if (Cells[Seed].ItemCount >= MinInstances) continue;
            const TPair<FCellId, FCellMetadata>* BestPair = FindBestNeighbor(Cells, Seed, MaxRadius);
            if (!BestPair) continue;
            const FCellId BestKey = BestPair->Key;          // copy before any TMap mutation
            FCellMetadata Source = Cells[Seed];             // copy before Remove invalidates references
            Cells.Remove(Seed);
            FCellMetadata& Target = Cells[BestKey];          // safe — distinct key, post-remove lookup
            Target.ItemCount += Source.ItemCount;
            Target.WorldBounds += Source.WorldBounds;
            bChanged = true;
        }
        if (!bChanged) break;
    }
}

namespace
{
    void RecurseAssignFloor(const FFragmentItem& Item, int16 CurrentFloor,
                            int16& FloorCounter, TMap<int64, int16>& Out)
    {
        // Detect IfcBuildingStorey by category prefix.
        int16 Floor = CurrentFloor;
        if (Item.Category.StartsWith(TEXT("IfcBuildingStorey"), ESearchCase::IgnoreCase))
        {
            Floor = FloorCounter++;
        }
        if (Item.LocalId != INDEX_NONE)
        {
            Out.Add(Item.LocalId, Floor);
        }
        for (const FFragmentItem* Child : Item.FragmentChildren)
        {
            if (Child)
            {
                RecurseAssignFloor(*Child, Floor, FloorCounter, Out);
            }
        }
    }
}

void BuildLocalIdToFloorMap(const FFragmentItem& RootItem,
                            TMap<int64, int16>& OutLocalIdToFloor)
{
    OutLocalIdToFloor.Reset();
    int16 FloorCounter = 0;
    RecurseAssignFloor(RootItem, /*CurrentFloor*/ -1, FloorCounter, OutLocalIdToFloor);
}

}  // namespace UE::Fragments::CellPartition
