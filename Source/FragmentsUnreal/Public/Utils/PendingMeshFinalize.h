// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

// Payload for the budget-paced mesh finalize queue.
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "MeshDescription.h"
#include "Chaos/TriangleMeshImplicitObject.h"

class UStaticMesh;

struct FPendingMeshFinalize
{
	TWeakObjectPtr<UStaticMesh>           WeakMesh;

	// One entry per LOD, index == LOD index.
	TArray<TUniquePtr<FMeshDescription>>  MeshDescriptions;

	// Parallel to MeshDescriptions.
	TArray<float>                         LODScreenSizes;

	Chaos::FTriangleMeshImplicitObjectPtr TriMesh;
	TFunction<void(UStaticMesh*)>         Finalizer;
	FString                               DebugLabel;
	int32                                 EstimatedTriangleCount = 0;

	// Vertex count recorded for finalize outlier diagnostics.
	int32                                 SourceVertexCount = 0;
	int32                                 CornerCount       = 0;

	// Game-thread cost this payload incurs BEYOND building the mesh itself — i.e. work its Finalizer does.
	float                                 ExtraFinalizeCostMs = 0.f;

	// Keep CPU buffers for meshes read by SnapshotCellMergeSources.
	bool                                  bNeedsCpuAccess = false;
};
