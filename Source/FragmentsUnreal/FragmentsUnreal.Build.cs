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

		// Platform-specific libtess2 static libs
        string TessLibPath_Win64 = Path.Combine(ModuleDirectory, "../../ThirdParty/libtess2/Lib/Win64");
        string TessLibPath_Android = Path.Combine(ModuleDirectory, "../../ThirdParty/libtess2/Lib/Android/ARM64");
		
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicAdditionalLibraries.Add(Path.Combine(TessLibPath_Win64, "tess2.lib"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PublicAdditionalLibraries.Add(Path.Combine(TessLibPath_Android, "libtess2.a"));
		}
    }
}
