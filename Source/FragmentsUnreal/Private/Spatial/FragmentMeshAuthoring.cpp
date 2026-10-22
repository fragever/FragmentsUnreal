// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentMeshAuthoring.h"

#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

bool FragmentMeshAuthoring::AuthorMeshDescription(
	TConstArrayView<FAuthoringSection> Sections,
	const TCHAR* SlotNamePrefix,
	FMeshDescription& OutMD,
	TArray<FVector3f>& OutCollisionPositions,
	TArray<uint32>& OutCollisionIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragMeshAuthoring_AuthorMeshDescription);

	if (Sections.Num() == 0) { return false; }

	// Group section indices by material slot (multiple sections can share a slot).
	TMap<int32, TArray<int32>> SlotToSectionIndices;
	for (int32 SecIdx = 0; SecIdx < Sections.Num(); ++SecIdx)
	{
		const int32 Slot = Sections[SecIdx].MaterialSlot;
		if (Slot < 0) { continue; }
		SlotToSectionIndices.FindOrAdd(Slot).Add(SecIdx);
	}
	if (SlotToSectionIndices.Num() == 0) { return false; }

	TArray<int32> SortedSlots;
	SlotToSectionIndices.GetKeys(SortedSlots);
	SortedSlots.Sort();

	int32 TotalVerts = 0, TotalCorners = 0;
	for (const FAuthoringSection& Sec : Sections)
	{
		const int32 NumInst = FMath::Max(1, Sec.Instances.Num());
		TotalVerts   += Sec.Positions.Num() * NumInst;
		TotalCorners += Sec.Indices.Num()   * NumInst;
	}
	if (TotalVerts == 0 || TotalCorners < 3) { return false; }

	FStaticMeshAttributes Attributes(OutMD);
	Attributes.Register();
	OutMD.SetNumUVChannels(1);
	OutMD.ReserveNewVertices(TotalVerts);
	OutMD.ReserveNewVertexInstances(TotalCorners);
	OutMD.ReserveNewPolygons(TotalCorners / 3);
	OutMD.ReserveNewTriangles(TotalCorners / 3);
	OutMD.ReserveNewPolygonGroups(SortedSlots.Num());
	OutCollisionPositions.Reserve(TotalVerts);
	OutCollisionIndices.Reserve(TotalCorners);

	TVertexAttributesRef<FVector3f>         VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstNormals  = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float>     InstBiSign   = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> InstColors   = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstUVs      = Attributes.GetVertexInstanceUVs();

	bool bAnyPolygons = false;

	for (int32 Slot : SortedSlots)
	{
		const FPolygonGroupID PG = OutMD.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[PG] =
			*FString::Printf(TEXT("%s%d"), SlotNamePrefix, Slot);

		for (int32 SecIdx : SlotToSectionIndices[Slot])
		{
			const FAuthoringSection& Sec = Sections[SecIdx];
			const int32 NumVerts = Sec.Positions.Num();
			if (NumVerts == 0) { continue; }

			const bool bHasTangents     = !Sec.TangentsX.IsEmpty();
			const bool bHasTangentSigns = !Sec.TangentYSigns.IsEmpty();
			const bool bHasUV0          = !Sec.UV0.IsEmpty();

			TArray<FVertexID, TInlineAllocator<256>>  InstVertIDs;
			TArray<FVector3f, TInlineAllocator<256>>  WorldNormals;
			TArray<FVector3f, TInlineAllocator<256>>  WorldTangents;
			TArray<FVector4f, TInlineAllocator<256>>  LinearColors;
			InstVertIDs.SetNumUninitialized(NumVerts);
			WorldNormals.SetNumUninitialized(NumVerts);
			WorldTangents.SetNumUninitialized(NumVerts);
			LinearColors.SetNumUninitialized(NumVerts);

			const bool bNoInstances = Sec.Instances.IsEmpty();
			const int32 NumInstIterations = bNoInstances ? 1 : Sec.Instances.Num();

			for (int32 InstIdx = 0; InstIdx < NumInstIterations; ++InstIdx)
			{
				const FTransform& Xform = bNoInstances ? FTransform::Identity : Sec.Instances[InstIdx];

				// Base index into OutCollisionPositions for this instance's verts.
				const int32 CollisionBase = OutCollisionPositions.Num();

				for (int32 i = 0; i < NumVerts; ++i)
				{
					const FVector3f WorldPos = FVector3f(Xform.TransformPosition(FVector(Sec.Positions[i])));
					const FVertexID VID = OutMD.CreateVertex();
					VertexPositions[VID] = WorldPos;
					InstVertIDs[i] = VID;
					OutCollisionPositions.Add(WorldPos);

					WorldNormals[i]  = FVector3f(Xform.TransformVectorNoScale(FVector(Sec.Normals[i]))).GetSafeNormal();
					WorldTangents[i] = bHasTangents
						? FVector3f(Xform.TransformVectorNoScale(FVector(Sec.TangentsX[i]))).GetSafeNormal()
						: FVector3f(1, 0, 0);
					LinearColors[i]  = FVector4f(FLinearColor(Sec.Colors[i]));
				}

				for (int32 t = 0; t + 2 < Sec.Indices.Num(); t += 3)
				{
					TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
					bool bValid = true;
					for (int32 k = 0; k < 3; ++k)
					{
						const int32 L = (int32)Sec.Indices[t + k];
						if (!Sec.Positions.IsValidIndex(L)) { bValid = false; break; }

						const FVertexInstanceID VIID = OutMD.CreateVertexInstance(InstVertIDs[L]);
						InstNormals[VIID]  = WorldNormals[L];
						InstTangents[VIID] = WorldTangents[L];
						InstBiSign[VIID]   = bHasTangentSigns ? Sec.TangentYSigns[L] : 1.0f;
						InstUVs.Set(VIID, 0, bHasUV0 ? Sec.UV0[L] : FVector2f::ZeroVector);
						InstColors[VIID]   = LinearColors[L];
						Corners.Add(VIID);
						OutCollisionIndices.Add((uint32)(CollisionBase + L));
					}
					if (bValid)
					{
						OutMD.CreatePolygon(PG, Corners);
						bAnyPolygons = true;
					}
					else
					{
						// Drop this triangle's collision indices; its already-created vertex instances stay unreferenced in OutMD (harmless — only reachable with malformed test data).
						OutCollisionIndices.SetNum(OutCollisionIndices.Num() - Corners.Num());
					}
				}
			}
		}
	}

	return bAnyPolygons;
}
