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
				"JsonUtilities",
				// Lipsync reads the LIVE output spectrum. UAudioComponent's own FFT and envelope readers are
				// COOKED — they need per-asset analysis ticked on every dialogue wave — so submix analysis
				// through UAudioMixerBlueprintLibrary is the only path that needs no per-asset setup.
				"AudioMixer"
			}
		);
	}
}
