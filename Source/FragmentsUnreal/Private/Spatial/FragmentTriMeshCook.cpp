// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Spatial/FragmentTriMeshCook.h"

#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/Particles.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace FragmentTriMeshCook
{
    Chaos::FTriangleMeshImplicitObjectPtr
    BuildTriMeshOnWorker(TArray<FVector3f> Positions, TArray<uint32> Indices)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(FragTriMesh_CookOnWorker);

        if (Positions.Num() < 3 || Indices.Num() < 3 || (Indices.Num() % 3) != 0)
        {
            return nullptr;
        }

        // TParticles is single-precision in 5.3 (FRealSingle = float).
        Chaos::TParticles<Chaos::FRealSingle, 3> Particles;
        Particles.AddParticles(Positions.Num());
        for (int32 i = 0; i < Positions.Num(); ++i)
        {
            Particles.X(i) = Chaos::TVec3<Chaos::FRealSingle>(
                Positions[i].X, Positions[i].Y, Positions[i].Z);
        }

        // Group flat indices into triangles. int32 (not uint16) because cell-merge cells can exceed 65535 verts at campus scale.
        const int32 NumTris = Indices.Num() / 3;
        TArray<Chaos::TVec3<int32>> Elements;
        Elements.Reserve(NumTris);
        for (int32 t = 0; t < NumTris; ++t)
        {
            Elements.Emplace(
                (int32)Indices[t * 3 + 0],
                (int32)Indices[t * 3 + 1],
                (int32)Indices[t * 3 + 2]);
        }

        // Empty material indices — we don't use per-triangle material lookup on hit; the BodySetup has its own per-section material slots.
        TArray<uint16> MaterialIndices;

        // Constructor runs RebuildFastBVH() inline on this thread.
        return Chaos::FTriangleMeshImplicitObjectPtr(new Chaos::FTriangleMeshImplicitObject(
            MoveTemp(Particles), MoveTemp(Elements), MoveTemp(MaterialIndices)));
    }
}
