// Copyright 2026 StoryFlow. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StoryFlowComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/MorphTarget.h"
#include "Sound/SoundWave.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "UObject/ObjectKey.h"
#include "Misc/AutomationTest.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowHandles.h"

#include "Lipsync/StoryFlowLipsyncComponent.h"

#include "StoryFlowScopedWorld.h"

// Explicit-instantiation access keeps MSVC member names and all production code intact.
namespace StoryFlowLipsyncLifecycleAccess
{
	template<class Tag, auto Member> struct Grant { friend auto Get(Tag) { return Member; } };
	template<class Tag, class Component> decltype(auto) Field(Component* C) { return C->*Get(Tag{}); }
	template<class Tag, class... Args> decltype(auto) Call(UStoryFlowLipsyncComponent* C, Args&&... A) { return (C->*Get(Tag{}))(Forward<Args>(A)...); }
	struct DialogueAudio { friend auto Get(DialogueAudio); }; template struct Grant<DialogueAudio, &UStoryFlowComponent::CurrentDialogueAudio>;
	struct HandleDialogueUpdated { friend auto Get(HandleDialogueUpdated); }; template struct Grant<HandleDialogueUpdated, &UStoryFlowLipsyncComponent::HandleDialogueUpdated>;
	struct LineAudio { friend auto Get(LineAudio); }; template struct Grant<LineAudio, &UStoryFlowLipsyncComponent::LineAudio>;
	struct bLineIsMine { friend auto Get(bLineIsMine); }; template struct Grant<bLineIsMine, &UStoryFlowLipsyncComponent::bLineIsMine>;
	struct bLineCarriesAudio { friend auto Get(bLineCarriesAudio); }; template struct Grant<bLineCarriesAudio, &UStoryFlowLipsyncComponent::bLineCarriesAudio>;
	struct bAnalysisAvailable { friend auto Get(bAnalysisAvailable); }; template struct Grant<bAnalysisAvailable, &UStoryFlowLipsyncComponent::bAnalysisAvailable>;
	struct Targets { friend auto Get(Targets); }; template struct Grant<Targets, &UStoryFlowLipsyncComponent::Targets>;
	struct Driver { friend auto Get(Driver); }; template struct Grant<Driver, &UStoryFlowLipsyncComponent::Driver>;
	struct ResolvedTable { friend auto Get(ResolvedTable); }; template struct Grant<ResolvedTable, &UStoryFlowLipsyncComponent::ResolvedTable>;
	struct HandleDialogueEnded { friend auto Get(HandleDialogueEnded); }; template struct Grant<HandleDialogueEnded, &UStoryFlowLipsyncComponent::HandleDialogueEnded>;
	struct DecideDrive { friend auto Get(DecideDrive); }; template struct Grant<DecideDrive, &UStoryFlowLipsyncComponent::DecideDrive>;
	struct ResolveFace { friend auto Get(ResolveFace); }; template struct Grant<ResolveFace, &UStoryFlowLipsyncComponent::ResolveFace>;
	struct RefreshFaceIfStale { friend auto Get(RefreshFaceIfStale); }; template struct Grant<RefreshFaceIfStale, &UStoryFlowLipsyncComponent::RefreshFaceIfStale>;
	struct TickComponent { friend auto Get(TickComponent); }; template struct Grant<TickComponent, &UStoryFlowLipsyncComponent::TickComponent>;
	struct ApplyWeights { friend auto Get(ApplyWeights); }; template struct Grant<ApplyWeights, &UStoryFlowLipsyncComponent::ApplyWeights>;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLifecycleTail,
	"StoryFlow.Lipsync.Lifecycle.EndedTextLineClosesAfterInheritedTail", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowLipsyncLifecycleTail::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncLifecycleAccess;

	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Lipsync = NewObject<UStoryFlowLipsyncComponent>(W.Component->GetOwner());
	Lipsync->Source = W.Component;
	Lipsync->RegisterComponent();
	W.Component->GetOwner()->DispatchBeginPlay();
	Lipsync->SetActive(true);
	TestTrue(TEXT("fixture ticks an active component"), Lipsync->IsActive());
	auto* Audio = NewObject<UAudioComponent>();
	// Real UAudioComponent::IsPlaying reads this flag. No sound hardware is needed.
	Audio->SetActiveFlag(true);
	Field<LineAudio>(Lipsync) = Audio;
	Field<bLineIsMine>(Lipsync) = true;
	Field<bLineCarriesAudio>(Lipsync) = false;
	Field<bAnalysisAvailable>(Lipsync) = true;
	Call<HandleDialogueEnded>(Lipsync);
	const auto TailDrive = Call<DecideDrive>(Lipsync);
	TestTrue(TEXT("the audible tail continues after the text ends"), TailDrive == decltype(TailDrive)::Analyse);
	TestTrue(TEXT("a surviving audio tail remains publicly active"), Lipsync->IsLipsyncActive());
	Audio->SetActiveFlag(false);
	const auto Drive = Call<DecideDrive>(Lipsync);
	AddInfo(FString::Printf(TEXT("after dialogue and inherited audio end: drive=%d (Silent=%d), lineIsMine=%d"), int32(Drive), int32(decltype(Drive)::Silent), Field<bLineIsMine>(Lipsync)));
	TestTrue(TEXT("ended dialogue must not resume idle speech after its inherited tail stops"), Drive == decltype(Drive)::Silent);
	TestFalse(TEXT("activity ends when the tail stops"), Lipsync->IsLipsyncActive());
	Field<Driver>(Lipsync) = MakeUnique<FStoryFlowLipsyncDriver>(StoryFlowVisemeTable::Default());
	Call<TickComponent>(Lipsync, 1.0f / 60.0f, LEVELTICK_All, nullptr);
	Audio->SetActiveFlag(true);
	const auto ReplayDrive = Call<DecideDrive>(Lipsync);
	TestTrue(TEXT("a later replay cannot revive a completed tail"), ReplayDrive == decltype(ReplayDrive)::Silent);
	Audio->SetActiveFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLifecycleDestroyedAudio,
	"StoryFlow.Lipsync.Lifecycle.DestroyedTrackedAudioCloses", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowLipsyncLifecycleDestroyedAudio::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncLifecycleAccess;

	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Lipsync = NewObject<UStoryFlowLipsyncComponent>(W.Component->GetOwner());
	auto* Audio = NewObject<UAudioComponent>(W.Component->GetOwner());
	Audio->RegisterComponent();
	Audio->SetActiveFlag(true);
	Lipsync->Source = W.Component;
	Field<DialogueAudio>(W.Component) = Audio;
	FStoryFlowDialogueState Line;
	Line.NodeId = TEXT("audio-line");
	Line.Audio = NewObject<USoundWave>();
	Call<HandleDialogueUpdated>(Lipsync, Line);
	Field<bAnalysisAvailable>(Lipsync) = true;
	Audio->SetActiveFlag(false);
	Audio->DestroyComponent();
	TestFalse(TEXT("destroyed audio is no longer a valid weak target"), Field<LineAudio>(Lipsync).IsValid());
	const auto Drive = Call<DecideDrive>(Lipsync);
	AddInfo(FString::Printf(TEXT("destroyed tracked sound: drive=%d (Silent=%d)"), int32(Drive), int32(decltype(Drive)::Silent)));
	TestTrue(TEXT("losing a tracked sound must close instead of analysing the unrelated mix"), Drive == decltype(Drive)::Silent);
	Lipsync->StopLipsync();
	Field<DialogueAudio>(W.Component) = nullptr;
	Call<HandleDialogueUpdated>(Lipsync, Line);
	const auto ExternalDrive = Call<DecideDrive>(Lipsync);
	TestTrue(TEXT("never-tracked custom playback still follows the line"), ExternalDrive == decltype(ExternalDrive)::Analyse);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLifecycleDestroyMouth,
	"StoryFlow.Lipsync.Lifecycle.DestroyComponentReleasesMouth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowLipsyncLifecycleDestroyMouth::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncLifecycleAccess;

	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Owner = W.Component->GetOwner();
	auto* Lipsync = NewObject<UStoryFlowLipsyncComponent>(Owner);
	Lipsync->Source = W.Component;
	Lipsync->RegisterComponent();
	Owner->DispatchBeginPlay();
	TestTrue(TEXT("component began play"), Lipsync->HasBegunPlay());
	auto* Mesh = NewObject<USkeletalMeshComponent>(Owner);
	auto& Target = Field<Targets>(Lipsync).AddDefaulted_GetRef();
	Target.Mesh = Mesh;
	Target.Morphs.Add(TEXT("jawOpen"));
	TArray<float> Speech;
	Speech.Init(0.02f, 24);
	Field<Driver>(Lipsync)->AdvanceFromMagnitudes(Speech, 1.0f);
	Call<ApplyWeights>(Lipsync);
	const float Before = Mesh->GetMorphTarget(TEXT("jawOpen"));
	TestTrue(TEXT("fixture drives the jaw"), Before > 0.05f);
	Mesh->SetMorphTarget(TEXT("defaultBuff"), 0.6f);
	Lipsync->DestroyComponent();
	AddInfo(FString::Printf(TEXT("jaw weight before destroy=%.4f, after=%.4f"), Before, Mesh->GetMorphTarget(TEXT("jawOpen"))));
	TestEqual(TEXT("destroying only lipsync must release the surviving face"), Mesh->GetMorphTarget(TEXT("jawOpen")), 0.0f);
	TestEqual(TEXT("teardown preserves morphs outside the owned set"), Mesh->GetMorphTarget(TEXT("defaultBuff")), 0.6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLifecyclePartialFace,
	"StoryFlow.Lipsync.Lifecycle.LateFacePartsAreDiscovered", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowLipsyncLifecyclePartialFace::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncLifecycleAccess;

	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Owner = W.Component->GetOwner();
	auto* Root = NewObject<USceneComponent>(Owner);
	Owner->SetRootComponent(Root);
	auto* Asset = NewObject<USkeletalMesh>();
	auto* Morph = NewObject<UMorphTarget>(Asset, TEXT("jawOpen"));
	auto& LOD = Morph->GetMorphLODModels().AddDefaulted_GetRef();
	LOD.Vertices.AddDefaulted();
	LOD.NumVertices = 1;
	Asset->RegisterMorphTarget(Morph, false);
	Asset->InitMorphTargets();
	auto AddMesh = [&]() {
		auto* Mesh = NewObject<USkeletalMeshComponent>(Owner);
		Mesh->SetSkeletalMesh(Asset);
		Mesh->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
		return Mesh;
	};
	AddMesh();
	auto* Lipsync = NewObject<UStoryFlowLipsyncComponent>(Owner);
	Field<ResolvedTable>(Lipsync).Add(TEXT("AA"), StoryFlowVisemeTable::FPose{{TEXT("jawOpen"), 1.0f}});
	Call<ResolveFace>(Lipsync, Field<ResolvedTable>(Lipsync));
	TestEqual(TEXT("initial head found"), Field<Targets>(Lipsync).Num(), 1);
	AddMesh();
	for (int32 Frame = 0; Frame < 600; ++Frame) Call<RefreshFaceIfStale>(Lipsync, 1.0f / 60.0f);
	AddInfo(FString::Printf(TEXT("targets after 10 seconds with second part attached: %d"), Field<Targets>(Lipsync).Num()));
	TestEqual(TEXT("late teeth/tongue part must join existing head"), Field<Targets>(Lipsync).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLifecycleScriptCollision,
	"StoryFlow.Lipsync.Lifecycle.SameNodeIdInAnotherScriptStartsNewLine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowLipsyncLifecycleScriptCollision::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncLifecycleAccess;

	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto Node = [](const FString& Id, EStoryFlowNodeType Type) {
		FStoryFlowNode N; N.Id = Id; N.Type = Type; return N;
	};
	auto Edge = [](const FString& From, const FString& To) {
		FStoryFlowConnection E; E.Id = From + To; E.Source = From; E.Target = To;
		E.SourceHandle = StoryFlowHandles::Source(From); E.TargetHandle = StoryFlowHandles::Target(To); return E;
	};
	auto* Parent = NewObject<UStoryFlowScriptAsset>();
	Parent->ScriptPath = TEXT("parent"); Parent->StartNode = TEXT("0");
	Parent->Nodes.Add(TEXT("0"), Node(TEXT("0"), EStoryFlowNodeType::Start));
	Parent->Nodes.Add(TEXT("1"), Node(TEXT("1"), EStoryFlowNodeType::Dialogue));
	Parent->Nodes[TEXT("1")].Data.Text = TEXT("Parent line");
	Parent->Nodes.Add(TEXT("run"), Node(TEXT("run"), EStoryFlowNodeType::RunScript));
	Parent->Nodes[TEXT("run")].Data.Script = TEXT("child");
	Parent->Connections.Add(Edge(TEXT("0"), TEXT("1")));
	Parent->Connections.Add(Edge(TEXT("1"), TEXT("run")));
	Parent->BuildConnectionIndices();
	auto* Child = NewObject<UStoryFlowScriptAsset>();
	Child->ScriptPath = TEXT("child"); Child->StartNode = TEXT("0");
	Child->Nodes.Add(TEXT("0"), Node(TEXT("0"), EStoryFlowNodeType::Start));
	Child->Nodes.Add(TEXT("1"), Node(TEXT("1"), EStoryFlowNodeType::Dialogue));
	Child->Nodes[TEXT("1")].Data.Text = TEXT("Child line");
	Child->Connections.Add(Edge(TEXT("0"), TEXT("1")));
	Child->Connections.Add(Edge(TEXT("1"), TEXT("1")));
	Child->BuildConnectionIndices();
	auto* Project = NewObject<UStoryFlowProjectAsset>();
	Project->Scripts.Add(TEXT("parent"), Parent); Project->Scripts.Add(TEXT("child"), Child);
	W.Subsystem->SetProject(Project);
	auto* Lipsync = NewObject<UStoryFlowLipsyncComponent>(W.Component->GetOwner());
	Lipsync->Source = W.Component; Lipsync->RegisterComponent();
	W.Component->GetOwner()->DispatchBeginPlay();
	W.Component->StartDialogueWithScript(TEXT("parent"));
	TestEqual(TEXT("parent starts"), Lipsync->GetLineStarts(), 1);
	W.Component->AdvanceDialogue();
	TestEqual(TEXT("execution actually enters child"), W.Component->GetCurrentDialogue().Text, FString(TEXT("Child line")));
	AddInfo(FString::Printf(TEXT("after entering a different script with node id 1: line starts=%d"), Lipsync->GetLineStarts()));
	TestEqual(TEXT("child is a fresh line even with the same local node id"), Lipsync->GetLineStarts(), 2);
	W.Component->PauseDialogue();
	W.Component->ResumeDialogue();
	TestEqual(TEXT("resuming redraws without restarting"), Lipsync->GetLineStarts(), 2);
	W.Component->AdvanceDialogue();
	TestEqual(TEXT("executing the same node again starts a new line"), Lipsync->GetLineStarts(), 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
