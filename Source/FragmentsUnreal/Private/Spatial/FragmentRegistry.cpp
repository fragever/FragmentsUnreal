// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

#include "Spatial/FragmentRegistry.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "Utils/FragmentOcclusionClassifier.h"
#include "Index/index_generated.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentRegistry, Log, All);

UFragmentRegistry::UFragmentRegistry()
	: WorldBounds(ForceInit)
	, bIsBuilt(false)
{
}

void UFragmentRegistry::BuildFromModel(const UFragmentModelWrapper* ModelWrapper, const FString& ModelGuid)
{
	if (!ModelWrapper)
	{
		UE_LOG(LogFragmentRegistry, Error, TEXT("BuildFromModel: Null ModelWrapper"));
		return;
	}

	const Model* ParsedModel = ModelWrapper->GetParsedModel();
	if (!ParsedModel)
	{
		UE_LOG(LogFragmentRegistry, Error, TEXT("BuildFromModel: No parsed model"));
		return;
	}

	ModelGuidRef = ModelGuid;

	// Clear any existing data
	Fragments.Empty();
	LocalIdToIndex.Empty();
	WorldBounds.Init();

	const double StartTime = FPlatformTime::Seconds();

	// Recursively collect fragment data from hierarchy
	const FFragmentItem& RootItem = ModelWrapper->GetModelItemRef();
	CollectFragmentData(RootItem, ParsedModel);

	// Calculate combined world bounds
	for (const FFragmentVisibilityData& Data : Fragments)
	{
		if (Data.WorldBounds.IsValid)
		{
			WorldBounds += Data.WorldBounds;
		}
	}

	const double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	bIsBuilt = true;

	UE_LOG(LogFragmentRegistry, Log, TEXT("FragmentRegistry built in %.2f ms: %d fragments, WorldBounds: %s"),
	       ElapsedTime * 1000.0,
	       Fragments.Num(),
	       *WorldBounds.ToString());

	// Log memory usage
	UE_LOG(LogFragmentRegistry, Log, TEXT("FragmentRegistry memory: %lld KB"),
	       GetMemoryUsage() / 1024);
}

void UFragmentRegistry::CollectFragmentData(const FFragmentItem& Item, const Model* ParsedModel)
{
	// Only process fragments with valid LocalId and at least one sample (representation)
	if (Item.LocalId >= 0 && Item.Samples.Num() > 0)
	{
		FFragmentVisibilityData VisData;
		VisData.LocalId = Item.LocalId;
		VisData.GlobalId = Item.Guid;  // FFragmentItem uses 'Guid', not 'GlobalId'
		VisData.Category = Item.Category;
		VisData.WorldBounds.Init();

		bool bHasValidBounds = false;
		int32 PrimaryMaterialIndex = 0;
		uint8 PrimaryMaterialAlpha = 255;  // Default to fully opaque

		// Iterate through all samples (representations) for this fragment
		for (const FFragmentSample& Sample : Item.Samples)
		{
			if (Sample.RepresentationIndex < 0 || !ParsedModel)
			{
				continue;
			}

			// Get meshes from model
			const Meshes* MeshesRef = ParsedModel->meshes();
			if (!MeshesRef)
			{
				continue;
			}

			// Get representations from meshes
			const auto* Representations = MeshesRef->representations();
			if (!Representations || Sample.RepresentationIndex >= static_cast<int32>(Representations->size()))
			{
				continue;
			}

			const Representation* Rep = Representations->Get(Sample.RepresentationIndex);
			if (!Rep)
			{
				continue;
			}

			// Store primary representation index
			if (VisData.RepresentationIndex < 0)
			{
				VisData.RepresentationIndex = Sample.RepresentationIndex;
			}

			// Get material index and alpha from FFragmentSample (not Representation)
			if (Sample.MaterialIndex >= 0)
			{
				PrimaryMaterialIndex = Sample.MaterialIndex;

				// Extract material alpha for occlusion classification
				const auto* Materials = MeshesRef->materials();
				if (Materials && Sample.MaterialIndex < static_cast<int32>(Materials->size()))
				{
					const Material* Mat = Materials->Get(Sample.MaterialIndex);
					if (Mat)
					{
						PrimaryMaterialAlpha = Mat->a();
					}
				}
			}

			// Get bounding box from representation
			const BoundingBox& bbox = Rep->bbox();
			const FloatVector& min = bbox.min();
			const FloatVector& max = bbox.max();

			// Convert FlatBuffers bbox to Unreal FBox Note: Coordinate transform (x,y,z) → (x*100, z*100, y*100).
			FVector Min(
				min.x() * 100.0f,
				min.z() * 100.0f,
				min.y() * 100.0f
			);
			FVector Max(
				max.x() * 100.0f,
				max.z() * 100.0f,
				max.y() * 100.0f
			);

			FBox RepBounds(Min, Max);

			// Apply fragment's global transform to bounding box
			FTransform GlobalTransform = Item.GlobalTransform;
			FBox TransformedBounds = RepBounds.TransformBy(GlobalTransform);

			// Combine with other representations
			if (bHasValidBounds)
			{
				VisData.WorldBounds += TransformedBounds;
			}
			else
			{
				VisData.WorldBounds = TransformedBounds;
				bHasValidBounds = true;
			}
		}

		if (bHasValidBounds)
		{
			// Calculate max dimension for screen size calculation
			const FVector Extent = VisData.WorldBounds.GetExtent();
			VisData.MaxDimension = FMath::Max3(Extent.X, Extent.Y, Extent.Z) * 2.0f;

			// Classify as small or large object
			VisData.bIsSmallObject = VisData.MaxDimension < SmallObjectSize;

			// Store material index and alpha
			VisData.MaterialIndex = PrimaryMaterialIndex;
			VisData.MaterialAlpha = PrimaryMaterialAlpha;

			// Classify occlusion role based on category and material transparency
			VisData.OcclusionRole = UFragmentOcclusionClassifier::ClassifyFragment(
				Item.Category, PrimaryMaterialAlpha);

			// Add to registry
			const int32 Index = Fragments.Num();
			Fragments.Add(VisData);
			LocalIdToIndex.Add(Item.LocalId, Index);

			// DEBUG: Log bounds for first few fragments to validate coordinate system
			if (Index < 5)
			{
				UE_LOG(LogFragmentRegistry, Log,
				       TEXT("Fragment %lld bounds: Min=%s Max=%s Center=%s MaxDim=%.1f OcclusionRole=%s"),
				       Item.LocalId,
				       *VisData.WorldBounds.Min.ToString(),
				       *VisData.WorldBounds.Max.ToString(),
				       *VisData.WorldBounds.GetCenter().ToString(),
				       VisData.MaxDimension,
				       *UFragmentOcclusionClassifier::GetOcclusionRoleString(VisData.OcclusionRole));
			}
		}
		else
		{
			// Fallback to position-based point if no bbox available
			FVector Position = Item.GlobalTransform.GetLocation();
			FBox PointBounds(Position, Position);
			VisData.WorldBounds = PointBounds.ExpandBy(50.0f); // 50cm safety margin

			const FVector Extent = VisData.WorldBounds.GetExtent();
			VisData.MaxDimension = FMath::Max3(Extent.X, Extent.Y, Extent.Z) * 2.0f;
			VisData.bIsSmallObject = true; // Point-based = small
			VisData.MaterialIndex = 0;
			VisData.MaterialAlpha = PrimaryMaterialAlpha;

			// Classify occlusion role (fallback fragments are typically small occludees)
			VisData.OcclusionRole = UFragmentOcclusionClassifier::ClassifyFragment(
				Item.Category, PrimaryMaterialAlpha);

			const int32 Index = Fragments.Num();
			Fragments.Add(VisData);
			LocalIdToIndex.Add(Item.LocalId, Index);

			UE_LOG(LogFragmentRegistry, Warning, TEXT("Fragment %lld has no valid bbox, using position fallback"),
			       Item.LocalId);
		}
	}

	// Recurse to children
	for (FFragmentItem* Child : Item.FragmentChildren)
	{
		if (Child)
		{
			CollectFragmentData(*Child, ParsedModel);
		}
	}
}

const FFragmentVisibilityData* UFragmentRegistry::FindFragment(int64 LocalId) const
{
	const int32* IndexPtr = LocalIdToIndex.Find(LocalId);
	if (IndexPtr && *IndexPtr >= 0 && *IndexPtr < Fragments.Num())
	{
		return &Fragments[*IndexPtr];
	}
	return nullptr;
}

int32 UFragmentRegistry::GetFragmentIndex(int64 LocalId) const
{
	const int32* IndexPtr = LocalIdToIndex.Find(LocalId);
	return IndexPtr ? *IndexPtr : INDEX_NONE;
}

int64 UFragmentRegistry::GetMemoryUsage() const
{
	int64 TotalBytes = 0;

	// Array storage (FFragmentVisibilityData is approximately 128 bytes due to FString members)
	TotalBytes += Fragments.GetAllocatedSize();
	TotalBytes += sizeof(FFragmentVisibilityData) * Fragments.Num();

	// String storage estimate (GlobalId + Category)
	for (const FFragmentVisibilityData& Data : Fragments)
	{
		TotalBytes += Data.GlobalId.GetAllocatedSize();
		TotalBytes += Data.Category.GetAllocatedSize();
	}

	// LocalIdToIndex map
	TotalBytes += LocalIdToIndex.GetAllocatedSize();

	return TotalBytes;
}
