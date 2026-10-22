// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once
#include "CoreMinimal.h"
struct FMeshDescription;

namespace FragmentMeshAuthoring
{
	/** One geometry section fed to the authoring pass. */
	struct FAuthoringSection
	{
		int32 MaterialSlot = 0;
		TConstArrayView<FVector3f>  Positions;
		TConstArrayView<FVector3f>  Normals;
		TConstArrayView<FVector3f>  TangentsX;      // empty -> default (1,0,0)
		TConstArrayView<float>      TangentYSigns;  // empty -> +1
		TConstArrayView<FVector2f>  UV0;            // empty -> (0,0)
		TConstArrayView<FColor>     Colors;
		TConstArrayView<uint32>     Indices;        // triangle list into this section's verts
		TConstArrayView<FTransform> Instances;      // empty -> single identity transform
	};

	/** Authors a multi-section FMeshDescription (one polygon group per used MaterialSlot, slot names "<SlotNamePrefix><slot>") plus flat world positions/indices for the collision cook, in one pass. */
	FRAGMENTSUNREAL_API bool AuthorMeshDescription(
		TConstArrayView<FAuthoringSection> Sections,
		const TCHAR* SlotNamePrefix,
		FMeshDescription& OutMD,
		TArray<FVector3f>& OutCollisionPositions,
		TArray<uint32>& OutCollisionIndices);
}
