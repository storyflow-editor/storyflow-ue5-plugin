// Copyright 2026 StoryFlow. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MetaHuman/StoryFlowMetaHumanBakeSubsystem.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncComponent.h"
#include "MetaHuman/StoryFlowMetaHumanBakeBridge.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Sound/SoundWave.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "Editor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "ReferenceSkeleton.h"
#include "GameFramework/Actor.h"
#include "Modules/ModuleManager.h"
#include "Components/StoryFlowComponent.h"
#include "StoryFlowScopedWorld.h"
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshModel.h"

namespace StoryFlowMetaHumanSetup
{
// Private editor implementation shared with SaveComponent; no public plugin API is added.
const UStoryFlowMetaHumanLipsyncComponent* FindSetupTemplate(
	const TArray<const UStoryFlowMetaHumanLipsyncComponent*>& Templates, FName ComponentName);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetupTemplateNameTest, "StoryFlow.MetaHuman.Setup.BlueprintTemplateNames", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowSetupTemplateNameTest::RunTest(const FString& Parameters)
{
	auto* Outer = NewObject<UStoryFlowProjectAsset>();
	auto* Foo = NewObject<UStoryFlowMetaHumanLipsyncComponent>(Outer, TEXT("Foo_GEN_VARIABLE"));
	auto* Bar = NewObject<UStoryFlowMetaHumanLipsyncComponent>(Outer, TEXT("Bar_GEN_VARIABLE"));
	TArray<const UStoryFlowMetaHumanLipsyncComponent*> Templates { Foo, Bar };
	TestTrue(TEXT("Live component selects its Blueprint template among multiple adapters"),
		StoryFlowMetaHumanSetup::FindSetupTemplate(Templates, TEXT("Foo")) == Foo);
	TestTrue(TEXT("Template selection also accepts a template name"),
		StoryFlowMetaHumanSetup::FindSetupTemplate(Templates, TEXT("Bar_GEN_VARIABLE")) == Bar);
	TestNull(TEXT("An unmatched adapter cannot report a template persisted"),
		StoryFlowMetaHumanSetup::FindSetupTemplate(Templates, TEXT("Missing")));
	Templates.Remove(Bar);
	TestNull(TEXT("An unrelated sole adapter is not overwritten"),
		StoryFlowMetaHumanSetup::FindSetupTemplate(Templates, TEXT("Missing")));
	Templates.Empty();
	TestNull(TEXT("A Blueprint without an adapter cannot report one persisted"),
		StoryFlowMetaHumanSetup::FindSetupTemplate(Templates, TEXT("Foo")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVoiceDiscoveryTest, "StoryFlow.MetaHuman.Setup.VoiceDiscovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowVoiceDiscoveryTest::RunTest(const FString& Parameters)
{
	auto* Setup = GEditor->GetEditorSubsystem<UStoryFlowMetaHumanBakeSubsystem>();
	auto* Project = NewObject<UStoryFlowProjectAsset>();
	auto* Script = NewObject<UStoryFlowScriptAsset>(Project);
	auto* OtherScript = NewObject<UStoryFlowScriptAsset>(Project);
	auto* Alice = NewObject<USoundWave>(Project);
	auto* Bob = NewObject<USoundWave>(Project);
	auto* Shared = NewObject<USoundWave>(Project);
	auto* Dynamic = NewObject<USoundWave>(Project);
	Project->Scripts.Add(TEXT("one"), Script);
	Project->Scripts.Add(TEXT("two"), OtherScript);
	Project->CharacterIdToPath.Add(TEXT("alice-id"), TEXT("characters\\alice"));
	Project->ResolvedAssets.Add(TEXT("alice-voice"), Alice);
	Project->ResolvedAssets.Add(TEXT("dynamic"), Dynamic);
	Script->ResolvedAssets.Add(TEXT("bob-voice"), Bob);
	OtherScript->ResolvedAssets.Add(TEXT("shared"), Shared);
	FStoryFlowNode Node; Node.Type = EStoryFlowNodeType::Dialogue;
	Node.Data.Audio = TEXT("alice-voice"); Node.Data.CharacterRefId = TEXT("alice-id");
	Script->Nodes.Add(TEXT("a1"), Node); Script->Nodes.Add(TEXT("a2"), Node);
	Node.Data.Audio = TEXT("bob-voice"); Node.Data.CharacterRefId.Empty(); Node.Data.Character = TEXT("characters/bob");
	Script->Nodes.Add(TEXT("bob"), Node);
	Node.Data.Audio = TEXT("shared"); Node.Data.Character.Empty();
	OtherScript->Nodes.Add(TEXT("unknown-speaker"), Node);
	TestEqual(TEXT("All speakers includes each referenced voice once, not unused imported audio"), Setup->DiscoverVoices(Project, TEXT("")).Num(), 3);
	const auto AliceVoices = Setup->DiscoverVoices(Project, TEXT("alice-id"));
	TestTrue(TEXT("Stable character ID resolves to matching voice"), AliceVoices.Contains(Alice));
	TestFalse(TEXT("Other known speaker is excluded"), AliceVoices.Contains(Bob));
	TestTrue(TEXT("Unknown speaker is conservatively included"), AliceVoices.Contains(Shared));
	TestEqual(TEXT("Path aliases select the same character"), Setup->DiscoverVoices(Project, TEXT("Characters/Alice")).Num(), 2);
	Script->Nodes[TEXT("bob")].Data.bCharacterUseVarInput = true;
	TestTrue(TEXT("Variable speaker includes the voice even when a different static fallback is stored"), Setup->DiscoverVoices(Project, TEXT("alice-id")).Contains(Bob));
	Script->Nodes[TEXT("bob")].Data.bCharacterUseVarInput = false;
	Script->Nodes[TEXT("a1")].Data.bAudioUseVarInput = true;
	const auto VariableVoices = Setup->DiscoverVoices(Project, TEXT("alice-id"));
	TestEqual(TEXT("Variable audio covers all imported candidates without duplicates"), VariableVoices.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowBakeLibraryTest, "StoryFlow.MetaHuman.Setup.LibraryRouting", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowBakeLibraryTest::RunTest(const FString& Parameters)
{
	auto* Library = NewObject<UStoryFlowMetaHumanLipsyncLibrary>();
	auto* Voice = NewObject<USoundWave>(Library);
	auto* OtherVoice = NewObject<USoundWave>(Library);
	auto* Face = NewObject<USkeletalMesh>(Library);
	auto* OtherFace = NewObject<USkeletalMesh>(Library);
	auto* Animation = NewObject<UAnimSequence>(Library);
	auto& Entry = Library->Entries.AddDefaulted_GetRef();
	Entry.Voice = Voice; Entry.FaceMesh = Face; Entry.Animation = Animation;
	TestTrue(TEXT("Shared library routes the correct face and voice"), Library->FindAnimation(Voice, Face) == Animation);
	TestNull(TEXT("Different voice cannot play this bake"), Library->FindAnimation(OtherVoice, Face));
	TestNull(TEXT("Different face cannot play this bake"), Library->FindAnimation(Voice, OtherFace));
	Entry.bStale = true;
	TestNull(TEXT("Changed voice falls back until rebaked"), Library->FindAnimation(Voice, Face));
	Entry.bStale = false; Entry.Animation = nullptr;
	TestNull(TEXT("Missing animation falls back"), Library->FindAnimation(Voice, Face));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowBakeFaceScopeTest, "StoryFlow.MetaHuman.Setup.FaceRootScopesBake", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowBakeFaceScopeTest::RunTest(const FString& Parameters)
{
	StoryFlowTestWorld::FScopedWorld World;
	if (!TestTrue(TEXT("Test world initialized"), World.Init())) return false;
	auto* Owner = World.Component->GetOwner();
	auto* Setup = NewObject<UStoryFlowMetaHumanLipsyncComponent>(Owner);
	auto* First = NewObject<USkeletalMeshComponent>(Owner, TEXT("FirstFace"));
	auto* Second = NewObject<USkeletalMeshComponent>(Owner, TEXT("SecondFace"));
	Owner->AddInstanceComponent(First);
	Owner->AddInstanceComponent(Second);
	for (auto* Face : { First, Second })
	{
		auto* Mesh = NewObject<USkeletalMesh>(Owner);
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("FACIAL_C_FacialRoot"), TEXT("FACIAL_C_FacialRoot"), INDEX_NONE), FTransform::Identity);
		Mesh->SetPostProcessAnimBlueprint(UAnimInstance::StaticClass());
		Face->SetSkeletalMesh(Mesh);
		Face->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	}
	FString Issue;
	TestNull(TEXT("Two unscoped faces require a selection"), UStoryFlowMetaHumanBakeSubsystem::FindFace(Setup, Issue));
	Setup->FaceRoot.OverrideComponent = Second;
	TestTrue(TEXT("Baking uses the selected face, not the first face on the actor"), UStoryFlowMetaHumanBakeSubsystem::FindFace(Setup, Issue) == Second->GetSkeletalMeshAsset());
	Setup->FaceRoot.OverrideComponent.Reset();
	Setup->FaceRoot.ComponentProperty = TEXT("MissingFace");
	TestNull(TEXT("An unresolved explicit selection must not fall back to an unrelated face"), UStoryFlowMetaHumanBakeSubsystem::FindFace(Setup, Issue));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowBakeOptionalBackendTest, "StoryFlow.MetaHuman.Setup.OptionalBackendIsLazy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowBakeOptionalBackendTest::RunTest(const FString& Parameters)
{
	const bool bLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("MetaHumanPerformance"));
	const FString Reason = StoryFlowMetaHumanBakeBridge::UnavailableReason();
	TestEqual(TEXT("Checking bake availability never loads Epic's solver"), FModuleManager::Get().IsModuleLoaded(TEXT("MetaHumanPerformance")), bLoaded);
	if (!Reason.IsEmpty())
	{
		FString Error;
		TestNull(TEXT("Unavailable baking cannot create a solver object"), StoryFlowMetaHumanBakeBridge::CreatePerformance(GetTransientPackage(), nullptr, nullptr, nullptr, NAME_None, Error));
		TestFalse(TEXT("Unavailable baking has an actionable explanation"), Error.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowBakeBackendSchemaTest, "StoryFlow.MetaHuman.Setup.InstalledBackendSchema", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowBakeBackendSchemaTest::RunTest(const FString& Parameters)
{
	const FString Unavailable = StoryFlowMetaHumanBakeBridge::UnavailableReason();
	if (!Unavailable.IsEmpty()) { AddInfo(Unavailable); return true; }
	FString Error;
	TestTrue(TEXT("The installed MetaHuman backend supports the complete reflected audio bake and export contract"), StoryFlowMetaHumanBakeBridge::ValidateSchema(Error));
	if (!Error.IsEmpty()) AddError(Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowBakeRigFingerprintTest, "StoryFlow.MetaHuman.Setup.ChangedMeshInvalidatesBake", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowBakeRigFingerprintTest::RunTest(const FString& Parameters)
{
	auto* Voice = NewObject<USoundWave>();
	auto* Face = NewObject<USkeletalMesh>();
	Face->SetSkeleton(NewObject<USkeleton>());
	if (!TestNotNull(TEXT("Imported mesh model exists"), Face->GetImportedModel())) return false;
	const FString Before = UStoryFlowMetaHumanBakeSubsystem::Fingerprint(Voice, Face);
	TestFalse(TEXT("The initial recipe has a fingerprint"), Before.IsEmpty());
	Face->GetImportedModel()->GenerateNewGUID();
	TestNotEqual(TEXT("Replacing mesh data at the same asset path invalidates its cached animation"), UStoryFlowMetaHumanBakeSubsystem::Fingerprint(Voice, Face), Before);
	return true;
}
#endif
