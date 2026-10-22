// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentProxyTiers.h"
#include "Utils/FragmentsUtils.h"

float FragmentProxyTiers::LocalRadiusCm(TConstArrayView<FVector3f> Positions)
{
	if (Positions.Num() == 0) { return 0.0f; }

	FVector3f Min = Positions[0];
	FVector3f Max = Min;
	for (const FVector3f& P : Positions)
	{
		Min = Min.ComponentMin(P);
		Max = Max.ComponentMax(P);
	}
	// Half the AABB diagonal — the bounding-sphere radius of the box, which over-estimates thin elements and therefore errs toward keeping geometry.
	return ((Max - Min) * 0.5f).Size();
}

float FragmentProxyTiers::LocalRadiusCm(const FFragmentMeshData& MeshData)
{
	if (!MeshData.bIsValid || MeshData.Vertices.Num() == 0) { return 0.0f; }

	TArray<FVector3f> Positions;
	Positions.Reserve(MeshData.Vertices.Num());
	for (const FFragmentMeshVertex& V : MeshData.Vertices) { Positions.Add(V.Position); }
	return LocalRadiusCm(Positions);
}
