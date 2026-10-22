// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Utils/FragmentOcclusionClassifier.h"

DEFINE_LOG_CATEGORY_STATIC(LogOcclusionClassifier, Log, All);

EOcclusionRole UFragmentOcclusionClassifier::ClassifyFragment(const FString& Category, uint8 MaterialAlpha)
{
	// Rule 1: Transparent materials never block visibility
	if (IsTransparentMaterial(MaterialAlpha))
	{
		UE_LOG(LogOcclusionClassifier, VeryVerbose,
		       TEXT("Fragment category '%s' classified as NonOccluder (alpha=%d < threshold=%d)"),
		       *Category, MaterialAlpha, TransparencyThreshold);
		return EOcclusionRole::NonOccluder;
	}

	// Rule 2: Large structural elements are occluders
	if (IsOccluderCategory(Category))
	{
		UE_LOG(LogOcclusionClassifier, VeryVerbose,
		       TEXT("Fragment category '%s' classified as Occluder (structural element)"),
		       *Category);
		return EOcclusionRole::Occluder;
	}

	// Rule 3: Everything else is an occludee (can be hidden by occluders)
	UE_LOG(LogOcclusionClassifier, VeryVerbose,
	       TEXT("Fragment category '%s' classified as Occludee (default)"),
	       *Category);
	return EOcclusionRole::Occludee;
}

bool UFragmentOcclusionClassifier::IsOccluderCategory(const FString& Category)
{
	// Get the set of occluder categories and check membership
	return GetOccluderCategories().Contains(Category);
}

bool UFragmentOcclusionClassifier::IsTransparentMaterial(uint8 MaterialAlpha)
{
	return MaterialAlpha < TransparencyThreshold;
}

FString UFragmentOcclusionClassifier::GetOcclusionRoleString(EOcclusionRole Role)
{
	switch (Role)
	{
	case EOcclusionRole::Occluder:
		return TEXT("Occluder");
	case EOcclusionRole::Occludee:
		return TEXT("Occludee");
	case EOcclusionRole::NonOccluder:
		return TEXT("NonOccluder");
	default:
		return TEXT("Unknown");
	}
}

const TSet<FString>& UFragmentOcclusionClassifier::GetOccluderCategories()
{
	// Static set of IFC categories that represent large structural elements These are the primary occluders in BIM models.
	static TSet<FString> OccluderCategories = {
		// Walls and wall-like elements
		TEXT("IfcWall"),
		TEXT("IfcWallStandardCase"),
		TEXT("IfcCurtainWall"),
		TEXT("IFCWALL"),
		TEXT("IFCWALLSTANDARDCASE"),
		TEXT("IFCCURTAINWALL"),

		// Floors and slabs
		TEXT("IfcSlab"),
		TEXT("IfcSlabStandardCase"),
		TEXT("IfcSlabElementedCase"),
		TEXT("IFCSLAB"),
		TEXT("IFCSLABSTANDARDCASE"),

		// Roofs
		TEXT("IfcRoof"),
		TEXT("IFCROOF"),

		// Structural columns and beams
		TEXT("IfcColumn"),
		TEXT("IfcColumnStandardCase"),
		TEXT("IfcBeam"),
		TEXT("IfcBeamStandardCase"),
		TEXT("IFCCOLUMN"),
		TEXT("IFCCOLUMNSTANDARDCASE"),
		TEXT("IFCBEAM"),
		TEXT("IFCBEAMSTANDARDCASE"),

		// Coverings (ceilings, flooring)
		TEXT("IfcCovering"),
		TEXT("IFCCOVERING"),

		// Stairs and ramps
		TEXT("IfcStair"),
		TEXT("IfcStairFlight"),
		TEXT("IfcRamp"),
		TEXT("IfcRampFlight"),
		TEXT("IFCSTAIR"),
		TEXT("IFCSTAIRFLIGHT"),
		TEXT("IFCRAMP"),
		TEXT("IFCRAMPFLIGHT"),

		// Plates and panels
		TEXT("IfcPlate"),
		TEXT("IfcPlateStandardCase"),
		TEXT("IFCPLATE"),
		TEXT("IFCPLATESTANDARDCASE"),

		// Building element proxy (often used for large structural elements)
		TEXT("IfcBuildingElementProxy"),
		TEXT("IFCBUILDINGELEMENTPROXY")
	};

	return OccluderCategories;
}
