// Copyright 2026 StoryFlow. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/MemStack.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncComponent.h"
#include "MetaHuman/StoryFlowMetaHumanAnimInstance.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncLibrary.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "StoryFlowScopedWorld.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Lipsync/StoryFlowVisemeMap.h"

namespace StoryFlowMetaHumanTestAccess
{
	template<class Tag, typename Tag::Type Member> struct Grant { friend typename Tag::Type Get(Tag) { return Member; } };
	template<class Tag, class Component> decltype(auto) Field(Component* C) { return C->*Get(Tag{}); }
	template<class Tag, class Component, class... Args> decltype(auto) Call(Component* C, Args&&... A) { return (C->*Get(Tag{}))(Forward<Args>(A)...); }
	struct ResolveFace { using Type = void (UStoryFlowMetaHumanLipsyncComponent::*)(const StoryFlowVisemeTable::FTable&); friend Type Get(ResolveFace); }; template struct Grant<ResolveFace, &UStoryFlowMetaHumanLipsyncComponent::ResolveFace>;
	struct Tick { using Type = void (UStoryFlowMetaHumanLipsyncComponent::*)(float, ELevelTick, FActorComponentTickFunction*); friend Type Get(Tick); }; template struct Grant<Tick, &UStoryFlowMetaHumanLipsyncComponent::TickComponent>;
	struct LineAudio { using Type = TWeakObjectPtr<UAudioComponent> UStoryFlowLipsyncComponent::*; friend Type Get(LineAudio); }; template struct Grant<LineAudio, &UStoryFlowLipsyncComponent::LineAudio>;
	struct DialogueAudio { using Type = TObjectPtr<UAudioComponent> UStoryFlowComponent::*; friend Type Get(DialogueAudio); }; template struct Grant<DialogueAudio, &UStoryFlowComponent::CurrentDialogueAudio>;
	struct OwnedSubmix { using Type = TObjectPtr<USoundSubmix> UStoryFlowMetaHumanLipsyncComponent::*; friend Type Get(OwnedSubmix); }; template struct Grant<OwnedSubmix, &UStoryFlowMetaHumanLipsyncComponent::OwnedAnalysisSubmix>;
	struct OwnsSubmix { using Type = bool UStoryFlowMetaHumanLipsyncComponent::*; friend Type Get(OwnsSubmix); }; template struct Grant<OwnsSubmix, &UStoryFlowMetaHumanLipsyncComponent::bOwnAnalysisSubmix>;
	struct RoutedAudio { using Type = TWeakObjectPtr<UAudioComponent> UStoryFlowMetaHumanLipsyncComponent::*; friend Type Get(RoutedAudio); }; template struct Grant<RoutedAudio, &UStoryFlowMetaHumanLipsyncComponent::RoutedAudio>;
	struct RoutedPlayOrder { using Type = uint32 UStoryFlowMetaHumanLipsyncComponent::*; friend Type Get(RoutedPlayOrder); }; template struct Grant<RoutedPlayOrder, &UStoryFlowMetaHumanLipsyncComponent::RoutedPlayOrder>;
	struct AudioPlayOrder { using Type = uint32 UAudioComponent::*; friend Type Get(AudioPlayOrder); }; template struct Grant<AudioPlayOrder, &UAudioComponent::LastSoundPlayOrder>;
	struct PlaybackHandle { using Type = FDelegateHandle UStoryFlowMetaHumanLipsyncComponent::*; friend Type Get(PlaybackHandle); }; template struct Grant<PlaybackHandle, &UStoryFlowMetaHumanLipsyncComponent::PlaybackPercentHandle>;
	struct Deactivate { using Type = void (UStoryFlowMetaHumanLipsyncComponent::*)(); friend Type Get(Deactivate); }; template struct Grant<Deactivate, &UStoryFlowMetaHumanLipsyncComponent::Deactivate>;
	struct ResolvedTable { using Type = StoryFlowVisemeTable::FTable UStoryFlowLipsyncComponent::*; friend Type Get(ResolvedTable); }; template struct Grant<ResolvedTable, &UStoryFlowLipsyncComponent::ResolvedTable>;
}

namespace
{
USkeletalMesh* CreateMetaHumanTestMesh()
{
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>();
	USkeleton* Skeleton = NewObject<USkeleton>();
	{
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), Skeleton);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("FACIAL_C_FacialRoot"), TEXT("FACIAL_C_FacialRoot"), 0), FTransform::Identity);
	}
		Skeleton->MergeAllBonesToBoneTree(Mesh);
	Mesh->SetSkeleton(Skeleton);
	return Mesh;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanControlsTest, "StoryFlow.MetaHuman.ControlsReleaseAndClamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanControlsTest::RunTest(const FString& Parameters)
{
	const auto Speaking = UStoryFlowMetaHumanLipsyncComponent::MapRigControls({
		{TEXT("jawOpen"), 2.f}, {TEXT("mouthFunnel"), .75f}, {TEXT("mouthPucker"), .4f}, {TEXT("mouthStretchLeft"), -.5f}});
	TestEqual(TEXT("Jaw input cannot exceed the rig range"), Speaking.FindRef(TEXT("CTRL_expressions_jawOpen")), 1.f);
	TestEqual(TEXT("Negative input cannot invert lips"), Speaking.FindRef(TEXT("CTRL_expressions_mouthStretchL")), 0.f);
	const auto LipOpening = UStoryFlowMetaHumanLipsyncComponent::MapRigControls({
		{TEXT("mouthLowerDownLeft"), .6f}, {TEXT("mouthUpperUpRight"), .4f}});
	TestEqual(TEXT("Open vowels depress the lower lip on MetaHuman"), LipOpening.FindRef(TEXT("CTRL_expressions_mouthLowerLipDepressL")), .6f);
	TestEqual(TEXT("Upper lip raising reaches the MetaHuman rig"), LipOpening.FindRef(TEXT("CTRL_expressions_mouthUpperLipRaiseR")), .4f);
	for (const TCHAR* Suffix : {TEXT("UL"), TEXT("UR"), TEXT("DL"), TEXT("DR")})
	{
		TestEqual(TEXT("Round vowels move all four lip quadrants"), Speaking.FindRef(FName(FString(TEXT("CTRL_expressions_mouthFunnel")) + Suffix)), .75f);
	}
	const auto Rest = UStoryFlowMetaHumanLipsyncComponent::MapRigControls({});
	for (const auto& Pair : Speaking)
	{
		TestTrue(TEXT("Rest must explicitly release each previously written control"), Rest.Contains(Pair.Key));
		TestEqual(TEXT("Rest releases the control"), Rest.FindRef(Pair.Key), 0.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanBakedCurvesTest, "StoryFlow.MetaHuman.BakedCurvesPreserveFaceAnimation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanBakedCurvesTest::RunTest(const FString& Parameters)
{
#if WITH_EDITOR
	UAnimSequence* Sequence = NewObject<UAnimSequence>();
	Sequence->SetSkeleton(CreateMetaHumanTestMesh()->GetSkeleton());
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	Controller.OpenBracket(FText::FromString(TEXT("Speech curve fixture")), false);
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	for (const TCHAR* Name : {TEXT("CTRL_expressions_jawOpen"), TEXT("CTRL_expressions_mouthFunnelUL"),
		TEXT("CTRL_expressions_teethUpU"), TEXT("CTRL_expressions_tongueUp"), TEXT("CTRL_expressions_neckThroatDown"),
		TEXT("CTRL_expressions_noseWrinkleL"), TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeLookLeftL"),
		TEXT("CTRL_expressions_browRaiseInL"), TEXT("HeadYaw"), TEXT("HeadControlSwitch")})
	{
		const FAnimationCurveIdentifier Id(FName(Name), ERawCurveTrackTypes::RCT_Float);
		Controller.AddCurve(Id, AACF_Editable, false);
		Controller.SetCurveKeys(Id, {FRichCurveKey(0.f, 0.f), FRichCurveKey(1.f, 1.f)}, false);
	}
	// Finish initial population before closing the bracket: UE 5.3's sequencer
	// model refreshes its FK rig's active controls only for a populated model.
	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
	// Curve edits enqueue compression. Complete it while this valid fixture is
	// still in scope so no background work escapes into later tests or shutdown.
	Sequence->WaitOnExistingCompression();

	FMemMark Mark(FMemStack::Get());
	FBlendedCurve Output;
	Output.Set(TEXT("CTRL_expressions_jawOpen"), .8f);
	Output.Set(TEXT("CTRL_expressions_eyeBlinkL"), .7f);
	Output.Set(TEXT("CTRL_expressions_eyeLookLeftL"), .6f);
	Output.Set(TEXT("CTRL_expressions_browRaiseInL"), .5f);
	Output.Set(TEXT("HeadYaw"), 17.f);
	Output.Set(TEXT("HeadControlSwitch"), 1.f);
	Output.Set(TEXT("ExistingMaterialCurve"), .4f);
	UStoryFlowMetaHumanAnimInstance::ApplyBakedSpeechCurves(*Sequence, .25f, Output);
	TestEqual(TEXT("Speech samples the requested playback position"), Output.Get(TEXT("CTRL_expressions_jawOpen")), .25f, .001f);
	TestEqual(TEXT("Lips receive baked speech"), Output.Get(TEXT("CTRL_expressions_mouthFunnelUL")), .25f, .001f);
	TestEqual(TEXT("Teeth receive baked speech"), Output.Get(TEXT("CTRL_expressions_teethUpU")), .25f, .001f);
	TestEqual(TEXT("Tongue receives baked speech"), Output.Get(TEXT("CTRL_expressions_tongueUp")), .25f, .001f);
	TestEqual(TEXT("Speech neck motion is retained"), Output.Get(TEXT("CTRL_expressions_neckThroatDown")), .25f, .001f);
	TestEqual(TEXT("Speech nose motion is retained"), Output.Get(TEXT("CTRL_expressions_noseWrinkleL")), .25f, .001f);
	TestEqual(TEXT("Baked blinks do not replace existing blinks"), Output.Get(TEXT("CTRL_expressions_eyeBlinkL")), .7f);
	TestEqual(TEXT("Baked gaze does not replace existing gaze"), Output.Get(TEXT("CTRL_expressions_eyeLookLeftL")), .6f);
	TestEqual(TEXT("Baked brows do not replace existing expression"), Output.Get(TEXT("CTRL_expressions_browRaiseInL")), .5f);
	TestEqual(TEXT("Exported head rotation does not replace head motion"), Output.Get(TEXT("HeadYaw")), 17.f);
	TestEqual(TEXT("Exported head switch does not disable the existing controller"), Output.Get(TEXT("HeadControlSwitch")), 1.f);
	TestEqual(TEXT("Curves absent from the bake are preserved"), Output.Get(TEXT("ExistingMaterialCurve")), .4f);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanCompatibilityTest, "StoryFlow.MetaHuman.RuntimeFaceCompatibility", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanCompatibilityTest::RunTest(const FString& Parameters)
{
	USkeletalMeshComponent* Face = NewObject<USkeletalMeshComponent>();
	Face->SetSkeletalMeshAsset(CreateMetaHumanTestMesh());
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 5
	Face->SetOverridePostProcessAnimBP(UAnimInstance::StaticClass(), false);
#else
	Face->GetSkeletalMeshAsset()->SetPostProcessAnimBlueprint(UAnimInstance::StaticClass());
#endif
	FString Reason;
	TestTrue(TEXT("Effective component postprocess supports a face without a main graph"), UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(Face, Reason));
	TestTrue(TEXT("Compatible face has no error"), Reason.IsEmpty());
	Face->SetAnimInstanceClass(UAnimInstance::StaticClass());
	TestFalse(TEXT("A custom facial graph is refused"), UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(Face, Reason));
	TestFalse(TEXT("A refusal explains the custom setup"), Reason.IsEmpty());
	TestEqual(TEXT("Checking compatibility preserves the custom class"), Face->GetAnimClass(), UAnimInstance::StaticClass());
	Face->SetAnimInstanceClass(nullptr);
	for (const auto Mode : {EAnimationMode::AnimationSingleNode, EAnimationMode::AnimationCustomMode})
	{
		Face->SetAnimationMode(Mode, false);
		TestFalse(TEXT("An externally controlled animation mode is refused"), UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(Face, Reason));
		TestEqual(TEXT("Checking compatibility preserves the animation mode"), Face->GetAnimationMode(), Mode);
	}
	Face->SetAnimationMode(EAnimationMode::AnimationBlueprint, false);
	USkeletalMeshComponent* Leader = NewObject<USkeletalMeshComponent>();
	Face->SetLeaderPoseComponent(Leader);
	TestFalse(TEXT("A leader pose cannot be replaced by the speech driver"), UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(Face, Reason));
	TestTrue(TEXT("Checking compatibility preserves the leader"), Face->LeaderPoseComponent.Get() == Leader);
	Face->SetLeaderPoseComponent(nullptr);
	Face->SetDisablePostProcessBlueprint(true);
	TestFalse(TEXT("Disabled postprocess cannot evaluate the speech controls"), UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(Face, Reason));
	TestTrue(TEXT("Checking compatibility does not enable postprocess"), Face->GetDisablePostProcessBlueprint());
	Face->SetDisablePostProcessBlueprint(false);
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 5
	Face->SetOverridePostProcessAnimBP(nullptr, false);
#else
	Face->GetSkeletalMeshAsset()->SetPostProcessAnimBlueprint(nullptr);
#endif
	TestFalse(TEXT("Missing effective postprocess is refused"), UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(Face, Reason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanDefaultsTest, "StoryFlow.MetaHuman.IndependentProceduralProfile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowMetaHumanTestAccess;
	const auto* Component = GetDefault<UStoryFlowMetaHumanLipsyncComponent>();
	TestTrue(TEXT("MetaHuman analyses source audio before volume"), Component->bAnalyzeVoiceBeforeVolume);
	TestTrue(TEXT("MetaHuman uses its spectral articulation profile"), Component->bSpectralArticulation);
	TestFalse(TEXT("MetaHuman does not inherit Sidekick's jaw-relative closure"), Component->bJawRelativeClosure);
	TestFalse(TEXT("Silent MetaHuman dialogue does not invent mouth motion"), Component->bIdleMouthWithoutAudio);
	TestEqual(TEXT("MetaHuman speech strength"), Component->Strength, .65f);
	TestEqual(TEXT("MetaHuman speech sensitivity"), Component->Sensitivity, .9f);
	TestEqual(TEXT("MetaHuman jaw response"), Component->JawBias, 1.2f);
	TestEqual(TEXT("MetaHuman smoothing"), Component->Smoothing, 14.f);
	TestEqual(TEXT("MetaHuman vowel calibration"), Component->VowelScale, 4.f);
	TestEqual(TEXT("MetaHuman vowel offset"), Component->VowelOffset, -1.1f);
	TestEqual(TEXT("MetaHuman spectral contrast"), Component->SpectralContrast, 2.f);

	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto AddComponent = [&W]() {
		auto* Instance = NewObject<UStoryFlowMetaHumanLipsyncComponent>(W.Component->GetOwner());
		Instance->Source = W.Component;
		Instance->CharacterId = TEXT("test-character");
		Instance->AnalysisSubmix = NewObject<USoundSubmix>(Instance);
		Instance->VisemeMap = NewObject<UStoryFlowVisemeMap>(Instance);
		Instance->RegisterComponent();
		return Instance;
	};
	auto* EmptyMapping = AddComponent();
	auto* AuthoredMapping = AddComponent();
	for (const FName PoseName : {FName(TEXT("OO")), FName(TEXT("OH")), FName(TEXT("AA")), FName(TEXT("EE"))})
	{
		auto& Pose = AuthoredMapping->VisemeMap->Poses.AddDefaulted_GetRef();
		Pose.Pose = PoseName;
		auto& Morph = Pose.Morphs.AddDefaulted_GetRef();
		Morph.Name = TEXT("jawOpen");
		Morph.Weight = .11f;
	}
	AddExpectedError(TEXT("no assembled facial mesh"), EAutomationExpectedErrorFlags::Contains, 2);
	W.Component->GetOwner()->DispatchBeginPlay();
	const auto& Table = Field<ResolvedTable>(EmptyMapping);
	TestEqual(TEXT("The independent MetaHuman table includes every calibrated pose"), Table.Num(), 7);
	TestTrue(TEXT("The MetaHuman table includes an open vowel"), Table.Contains(TEXT("AA")));
	const auto Open = UStoryFlowMetaHumanLipsyncComponent::MapRigControls(Table.FindRef(TEXT("AA")));
	TestEqual(TEXT("Default open vowel moves the lower lips on MetaHuman"), Open.FindRef(TEXT("CTRL_expressions_mouthLowerLipDepressL")), .7f);
	TestEqual(TEXT("Default open vowel retains the calibrated jaw opening"), Open.FindRef(TEXT("CTRL_expressions_jawOpen")), .75f);
	TestTrue(TEXT("The MetaHuman table includes a closed consonant"), Table.Contains(TEXT("MM")));
	const auto Closed = UStoryFlowMetaHumanLipsyncComponent::MapRigControls(Table.FindRef(TEXT("MM")));
	TestEqual(TEXT("Default closed consonant seals the MetaHuman lips"), Closed.FindRef(TEXT("CTRL_expressions_mouthLipsTogetherUL")), 1.f);
	TestEqual(TEXT("An assigned empty mapping retains MetaHuman's open-vowel lip profile"),
		Field<ResolvedTable>(EmptyMapping).FindRef(TEXT("AA")).FindRef(TEXT("mouthLowerDownLeft")), .7f);
	TestEqual(TEXT("An authored mapping overrides the MetaHuman profile"),
		Field<ResolvedTable>(AuthoredMapping).FindRef(TEXT("AA")).FindRef(TEXT("jawOpen")), .11f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanAmbiguousFaceTest, "StoryFlow.MetaHuman.AmbiguousFaceReleasesPreviousBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanAmbiguousFaceTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowMetaHumanTestAccess;
	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	AActor* Owner = W.Component->GetOwner();
	auto* Component = NewObject<UStoryFlowMetaHumanLipsyncComponent>(Owner);
	auto AddFace = [Owner]() {
		auto* Face = NewObject<USkeletalMeshComponent>(Owner);
		Owner->AddInstanceComponent(Face);
		Face->SetSkeletalMeshAsset(CreateMetaHumanTestMesh());
		Face->GetSkeletalMeshAsset()->SetPostProcessAnimBlueprint(UAnimInstance::StaticClass());
		return Face;
	};
	auto* FirstFace = AddFace();
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	TestTrue(TEXT("The only compatible face is bound"), Component->GetDrivenFace() == FirstFace);
	TestEqual(TEXT("The speech animation class owns that face"), FirstFace->GetAnimClass(), UStoryFlowMetaHumanAnimInstance::StaticClass());
	AddFace();
	AddExpectedError(TEXT("multiple faces"), EAutomationExpectedErrorFlags::Contains, 1);
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	TestNull(TEXT("An ambiguous selection releases the previously driven face"), Component->GetDrivenFace());
	TestNull(TEXT("Releasing the face restores its original animation class"), FirstFace->GetAnimClass());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanInvalidFaceRootTest, "StoryFlow.MetaHuman.InvalidFaceRootReleasesPreviousBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanInvalidFaceRootTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowMetaHumanTestAccess;
	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	AActor* Owner = W.Component->GetOwner();
	auto* Component = NewObject<UStoryFlowMetaHumanLipsyncComponent>(Owner);
	auto* Face = NewObject<USkeletalMeshComponent>(Owner);
	Owner->AddInstanceComponent(Face);
	Face->SetSkeletalMeshAsset(CreateMetaHumanTestMesh());
	Face->GetSkeletalMeshAsset()->SetPostProcessAnimBlueprint(UAnimInstance::StaticClass());
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	TestTrue(TEXT("The compatible face is initially bound"), Component->GetDrivenFace() == Face);
	Component->FaceRoot.ComponentProperty = TEXT("MissingFaceRoot");
	AddExpectedError(TEXT("selected FaceRoot could not be resolved on"), EAutomationExpectedErrorFlags::Contains, 2);
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	TestNull(TEXT("An unresolved explicit root releases the previously driven face"), Component->GetDrivenFace());
	TestNull(TEXT("An unresolved root restores the original animation class"), Face->GetAnimClass());
	Component->FaceRoot = FComponentReference();
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	TestTrue(TEXT("Clearing the invalid selection permits automatic binding again"), Component->GetDrivenFace() == Face);
	AActor* OtherOwner = W.World->SpawnActor<AActor>();
	Component->FaceRoot.OverrideComponent = NewObject<USceneComponent>(OtherOwner);
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	Call<ResolveFace>(Component, StoryFlowVisemeTable::FTable{});
	TestNull(TEXT("A root owned by another actor releases the previously driven face"), Component->GetDrivenFace());
	TestNull(TEXT("A foreign root restores the original animation class"), Face->GetAnimClass());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanRetainedTailTest, "StoryFlow.MetaHuman.AnalysisRoutesOwnedAudioTail", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanRetainedTailTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowMetaHumanTestAccess;
	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Component = NewObject<UStoryFlowMetaHumanLipsyncComponent>(W.Component->GetOwner());
	Component->Source = W.Component;
	Component->RegisterComponent();
	Component->SetActive(true, true);
	auto* Tail = NewObject<UAudioComponent>();
	auto* NewSpeaker = NewObject<UAudioComponent>();
	Field<AudioPlayOrder>(Tail) = 17;
	Field<DialogueAudio>(W.Component) = NewSpeaker;
	Field<LineAudio>(Component) = Tail;
	Field<OwnedSubmix>(Component) = NewObject<USoundSubmix>(Component);
	Field<OwnsSubmix>(Component) = true;
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Analysis stays on this speaker's retained tail"), Field<RoutedAudio>(Component).Get() == Tail);
	TestEqual(TEXT("Analysis targets the owned tail's current playback"), Field<RoutedPlayOrder>(Component), uint32(17));
	TestTrue(TEXT("The new speaker's sound is not claimed"), Field<RoutedAudio>(Component).Get() != NewSpeaker);
	Field<LineAudio>(Component).Reset();
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	TestFalse(TEXT("Routing is released after the tracked tail ends"), Field<RoutedAudio>(Component).IsValid());
	TestEqual(TEXT("Finished routing clears the old playback generation"), Field<RoutedPlayOrder>(Component), uint32(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanLibraryRoutingTest, "StoryFlow.MetaHuman.LibraryRoutingAndStaleFallback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanLibraryRoutingTest::RunTest(const FString& Parameters)
{
	auto* Library = NewObject<UStoryFlowMetaHumanLipsyncLibrary>();
	auto* Voice = NewObject<USoundWave>();
	auto* Face = CreateMetaHumanTestMesh();
	auto* Animation = NewObject<UAnimSequence>();
	auto& Entry = Library->Entries.AddDefaulted_GetRef();
	Entry.Voice = Voice; Entry.FaceMesh = Face; Entry.Animation = Animation;
	TestTrue(TEXT("Bake selection matches both the voice and face"), Library->FindAnimation(Voice, Face) == Animation);
	TestNull(TEXT("A different voice uses live fallback"), Library->FindAnimation(NewObject<USoundWave>(), Face));
	TestNull(TEXT("A different face uses live fallback"), Library->FindAnimation(Voice, CreateMetaHumanTestMesh()));
	Entry.bStale = true;
	TestNull(TEXT("A stale bake uses live fallback"), Library->FindAnimation(Voice, Face));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanPlaybackTimeTest, "StoryFlow.MetaHuman.BakedPlaybackFollowsAudioPosition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanPlaybackTimeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowMetaHumanTestAccess;
	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Component = NewObject<UStoryFlowMetaHumanLipsyncComponent>(W.Component->GetOwner());
	Component->BakeLibrary = NewObject<UStoryFlowMetaHumanLipsyncLibrary>();
	Component->RegisterComponent();
	Component->SetActive(true, true);
	auto* Audio = NewObject<UAudioComponent>();
	auto* Voice = NewObject<USoundWave>();
	Voice->Duration = 10.f;
	Audio->SetSound(Voice);
	Field<LineAudio>(Component) = Audio;
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, 1.f);
	TestEqual(TEXT("An uninitialized mixer endpoint does not jump to the last pose"), Component->GetBakedPlaybackTime(), 0.f);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, .25f);
	TestEqual(TEXT("The baked pose follows the audio sample position"), Component->GetBakedPlaybackTime(), 2.5f);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, .1f);
	TestEqual(TEXT("A restart or seek moves the bake back with its audio"), Component->GetBakedPlaybackTime(), 1.f);
	Voice->bLooping = true;
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, 1.25f);
	TestEqual(TEXT("A native loop's cumulative percent wraps within the voice"), Component->GetBakedPlaybackTime(), 2.5f);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, NewObject<USoundWave>(), .9f);
	TestEqual(TEXT("A notification from another wave cannot change the pose"), Component->GetBakedPlaybackTime(), 2.5f);
	Field<LineAudio>(Component).Reset();
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Releasing the owned voice resets the playback position"), Component->GetBakedPlaybackTime(), 0.f);
	TestFalse(TEXT("Released playback no longer owns an audio-position delegate"), Audio->OnAudioPlaybackPercentNative.IsBoundToObject(Component));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMetaHumanReplayGenerationTest, "StoryFlow.MetaHuman.ReplayRefreshesRoutingAndBakedPosition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMetaHumanReplayGenerationTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowMetaHumanTestAccess;
	StoryFlowTestWorld::FScopedWorld W;
	if (!W.Init()) return false;
	auto* Component = NewObject<UStoryFlowMetaHumanLipsyncComponent>(W.Component->GetOwner());
	Component->BakeLibrary = NewObject<UStoryFlowMetaHumanLipsyncLibrary>();
	Component->RegisterComponent();
	Component->SetActive(true, true);
	auto* Audio = NewObject<UAudioComponent>();
	auto* Voice = NewObject<USoundWave>();
	Voice->Duration = 10.f;
	Audio->SetSound(Voice);
	Field<AudioPlayOrder>(Audio) = 41;
	Field<LineAudio>(Component) = Audio;
	Field<OwnedSubmix>(Component) = NewObject<USoundSubmix>(Component);
	Field<OwnsSubmix>(Component) = true;
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, .5f);
	const FDelegateHandle InitialHandle = Field<PlaybackHandle>(Component);
	Audio->SetPaused(true);
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Pausing retains the current baked position"), Component->GetBakedPlaybackTime(), 5.f);
	TestTrue(TEXT("Pausing retains the same playback binding"), Field<PlaybackHandle>(Component) == InitialHandle);
	Audio->SetPaused(false);
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Resuming does not restart the bake"), Component->GetBakedPlaybackTime(), 5.f);
	// PlayInternal updates this field for a replay; no audio hardware is needed for this fixture.
	Field<AudioPlayOrder>(Audio) = 42;
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, 1.f);
	TestEqual(TEXT("An old binding ignores a notification after its component restarts"), Component->GetBakedPlaybackTime(), 5.f);
	Call<Tick>(Component, 1.f / 60.f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Same-component replay refreshes the submix target"), Field<RoutedPlayOrder>(Component), uint32(42));
	TestEqual(TEXT("Same-component replay resets the baked position"), Component->GetBakedPlaybackTime(), 0.f);
	TestTrue(TEXT("Replay installs a new playback-percent binding"), Field<PlaybackHandle>(Component) != InitialHandle);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, 1.f);
	TestEqual(TEXT("Each new playback rejects the mixer's initial endpoint"), Component->GetBakedPlaybackTime(), 0.f);
	Audio->OnAudioPlaybackPercentNative.Broadcast(Audio, Voice, .1f);
	TestEqual(TEXT("The replay accepts its first valid position"), Component->GetBakedPlaybackTime(), 1.f);
	Call<Deactivate>(Component);
	TestEqual(TEXT("Deactivation clears baked playback position"), Component->GetBakedPlaybackTime(), 0.f);
	TestFalse(TEXT("Deactivation releases the playback-percent binding"), Audio->OnAudioPlaybackPercentNative.IsBoundToObject(Component));
	TestFalse(TEXT("Deactivation releases audio routing"), Field<RoutedAudio>(Component).IsValid());
	TestEqual(TEXT("Deactivation clears the routed playback generation"), Field<RoutedPlayOrder>(Component), uint32(0));
	return true;
}
#endif
