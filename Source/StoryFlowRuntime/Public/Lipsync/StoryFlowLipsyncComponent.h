// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "StoryFlowLipsyncComponent.generated.h"

class UAudioComponent;
class USkeletalMeshComponent;
class USoundSubmix;
class UStoryFlowComponent;
class UStoryFlowVisemeMap;
struct FStoryFlowDialogueState;

/**
 * Audio-driven mouth movement for one character's face. Put it on the actor, point it at the face, tell it
 * which StoryFlow character it is, and it moves the mouth on that character's lines. No per-line wiring.
 *
 * A SEPARATE COMPONENT, not part of UStoryFlowComponent, and deliberately so: StoryFlow is the dialogue
 * brain and does not know the scene. Characters are ids and data; the GAME owns which actor wears which
 * face. Lipsync is presentational, so it lives where the face lives, and a project with no 3D faces pays
 * nothing for it.
 *
 * ANY ARKIT-NAMED RIG, not just Synty. See UStoryFlowVisemeMap for what that buys.
 *
 * THE FAN-OUT is the Unreal-specific problem. A Sidekick character is one USkeletalMeshComponent PER PART,
 * wired together with SetLeaderPoseComponent — which shares BONES but NOT morph weights. So `jawOpen` has to
 * be written on the head AND the teeth AND the tongue, and `mouthClose` also on the nose, or the jaw opens
 * while the teeth stay put. This component drives every skeletal mesh under its face root that owns a morph
 * it needs, so a merged single-mesh character (see the SKMerger path) simply yields one target instead.
 *
 * Tier 1 (this): live analysis of the audio the mixer is producing, works on any audio with no baking, reads
 * convincingly but will not hit a specific consonant. Tier 2 replaces the analysis with baked viseme tracks
 * and reuses everything else. See LIPSYNC_DESIGN.md.
 */
UCLASS(ClassGroup = (StoryFlow), meta = (BlueprintSpawnableComponent, DisplayName = "StoryFlow Lipsync"))
class STORYFLOWRUNTIME_API UStoryFlowLipsyncComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStoryFlowLipsyncComponent();

	/** The dialogue component to listen to. Empty: the first one found in the world on begin play. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TObjectPtr<UStoryFlowComponent> Source;

	/** This face's StoryFlow character id. Empty: move on EVERY line, which is right for a one-character scene. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	FString CharacterId;

	/**
	 * Where the face meshes live. Empty: the owning actor. Every USkeletalMeshComponent beneath it carrying
	 * ARKit-named morph targets is driven — see THE FAN-OUT above.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TObjectPtr<USceneComponent> FaceRoot;

	/** Optional per-rig mapping. Empty: the built-in ARKit table, tuned on Synty Sidekick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TObjectPtr<UStoryFlowVisemeMap> VisemeMap;

	/**
	 * The submix to analyse. STRONGLY RECOMMENDED: route dialogue to its own submix and name it here.
	 *
	 * Empty analyses the master output, which works out of the box but hears the whole mix — music included,
	 * so a loud score would move the mouth. Unreal has no per-AudioComponent live spectrum: UAudioComponent's
	 * FFT and envelope readers are COOKED, needing per-asset analysis ticked on every dialogue wave, which is
	 * exactly the per-asset chore Tier 2's editor-side baking exists to avoid.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TObjectPtr<USoundSubmix> AnalysisSubmix;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync|Feel", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Strength = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync|Feel", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float Sensitivity = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync|Feel", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float JawBias = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync|Feel", meta = (ClampMin = "1.0", ClampMax = "40.0"))
	float Smoothing = 16.0f;

	/** Move the mouth on lines whose audio cannot be analysed, instead of leaving a dead face. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync|Feel")
	bool bIdleMouthWithoutAudio = true;

	/**
	 * Drive the mouth from any playing audio: a cutscene line, a bark, a radio. Nothing to bake and nothing
	 * to author. The analysis is submix-wide, so this only says "start moving"; what it hears is whatever
	 * AnalysisSubmix is carrying.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Lipsync")
	void StartLipsync();

	/** Let the mouth close. Safe to call when nothing is playing. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Lipsync")
	void StopLipsync();

	/** Loudness 0..1 the driver is seeing, for a debug meter. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Lipsync")
	float GetLevel() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	UFUNCTION()
	void HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState);

	UFUNCTION()
	void HandleDialogueEnded();

	bool SpeakerIsMine() const;
	void ResolveFace(const StoryFlowVisemeTable::FTable& Table);
	void ApplyWeights();
	void StartAnalysis();
	void StopAnalysis();

	/** One entry per skeletal mesh that owns at least one morph we drive — see THE FAN-OUT. */
	struct FFaceTarget
	{
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		TArray<FName> Morphs;
	};

	TArray<FFaceTarget> Targets;
	TArray<float> AnalysisFrequencies;
	TArray<float> Magnitudes;
	TUniquePtr<FStoryFlowLipsyncDriver> Driver;

	bool bSpeaking = false;
	bool bLineIsMine = false;
	bool bAnalysing = false;
};
