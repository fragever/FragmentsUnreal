// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "Misc/AutomationTest.h"
#include "Importer/FragmentModelWrapper.h"
#include "Importer/FragmentsImporter.h"
#include "Spatial/FragmentShellAuthoring.h"
#include "Index/fragments361_generated.h"
#include "Index/modern_generated.h"
#include "Index/legacy_generated.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "MeshDescription.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
// Build with the independent upstream schema, including both incompatible root slots.
TArray<uint8> LatestFixture(bool bBig, bool bBadIndex = false, bool bFaceIds = true, bool bLegacyRoot = false)
{
	namespace F = FragmentsModern;
	flatbuffers::FlatBufferBuilder B;
	std::vector<F::FloatVector> Points(bBig ? 65539 : 3);
	Points[0] = F::FloatVector(0, 0, 0);
	Points[bBig ? 65536 : 1] = F::FloatVector(10, 0, 0);
	Points[bBig ? 65537 : 2] = F::FloatVector(0, 0, 10);
	std::vector<flatbuffers::Offset<F::BigShellHole>> BigHoleList;
	if (bBig)
	{
		Points[65538] = F::FloatVector(1, 0, 1);
		Points[1] = F::FloatVector(2, 0, 1);
		Points[2] = F::FloatVector(1, 0, 2);
		const auto HoleIndices = B.CreateVector(std::vector<uint32_t>{65538, 1, 2});
		BigHoleList.push_back(F::CreateBigShellHole(B, HoleIndices, 0));
	}
	const auto PointData = B.CreateVectorOfStructs(Points);
	std::vector<flatbuffers::Offset<F::ShellProfile>> Profiles;
	std::vector<flatbuffers::Offset<F::BigShellProfile>> BigProfiles;
	if (bBig)
	{
		const auto Indices = B.CreateVector(std::vector<uint32_t>{0, 65536, bBadIndex ? 999999u : 65537u});
		BigProfiles.push_back(F::CreateBigShellProfile(B, Indices));
	}
	else
	{
		const auto Indices = B.CreateVector(std::vector<uint16_t>{0, 1, 2});
		Profiles.push_back(F::CreateShellProfile(B, Indices));
	}
	const auto ProfileData = B.CreateVector(Profiles);
	const auto Holes = B.CreateVector(std::vector<flatbuffers::Offset<F::ShellHole>>{});
	const auto BigProfileData = B.CreateVector(BigProfiles);
	const auto BigHoles = B.CreateVector(BigHoleList);
	const auto FaceIds = bFaceIds ? B.CreateVector(std::vector<uint16_t>{7}) : flatbuffers::Offset<flatbuffers::Vector<uint16_t>>();
	const auto ShellData = F::CreateShell(B, ProfileData, Holes, PointData, BigProfileData, BigHoles,
		bBig ? F::ShellType_BIG : F::ShellType_NONE, FaceIds);
	const auto Shells = B.CreateVector(std::vector<flatbuffers::Offset<F::Shell>>{ShellData});
	const auto EmptyIds = B.CreateVector(std::vector<uint32_t>{});
	const auto Samples = B.CreateVectorOfStructs(std::vector<F::Sample>{F::Sample(0, 0, 0, 0)});
	const auto Reps = B.CreateVectorOfStructs(std::vector<F::Representation>{F::Representation(0,
		F::BoundingBox(F::FloatVector(0, 0, 0), F::FloatVector(1, 0, 1)), F::RepresentationClass_SHELL)});
	const auto Mats = B.CreateVectorOfStructs(std::vector<F::Material>{F::Material(255, 255, 255, 255, F::RenderedFaces_ONE, F::Stroke_DEFAULT)});
	const auto Circles = B.CreateVector(std::vector<flatbuffers::Offset<F::CircleExtrusion>>{});
	const F::Transform Coordinates(F::DoubleVector(0, 0, 0), F::FloatVector(1, 0, 0), F::FloatVector(0, 1, 0));
	const auto Transforms = B.CreateVectorOfStructs(std::vector<F::Transform>{Coordinates});
	const auto MeshItems = B.CreateVector(std::vector<uint32_t>{0});
	const auto Mesh = F::CreateMeshes(B, &Coordinates, MeshItems, Samples, Reps, Mats, Circles, Shells, Transforms, Transforms);
	const auto Json = B.CreateString("[\"Name\",\"Wall, \\\"east\\\"\",123]");
	const auto AttrData = B.CreateVector(std::vector<flatbuffers::Offset<flatbuffers::String>>{Json});
	const auto Attr = F::CreateAttribute(B, AttrData);
	const auto Attrs = B.CreateVector(std::vector<flatbuffers::Offset<F::Attribute>>{Attr});
	const auto Strings = B.CreateVector(std::vector<flatbuffers::Offset<flatbuffers::String>>{});
	const auto LocalIds = B.CreateVector(std::vector<uint32_t>{42});
	const auto Guid = B.CreateString(bBig ? "latest-big" : "latest-normal");
	const auto Unique = bLegacyRoot ? flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>>()
		: B.CreateVector(std::vector<flatbuffers::Offset<flatbuffers::String>>{Json});
	const auto RelName = B.CreateString("IsDefinedBy");
	auto RelNames = B.CreateVector(std::vector<flatbuffers::Offset<flatbuffers::String>>{RelName});
	if (bLegacyRoot)
	{
		const auto GeometrySamples = B.CreateVectorOfStructs(std::vector<FragmentsLegacy::GeometrySample>{});
		const auto GeometryTransforms = B.CreateVectorOfStructs(std::vector<FragmentsLegacy::Transform>{});
		const auto GeometryLines = B.CreateVector(std::vector<flatbuffers::Offset<FragmentsLegacy::GeometryLines>>{});
		const auto Geometry = FragmentsLegacy::CreateGeometries(B, GeometrySamples, GeometryTransforms, GeometryLines);
		// Same slot 13, different wire type: deliberately construct the legacy-root exporter variant.
		RelNames = decltype(RelNames)(Geometry.o);
	}
	const auto Category = B.CreateString("IFCWALL");
	const auto Categories = B.CreateVector(std::vector<flatbuffers::Offset<flatbuffers::String>>{Category});
	const auto Structure = F::CreateSpatialStructure(B, 42, Category);
	const auto Root = F::CreateModel(B, 0, Strings, EmptyIds, 43, LocalIds, Categories, Mesh, Attrs, 0, 0, Guid, Structure, Unique, RelNames);
	F::FinishModelBuffer(B, Root);
	return TArray<uint8>(B.GetBufferPointer(), static_cast<int32>(B.GetSize()));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiVersionSchemaTest, "FragmentsUnreal.Importer.MultiVersion.ReadersAndLargeShells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiVersionSchemaTest::RunTest(const FString& Parameters)
{
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("FragmentsUnreal"));
	if (!Plugin) return false;
	TArray<uint8> Legacy;
	FString Error;
	if (!TestTrue(TEXT("legacy fixture inflates"), UFragmentsImporter::ReadAndInflate(
		FPaths::Combine(Plugin->GetContentDir(), TEXT("Resources/small_test.frag")), Legacy, Error))) return false;
	TestTrue(TEXT("legacy fixture remains supported"), UFragmentModelWrapper::IsValidModelBuffer(Legacy));
	UFragmentModelWrapper* Old = NewObject<UFragmentModelWrapper>();
	Old->LoadModel(MoveTemp(Legacy));
	const FString LegacyGuid = UTF8_TO_TCHAR(Old->GetParsedModel()->guid()->c_str());
	for (bool bBig : {false, true})
	{
		const auto Fixture = LatestFixture(bBig);
		flatbuffers::Verifier UpstreamVerifier(Fixture.GetData(), Fixture.Num());
		TestTrue(TEXT("fixture passes the unmodified latest upstream verifier"), Fragments361::VerifyModelBuffer(UpstreamVerifier));
		UFragmentsImporter* Importer = NewObject<UFragmentsImporter>();
		const FString ImportedGuid = Importer->LoadFragmentFromBuffer(LatestFixture(bBig), TEXT("C:/multiversion-test.frag"), FFragmentLoadOptions());
		TestFalse(TEXT("latest buffer imports through the existing importer"), ImportedGuid.IsEmpty());
		const auto* ImportedWrapper = Importer->GetFragmentModels().Find(ImportedGuid);
		if (!TestNotNull(TEXT("latest model registered"), ImportedWrapper)) return false;
		FFragmentItem* ImportedItem = (*ImportedWrapper)->FindItemByLocalId(42);
		if (!TestNotNull(TEXT("latest item is available to filters and selection"), ImportedItem)) return false;
		TestEqual(TEXT("category preserved"), ImportedItem->Category, FString(TEXT("IFCWALL")));
		TestEqual(TEXT("sample preserved"), ImportedItem->Samples.Num(), 1);
		UFragmentModelWrapper* Latest = NewObject<UFragmentModelWrapper>();
		Latest->LoadModel(LatestFixture(bBig));
		TestTrue(TEXT("latest detected independently of legacy root slots"), Latest->GetSchemaFormat() == EFragmentSchemaFormat::Fragments361);
		if (!TestNotNull(TEXT("latest model loaded"), Latest->GetParsedModel())) return false;
		TestEqual(TEXT("local ID unchanged"), Latest->GetFlatIndexForLocalId(42), int64(0));
		const auto Props = UFragmentsUtils::ParseItemAttribute(Latest->GetParsedModel()->attributes()->Get(0));
		TestEqual(TEXT("one attribute"), Props.Num(), 1);
		if (Props.Num()) TestEqual(TEXT("commas and quotes survive JSON decoding"), Props[0].Value, FString(TEXT("Wall, \"east\"")));
		const FModelBufferPin Pin = Latest->GetBufferPin();
		const Shell* ShellRef = Latest->GetParsedModel()->meshes()->shells()->Get(0);
		Latest->LoadModel(TArray<uint8>());
		FragmentShellAuthoring::FShellSource Source;
		TestTrue(TEXT("pinned shell survives wrapper reload"), FragmentShellAuthoring::ReadShellSource(ShellRef, Source));
		if (Source.Profiles.Num()) TestEqual(TEXT("indices keep their full width"), Source.Profiles[0][1], bBig ? 65536 : 1);
		if (bBig) TestEqual(TEXT("hole indices keep their full width"), Source.ProfileHoles.FindChecked(0)[0][0], 65538);
		TestEqual(TEXT("face IDs preserved"), Source.ProfileFaceIds.Num(), 1);
		FMeshDescription MD;
		TArray<FVector3f> CollisionPositions;
		TArray<uint32> CollisionIndices;
		TestTrue(TEXT("latest shell authors a mesh"), FragmentShellAuthoring::AuthorShellMeshDescription(
			Source, FColor::White, MD, CollisionPositions, CollisionIndices));
		TestEqual(TEXT("legacy model coexists unchanged"), FString(UTF8_TO_TCHAR(Old->GetParsedModel()->guid()->c_str())), LegacyGuid);
	}
	TestFalse(TEXT("out-of-range large-shell point rejected"), UFragmentModelWrapper::IsValidModelBuffer(LatestFixture(true, true)));
	const auto EarlierModern = LatestFixture(true, false, false);
	TestTrue(TEXT("pre-face-ID modern exporter remains supported"), FragmentSchema::Identify(EarlierModern.GetData(), EarlierModern.Num()) == EFragmentSchemaFormat::ModernWithoutFaceIds);
	const auto ExtendedLegacy = LatestFixture(true, false, false, true);
	TestTrue(TEXT("legacy root with extended shells remains supported"), FragmentSchema::Identify(ExtendedLegacy.GetData(), ExtendedLegacy.Num()) == EFragmentSchemaFormat::LegacyWithShellExtensions);
	auto Truncated = LatestFixture(true);
	Truncated.SetNum(Truncated.Num() / 2);
	TestFalse(TEXT("truncated latest file rejected"), UFragmentModelWrapper::IsValidModelBuffer(Truncated));
	return true;
}
#endif
