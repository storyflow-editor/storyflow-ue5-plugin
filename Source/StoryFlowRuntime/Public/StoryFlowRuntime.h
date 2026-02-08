// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

STORYFLOWRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogStoryFlow, Log, All);

class FStoryFlowRuntimeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
