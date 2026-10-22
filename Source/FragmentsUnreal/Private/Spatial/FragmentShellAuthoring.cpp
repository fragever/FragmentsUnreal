// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentShellAuthoring.h"

#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include <StaticMeshOperations.h>
#include "Utils/FragmentsUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/ThreadSafeCounter.h"
#include "Algo/Reverse.h"
#include "Misc/ScopeExit.h"
#include "tesselator.h"

namespace FragmentShellAuthoring
{
namespace
{
	FThreadSafeCounter GShellAssetCounter;
}

void ComputePerFacePlanarUVs(FMeshDescription& MeshDescription, float Scale)
{
	FStaticMeshAttributes Attributes(MeshDescription);
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

	if (UVs.GetNumChannels() < 1)
	{
		UVs.SetNumChannels(1);
	}

	for (const FTriangleID TriId : MeshDescription.Triangles().GetElementIDs())
	{
		TArrayView<const FVertexInstanceID> TriInstances = MeshDescription.GetTriangleVertexInstances(TriId);

		const FVertexID V0 = MeshDescription.GetVertexInstanceVertex(TriInstances[0]);
		const FVertexID V1 = MeshDescription.GetVertexInstanceVertex(TriInstances[1]);
		const FVertexID V2 = MeshDescription.GetVertexInstanceVertex(TriInstances[2]);
		const FVector3f P0 = Positions.Get(V0);
		const FVector3f P1 = Positions.Get(V1);
		const FVector3f P2 = Positions.Get(V2);

		FVector3f Normal3f = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
		if (Normal3f.IsNearlyZero())
		{
			Normal3f = FVector3f::UpVector;
		}

		FVector UAxis, VAxis;
		FVector(Normal3f).FindBestAxisVectors(UAxis, VAxis);
		const FVector3f UAxis3f(UAxis);
		const FVector3f VAxis3f(VAxis);

		const FVector3f* CornerP[3] = { &P0, &P1, &P2 };
		for (int32 Corner = 0; Corner < 3; Corner++)
		{
			const FVector3f& P = *CornerP[Corner];
			UVs.Set(TriInstances[Corner], 0, FVector2f(
				FVector3f::DotProduct(P, UAxis3f) * Scale,
				FVector3f::DotProduct(P, VAxis3f) * Scale));
		}
	}
}

FString MakeUniqueShellAssetName()
{
	return FString::Printf(TEXT("Shell_FB_%d"), GShellAssetCounter.Increment());
}

bool TriangulatePolygonWithHoles(
	const TArray<FVector>& Points,
	const TArray<int32>& Profile,
	const TArray<TArray<int32>>& Holes,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutIndices)
{
	TESStesselator* Tess = tessNewTess(nullptr);
	FPlaneProjection Projection = UFragmentsUtils::BuildProjectionPlane(Points, Profile);

	auto AddContour = [&](const TArray<int32>& Indices, bool bIsHole)
		{
			TArray<FVector2D> Projected;
			TArray<float> Contour;

			for (int32 Index : Indices)
			{
				FVector2D P2d = Projection.Project(Points[Index]);
				Projected.Add(P2d);
			}

			if (Projected.Num() < 3)
			{
				UE_LOG(LogFragments, Error, TEXT("Contour has fewer than 3 points, skipping."));
				return;
			}

			// Check for colinearity
			bool bColinear = true;
			const FVector2D& A = Projected[0];

			for (int i = 1; i < Projected.Num() - 1; ++i)
			{
				FVector2D Dir1 = (Projected[i] - A).GetSafeNormal();
				FVector2D Dir2 = (Projected[i + 1] - Projected[i]).GetSafeNormal();
				if (!Dir1.Equals(Dir2, 0.001f))
				{
					bColinear = false;
					break;
				}
			}
			if (bColinear)
			{
				UE_LOG(LogFragments, Verbose, TEXT("Contour is colinear in 2D projection, skipping."));
				return;
			}

			TArray<FVector2D> UniqueProjected;
			UniqueProjected.Reserve(Projected.Num());

			for (int32 i = 0; i < Projected.Num(); ++i)
			{
				if (i == 0 || !Projected[i].Equals(Projected[i - 1], 0.001))
				{
					UniqueProjected.Add(Projected[i]);
				}
			}
			Projected = UniqueProjected;

			// Fix winding
			bool bClockwise = UFragmentsUtils::IsClockwise(Projected);

			if (!bIsHole && bClockwise)
			{
				Algo::Reverse(Projected); // Outer should be CCW
			}
			else if (bIsHole && !bClockwise)
			{
				Algo::Reverse(Projected); // Holes should be CW
			}

			for (const FVector2D& P : Projected)
			{
				Contour.Add(P.X);
				Contour.Add(P.Y);
			}

			tessAddContour(Tess, 2, Contour.GetData(), sizeof(float) * 2, Projected.Num());
		};

	AddContour(Profile, false);

	for (const TArray<int32>& Hole : Holes)
	{
		AddContour(Hole, true);
	}

	if (!tessTesselate(Tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, nullptr))
	{
		UE_LOG(LogFragments, Verbose, TEXT("tessTesselate failed."));
		tessDeleteTess(Tess);
		return false;
	}

	const int32 VertexCount = tessGetVertexCount(Tess);
	const TESSreal* Vertices = tessGetVertices(Tess);

	if (VertexCount == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("TriangulatePolygonWithHoles: libtess2 returned 0 vertices for profile with %d points, %d holes"),
			Profile.Num(), Holes.Num());

		for (const int32& P : Profile)
		{
			UE_LOG(LogFragments, Verbose, TEXT("\tPoints of Vertex 0 X: %.6f, Y: %.6f, Z: %.6f"), Points[P].X, Points[P].Y, Points[P].Z);
		}

		int32 HoleIdx = 0;
		for (const auto& H : Holes)
		{
			for (const int32& P : H)
			{
				UE_LOG(LogFragments, Verbose, TEXT("\tPoints of Hole %d 0 X: %.6f, Y: %.6f, Z: %.6f"), HoleIdx, Points[P].X, Points[P].Y, Points[P].Z);
			}
			HoleIdx++;
		}

		tessDeleteTess(Tess);
		return false;
	}

	TMap<int32, int32> IndexRemap;

	for (int32 i = 0; i < VertexCount; i++)
	{
		FVector2D P2d(Vertices[i * 2], Vertices[i * 2 + 1]);
		FVector P3d = Projection.Unproject(P2d);
		IndexRemap.Add(i, OutVertices.Num());
		OutVertices.Add(P3d);
	}
	const int32* Indices = tessGetElements(Tess);
	const int32 ElementCount = tessGetElementCount(Tess);

	for (int32 i = 0; i < ElementCount; ++i)
	{
		const int32* Poly = &Indices[i * 3];
		for (int j = 0; j < 3; ++j)
		{
			int32 Idx = Poly[j];
			if (Idx != TESS_UNDEF)
			{
				OutIndices.Add(IndexRemap[Idx]);
			}
		}
	}

	tessDeleteTess(Tess);

	return true;
}

bool ReadShellSource(const Shell* ShellRef, FShellSource& Out)
{
	if (!ShellRef) { return false; }

	const auto* Points = ShellRef->points();
	if (!Points || Points->size() == 0) { return false; }

	const auto* Profiles = ShellRef->profiles();
	if (!Profiles || Profiles->size() == 0) { return false; }

	// Coordinate conversion: meters Z-down -> centimeters Z-up.
	Out.Points.Reset(Points->size());
	for (flatbuffers::uoffset_t i = 0; i < Points->size(); i++)
	{
		const auto& P = *Points->Get(i);
		Out.Points.Add(FVector(P.x() * 100.0, P.z() * 100.0, P.y() * 100.0));
	}

	Out.Profiles.Reset(Profiles->size());
	for (flatbuffers::uoffset_t i = 0; i < Profiles->size(); i++)
	{
		const ShellProfile* Profile = Profiles->Get(i);
		const auto* Indices = Profile->indices();
		TArray<int32>& Dst = Out.Profiles.AddDefaulted_GetRef();
		if (!Indices) { continue; }
		Dst.Reserve(Indices->size());
		for (flatbuffers::uoffset_t j = 0; j < Indices->size(); j++)
		{
			Dst.Add(Indices->Get(j));
		}
	}

	// Hole map, mirroring FragmentsImporter.cpp:3495-3520.
	if (const auto* Holes = ShellRef->holes())
	{
		for (flatbuffers::uoffset_t j = 0; j < Holes->size(); j++)
		{
			const auto* Hole = Holes->Get(j);
			const auto* HoleIndices = Hole->indices();
			if (!HoleIndices) { continue; }
			TArray<int32> HoleIdx;
			HoleIdx.Reserve(HoleIndices->size());
			for (flatbuffers::uoffset_t k = 0; k < HoleIndices->size(); k++)
			{
				HoleIdx.Add(HoleIndices->Get(k));
			}
			Out.ProfileHoles.FindOrAdd(Hole->profile_id()).Add(MoveTemp(HoleIdx));
		}
	}

	return true;
}

bool AuthorShellMeshDescription(
	const FShellSource& Src,
	const FColor& VertexColor,
	FMeshDescription& OutMD,
	TArray<FVector3f>& OutCollisionPositions,
	TArray<uint32>& OutCollisionIndices,
	FShellAuthorStats* OutStats)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragShellAuthoring_AuthorShellMeshDescription);

	int32 FailedProfiles = 0;
	int32 SkippedDegenerateTriangles = 0;

	// Written on every exit path so a caller that passes stats can report them even when authoring fails outright.
	ON_SCOPE_EXIT
	{
		if (OutStats)
		{
			OutStats->TotalProfiles = Src.Profiles.Num();
			OutStats->FailedProfiles = FailedProfiles;
			OutStats->SkippedDegenerateTriangles = SkippedDegenerateTriangles;
		}
	};

	if (Src.IsEmpty()) { return false; }

	// Plain FMeshDescription — NOT UStaticMesh::CreateStaticMeshDescription, which allocates a UObject and is game-thread only.
	FStaticMeshAttributes Attributes(OutMD);
	Attributes.Register();
	OutMD.SetNumUVChannels(1);

	const FPolygonGroupID PGId = OutMD.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[PGId] = TEXT("Fragment");

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();

	static const TArray<TArray<int32>> EmptyHoles;

	bool bHasValidPolygons = false;

	TArray<int32> OutIndices;
	TArray<FVector> OutVertices;
	TArray<FVertexID> TessVertexIDs;
	TArray<FVertexInstanceID, TInlineAllocator<3>> Triangle;

	for (int32 i = 0; i < Src.Profiles.Num(); i++)
	{
		const TArray<int32>& ProfilePointsIndex = Src.Profiles[i];
		if (ProfilePointsIndex.Num() < 3) { continue; }

		const TArray<TArray<int32>>* HolesPtr = Src.ProfileHoles.Find(i);
		const TArray<TArray<int32>>& ProfileHoles = HolesPtr ? *HolesPtr : EmptyHoles;

		OutIndices.Reset();
		OutVertices.Reset();
		if (!TriangulatePolygonWithHoles(Src.Points, ProfilePointsIndex, ProfileHoles, OutVertices, OutIndices))
		{
			FailedProfiles++;
			continue;
		}

		TessVertexIDs.Reset(OutVertices.Num());
		for (int32 j = 0; j < OutVertices.Num(); j++)
		{
			const FVertexID VId = OutMD.CreateVertex();
			VertexPositions[VId] = FVector3f(OutVertices[j]);
			TessVertexIDs.Add(VId);
			OutCollisionPositions.Add(FVector3f(OutVertices[j]));
		}

		for (int32 j = 0; j + 2 < OutIndices.Num(); j += 3)
		{
			const int32 I0 = OutIndices[j];
			const int32 I1 = OutIndices[j + 1];
			const int32 I2 = OutIndices[j + 2];

			// Zero-area tris would register a duplicate (Polygon, Vertex) ref in FMeshDescription's element indexer and trip check(!bExists).
			if (I0 == I1 || I1 == I2 || I0 == I2)
			{
				SkippedDegenerateTriangles++;
				continue;
			}

			Triangle.Reset();
			Triangle.Add(OutMD.CreateVertexInstance(TessVertexIDs[I0]));
			Triangle.Add(OutMD.CreateVertexInstance(TessVertexIDs[I1]));
			Triangle.Add(OutMD.CreateVertexInstance(TessVertexIDs[I2]));
			OutMD.CreatePolygon(PGId, Triangle);
			bHasValidPolygons = true;

			// Collision indices reference this profile's vertex block.
			const int32 CollisionBase = OutCollisionPositions.Num() - OutVertices.Num();
			OutCollisionIndices.Add((uint32)(CollisionBase + I0));
			OutCollisionIndices.Add((uint32)(CollisionBase + I1));
			OutCollisionIndices.Add((uint32)(CollisionBase + I2));
		}
	}

	if (!bHasValidPolygons)
	{
		return false;
	}

	// Same computation the game-thread path used, minus the UObject.
	ComputePerFacePlanarUVs(OutMD, 0.01f);
	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(OutMD);
	FStaticMeshOperations::ComputeTangentsAndNormals(OutMD,
		EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents | EComputeNTBsFlags::WeightedNTBs);

	// Vertex color: previously applied during the extract step; now authored directly so BuildFromMeshDescriptions consumes this MD as-is.
	TVertexInstanceAttributesRef<FVector4f> InstColors = Attributes.GetVertexInstanceColors();
	const FVector4f LinearColor = FVector4f(FLinearColor(VertexColor));
	for (const FVertexInstanceID VIID : OutMD.VertexInstances().GetElementIDs())
	{
		InstColors[VIID] = LinearColor;
	}

	return true;
}

void ExtractMeshData(const FMeshDescription& MD, const FColor& VertexColor, FFragmentMeshData& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FragShellAuthoring_ExtractMeshData);

	Out.Reset();
	if (MD.VertexInstances().Num() == 0) { return; }

	TVertexAttributesConstRef<FVector3f> PosAttrib = MD.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> NormalAttrib =
		MD.VertexInstanceAttributes().GetAttributesRef<FVector3f>(MeshAttribute::VertexInstance::Normal);
	TVertexInstanceAttributesConstRef<FVector3f> TangentAttrib =
		MD.VertexInstanceAttributes().GetAttributesRef<FVector3f>(MeshAttribute::VertexInstance::Tangent);
	TVertexInstanceAttributesConstRef<float> BinormalSignAttrib =
		MD.VertexInstanceAttributes().GetAttributesRef<float>(MeshAttribute::VertexInstance::BinormalSign);
	TVertexInstanceAttributesConstRef<FVector2f> UVAttrib =
		MD.VertexInstanceAttributes().GetAttributesRef<FVector2f>(MeshAttribute::VertexInstance::TextureCoordinate);

	FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
	FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	Out.Vertices.Reserve(MD.VertexInstances().Num());

	// Use a direct array lookup for vertex-instance IDs.
	TArray<uint32> InstanceToIndex;
	InstanceToIndex.SetNumUninitialized(MD.VertexInstances().GetArraySize());
	for (const FVertexInstanceID VIId : MD.VertexInstances().GetElementIDs())
	{
		const FVertexID VId = MD.GetVertexInstanceVertex(VIId);
		const FVector3f Pos = PosAttrib.Get(VId);

		FFragmentMeshVertex V;
		V.Position = Pos;
		V.Normal = NormalAttrib.Get(VIId);
		V.TangentX = TangentAttrib.Get(VIId);
		V.TangentYSign = BinormalSignAttrib.Get(VIId);
		V.UV0 = UVAttrib.Get(VIId, 0);
		V.Color = VertexColor;

		BoundsMin = FVector3f::Min(BoundsMin, Pos);
		BoundsMax = FVector3f::Max(BoundsMax, Pos);

		InstanceToIndex[VIId.GetValue()] = static_cast<uint32>(Out.Vertices.Num());
		Out.Vertices.Add(V);
	}

	Out.Indices.Reserve(MD.Triangles().Num() * 3);
	for (const FTriangleID TriId : MD.Triangles().GetElementIDs())
	{
		for (const FVertexInstanceID VIId : MD.GetTriangleVertexInstances(TriId))
		{
			Out.Indices.Add(InstanceToIndex[VIId.GetValue()]);
		}
	}

	if (Out.Vertices.Num() > 0)
	{
		Out.BoundingBox = FBox3f(BoundsMin, BoundsMax);
	}
	Out.AssetName = MakeUniqueShellAssetName();
	Out.bIsValid = true;
}
} // namespace FragmentShellAuthoring
