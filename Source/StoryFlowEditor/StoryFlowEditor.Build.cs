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
				// Required to link, despite StoryFlowRuntime depending on UMG publicly:
				// the automation tests subclass UStoryFlowDialogueWidget, and the subclass
				// vtable references UMG symbols directly. Dropping this entry fails the
				// module link with 83 unresolved UMG externals.
				"UMG",
				"EditorScriptingUtilities",
				"ToolMenus",
				"Projects",
				"DesktopPlatform",
				"SourceControl"
			}
		);
	}
}
