// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Index/FragmentsSchema.h"
#include "Index/legacy_generated.h"
#include "Index/fragments361_generated.h"
#include "Index/modern_generated.h"

namespace FragmentSchema
{
namespace
{
template <typename T> bool Verify(const uint8* Data, size_t Size)
{
	flatbuffers::Verifier::Options Options;
	Options.max_tables = TNumericLimits<flatbuffers::uoffset_t>::Max();
	flatbuffers::Verifier Verifier(Data, Size, Options);
	return Verifier.VerifyBuffer<T>(nullptr);
}

template <typename Profiles, typename Holes>
bool CheckContours(const Profiles* ProfilesRef, const Holes* HolesRef, uint32 PointCount)
{
	if (!ProfilesRef || !HolesRef) return false;
	for (const auto* Profile : *ProfilesRef)
	{
		for (auto Index : *Profile->indices())
			if (Index >= PointCount || Index > MAX_int32) return false;
	}
	for (const auto* Hole : *HolesRef)
	{
		if (Hole->profile_id() >= ProfilesRef->size()) return false;
		for (auto Index : *Hole->indices())
			if (Index >= PointCount || Index > MAX_int32) return false;
	}
	return true;
}
}

EFragmentSchemaFormat Identify(const uint8* Data, size_t Size)
{
	if (!Data || Size < sizeof(flatbuffers::uoffset_t) || Size >= FLATBUFFERS_MAX_BUFFER_SIZE)
		return EFragmentSchemaFormat::Invalid;
	// This view skips the two incompatible root slots and checks all shared and shell fields.
	if (!Verify<Model>(Data, Size)) return EFragmentSchemaFormat::Invalid;
	const Model* View = GetModel(Data);
	bool bHasNewShellFields = View->indexes() != nullptr;
	const auto* Mesh = View->meshes();
	for (const auto* ShellRef : *Mesh->shells())
	{
		bHasNewShellFields |= ShellRef->big_profiles() || ShellRef->big_holes() || ShellRef->profiles_face_ids();
		if (ShellRef->type() == ShellType_BIG)
		{
			if (!CheckContours(ShellRef->big_profiles(), ShellRef->big_holes(), ShellRef->points()->size()))
				return EFragmentSchemaFormat::Invalid;
		}
		else if (ShellRef->type() != ShellType_NONE ||
			!CheckContours(ShellRef->profiles(), ShellRef->holes(), ShellRef->points()->size()))
			return EFragmentSchemaFormat::Invalid;
	}
	// Check semantic references before the existing importer follows them.
	for (const auto* Rep : *Mesh->representations())
	{
		if (Rep->representation_class() == RepresentationClass_SHELL && Rep->id() >= Mesh->shells()->size())
			return EFragmentSchemaFormat::Invalid;
		if (Rep->representation_class() == RepresentationClass_CIRCLE_EXTRUSION && Rep->id() >= Mesh->circle_extrusions()->size())
			return EFragmentSchemaFormat::Invalid;
	}
	for (const auto* Instance : *Mesh->samples())
	{
		if (Instance->item() >= Mesh->meshes_items()->size() || Instance->item() >= View->local_ids()->size() ||
			Mesh->meshes_items()->Get(Instance->item()) >= Mesh->global_transforms()->size() ||
			Instance->material() >= Mesh->materials()->size() || Instance->representation() >= Mesh->representations()->size() ||
			Instance->local_transform() >= Mesh->local_transforms()->size()) return EFragmentSchemaFormat::Invalid;
	}
	const bool bLatest = Verify<Fragments361::Model>(Data, Size);
	const bool bLegacy = Verify<FragmentsLegacy::Model>(Data, Size);
	if (bHasNewShellFields)
	{
		if (bLatest) return EFragmentSchemaFormat::Fragments361;
		// Earlier modern exporters wrote the same layout before profiles_face_ids was introduced.
		if (Verify<FragmentsModern::Model>(Data, Size)) return EFragmentSchemaFormat::ModernWithoutFaceIds;
		// Some exporters retained legacy root slots while adding the shell extensions.
		// The common verifier above has independently checked those extra shell fields.
		return bLegacy ? EFragmentSchemaFormat::LegacyWithShellExtensions : EFragmentSchemaFormat::Invalid;
	}
	if (bLatest && bLegacy) return EFragmentSchemaFormat::Shared;
	if (bLatest) return EFragmentSchemaFormat::Fragments361;
	return bLegacy ? EFragmentSchemaFormat::Legacy : EFragmentSchemaFormat::Invalid;
}

const TCHAR* Name(EFragmentSchemaFormat Format)
{
	switch (Format)
	{
	case EFragmentSchemaFormat::Legacy: return TEXT("legacy schema");
	case EFragmentSchemaFormat::Fragments361: return TEXT("Fragments 3.6.1 schema");
	case EFragmentSchemaFormat::ModernWithoutFaceIds: return TEXT("Fragments modern schema (before face IDs)");
	case EFragmentSchemaFormat::LegacyWithShellExtensions: return TEXT("legacy schema with extended shells");
	case EFragmentSchemaFormat::Shared: return TEXT("shared legacy/3.6.1 schema (version ambiguous)");
	default: return TEXT("invalid or unsupported schema");
	}
}
}
