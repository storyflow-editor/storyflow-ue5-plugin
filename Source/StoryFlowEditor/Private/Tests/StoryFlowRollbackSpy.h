// Copyright 2026 StoryFlow. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UI/StoryFlowDialogueWidget.h"
#include "TimerManager.h"
#include "Components/Button.h"
#include "StoryFlowRollbackSpy.generated.h"

/** Opt-in consuming widget exercises the BlueprintNativeEvent contract used by authored widgets. */
UCLASS()
class UStoryFlowRollbackWidgetSpy : public UStoryFlowDialogueWidget
{
    GENERATED_BODY()
public:
    int32 Updated = 0, Restored = 0, AutoTicks = 0;
    FStoryFlowDialogueState Visible;
    FTimerHandle AutoTimer;
    FStoryFlowRollbackAvailability LastAvailability;
    void InstallBackButton(bool Allowed) { BackButton = NewObject<UButton>(this); BackButton->SetIsEnabled(Allowed); }
    bool BackEnabled() const { return BackButton && BackButton->GetIsEnabled(); }
    void ClickBack() { if (BackButton) BackButton->OnClicked.Broadcast(); }
    virtual void OnRollbackAvailabilityChanged_Implementation(const FStoryFlowRollbackAvailability& State) override { LastAvailability=State; }
    virtual void OnDialogueUpdated_Implementation(const FStoryFlowDialogueState& State) override
    { ++Updated; Visible = State; }
    virtual void OnDialogueRestored_Implementation(const FStoryFlowDialogueState& State) override;
    void StartAutoTimer();
    void DestructForTest() { NativeDestruct(); }
};

UCLASS()
class UStoryFlowRollbackObserver : public UObject
{
    GENERATED_BODY()
public:
    int32 Updated = 0, Restored = 0, Availability = 0;
    FStoryFlowRollbackAvailability LastAvailability;
    TFunction<void()> OnRestore, OnAvailable, OnUpdate, OnEnd;
    UFUNCTION() void Update(const FStoryFlowDialogueState& State) { ++Updated; if (OnUpdate) OnUpdate(); }
    UFUNCTION() void Restore(const FStoryFlowDialogueState& State) { ++Restored; if (OnRestore) OnRestore(); }
    UFUNCTION() void Available(const FStoryFlowRollbackAvailability& State) { ++Availability; LastAvailability = State; if (OnAvailable) OnAvailable(); }
    UFUNCTION() void End() { if (OnEnd) OnEnd(); }
};
