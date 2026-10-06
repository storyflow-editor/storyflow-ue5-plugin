// Copyright 2026 StoryFlow. All Rights Reserved.
#include "Evaluation/StoryFlowExecutionSnapshot.h"
#include "Evaluation/StoryFlowRollbackController.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"

/** Count first, detach second. Pointer-key memo preserves map aliases without quadratic scans. */
struct FStoryFlowRollbackCloner
{
    int64 Bytes = 0;
    FString Reason;
    bool bDetach;
    TMap<const void*, TSharedPtr<TArray<FStoryFlowMapEntry>>> Memo;
    TSet<const void*> Active;
    explicit FStoryFlowRollbackCloner(bool Detach) : bDetach(Detach) {}
    bool Add(int64 N) { Bytes += N; if (Bytes > FStoryFlowRollbackController::PayloadLimit) Reason = TEXT("budget"); return Reason.IsEmpty(); }
    bool String(const FString& S) { return Add(32 + int64(S.Len()) * sizeof(TCHAR)); }
    bool Strings(const TArray<FString>& A) { for (const auto& S : A) if (!String(S)) return false; return true; }
    bool Variant(FStoryFlowVariant& V, int32 Depth = 0)
    {
        if (Depth > 64) { Reason = TEXT("unsupportedState"); return false; }
        if (!Add(sizeof(FStoryFlowVariant)) || !String(V.StringValue) || !Add(V.SerializedArrayData.Num())) return false;
        if (V.MapValue.IsValid())
        {
            const void* Key = V.MapValue.Get();
            if (Active.Contains(Key)) { Reason = TEXT("unsupportedState"); return false; }
            if (auto* Prior = Memo.Find(Key)) { if (bDetach) V.MapValue = *Prior; return true; }
            auto Old = V.MapValue;
            auto New = bDetach ? MakeShared<TArray<FStoryFlowMapEntry>>() : TSharedPtr<TArray<FStoryFlowMapEntry>>();
            Memo.Add(Key, New); Active.Add(Key);
            if (!Add(int64(Old->Num()) * sizeof(FStoryFlowMapEntry))) return false;
            if (bDetach) { *New = *Old; V.MapValue = New; }
            auto& Entries = bDetach ? *New : *Old;
            for (auto& E : Entries) if (!Variant(E.Key, Depth+1) || !Variant(E.Value, Depth+1)) return false;
            Active.Remove(Key);
        }
        for (auto& E : V.ArrayValue) if (!Variant(E, Depth+1)) return false;
        if (bDetach) V.SerializedArrayData.Reset();
        return true;
    }
    bool Variable(FStoryFlowVariable& V)
    { return String(V.Id) && String(V.Name) && Strings(V.EnumValues) && Strings(V.KeyEnumValues) && Strings(V.ValueEnumValues) && Variant(V.Value); }
    bool Variables(TMap<FString, FStoryFlowVariable>& Map)
    { for (auto& P : Map) if (!Add(64) || !String(P.Key) || !Variable(P.Value)) return false; return true; }
    bool Values(TMap<FString, FStoryFlowVariant>& Map)
    { for (auto& P : Map) if (!Add(64) || !String(P.Key) || !Variant(P.Value)) return false; return true; }
    bool Nodes(TMap<FString, FNodeRuntimeState>& Map)
    {
        for (auto& P : Map)
        {
            auto& N = P.Value;
            if (!String(P.Key) || !Add(sizeof(FNodeRuntimeState)) || !Variant(N.CachedOutput) || !Variant(N.LoopKey) || !Variant(N.LoopValue)) return false;
            for (auto& V : N.LoopArray) if (!Variant(V)) return false;
            for (auto& E : N.LoopEntries) if (!Variant(E.Key) || !Variant(E.Value)) return false;
            if (!Values(N.OutputValues) || !Variables(N.MapOutputVariables) || !Variable(N.DataAssetMapSnapshot) || !Variable(N.MapExecutionOutput)) return false;
            for (const auto& D : N.OutputDeclarations) if (!String(D.Key) || !Add(32)) return false;
        }
        return true;
    }
    bool Loops(const TArray<FStoryFlowLoopContext>& L) { for (const auto& E : L) if (!String(E.NodeId) || !Add(32)) return false; return true; }
    bool Context(FStoryFlowExecutionContext& C)
    {
        if (!String(C.CurrentNodeId) || !String(C.SeedLanguageCode) || !Variables(C.LocalVariables) || !Nodes(C.NodeRuntimeStates) || !Loops(C.LoopStack)) return false;
        for (auto& F : C.CallStack) if (!String(F.ScriptPath) || !String(F.ReturnNodeId) || !Variables(F.SavedVariables) || !Strings(F.SavedFlowStack)) return false;
        for (auto& A : C.CallerActivations) if (!Loops(A.Loops) || !Nodes(A.Nodes)) return false;
        for (const auto& F : C.FlowCallStack) if (!String(F.FlowId)) return false;
        auto& D = C.CurrentDialogueState;
        if (!String(D.NodeId) || !String(D.Title) || !String(D.Text) || !Strings(D.Tags) || !String(D.Character.Name) || !String(D.Character.CharacterPath) || !Values(D.Character.Variables)) return false;
        for (const auto& O : D.Options) if (!String(O.Id) || !String(O.Text)) return false;
        for (const auto& O : D.TextBlocks) if (!String(O.Id) || !String(O.Text)) return false;
        for (const auto* Index : { &C.LocalVariableNameIndex, &C.GlobalVariableNameIndex }) for (const auto& P : *Index) if (!String(P.Key) || !String(P.Value)) return false;
        for (const auto* Set : { &C.WarnedUnknownNodes, &C.WarnedMapNodes, &C.WarnedDataAssetNodes, &C.WarnedCharacterIds }) for (const auto& S : *Set) if (!String(S)) return false;
        return true;
    }
    bool Shared(TMap<FString, FStoryFlowVariable>& G, TMap<FString, FStoryFlowCharacterDef>& C, TSet<FString>& Once, StoryFlowDataAssets::FOverlay& O)
    {
        if (!Variables(G)) return false;
        for (auto& P : C) if (!String(P.Key) || !String(P.Value.Name) || !String(P.Value.Image) || !Variables(P.Value.Variables)) return false;
        for (const auto& S : Once) if (!String(S)) return false;
        for (auto& P : O) if (!String(P.Key) || !Values(P.Value)) return false;
        return true;
    }
};

namespace
{
    bool Copy(const FStoryFlowExecutionContext& Source, const TMap<FString,FStoryFlowVariable>& Globals,
        const TMap<FString,FStoryFlowCharacterDef>& Characters, const TSet<FString>& Once, const StoryFlowDataAssets::FOverlay& Overlay,
        FStoryFlowExecutionSnapshot& Out, FString& Reason)
    {
        FStoryFlowRollbackCloner Count(false);
        // The measuring pass is read-only and aborts before copying any container.
        if (!Count.Context(const_cast<FStoryFlowExecutionContext&>(Source)) ||
            !Count.Shared(const_cast<TMap<FString,FStoryFlowVariable>&>(Globals), const_cast<TMap<FString,FStoryFlowCharacterDef>&>(Characters),
                const_cast<TSet<FString>&>(Once), const_cast<StoryFlowDataAssets::FOverlay&>(Overlay)))
        { Reason = Count.Reason; return false; }
        Out.Context = Source; Out.Globals = Globals; Out.Characters = Characters; Out.Once = Once; Out.Overlay = Overlay; Out.Bytes = Count.Bytes;
        FStoryFlowRollbackCloner Clone(true);
        if (!Clone.Context(Out.Context) || !Clone.Shared(Out.Globals, Out.Characters, Out.Once, Out.Overlay)) { Reason = Clone.Reason; return false; }
        Out.Context.ExternalGlobalVariables = nullptr; Out.Context.ExternalCharacters = nullptr; Out.Context.ExternalUsedOnceOnlyOptions = nullptr;
        Out.Context.DataAssetStore = {}; Out.Context.CharacterIdToPath = nullptr; Out.Context.ActiveLanguage = nullptr;
        return true;
    }
}

bool FStoryFlowExecutionSnapshot::Capture(const FStoryFlowExecutionContext& C, uint64 Entry, FStoryFlowExecutionSnapshot& Out, FString& Reason)
{
    if (!C.ExternalGlobalVariables || !C.ExternalCharacters || !C.ExternalUsedOnceOnlyOptions || !C.DataAssetStore.IsValid())
    { Reason = TEXT("unsupportedState"); return false; }
    if (!Copy(C, *C.ExternalGlobalVariables, *C.ExternalCharacters, *C.ExternalUsedOnceOnlyOptions, *C.DataAssetStore.Overlay, Out, Reason)) return false;
    Out.Entry = Entry;
    Out.Background = C.PersistentBackgroundImage; Out.Image = C.CurrentDialogueState.Image;
    Out.Portrait = C.CurrentDialogueState.Character.Image; Out.Audio = C.CurrentDialogueState.Audio;
    Out.Context.PersistentBackgroundImage = nullptr; Out.Context.CurrentDialogueState.Image = nullptr;
    Out.Context.CurrentDialogueState.Character.Image = nullptr; Out.Context.CurrentDialogueState.Audio = nullptr;
    for (auto& P : Out.Characters) { Out.CachedPortraits.Add(P.Key, P.Value.CachedImage); P.Value.CachedImage = nullptr; }
    return true;
}

bool FStoryFlowExecutionSnapshot::Prepare(const FStoryFlowExecutionContext& Live, FStoryFlowExecutionSnapshot& Out, FString& Reason) const
{
    auto* Script = Context.CurrentScript.Get(); auto* Project = Context.Project.Get();
    if (!Project || Project != Live.Project.Get() || !Script || Project->GetScriptByPath(Script->ScriptPath) != Script ||
        !Script->Nodes.Contains(Context.CurrentNodeId) || !Script->Nodes.Contains(Context.CurrentDialogueState.NodeId) ||
        Context.CallStack.Num() != Context.CallerActivations.Num()) { Reason = TEXT("contentChanged"); return false; }
    for (const auto& F : Context.CallStack)
        if (!F.ScriptAsset.IsValid() || Project->GetScriptByPath(F.ScriptPath) != F.ScriptAsset.Get() || !F.ScriptAsset->Nodes.Contains(F.ReturnNodeId))
        { Reason = TEXT("contentChanged"); return false; }
    auto ValidFlow = [](const UStoryFlowScriptAsset* Asset, const FString& Id) {
        if (Id.Equals(TEXT("start"), ESearchCase::IgnoreCase)) return Asset->Nodes.Contains(TEXT("0"));
        for (const auto& Flow : Asset->Flows) if (Flow.Id == Id) return true;
        for (const auto& Node : Asset->Nodes)
            if (Node.Value.Type == EStoryFlowNodeType::EntryFlow && Node.Value.Data.FlowId == Id) return true;
        return false;
    };
    for (const auto& Flow : Context.FlowCallStack)
        if (!ValidFlow(Script, Flow.FlowId)) { Reason = TEXT("contentChanged"); return false; }
    for (const auto& Frame : Context.CallStack) for (const auto& Id : Frame.SavedFlowStack)
        if (!ValidFlow(Frame.ScriptAsset.Get(), Id)) { Reason = TEXT("contentChanged"); return false; }
    auto ValidActivation = [](const UStoryFlowScriptAsset* Asset, const TArray<FStoryFlowLoopContext>& Loops, const TMap<FString,FNodeRuntimeState>& Nodes) {
        for (const auto& Loop : Loops) if (!Asset->Nodes.Contains(Loop.NodeId)) return false;
        for (const auto& Node : Nodes) if (!Asset->Nodes.Contains(Node.Key)) return false;
        return true;
    };
    if (!ValidActivation(Script, Context.LoopStack, Context.NodeRuntimeStates)) { Reason = TEXT("contentChanged"); return false; }
    for (int32 I=0; I<Context.CallStack.Num(); ++I)
        if (!ValidActivation(Context.CallStack[I].ScriptAsset.Get(), Context.CallerActivations[I].Loops, Context.CallerActivations[I].Nodes))
        { Reason = TEXT("contentChanged"); return false; }
    const auto& Authored = Script->Nodes.FindChecked(Context.CurrentDialogueState.NodeId).Data;
    for (const auto& Option : Context.CurrentDialogueState.Options)
        if (!Authored.Options.ContainsByPredicate([&](const auto& Choice) { return Choice.Id == Option.Id; })) { Reason = TEXT("contentChanged"); return false; }
    for (const auto& Block : Context.CurrentDialogueState.TextBlocks)
        if (!Authored.TextBlocks.ContainsByPredicate([&](const auto& Text) { return Text.Id == Block.Id; })) { Reason = TEXT("contentChanged"); return false; }
    if (!Copy(Context, Globals, Characters, Once, Overlay, Out, Reason)) return false;
    Out.Entry = Entry; Out.Background = Background; Out.Image = Image; Out.Portrait = Portrait; Out.Audio = Audio; Out.LoopSound = LoopSound; Out.CachedPortraits = CachedPortraits;
    Out.LoopPosition = LoopPosition; Out.LoopVolume = LoopVolume;
    auto& Stage = Out.Context;
    Stage.ExternalGlobalVariables = &Out.Globals; Stage.ExternalCharacters = &Out.Characters; Stage.ExternalUsedOnceOnlyOptions = &Out.Once;
    Out.ResolverShared.Project = Project;
    Stage.DataAssetStore = { Live.DataAssetStore.Seed, &Out.Overlay, &Out.ResolverShared };
    Stage.CharacterIdToPath = Live.CharacterIdToPath; Stage.ActiveLanguage = Live.ActiveLanguage; Stage.SeenSharedRevision = 0;
    Stage.SeedLanguageCode = Live.SeedLanguageCode;
    auto& D = Stage.CurrentDialogueState;
    FStoryFlowNode* Node = Script->Nodes.Find(D.NodeId);
    const FString Speaker = Stage.ResolveCharacterRef(Node->Data.CharacterRefId, Node->Data.Character);
    D.Character.CharacterPath = Speaker;
    if (const auto* Character = Out.Characters.Find(Speaker))
    { D.Character.Name = Stage.ResolveCharacterName(*Character); D.Character.Variables.Reset(); for (const auto& P : Character->Variables) D.Character.Variables.Add(P.Key, P.Value.Value); }
    D.Title = Stage.InterpolateVariables(Stage.GetString(Node->Data.Title)); D.Text = Stage.InterpolateVariables(Stage.GetString(Node->Data.Text));
    for (auto& O : D.Options) for (const auto& Choice : Node->Data.Options) if (Choice.Id == O.Id) O.Text = Stage.InterpolateVariables(Stage.GetString(Choice.Text));
    for (auto& B : D.TextBlocks) for (const auto& Block : Node->Data.TextBlocks) if (Block.Id == B.Id) B.Text = Stage.InterpolateVariables(Stage.GetString(Block.Text));
    D.bAudioAdvanceOnEnd = false; D.bAudioAllowSkip = false;
    Stage.bIsPaused = false; Stage.bEnteringDialogueViaEdge = false; Stage.ProcessingDepth = 0; Stage.EvaluationDepth = 0;
    FStoryFlowRollbackCloner Count(false);
    if (!Count.Context(Stage) || !Count.Shared(Out.Globals, Out.Characters, Out.Once, Out.Overlay)) { Reason = Count.Reason; return false; }
    Out.Bytes = Count.Bytes;
    return true;
}

void FStoryFlowExecutionSnapshot::Apply(FStoryFlowExecutionContext& Live)
{
    auto* G = Live.ExternalGlobalVariables; auto* C = Live.ExternalCharacters; auto* OnceSet = Live.ExternalUsedOnceOnlyOptions;
    auto Store = Live.DataAssetStore; auto* IDs = Live.CharacterIdToPath; auto* Language = Live.ActiveLanguage;
    *G = MoveTemp(Globals); *OnceSet = MoveTemp(Once); *Store.Overlay = MoveTemp(Overlay);
    for (auto It = C->CreateIterator(); It; ++It) if (!Characters.Contains(It.Key())) It.RemoveCurrent();
    for (auto& P : Characters) { P.Value.CachedImage = CachedPortraits.FindRef(P.Key).Get(); C->FindOrAdd(P.Key) = MoveTemp(P.Value); }
    Live = MoveTemp(Context); Live.ExternalGlobalVariables = G; Live.ExternalCharacters = C; Live.ExternalUsedOnceOnlyOptions = OnceSet;
    Live.DataAssetStore = Store; Live.CharacterIdToPath = IDs; Live.ActiveLanguage = Language;
    Store.NotifyChanged(); Live.SeenSharedRevision = Store.SharedState ? Store.SharedState->Revision : 0;
    Live.PersistentBackgroundImage = Background.Get(); Live.CurrentDialogueState.Image = Image.Get();
    Live.CurrentDialogueState.Character.Image = Portrait.Get(); Live.CurrentDialogueState.Audio = Audio.Get();
}
