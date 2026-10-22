// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant

using UnrealBuildTool;

public class FragmentsEditor: ModuleRules
{
    public FragmentsEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "EditorSubsystem",
            "UnrealEd",
            "WorkspaceMenuStructure",
            "Projects",
            "DesktopPlatform",
            "PropertyEditor",
            "EditorWidgets",
            "Json",
            "JsonUtilities",
            "FragmentsUnreal",
            "InputCore",
            "ApplicationCore"
        });
    }
}
