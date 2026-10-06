// Copyright 2026 StoryFlow. All Rights Reserved.
#include "StoryFlowRollbackSpy.h"
#include "Engine/World.h"
void UStoryFlowRollbackWidgetSpy::OnDialogueRestored_Implementation(const FStoryFlowDialogueState& State)
{
    ++Restored; Visible = State;
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(AutoTimer);
}
void UStoryFlowRollbackWidgetSpy::StartAutoTimer()
{
    if (GetWorld()) GetWorld()->GetTimerManager().SetTimer(AutoTimer, FTimerDelegate::CreateWeakLambda(this, [this]() { ++AutoTicks; }), 0.1f, true);
}
