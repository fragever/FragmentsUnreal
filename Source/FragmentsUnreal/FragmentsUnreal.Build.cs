// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

using UnrealBuildTool;
using System.IO;

public class FragmentsUnreal : ModuleRules
{
	public FragmentsUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				Path.Combine(ModuleDirectory, "../../ThirdParty/FlatBuffers/include"),
                Path.Combine(ModuleDirectory, "../../ThirdParty/libtess2/Include"),
                Path.Combine(EngineDirectory, "Source/ThirdParty/zlib")
            }
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
            }
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
                "ProceduralMeshComponent",
				"MeshDescription",
				"StaticMeshDescription",
				"MeshConversion",
				"GeometryScriptingCore",
				"GeometryCore",
				"GeometryFramework",
				"DynamicMesh",
				"zlib"
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"InputCore",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"Json",
				"Chaos",
				"ChaosCore"
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
			);

		// Zlib (Unreal dependency)
        AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		// libtess2 is compiled from ThirdParty/libtess2/Source by the wrappers in Private/Tess2, so every platform
		// (Win64, Linux, Wasm, ...) gets it without a prebuilt library.
    }
}
