// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#pragma once

#include "CoreMinimal.h"
#include "Chaos/ImplicitFwd.h"

namespace Chaos { class FTriangleMeshImplicitObject; }

/** Worker-thread runtime trimesh cook helper. */
namespace FragmentTriMeshCook
{
    /** Build a Chaos triangle-mesh implicit object from positions + flat-triangle indices. */
    Chaos::FTriangleMeshImplicitObjectPtr
    BuildTriMeshOnWorker(TArray<FVector3f> Positions, TArray<uint32> Indices);
}
