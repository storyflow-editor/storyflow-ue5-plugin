// Copyright 2026 StoryFlow. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowRestoredListener.generated.h"
class UStoryFlowComponent;

/** One relay per binding: an in-flight multicast keeps its original source identity. */
UCLASS(Transient)
class STORYFLOWRUNTIME_API UStoryFlowRestoredListener : public UObject
{
    GENERATED_BODY()
public:
    TWeakObjectPtr<UStoryFlowComponent> Source;
    TFunction<void(const FStoryFlowDialogueState&, UStoryFlowComponent*)> Callback;
    UFUNCTION() void Restore(const FStoryFlowDialogueState& State);
};
