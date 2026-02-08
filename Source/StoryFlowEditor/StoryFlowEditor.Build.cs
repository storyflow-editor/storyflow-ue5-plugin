// Copyright 2026 StoryFlow. All Rights Reserved.

using UnrealBuildTool;

public class StoryFlowEditor : ModuleRules
{
	public StoryFlowEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
