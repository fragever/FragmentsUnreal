// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Spatial/FragmentProxyDecimate.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Build a closed box whose six faces are authored as SEPARATE vertex blocks — no vertex is shared across a face seam. */
	FragmentProxyDecimate::FDecimateSection BuildUnweldedBox(float HalfSize, int32 CellsPerSide)
	{
		using namespace FragmentProxyDecimate;
		FDecimateSection S;

		// Six faces: axis index (0=X,1=Y,2=Z) and which side (+1/-1).
		const int32 Axis[6] = { 0, 0, 1, 1, 2, 2 };
		const float Side[6] = { +1, -1, +1, -1, +1, -1 };

		const int32 N = CellsPerSide + 1;   // verts per row

		for (int32 F = 0; F < 6; ++F)
		{
			const int32 A = Axis[F];
			const int32 U = (A + 1) % 3;
			const int32 V = (A + 2) % 3;
			const int32 Base = S.Positions.Num();   // this face's own vertex block

			FVector3f Normal(0, 0, 0);
			Normal[A] = Side[F];

			FVector3f Tangent(0, 0, 0);
			Tangent[U] = 1.0f;

			for (int32 iv = 0; iv < N; ++iv)
			{
				for (int32 iu = 0; iu < N; ++iu)
				{
					const float u = -HalfSize + (2.0f * HalfSize) * (iu / float(CellsPerSide));
					const float v = -HalfSize + (2.0f * HalfSize) * (iv / float(CellsPerSide));

					FVector3f P(0, 0, 0);
					P[A] = Side[F] * HalfSize;
					P[U] = u;
					P[V] = v;

					S.Positions.Add(P);
					S.Normals.Add(Normal);
					S.TangentsX.Add(Tangent);
					S.TangentYSigns.Add(1.0f);
					S.UV0.Add(FVector2f(iu / float(CellsPerSide), iv / float(CellsPerSide)));
					S.VertexColors.Add(FColor(200, 200, 200, 255));
				}
			}

			// Two triangles per cell, wound so the face points along +Side (outward).
			for (int32 iv = 0; iv < CellsPerSide; ++iv)
			{
				for (int32 iu = 0; iu < CellsPerSide; ++iu)
				{
					const uint32 V00 = (uint32)(Base + iv * N + iu);
					const uint32 V10 = V00 + 1;
					const uint32 V01 = (uint32)(Base + (iv + 1) * N + iu);
					const uint32 V11 = V01 + 1;

					// Winding flips with the sign of the face so all normals point outward.
					if (Side[F] > 0)
					{
						S.Indices.Add(V00); S.Indices.Add(V10); S.Indices.Add(V11);
						S.Indices.Add(V00); S.Indices.Add(V11); S.Indices.Add(V01);
					}
					else
					{
						S.Indices.Add(V00); S.Indices.Add(V11); S.Indices.Add(V10);
						S.Indices.Add(V00); S.Indices.Add(V01); S.Indices.Add(V11);
					}
				}
			}
		}
		return S;
	}

	/** Quantize a position to a 1e-3 cm lattice so identical corners hash identically. */
	FIntVector3 QuantizePos(const FVector3f& P)
	{
		return FIntVector3(
			FMath::RoundToInt32(P.X * 1000.0f),
			FMath::RoundToInt32(P.Y * 1000.0f),
			FMath::RoundToInt32(P.Z * 1000.0f));
	}

	/** Count how many undirected edges of the triangle soup are NOT shared by exactly two triangles, keying edges by quantized endpoint POSITION rather than index. */
	int32 CountNonManifoldPositionEdges(const FragmentProxyDecimate::FDecimateSection& S)
	{
		TMap<TPair<FIntVector3, FIntVector3>, int32> EdgeCounts;
		EdgeCounts.Reserve(S.Indices.Num());

		for (int32 t = 0; t + 2 < S.Indices.Num(); t += 3)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& PA = S.Positions[S.Indices[t + k]];
				const FVector3f& PB = S.Positions[S.Indices[t + ((k + 1) % 3)]];

				FIntVector3 QA = QuantizePos(PA);
				FIntVector3 QB = QuantizePos(PB);
				if (QA == QB) { continue; }   // degenerate edge, not a hole

				// Canonical ordering so the two sides of a shared edge hash to one key.
				const bool bSwap =
					(QB.X < QA.X) ||
					(QB.X == QA.X && QB.Y < QA.Y) ||
					(QB.X == QA.X && QB.Y == QA.Y && QB.Z < QA.Z);
				if (bSwap) { Swap(QA, QB); }

				EdgeCounts.FindOrAdd(TPair<FIntVector3, FIntVector3>(QA, QB), 0)++;
			}
		}

		int32 NumOpen = 0;
		for (const TPair<TPair<FIntVector3, FIntVector3>, int32>& E : EdgeCounts)
		{
			if (E.Value != 2) { ++NumOpen; }
		}
		return NumOpen;
	}

	FBox3f BoundsOf(const FragmentProxyDecimate::FDecimateSection& S)
	{
		FBox3f B(ForceInit);
		for (const FVector3f& P : S.Positions) { B += P; }
		return B;
	}
}

/** The hole bug, reduced: decimating a closed-but-unwelded surface must not open it up. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimateUnweldedBoxStaysClosedTest,
	"FragmentsUnreal.ProxyDecimate.UnweldedBoxStaysClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimateUnweldedBoxStaysClosedTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	const float HalfSize = 100.0f;
	FDecimateSection S = BuildUnweldedBox(HalfSize, /*CellsPerSide*/ 6);   // 6 * 72 = 432 tris

	const int32 OrigTris = S.Indices.Num() / 3;
	TestEqual(TEXT("fixture triangle count"), OrigTris, 432);

	// Fixture sanity: closed as a surface, but shattered as topology (6 disjoint vertex blocks).
	TestEqual(TEXT("fixture is geometrically closed"), CountNonManifoldPositionEdges(S), 0);
	TestEqual(TEXT("fixture is unwelded (6 separate vertex blocks)"),
		S.Positions.Num(), 6 * 7 * 7);

	const FBox3f OrigBounds = BoundsOf(S);

	// The production proxy ratio.
	const bool bRan = DecimateSection(S, 0.1f, 0.01f);
	TestTrue(TEXT("decimation ran"), bRan);

	const int32 NewTris = S.Indices.Num() / 3;
	TestTrue(FString::Printf(TEXT("triangles reduced (%d -> %d)"), OrigTris, NewTris),
		NewTris < OrigTris);

	// Every repaired seam edge must belong to two triangles.
	const int32 OpenEdges = CountNonManifoldPositionEdges(S);
	TestEqual(
		FString::Printf(TEXT("decimated surface stays closed (%d open edges, %d tris)"),
			OpenEdges, NewTris),
		OpenEdges, 0);

	AddInfo(FString::Printf(
		TEXT("[spike] closed-box: %d -> %d tris (requested 0.1 = %d), %d open edges"),
		OrigTris, NewTris, FMath::RoundToInt32(OrigTris * 0.1f), OpenEdges));

	// And it must not have shrunk: erosion pulls the silhouette in even when it stays closed.
	const FBox3f NewBounds = BoundsOf(S);
	const float Tolerance = HalfSize * 0.05f;
	TestTrue(
		FString::Printf(TEXT("bounds preserved (min %s -> %s, max %s -> %s)"),
			*OrigBounds.Min.ToString(), *NewBounds.Min.ToString(),
			*OrigBounds.Max.ToString(), *NewBounds.Max.ToString()),
		NewBounds.Min.Equals(OrigBounds.Min, Tolerance) &&
		NewBounds.Max.Equals(OrigBounds.Max, Tolerance));

	return true;
}

/** Cause #3, quantified: the triangle target is global across a section, but a section is every instance of every element sharing a material (a whole cell at LOD1, the whole building at LOD2). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimateSmallComponentSurvivalTest,
	"FragmentsUnreal.ProxyDecimate.SmallComponentSurvival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimateSmallComponentSurvivalTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	// One large surface plus eight small detail objects, all in one section — the production grouping.
	FDecimateSection S = BuildUnweldedBox(/*HalfSize*/ 500.0f, /*CellsPerSide*/ 6);

	const int32 NumSmall = 8;
	const float SmallHalf = 15.0f;
	TArray<FVector3f> SmallCenters;
	for (int32 i = 0; i < NumSmall; ++i)
	{
		// Park them well clear of the big box and of each other so component attribution is exact.
		const FVector3f Center(1500.0f + i * 200.0f, 0.0f, 0.0f);
		SmallCenters.Add(Center);

		FDecimateSection Small = BuildUnweldedBox(SmallHalf, /*CellsPerSide*/ 2);
		const uint32 Base = (uint32)S.Positions.Num();
		for (int32 v = 0; v < Small.Positions.Num(); ++v)
		{
			S.Positions.Add(Small.Positions[v] + Center);
			S.Normals.Add(Small.Normals[v]);
			S.TangentsX.Add(Small.TangentsX[v]);
			S.TangentYSigns.Add(Small.TangentYSigns[v]);
			S.UV0.Add(Small.UV0[v]);
			S.VertexColors.Add(Small.VertexColors[v]);
		}
		for (int32 idx = 0; idx < Small.Indices.Num(); ++idx)
		{
			S.Indices.Add(Small.Indices[idx] + Base);
		}
	}

	const int32 OrigTris = S.Indices.Num() / 3;
	const bool bRan = DecimateSection(S, 0.1f, 0.01f);
	TestTrue(TEXT("decimation ran"), bRan);
	const int32 NewTris = S.Indices.Num() / 3;

	// How many of the small objects kept at least one triangle?
	int32 Survivors = 0;
	for (const FVector3f& Center : SmallCenters)
	{
		const FBox3f Box(Center - FVector3f(SmallHalf * 2.0f), Center + FVector3f(SmallHalf * 2.0f));
		bool bAlive = false;
		for (int32 t = 0; t + 2 < S.Indices.Num() && !bAlive; t += 3)
		{
			const FVector3f C = (S.Positions[S.Indices[t]] +
			                     S.Positions[S.Indices[t + 1]] +
			                     S.Positions[S.Indices[t + 2]]) / 3.0f;
			if (Box.IsInside(C)) { bAlive = true; }
		}
		if (bAlive) { ++Survivors; }
	}

	AddInfo(FString::Printf(
		TEXT("[spike] heterogeneous: %d -> %d tris; small components surviving: %d/%d"),
		OrigTris, NewTris, Survivors, NumSmall));

	// What welding guarantees today: reduction happened and the section did not tear itself apart into a soup.
	TestTrue(TEXT("triangles reduced"), NewTris < OrigTris);

	return true;
}

namespace
{
	/** Append Src offset by Center into Dst, keeping it a separate (unwelded) vertex block. */
	void AppendSection(FragmentProxyDecimate::FDecimateSection& Dst,
	                   const FragmentProxyDecimate::FDecimateSection& Src,
	                   const FVector3f& Center)
	{
		const uint32 Base = (uint32)Dst.Positions.Num();
		for (int32 v = 0; v < Src.Positions.Num(); ++v)
		{
			Dst.Positions.Add(Src.Positions[v] + Center);
			Dst.Normals.Add(Src.Normals[v]);
			Dst.TangentsX.Add(Src.TangentsX[v]);
			Dst.TangentYSigns.Add(Src.TangentYSigns[v]);
			Dst.UV0.Add(Src.UV0[v]);
			Dst.VertexColors.Add(Src.VertexColors[v]);
		}
		for (int32 i = 0; i < Src.Indices.Num(); ++i)
		{
			Dst.Indices.Add(Src.Indices[i] + Base);
		}
	}

	/** Longest triangle edge in the section. */
	float MaxEdgeLength(const FragmentProxyDecimate::FDecimateSection& S)
	{
		float MaxLen = 0.0f;
		for (int32 t = 0; t + 2 < S.Indices.Num(); t += 3)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& A = S.Positions[S.Indices[t + k]];
				const FVector3f& B = S.Positions[S.Indices[t + ((k + 1) % 3)]];
				MaxLen = FMath::Max(MaxLen, (A - B).Size());
			}
		}
		return MaxLen;
	}

	/** Two boxes sharing a face exactly — abutting slabs, a panel against its frame, stacked elements. */
	FragmentProxyDecimate::FDecimateSection BuildAbuttingBoxes(int32 NumBoxes, float Half)
	{
		FragmentProxyDecimate::FDecimateSection Out;
		const FragmentProxyDecimate::FDecimateSection Unit = BuildUnweldedBox(Half, /*CellsPerSide*/ 4);
		for (int32 i = 0; i < NumBoxes; ++i)
		{
			// Centers spaced by exactly 2*Half, so neighbouring faces are coincident.
			AppendSection(Out, Unit, FVector3f(i * 2.0f * Half, 0.0f, 0.0f));
		}
		return Out;
	}
}

/** Why the campus proxy came out spiky. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimateTouchingObjectsTest,
	"FragmentsUnreal.ProxyDecimate.TouchingObjectsNotFused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimateTouchingObjectsTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	const float Half = 100.0f;
	const int32 NumBoxes = 6;

	// Longest edge in the input: a face diagonal of one box's grid cell, well under one box.
	FDecimateSection Input = BuildAbuttingBoxes(NumBoxes, Half);
	const int32 OrigTris = Input.Indices.Num() / 3;
	const float InputMaxEdge = MaxEdgeLength(Input);
	const FBox3f OrigBounds = BoundsOf(Input);

	FDecimateSection NoWeld = Input;
	DecimateSection(NoWeld, 0.1f, /*WeldToleranceCm*/ 0.0f);

	FDecimateSection Welded = Input;
	DecimateSection(Welded, 0.1f, /*WeldToleranceCm*/ 0.01f);

	const FBox3f NoWeldBounds = BoundsOf(NoWeld);
	const FBox3f WeldedBounds = BoundsOf(Welded);

	AddInfo(FString::Printf(TEXT("[spike] input:    %d tris, max edge %.1f cm, bounds %s..%s"),
		OrigTris, InputMaxEdge, *OrigBounds.Min.ToString(), *OrigBounds.Max.ToString()));
	AddInfo(FString::Printf(TEXT("[spike] no-weld:  %d tris, max edge %.1f cm, bounds %s..%s"),
		NoWeld.Indices.Num() / 3, MaxEdgeLength(NoWeld),
		*NoWeldBounds.Min.ToString(), *NoWeldBounds.Max.ToString()));
	AddInfo(FString::Printf(TEXT("[spike] welded:   %d tris, max edge %.1f cm, bounds %s..%s"),
		Welded.Indices.Num() / 3, MaxEdgeLength(Welded),
		*WeldedBounds.Min.ToString(), *WeldedBounds.Max.ToString()));

	// Diagnostic only for now — the numbers above drive the scoped-weld design.
	TestTrue(TEXT("both paths produced geometry"),
		NoWeld.Indices.Num() >= 3 && Welded.Indices.Num() >= 3);

	return true;
}

namespace
{
	/** One flat quad, grid-subdivided, with its OWN private vertex block (constant Z, normal +Z, tangent +X) — the render-buffer shape of one hard-edge-bounded patch. */
	FragmentProxyDecimate::FDecimateSection BuildFlatQuad(
		float XMin, float XMax, float YMin, float YMax, float Z, int32 CellsU, int32 CellsV)
	{
		using namespace FragmentProxyDecimate;
		FDecimateSection S;
		const int32 NU = CellsU + 1;
		const int32 NV = CellsV + 1;

		for (int32 iv = 0; iv < NV; ++iv)
		{
			for (int32 iu = 0; iu < NU; ++iu)
			{
				const float X = XMin + (XMax - XMin) * (iu / float(CellsU));
				const float Y = YMin + (YMax - YMin) * (iv / float(CellsV));
				S.Positions.Add(FVector3f(X, Y, Z));
				S.Normals.Add(FVector3f(0, 0, 1));
				S.TangentsX.Add(FVector3f(1, 0, 0));
				S.TangentYSigns.Add(1.0f);
				S.UV0.Add(FVector2f(iu / float(CellsU), iv / float(CellsV)));
				S.VertexColors.Add(FColor(200, 200, 200, 255));
			}
		}
		for (int32 iv = 0; iv < CellsV; ++iv)
		{
			for (int32 iu = 0; iu < CellsU; ++iu)
			{
				const uint32 V00 = (uint32)(iv * NU + iu);
				const uint32 V10 = V00 + 1;
				const uint32 V01 = (uint32)((iv + 1) * NU + iu);
				const uint32 V11 = V01 + 1;
				S.Indices.Add(V00); S.Indices.Add(V10); S.Indices.Add(V11);
				S.Indices.Add(V00); S.Indices.Add(V11); S.Indices.Add(V01);
			}
		}
		return S;
	}

	/** Count Positions lying exactly on the plane X == TargetX. */
	int32 CountVertsOnPlaneX(const FragmentProxyDecimate::FDecimateSection& S, float TargetX)
	{
		const int32 TargetQ = FMath::RoundToInt32(TargetX * 1000.0f);
		int32 Count = 0;
		for (const FVector3f& P : S.Positions)
		{
			if (FMath::RoundToInt32(P.X * 1000.0f) == TargetQ) { ++Count; }
		}
		return Count;
	}

	/** Open vs total distinct edges found lying exactly on a seam plane -- see CountEdgesOnPlaneX. */
	struct FSeamEdgeStats
	{
		int32 NumOpen  = 0;   // not shared by exactly two triangles (a tear)
		int32 NumTotal = 0;   // distinct edges found there at all, open or closed
	};

	/** Count open edges along the target seam plane. */
	FSeamEdgeStats CountEdgesOnPlaneX(const FragmentProxyDecimate::FDecimateSection& S, float TargetX)
	{
		const int32 TargetQ = FMath::RoundToInt32(TargetX * 1000.0f);
		TMap<TPair<FIntVector3, FIntVector3>, int32> EdgeCounts;

		for (int32 t = 0; t + 2 < S.Indices.Num(); t += 3)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector3f& PA = S.Positions[S.Indices[t + k]];
				const FVector3f& PB = S.Positions[S.Indices[t + ((k + 1) % 3)]];

				FIntVector3 QA = QuantizePos(PA);
				FIntVector3 QB = QuantizePos(PB);
				if (QA == QB) { continue; }                              // degenerate edge
				if (QA.X != TargetQ || QB.X != TargetQ) { continue; }    // not on the seam plane

				const bool bSwap =
					(QB.X < QA.X) ||
					(QB.X == QA.X && QB.Y < QA.Y) ||
					(QB.X == QA.X && QB.Y == QA.Y && QB.Z < QA.Z);
				if (bSwap) { Swap(QA, QB); }

				EdgeCounts.FindOrAdd(TPair<FIntVector3, FIntVector3>(QA, QB), 0)++;
			}
		}

		FSeamEdgeStats Stats;
		Stats.NumTotal = EdgeCounts.Num();
		for (const TPair<TPair<FIntVector3, FIntVector3>, int32>& E : EdgeCounts)
		{
			if (E.Value != 2) { ++Stats.NumOpen; }
		}
		return Stats;
	}
}

/** Adjacent quads with split seam vertices model the render-buffer input. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimateWeldRepairsTornSoupTest,
	"FragmentsUnreal.ProxyDecimateTopology.WeldRepairsTornSoup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimateWeldRepairsTornSoupTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	const float HalfExtent = 100.0f;
	const int32 CellsV = 8;    // shared with both quads -- this is what makes the seam actually match

	// Only the shared seam matches; the surrounding quads remain asymmetric.
	FDecimateSection QuadA = BuildFlatQuad(-HalfExtent * 1.5f, 0.0f, -HalfExtent, HalfExtent, 0.0f, /*CellsU*/ 11, CellsV);
	FDecimateSection QuadB = BuildFlatQuad(0.0f, HalfExtent * 0.6f, -HalfExtent, HalfExtent, 0.0f, /*CellsU*/ 5, CellsV);

	FDecimateSection Soup;
	AppendSection(Soup, QuadA, FVector3f::ZeroVector);
	AppendSection(Soup, QuadB, FVector3f::ZeroVector);

	const int32 OrigTris = Soup.Indices.Num() / 3;
	TestEqual(TEXT("fixture triangle count"), OrigTris, 11 * CellsV * 2 + 5 * CellsV * 2);

	// Fixture sanity: the seam at x=0 is DUPLICATED, not shared -- (CellsV+1) vertices from each quad's own private vertex block sit at the exact same positions, rather than one shared set.
	const int32 SeamVertsBeforeDecimate = CountVertsOnPlaneX(Soup, 0.0f);
	TestEqual(TEXT("seam vertices are duplicated (one private block per quad) before decimation"),
		SeamVertsBeforeDecimate, 2 * (CellsV + 1));

	FDecimateSection NoWeld = Soup;
	TestTrue(TEXT("unwelded decimation ran"), DecimateSection(NoWeld, 0.5f, /*WeldToleranceCm*/ 0.0f));

	FDecimateSection Welded = Soup;
	TestTrue(TEXT("welded decimation ran"), DecimateSection(Welded, 0.5f, /*WeldToleranceCm*/ 0.01f));

	const FSeamEdgeStats SeamNoWeld = CountEdgesOnPlaneX(NoWeld, 0.0f);
	const FSeamEdgeStats SeamWelded = CountEdgesOnPlaneX(Welded, 0.0f);

	AddInfo(FString::Printf(
		TEXT("[soup] orig %d tris; no-weld -> %d tris, %d/%d open/total seam edges; ")
		TEXT("welded -> %d tris, %d/%d open/total seam edges"),
		OrigTris, NoWeld.Indices.Num() / 3, SeamNoWeld.NumOpen, SeamNoWeld.NumTotal,
		Welded.Indices.Num() / 3, SeamWelded.NumOpen, SeamWelded.NumTotal));

	// The unwelded control must retain an open seam.
	TestTrue(
		FString::Printf(TEXT("unwelded run still shows an open seam after simplify (%d open edges)"),
			SeamNoWeld.NumOpen),
		SeamNoWeld.NumOpen > 0);

	// Verify the seam remains present when asserting it has no open edges.
	TestTrue(
		FString::Printf(TEXT("welded run's seam plane still carries edges (%d total)"),
			SeamWelded.NumTotal),
		SeamWelded.NumTotal > 0);
	TestEqual(
		FString::Printf(TEXT("welded run's seam is fully interior after simplify (%d open of %d total)"),
			SeamWelded.NumOpen, SeamWelded.NumTotal),
		SeamWelded.NumOpen, 0);

	return true;
}

/** Section-wide welding can fuse distinct boxes that share a face. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProxyDecimateWeldDoesNotFuseDistinctElementsTest,
	"FragmentsUnreal.ProxyDecimateTopology.WeldDoesNotFuseDistinctElements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProxyDecimateWeldDoesNotFuseDistinctElementsTest::RunTest(const FString& Parameters)
{
	using namespace FragmentProxyDecimate;

	const float Half = 100.0f;
	const float Ratio = 0.5f;               // moderate, representative cell-merge decimation ratio
	const float WeldToleranceCm = 0.01f;    // the production template-scope tolerance

	// Two boxes sharing a face exactly — abutting slabs, a panel against its frame.
	FDecimateSection Input = BuildAbuttingBoxes(2, Half);
	const float InputMaxEdge = MaxEdgeLength(Input);

	FDecimateSection NoWeld = Input;
	TestTrue(TEXT("unwelded decimation ran"), DecimateSection(NoWeld, Ratio, /*WeldToleranceCm*/ 0.0f));
	const float NoWeldMaxEdge = MaxEdgeLength(NoWeld);

	FDecimateSection Welded = Input;
	TestTrue(TEXT("welded decimation ran"), DecimateSection(Welded, Ratio, WeldToleranceCm));
	const float WeldedMaxEdge = MaxEdgeLength(Welded);

	AddInfo(FString::Printf(
		TEXT("[fuse-guard] input maxedge=%.1f | no-weld maxedge=%.1f | welded maxedge=%.1f (%.1f%% vs no-weld)"),
		InputMaxEdge, NoWeldMaxEdge, WeldedMaxEdge,
		(NoWeldMaxEdge > 0.0f) ? (100.0f * (WeldedMaxEdge - NoWeldMaxEdge) / NoWeldMaxEdge) : 0.0f));

	// Welding must keep maximum edge length within 15% of the unwelded mesh.
	const float KMaxEdgeGrowth = 1.15f;
	TestTrue(
		FString::Printf(
			TEXT("welded max edge (%.1f) stays within %.0f%% of the unwelded run's (%.1f)"),
			WeldedMaxEdge, (KMaxEdgeGrowth - 1.0f) * 100.0f, NoWeldMaxEdge),
		WeldedMaxEdge <= NoWeldMaxEdge * KMaxEdgeGrowth);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
