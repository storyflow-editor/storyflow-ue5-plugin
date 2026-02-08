// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

STORYFLOWRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogStoryFlow, Log, All);

/** Normalize a script path by stripping the .json extension if present */
inline FString NormalizeScriptPath(const FString& Path)
{
	FString Result = Path;
	Result.RemoveFromEnd(TEXT(".json"));
	return Result;
}

class FStoryFlowRuntimeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
