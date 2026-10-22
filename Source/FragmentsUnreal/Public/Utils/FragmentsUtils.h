// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant



#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsLog.h"
#include "Utils/FragmentCellPartition.h"
#include "FragmentsUtils.generated.h"

struct FFragmentEdge
{
	float A, B;

	FFragmentEdge(float InA, float InB)
	{
		A = InA;
		B = InB;
	}

	bool operator==(const FFragmentEdge& Other) const
	{
		return (A == Other.A && B == Other.B) || (A == Other.B && B == Other.A);
	}
};

struct FPlaneProjection
{
	FVector Origin;
	FVector AxisX;
	FVector AxisY;

	FVector2D Project(const FVector& Point) const
	{
		FVector Local = Point - Origin;
		return FVector2D(FVector::DotProduct(Local, AxisX), FVector::DotProduct(Local, AxisY));
	}

	FVector Unproject(const FVector2D& Point2D) const
	{
		return Origin + AxisX * Point2D.X + AxisY * Point2D.Y;
	}
};

struct FTriangulationResult
{
	TArray<FVector> FlattenedPoints;
	TArray<int32> TriangleIndices;
};

/** Single vertex in the intermediate mesh format. */
struct FFragmentMeshVertex
{
	FVector3f Position;
	FVector3f Normal;
	FVector3f TangentX;
	float TangentYSign;  // +1 or -1, used to reconstruct bitangent via cross(Normal, TangentX) * sign
	FVector2f UV0;
	FColor Color;
};

/** Thread-safe intermediate mesh data produced by BuildShellMeshDataFromFlatBuffers. */
struct FFragmentMeshData
{
	TArray<FFragmentMeshVertex> Vertices;
	TArray<uint32> Indices;  // Triangle list. uint32 because GPU only supports uint16/uint32, and meshes can exceed 65K verts.
	FBox3f BoundingBox;
	FString AssetName;
	int32 RepresentationId = -1;
	bool bIsValid = false;

	void Reset()
	{
		Vertices.Reset();
		Indices.Reset();
		BoundingBox.Init();
		AssetName.Reset();
		RepresentationId = -1;
		bIsValid = false;
	}
};

/** One material section within a proxy mesh. */
struct FProxyMeshSection
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> TangentsX;      // world-space (transformed at accumulation, like Normals)
	TArray<float>     TangentYSigns;  // carried from source (handedness); +1/-1
	TArray<FVector2f> UV0;            // local-space surface UVs, carried untransformed (matches LOD1)
	TArray<FColor> VertexColors;
	TArray<uint32> Indices;
	uint32 MaterialCRC = 0;
	UMaterialInterface* Material = nullptr;

	int32 VertexCount() const { return Positions.Num(); }
};

/** Accumulates shell geometry during import for proxy mesh generation. */
struct FProxyMeshAccumulator
{
	/** Material sections keyed by CRC hash from MaterialPool. */
	TMap<uint32, FProxyMeshSection> Sections;

	/** Bounding box accumulator — expanded with each AddGeometry call. */
	FBox3f BoundingBox;

	FProxyMeshAccumulator()
	{
		BoundingBox.Init();
	}

	/** Add a shell item's geometry to the accumulator. */
	void AddGeometry(const FFragmentMeshData& MeshData,
	                 const FTransform& WorldTransform,
	                 uint32 MaterialCRC,
	                 UMaterialInterface* Material)
	{
		if (!MeshData.bIsValid || MeshData.Vertices.Num() == 0) return;

		FProxyMeshSection& Section = Sections.FindOrAdd(MaterialCRC);
		if (!Section.Material)
		{
			Section.MaterialCRC = MaterialCRC;
			Section.Material = Material;
		}

		const int32 IndexOffset = Section.VertexCount();
		const int32 NumVerts = MeshData.Vertices.Num();

		Section.Positions.Reserve(Section.Positions.Num() + NumVerts);
		Section.Normals.Reserve(Section.Normals.Num() + NumVerts);
		Section.TangentsX.Reserve(Section.TangentsX.Num() + NumVerts);
		Section.TangentYSigns.Reserve(Section.TangentYSigns.Num() + NumVerts);
		Section.UV0.Reserve(Section.UV0.Num() + NumVerts);
		Section.VertexColors.Reserve(Section.VertexColors.Num() + NumVerts);

		for (int32 i = 0; i < NumVerts; i++)
		{
			const FFragmentMeshVertex& V = MeshData.Vertices[i];
			const FVector3f TransformedPos = FVector3f(WorldTransform.TransformPosition(FVector(V.Position)));
			const FVector3f TransformedNormal = FVector3f(WorldTransform.TransformVectorNoScale(FVector(V.Normal)));
			const FVector3f TransformedTangent = FVector3f(WorldTransform.TransformVectorNoScale(FVector(V.TangentX)));

			Section.Positions.Add(TransformedPos);
			Section.Normals.Add(TransformedNormal);
			// UV0 carried untransformed (surface coords, projected local-space upstream); tangents world-transformed like normals — mirrors LOD1's frames.
			Section.TangentsX.Add(TransformedTangent);
			Section.TangentYSigns.Add(V.TangentYSign);
			Section.UV0.Add(V.UV0);
			Section.VertexColors.Add(V.Color);
			BoundingBox += TransformedPos;
		}

		const int32 NumIndices = MeshData.Indices.Num();
		Section.Indices.Reserve(Section.Indices.Num() + NumIndices);
		for (int32 i = 0; i < NumIndices; i++)
		{
			Section.Indices.Add(MeshData.Indices[i] + IndexOffset);
		}
	}

	bool IsEmpty() const { return Sections.Num() == 0; }

	/** Total vertices across all sections — for logging. */
	int32 TotalVertexCount() const
	{
		int32 Total = 0;
		for (const auto& [CRC, Section] : Sections)
		{
			Total += Section.VertexCount();
		}
		return Total;
	}

	/** Total triangles across all sections — for logging. */
	int32 TotalTriangleCount() const
	{
		int32 Total = 0;
		for (const auto& [CRC, Section] : Sections)
		{
			Total += Section.Indices.Num() / 3;
		}
		return Total;
	}

	void Reset()
	{
		Sections.Reset();
		BoundingBox.Init();
	}
};

/** Origin interpretation mode for LoadFragment. */
UENUM(BlueprintType)
enum class EFragmentOriginMode : uint8
{
	// Rebase all models to the first-loaded model's origin (existing co-dev workflow).
	UseBaseCoordinates UMETA(DisplayName = "Use Base Coordinates"),

	// Neutralize each model's Revit root independently; place manually in world space.
	UseWorldOrigin UMETA(DisplayName = "Use World Origin"),
};

/** Per-load options for UFragmentsImporter::LoadFragment. */
USTRUCT(BlueprintType)
struct FRAGMENTSUNREAL_API FFragmentLoadOptions
{
	GENERATED_BODY()

	// How to interpret the fragment's Revit-native coordinates relative to the Unreal scene.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment Load Options")
	EFragmentOriginMode OriginMode = EFragmentOriginMode::UseBaseCoordinates;

	// World-space offset/rotation applied after origin placement. Identity = no-op.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment Load Options")
	FTransform PlacementTransform = FTransform::Identity;

	// If not empty, these prefixes replace the default shell/proxy category list for this model.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fragment Load Options")
	TArray<FString> ShellCategoryOverridePrefixes;
};

USTRUCT(BlueprintType)
struct FFragmentLookup
{
	GENERATED_BODY()

public:

	UPROPERTY()
	TMap<int64, class AFragment*> Fragments;
};

USTRUCT(BlueprintType)
struct FFragmentSample
{
	GENERATED_BODY()

	// Original indices (kept for debugging and backward compatibility)
	UPROPERTY()
	int64 SampleIndex = -1;
	UPROPERTY()
	int64 LocalTransformIndex = -1;
	UPROPERTY()
	int64 RepresentationIndex = -1;
	UPROPERTY()
	int64 MaterialIndex = -1;
};


USTRUCT(BlueprintType)
struct FItemAttribute
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FString Key;

	UPROPERTY(BlueprintReadOnly)
	FString Value;

	UPROPERTY(BlueprintReadOnly)
	FString PropertySet;

	UPROPERTY(BlueprintReadOnly)
	int64 TypeHash;

	FItemAttribute() : Key(TEXT("")), Value(TEXT("")), PropertySet(TEXT("")), TypeHash(0) {}
	FItemAttribute(const FString& InKey, const FString& InValue, const FString& InPropertySet, int64 InTypeHash)
		:Key(InKey), Value(InValue), PropertySet(InPropertySet), TypeHash(InTypeHash) {}
	FItemAttribute(const FString& InKey, const FString& InValue, int64 InTypeHash)
		:Key(InKey), Value(InValue), PropertySet(TEXT("")), TypeHash(InTypeHash) {}
};

/** One property set (named group) and the attributes that belong to it. */
USTRUCT(BlueprintType)
struct FFragmentPropertySetGroup
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	TArray<FItemAttribute> Rows;
};

USTRUCT(BlueprintType)
struct FFragmentElementInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int64 LocalId = -1;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString GlobalId;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Category;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 FloorIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString ModelGuid;

	FFragmentElementInfo() = default;
	FFragmentElementInfo(int64 InLocalId, const FString& InGlobalId, const FString& InCategory, const FString& InName, int32 InFloorIndex, const FString& InModelGuid)
		: LocalId(InLocalId), GlobalId(InGlobalId), Category(InCategory), Name(InName), FloorIndex(InFloorIndex), ModelGuid(InModelGuid) {}
};

/** Combined element identity + all IFC properties in a single Blueprint-friendly struct. */
USTRUCT(BlueprintType)
struct FFragmentElementDetail
{
	GENERATED_BODY()

	/** Whether this detail was successfully resolved */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bValid = false;

	/** Element identity */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int64 LocalId = -1;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString GlobalId;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Category;

	/** Human-readable name (IFC "Name" attribute). Empty if the element has none. */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Name;

	/** Floor/level index, or INDEX_NONE (-1) if the element isn't floor-partitioned. */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 FloorIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString ModelGuid;

	/** All IFC properties (flat key/value list) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	TArray<FItemAttribute> Attributes;

	/** Property-set attributes as a flat list (each carries its PropertySet name) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	TArray<FItemAttribute> PropertySets;

	/** Same property-set attributes pre-grouped by set name (first-seen order). */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	TArray<FFragmentPropertySetGroup> PropertySetGroups;

	FFragmentElementDetail() = default;
};

USTRUCT()
struct FFragmentItem
{

	GENERATED_BODY()

	FString ModelGuid;
	int64 LocalId = -1;
	FString Category;
	FString Guid;
	TArray<FItemAttribute> Attributes;  // List of attributes for the fragment
	TArray <FFragmentItem*> FragmentChildren;  // Use TObjectPtr for nested recursion
	TArray<FFragmentSample> Samples;
	FTransform GlobalTransform;
};

// Forward declaration for FFindResult
class AFragment;

/** Lightweight proxy for instanced BIM elements. ~200 bytes vs ~5KB for AFragment actor. */
USTRUCT(BlueprintType)
struct FFragmentProxy
{
	GENERATED_BODY()

	/** Weak reference to the HISMC containing this instance */
	UPROPERTY()
	TWeakObjectPtr<class UHierarchicalInstancedStaticMeshComponent> ISMC;

	/** Index of this instance within the ISMC */
	UPROPERTY()
	int32 InstanceIndex = INDEX_NONE;

	/** LocalId of the fragment in the BIM model */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int64 LocalId = INDEX_NONE;

	/** Global unique ID (GUID) of the fragment */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString GlobalId;

	/** IFC category (e.g., IfcDoor, IfcWindow) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Category;

	/** Model GUID this fragment belongs to */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString ModelGuid;

	/** Attributes/properties of the BIM element */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	TArray<FItemAttribute> Attributes;

	/** Parent fragment LocalId (INDEX_NONE if root) */
	UPROPERTY()
	int64 ParentLocalId = INDEX_NONE;

	/** Child fragment LocalIds */
	UPROPERTY()
	TArray<int64> ChildLocalIds;

	/** Cached world transform of this instance */
	UPROPERTY()
	FTransform WorldTransform;

	FFragmentProxy() = default;
};

/** Pending HISM item queued for frame-budgeted spawning */
struct FPendingHISMItem
{
	int32 RepresentationIndex = INDEX_NONE;
	int32 MaterialIndex = INDEX_NONE;
	int32 LocalTransformIndex = INDEX_NONE;
	int64 LocalId = INDEX_NONE;
	int64 FloorKey = INDEX_NONE;

	/** QueueIndex is unique per sample even when samples share a LocalId. */
	int32 QueueIndex = INDEX_NONE;
	FTransform WorldTransform;
	FString Category;
	FString ModelGuid;

	/** MeshCache/InFlightShellBuilds key for this item's representation, e.g. "/Game/Buildings/<ModelGuid>/rep_<RepId>.rep_<RepId>". */
	FString SamplePath;
	uint8 R = 255, G = 255, B = 255, A = 255;
	bool bIsGlass = false;

	/** Set by the cell partition pass during ProcessHISMSpawnChunk. */
	FCellId Cell = FCellId::Sentinel();

	/** Cached IsShellCategory(Category) result, set during cell partition. */
	bool bShell = true;

	FPendingHISMItem() = default;
};

/** Returns true if the IFC category is a structural shell element that should be loaded immediately (walls, slabs, roofs, columns, etc.). */
inline bool IsShellCategory(const FString& Category)
{
	// Prefix match — catches variants like IfcWallStandardCase, IfcSlabElementedCase, etc.
	static const TArray<FString> ShellPrefixes = {
		TEXT("IfcWall"),
		TEXT("IfcSlab"),
		TEXT("IfcRoof"),
		TEXT("IfcCurtainWall"),
		TEXT("IfcColumn"),
		TEXT("IfcBeam"),
		TEXT("IfcStair"),
		TEXT("IfcRamp"),
		TEXT("IfcPlate"),
		TEXT("IfcMember"),
		TEXT("IfcBuildingStorey"),
	};

	for (const FString& Prefix : ShellPrefixes)
	{
		if (Category.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

/** Unified lookup result for both actors and instanced proxies. */
USTRUCT(BlueprintType)
struct FRAGMENTSUNREAL_API FFindResult
{
	GENERATED_BODY()

	/** Whether a fragment was found */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bFound = false;

	/** Whether the fragment is GPU-instanced (true) or an actor (false) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bIsInstanced = false;

	/** Fragment actor (valid if bIsInstanced == false) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	AFragment* Fragment = nullptr;

	/** Proxy data (valid if bIsInstanced == true) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FFragmentProxy Proxy;

	/** Get LocalId regardless of instancing */
	int64 GetLocalId() const;

	/** Get Category regardless of instancing */
	FString GetCategory() const;

	/** Get world transform regardless of instancing */
	FTransform GetWorldTransform() const;

	/** Create a not-found result */
	static FFindResult NotFound();

	/** Create a result from an actor */
	static FFindResult FromActor(AFragment* Actor);

	/** Create a result from a proxy */
	static FFindResult FromProxy(const FFragmentProxy& InProxy);

	FFindResult() = default;
};

UCLASS()
class FRAGMENTSUNREAL_API UFragmentsUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// UFUNCTION(BlueprintCallable, Category = "Fragments")
	static FTransform MakeTransform(const Transform* FragmentsTransform, bool bIsLocalTransform = false, bool bIsRootCoordinates = false);
	static FPlaneProjection BuildProjectionPlane(const TArray<FVector>& Points, const TArray<int32>& Profile);
	static bool IsClockwise(const TArray<FVector2D>& Points);
	static TArray<FItemAttribute> ParseItemAttribute(const Attribute* Attr);
	static class AFragment* MapModelStructure(const SpatialStructure* InS, AFragment*& ParentActor, TMap<int64, AFragment*>& FragmentLookupMapRef, const FString& InheritedCategory);
	static void MapModelStructureToData(const SpatialStructure* InS, FFragmentItem& ParentItem, const FString& InheritedCategory);
	static FString GetIfcCategory(const int64 InTypeHash);
	static float SafeComponent(float Value);
	static FVector SafeVector(const FVector& Vec);
	static FVector SafeAxis(const FVector& V, const FVector& Fallback, float Eps = 1e-8f);
	static FRotator SafeRotator(const FRotator& Rot);
	static int64 GetIndexForLocalId(const Model* InModelRef, int64 LocalId);

	/** Indexes an FFragmentItem hierarchy by LocalId (iterative, includes Root). */
	static void BuildLocalIdToItemMap(FFragmentItem& Root, TMap<int64, FFragmentItem*>& OutMap);
	static TArray<FItemAttribute> ParsePropertySets(const TArray<FItemAttribute>& InAttributes);

private:
	static bool IsValueKey(const FString& Key);
	static FTransform BuildFromXY(const FVector& XIn, const FVector& YIn, const FVector& PostCm);
	static FTransform BuildFromXZ(const FVector& XIn, const FVector& ZIn, const FVector& PostCm);

};
