// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// Spatial partition primitives for the LOD chain.

#pragma once

#include "CoreMinimal.h"
#include "FragmentCellPartition.generated.h"

USTRUCT()
struct FRAGMENTSUNREAL_API FCellId
{
    GENERATED_BODY()

    UPROPERTY() int16 Floor = 0;
    UPROPERTY() int16 GridX = 0;
    UPROPERTY() int16 GridY = 0;

    FCellId() = default;
    FCellId(int16 InFloor, int16 InX, int16 InY) : Floor(InFloor), GridX(InX), GridY(InY) {}

    static FCellId Sentinel() { return FCellId(-1, -1, -1); }
    bool IsSentinel() const { return Floor == -1 && GridX == -1 && GridY == -1; }

    bool operator==(const FCellId& O) const
    {
        return Floor == O.Floor && GridX == O.GridX && GridY == O.GridY;
    }
    bool operator!=(const FCellId& O) const { return !(*this == O); }

    /** A TOTAL ORDER on the cell's identity, so anything that sorts cells can break ties on the cell itself rather than on TMap iteration order. */
    bool operator<(const FCellId& O) const
    {
        if (Floor != O.Floor) { return Floor < O.Floor; }
        if (GridX != O.GridX) { return GridX < O.GridX; }
        return GridY < O.GridY;
    }
};

FORCEINLINE uint32 GetTypeHash(const FCellId& K)
{
    const uint32 A = ::GetTypeHash(K.Floor);
    const uint32 B = ::GetTypeHash(K.GridX);
    const uint32 C = ::GetTypeHash(K.GridY);
    return HashCombine(HashCombine(A, B), C);
}

USTRUCT()
struct FRAGMENTSUNREAL_API FCellMetadata
{
    GENERATED_BODY()

    UPROPERTY() int32 ItemCount = 0;
    UPROPERTY() FBox WorldBounds = FBox(ForceInit);
};

// Forward decl avoids pulling FragmentsUtils.h (and the FlatBuffers index_generated.h transitive include) into this lightweight partition header.
struct FFragmentItem;

namespace UE::Fragments::CellPartition
{
    /** Compute model-relative grid indices for a world XY position. */
    FORCEINLINE void ComputeGridXY(const FVector& WorldPos, double AnchorX, double AnchorY,
                                   float CellSizeCm, int16& OutX, int16& OutY)
    {
        const double RelX = WorldPos.X - AnchorX;
        const double RelY = WorldPos.Y - AnchorY;
        OutX = static_cast<int16>(FMath::FloorToInt32(RelX / CellSizeCm));
        OutY = static_cast<int16>(FMath::FloorToInt32(RelY / CellSizeCm));
    }

    /** Runs a bounded collapse pass (up to 3 iterations) on a cell table in-place. - Cells with ItemCount < MinInstances try to absorb into a same-floor neighbor within ManhattanDistance <= MaxRadius. */
    void CollapseSparseCells(
        TMap<FCellId, FCellMetadata>& Cells,
        int32 MinInstances,
        int32 MaxRadius);

    /** Build a LocalId -> FloorIndex map by walking the FFragmentItem hierarchy. */
    void BuildLocalIdToFloorMap(
        const FFragmentItem& RootItem,
        TMap<int64, int16>& OutLocalIdToFloor);
}
