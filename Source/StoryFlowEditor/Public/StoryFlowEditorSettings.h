// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StoryFlowEditorSettings.generated.h"

/**
 * Per-project user settings for StoryFlow Editor integration.
 * Persisted to Saved/Config/<Platform>/EditorPerProjectUserSettings.ini.
 */
UCLASS(Config=EditorPerProjectUserSettings)
class STORYFLOWEDITOR_API UStoryFlowEditorSettings : public UObject
{
	GENERATED_BODY()

public:
	/** WebSocket port used to connect to the StoryFlow Editor */
	UPROPERTY(Config)
	int32 Port = 9000;
};
