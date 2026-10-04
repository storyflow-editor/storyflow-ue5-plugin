// Copyright 2026 StoryFlow. All Rights Reserved.
#include "Evaluation/StoryFlowRollbackController.h"
#include "Components/StoryFlowComponent.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "Components/AudioComponent.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "UObject/UObjectIterator.h"
#include "StoryFlowRuntime.h"
#include "Sound/SoundWave.h"
#include "Templates/UnrealTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS
bool FStoryFlowRollbackController::bFailPrepare = false;
bool FStoryFlowRollbackController::bFailCommit = false;
bool FStoryFlowRollbackController::bFailRecovery = false;
#endif

FStoryFlowRollbackController::FStoryFlowRollbackController(UStoryFlowComponent* C, UStoryFlowSubsystem* S, int32 L) : Component(C), Subsystem(S), Limit(L) {}
FStoryFlowRollbackAvailability FStoryFlowRollbackController::Availability() const
{
    FStoryFlowRollbackAvailability A;
    if (bContentChanged) A.Reason = TEXT("contentChanged");
    else if (!Subsystem.IsValid() || !Subsystem->OwnsRollback(this)) A.Reason = TEXT("multipleSessions");
    else if (Subsystem->IsRollbackMutationActive()) A.Reason = TEXT("barrier");
    else if (bRestoring || Depth > 0) A.Reason = TEXT("busy");
    else if (bInvalid || History.Num() < 2) A.Reason = Reason;
    else { A.bCanGoBack = true; A.Steps = History.Num()-1; A.Reason.Reset(); }
    return A;
}
void FStoryFlowRollbackController::Publish(bool Force)
{
    auto* C = Component.Get(); if (!C || C->Rollback.Get() != this) return;
    const auto A = Availability();
    if (!Force && A.bCanGoBack == Published.bCanGoBack && A.Steps == Published.Steps && A.Reason == Published.Reason) return;
    Published = A; C->PublishRollbackAvailability();
}
void FStoryFlowRollbackController::Clear(const FString& Why, bool Permanent)
{
    History.Reset(); Bytes = 0; Reason = Why; bContentChanged |= Permanent; bInvalid = false;
    BlockedEntry = Component.IsValid() ? Component->DialogueEntrySerial : 0; bBlocked = true; Publish();
}
void FStoryFlowRollbackController::BeforeLeave()
{ if (History.Num() && Component.IsValid() && History.Last().Entry != Component->DialogueEntrySerial) Clear(TEXT("busy")); }
void FStoryFlowRollbackController::Exit() { --Depth; if (Depth == 0) Capture(); }
void FStoryFlowRollbackController::Capture()
{
    auto* C = Component.Get(); auto* S = Subsystem.Get();
    if (!C || !S || C->Rollback.Get() != this || bContentChanged || bInvalid || bRestoring || Depth || S->IsRollbackMutationActive() || !S->OwnsRollback(this)) return;
    auto& Context = C->ExecutionContext;
    if (!Context.bIsExecuting || !Context.bIsWaitingForInput || !Context.CurrentDialogueState.bIsValid) return;
    if ((bBlocked && BlockedEntry == C->DialogueEntrySerial) || (History.Num() && History.Last().Entry == C->DialogueEntrySerial)) { Publish(true); return; }
    const double Start = FPlatformTime::Seconds(); FStoryFlowExecutionSnapshot Snapshot; FString Failure;
    if (!FStoryFlowExecutionSnapshot::Capture(Context, C->DialogueEntrySerial, Snapshot, Failure))
    { Clear(Failure); UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Rollback capture failed: %s"), *Failure); return; }
    if (C->CurrentDialogueAudio && C->CurrentDialogueAudio->ComponentTags.Contains(TEXT("StoryFlowLoop")))
    { Snapshot.LoopSound = C->CurrentDialogueAudio->Sound; Snapshot.LoopPosition = C->CurrentAudioPosition; Snapshot.LoopVolume = C->CurrentDialogueAudio->VolumeMultiplier; }
    Bytes += Snapshot.Bytes; History.Add(MoveTemp(Snapshot)); ++Captures;
    while (History.Num() > Limit+1 || Bytes > PayloadLimit) { Bytes -= History[0].Bytes; History.RemoveAt(0); }
    Reason = TEXT("empty"); CaptureMilliseconds = (FPlatformTime::Seconds()-Start)*1000; Publish(true);
}
FStoryFlowRollbackResult FStoryFlowRollbackController::GoBack()
{
    FStoryFlowRollbackResult R; if (bNotifying) { R.Reason = TEXT("busy"); return R; }
    const auto A = Availability(); if (!A.bCanGoBack) { R.Reason = A.Reason; return R; }
    auto* C = Component.Get(); auto* S = Subsystem.Get(); bRestoring = true;
    auto& Live = C->ExecutionContext;
    FStoryFlowExecutionSnapshot Target, Recovery; FString Failure;
    const double PrepareStart = FPlatformTime::Seconds();
#if WITH_DEV_AUTOMATION_TESTS
    if (bFailPrepare) { bFailPrepare = false; Failure = TEXT("restoreFailed"); }
#endif
    if (Failure.IsEmpty() && !History[History.Num()-2].Prepare(Live, Target, Failure)) {}
    if (Failure.IsEmpty() && !FStoryFlowExecutionSnapshot::Capture(Live, C->DialogueEntrySerial, Recovery, Failure)) {}
    if (Failure.IsEmpty() && (!S->OwnsRollback(this) || bContentChanged || C->Rollback.Get() != this)) Failure = TEXT("contentChanged");
    PrepareMilliseconds = (FPlatformTime::Seconds()-PrepareStart)*1000;
    if (!Failure.IsEmpty()) { bRestoring = false; bInvalid = true; Reason = Failure; Publish(); R.Reason = Failure; return R; }
    // Suspend native playback without destroying its logical identity; failed application resumes
    // the same component, while callback generations only increase. No Blueprint playback hook runs.
    UAudioComponent* Audio = C->CurrentDialogueAudio;
    const bool WasPaused = Audio && Audio->GetPlayState() == EAudioComponentPlayState::Paused;
    const bool WasWaiting = C->bWaitingForAudioAdvance, WasAllowSkip = C->bAudioAdvanceAllowSkip;
    const FString Speaker = C->CurrentSpeakerPath;
    ++C->AudioGeneration;
    if (Audio) { Audio->OnAudioFinishedNative.RemoveAll(C); Audio->OnAudioFinished.RemoveAll(C); Audio->OnAudioPlaybackPercentNative.RemoveAll(C); Audio->SetPaused(true); }
    const double CommitStart = FPlatformTime::Seconds();
    Target.Apply(Live);
    bool ApplicationFailed = false;
#if WITH_DEV_AUTOMATION_TESTS
    ApplicationFailed = bFailCommit; bFailCommit = false;
#endif
    if (ApplicationFailed)
    {
        bool RecoveryFailed = false;
#if WITH_DEV_AUTOMATION_TESTS
        RecoveryFailed = bFailRecovery; bFailRecovery = false;
#endif
        if (!RecoveryFailed)
        {
            Recovery.Apply(Live); C->CurrentSpeakerPath = Speaker; C->bWaitingForAudioAdvance = WasWaiting; C->bAudioAdvanceAllowSkip = WasAllowSkip;
            if (Audio) { C->BindAudioFinished(); Audio->SetPaused(WasPaused); }
        }
        else C->StopDialogue();
        bRestoring = false; bInvalid = true; Reason = TEXT("restoreFailed"); Publish(); R.Reason = Reason; return R;
    }
    C->StopDialogueAudio_Implementation();
    // Persistent loop audio is owned presentation; native path only, without replaying host hooks.
    if (Target.LoopSound.IsValid()) C->PlayDialogueAudioNative(Target.LoopSound.Get(), true, Target.LoopPosition, Target.LoopVolume, true);
    C->CurrentSpeakerPath = Live.CurrentDialogueState.Character.CharacterPath;
    Bytes -= History.Last().Bytes; History.Pop();
    ++C->DialogueEntrySerial; History.Last().Entry = C->DialogueEntrySerial; C->RestoredEntry = C->DialogueEntrySerial;
    CommitMilliseconds = (FPlatformTime::Seconds()-CommitStart)*1000;
    bRestoring = false; bNotifying = true; Publish();
    if (C->Rollback.Get() == this && C->IsCurrentDialogueRestored())
    {
        const auto State = Live.CurrentDialogueState;
        TGuardValue<uint64> Delivery(C->RestoredDeliveryEntry, C->DialogueEntrySerial);
        C->OnDialogueRestored.Broadcast(State);
    }
    bNotifying = false; R.bOk = true; return R;
}

void UStoryFlowSubsystem::RegisterRollback(TSharedPtr<FStoryFlowRollbackController> Owner)
{
    RollbackOwners.Add(Owner);
    if (RollbackContentDepth || FStoryFlowContentUpdateScope::IsChanging(GetProject())) Owner->Clear(TEXT("contentChanged"), true);
    else if (RollbackMutationDepth) Owner->Clear(TEXT("barrier"));
    else if (ActiveDialogueCount != 1) InvalidateRollback(TEXT("multipleSessions"));
}
void UStoryFlowSubsystem::UnregisterRollback(FStoryFlowRollbackController* Owner)
{ RollbackOwners.RemoveAll([Owner](const auto& P) { return P.Get() == Owner; }); }
void UStoryFlowSubsystem::InvalidateRollback(const FString& Why, bool Permanent)
{ const auto Owners = RollbackOwners; for (auto P : Owners) P->Clear(Why, Permanent); }
bool UStoryFlowSubsystem::OwnsRollback(const FStoryFlowRollbackController* Owner) const
{ return ActiveDialogueCount == 1 && RollbackOwners.ContainsByPredicate([Owner](const auto& P) { return P.Get() == Owner; }); }

FStoryFlowRollbackMutationScope::FStoryFlowRollbackMutationScope(UStoryFlowSubsystem* S, bool Content) : Subsystem(S), bContent(Content)
{
    if (!S || (!Content && S->RollbackOwners.IsEmpty())) { Subsystem = nullptr; return; }
    if (Content) ++S->RollbackContentDepth; else ++S->RollbackMutationDepth;
    S->InvalidateRollback(Content ? TEXT("contentChanged") : TEXT("barrier"), Content);
}
FStoryFlowRollbackMutationScope::~FStoryFlowRollbackMutationScope()
{
    if (!Subsystem) return;
    if (bContent) --Subsystem->RollbackContentDepth;
    else { Subsystem->InvalidateRollback(); --Subsystem->RollbackMutationDepth; }
}
namespace { TMap<TWeakObjectPtr<UStoryFlowProjectAsset>, int32> ContentUpdates; }
FStoryFlowContentUpdateScope::FStoryFlowContentUpdateScope(UStoryFlowProjectAsset* P) : Project(P)
{
    if (!P) return; ++ContentUpdates.FindOrAdd(P);
    for (TObjectIterator<UStoryFlowSubsystem> It; It; ++It) if (It->GetProject() == P) It->InvalidateRollback(TEXT("contentChanged"), true);
}
FStoryFlowContentUpdateScope::~FStoryFlowContentUpdateScope()
{ if (auto* Count = ContentUpdates.Find(Project)) { if (--*Count == 0) ContentUpdates.Remove(Project); } }
bool FStoryFlowContentUpdateScope::IsChanging(UStoryFlowProjectAsset* P) { return ContentUpdates.Contains(P); }

void UStoryFlowComponent::PublishRollbackAvailability()
{
    if (!OnRollbackAvailabilityChanged.IsBound()) return;
    bPendingRollbackAvailability = true;
    if (bPublishingRollbackAvailability) return;
    TGuardValue<bool> PublishingGuard(bPublishingRollbackAvailability, true);
    // Keep dispatch on the component when observers replace the private controller.
    while (bPendingRollbackAvailability)
    {
        bPendingRollbackAvailability = false;
        const auto Current = GetRollbackAvailability();
        OnRollbackAvailabilityChanged.Broadcast(Current);
    }
}

bool UStoryFlowComponent::CanGoBack() const { return GetRollbackAvailability().bCanGoBack; }
FStoryFlowRollbackAvailability UStoryFlowComponent::GetRollbackAvailability() const
{ return Rollback ? Rollback->Availability() : FStoryFlowRollbackAvailability(); }
FStoryFlowRollbackResult UStoryFlowComponent::GoBack()
{
    const auto Owner = Rollback;
    if (Owner) return Owner->GoBack();
    FStoryFlowRollbackResult R; R.Reason = TEXT("disabled"); return R;
}
void UStoryFlowComponent::BlockRollback(const FString&)
{ const auto Owner = Rollback; if (Owner) Owner->Clear(TEXT("barrier")); }
void UStoryFlowComponent::DetachRollback()
{
    auto Outgoing = MoveTemp(Rollback); auto* S = RollbackSubsystem.Get(); RollbackSubsystem.Reset();
    if (Outgoing) { if (S) S->UnregisterRollback(Outgoing.Get()); Outgoing->Clear(TEXT("empty")); }
}
void UStoryFlowComponent::HandleBlockRollback(FStoryFlowNode* Node)
{
    // Copy the continuation before notifying: callbacks can replace the script/node storage.
    const FString NodeId = Node->Id;
    const FString Continuation = StoryFlowHandles::Source(NodeId, TEXT("1"));
    const uint64 Generation = SessionGeneration;
    const uint64 Entry = DialogueEntrySerial;
    const auto CapturedScript = ExecutionContext.CurrentScript;
    BlockRollback();
    if (SessionGeneration != Generation || DialogueEntrySerial != Entry || !ExecutionContext.bIsExecuting ||
        ExecutionContext.CurrentScript != CapturedScript || ExecutionContext.CurrentNodeId != NodeId) return;
    ProcessNextNode(Continuation);
}
void UStoryFlowComponent::BindAudioFinished()
{
    if (!CurrentDialogueAudio) return;
    const uint64 Generation = ++AudioGeneration;
    const TWeakObjectPtr<UAudioComponent> Playing = CurrentDialogueAudio;
    CurrentDialogueAudio->OnAudioFinishedNative.AddWeakLambda(this, [this, Generation, Playing](UAudioComponent*) {
        if (Generation == AudioGeneration && Playing.Get() == CurrentDialogueAudio) OnDialogueAudioFinished();
    });
    CurrentDialogueAudio->OnAudioPlaybackPercentNative.AddWeakLambda(this, [this, Generation, Playing](const UAudioComponent*, const USoundWave* Wave, const float Percent) {
        if (Generation == AudioGeneration && Playing.Get() == CurrentDialogueAudio && Wave && FMath::IsFinite(Wave->Duration) && Wave->Duration > 0 && Wave->Duration < INDEFINITELY_LOOPING_DURATION)
            CurrentAudioPosition = FMath::Clamp(Percent, 0.0f, 1.0f) * Wave->Duration;
    });
}

FString UStoryFlowComponent::ResolveCharacterName(const FStoryFlowCharacterDef& Character) const
{
    if (Character.bNameIsLiteral) return Character.Name;
    const auto* Subsystem = GetStoryFlowSubsystem(); const auto* Project = Subsystem ? Subsystem->GetProject() : nullptr;
    return Project ? Project->GetGlobalString(Character.Name, ActiveLanguageCode()) : Character.Name;
}
