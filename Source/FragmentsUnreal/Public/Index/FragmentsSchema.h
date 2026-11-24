// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#pragma once

// Include the Fragments FlatBuffers schema through this header, never index_generated.h directly.
// The schema declares global types (Transform, Material, Shell, Model, ...). Engine headers parsed after it
// read their own parameters with those names as types and fail to compile, which shows up in builds without a
// shared PCH (Blueprint-only projects packaging a game, BuildPlugin). Pulling those engine headers in first
// keeps them parsed before the schema.
#include "CoreMinimal.h"
#include "StaticMeshResources.h"
#include "Index/compatibility_generated.h"

/** Wire schema families, not guessed package versions. Both use file identifier 0001. */
enum class EFragmentSchemaFormat : uint8
{
	Invalid,
	Legacy,
	Fragments361,
	ModernWithoutFaceIds,
	LegacyWithShellExtensions,
	Shared // Both verifiers accept a file containing only common fields.
};

namespace FragmentSchema
{
	FRAGMENTSUNREAL_API EFragmentSchemaFormat Identify(const uint8* Data, size_t Size);
	FRAGMENTSUNREAL_API const TCHAR* Name(EFragmentSchemaFormat Format);
}
