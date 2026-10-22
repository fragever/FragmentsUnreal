// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Overrides/FragmentPlacementMath.h"

namespace UE::Fragments::Placement
{
	// FTransform A * B applies A first, then B.

	FTransform ComposeSavedPlacement(
		const FTransform& BakedFrame, const FTransform& BakedPlacement, const FTransform& ActorDelta)
	{
		// Saved * OriginFrame == BakedFrame * ActorDelta => Saved = BakedFrame * ActorDelta * OriginFrame^-1.
		return BakedFrame * ActorDelta * BakedFrame.Inverse() * BakedPlacement;
	}

	FTransform ActorDeltaForNoPlacement(const FTransform& BakedFrame, const FTransform& BakedPlacement)
	{
		// No placement is placement identity.
		return ActorDeltaForPlacement(BakedFrame, BakedPlacement, FTransform::Identity);
	}

	FTransform ActorDeltaForPlacement(
		const FTransform& BakedFrame, const FTransform& BakedPlacement, const FTransform& P)
	{
		// BakedFrame * Result == P * OriginFrame  =>  Result = BakedFrame^-1 * P * OriginFrame
		return BakedFrame.Inverse() * P * BakedPlacement.Inverse() * BakedFrame;
	}

	// The Details panel copies a struct property with ExportText: parenthesised, comma-separated, six decimals.
	FString FormatLocationForClipboard(const FVector& Location)
	{
		return FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), Location.X, Location.Y, Location.Z);
	}

	FString FormatRotationForClipboard(const FRotator& Rotation)
	{
		return FString::Printf(TEXT("(Pitch=%f,Yaw=%f,Roll=%f)"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
	}

	// FParse::Value finds "Key=" anywhere and reads the number after it, so one parser covers the Details panel's "(X=1,Y=2,Z=3)" and ToString's "X=1 Y=2 Z=3".
	bool ParseLocationFromClipboard(const FString& Text, FVector& Out)
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!FParse::Value(*Text, TEXT("X="), X) || !FParse::Value(*Text, TEXT("Y="), Y) || !FParse::Value(*Text, TEXT("Z="), Z))
		{
			return false;
		}
		Out = FVector(X, Y, Z);
		return true;
	}

	bool ParseRotationFromClipboard(const FString& Text, FRotator& Out)
	{
		// Details panel form first ("Pitch=", "Yaw=", "Roll="), then ToString's short form ("P=", "Y=", "R=").
		double P = 0.0, Y = 0.0, R = 0.0;
		const bool bLong = FParse::Value(*Text, TEXT("Pitch="), P) && FParse::Value(*Text, TEXT("Yaw="), Y)
			&& FParse::Value(*Text, TEXT("Roll="), R);
		const bool bShort = !bLong && FParse::Value(*Text, TEXT("P="), P) && FParse::Value(*Text, TEXT("Y="), Y)
			&& FParse::Value(*Text, TEXT("R="), R);
		if (!bLong && !bShort)
		{
			return false;
		}
		Out = FRotator(P, Y, R);
		return true;
	}
}
