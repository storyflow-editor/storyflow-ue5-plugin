// Copyright 2026 StoryFlow. All Rights Reserved.

using UnrealBuildTool;

public class StoryFlowRuntime : ModuleRules
{
	public StoryFlowRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UMG",
				"Slate",
				"SlateCore",
				"Json",
				"JsonUtilities"
			}
		);
	}
}
