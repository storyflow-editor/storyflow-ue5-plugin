// Copyright 2026 StoryFlow. All Rights Reserved.
#include "Evaluation/StoryFlowRestoredListener.h"
#include "Components/StoryFlowComponent.h"
void UStoryFlowRestoredListener::Restore(const FStoryFlowDialogueState& State)
{
    if (Source.IsValid() && Source->IsCurrentRestoredDelivery() && Callback) Callback(State, Source.Get());
}
