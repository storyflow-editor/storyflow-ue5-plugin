// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "UObject/ObjectKey.h"
#include "StoryFlowLipsyncComponent.generated.h"

class UAudioComponent;
class USkeletalMesh;
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

	/**
	 * The actor carrying the dialogue component to listen to. Empty: the first one found in the world.
	 *
	 * An actor rather than the component, because a component on ANOTHER actor cannot be picked in a details
	 * panel — Source below is settable from Blueprint and from code and from nothing else, which made
	 * discovery the only path anyone ever took.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TObjectPtr<AActor> SourceActor;

	/** The dialogue component to listen to. Resolved from SourceActor, then by search, on begin play. */
	UPROPERTY(BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TObjectPtr<UStoryFlowComponent> Source;

	/** This face's StoryFlow character id. Empty: move on EVERY line, which is right for a one-character scene. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	FString CharacterId;

	/**
	 * The character's MESH COMPONENT — the parent the Sidekick parts attach under — or empty for the actor
	 * root. Every skeletal mesh beneath it carrying ARKit-named morph targets is driven.
	 *
	 * NEVER a single part. Parts are attached as SIBLINGS under the leader mesh, so pointing this at the
	 * head drives the head alone and leaves the teeth and the tongue behind, which reads as a jaw that opens
	 * onto a closed mouth. See THE FAN-OUT above.
	 *
	 * No AllowAnyActor: the picker reads that key by PRESENCE, not by value, so writing it as false would
	 * turn cross-actor picking ON. Omitting it is what restricts the list to this actor's own components.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync", meta = (UseComponentPicker))
	FComponentReference FaceRoot;

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
	 *
	 * A submix the mixer has not registered analyses the MASTER instead, silently — the engine falls back
	 * rather than failing — so a mouth that hears the music when this is set is a submix routing problem,
	 * not a lipsync one.
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
	 * The raw magnitude a full-scale sine produces at its own bin, which is what the driver divides by to
	 * put the mixer's spectrum back on the decibel scale its constants were tuned on.
	 *
	 * 5.66 is ALGEBRA, not measurement: a Hann window over 512 samples with the mixer's sqrt-of-FFT-size
	 * scaling gives A * sqrt(512) / 4. Nobody has measured it. To set it properly, play the loudest line in
	 * the game, read GetRawPeak(), and put that number here — too low and every line saturates the mouth
	 * wide open, too high and quiet lines never pass the gate.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "StoryFlow|Lipsync", meta = (ClampMin = "0.001"))
	float AnalysisFullScale = 5.66f;

	/**
	 * Drive the mouth from any playing audio: a cutscene line, a bark, a radio. Nothing to bake and nothing
	 * to author. The analysis is submix-wide, so this only says "start moving"; what it hears is whatever
	 * AnalysisSubmix is carrying.
	 *
	 * Game-code entry, and it keeps game-code semantics: it runs until StopLipsync, because the plugin has
	 * no sound of its own to follow here. The dialogue path closes the mouth on its own audio instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Lipsync")
	void StartLipsync();

	/** Let the mouth close. Safe to call when nothing is playing. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Lipsync")
	void StopLipsync();

	/** Loudness 0..1 the driver is seeing, for a debug meter. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Lipsync")
	float GetLevel() const;

	/** The largest raw magnitude seen since this line started — the number AnalysisFullScale wants. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Lipsync")
	float GetRawPeak() const;

	/**
	 * Where the analysed audio sits on the vowel axis, 0 (OO) to 1 (EE). Read it with GetLevel when a mouth
	 * opens but looks wrong: a centroid pinned at 1 is a stretched grin, pinned at 0 a permanent pucker.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Lipsync")
	float GetCentroid() const;

	/** Is this face driven by its line, a retained audio tail, or StartLipsync from game code? */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Lipsync")
	bool IsLipsyncActive() const { return bManualLipsync || bLineIsMine || SourceAudioIsPlaying(); }

	/**
	 * Do two character paths name the same character?
	 *
	 * Both sides go through NormalizeCharacterPath because they come from different places and disagree
	 * about shape: GetCharacterPathById answers the normalized bridge key, while a node whose id fails to
	 * resolve falls back to the AUTHORED path verbatim, slashes and casing and all.
	 */
	static bool SpeakerPathsMatch(const FString& APath, const FString& BPath);

	/**
	 * How many dialogue updates this component has treated as the START of a line — a TEST SEAM for the
	 * re-render rule, which turns on this count staying at one across a re-broadcast of the same node.
	 */
	int32 GetLineStarts() const { return LineStarts; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;

private:
	UFUNCTION()
	void HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState);

	UFUNCTION()
	void HandleDialogueEnded();

	/** What the mouth is following this frame. */
	enum class EMouthDrive : uint8
	{
		/** Audio we can hear: the spectrum decides the pose. */
		Analyse,
		/** A line we cannot hear — no audio, or no analysis available. Keep the face alive. */
		Idle,
		/** Nothing, or a line whose sound has ended. Close. */
		Silent
	};
	EMouthDrive DecideDrive() const;

	/** Is the audio component the mouth is following still sounding? False when the game plays its own. */
	bool SourceAudioIsPlaying() const;

	/** Analyse when there is a mixer to ask, else the idle mouth if allowed, else close. */
	EMouthDrive HearOrIdle() const;

	bool SpeakerIsMine() const;
	void ResolveSource();
	void ResolveFace(const StoryFlowVisemeTable::FTable& Table);

	/**
	 * Re-resolve the face when the meshes we cached are gone OR have been replaced.
	 *
	 * Sidekick assembles a character from part components and rebuilds them whenever the outfit changes at
	 * runtime — sometimes by destroying the components, sometimes by calling SetSkeletalMesh on the ones
	 * already there. A weak pointer only catches the first, so the mesh ASSET is cached too: the second
	 * leaves live components whose morphs are new and undriven, which is a mouth that quietly stops moving.
	 */
	bool RefreshFaceIfStale(float DeltaSeconds);
	void ApplyWeights();
	void ZeroOwnedMorphs();
	void StartAnalysis();
	void StopAnalysis();

	/** One entry per skeletal mesh that owns at least one morph we drive — see THE FAN-OUT. */
	struct FFaceTarget
	{
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		TWeakObjectPtr<USkeletalMesh> Asset;
		TArray<FName> Morphs;
	};

	TArray<FFaceTarget> Targets;

	/** Kept so a re-resolve costs nothing but the component walk. */
	StoryFlowVisemeTable::FTable ResolvedTable;

	/** Throttles the "still no face" retry and the "still no source" retry, and keeps their warnings to one. */
	float SinceFaceCheck = 0.0f;
	float SinceSourceCheck = 0.0f;
	bool bWarnedNoFace = false;
	bool bWarnedNoSource = false;
	bool bWarnedMissingMorphs = false;
	bool bWarnedMasterSubmix = false;
	bool bNotedEveryLine = false;

	/**
	 * ResolveFace has run at least once. The first resolve is at BeginPlay, before a runtime-assembled
	 * character may have its parts, so "no face" is only worth saying once the retry has looked too.
	 */
	bool bFaceResolvedBefore = false;

	/** Bound to the source's delegates. Separate from Source being set: a designer-set Source needs binding too. */
	bool bSubscribed = false;

	/** Latched inside SpeakerIsMine, which is const because asking who is speaking changes nothing. */
	mutable bool bWarnedUnknownCharacter = false;
	TArray<float> AnalysisFrequencies;
	TArray<float> Magnitudes;
	TUniquePtr<FStoryFlowLipsyncDriver> Driver;

	/** Both the node and its execution identity, so repeated nodes can start new audio. */
	FString LineNodeId;
	uint64 LineEntrySerial = 0;
	int32 LineStarts = 0;

	bool bLineIsMine = false;

	/** A missing weak pointer can mean a tracked sound was destroyed, rather than external playback. */
	bool bHadTrackedAudio = false;

	/** The line carries audio of its OWN, as opposed to riding the previous line's tail. */
	bool bLineCarriesAudio = false;

	/**
	 * The audio component the plugin started for the audio being followed, so the mouth closes when THAT
	 * stops rather than when the dialogue component starts something else. Empty when the game plays its own.
	 */
	TWeakObjectPtr<UAudioComponent> LineAudio;

	/** StartLipsync was called by game code: no line to follow, so it runs until StopLipsync. */
	bool bManualLipsync = false;

	/** There is an audio mixer to ask. Decided once: without one every read logs an engine error. */
	bool bAnalysisAvailable = false;
	bool bAnalysing = false;

	/** The analysis reference this component holds, so Stop releases what Start took even if AnalysisSubmix changed since. */
	uint32 AnalysingDeviceId = 0;
	FObjectKey AnalysingSubmix;
};
