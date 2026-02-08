// Copyright 2026 StoryFlow. All Rights Reserved.

#include "StoryFlowBlueprintLibrary.h"
#include "Import/StoryFlowImporter.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"

UStoryFlowProjectAsset* UStoryFlowBlueprintLibrary::ImportStoryFlowProject(const FString& BuildDirectory, const FString& ContentPath)
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Importing project from %s to %s"), *BuildDirectory, *ContentPath);

	UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(BuildDirectory, ContentPath);

	if (Project)
	{
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Project imported successfully"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("StoryFlow: Failed to import project"));
	}

	return Project;
}

UStoryFlowScriptAsset* UStoryFlowBlueprintLibrary::ImportStoryFlowScript(const FString& JsonFilePath, const FString& ContentPath)
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Importing script from %s to %s"), *JsonFilePath, *ContentPath);

	UStoryFlowScriptAsset* Script = UStoryFlowImporter::ImportScript(JsonFilePath, ContentPath);

	if (Script)
	{
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Script imported successfully"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("StoryFlow: Failed to import script"));
	}

	return Script;
}
