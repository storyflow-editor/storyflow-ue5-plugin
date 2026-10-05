// Copyright 2026 StoryFlow. All Rights Reserved.

#include "MetaHuman/StoryFlowMetaHumanLipsyncComponent.h"
#include "MetaHuman/StoryFlowMetaHumanAnimInstance.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Sound/SoundSubmix.h"
#include "GameFramework/Actor.h"
#include "AudioDevice.h"
#include "AudioDeviceHandle.h"
#include "AudioThread.h"
#include "Engine/World.h"
#include "Animation/AnimSequence.h"
#include "Sound/SoundWave.h"
#include "ActiveSound.h"
#include "Runtime/Launch/Resources/Version.h"
#include "StoryFlowRuntime.h"

namespace
{
void SendMetaHumanAnalysisSubmix(UAudioComponent* Audio, USoundSubmix* Submix, uint32 PlayOrder, float Level)
{
	if (!Audio || !Submix) return;
	if (FAudioDevice* Device = Audio->GetAudioDevice())
	{
		FSoundSubmixSendInfo Send;
		Send.SoundSubmix = Submix;
		Send.SendLevel = Level;
		// The component can replay before this audio-thread command runs. Apply the
		// send only to the playback we own, including when releasing an older send.
		Device->SendCommandToActiveSounds(Audio->GetAudioComponentID(), [PlayOrder, Send](FActiveSound& Sound) {
			if (Sound.GetPlayOrder() == PlayOrder) Sound.SetSubmixSend(Send);
		});
	}
}
}

UStoryFlowMetaHumanLipsyncComponent::UStoryFlowMetaHumanLipsyncComponent()
{
	Strength = .65f;
	Sensitivity = .9f;
	JawBias = 1.2f;
	Smoothing = 14.f;
	AnalysisFullScale = 32.f;
	VowelScale = 4.f;
	VowelOffset = -1.1f;
	SpectralContrast = 2.f;
	bSpectralArticulation = true;
	ArticulationBlend = 1.f;
	bAnalyzeVoiceBeforeVolume = true;
	bJawRelativeClosure = false;
	bIdleMouthWithoutAudio = false;
}

StoryFlowVisemeTable::FTable UStoryFlowMetaHumanLipsyncComponent::GetDefaultVisemeTable() const
{
	// MetaHuman's lip controls need their own profile; the base component targets ARKit morphs.
	return {
		{TEXT("rest"), {}},
		{TEXT("OO"), {{TEXT("jawOpen"), .23f}, {TEXT("mouthFunnel"), .5f}, {TEXT("mouthPucker"), .95f},
			{TEXT("mouthLowerDownLeft"), .03f}, {TEXT("mouthUpperUpLeft"), .035f},
			{TEXT("mouthLowerDownRight"), .03f}, {TEXT("mouthUpperUpRight"), .035f}}},
		{TEXT("OH"), {{TEXT("jawOpen"), .58f}, {TEXT("mouthFunnel"), .6f}, {TEXT("mouthPucker"), .6f},
			{TEXT("mouthLowerDownLeft"), .12f}, {TEXT("mouthUpperUpLeft"), .05f},
			{TEXT("mouthLowerDownRight"), .12f}, {TEXT("mouthUpperUpRight"), .05f}}},
		{TEXT("AA"), {{TEXT("jawOpen"), .75f}, {TEXT("mouthFunnel"), .12f}, {TEXT("mouthPucker"), .08f},
			{TEXT("mouthLowerDownLeft"), .7f}, {TEXT("mouthUpperUpLeft"), .08f}, {TEXT("mouthStretchLeft"), .18f},
			{TEXT("mouthLowerDownRight"), .7f}, {TEXT("mouthUpperUpRight"), .08f}, {TEXT("mouthStretchRight"), .18f}}},
		{TEXT("EE"), {{TEXT("jawOpen"), .3f}, {TEXT("mouthFunnel"), .05f},
			{TEXT("mouthLowerDownLeft"), .15f}, {TEXT("mouthUpperUpLeft"), .06f}, {TEXT("mouthStretchLeft"), .65f}, {TEXT("mouthSmileLeft"), .08f},
			{TEXT("mouthLowerDownRight"), .15f}, {TEXT("mouthUpperUpRight"), .06f}, {TEXT("mouthStretchRight"), .65f}, {TEXT("mouthSmileRight"), .08f}}},
		{TEXT("MM"), {{TEXT("jawOpen"), .3f}, {TEXT("mouthClose"), 1.f}, {TEXT("mouthPressLeft"), .12f}, {TEXT("mouthPressRight"), .12f}}},
		{TEXT("SS"), {{TEXT("jawOpen"), .18f}, {TEXT("mouthStretchLeft"), .3f}, {TEXT("mouthStretchRight"), .3f},
			{TEXT("mouthLowerDownLeft"), .035f}, {TEXT("mouthLowerDownRight"), .035f}}}
	};
}

USoundSubmix* UStoryFlowMetaHumanLipsyncComponent::GetAnalysisSubmix() const
{
	return OwnedAnalysisSubmix ? OwnedAnalysisSubmix.Get() : AnalysisSubmix.Get();
}

void UStoryFlowMetaHumanLipsyncComponent::BeginPlay()
{
	if (!AnalysisSubmix && GetNetMode() != NM_DedicatedServer)
	{
		// Never put this runtime object in editable AnalysisSubmix. Blueprint
		// reconstruction copies that property and auto-registers duplicate submixes.
		OwnedAnalysisSubmix = NewObject<USoundSubmix>(this, NAME_None, RF_Transient | RF_DuplicateTransient);
		OwnedAnalysisSubmix->bAutoDisable = false;
		OwnedAnalysisSubmix->OutputVolumeModulation.Value = -96.f;
		if (FAudioDeviceHandle Device = GetWorld()->GetAudioDevice(); Device.IsValid())
			Device->RegisterSoundSubmix(OwnedAnalysisSubmix, true);
		bOwnAnalysisSubmix = true;
	}
	Super::BeginPlay();
}

void UStoryFlowMetaHumanLipsyncComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
	if (!IsActive())
	{
		Super::TickComponent(DeltaTime, TickType, TickFunction);
		return;
	}
	TrackPlayback(BakeLibrary || !BakedAnimations.IsEmpty() ? GetTrackedDialogueAudio() : nullptr);
	if (bOwnAnalysisSubmix)
	{
		UAudioComponent* Audio = GetTrackedDialogueAudio();
		const uint32 PlayOrder = Audio ? Audio->GetLastPlayOrder() : 0;
		if (Audio != RoutedAudio.Get() || PlayOrder != RoutedPlayOrder)
		{
			ReleaseRouting();
			RoutedAudio = Audio;
			RoutedPlayOrder = PlayOrder;
			if (Audio)
			{
				SendMetaHumanAnalysisSubmix(Audio, OwnedAnalysisSubmix, PlayOrder, 1.f);
				// Keep normal dialogue playback audible; this auxiliary send is analysis only.
				OwnedAnalysisSubmix->SetSubmixOutputVolume(this, 0.f);
			}
		}
	}
	Super::TickComponent(DeltaTime, TickType, TickFunction);
}

void UStoryFlowMetaHumanLipsyncComponent::TrackPlayback(UAudioComponent* Audio)
{
	const uint32 PlayOrder = Audio ? Audio->GetLastPlayOrder() : 0;
	if (Audio == PlaybackAudio.Get() && PlayOrder == PlaybackPlayOrder
		&& (Audio || !PlaybackPercentHandle.IsValid())) return;
	if (PlaybackAudio.IsValid()) PlaybackAudio->OnAudioPlaybackPercentNative.Remove(PlaybackPercentHandle);
	PlaybackPercentHandle.Reset();
	PlaybackAudio = Audio;
	PlaybackPlayOrder = PlayOrder;
	BakedPlaybackTime = 0.f;
	bHasPlaybackPosition = false;
	if (!Audio) return;
	PlaybackPercentHandle = Audio->OnAudioPlaybackPercentNative.AddUObject(this, &UStoryFlowMetaHumanLipsyncComponent::HandlePlaybackPercent);
	// Discovery can follow Play(). A replay reuses the component with a new
	// ActiveSound; pause/resume retains its play order and must retain its position.
	if (FAudioDevice* Device = Audio->GetAudioDevice())
		Device->SendCommandToActiveSounds(Audio->GetAudioComponentID(), [PlayOrder](FActiveSound& Sound) {
			if (Sound.GetPlayOrder() == PlayOrder) Sound.bUpdatePlayPercentage = true;
		});
}

void UStoryFlowMetaHumanLipsyncComponent::HandlePlaybackPercent(const UAudioComponent* Audio, const USoundWave* Wave, float Percent)
{
	if (!Audio || Audio != PlaybackAudio.Get() || Audio->GetLastPlayOrder() != PlaybackPlayOrder
		|| !Wave || Audio->Sound != Wave || !FMath::IsFinite(Percent)) return;
	// The mixer can report 100% before a source initializes. Do not start at the
	// last pose. Thereafter playback percent follows pauses, pitch and loop restarts.
	if (!bHasPlaybackPosition && !Wave->bLooping && Percent >= 1.f) return;
	// A natively looping wave reports cumulative percent, unlike StoryFlow's
	// callback-based replay of a non-looping wave, which restarts at zero.
	const float ClipPercent = Wave->bLooping ? FMath::Fmod(FMath::Max(Percent, 0.f), 1.f) : FMath::Clamp(Percent, 0.f, 1.f);
	BakedPlaybackTime = ClipPercent * Wave->Duration;
	bHasPlaybackPosition = true;
}

void UStoryFlowMetaHumanLipsyncComponent::ReleaseRouting()
{
	if (bOwnAnalysisSubmix)
		SendMetaHumanAnalysisSubmix(RoutedAudio.Get(), OwnedAnalysisSubmix, RoutedPlayOrder, 0.f);
	RoutedAudio.Reset();
	RoutedPlayOrder = 0;
}

void UStoryFlowMetaHumanLipsyncComponent::Deactivate()
{
	TrackPlayback(nullptr);
	ReleaseRouting();
	Super::Deactivate();
}

void UStoryFlowMetaHumanLipsyncComponent::ResolveFace(const StoryFlowVisemeTable::FTable& Table)
{
	if (!GetOwner()) return;
	USkeletalMeshComponent* Found = nullptr;
	USceneComponent* Scope = Cast<USceneComponent>(FaceRoot.GetComponent(GetOwner()));
	const bool bExplicitScope = !FaceRoot.ComponentProperty.IsNone() || !FaceRoot.PathToComponent.IsEmpty()
		|| FaceRoot.OverrideComponent.IsValid() || FaceRoot.OtherActor.IsValid();
	if (bExplicitScope && (!Scope || Scope->GetOwner() != GetOwner()))
	{
		RestoreFace();
		if (!bWarnedFace) UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow Editor MetaHuman: selected FaceRoot could not be resolved on %s; select its facial mesh or parent component."), *GetOwner()->GetName());
		bWarnedFace = true;
		return;
	}
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		if (Scope && Mesh != Scope && !Mesh->IsAttachedTo(Scope)) continue;
		if (Mesh->GetBoneIndex(TEXT("FACIAL_C_FacialRoot")) != INDEX_NONE && Mesh->GetSkeletalMeshAsset())
		{
			if (Found)
			{
				RestoreFace();
				if (!bWarnedFace) UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow Editor MetaHuman: multiple faces on %s; select FaceRoot."), *GetOwner()->GetName());
				bWarnedFace = true;
				return;
			}
			Found = Mesh;
		}
	}
	FString Incompatibility;
	if (Found == Face.Get() && Found && Found->GetAnimClass() == UStoryFlowMetaHumanAnimInstance::StaticClass()
		&& CanDriveFace(Found, Incompatibility)) return;
	RestoreFace();
	if (!Found)
	{
		if (!bWarnedFace) UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow Editor MetaHuman: no assembled facial mesh on %s."), *GetOwner()->GetName());
		bWarnedFace = true;
		return;
	}
	if (!CanDriveFace(Found, Incompatibility))
	{
		if (!bWarnedFace) UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow Editor MetaHuman: cannot bind %s.%s: %s"), *GetOwner()->GetName(), *Found->GetName(), *Incompatibility);
		bWarnedFace = true;
		return;
	}
	bWarnedFace = false;
	Face = Found;
	OriginalAnimClass = Found->GetAnimClass();
	Found->SetAnimInstanceClass(UStoryFlowMetaHumanAnimInstance::StaticClass());
	Found->AddTickPrerequisiteComponent(this);
}

bool UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(USkeletalMeshComponent* Mesh, FString& OutReason)
{
	OutReason.Reset();
	if (!Mesh || !Mesh->GetSkeletalMeshAsset())
		OutReason = TEXT("No facial skeletal mesh is assigned.");
	else if (Mesh->GetAnimationMode() != EAnimationMode::AnimationBlueprint)
		OutReason = TEXT("The face uses single-node or custom animation. Custom integration is required to preserve its existing animation.");
	else if (Mesh->GetAnimClass() && Mesh->GetAnimClass() != UStoryFlowMetaHumanAnimInstance::StaticClass())
		OutReason = FString::Printf(TEXT("AnimClass '%s' already controls the face. Custom integration must apply speech after the existing facial animation and before RigLogic."), *Mesh->GetAnimClass()->GetPathName());
	else if (Mesh->GetAnimInstance() && Mesh->GetAnimInstance()->GetClass() != UStoryFlowMetaHumanAnimInstance::StaticClass())
		OutReason = FString::Printf(TEXT("Animation instance '%s' already controls the face. Custom integration is required to preserve its animation."), *Mesh->GetAnimInstance()->GetClass()->GetPathName());
	else if (Mesh->LeaderPoseComponent.IsValid())
		OutReason = TEXT("The face follows a leader pose component. Custom integration is required before StoryFlow can drive this face.");
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 5
	else if (!Mesh->GetPostProcessAnimBPClassToBeUsed())
#else
	else if (!Mesh->GetSkeletalMeshAsset()->GetPostProcessAnimBlueprint())
#endif
		OutReason = TEXT("The face has no effective postprocess animation Blueprint. An assembled MetaHuman RigLogic postprocess is required.");
	else if (Mesh->GetDisablePostProcessBlueprint())
		OutReason = TEXT("The facial postprocess animation Blueprint is disabled. Enable the assembled MetaHuman RigLogic postprocess before using StoryFlow.");
	return OutReason.IsEmpty();
}

void UStoryFlowMetaHumanLipsyncComponent::RestoreFace()
{
	if (Face.IsValid())
	{
		Face->RemoveTickPrerequisiteComponent(this);
		// An external controller may have changed mode or instance after binding.
		// Only release animation state that this component still owns.
		if (Face->GetAnimationMode() == EAnimationMode::AnimationBlueprint
			&& Face->GetAnimClass() == UStoryFlowMetaHumanAnimInstance::StaticClass()
			&& (!Face->GetAnimInstance() || Face->GetAnimInstance()->GetClass() == UStoryFlowMetaHumanAnimInstance::StaticClass()))
		{
			ZeroOwnedMorphs();
			Face->SetAnimInstanceClass(OriginalAnimClass);
		}
	}
	Face.Reset();
	OriginalAnimClass = nullptr;
}

TMap<FName, float> UStoryFlowMetaHumanLipsyncComponent::MapRigControls(const TMap<FName, float>& Weights)
{
	TMap<FName, float> Controls;
	auto Read = [&](const TCHAR* Name) { return FMath::Clamp(Weights.FindRef(FName(Name)), 0.f, 1.f); };
	auto Set = [&](const TCHAR* Name, float Value) { Controls.Add(FName(FString(TEXT("CTRL_expressions_")) + Name), Value); };
	Set(TEXT("jawOpen"), Read(TEXT("jawOpen")));
	for (const TCHAR* Quadrant : {TEXT("UL"), TEXT("UR"), TEXT("DL"), TEXT("DR")})
	{
		Set(*(FString(TEXT("mouthFunnel")) + Quadrant), Read(TEXT("mouthFunnel")));
		Set(*(FString(TEXT("mouthLipsPurse")) + Quadrant), Read(TEXT("mouthPucker")));
		Set(*(FString(TEXT("mouthLipsTogether")) + Quadrant), Read(TEXT("mouthClose")));
	}
	for (const TCHAR* Side : {TEXT("L"), TEXT("R")})
	{
		const FString Suffix = FString(Side) == TEXT("L") ? TEXT("Left") : TEXT("Right");
		Set(*(FString(TEXT("mouthStretch")) + Side), Read(*(TEXT("mouthStretch") + Suffix)));
		Set(*(FString(TEXT("mouthLowerLipDepress")) + Side), Read(*(TEXT("mouthLowerDown") + Suffix)));
		Set(*(FString(TEXT("mouthUpperLipRaise")) + Side), Read(*(TEXT("mouthUpperUp") + Suffix)));
		Set(*(FString(TEXT("mouthCornerPull")) + Side), Read(*(TEXT("mouthSmile") + Suffix)));
		Set(*(FString(TEXT("mouthLipsPress")) + Side), Read(*(TEXT("mouthPress") + Suffix)));
		Set(*(FString(TEXT("mouthLowerLipRollIn")) + Side), Read(TEXT("mouthRollLower")));
	}
	Set(TEXT("tongueOut"), Read(TEXT("tongueOut")));
	Set(TEXT("tongueUp"), FMath::Max(Read(TEXT("tongueUp")), Read(TEXT("tongueRaise"))));
	return Controls;
}

void UStoryFlowMetaHumanLipsyncComponent::ApplyWeights()
{
	FString Incompatibility;
	if (Face.IsValid() && !CanDriveFace(Face.Get(), Incompatibility))
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow Editor MetaHuman: released %s because its animation setup changed: %s"), *Face->GetPathName(), *Incompatibility);
		RestoreFace();
		return;
	}
	if (Face.IsValid())
		if (auto* Anim = Cast<UStoryFlowMetaHumanAnimInstance>(Face->GetAnimInstance()))
		{
			Anim->BakedAnimation = nullptr;
			if (IsActive() && IsLipsyncActive() && bHasPlaybackPosition && PlaybackAudio.IsValid() && PlaybackAudio->IsPlaying())
			{
				USoundWave* Voice = Cast<USoundWave>(PlaybackAudio->Sound);
				UAnimSequence* Sequence = BakeLibrary ? BakeLibrary->FindAnimation(Voice, Face->GetSkeletalMeshAsset()) : nullptr;
				if (!BakeLibrary)
					if (const auto* Manual = BakedAnimations.Find(Voice)) Sequence = *Manual;
				if (Sequence)
				{
					Anim->BakedAnimation = Sequence;
					Anim->BakedTime = FMath::Clamp(BakedPlaybackTime, 0.f, Sequence->GetPlayLength());
				}
			}
			Anim->RigControls = MapRigControls(GetOutputWeights());
		}
}

void UStoryFlowMetaHumanLipsyncComponent::ZeroOwnedMorphs()
{
	if (Face.IsValid() && Face->GetAnimationMode() == EAnimationMode::AnimationBlueprint
		&& Face->GetAnimClass() == UStoryFlowMetaHumanAnimInstance::StaticClass())
		if (auto* Anim = Cast<UStoryFlowMetaHumanAnimInstance>(Face->GetAnimInstance()))
		{
			if (Anim->GetClass() != UStoryFlowMetaHumanAnimInstance::StaticClass()) return;
			Anim->BakedAnimation = nullptr;
			Anim->RigControls = MapRigControls({});
		}
}

bool UStoryFlowMetaHumanLipsyncComponent::IsUsingBakedAnimation() const
{
	if (Face.IsValid())
		if (const auto* Anim = Cast<UStoryFlowMetaHumanAnimInstance>(Face->GetAnimInstance()))
			return Anim->BakedAnimation != nullptr;
	return false;
}

float UStoryFlowMetaHumanLipsyncComponent::GetJawControl() const
{
	if (Face.IsValid())
		if (auto* Anim = Cast<UStoryFlowMetaHumanAnimInstance>(Face->GetAnimInstance()))
		{
			if (Anim->BakedAnimation)
			{
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 6
				return Anim->BakedAnimation->EvaluateCurveData(TEXT("CTRL_expressions_jawOpen"), FAnimExtractContext(double(Anim->BakedTime)));
#else
				return Anim->BakedAnimation->EvaluateCurveData(TEXT("CTRL_expressions_jawOpen"), Anim->BakedTime);
#endif
			}
			return Anim->RigControls.FindRef(TEXT("CTRL_expressions_jawOpen"));
		}
	return 0.f;
}

void UStoryFlowMetaHumanLipsyncComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	Super::EndPlay(Reason);
	TrackPlayback(nullptr);
	ReleaseRouting();
	if (OwnedAnalysisSubmix)
	{
		if (FAudioDeviceHandle Device = GetWorld()->GetAudioDevice(); Device.IsValid())
		{
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 4
			Device->UnregisterSoundSubmix(OwnedAnalysisSubmix, false);
#else
			Device->UnregisterSoundSubmix(OwnedAnalysisSubmix);
#endif
		}

		// The mixer roots registered submix UObjects, but unregistering only removes
		// the mixer instance. Drain registration/unregistration before releasing our
		// root, including an immediate PIE stop while registration is still queued.
		FAudioCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
		OwnedAnalysisSubmix->RemoveFromRoot();
		OwnedAnalysisSubmix = nullptr;
	}
	bOwnAnalysisSubmix = false;
	RestoreFace();
}
