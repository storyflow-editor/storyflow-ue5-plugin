// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StoryFlowBlueprintLibrary.generated.h"

class UStoryFlowProjectAsset;
class UStoryFlowScriptAsset;

/**
 * Blueprint function library for StoryFlow editor operations
 * These functions are available in any Blueprint in the editor
 */
UCLASS()
class STORYFLOWEDITOR_API UStoryFlowBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Import a StoryFlow project from a build directory
	 *
	 * @param BuildDirectory Path to the build folder (containing project.json)
	 * @param ContentPath Unreal content path where assets will be created
	 * @return The imported project asset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow", meta = (DevelopmentOnly))
	static UStoryFlowProjectAsset* ImportStoryFlowProject(const FString& BuildDirectory, const FString& ContentPath = TEXT("/Game/StoryFlow"));

	/**
	 * Import a single StoryFlow script from JSON
	 *
	 * @param JsonFilePath Full path to the script JSON file
	 * @param ContentPath Unreal content path where the asset will be created
	 * @return The imported script asset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow", meta = (DevelopmentOnly))
	static UStoryFlowScriptAsset* ImportStoryFlowScript(const FString& JsonFilePath, const FString& ContentPath = TEXT("/Game/StoryFlow/Scripts"));
};
