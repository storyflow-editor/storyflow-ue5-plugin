// Copyright 2026 StoryFlow. All Rights Reserved.

using UnrealBuildTool;

public class StoryFlowEditor : ModuleRules
{
	public StoryFlowEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// minimp3 (CC0 public domain) - MP3 to WAV conversion during import
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "ThirdParty"));

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"StoryFlowRuntime",
				"Json",
				"JsonUtilities"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"AssetTools",
				"EditorSubsystem",
				"WebSockets",
				"Slate",
				"SlateCore",
				"EditorScriptingUtilities",
				"ToolMenus",
				"Projects"
			}
		);
	}
}
