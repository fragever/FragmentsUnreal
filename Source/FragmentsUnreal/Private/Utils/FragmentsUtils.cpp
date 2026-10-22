// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant




#include "Utils/FragmentsUtils.h"
#include "Utils/FragmentsLog.h"
#include "Fragment/Fragment.h"

FTransform UFragmentsUtils::MakeTransform(const Transform* FragmentsTransform, bool bIsLocalTransform, bool bIsRootCoordinates)
{
	if (!FragmentsTransform)
	{
		UE_LOG(LogFragments, Error, TEXT("MakeTransform received null Transform pointer."));
		return FTransform::Identity;
	}

	// Axis swap (Fragments -> UE)
	const FVector X = SafeAxis(FVector(
		FragmentsTransform->x_direction().x(),
		FragmentsTransform->x_direction().z(),
		FragmentsTransform->x_direction().y()), FVector::ForwardVector);

	const FVector YRaw = SafeAxis(FVector(
		FragmentsTransform->y_direction().x(),
		FragmentsTransform->y_direction().z(),
		FragmentsTransform->y_direction().y()), FVector::RightVector);

	const FVector Pos = SafeVector(FVector(
		FragmentsTransform->position().x(),
		FragmentsTransform->position().z(),
		FragmentsTransform->position().y())) * 100.0f;

	// Non-root: preserve legacy behavior to avoid unintended changes
	if (!bIsRootCoordinates)
	{
		// Legacy path: treat y_direction as Z
		return BuildFromXZ(X, YRaw, Pos);
	}

	// Root-only auto detect: Option A: treat y_direction as Y.
	const FTransform Txy = BuildFromXY(X, YRaw, Pos);

	// Option B: treat y_direction as Z (legacy)
	const FTransform Txz = BuildFromXZ(X, YRaw, Pos);

	// Pick whichever makes UE "Up" (Z axis) more aligned with +Z
	const float ScoreXY = FVector::DotProduct(Txy.GetUnitAxis(EAxis::Z), FVector::UpVector);
	const float ScoreXZ = FVector::DotProduct(Txz.GetUnitAxis(EAxis::Z), FVector::UpVector);

	const FTransform Out = (ScoreXY >= ScoreXZ) ? Txy : Txz;

	UE_LOG(LogFragments, Log, TEXT("Root basis auto-detect: ScoreXY=%.4f ScoreXZ=%.4f -> %s"),
		ScoreXY, ScoreXZ, (ScoreXY >= ScoreXZ) ? TEXT("Use XY") : TEXT("Use XZ"));

	return Out;
}

FPlaneProjection UFragmentsUtils::BuildProjectionPlane(const TArray<FVector>& Points, const TArray<int32>& Profile)
{
	FPlaneProjection Projection;
	const int32 N = Profile.Num();

	if (N < 3)
		return Projection;

	const FVector A = Points[Profile[0]];

	// Newell's method is robust against colinear leading-edge triplets that broke the previous 3-point cross-product approach.
	FVector Normal = FVector::ZeroVector;
	FVector LongestEdge = FVector::ZeroVector;
	double LongestEdgeSq = 0.0;
	for (int32 i = 0; i < N; ++i)
	{
		const FVector& Cur = Points[Profile[i]];
		const FVector& Nxt = Points[Profile[(i + 1) % N]];
		Normal.X += (Cur.Y - Nxt.Y) * (Cur.Z + Nxt.Z);
		Normal.Y += (Cur.Z - Nxt.Z) * (Cur.X + Nxt.X);
		Normal.Z += (Cur.X - Nxt.X) * (Cur.Y + Nxt.Y);

		const FVector Edge = Nxt - Cur;
		const double LenSq = Edge.SizeSquared();
		if (LenSq > LongestEdgeSq)
		{
			LongestEdgeSq = LenSq;
			LongestEdge = Edge;
		}
	}

	if (Normal.IsNearlyZero())
	{
		UE_LOG(LogFragments, Verbose, TEXT("Failed to find polygon normal (degenerate profile)! Projection may be invalid."));
		Projection.Origin = A;
		Projection.AxisX = FVector(1, 0, 0);
		Projection.AxisY = FVector(0, 1, 0);
		return Projection;
	}

	Normal.Normalize();

	FVector AxisX = LongestEdge - FVector::DotProduct(LongestEdge, Normal) * Normal;

	if (AxisX.IsNearlyZero())
	{
		// Pathological: every edge parallel to the normal. Pick any perpendicular.
		AxisX = FVector::CrossProduct(Normal, FVector::UpVector);
		if (AxisX.IsNearlyZero())
		{
			AxisX = FVector::CrossProduct(Normal, FVector::RightVector);
		}
	}
	AxisX.Normalize();

	const FVector AxisY = FVector::CrossProduct(Normal, AxisX).GetSafeNormal();

	Projection.Origin = A;
	Projection.AxisX = AxisX;
	Projection.AxisY = AxisY;
	return Projection;
}

bool UFragmentsUtils::IsClockwise(const TArray<FVector2D>& Points)
{
	float Area = 0.0f;
	for (int32 i = 0; i < Points.Num(); ++i)
	{
		const FVector2D& p0 = Points[i];
		const FVector2D& p1 = Points[(i + 1) % Points.Num()];
		Area += (p1.X - p0.X) * (p1.Y + p0.Y);
	}
	return Area > 0.0;
}

TArray<FItemAttribute> UFragmentsUtils::ParseItemAttribute(const Attribute* Attr)
{
	TArray<FItemAttribute> Parsed;
	if (!Attr || !Attr->data()) return Parsed;
	
	for (flatbuffers::uoffset_t i = 0; i < Attr->data()->size(); i++)
	{
		const auto* Raw = Attr->data()->Get(i);
		if (!Raw) continue;

		FString RawString = UTF8_TO_TCHAR(Raw->c_str());

		TArray<FString> Tokens;
		RawString.Replace(TEXT("["), TEXT(""))
			.Replace(TEXT("]"), TEXT(""))
			.ParseIntoArray(Tokens, TEXT(","), true);

		if (Tokens.Num() < 3) continue;

		FString Key = Tokens[0].TrimStartAndEnd().Replace(TEXT("\""), TEXT(""));
		FString Value = Tokens[1].TrimStartAndEnd().Replace(TEXT("\""), TEXT(""));
		int64 TypeHash = FCString::Atoi(*Tokens[2].TrimStartAndEnd());

		Parsed.Add(FItemAttribute(Key, Value, TEXT(""), TypeHash));
	}
	return Parsed;
}

AFragment* UFragmentsUtils::MapModelStructure(const SpatialStructure* InS, AFragment*& ParentActor, TMap<int64, AFragment*>& FragmentLookupMapRef, const FString& InheritedCategory)
{
	// Determine if this node should spawn an actor
	const bool bHasLocalId = InS->local_id().has_value();
	const bool bHasCategory = InS->category() && InS->category()->size() > 0;
	const FString ThisCategory = bHasCategory ? UTF8_TO_TCHAR(InS->category()->c_str()) : InheritedCategory;

	const bool bEffectiveCategory = bHasCategory || !InheritedCategory.IsEmpty();
	const bool bShouldSpawn = bHasLocalId && bEffectiveCategory;

	AFragment* FragmentActor = nullptr;

	if (bShouldSpawn)
	{
		FragmentActor = ParentActor->GetWorld()->SpawnActor<AFragment>(AFragment::StaticClass(), FTransform::Identity);

		if (bHasLocalId)
		{
			int64 LocalId = InS->local_id().value();
			FragmentActor->SetLocalId(LocalId);
			FragmentLookupMapRef.Add(LocalId, FragmentActor);
		}

		FragmentActor->SetCategory(ThisCategory);
		FragmentActor->SetModelGuid(ParentActor->GetModelGuid());

#if WITH_EDITOR
		{
			FragmentActor->SetActorLabel(ThisCategory);
		}
#endif

		ParentActor->AddChild(FragmentActor);
	}

	const FString NextInheritedCategory = (!FragmentActor && bHasCategory) ? ThisCategory : TEXT("");

	if (InS->children())
	{
		for (flatbuffers::uoffset_t i = 0; i < InS->children()->size(); i++)
		{
			MapModelStructure(InS->children()->Get(i), FragmentActor ? FragmentActor : ParentActor, FragmentLookupMapRef, NextInheritedCategory);
		}
	}

	return FragmentActor;
}

void UFragmentsUtils::MapModelStructureToData(const SpatialStructure* InS, FFragmentItem& ParentItem, const FString& InheritedCategory)
{
	// Determine if this node should be stored as a fragment data object
	const bool bHasLocalId = InS->local_id().has_value();
	const bool bHasCategory = InS->category() && InS->category()->size() > 0;
	const FString ThisCategory = bHasCategory ? UTF8_TO_TCHAR(InS->category()->c_str()) : InheritedCategory;

	const bool bEffectiveCategory = bHasCategory || !InheritedCategory.IsEmpty();
	const bool bShouldStore = bHasLocalId && bEffectiveCategory;

	// Only allocate and store when this node qualifies as a fragment
	if (bShouldStore)
	{
		FFragmentItem* FragmentItem = new FFragmentItem();

		// Populate the fragment item with data from the SpatialStructure
		FragmentItem->ModelGuid = ParentItem.ModelGuid;
		FragmentItem->LocalId = InS->local_id().value();
		FragmentItem->Category = ThisCategory;

		// Store this FragmentItem as a child of the parent fragment item
		ParentItem.FragmentChildren.Add(FragmentItem);
	}


	// Now process children recursively if any
	if (InS->children())
	{
		for (flatbuffers::uoffset_t i = 0; i < InS->children()->size(); i++)
		{
			// Recursively add child fragments
			MapModelStructureToData(InS->children()->Get(i), bShouldStore ? *ParentItem.FragmentChildren.Last() : ParentItem, ThisCategory);
		}
	}
}

FString UFragmentsUtils::GetIfcCategory(const int64 InTypeHash)
{
	static const TMap<int64, FString> IfcCategoryMap = {
		{ 3258342251, TEXT("IFCBUILDINGELEMENTPROXY") },
		{ 1549835152, TEXT("IFCANNOTATION") },
		{ 3693920615, TEXT("IFCROOF") },
		{ 1939436016, TEXT("IFCBUILDING") },
		{ 983778844,  TEXT("IFCSLAB") },
		{ 2586569798, TEXT("IFCFOOTING") },
		{ 3006469996, TEXT("IFCCOLUMN") },
		{ 4085436816, TEXT("IFCWALLSTANDARDCASE") },
		{ 3234207722, TEXT("IFCSTAIRFLIGHT") },
		{ 3792847397, TEXT("IFCFURNISHINGELEMENT") },
		{ 3722059766, TEXT("IFCOPENINGELEMENT") },
		{ 3812528620, TEXT("IFCSITE") },
		{ 3999184608, TEXT("IFCSTAIR") },
		{ 3047399987, TEXT("IFCDOOR") },
		{ 2587468083, TEXT("IFCFLOWTERMINAL") },
		{ 2016195849, TEXT("IFCWALL") },
		{ 2692852465, TEXT("IFCSPACE") },
		{ 2476758395, TEXT("IFCFLOWSEGMENT") },
		{ 2735952531, TEXT("IFCFLOWFITTING") },
		{ 2568828275, TEXT("IFCPLATE") },
		{ 1319467500, TEXT("IFCFLOWCONTROLLER") },
		{ 2998110625, TEXT("IFCBEAM") },
		{ 1858898314, TEXT("IFCMEMBER") },
		{ 1592764394, TEXT("IFCCOVERING") },
		{ 3420386420, TEXT("IFCTENDON") },
		{ 3351459610, TEXT("IFCREINFORCINGBAR") },
		{ 3648960775, TEXT("IFCWINDOW") },
		{ 3123201583, TEXT("IFCELEMENTASSEMBLY") },
		{ 3356537533, TEXT("IFCTRANSPORTELEMENT") },
		{ 3237983705, TEXT("IFCELECTRICALELEMENT") },
		{ 3250150772, TEXT("IFCELECTRICAPPLIANCE") },
		{ 3223608652, TEXT("IFCELECTRICDISTRIBUTIONBOARD") },
		{ 2791496458, TEXT("IFCELECTRICFLOWSTORAGEDEVICE") },
		{ 2196933786, TEXT("IFCELECTRICGENERATOR") },
		{ 3992818766, TEXT("IFCELECTRICMOTOR") },
		{ 2625861777, TEXT("IFCELECTRICTIMECONTROL") },
		{ 4014341074, TEXT("IFCFIRESUPPRESSIONTERMINAL") },
		{ 3351767623, TEXT("IFCSANITARYTERMINAL") },
		{ 3102286572, TEXT("IFCLIGHTFIXTURE") },
		{ 2150113622, TEXT("IFCSWITCHINGDEVICE") },
		{ 3020706450, TEXT("IFCDISTRIBUTIONFLOWELEMENT") },
		{ 2996378946, TEXT("IFCFURNITURE") },
		{ 2330826553, TEXT("IFCPILE") },
		{ 3277505449, TEXT("IFCCHIMNEY") },
		{ 1647198132, TEXT("IFCTRIM") },
		{ 3035534955, TEXT("IFCMOVINGWALKWAY") },
		{ 1988756489, TEXT("IFCSHADINGDEVICE") },
		{ 3063033404, TEXT("IFCRAILING") },
		{ 2882494310, TEXT("IFCFLOWTREATMENTDEVICE") },
		{ 3258399478, TEXT("IFCCOMMUNICATIONSAPPLIANCE") },
		{ 2705486713, TEXT("IFCDUCTSILENCER") },
		{ 2484214793, TEXT("IFCUNITARYCONTROLELEMENT") },
		{ 4028950474, TEXT("IFCMECHANICALFASTENER") },
		{ 2122156756, TEXT("IFCFASTENER") },
		{ 1513078977, TEXT("IFCDISCRETEACCESSORY") },
		{ 2904403302, TEXT("IFCREINFORCINGMESH") },
		{ 1864285793, TEXT("IFCTENDONANCHOR") },
		{ 3465791920, TEXT("IFCVIBRATIONISOLATOR") },
		{ 2402280193, TEXT("IFCSTRUCTURALCURVEACTION") },
		{ 1481989373, TEXT("IFCSTRUCTURALCURVECONNECTION") },
		{ 2556467911, TEXT("IFCSTRUCTURALCURVEMEMBER") },
		{ 2028047011, TEXT("IFCSTRUCTURALLOADGROUP") },
		{ 2284384606, TEXT("IFCSTRUCTURALPLANARACTION") },
		{ 4010788049, TEXT("IFCSTRUCTURALPOINTCONNECTION") },
		{ 2974137194, TEXT("IFCSTRUCTURALPOINTLOAD") },
		{ 2094780014, TEXT("IFCSTRUCTURALPOINTREACTION") },
		{ 2934528095, TEXT("IFCSTRUCTURALRESULTGROUP") },
		{ 1340962872, TEXT("IFCSTRUCTURALSURFACEACTION") },
		{ 2002803610, TEXT("IFCSTRUCTURALSURFACECONNECTION") },
		{ 2750787162, TEXT("IFCSTRUCTURALSURFACEMEMBER") }
	};
	const FString* Found = IfcCategoryMap.Find(InTypeHash);

	return Found ? *Found : TEXT("UNKNOWN");
}

float UFragmentsUtils::SafeComponent(float Value)
{
	return FMath::IsFinite(Value) ? Value : 0.0f;
}

FVector UFragmentsUtils::SafeVector(const FVector& Vec)
{
	return FVector(
		SafeComponent(Vec.X),
		SafeComponent(Vec.Y),
		SafeComponent(Vec.Z)
	);
}

FVector UFragmentsUtils::SafeAxis(const FVector& V, const FVector& Fallback, float Eps)
{
	FVector Out(
		FMath::IsFinite(V.X) ? V.X : 0.f,
		FMath::IsFinite(V.Y) ? V.Y : 0.f,
		FMath::IsFinite(V.Z) ? V.Z : 0.f
	);

	if (Out.SizeSquared() < Eps)
	{
		return Fallback;
	}

	return Out;
}

FRotator UFragmentsUtils::SafeRotator(const FRotator& Rot)
{
	return FRotator(
		SafeComponent(Rot.Pitch),
		SafeComponent(Rot.Yaw),
		SafeComponent(Rot.Roll)
	);
}

bool UFragmentsUtils::IsValueKey(const FString& Key)
{
	return Key.Equals(TEXT("NominalValue"), ESearchCase::IgnoreCase)
		|| Key.EndsWith(TEXT("Value"));
}

FTransform UFragmentsUtils::BuildFromXY(const FVector& XIn, const FVector& YIn, const FVector& PostCm)
{
	FVector X = XIn; FVector Y = YIn;

	if (!X.Normalize()) X = FVector::ForwardVector;

	// Orthonormalize Y vs X
	Y = (Y - FVector::DotProduct(Y, X) * X);
	if (!Y.Normalize()) Y = FVector::RightVector;

	FVector Z = FVector::CrossProduct(X, Y);
	if (!Z.Normalize()) Z = FVector::UpVector;

	FMatrix M(X, Y, Z, FVector::ZeroVector);
	FRotator R = SafeRotator(M.Rotator());
	return FTransform(R, PostCm, FVector(1.0f));
}

FTransform UFragmentsUtils::BuildFromXZ(const FVector& XIn, const FVector& ZIn, const FVector& PostCm)
{
	FVector X = XIn; FVector Z = ZIn;

	if (!X.Normalize()) X = FVector::ForwardVector;

	// Orthonormalize Z vs X
	Z = (Z - FVector::DotProduct(Z, X) * X);
	if (!Z.Normalize()) Z = FVector::UpVector;

	// Y = Z x X
	FVector Y = FVector::CrossProduct(Z, X);
	if (!Y.Normalize()) Y = FVector::RightVector;

	FMatrix M(X, Y, Z, FVector::ZeroVector);
	FRotator R = SafeRotator(M.Rotator());
	return FTransform(R, PostCm, FVector(1.0f));
}

TArray<FItemAttribute> UFragmentsUtils::ParsePropertySets(const TArray<FItemAttribute>& InAttributes)
{
	FString CurrentPropertySet;
	TArray<FItemAttribute> ParsedPropertySets;

	for (int32 i = 0; i < InAttributes.Num(); ++i)
	{
		const FItemAttribute& A = InAttributes[i];
		const FItemAttribute* Next = (i + 1 < InAttributes.Num()) ? &InAttributes[i + 1] : nullptr;

		if (A.Key.Equals(TEXT("Name"), ESearchCase::IgnoreCase) &&
			Next && Next->Key.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
		{
			CurrentPropertySet = A.Value;
			continue;
		}
		if (A.Key.Equals(TEXT("Name"), ESearchCase::IgnoreCase) &&
			Next && IsValueKey(Next->Key))
		{
			FItemAttribute pa;
			pa.Key = A.Value;
			pa.Value = Next->Value;
			pa.PropertySet = CurrentPropertySet;

			ParsedPropertySets.Add(MoveTemp(pa));
			++i;
			continue;
		}
	}

	return ParsedPropertySets;
}

int64 UFragmentsUtils::GetIndexForLocalId(const Model* InModelRef, int64 LocalId)
{
	if (!InModelRef || !InModelRef->local_ids()) return INDEX_NONE;

	const auto* LocalIds = InModelRef->local_ids();
	for (flatbuffers::uoffset_t i = 0; i < LocalIds->size(); i++)
	{
		if (LocalIds->Get(i) == LocalId)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void UFragmentsUtils::BuildLocalIdToItemMap(FFragmentItem& Root, TMap<int64, FFragmentItem*>& OutMap)
{
	TArray<FFragmentItem*> Stack;
	Stack.Add(&Root);
	while (Stack.Num() > 0)
	{
		FFragmentItem* Item = Stack.Pop();
		if (!Item) continue;
		OutMap.Add(Item->LocalId, Item);
		for (FFragmentItem* Child : Item->FragmentChildren)
		{
			if (Child) Stack.Add(Child);
		}
	}
}

// FFindResult Implementation.

int64 FFindResult::GetLocalId() const
{
	if (bIsInstanced)
	{
		return Proxy.LocalId;
	}
	else if (Fragment)
	{
		return Fragment->GetLocalId();
	}
	return INDEX_NONE;
}

FString FFindResult::GetCategory() const
{
	if (bIsInstanced)
	{
		return Proxy.Category;
	}
	else if (Fragment)
	{
		return Fragment->GetCategory();
	}
	return FString();
}

FTransform FFindResult::GetWorldTransform() const
{
	if (bIsInstanced)
	{
		return Proxy.WorldTransform;
	}
	else if (Fragment)
	{
		return Fragment->GetActorTransform();
	}
	return FTransform::Identity;
}

FFindResult FFindResult::NotFound()
{
	FFindResult Result;
	Result.bFound = false;
	Result.bIsInstanced = false;
	Result.Fragment = nullptr;
	return Result;
}

FFindResult FFindResult::FromActor(AFragment* Actor)
{
	FFindResult Result;
	if (Actor)
	{
		Result.bFound = true;
		Result.bIsInstanced = false;
		Result.Fragment = Actor;
	}
	else
	{
		Result.bFound = false;
		Result.bIsInstanced = false;
		Result.Fragment = nullptr;
	}
	return Result;
}

FFindResult FFindResult::FromProxy(const FFragmentProxy& InProxy)
{
	FFindResult Result;
	Result.bFound = true;
	Result.bIsInstanced = true;
	Result.Fragment = nullptr;
	Result.Proxy = InProxy;
	return Result;
}
