// Copyright 2026 StoryFlow. All Rights Reserved.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "StoryFlowScopedWorld.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Import/StoryFlowImporter.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "EditorAssetLibrary.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Evaluation/StoryFlowRollbackController.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Lipsync/StoryFlowLipsyncComponent.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Data/StoryFlowHandles.h"
#include "WebSocket/StoryFlowSyncManager.h"
#include "WebSocket/StoryFlowWebSocketClient.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Engine/Texture2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Data/StoryFlowSaveGame.h"
#include "StoryFlowRollbackSpy.h"

struct FStoryFlowRollbackTestAccess
{
    static FStoryFlowExecutionContext& Context(UStoryFlowComponent* C) { return C->ExecutionContext; }
    static TSharedPtr<FStoryFlowRollbackController> Controller(UStoryFlowComponent* C) { return C->Rollback; }
    static void Audio(UStoryFlowComponent* C, UAudioComponent* A) { C->CurrentDialogueAudio = A; C->BindAudioFinished(); }
    static uint64 Generation(UStoryFlowComponent* C) { return C->AudioGeneration; }
    static bool Counted(UStoryFlowComponent* C) { return C->bCountedActiveDialogue; }
    static float AudioPosition(UStoryFlowComponent* C) { return C->CurrentAudioPosition; }
    static void AudioAdvance(UStoryFlowComponent* C) { C->bWaitingForAudioAdvance = true; C->bAudioAdvanceAllowSkip = true; }
    static bool WaitingAudio(UStoryFlowComponent* C) { return C->bWaitingForAudioAdvance && C->bAudioAdvanceAllowSkip; }
    static void Bind(UStoryFlowLipsyncComponent* L) { L->Driver = MakeUnique<FStoryFlowLipsyncDriver>(StoryFlowVisemeTable::Default()); L->ResolveSource(); }
    static void Tick(UStoryFlowLipsyncComponent* L) { L->TickComponent(0, LEVELTICK_All, nullptr); }
    static void Open(UStoryFlowLipsyncComponent* L) { for(int32 I=0;I<20;++I) L->Driver->AdvanceFromMagnitudes({1.0f,0.7f,0.5f},1.0f); }
    static float Pose(UStoryFlowLipsyncComponent* L) { float Sum=0; for (const auto& P:L->Driver->Current()) Sum += FMath::Abs(P.Value); return Sum; }
    static UAudioComponent* TrackedAudio(UStoryFlowLipsyncComponent* L) { return L->LineAudio.Get(); }
    static void Face(UStoryFlowLipsyncComponent* L, USkeletalMeshComponent* Mesh) { auto& Target=L->Targets.AddDefaulted_GetRef(); Target.Mesh=Mesh; Target.Morphs.Add(TEXT("jawOpen")); L->ApplyWeights(); }
    static void StaleFace(UStoryFlowLipsyncComponent* L) { L->SinceFaceCheck=2.0f; }
    static void RootEnd(UStoryFlowComponent* C) { FStoryFlowNode Node; Node.Id=TEXT("native-end"); Node.Type=EStoryFlowNodeType::End; C->HandleEnd(&Node); }
};

namespace StoryFlowRollbackTests
{
    FString Fixtures() { return FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("StoryFlowPlugin"))->GetBaseDir(), TEXT("TestContent/dialogue-rollback-v1")); }
    struct FFixture
    {
        StoryFlowTestWorld::FScopedWorld W;
        UStoryFlowProjectAsset* P = nullptr;
        FString Content;
        bool Init(const TCHAR* Name=TEXT("purchase"), bool Enabled=true, int32 Limit=100)
        {
            if (!W.Init()) return false;
            Content = TEXT("/Game/RollbackNative/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            P = UStoryFlowImporter::ImportProject(FPaths::Combine(Fixtures(), Name), Content);
            if (!P) return false;
            P->Metadata.DialogueRollback.bEnabled=Enabled; P->Metadata.DialogueRollback.HistoryLimit=Limit;
            W.Subsystem->SetProject(P); W.Component->bTraceEnabled=false; return true;
        }
        void Start() { W.Subsystem->GetUsedOnceOnlyOptions().Reset(); W.Component->StartDialogueWithScript(P->StartupScript); }
        ~FFixture() { if (W.Component) W.Component->StopDialogue(); if (W.Subsystem) W.Subsystem->SetProject(nullptr); if (!Content.IsEmpty()) UEditorAssetLibrary::DeleteDirectory(Content); }
    };
    UStoryFlowRollbackObserver* Observe(UStoryFlowComponent* C)
    {
        auto* O=NewObject<UStoryFlowRollbackObserver>(C);
        C->OnDialogueUpdated.AddDynamic(O,&UStoryFlowRollbackObserver::Update);
        C->OnDialogueRestored.AddDynamic(O,&UStoryFlowRollbackObserver::Restore);
        C->OnRollbackAvailabilityChanged.AddDynamic(O,&UStoryFlowRollbackObserver::Available);
        C->OnDialogueEnded.AddDynamic(O,&UStoryFlowRollbackObserver::End); return O;
    }
    TSharedPtr<FJsonObject> Read(const FString& Path)
    {
        FString Text; FFileHelper::LoadFileToString(Text, *Path);
        TSharedPtr<FJsonObject> Json; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json); return Json;
    }
    bool CanBack(UStoryFlowComponent* C)
    {
        struct { bool Value = false; } Params;
        if (UFunction* F = C->FindFunction(TEXT("CanGoBack"))) C->ProcessEvent(F, &Params);
        return Params.Value;
    }
    void Back(UStoryFlowComponent* C)
    {
        struct { bool Ok = false; FString Reason; } Params;
        if (UFunction* F = C->FindFunction(TEXT("GoBack"))) C->ProcessEvent(F, &Params);
    }
    void CheckValue(FAutomationTestBase& T, const FString& Label, const FStoryFlowVariant& V, const TSharedPtr<FJsonValue>& Expected)
    {
        if (Expected->Type == EJson::Array)
        {
            const auto& A = Expected->AsArray();
            if (V.IsMap())
            {
                T.TestEqual(Label + TEXT(" map size"), V.GetMap().Num(), A.Num());
                for (int32 I=0; I<FMath::Min(A.Num(), V.GetMap().Num()); ++I)
                { CheckValue(T, Label + TEXT(" key"), V.GetMap()[I].Key, A[I]->AsObject()->Values[TEXT("key")]); CheckValue(T, Label + TEXT(" value"), V.GetMap()[I].Value, A[I]->AsObject()->Values[TEXT("value")]); }
            }
            else
            {
                T.TestEqual(Label + TEXT(" array size"), V.GetArray().Num(), A.Num());
                for (int32 I=0; I<FMath::Min(A.Num(), V.GetArray().Num()); ++I) CheckValue(T, Label, V.GetArray()[I], A[I]);
            }
        }
        else if (Expected->Type == EJson::Number) T.TestEqual(Label, double(V.GetFloat()), Expected->AsNumber());
        else if (Expected->Type == EJson::Boolean) T.TestEqual(Label, V.GetBool(), Expected->AsBool());
        else T.TestEqual(Label, V.GetString(), Expected->AsString());
    }
    bool RunRollbackCase(FAutomationTestBase& T, const FString& Name)
    {
        StoryFlowTestWorld::FScopedWorld W; if (!T.TestTrue(TEXT("world"), W.Init())) return false;
        const FString Content = TEXT("/Game/RollbackTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        UStoryFlowProjectAsset* P = UStoryFlowImporter::ImportProject(FPaths::Combine(Fixtures(), Name), Content);
        if (!T.TestNotNull(TEXT("imported shared fixture"), P)) return false;
        W.Subsystem->SetProject(P); W.Component->bTraceEnabled = false; W.Component->StartDialogueWithScript(P->StartupScript);
        auto Trace = Read(FPaths::Combine(Fixtures(), TEXT("expected-traces.json")))->GetArrayField(Name);
        TMap<FString, FStoryFlowVariant> Remembered;
        for (const auto& Action : Trace)
        {
            const auto A = Action->AsObject(); const FString Op = A->GetStringField(TEXT("op"));
            if (Op == TEXT("choose")) W.Component->SelectOption(A->GetStringField(TEXT("id")));
            else if (Op == TEXT("advance")) W.Component->AdvanceDialogue();
            else if (Op == TEXT("back")) Back(W.Component);
            else if (Op == TEXT("block"))
            { FString Reason; if (UFunction* F = W.Component->FindFunction(TEXT("BlockRollback"))) W.Component->ProcessEvent(F, &Reason); }
            else
            {
                auto S = A->GetObjectField(TEXT("state")); auto& C = FStoryFlowRollbackTestAccess::Context(W.Component);
                FString Node; if (S->TryGetStringField(TEXT("dialogue"), Node)) T.TestEqual(Name + TEXT(" dialogue"), W.Component->GetCurrentDialogue().NodeId, Node);
                bool Available; if (S->TryGetBoolField(TEXT("canGoBack"), Available)) T.TestEqual(Name + TEXT(" canGoBack"), CanBack(W.Component), Available);
                double Depth; if (S->TryGetNumberField(TEXT("callDepth"), Depth)) T.TestEqual(Name + TEXT(" callDepth"), C.CallStack.Num(), int32(Depth));
                const TSharedPtr<FJsonObject>* Globals;
                if (S->TryGetObjectField(TEXT("globals"), Globals)) for (const auto& Pair : (*Globals)->Values)
                { const FString Key(Pair.Key); auto* V = C.FindVariableByName(Key, true); if (T.TestNotNull(Key, V)) CheckValue(T, Key, V->Value, Pair.Value); }
                const TArray<TSharedPtr<FJsonValue>>* Values;
                if (S->TryGetArrayField(TEXT("loopCursors"), Values))
                {
                    TArray<int32> Cursors;
                    for (const auto& Activation : C.CallerActivations) for (const auto& L : Activation.Loops) Cursors.Add(Activation.Nodes[L.NodeId].LoopIndex);
                    for (const auto& L : C.LoopStack) Cursors.Add(C.NodeRuntimeStates[L.NodeId].LoopIndex);
                    T.TestEqual(TEXT("parked cursor count"), Cursors.Num(), Values->Num());
                    for (int32 I=0; I<FMath::Min(Cursors.Num(), Values->Num()); ++I) T.TestEqual(TEXT("parked loop cursor"), Cursors[I], int32((*Values)[I]->AsNumber()));
                }
                if (S->TryGetArrayField(TEXT("options"), Values))
                {
                    auto Options = W.Component->GetCurrentDialogue().Options; T.TestEqual(TEXT("option count"), Options.Num(), Values->Num());
                    for (int32 I=0; I<FMath::Min(Options.Num(), Values->Num()); ++I) T.TestEqual(TEXT("option identity"), Options[I].Id, (*Values)[I]->AsString());
                }
                if (S->TryGetArrayField(TEXT("rememberGlobals"), Values)) for (auto V : *Values) Remembered.Add(V->AsString(), C.FindVariableByName(V->AsString(), true)->Value);
                if (S->TryGetArrayField(TEXT("sameGlobals"), Values)) for (auto V : *Values) T.TestEqual(TEXT("repeat random"), C.FindVariableByName(V->AsString(), true)->Value.ToString(), Remembered[V->AsString()].ToString());
            }
        }
        W.Component->StopDialogue(); W.Subsystem->SetProject(nullptr); UEditorAssetLibrary::DeleteDirectory(Content);
        return true;
    }
}
#define ROLLBACK_CASE(Name, Fixture) \
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollback##Name, "StoryFlow.Rollback." #Name, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter) \
bool FStoryFlowRollback##Name::RunTest(const FString&) { return StoryFlowRollbackTests::RunRollbackCase(*this, TEXT(Fixture)); }
ROLLBACK_CASE(Purchase, "purchase")
ROLLBACK_CASE(NestedLoops, "nested-loops")
ROLLBACK_CASE(SameNode, "same-node")
ROLLBACK_CASE(Random, "random")
ROLLBACK_CASE(Barrier, "barrier")

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackSettingsTest, "StoryFlow.Rollback.Settings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackSettingsTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FString Text; FFileHelper::LoadFileToString(Text, *FPaths::Combine(Fixtures(), TEXT("settings.json")));
    TArray<TSharedPtr<FJsonValue>> Vectors; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Vectors);
    const FString Content = TEXT("/Game/RollbackSettingsTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    for (auto Vector : Vectors)
    {
        auto V = Vector->AsObject(); auto Metadata = MakeShared<FJsonObject>();
        if (auto Input = V->TryGetField(TEXT("input"))) Metadata->SetField(TEXT("dialogueRollback"), Input);
        auto Document = MakeShared<FJsonObject>(); Document->SetObjectField(TEXT("metadata"), Metadata);
        auto P = UStoryFlowImporter::ImportProjectFromJson(Document, Fixtures(), Content);
        auto E = V->GetObjectField(TEXT("expected"));
        if (!TestNotNull(TEXT("vector imported"), P)) continue;
        TestEqual(V->GetStringField(TEXT("name")) + TEXT(" enabled"), P->Metadata.DialogueRollback.bEnabled, E->GetBoolField(TEXT("enabled")));
        TestEqual(TEXT("normalized limit"), P->Metadata.DialogueRollback.HistoryLimit, int32(E->GetNumberField(TEXT("historyLimit"))));
        TArray<uint8> Bytes; FObjectWriter(P, Bytes, false, false, false);
        auto Reloaded = NewObject<UStoryFlowProjectAsset>(); FObjectReader(Reloaded, Bytes);
        TestEqual(TEXT("serialized enabled"), Reloaded->Metadata.DialogueRollback.bEnabled, E->GetBoolField(TEXT("enabled")));
        TestEqual(TEXT("serialized limit"), Reloaded->Metadata.DialogueRollback.HistoryLimit, int32(E->GetNumberField(TEXT("historyLimit"))));
    }
    UEditorAssetLibrary::DeleteDirectory(Content); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackDisabledTest, "StoryFlow.Rollback.DisabledAndLimits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackDisabledTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init(TEXT("same-node"), false)) return false; F.Start();
    F.W.Component->SelectOption(TEXT("again"));
    TestFalse(TEXT("disabled has no controller or captures"), FStoryFlowRollbackTestAccess::Controller(F.W.Component).IsValid());
    TestFalse(TEXT("disabled has no private RNG"),FStoryFlowRollbackTestAccess::Context(F.W.Component).RollbackRandomState.IsSet());
    TestEqual(TEXT("disabled reason"), F.W.Component->GoBack().Reason, FString(TEXT("disabled")));
    F.W.Component->StopDialogue(); F.P->Metadata.DialogueRollback.bEnabled=true; F.P->Metadata.DialogueRollback.HistoryLimit=1; F.Start();
    auto Controller=FStoryFlowRollbackTestAccess::Controller(F.W.Component);
    auto* O=Observe(F.W.Component); bool LastAvailable=false;
    O->OnAvailable=[&]() { LastAvailable=F.W.Component->CanGoBack(); };
    O->OnUpdate=[&]() { LastAvailable=F.W.Component->CanGoBack(); };
    for (int32 I=0;I<8;++I) F.W.Component->SelectOption(TEXT("again"));
    TestEqual(TEXT("limit retains current plus one prior"), Controller->Count(), 2);
    TestTrue(TEXT("capped redraw receives idle availability"), LastAvailable);
    TestTrue(TEXT("one back"), F.W.Component->GoBack().bOk); TestFalse(TEXT("no older entry"), F.W.Component->CanGoBack());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackOwnershipTest, "StoryFlow.Rollback.OwnerMutationAndContent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackOwnershipTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false; F.Start(); F.W.Component->SelectOption(TEXT("buy"));
    auto* O=Observe(F.W.Component); bool Restart=true;
    O->OnAvailable=[&]() { if(Restart) { Restart=false; F.Start(); } };
    F.W.Component->SetIntVariable(TEXT("Gold"), 777, true);
    F.W.Component->SelectOption(TEXT("leave"));
    TestFalse(TEXT("callback-created owner cannot reach before host write"), F.W.Component->CanGoBack());
    TestEqual(TEXT("host value retained"), FStoryFlowRollbackTestAccess::Context(F.W.Component).FindVariableByName(TEXT("Gold"),true)->Value.GetInt(), 777);
    O->OnAvailable=nullptr; F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("buy"));
    auto* Other=NewObject<UStoryFlowComponent>(F.W.World->SpawnActor<AActor>()); Other->RegisterComponent(); Other->bTraceEnabled=false; Other->StartDialogueWithScript(F.P->StartupScript);
    TestFalse(TEXT("second owner invalidates first"), F.W.Component->CanGoBack()); TestFalse(TEXT("second cannot rewind shared store"), Other->CanGoBack());
    Other->StopDialogue(); F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("buy")); Restart=true;
    O->OnAvailable=[&]() { if(Restart) { Restart=false; F.Start(); } };
    { FStoryFlowContentUpdateScope Updating(F.P); F.W.Component->SelectOption(TEXT("leave")); TestEqual(TEXT("new owner inherits content guard"), F.W.Component->GetRollbackAvailability().Reason, FString(TEXT("contentChanged"))); }
    TestFalse(TEXT("content invalidation stays permanent"), F.W.Component->CanGoBack());
    O->OnAvailable=nullptr; F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("buy")); TestTrue(TEXT("fresh session can capture"), F.W.Component->CanGoBack());
    F.W.Subsystem->ResetGlobalVariables(); TestFalse(TEXT("reset is external boundary"),F.W.Component->CanGoBack());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackFaultTest, "StoryFlow.Rollback.TransactionFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackFaultTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false; F.Start(); F.W.Component->SelectOption(TEXT("buy"));
    auto Owner=FStoryFlowRollbackTestAccess::Controller(F.W.Component); auto* O=Observe(F.W.Component);
    FStoryFlowRollbackController::bFailPrepare=true; TestEqual(TEXT("prepare fails"),F.W.Component->GoBack().Reason,FString(TEXT("restoreFailed")));
    TestEqual(TEXT("prepare leaves live entry"),F.W.Component->GetCurrentDialogue().NodeId,FString(TEXT("B"))); TestEqual(TEXT("history untouched"),Owner->Count(),2);
    F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("buy")); Owner=FStoryFlowRollbackTestAccess::Controller(F.W.Component);
    auto* L=NewObject<UStoryFlowLipsyncComponent>(F.W.Component->GetOwner()); L->Source=F.W.Component; L->RegisterComponent();
    auto* Wave=NewObject<USoundWaveProcedural>(F.W.Component); Wave->NumChannels=1; Wave->SetSampleRate(44100); Wave->Duration=10.0f;
    TArray<uint8> PCM; PCM.SetNumZeroed(44100*2*10); Wave->QueueAudio(PCM.GetData(),PCM.Num());
    auto* Audio=NewObject<UAudioComponent>(F.W.Component->GetOwner()); Audio->bAutoDestroy=false; Audio->SetSound(Wave); Audio->RegisterComponent(); Audio->Play();
    TestTrue(TEXT("real native audio started"),Audio->IsPlaying()); FStoryFlowRollbackTestAccess::Context(F.W.Component).CurrentDialogueState.Audio=Wave;
    FStoryFlowRollbackTestAccess::Audio(F.W.Component,Audio); FStoryFlowRollbackTestAccess::Bind(L);
    F.W.Component->PauseDialogue(); FStoryFlowRollbackTestAccess::AudioAdvance(F.W.Component); const auto Generation=FStoryFlowRollbackTestAccess::Generation(F.W.Component);
    FStoryFlowRollbackController::bFailCommit=true; TestEqual(TEXT("commit fails"),F.W.Component->GoBack().Reason,FString(TEXT("restoreFailed")));
    TestEqual(TEXT("failed commit restores live values"),FStoryFlowRollbackTestAccess::Context(F.W.Component).FindVariableByName(TEXT("Gold"),true)->Value.GetInt(),90);
    TestTrue(TEXT("pause recovered"),F.W.Component->IsPaused()); TestTrue(TEXT("auto/skip recovered"),FStoryFlowRollbackTestAccess::WaitingAudio(F.W.Component));
    TestTrue(TEXT("native voice physical identity recovered"),F.W.Component->GetCurrentDialogueAudio()==Audio);
    TestTrue(TEXT("real native audio resumed after failure"),Audio->IsPlaying()); TestTrue(TEXT("automatic lipsync still owns same voice"),FStoryFlowRollbackTestAccess::TrackedAudio(L)==Audio && L->IsLipsyncActive());
    TestTrue(TEXT("callback generation monotonic"),FStoryFlowRollbackTestAccess::Generation(F.W.Component)>Generation);
    TestEqual(TEXT("failed commit history unchanged"),Owner->Count(),2); TestEqual(TEXT("failed restore never presents"),O->Restored,0);
    F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("buy"));
    FStoryFlowRollbackController::bFailCommit=true; FStoryFlowRollbackController::bFailRecovery=true;
    TestEqual(TEXT("unrecoverable commit reason"),F.W.Component->GoBack().Reason,FString(TEXT("restoreFailed"))); TestFalse(TEXT("unrecoverable stops session"),F.W.Component->IsDialogueActive());
    F.Start(); F.W.Component->SelectOption(TEXT("buy")); auto* Voice=NewObject<UAudioComponent>(F.W.Component->GetOwner()); Voice->RegisterComponent(); Voice->SetSound(Wave); Voice->Play(); FStoryFlowRollbackTestAccess::Audio(F.W.Component,Voice);
    FStoryFlowRollbackTestAccess::AudioAdvance(F.W.Component); F.W.Component->PauseDialogue(); const auto Stale=Voice->OnAudioFinishedNative;
    TestTrue(TEXT("successful Back after fresh session"),F.W.Component->GoBack().bOk); TestNull(TEXT("successful Back stops transient voice"),F.W.Component->GetCurrentDialogueAudio()); TestFalse(TEXT("successful Back cancels auto/skip"),FStoryFlowRollbackTestAccess::WaitingAudio(F.W.Component)); TestFalse(TEXT("successful Back unpauses"),F.W.Component->IsPaused());
    Stale.Broadcast(Voice); TestEqual(TEXT("stale audio cannot advance restored entry"),F.W.Component->GetCurrentDialogue().NodeId,FString(TEXT("A")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackSnapshotTest, "StoryFlow.Rollback.DetachedAliasesBudgetAndContent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackSnapshotTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false; F.Start(); auto& C=FStoryFlowRollbackTestAccess::Context(F.W.Component);
    auto* Counts=C.FindVariableByName(TEXT("Counts"),true); FStoryFlowVariable Alias=*Counts; Alias.Id=TEXT("alias"); Alias.Name=TEXT("Alias"); C.ExternalGlobalVariables->Add(Alias.Id,Alias);
    FStoryFlowExecutionSnapshot S; FString Reason; TestTrue(TEXT("capture aliases"),FStoryFlowExecutionSnapshot::Capture(C,1,S,Reason));
    Counts->Value.GetMapMutable()[0].Value.SetInt(9); FStoryFlowExecutionSnapshot Prepared;
    TestTrue(TEXT("prepare"),S.Prepare(C,Prepared,Reason)); auto* Root=C.ExternalGlobalVariables; Prepared.Apply(C);
    TestTrue(TEXT("global owner stable"),C.ExternalGlobalVariables==Root);
    TestEqual(TEXT("detached map checkpoint"),C.FindVariableByName(TEXT("Counts"),true)->Value.GetMap()[0].Value.GetInt(),1);
    C.ExternalGlobalVariables->Find(TEXT("alias"))->Value.GetMapMutable()[0].Value.SetInt(4);
    TestEqual(TEXT("aliases restored within state"),C.FindVariableByName(TEXT("Counts"),true)->Value.GetMap()[0].Value.GetInt(),4);
    FStoryFlowExecutionSnapshot Missing; auto SavedNode=F.P->GetScriptByPath(F.P->StartupScript)->Nodes.FindChecked(TEXT("A")); F.P->GetScriptByPath(F.P->StartupScript)->Nodes.Remove(TEXT("A"));
    TestFalse(TEXT("missing content rejected before mutation"),S.Prepare(C,Missing,Reason)); TestEqual(TEXT("content reason"),Reason,FString(TEXT("contentChanged"))); F.P->GetScriptByPath(F.P->StartupScript)->Nodes.Add(TEXT("A"),SavedNode);
    C.CurrentDialogueState.Text=FString::ChrN(17*1024*1024,TCHAR('x')); Reason.Reset();
    TestFalse(TEXT("32MiB budget aborts capture"),FStoryFlowExecutionSnapshot::Capture(C,2,Missing,Reason)); TestEqual(TEXT("budget reason"),Reason,FString(TEXT("budget")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackWidgetTest, "StoryFlow.Rollback.WidgetDedicatedRestore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackWidgetTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false; F.Start();
    auto* Widget=NewObject<UStoryFlowRollbackWidgetSpy>(F.W.World); Widget->InitializeWithComponent(F.W.Component);
    auto* O=Observe(F.W.Component); F.W.Component->SelectOption(TEXT("buy")); const int32 Updates=O->Updated;
    Widget->StartAutoTimer(); TestTrue(TEXT("fixture native custom timer running"),F.W.World->GetTimerManager().IsTimerActive(Widget->AutoTimer));
    TestTrue(TEXT("Back succeeds"),F.W.Component->GoBack().bOk);
    TestEqual(TEXT("dedicated widget restored event"),Widget->Restored,1); TestEqual(TEXT("visible previous entry"),Widget->Visible.NodeId,FString(TEXT("A")));
    TestEqual(TEXT("fully revealed text"),Widget->Visible.Text,F.W.Component->GetCurrentDialogue().Text); TestEqual(TEXT("previous options"),Widget->Visible.Options.Num(),2);
    TestFalse(TEXT("restored handler cancels real host auto timer"),F.W.World->GetTimerManager().IsTimerActive(Widget->AutoTimer));
    TestEqual(TEXT("ordinary updated not replayed"),O->Updated,Updates);
    F.W.Component->SelectOption(TEXT("leave")); TestEqual(TEXT("next normal update once"),O->Updated,Updates+1);
    Widget->DestructForTest(); const int32 Before=Widget->Restored; F.W.Component->GoBack(); TestEqual(TEXT("destroy unsubscribes"),Widget->Restored,Before);
    auto* Replacement=NewObject<UStoryFlowRollbackWidgetSpy>(F.W.World); Replacement->InitializeWithComponent(F.W.Component); TestNotNull(TEXT("restore is Blueprint override event"),Replacement->FindFunction(TEXT("OnDialogueRestored")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackLipsyncTest, "StoryFlow.Rollback.LipsyncRestoredFreshness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackLipsyncTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init(TEXT("same-node"))) return false; F.Start();
    auto* O=Observe(F.W.Component); auto* L=NewObject<UStoryFlowLipsyncComponent>(F.W.Component->GetOwner()); L->Source=F.W.Component; L->RegisterComponent(); FStoryFlowRollbackTestAccess::Bind(L);
    F.W.Component->SelectOption(TEXT("again")); FStoryFlowRollbackTestAccess::Open(L);
    TestTrue(TEXT("automatic line active before Back"),L->IsLipsyncActive()); TestTrue(TEXT("fixture opened driver"),FStoryFlowRollbackTestAccess::Pose(L)>0);
    F.W.Component->GoBack(); TestFalse(TEXT("restored line releases automatic owner"),L->IsLipsyncActive()); TestEqual(TEXT("restored driver closes synchronously"),FStoryFlowRollbackTestAccess::Pose(L),0.0f);
    F.W.Component->ResumeDialogue(); TestFalse(TEXT("redraw of restored line stays closed"),L->IsLipsyncActive());
    auto* Late=NewObject<UStoryFlowLipsyncComponent>(F.W.Component->GetOwner()); Late->Source=F.W.Component; Late->RegisterComponent(); FStoryFlowRollbackTestAccess::Bind(Late); TestFalse(TEXT("late binding restored stays closed"),Late->IsLipsyncActive());
    F.W.Component->SelectOption(TEXT("again")); TestTrue(TEXT("fresh same-node entry resumes"),L->IsLipsyncActive());
    L->StartLipsync(); F.W.Component->GoBack(); TestTrue(TEXT("manual mode survives restore"),L->IsLipsyncActive()); L->StopLipsync();
    for(int32 Kind=0;Kind<3;++Kind)
    {
        F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("again")); bool Once=true;
        auto Advance=[&]() { if(Once) { Once=false; if(Kind==2) F.Start(); F.W.Component->SelectOption(TEXT("again")); } };
        if(Kind==1) O->OnRestore=Advance; else O->OnAvailable=Advance;
        F.W.Component->GoBack(); O->OnRestore=nullptr; O->OnAvailable=nullptr;
        TestTrue(TEXT("stale restore cannot stop listener-created fresh entry"),L->IsLipsyncActive());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackLocalizedOwnedTest, "StoryFlow.Rollback.LocalizedOwnedCharacterAndOverlay", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackLocalizedOwnedTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false;
    const FString HeroId=TEXT("da_00000000000000000000000000000001"), FriendId=TEXT("da_00000000000000000000000000000002"), LootId=TEXT("da_00000000000000000000000000000003");
    auto* Hero=NewObject<UStoryFlowCharacterAsset>(F.P); Hero->Name=TEXT("hero.key");
    auto* Friend=NewObject<UStoryFlowCharacterAsset>(F.P); Friend->Name=TEXT("friend.key");
    FStoryFlowVariable Score; Score.Id=TEXT("score"); Score.Name=TEXT("Score"); Score.Type=EStoryFlowVariableType::Integer; Score.Value.SetInt(1); Friend->Variables.Add(Score.Id,Score);
    F.P->Characters.Add(TEXT("hero"),Hero); F.P->Characters.Add(TEXT("friend"),Friend); F.P->CharacterIdToPath.Add(HeroId,TEXT("hero")); F.P->CharacterIdToPath.Add(FriendId,TEXT("friend"));
    FStoryFlowVariable FriendRef; FriendRef.Id=TEXT("friendref"); FriendRef.Name=TEXT("Friend"); FriendRef.Type=EStoryFlowVariableType::Character; FriendRef.Value.SetString(FriendId); F.P->GlobalVariables.Add(FriendRef.Id,FriendRef);
    auto* Loot=NewObject<UStoryFlowDataAssetAsset>(F.P); Loot->AssetId=LootId; Loot->Name=TEXT("Loot"); auto Price=Score; Price.Id=TEXT("price"); Price.Name=TEXT("Price"); Price.Value.SetInt(30); Loot->Variables.Add(Price); F.P->DataAssets.Add(LootId,Loot);
    auto LootRef=FriendRef; LootRef.Id=TEXT("lootref"); LootRef.Name=TEXT("Loot"); LootRef.Type=EStoryFlowVariableType::DataAsset; LootRef.Value.SetDataAsset(LootId); F.P->GlobalVariables.Add(LootRef.Id,LootRef);
    auto* Script=F.P->GetScriptByPath(F.P->StartupScript); auto& A=Script->Nodes.FindChecked(TEXT("A")); A.Data.CharacterRefId=HeroId;
    const FString Template=TEXT("{Character.Name}/{Friend.Name}/{Loot.Price}/{Friend.Score}"); A.Data.Text=Template; A.Data.Title=Template;
    for(auto& Choice:A.Data.Options) Choice.Text=Template;
    FStoryFlowTextBlock Block; Block.Id=TEXT("block"); Block.Text=Template; A.Data.TextBlocks.Add(Block);
    F.P->GlobalStrings.Add(TEXT("en.friend.key"),TEXT("PROJECT FRIEND")); Script->Strings.Add(TEXT("en.friend.key"),TEXT("SCRIPT SHADOW"));
    F.P->bHasLocalization=true; FStoryFlowLanguage Fr; Fr.Code=TEXT("fr"); Fr.Name=TEXT("French"); F.P->Languages.Add(Fr); F.P->LanguageStrings.FindOrAdd(TEXT("fr")).Entries.Add(TEXT("friend.key"),TEXT("AMI"));
    F.W.Subsystem->SetProject(F.P); auto& RuntimeHero=F.W.Subsystem->GetRuntimeCharacters().FindChecked(TEXT("hero")); RuntimeHero.Name=TEXT("LegacyHero"); RuntimeHero.bNameIsLiteral=true;
    FStoryFlowVariant Forty; Forty.SetInt(40); StoryFlowDataAssets::TrySet(F.W.Subsystem->GetDataAssetStore(),LootId,Price.Id,Forty);
    F.Start(); auto& C=FStoryFlowRollbackTestAccess::Context(F.W.Component); const FString Initial=TEXT("LegacyHero/PROJECT FRIEND/40/1");
    TestEqual(TEXT("ordinary project-only character resolution"),F.W.Component->GetCurrentDialogue().Text,Initial);
    TestEqual(TEXT("native character read uses same project scope"),C.GetCharacterVariableValue(TEXT("friend"),TEXT("Name")).GetString(),FString(TEXT("PROJECT FRIEND")));
    auto* G=C.ExternalGlobalVariables; auto* Characters=C.ExternalCharacters; auto* Overlay=C.DataAssetStore.Overlay;
    F.W.Component->SelectOption(TEXT("buy")); FStoryFlowVariant Future; Future.SetInt(99); C.SetCharacterVariable(TEXT("friend"),TEXT("score"),Future); StoryFlowDataAssets::TrySet(C.DataAssetStore,LootId,Price.Id,Future);
    F.W.Subsystem->SetLanguage(TEXT("fr")); TestTrue(TEXT("Back localized owned state"),F.W.Component->GoBack().bOk);
    const FString Restored=TEXT("LegacyHero/AMI/40/1"); const auto D=F.W.Component->GetCurrentDialogue();
    TestEqual(TEXT("body uses detached overlay and immutable ID bridge"),D.Text,Restored); TestEqual(TEXT("title localized"),D.Title,Restored);
    for(const auto& O:D.Options) TestEqual(TEXT("options localized"),O.Text,Restored);
    TestEqual(TEXT("text blocks retained"),D.TextBlocks.Num(),1); if(D.TextBlocks.Num()) TestEqual(TEXT("text block localized"),D.TextBlocks[0].Text,Restored);
    TestEqual(TEXT("non-speaker authored name key preserved"),C.ExternalCharacters->FindChecked(TEXT("friend")).Name,FString(TEXT("friend.key")));
    TestTrue(TEXT("literal name preserved"),C.ExternalCharacters->FindChecked(TEXT("hero")).bNameIsLiteral); TestTrue(TEXT("all store owners stable"),C.ExternalGlobalVariables==G&&C.ExternalCharacters==Characters&&C.DataAssetStore.Overlay==Overlay);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackRecursiveTest, "StoryFlow.Rollback.RecursiveActivationSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackRecursiveTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false; F.Start(); auto& C=FStoryFlowRollbackTestAccess::Context(F.W.Component);
    FStoryFlowVariable Marker; Marker.Id=TEXT("marker"); Marker.Name=TEXT("Marker"); Marker.Type=EStoryFlowVariableType::Integer;
    auto SetActivation=[&](int32 N) { Marker.Value.SetInt(N); C.LocalVariables.Add(Marker.Id,Marker); auto& NS=C.NodeRuntimeStates.FindOrAdd(TEXT("0")); NS.LoopIndex=N; NS.bHasMapExecutionOutput=true; NS.MapExecutionOutput=*C.FindVariableByName(TEXT("Counts"),true); NS.OutputValues.Add(TEXT("result"),Marker.Value); FStoryFlowLoopContext L; L.NodeId=TEXT("0"); C.LoopStack.Add(L); };
    SetActivation(1); TestTrue(TEXT("first recursive push"),C.PushScript(F.P->StartupScript,TEXT("A"))); SetActivation(2); TestTrue(TEXT("second recursive push"),C.PushScript(F.P->StartupScript,TEXT("A"))); SetActivation(3); C.CurrentNodeId=TEXT("A");
    FStoryFlowExecutionSnapshot S,Prepared; FString Reason; TestTrue(TEXT("recursive capture"),FStoryFlowExecutionSnapshot::Capture(C,1,S,Reason));
    C.CallerActivations[0].Nodes[TEXT("0")].LoopIndex=88; C.CallerActivations[1].Nodes[TEXT("0")].OutputValues[TEXT("result")].SetInt(99); C.LocalVariables[TEXT("marker")].Value.SetInt(999);
    TestTrue(TEXT("recursive prepare"),S.Prepare(C,Prepared,Reason)); Prepared.Apply(C);
    TestEqual(TEXT("callee locals"),C.LocalVariables[TEXT("marker")].Value.GetInt(),3); TestEqual(TEXT("callee cursor"),C.NodeRuntimeStates[TEXT("0")].LoopIndex,3);
    TestTrue(TEXT("pop inner"),C.PopScript()); TestEqual(TEXT("recursive caller locals"),C.LocalVariables[TEXT("marker")].Value.GetInt(),2); TestEqual(TEXT("caller cached return"),C.NodeRuntimeStates[TEXT("0")].OutputValues[TEXT("result")].GetInt(),2);
    TestEqual(TEXT("cached map execution output"),C.NodeRuntimeStates[TEXT("0")].MapExecutionOutput.Value.GetMap()[0].Value.GetInt(),1);
    TestTrue(TEXT("pop outer"),C.PopScript()); TestEqual(TEXT("outer same-asset cursor independent"),C.NodeRuntimeStates[TEXT("0")].LoopIndex,1); TestEqual(TEXT("outer locals"),C.LocalVariables[TEXT("marker")].Value.GetInt(),1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackMissingTest, "StoryFlow.Rollback.MissingRetainedContent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackMissingTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init()) return false; F.Start(); auto& C=FStoryFlowRollbackTestAccess::Context(F.W.Component); FStoryFlowExecutionSnapshot S,Prepared; FString Reason;
    TestTrue(TEXT("capture"),FStoryFlowExecutionSnapshot::Capture(C,1,S,Reason)); auto* Script=F.P->GetScriptByPath(F.P->StartupScript); const auto Choices=Script->Nodes[TEXT("A")].Data.Options;
    Script->Nodes[TEXT("A")].Data.Options.RemoveAt(0); TestFalse(TEXT("retained choice missing rejects prepare"),S.Prepare(C,Prepared,Reason)); Script->Nodes[TEXT("A")].Data.Options=Choices;
    S.Context.NodeRuntimeStates.Add(TEXT("deleted-state"),FNodeRuntimeState()); Reason.Reset(); TestFalse(TEXT("deleted cached-node identity rejects prepare"),S.Prepare(C,Prepared,Reason)); S.Context.NodeRuntimeStates.Remove(TEXT("deleted-state"));
    FStoryFlowLoopContext L; L.NodeId=TEXT("deleted-loop"); S.Context.LoopStack.Add(L); Reason.Reset(); TestFalse(TEXT("deleted parked-loop identity rejects prepare"),S.Prepare(C,Prepared,Reason));
    S.Context.LoopStack.Reset(); FStoryFlowFlowFrame Flow; Flow.FlowId=TEXT("deleted-flow"); S.Context.FlowCallStack.Add(Flow); Reason.Reset(); TestFalse(TEXT("deleted flow identity rejects prepare"),S.Prepare(C,Prepared,Reason));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackWidgetBindingTest, "StoryFlow.Rollback.WidgetBindingAndAuthorControl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackWidgetBindingTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init(TEXT("same-node"))) return false; F.Start(); F.W.Component->SelectOption(TEXT("again"));
    auto* O=Observe(F.W.Component); auto* Widget=NewObject<UStoryFlowRollbackWidgetSpy>(F.W.World); Widget->InstallBackButton(false); Widget->SetBackAllowed(false); Widget->InitializeWithComponent(F.W.Component);
    TestTrue(TEXT("replacement reads availability immediately"),Widget->LastAvailability.bCanGoBack); TestFalse(TEXT("authored-disabled button remains disabled"),Widget->BackEnabled());
    Widget->SetBackAllowed(true); TestTrue(TEXT("author can explicitly permit Back"),Widget->BackEnabled()); Widget->SetBackAllowed(false); TestFalse(TEXT("availability cannot override author allow state"),Widget->BackEnabled());
    const uint64 Before=F.W.Component->GetDialogueEntrySerial(); Widget->ClickBack(); TestEqual(TEXT("disabled click cannot restore"),F.W.Component->GetDialogueEntrySerial(),Before); Widget->SetBackAllowed(true);
    auto* Second=NewObject<UStoryFlowComponent>(F.W.World->SpawnActor<AActor>()); Second->RegisterComponent(); Second->bTraceEnabled=false;
    auto* Replacement=DuplicateObject<UStoryFlowScriptAsset>(F.P->GetScriptByPath(F.P->StartupScript),F.P); Replacement->ScriptPath=TEXT("replacement"); Replacement->Nodes[TEXT("A")].Data.Text=TEXT("REPLACEMENT"); F.P->Scripts.Add(TEXT("replacement"),Replacement);
    O->OnRestore=[&]() { Widget->InitializeWithComponent(Second); Second->StartDialogueWithScript(TEXT("replacement")); };
    TestTrue(TEXT("restore rebind trigger"),Widget->GoBack().bOk); TestTrue(TEXT("bound to second"),Widget->GetStoryFlowComponent()==Second); TestEqual(TEXT("old notification cannot redraw replacement"),Widget->Visible.Text,FString(TEXT("REPLACEMENT")));
    O->OnRestore=nullptr; Second->StopDialogue(); Widget->DetachFromComponent(); TestEqual(TEXT("detached helper reason"),Widget->GoBack().Reason,FString(TEXT("disabled")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackPersistentAvailabilityTest, "StoryFlow.Rollback.PersistentAvailabilityAfterLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackPersistentAvailabilityTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"))) return false; F.Start(); F.W.Component->SelectOption(TEXT("again"));
    auto* O=Observe(F.W.Component); auto* Widget=NewObject<UStoryFlowRollbackWidgetSpy>(F.W.World); Widget->InstallBackButton(true); Widget->InitializeWithComponent(F.W.Component);
    TestTrue(TEXT("persistent button initially enabled"),Widget->BackEnabled()); const int32 BeforeStop=O->Availability;
    F.W.Component->StopDialogue(); TestFalse(TEXT("stopped getter unavailable"),F.W.Component->CanGoBack()); TestFalse(TEXT("persistent button disabled after Stop"),Widget->BackEnabled()); TestFalse(TEXT("cached availability unavailable after Stop"),Widget->LastAvailability.bCanGoBack); TestTrue(TEXT("subscriber notified on Stop"),O->Availability>BeforeStop);
    F.Start(); F.W.Component->SelectOption(TEXT("again")); TestTrue(TEXT("persistent button available in new session"),Widget->BackEnabled()); const int32 BeforeEnd=O->Availability;
    FStoryFlowRollbackTestAccess::RootEnd(F.W.Component); TestFalse(TEXT("persistent button disabled after root End"),Widget->BackEnabled()); TestFalse(TEXT("root End cached availability unavailable"),Widget->LastAvailability.bCanGoBack); TestTrue(TEXT("subscriber notified on root End"),O->Availability>BeforeEnd);
    F.Start(); F.W.Component->SelectOption(TEXT("again")); F.P->Metadata.DialogueRollback.bEnabled=false; const int32 BeforeDisabled=O->Availability; F.Start();
    TestFalse(TEXT("disabled replacement clears persistent control"),Widget->BackEnabled()); TestFalse(TEXT("disabled replacement cached unavailable"),Widget->LastAvailability.bCanGoBack); TestTrue(TEXT("subscriber notified on disabled replacement"),O->Availability>BeforeDisabled);
    F.W.Component->StopDialogue(); F.P->Metadata.DialogueRollback.bEnabled=true; F.Start(); F.W.Component->SelectOption(TEXT("again")); bool Once=true;
    O->OnAvailable=[&]() { if(Once && !F.W.Component->IsDialogueActive()) { Once=false; F.Start(); F.W.Component->SelectOption(TEXT("again")); } };
    F.W.Component->StopDialogue(); O->OnAvailable=nullptr; TestTrue(TEXT("availability callback replacement active"),F.W.Component->IsDialogueActive()); TestTrue(TEXT("replacement availability preserved"),Widget->LastAvailability.bCanGoBack && Widget->BackEnabled());
    F.W.Component->StopDialogue(); TestFalse(TEXT("final Stop count balanced"),F.W.Subsystem->IsDialogueActive()); Widget->DetachFromComponent(); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackRestoredReplacementTest, "StoryFlow.Rollback.RestoredReplacementWithSameNode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackRestoredReplacementTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"))) return false; F.Start(); F.W.Component->SelectOption(TEXT("again")); auto* O=Observe(F.W.Component);
    auto* Replacement=DuplicateObject<UStoryFlowScriptAsset>(F.P->GetScriptByPath(F.P->StartupScript),F.P); Replacement->ScriptPath=TEXT("replacement"); Replacement->Nodes[TEXT("A")].Data.Text=TEXT("REPLACEMENT"); Replacement->Nodes[TEXT("A")].Data.Options[0].Text=TEXT("REPLACEMENT OPTION"); F.P->Scripts.Add(TEXT("replacement"),Replacement);
    bool Once=true, NestedOk=false; O->OnRestore=[&]() { if(Once) { Once=false; F.W.Component->StartDialogueWithScript(TEXT("replacement")); F.W.Component->SelectOption(TEXT("again")); NestedOk=F.W.Component->GoBack().bOk; } };
    auto* Widget=NewObject<UStoryFlowRollbackWidgetSpy>(F.W.World); Widget->InitializeWithComponent(F.W.Component);
    TestTrue(TEXT("outer Back succeeds"),F.W.Component->GoBack().bOk); TestTrue(TEXT("nested replacement Back succeeds"),NestedOk); O->OnRestore=nullptr;
    const auto Live=F.W.Component->GetCurrentDialogue(); TestEqual(TEXT("replacement uses same node"),Live.NodeId,FString(TEXT("A"))); TestEqual(TEXT("live replacement text"),Live.Text,FString(TEXT("REPLACEMENT")));
    TestEqual(TEXT("stale restored payload cannot overwrite replacement"),Widget->Visible.Text,Live.Text); TestEqual(TEXT("replacement options remain visible"),Widget->Visible.Options[0].Text,Live.Options[0].Text); TestEqual(TEXT("only current restoration reaches widget"),Widget->Restored,1);
    Widget->DetachFromComponent(); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackLifecycleTest, "StoryFlow.Rollback.LifecycleReentrantStartAndFailedLoad", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackLifecycleTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if (!F.Init(TEXT("same-node"))) return false; F.Start(); F.W.Component->SelectOption(TEXT("again")); auto* O=Observe(F.W.Component);
    AddExpectedError(TEXT("Cannot load while a dialogue is active"),EAutomationExpectedErrorFlags::Contains,1);
    TestFalse(TEXT("active Load rejection preserved"),F.W.Subsystem->LoadFromSlot(TEXT("nonexistent-rollback-slot"))); TestTrue(TEXT("failed Load keeps history"),F.W.Component->CanGoBack());
    F.W.Component->ResetVariables(); TestFalse(TEXT("local reset invalidates history"),F.W.Component->CanGoBack());
    bool Once=true; O->OnEnd=[&]() { if(Once) { Once=false; F.Start(); F.W.Component->SelectOption(TEXT("again")); } };
    F.W.Component->StopDialogue(); O->OnEnd=nullptr; TestTrue(TEXT("replacement session remains active"),F.W.Component->IsDialogueActive()); TestTrue(TEXT("replacement owns rollback"),F.W.Component->CanGoBack());
    F.W.Component->StopDialogue(); TestFalse(TEXT("last Stop releases subsystem count"),F.W.Subsystem->IsDialogueActive()); TestFalse(TEXT("component count reset"),FStoryFlowRollbackTestAccess::Counted(F.W.Component));
    // A Start callback starts and advances its replacement. The outgoing Start must not process it again.
    auto* R=NewObject<UStoryFlowRollbackObserver>(F.W.Component); bool StartOnce=true;
    R->OnEnd=[&]() { if(StartOnce) { StartOnce=false; F.Start(); F.W.Component->SelectOption(TEXT("again")); } };
    F.W.Component->OnDialogueStarted.AddDynamic(R,&UStoryFlowRollbackObserver::End);
    F.Start(); TestEqual(TEXT("outer Start does not redraw replacement baseline"),FStoryFlowRollbackTestAccess::Controller(F.W.Component)->Count(),2);
    F.W.Component->OnDialogueStarted.RemoveDynamic(R,&UStoryFlowRollbackObserver::End); F.W.Component->StopDialogue(); TestFalse(TEXT("reentrant Start final count balanced"),F.W.Subsystem->IsDialogueActive());
    F.Start(); F.W.Component->SelectOption(TEXT("again")); const FString Save=StoryFlowSaveHelpers::SerializeSaveData(F.W.Subsystem->GetGlobalVariables(),F.W.Subsystem->GetRuntimeCharacters(),F.W.Subsystem->GetUsedOnceOnlyOptions(),*F.W.Subsystem->GetDataAssetStore().Seed,*F.W.Subsystem->GetDataAssetStore().Overlay);
    TestFalse(TEXT("ordinary Save has no rollback history"),Save.Contains(TEXT("rollback"),ESearchCase::IgnoreCase)); TestTrue(TEXT("saving keeps history"),F.W.Component->CanGoBack());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackRecursiveTraversalTest, "StoryFlow.Rollback.RecursiveRunScriptTraversal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackRecursiveTraversalTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init()) return false; auto* Script=NewObject<UStoryFlowScriptAsset>(F.P); Script->ScriptPath=TEXT("recursive");
    FStoryFlowNode Start; Start.Id=TEXT("0"); Start.Type=EStoryFlowNodeType::Start; Script->Nodes.Add(Start.Id,Start);
    FStoryFlowNode Line; Line.Id=TEXT("line"); Line.Type=EStoryFlowNodeType::Dialogue; Line.Data.Text=TEXT("recursive line"); Script->Nodes.Add(Line.Id,Line);
    FStoryFlowNode Run; Run.Id=TEXT("run"); Run.Type=EStoryFlowNodeType::RunScript; Run.Data.Script=Script->ScriptPath; Script->Nodes.Add(Run.Id,Run);
    for(const auto& Pair : {TPair<FString,FString>(TEXT("0"),TEXT("line")),TPair<FString,FString>(TEXT("line"),TEXT("run"))})
    { FStoryFlowConnection E; E.Id=Pair.Key+Pair.Value; E.Source=Pair.Key; E.Target=Pair.Value; E.SourceHandle=StoryFlowHandles::Source(Pair.Key); E.TargetHandle=StoryFlowHandles::Target(Pair.Value); Script->Connections.Add(E); }
    Script->BuildConnectionIndices(); F.P->Scripts.Add(Script->ScriptPath,Script); F.W.Component->StartDialogueWithScript(Script->ScriptPath);
    F.W.Component->AdvanceDialogue(); F.W.Component->AdvanceDialogue(); auto& C=FStoryFlowRollbackTestAccess::Context(F.W.Component);
    TestEqual(TEXT("native recursive script depth"),C.CallStack.Num(),2); TestTrue(TEXT("recursive Back"),F.W.Component->GoBack().bOk); TestEqual(TEXT("restored invocation depth"),C.CallStack.Num(),1);
    F.W.Component->AdvanceDialogue(); TestEqual(TEXT("repeated recursive traversal depth"),C.CallStack.Num(),2); TestEqual(TEXT("same script independently activated"),C.CallerActivations.Num(),2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackRngTest, "StoryFlow.Rollback.PrivateRngContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackRngTest::RunTest(const FString&)
{
    FStoryFlowExecutionContext C; C.RollbackRandomState=uint32(1);
    TestEqual(TEXT("raw xorshift draw1"),C.NextRollbackRandom(),uint32(270369)); TestEqual(TEXT("draw2"),C.NextRollbackRandom(),uint32(67634689)); TestEqual(TEXT("draw3"),C.NextRollbackRandom(),uint32(2647435461u));
    C.RollbackRandomState=uint32(0); TestEqual(TEXT("zero seed normalized"),C.NextRollbackRandom(),uint32(270369));
    FMath::RandInit(123); const int32 Expected=FMath::Rand(); FMath::RandInit(123);
    C.RandomInt(MIN_int32,MAX_int32); C.RandomFloat(-1,1); TestEqual(TEXT("story RNG leaves global stream alone"),FMath::Rand(),Expected);
    TestEqual(TEXT("equal integer endpoints"),C.RandomInt(42,42),42); TestEqual(TEXT("equal float endpoints"),C.RandomFloat(1.25f,1.25f),1.25f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackLoopMediaTest, "StoryFlow.Rollback.PersistentLoopDescriptor", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackLoopMediaTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"))) return false; F.Start();
    auto* Wave=NewObject<USoundWaveProcedural>(F.W.Component); Wave->NumChannels=1; Wave->SetSampleRate(44100); Wave->Duration=10.0f;
    TArray<uint8> PCM; PCM.SetNumZeroed(44100*2*10); Wave->QueueAudio(PCM.GetData(),PCM.Num());
    auto* Audio=NewObject<UAudioComponent>(F.W.Component->GetOwner()); Audio->SetSound(Wave); Audio->VolumeMultiplier=0.37f; Audio->ComponentTags.Add(TEXT("StoryFlowLoop")); Audio->RegisterComponent(); Audio->Play(); FStoryFlowRollbackTestAccess::Audio(F.W.Component,Audio);
    Audio->OnAudioPlaybackPercentNative.Broadcast(Audio,Wave,0.5f); F.W.Component->SelectOption(TEXT("again")); Audio->OnAudioPlaybackPercentNative.Broadcast(Audio,Wave,0.8f); F.W.Component->SelectOption(TEXT("again"));
    TestTrue(TEXT("loop Back"),F.W.Component->GoBack().bOk); auto* Restored=F.W.Component->GetCurrentDialogueAudio(); TestNotNull(TEXT("loop native channel restarted"),Restored);
    if(Restored) TestEqual(TEXT("loop volume restored"),Restored->VolumeMultiplier,0.37f);
    TestEqual(TEXT("available loop position restored"),FStoryFlowRollbackTestAccess::AudioPosition(F.W.Component),5.0f);
    FStoryFlowExecutionSnapshot S; FString Reason; FStoryFlowExecutionSnapshot::Capture(FStoryFlowRollbackTestAccess::Context(F.W.Component),1,S,Reason);
    auto Controller=FStoryFlowRollbackTestAccess::Controller(F.W.Component); TestTrue(TEXT("loop owned controller"),Controller.IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackImportLifecycleTest, "StoryFlow.Rollback.ImportAndLiveSyncLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackImportLifecycleTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init()) return false; F.Start(); F.W.Component->SelectOption(TEXT("buy")); auto* O=Observe(F.W.Component); bool Once=true;
    O->OnAvailable=[&]() { if(Once) { Once=false; F.Start(); F.W.Component->SelectOption(TEXT("leave")); } };
    auto* Imported=UStoryFlowImporter::ImportProject(FPaths::Combine(Fixtures(),TEXT("purchase")),F.Content);
    TestTrue(TEXT("in-place import preserves project identity"),Imported==F.P); TestEqual(TEXT("callback replacement excluded throughout actual import"),F.W.Component->GetRollbackAvailability().Reason,FString(TEXT("contentChanged")));
    O->OnAvailable=nullptr; F.W.Component->StopDialogue(); F.Start(); F.W.Component->SelectOption(TEXT("buy")); TestTrue(TEXT("post-import session captures"),F.W.Component->CanGoBack());
    const FString Root=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("RollbackSync"),FGuid::NewGuid().ToString(EGuidFormats::Digits)); const FString Build=FPaths::Combine(Root,TEXT("build"));
    TestTrue(TEXT("copy shared sync fixture to task scratch"),FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*Build,*FPaths::Combine(Fixtures(),TEXT("purchase")),true));
    auto Client=MakeShared<FStoryFlowWebSocketClient>(); auto Sync=MakeShared<FStoryFlowSyncManager>(); Sync->Initialize(Client); Sync->SetContentPath(F.Content);
    auto Payload=MakeShared<FJsonObject>(); Payload->SetStringField(TEXT("projectPath"),Root); Client->OnMessageReceived.Broadcast(TEXT("project-updated"),Payload); FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    TestTrue(TEXT("live sync uses same importer project"),Sync->GetProjectAsset()==F.P); TestEqual(TEXT("live sync invalidates old history"),F.W.Component->GetRollbackAvailability().Reason,FString(TEXT("contentChanged")));
    F.W.Component->StopDialogue(); auto Document=Read(FPaths::Combine(Build,TEXT("project.json"))); Document->GetObjectField(TEXT("metadata"))->Values.Remove(TEXT("dialogueRollback")); FString Json; FJsonSerializer::Serialize(Document.ToSharedRef(),TJsonWriterFactory<>::Create(&Json)); FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Build,TEXT("project.json")));
    Client->OnMessageReceived.Broadcast(TEXT("project-updated"),Payload); FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread); TestFalse(TEXT("sync omission disables metadata"),F.P->Metadata.DialogueRollback.bEnabled);
    F.Start(); TestFalse(TEXT("new session latches omitted settings disabled"),FStoryFlowRollbackTestAccess::Controller(F.W.Component).IsValid()); Sync->Shutdown(); IFileManager::Get().DeleteDirectory(*Root,false,true);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackFaceReleaseTest, "StoryFlow.Rollback.RestoredFaceSurrendersMorphWrites", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackFaceReleaseTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"))) return false; F.Start(); auto* L=NewObject<UStoryFlowLipsyncComponent>(F.W.Component->GetOwner()); L->Source=F.W.Component; L->RegisterComponent(); FStoryFlowRollbackTestAccess::Bind(L);
    L->SetActive(true); TestTrue(TEXT("fixture ticks active native lipsync"),L->IsActive());
    auto* Mesh=NewObject<USkeletalMeshComponent>(F.W.Component->GetOwner()); FStoryFlowRollbackTestAccess::Open(L); FStoryFlowRollbackTestAccess::Face(L,Mesh); Mesh->SetMorphTarget(TEXT("defaultBuff"),0.6f);
    F.W.Component->SelectOption(TEXT("again")); TestTrue(TEXT("fixture owns a visible morph"),Mesh->GetMorphTarget(TEXT("jawOpen"))>0);
    F.W.Component->GoBack(); TestEqual(TEXT("restored owned morph zero"),Mesh->GetMorphTarget(TEXT("jawOpen")),0.0f); TestEqual(TEXT("unowned morph unchanged"),Mesh->GetMorphTarget(TEXT("defaultBuff")),0.6f);
    Mesh->SetMorphTarget(TEXT("jawOpen"),0.7f); FStoryFlowRollbackTestAccess::StaleFace(L); FStoryFlowRollbackTestAccess::Tick(L); TestEqual(TEXT("restored stale-face retry cannot overwrite external animation"),Mesh->GetMorphTarget(TEXT("jawOpen")),0.7f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackBudgetRetryTest, "StoryFlow.Rollback.BudgetEvictionRetryAndMissingMedia", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackBudgetRetryTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"),true,1000)) return false; F.Start(); auto* Script=F.P->GetScriptByPath(F.P->StartupScript); auto& C=FStoryFlowRollbackTestAccess::Context(F.W.Component);
    Script->Nodes[TEXT("A")].Data.Text=FString::ChrN(1024*1024,TCHAR('x'));
    for(int32 I=0;I<20;++I) F.W.Component->SelectOption(TEXT("again"));
    auto Owner=FStoryFlowRollbackTestAccess::Controller(F.W.Component); TestTrue(TEXT("payload budget evicts oldest checkpoints"),Owner->Count()<21); TestTrue(TEXT("retained payload bounded"),Owner->EstimatedBytes()<=FStoryFlowRollbackController::PayloadLimit); TestTrue(TEXT("recent entry still available"),F.W.Component->CanGoBack());
    Script->Nodes[TEXT("A")].Data.Text=FString::ChrN(17*1024*1024,TCHAR('x')); F.W.Component->SelectOption(TEXT("again")); TestEqual(TEXT("oversized checkpoint reason"),F.W.Component->GetRollbackAvailability().Reason,FString(TEXT("budget"))); TestEqual(TEXT("oversized clears retained history"),Owner->Count(),0);
    Script->Nodes[TEXT("A")].Data.Text=TEXT("small"); F.W.Component->SelectOption(TEXT("again")); TestEqual(TEXT("retry makes fresh baseline"),Owner->Count(),1); F.W.Component->SelectOption(TEXT("again")); TestTrue(TEXT("retry recovers Back"),F.W.Component->CanGoBack());
    auto* Media=NewObject<UTexture2D>(); C.CurrentDialogueState.Image=Media; FStoryFlowExecutionSnapshot Snapshot,Prepared; FString Reason; TestTrue(TEXT("media snapshot"),FStoryFlowExecutionSnapshot::Capture(C,1,Snapshot,Reason)); C.CurrentDialogueState.Image=nullptr; Media->MarkAsGarbage();
    TestTrue(TEXT("missing media uses fallback without invalidating story"),Snapshot.Prepare(C,Prepared,Reason)); Prepared.Apply(C); TestNull(TEXT("missing image fallback"),C.CurrentDialogueState.Image.Get());
    FStoryFlowVariant Cycle; Cycle.SetMap({}); FStoryFlowMapEntry Entry; Entry.Key.SetString(TEXT("cycle")); Entry.Value.AliasMap(Cycle); Cycle.GetMapMutable().Add(Entry); FStoryFlowVariable Var; Var.Id=TEXT("cycle"); Var.Value=Cycle; C.LocalVariables.Add(Var.Id,Var);
    Reason.Reset(); TestFalse(TEXT("cyclic map rejected before full copy"),FStoryFlowExecutionSnapshot::Capture(C,2,Snapshot,Reason)); TestEqual(TEXT("cycle reason"),Reason,FString(TEXT("unsupportedState"))); Cycle.GetMapMutable().Reset(); C.LocalVariables.Remove(Var.Id);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackCostTest, "StoryFlow.Rollback.NativeCostAndDoubleBack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackCostTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init()) return false; F.Start(); F.W.Component->SelectOption(TEXT("buy")); auto Owner=FStoryFlowRollbackTestAccess::Controller(F.W.Component); const int64 Payload=Owner->EstimatedBytes(); const double CaptureMs=Owner->CaptureMilliseconds;
    auto* O=Observe(F.W.Component); FString NestedReason; O->OnRestore=[&]() { NestedReason=F.W.Component->GoBack().Reason; };
    TestTrue(TEXT("first Back succeeds"),F.W.Component->GoBack().bOk); TestEqual(TEXT("reentrant Back blocked"),NestedReason,FString(TEXT("busy"))); TestEqual(TEXT("sequential second Back empty"),F.W.Component->GoBack().Reason,FString(TEXT("empty")));
    AddInfo(FString::Printf(TEXT("purchase native observed retained=%lld bytes, captures=%llu, last capture=%.6f ms, prepare=%.6f ms, commit=%.6f ms; allocation count not instrumented"),Payload,Owner->Captures,CaptureMs,Owner->PrepareMilliseconds,Owner->CommitMilliseconds));
    F.W.Component->StopDialogue(); TestFalse(TEXT("ended entry has no restored presentation marker"),F.W.Component->IsCurrentDialogueRestored());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackBeforeLeaveTest, "StoryFlow.Rollback.BeforeLeaveSessionReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackBeforeLeaveTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    for(bool Advance : {false,true}) for(bool Restart : {false,true}) {
        FFixture F; if(!F.Init(TEXT("same-node"))) return false;
        auto* Script=F.P->GetScriptByPath(F.P->StartupScript);
        if(Advance) { Script->Nodes[TEXT("A")].Data.Options.Reset(); Script->Connections[1].SourceHandle=StoryFlowHandles::Source(TEXT("A")); Script->BuildConnectionIndices(); }
        F.Start(); auto* C=F.W.Component; auto* O=Observe(C); bool Selected=false, Replaced=false;
        auto Input=[&]() { if(Advance) C->AdvanceDialogue(); else C->SelectOption(TEXT("again")); };
        O->OnUpdate=[&]() { if(!Selected && C->GetIntVariable(TEXT("Visits"),true)==1) { Selected=true; Input(); } };
        O->OnAvailable=[&]() { if(!Replaced && O->LastAvailability.Reason==TEXT("busy")) { Replaced=true; C->StopDialogue(); if(Restart) F.Start(); } };
        Input(); O->OnUpdate=nullptr; O->OnAvailable=nullptr;
        TestTrue(TEXT("BeforeLeave callback ran"),Replaced); TestEqual(TEXT("obsolete input cannot execute ordinary set traversal"),C->GetIntVariable(TEXT("Visits"),true),1);
        TestEqual(TEXT("observer lifecycle wins"),C->IsDialogueActive(),Restart); TestEqual(TEXT("counted owner matches active session"),FStoryFlowRollbackTestAccess::Counted(C),Restart);
        if(Restart) TestTrue(TEXT("current controller has sole registered ownership"),F.W.Subsystem->OwnsRollback(FStoryFlowRollbackTestAccess::Controller(C).Get()));
        C->StopDialogue(); TestFalse(TEXT("final count returns to zero"),F.W.Subsystem->IsDialogueActive());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackAudioRedrawTest, "StoryFlow.Rollback.RestoredAudioRedraw", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackAudioRedrawTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"))) return false; auto* C=F.W.Component;
    auto* Script=F.P->GetScriptByPath(F.P->StartupScript); Script->Nodes[TEXT("A")].Data.bAudioAdvanceOnEnd=true; Script->Nodes[TEXT("A")].Data.bAudioAllowSkip=false;
    F.Start(); C->SelectOption(TEXT("again")); TestTrue(TEXT("Back"),C->GoBack().bOk); TestFalse(TEXT("initial restored audio gating disabled"),C->GetCurrentDialogue().bAudioAdvanceOnEnd);
    C->SetIntVariable(TEXT("Visits"),42,true);
    TestFalse(TEXT("redraw retains restored audio advance contract"),C->GetCurrentDialogue().bAudioAdvanceOnEnd); TestFalse(TEXT("redraw retains restored audio skip contract"),C->GetCurrentDialogue().bAudioAllowSkip);
    C->SelectOption(TEXT("again")); TestTrue(TEXT("fresh forward entry recovers authored audio gating"),C->GetCurrentDialogue().bAudioAdvanceOnEnd); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackIndividualImportTest, "StoryFlow.Rollback.IndividualScriptImport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackIndividualImportTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    for(bool FileApi : {false,true}) {
        FFixture F, Unaffected; if(!F.Init(TEXT("same-node")) || !Unaffected.Init(TEXT("same-node"))) return false;
        F.Start(); Unaffected.Start(); auto* C=F.W.Component; C->SelectOption(TEXT("again")); Unaffected.W.Component->SelectOption(TEXT("again"));
        auto* Script=F.P->GetScriptByPath(F.P->StartupScript); TestTrue(TEXT("runtime and imported project share script object"),FStoryFlowRollbackTestAccess::Context(C).CurrentScript.Get()==Script);
        const FString DataPath=FPaths::Combine(F.Content,TEXT("Data")); auto Json=Read(FPaths::Combine(Fixtures(),TEXT("same-node/main.json")));
        bool Skipped=false; TestTrue(TEXT("identical single-script import reuses object"),UStoryFlowImporter::ImportScriptFromJson(Json,TEXT("main"),DataPath,&Skipped)==Script);
        TestTrue(TEXT("identical import deliberately skips"),Skipped); TestTrue(TEXT("identical import preserves history"),C->CanGoBack());
        auto* O=Observe(C); bool Restarted=false;
        O->OnAvailable=[&]() { if(!Restarted && O->LastAvailability.Reason==TEXT("contentChanged")) { Restarted=true; F.Start(); } };
        Json->GetObjectField(TEXT("strings"))->GetObjectField(TEXT("en"))->SetStringField(TEXT("A.text"),TEXT("CHANGED"));
        UStoryFlowScriptAsset* Imported=nullptr;
        if(FileApi) { const FString Temp=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("RollbackReview2/main.json")); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Temp),true); FString Text; FJsonSerializer::Serialize(Json.ToSharedRef(),TJsonWriterFactory<>::Create(&Text)); FFileHelper::SaveStringToFile(Text,*Temp); Imported=UStoryFlowImporter::ImportScript(Temp,DataPath); IFileManager::Get().Delete(*Temp); }
        else Imported=UStoryFlowImporter::ImportScriptFromJson(Json,TEXT("main"),DataPath);
        O->OnAvailable=nullptr; TestTrue(TEXT("single-script update reuses live asset"),Imported==Script); TestTrue(TEXT("callback-started owner observed guard"),Restarted);
        TestFalse(TEXT("changed script unavailable"),C->CanGoBack()); TestEqual(TEXT("changed script permanent reason"),C->GetRollbackAvailability().Reason,FString(TEXT("contentChanged")));
        C->SelectOption(TEXT("again")); C->SelectOption(TEXT("again")); TestEqual(TEXT("no later collection until restart"),FStoryFlowRollbackTestAccess::Controller(C)->Count(),0);
        TestTrue(TEXT("unrelated project history unaffected"),Unaffected.W.Component->CanGoBack()); F.Start(); C->SelectOption(TEXT("again")); TestTrue(TEXT("restart recovers collection"),C->CanGoBack());
        TestEqual(TEXT("new graph presentation"),C->GetCurrentDialogue().Text,FString(TEXT("CHANGED")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackNonYieldRestartTest, "StoryFlow.Rollback.NonYieldRestartAvailability", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackNonYieldRestartTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    FFixture F; if(!F.Init(TEXT("same-node"))) return false; auto* C=F.W.Component;
    auto* Empty=NewObject<UStoryFlowScriptAsset>(F.P); Empty->ScriptPath=TEXT("empty");
    FStoryFlowNode Start; Start.Id=TEXT("0"); Start.Type=EStoryFlowNodeType::Start; Empty->Nodes.Add(Start.Id,Start);
    FStoryFlowNode Branch; Branch.Id=TEXT("branch"); Branch.Type=EStoryFlowNodeType::Branch; Empty->Nodes.Add(Branch.Id,Branch);
    FStoryFlowConnection Edge; Edge.Source=TEXT("0"); Edge.Target=TEXT("branch"); Edge.SourceHandle=StoryFlowHandles::Source(TEXT("0")); Edge.TargetHandle=StoryFlowHandles::Target(TEXT("branch")); Empty->Connections.Add(Edge); Empty->BuildConnectionIndices(); F.P->Scripts.Add(Empty->ScriptPath,Empty);
    F.Start(); C->SelectOption(TEXT("again")); auto* O=Observe(C); auto* Widget=NewObject<UStoryFlowRollbackWidgetSpy>(F.W.World); Widget->InstallBackButton(true); Widget->InitializeWithComponent(C);
    const int32 Before=O->Availability; C->StartDialogueWithScript(Empty->ScriptPath);
    TestFalse(TEXT("non-yield getter unavailable"),C->CanGoBack()); TestTrue(TEXT("non-yield restart notified subscriber"),O->Availability>Before); TestFalse(TEXT("non-yield restart disables persistent button"),Widget->BackEnabled());
    TestEqual(TEXT("final notification matches current getter reason"),O->LastAvailability.Reason,C->GetRollbackAvailability().Reason); TestEqual(TEXT("final notification matches current getter available"),O->LastAvailability.bCanGoBack,C->CanGoBack());
    F.Start(); TestEqual(TEXT("yielding restart final notification matches getter"),O->LastAvailability.Reason,C->GetRollbackAvailability().Reason); C->SelectOption(TEXT("again")); TestTrue(TEXT("yielding restart enables persistent button"),Widget->BackEnabled());
    F.P->Metadata.DialogueRollback.bEnabled=false; F.Start(); TestFalse(TEXT("disabled restart clears persistent button"),Widget->BackEnabled()); TestEqual(TEXT("disabled final reason matches getter"),O->LastAvailability.Reason,C->GetRollbackAvailability().Reason);
    Widget->DetachFromComponent(); C->StopDialogue(); TestFalse(TEXT("all restart ownership balanced"),F.W.Subsystem->IsDialogueActive()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackCallerMemoTest, "StoryFlow.Rollback.CallerMemoAfterCalleeWrite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackCallerMemoTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    for (bool Enabled : {false, true}) for (bool ComponentReturn : {false, true})
    {
        FFixture F; if (!F.Init(TEXT("barrier"), Enabled)) return false;
        auto* Caller=F.P->GetScriptByPath(F.P->StartupScript);
        auto* Callee=NewObject<UStoryFlowScriptAsset>(F.P); Callee->ScriptPath=TEXT("memo-callee"); F.P->Scripts.Add(Callee->ScriptPath,Callee);
        FStoryFlowVariable X; X.Id=TEXT("memo-x"); X.Name=TEXT("MemoX"); X.Type=EStoryFlowVariableType::Boolean; X.Value=FStoryFlowVariant::FromBool(false);
        F.W.Subsystem->GetGlobalVariables().Add(X.Id,X);
        FStoryFlowVariable Y=X; Y.Id=TEXT("memo-y"); Y.Name=TEXT("MemoY"); F.W.Subsystem->GetGlobalVariables().Add(Y.Id,Y);
        FStoryFlowNode Getter; Getter.Id=TEXT("memo-get"); Getter.Type=EStoryFlowNodeType::GetBool; Getter.Data.Variable=X.Id; Getter.Data.bIsGlobal=true;
        Caller->Nodes.Add(Getter.Id,Getter); Callee->Nodes.Add(Getter.Id,Getter);
        FStoryFlowNode Call; Call.Id=TEXT("memo-call"); Call.Type=EStoryFlowNodeType::RunScript; Caller->Nodes.Add(Call.Id,Call);
        FStoryFlowNode Copy; Copy.Id=TEXT("memo-copy"); Copy.Type=EStoryFlowNodeType::SetBool; Copy.Data.Variable=Y.Id; Copy.Data.bIsGlobal=true; Caller->Nodes.Add(Copy.Id,Copy);
        auto Edge=[&](const FString& Source,const FString& Suffix,const FString& Target,const FString& Input) {
            FStoryFlowConnection E; E.Id=Source+Target; E.Source=Source; E.Target=Target; E.SourceHandle=StoryFlowHandles::Source(Source,Suffix); E.TargetHandle=StoryFlowHandles::Target(Target,Input); Caller->Connections.Add(E);
        };
        Edge(Call.Id,StoryFlowHandles::Out_Output,Copy.Id,TEXT("")); Edge(Getter.Id,StoryFlowHandles::Out_Boolean,Copy.Id,StoryFlowHandles::In_Boolean); Edge(Copy.Id,TEXT("1"),TEXT("A"),TEXT("")); Caller->BuildConnectionIndices();
        F.Start(); auto& Context=FStoryFlowRollbackTestAccess::Context(F.W.Component); FStoryFlowEvaluator Evaluator(&Context);
        TestFalse(TEXT("caller initially reads false"),Evaluator.EvaluateBooleanFromNode(Caller->Nodes.Find(Getter.Id),Copy.Id,StoryFlowHandles::Source(Getter.Id,StoryFlowHandles::Out_Boolean)));
        auto& Semantic=Context.GetNodeState(TEXT("semantic-output")); Semantic.bIsExecutionOutput=true; Semantic.bHasCachedOutput=true; Semantic.CachedOutput=FStoryFlowVariant::FromInt(42); Semantic.OutputValues.Add(TEXT("output"),FStoryFlowVariant::FromInt(7)); Semantic.bHasOutputValues=true; Semantic.LoopIndex=2; Semantic.bLoopInitialized=true;
        TestTrue(TEXT("enter callee"),Context.PushScript(Callee->ScriptPath,Call.Id));
        Context.SetVariable(X.Id,FStoryFlowVariant::FromBool(true),true);
        TestTrue(TEXT("callee reads live write and consumes revision"),Evaluator.EvaluateBooleanFromNode(Callee->Nodes.Find(Getter.Id),TEXT("callee-branch"),StoryFlowHandles::Source(Getter.Id,StoryFlowHandles::Out_Boolean)));
        if (ComponentReturn) { FStoryFlowRollbackTestAccess::RootEnd(F.W.Component); TestTrue(TEXT("real end continuation copies fresh global"),F.W.Component->GetBoolVariable(Y.Name,true)); }
        else { TestTrue(TEXT("context return"),Context.PopScript()); TestTrue(TEXT("caller reads fresh global after return"),Evaluator.EvaluateBooleanFromNode(Caller->Nodes.Find(Getter.Id),Copy.Id,StoryFlowHandles::Source(Getter.Id,StoryFlowHandles::Out_Boolean))); }
        const auto& Retained=Context.NodeRuntimeStates.FindChecked(TEXT("semantic-output"));
        TestEqual(TEXT("execution output retained"),Retained.CachedOutput.GetInt(),42); TestEqual(TEXT("completed call output retained"),Retained.OutputValues.FindChecked(TEXT("output")).GetInt(),7); TestEqual(TEXT("loop continuation retained"),Retained.LoopIndex,2);
        TestEqual(TEXT("call stack returned"),Context.CallStack.Num(),0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackGraphBarrierCallbackTest, "StoryFlow.Rollback.GraphBarrierCallback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackGraphBarrierCallbackTest::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    for (const FString Action : {TEXT("normal"),TEXT("disabled"),TEXT("stop"),TEXT("restart"),TEXT("replace")})
    {
        FFixture F; if (!F.Init(TEXT("barrier"),Action!=TEXT("disabled"))) return false;
        auto* Replacement=DuplicateObject<UStoryFlowScriptAsset>(F.P->GetScriptByPath(F.P->StartupScript),F.P); Replacement->ScriptPath=TEXT("barrier-replacement"); F.P->Scripts.Add(Replacement->ScriptPath,Replacement);
        F.Start(); F.W.Component->AdvanceDialogue(); TestEqual(TEXT("before graph barrier"),F.W.Component->GetCurrentDialogue().NodeId,FString(TEXT("B")));
        auto* O=Observe(F.W.Component); bool Fired=false;
        O->OnAvailable=[&]() {
            if(Fired || O->LastAvailability.bCanGoBack) return; Fired=true;
            if(Action==TEXT("stop")) F.W.Component->StopDialogue();
            else if(Action==TEXT("restart")) F.W.Component->StartDialogueWithScript(F.P->StartupScript);
            else if(Action==TEXT("replace")) F.W.Component->StartDialogueWithScript(Replacement->ScriptPath);
        };
        F.W.Component->AdvanceDialogue(); auto& Context=FStoryFlowRollbackTestAccess::Context(F.W.Component);
        if(Action==TEXT("normal") || Action==TEXT("disabled")) TestEqual(TEXT("ordinary barrier continues"),F.W.Component->GetCurrentDialogue().NodeId,FString(TEXT("C")));
        else if(Action==TEXT("stop")) { TestTrue(TEXT("stop callback fired"),Fired); TestFalse(TEXT("old graph cannot resume stopped session"),Context.bIsExecuting); }
        else {
            TestTrue(TEXT("replacement callback fired"),Fired); TestEqual(TEXT("old graph leaves replacement at first line"),F.W.Component->GetCurrentDialogue().NodeId,FString(TEXT("A")));
            F.W.Component->PauseDialogue(); F.W.Component->ResumeDialogue(); TestEqual(TEXT("public resume has no queued old edge"),F.W.Component->GetCurrentDialogue().NodeId,FString(TEXT("A")));
            TestTrue(TEXT("replacement counted active"),F.W.Subsystem->IsDialogueActive() && FStoryFlowRollbackTestAccess::Counted(F.W.Component));
        }
        F.W.Component->StopDialogue(); TestFalse(TEXT("final lifecycle count balanced"),F.W.Subsystem->IsDialogueActive());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRollbackAvailabilityReentry, "StoryFlow.Rollback.AvailabilityObserverMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowRollbackAvailabilityReentry::RunTest(const FString&)
{
    using namespace StoryFlowRollbackTests;
    for (const FString Action : {TEXT("back"), TEXT("block"), TEXT("stop"), TEXT("restart"), TEXT("disabled")})
    {
        FFixture F; if (!F.Init(TEXT("same-node"))) return false; F.Start();
        auto* C = F.W.Component; auto* First = Observe(C); auto* Later = Observe(C); bool Changed = false;
        First->OnAvailable = [&, C]() {
            if (Changed || !First->LastAvailability.bCanGoBack) return;
            Changed = true;
            if (Action == TEXT("back")) { TestTrue(TEXT("observer Back succeeds"), C->GoBack().bOk); }
            else if (Action == TEXT("block")) C->BlockRollback(TEXT("host"));
            else { C->StopDialogue(); if (Action != TEXT("stop")) { F.P->Metadata.DialogueRollback.bEnabled = Action != TEXT("disabled"); F.Start(); } }
        };
        C->SelectOption(TEXT("again"));
        const auto Live = C->GetRollbackAvailability();
        TestTrue(Action + TEXT(" observer mutates session"), Changed);
        TestTrue(Action + TEXT(" later observer receives correction"), Later->Availability >= 2);
        TestFalse(Action + TEXT(" final payload disables Back"), Later->LastAvailability.bCanGoBack);
        TestEqual(Action + TEXT(" final availability is live"), Later->LastAvailability.bCanGoBack, Live.bCanGoBack);
        TestEqual(Action + TEXT(" final steps are live"), Later->LastAvailability.Steps, Live.Steps);
        TestEqual(Action + TEXT(" final reason is live"), Later->LastAvailability.Reason, Live.Reason);
        First->OnAvailable = nullptr;
    }
    return true;
}

#endif
