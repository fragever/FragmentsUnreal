// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Spatial/FragmentProxyDecimate.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "MeshSimplification.h"
#include "MeshConstraintsUtil.h"
#include "IndexTypes.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Utils/FragmentsLog.h"

using namespace UE::Geometry;

namespace FragmentProxyDecimate
{
	bool DecimateSection(FDecimateSection& InOut, float Ratio, float WeldToleranceCm)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FragProxyDecimate_DecimateSection);

		const int32 OrigTris = InOut.Indices.Num() / 3;
		const int32 NumVerts = InOut.Positions.Num();
		if (Ratio >= 1.0f || OrigTris < 8 || NumVerts < 3) { return false; }

		const int32 TargetTris = FMath::Max(4, FMath::RoundToInt32(OrigTris * Ratio));
		if (OrigTris <= TargetTris) { return false; }

		// Build a raw FDynamicMesh3 (no UObject) from the section POD
		FDynamicMesh3 Mesh;
		for (int32 i = 0; i < NumVerts; ++i)
		{
			Mesh.AppendVertex(FVector3d(InOut.Positions[i]));
		}
		for (int32 t = 0; t + 2 < InOut.Indices.Num(); t += 3)
		{
			Mesh.AppendTriangle(FIndex3i(
				(int32)InOut.Indices[t], (int32)InOut.Indices[t + 1], (int32)InOut.Indices[t + 2]));
		}

		Mesh.EnableAttributes();                              // PrimaryUV[0] + PrimaryNormals
		FDynamicMeshAttributeSet* Attr = Mesh.Attributes();
		Attr->EnableTangents();                               // PrimaryTangents + PrimaryBiTangents
		Attr->EnablePrimaryColors();                          // PrimaryColors

		FDynamicMeshUVOverlay*     UVOv = Attr->PrimaryUV();
		FDynamicMeshNormalOverlay* NOv  = Attr->PrimaryNormals();
		FDynamicMeshNormalOverlay* TOv  = Attr->PrimaryTangents();
		FDynamicMeshNormalOverlay* BOv  = Attr->PrimaryBiTangents();
		FDynamicMeshColorOverlay*  COv  = Attr->PrimaryColors();

		// One overlay element per source vertex (1:1) — element id == vertex id on a fresh mesh.
		for (int32 i = 0; i < NumVerts; ++i)
		{
			const FVector3f N = InOut.Normals.IsValidIndex(i) ? InOut.Normals[i] : FVector3f(0, 0, 1);
			const FVector3f T = InOut.TangentsX.IsValidIndex(i) ? InOut.TangentsX[i] : FVector3f(1, 0, 0);
			const float     S = InOut.TangentYSigns.IsValidIndex(i) ? InOut.TangentYSigns[i] : 1.0f;
			const FVector3f B = FVector3f::CrossProduct(N, T) * S;   // encode handedness
			const FVector2f UV = InOut.UV0.IsValidIndex(i) ? InOut.UV0[i] : FVector2f::ZeroVector;
			const FColor    C = InOut.VertexColors.IsValidIndex(i) ? InOut.VertexColors[i] : FColor::White;

			UVOv->AppendElement(UV);
			NOv->AppendElement(N);
			TOv->AppendElement(T);
			BOv->AppendElement(B);
			COv->AppendElement(FVector4f(C.R / 255.0f, C.G / 255.0f, C.B / 255.0f, C.A / 255.0f));
		}
		for (int32 tid : Mesh.TriangleIndicesItr())
		{
			const FIndex3i Tri = Mesh.GetTriangle(tid);          // element ids == vertex ids
			UVOv->SetTriangle(tid, Tri);
			NOv->SetTriangle(tid, Tri);
			TOv->SetTriangle(tid, Tri);
			BOv->SetTriangle(tid, Tri);
			COv->SetTriangle(tid, Tri);
		}

		// Reconnect split render vertices before simplifying the triangle soup.
		if (WeldToleranceCm > 0.0f)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FragProxyDecimate_WeldSoup);
			FMergeCoincidentMeshEdges Welder(&Mesh);
			Welder.MergeVertexTolerance    = WeldToleranceCm;
			Welder.MergeSearchTolerance    = WeldToleranceCm * 2.0;
			Welder.bWeldAttrsOnMergedEdges = false;
			Welder.Apply();

			// Repair bowties caused when welding mesh vertices leaves attribute overlays split.
			if (Mesh.HasAttributes())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(FragProxyDecimate_SplitAllBowties);

				const int32 Before = UVOv->ElementCount() + NOv->ElementCount()
					+ TOv->ElementCount() + BOv->ElementCount() + COv->ElementCount();

				Attr->SplitAllBowties();

				const int32 After = UVOv->ElementCount() + NOv->ElementCount()
					+ TOv->ElementCount() + BOv->ElementCount() + COv->ElementCount();

				// Self-reporting: by hypothesis (weld scoped to one element's own geometry) this never fires.
				if (After > Before)
				{
					UE_LOG(LogFragments, Warning,
						TEXT("[Decimate] SplitAllBowties split %d overlay element(s) on a %d-tri "
						     "section after welding. The template-scope no-bowtie assumption does "
						     "NOT hold."),
						After - Before, OrigTris);
				}
			}
		}

		// Attribute-aware simplify (mirrors GeometryScript DoSimplifyMesh<FAttrMeshSimplification>)
		FAttrMeshSimplification Simplifier(&Mesh);
		Simplifier.DEBUG_CHECK_LEVEL = 0;
		Simplifier.bAllowSeamCollapse = true;
		Simplifier.SetEdgeFlipTolerance(1.e-5);

		FMeshConstraints Constraints;
		FMeshConstraintsUtil::ConstrainAllBoundariesAndSeams(
			Constraints, Mesh,
			EEdgeRefineFlags::NoFlip,        // mesh boundary
			EEdgeRefineFlags::NoConstraint,  // group boundary
			EEdgeRefineFlags::NoConstraint,  // material boundary
			/*bAllowSeamSplits*/ true,
			/*bAllowSeamSmoothing*/ false,
			/*bAllowSeamCollapse*/ true);
		Simplifier.SetExternalConstraints(MoveTemp(Constraints));

		Simplifier.SimplifyToTriangleCount(FMath::Max(4, TargetTris));
		Mesh.CompactInPlace();

		// Read back per-corner (non-indexed): exact seam preservation, no welding.
		FDecimateSection Out;
		const int32 OutTris = Mesh.TriangleCount();
		Out.Positions.Reserve(OutTris * 3);
		Out.Normals.Reserve(OutTris * 3);
		Out.TangentsX.Reserve(OutTris * 3);
		Out.TangentYSigns.Reserve(OutTris * 3);
		Out.UV0.Reserve(OutTris * 3);
		Out.VertexColors.Reserve(OutTris * 3);
		Out.Indices.Reserve(OutTris * 3);

		uint32 NextIndex = 0;
		for (int32 tid : Mesh.TriangleIndicesItr())
		{
			const FIndex3i TriV = Mesh.GetTriangle(tid);
			const bool bUV = UVOv->IsSetTriangle(tid);
			const bool bN  = NOv->IsSetTriangle(tid);
			const bool bT  = TOv->IsSetTriangle(tid);
			const bool bB  = BOv->IsSetTriangle(tid);
			const bool bC  = COv->IsSetTriangle(tid);
			const FIndex3i UVe = bUV ? UVOv->GetTriangle(tid) : FIndex3i::Zero();
			const FIndex3i Ne  = bN  ? NOv->GetTriangle(tid)  : FIndex3i::Zero();
			const FIndex3i Te  = bT  ? TOv->GetTriangle(tid)  : FIndex3i::Zero();
			const FIndex3i Be  = bB  ? BOv->GetTriangle(tid)  : FIndex3i::Zero();
			const FIndex3i Ce  = bC  ? COv->GetTriangle(tid)  : FIndex3i::Zero();

			for (int32 k = 0; k < 3; ++k)
			{
				Out.Positions.Add(FVector3f(Mesh.GetVertex(TriV[k])));

				FVector3f N(0, 0, 1);
				if (bN) { NOv->GetElement(Ne[k], N); }
				N = N.GetSafeNormal(1e-6f, FVector3f(0, 0, 1));
				Out.Normals.Add(N);

				FVector3f T(1, 0, 0);
				if (bT) { TOv->GetElement(Te[k], T); }
				T = T.GetSafeNormal(1e-6f, FVector3f(1, 0, 0));
				Out.TangentsX.Add(T);

				FVector3f B(0, 1, 0);
				if (bB) { BOv->GetElement(Be[k], B); }
				const float Sign = (FVector3f::DotProduct(FVector3f::CrossProduct(N, T), B) >= 0.0f) ? 1.0f : -1.0f;
				Out.TangentYSigns.Add(Sign);

				FVector2f UV(0, 0);
				if (bUV) { UVOv->GetElement(UVe[k], UV); }
				Out.UV0.Add(UV);

				FVector4f Col(1, 1, 1, 1);
				if (bC) { COv->GetElement(Ce[k], Col); }
				Out.VertexColors.Add(FColor(
					FMath::Clamp(FMath::RoundToInt32(Col.X * 255.0f), 0, 255),
					FMath::Clamp(FMath::RoundToInt32(Col.Y * 255.0f), 0, 255),
					FMath::Clamp(FMath::RoundToInt32(Col.Z * 255.0f), 0, 255),
					FMath::Clamp(FMath::RoundToInt32(Col.W * 255.0f), 0, 255)));

				Out.Indices.Add(NextIndex++);
			}
		}

		if (Out.Indices.Num() < 3) { return false; }   // degenerate result — leave InOut untouched

		InOut = MoveTemp(Out);
		return true;
	}

	FDecimateSettings ResolveSettings(int32 CVarEnable, float CVarRatio, bool PropEnabled, float PropRatio)
	{
		FDecimateSettings Out;
		Out.bEnabled = (CVarEnable >= 0) ? (CVarEnable != 0) : PropEnabled;
		const float Ratio = (CVarRatio > 0.0f) ? CVarRatio : PropRatio;
		Out.Ratio = FMath::Clamp(Ratio, 0.05f, 1.0f);
		return Out;
	}
}
