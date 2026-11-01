// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

// Include the Fragments FlatBuffers schema through this header, never index_generated.h directly.
// The schema declares global types (Transform, Material, Shell, Model, ...). Engine headers parsed after it
// read their own parameters with those names as types and fail to compile, which shows up in builds without a
// shared PCH (Blueprint-only projects packaging a game, BuildPlugin). Pulling those engine headers in first
// keeps them parsed before the schema.
#include "CoreMinimal.h"
#include "StaticMeshResources.h"
#include "Index/index_generated.h"
