// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Index/index_generated.h"

struct FMeshDescription;
struct FFragmentMeshData;

/** Worker-safe authoring for LOD0 per-element shell meshes. */
namespace FragmentShellAuthoring
{
	/** POD snapshot of one flatbuffer Shell. Worker-safe: owns all its memory. */
	struct FShellSource
	{
		/** Shell points, already converted meters Z-down -> centimeters Z-up. */
		TArray<FVector> Points;

		/** Per-profile point indices into Points. */
		TArray<TArray<int32>> Profiles;

		/** Profile index -> that profile's hole contours (each an index list). */
		TMap<int32, TArray<TArray<int32>>> ProfileHoles;

		bool IsEmpty() const { return Points.Num() == 0 || Profiles.Num() == 0; }
	};

	/** Reads a flatbuffer Shell into a worker-safe POD snapshot. */
	FRAGMENTSUNREAL_API bool ReadShellSource(const Shell* ShellRef, FShellSource& Out);

	/** Tessellation outcome counters. */
	struct FShellAuthorStats
	{
		int32 TotalProfiles = 0;
		int32 FailedProfiles = 0;
		int32 SkippedDegenerateTriangles = 0;
	};

	/** Tessellate a shell profile and author UVs, tangents, normals, and color. */
	FRAGMENTSUNREAL_API bool AuthorShellMeshDescription(
		const FShellSource& Src,
		const FColor& VertexColor,
		FMeshDescription& OutMD,
		TArray<FVector3f>& OutCollisionPositions,
		TArray<uint32>& OutCollisionIndices,
		FShellAuthorStats* OutStats = nullptr);

	/** Extracts an FFragmentMeshData (one vertex per vertex instance) from an authored FMeshDescription. */
	FRAGMENTSUNREAL_API void ExtractMeshData(
		const FMeshDescription& MD,
		const FColor& VertexColor,
		FFragmentMeshData& Out);

	/** Triangulates one polygon with holes via libtess2. */
	FRAGMENTSUNREAL_API bool TriangulatePolygonWithHoles(
		const TArray<FVector>& Points,
		const TArray<int32>& Profile,
		const TArray<TArray<int32>>& Holes,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutIndices);

	/** Per-face planar UVs into channel 0. */
	FRAGMENTSUNREAL_API void ComputePerFacePlanarUVs(FMeshDescription& MeshDescription, float Scale);

	/** Thread-safe unique asset name ("Shell_FB_<n>"). */
	FRAGMENTSUNREAL_API FString MakeUniqueShellAssetName();
}
