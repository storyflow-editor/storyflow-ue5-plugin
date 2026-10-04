// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowLipsyncComponent.h"

#include "AudioDeviceHandle.h"
#include "AudioDeviceManager.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StoryFlowComponent.h"
#include "Evaluation/StoryFlowRestoredListener.h"
#include "Data/StoryFlowTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Lipsync/StoryFlowVisemeMap.h"
#include "StoryFlowRuntime.h"
#include "UObject/ObjectKey.h"

namespace
{
	/** How many frequencies to ask the mixer for across the band. Ample for a mouth, cheap to sample. */
	constexpr int32 AnalysisBands = 24;

	/** How often to reconcile face parts or retry a missing dialogue component. */
	constexpr float FaceRecheckSeconds = 1.0f;

	/**
	 * One live analysis per AUDIO DEVICE and submix, however many faces are listening to it.
	 *
	 * The device, not the world: several PIE worlds share one audio device unless the user opts out, and the
	 * engine's start/stop act on the device's submix. Counted per world, the last face in one world would
	 * stop the analysis every face in the other was still reading.
	 */
	struct FAnalysisKey
	{
		uint32 DeviceId = 0;
		FObjectKey Submix;

		bool operator==(const FAnalysisKey& Other) const
		{
			return DeviceId == Other.DeviceId && Submix == Other.Submix;
		}

		// A hidden friend, found by argument lookup only, so it cannot hide the integer and FObjectKey
		// overloads it is built from the way a namespace-level overload would.
		friend uint32 GetTypeHash(const FAnalysisKey& Key)
		{
			return HashCombine(GetTypeHash(Key.DeviceId), GetTypeHash(Key.Submix));
		}
	};

	uint32 AudioDeviceIdOf(const UWorld* World)
	{
		const FAudioDeviceHandle Handle = World != nullptr ? World->GetAudioDevice() : FAudioDeviceHandle();
		return Handle.IsValid() ? Handle.GetDeviceID() : 0;
	}

	/**
	 * REFERENCE COUNTED, because StopAnalyzingOutput is not.
	 *
	 * The engine's stop resets the submix's analyser outright with no notion of who else was reading it, so
	 * the first lipsync actor destroyed or streamed out used to end analysis for every other face in the
	 * scene — which then read empty magnitudes forever, still believing they were analysing, and logged a
	 * mixer warning per tick each while they did it.
	 */
	TMap<FAnalysisKey, int32>& AnalysisRefCounts()
	{
		static TMap<FAnalysisKey, int32> Counts;
		return Counts;
	}
}

UStoryFlowLipsyncComponent::UStoryFlowLipsyncComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Activation is the off switch: SetActive(false) has to close the mouth, and a component that never
	// activates would never tick.
	bAutoActivate = true;
}

void UStoryFlowLipsyncComponent::BeginPlay()
{
	Super::BeginPlay();

	// No faces on a server: no driver, no analysis and no tick, rather than an idle mouth writing morph
	// targets nobody renders on every server frame.
	if (const UWorld* ServerCheck = GetWorld(); ServerCheck != nullptr && ServerCheck->GetNetMode() == NM_DedicatedServer)
	{
		SetComponentTickEnabled(false);
		return;
	}

	ResolvedTable = VisemeMap != nullptr ? VisemeMap->ToTable() : StoryFlowVisemeTable::Default();
	Driver = MakeUnique<FStoryFlowLipsyncDriver>(ResolvedTable);
	ResolveFace(ResolvedTable);

	// The frequencies the analysis is sampled at: evenly spaced across the driver's band, so the
	// magnitude-weighted mean index IS the normalised centroid the spec describes.
	AnalysisFrequencies.Reset();
	for (int32 Index = 0; Index < AnalysisBands; ++Index)
	{
		const float Alpha = static_cast<float>(Index) / (AnalysisBands - 1);
		AnalysisFrequencies.Add(FMath::Lerp(FStoryFlowLipsyncDriver::MinHz, FStoryFlowLipsyncDriver::MaxHz, Alpha));
	}

	// Decided once, here, rather than discovered per tick: without a mixer every magnitude read logs an
	// engine ERROR, so a -nosound run would fill the log at frame rate.
	bAnalysisAvailable = FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(this) != nullptr;

	ResolveSource();

	if (CharacterId.IsEmpty() && !bNotedEveryLine)
	{
		bNotedEveryLine = true;
		UE_LOG(LogStoryFlow, Log,
			TEXT("StoryFlow: Lipsync on '%s' has no CharacterId, so this face moves on EVERY line. ")
			TEXT("Right for a one-character scene; set CharacterId when more than one face is listening."),
			*GetNameSafe(GetOwner()));
	}
}

void UStoryFlowLipsyncComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindSource();
	StopLipsync();
	StopAnalysis();
	ZeroOwnedMorphs();
	Super::EndPlay(EndPlayReason);
}

void UStoryFlowLipsyncComponent::Activate(bool bReset)
{
	Super::Activate(bReset);

	// A face switched back on starts listening fresh. Inheriting the loudness history of whenever it was
	// switched off would scale the next line against a peak from a scene ago.
	if (Driver.IsValid())
	{
		Driver->ResetLevel();

		// And catches up: the update that started the line on screen went by while this was off, and the
		// next one may be a while coming.
		if (bSubscribed && Source != nullptr && Source->IsDialogueActive())
		{
			const FStoryFlowDialogueState Current = Source->GetCurrentDialogue();
			if (Current.bIsValid)
			{
				HandleDialogueUpdated(Current);
			}
		}
	}
}

void UStoryFlowLipsyncComponent::Deactivate()
{
	Super::Deactivate();

	// Off means off: a face left holding whatever vowel it was mid-way through is worse than one that never
	// moved, and it would stay that way until something else wrote those morphs.
	StopLipsync();
	StopAnalysis();
	if (Driver.IsValid())
	{
		// A delta this large makes the ease a complete move, so the driver lands exactly on rest rather than
		// keeping a pose it would stamp back the frame it is re-enabled.
		Driver->AdvanceSilent(10.0f);
	}
	ZeroOwnedMorphs();
}

void UStoryFlowLipsyncComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Driver.IsValid() || !IsActive())
	{
		return;
	}

	// REAL seconds. The audio clock is not dilated, so a slow-motion scene must not lag the voice, and a
	// paused one must not snap the mouth at dilation zero.
	const UWorld* World = GetWorld();
	const float RealDelta = World != nullptr ? World->DeltaRealTimeSeconds : DeltaTime;

	Driver->Strength = Strength;
	Driver->Sensitivity = Sensitivity;
	Driver->JawBias = JawBias;
	Driver->Smooth = Smoothing;
	// The details panel clamps this; a Blueprint write does not, and 0 would put every bin at full scale
	// and hold the mouth wide open.
	Driver->FullScale = FMath::Max(AnalysisFullScale, 0.001f);

	// The dialogue actor can go away and come back (streaming, a respawn). Its delegates died with it, so
	// discovery has to be re-armed, or this face is deaf for the rest of the session with nothing said.
	if (bSubscribed && (!BoundSource.IsValid() || BoundSource.Get() != Source)) ResolveSource();

	if (!bSubscribed)
	{
		SinceSourceCheck += RealDelta;
		if (SinceSourceCheck >= FaceRecheckSeconds)
		{
			SinceSourceCheck = 0.0f;
			ResolveSource();
		}
	}
	// A restored line has surrendered its face. Do not overwrite another animation's later pose.
	if (bReleasedByRestore && !bManualLipsync) return;
	RefreshFaceIfStale(RealDelta);

	const EMouthDrive Drive = DecideDrive();

	// READING the submix follows the mouth: the drive above stops asking for magnitudes the frame the line's
	// sound ends, which is what used to leave a face mouthing the music for the ten seconds after a two
	// second line had finished. The ANALYSIS itself outlives the line and stops with the dialogue: starting
	// it is an audio-thread round trip, and every read that lands before it completes logs the engine's
	// "call StartSpectrumAnalysis first" warning, so tearing it down and rebuilding it around every line
	// would repeat that warning per line instead of once per conversation.
	if (Drive == EMouthDrive::Analyse)
	{
		if (!bAnalysing)
		{
			StartAnalysis();
		}
	}
	else if (bAnalysing && !(Source != nullptr && Source->IsDialogueActive()))
	{
		StopAnalysis();
	}

	switch (Drive)
	{
	case EMouthDrive::Analyse:
		Magnitudes.Reset();
		UAudioMixerBlueprintLibrary::GetMagnitudeForFrequencies(this, AnalysisFrequencies, Magnitudes, AnalysisSubmix);
		Driver->AdvanceFromMagnitudes(Magnitudes, RealDelta);
		break;
	case EMouthDrive::Idle:
		Driver->AdvanceIdle(RealDelta);
		break;
	default:
		Driver->AdvanceSilent(RealDelta);
		if (!bLineIsMine && !bManualLipsync)
		{
			// A completed tail cannot adopt a later replay of the same audio component.
			StopLipsync();
		}
		break;
	}

	ApplyWeights();
}

/**
 * The audio-follow rule.
 *
 * A line with audio drives the mouth only while that audio is SOUNDING, and closes when it stops — it does
 * not fall through to the idle mouth, because the line is not silent, it is over. Idle is for a line the arm
 * cannot hear at all: no audio on the node, or no mixer to ask. A text-only line that arrived while the
 * previous line's sound was still playing keeps following that sound, and idles only once it has stopped.
 */
UStoryFlowLipsyncComponent::EMouthDrive UStoryFlowLipsyncComponent::DecideDrive() const
{
	if (bManualLipsync)
	{
		// Game-code lipsync has no sound of its own to follow, so it runs until StopLipsync.
		return HearOrIdle();
	}

	if (LineAudio.IsValid())
	{
		if (SourceAudioIsPlaying())
		{
			return HearOrIdle();
		}
		// The sound the mouth was following has stopped: a line with audio of its own is over, a text-only
		// line riding the previous line's tail goes on being read.
		return bLineIsMine && !bLineCarriesAudio && bIdleMouthWithoutAudio ? EMouthDrive::Idle : EMouthDrive::Silent;
	}

	if (!bLineIsMine)
	{
		return EMouthDrive::Silent;
	}

	if (bLineCarriesAudio)
	{
		// Only never-tracked external playback follows the line. A destroyed tracked sound is over.
		return bHadTrackedAudio ? EMouthDrive::Silent : HearOrIdle();
	}

	// No audio on this line: the idle mouth carries it while the text is read.
	return bIdleMouthWithoutAudio ? EMouthDrive::Idle : EMouthDrive::Silent;
}

UStoryFlowLipsyncComponent::EMouthDrive UStoryFlowLipsyncComponent::HearOrIdle() const
{
	return bAnalysisAvailable ? EMouthDrive::Analyse : (bIdleMouthWithoutAudio ? EMouthDrive::Idle : EMouthDrive::Silent);
}

bool UStoryFlowLipsyncComponent::SourceAudioIsPlaying() const
{
	const UAudioComponent* Audio = LineAudio.Get();
	return Audio != nullptr && Audio->IsPlaying();
}

void UStoryFlowLipsyncComponent::StartLipsync()
{
	bReleasedByRestore = false;
	bManualLipsync = true;
	bLineIsMine = false;
	bHadTrackedAudio = false;
	bLineCarriesAudio = false;
	LineAudio.Reset();
	LineNodeId.Reset();
	LineEntrySerial = 0;
	if (Driver.IsValid())
	{
		Driver->ResetLevel();
	}
}

void UStoryFlowLipsyncComponent::StopLipsync()
{
	bManualLipsync = false;
	bLineIsMine = false;
	bHadTrackedAudio = false;
	bLineCarriesAudio = false;
	LineAudio.Reset();
	LineNodeId.Reset();
	LineEntrySerial = 0;
}

float UStoryFlowLipsyncComponent::GetLevel() const
{
	return Driver.IsValid() ? Driver->Level() : 0.0f;
}

float UStoryFlowLipsyncComponent::GetRawPeak() const
{
	return Driver.IsValid() ? Driver->RawPeak() : 0.0f;
}

float UStoryFlowLipsyncComponent::GetCentroid() const
{
	return Driver.IsValid() ? Driver->Centroid() : 0.0f;
}

/**
 * A dialogue update. THE RE-RENDER RULE decides whether it starts a line.
 *
 * The same node comes round again for reasons that have nothing to do with speech: a variable changed and
 * the text was re-interpolated, the dialogue resumed, a dead-end branch re-broadcast the line it is still
 * sitting on. Treating each as a line start re-armed the loudness history every time, and a game writing a
 * variable per frame pinned the peak to its initial value and held the mouth wide open.
 */
void UStoryFlowLipsyncComponent::HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState)
{
	if (Source && Source->IsCurrentDialogueRestored()) { HandleDialogueRestored(DialogueState, Source); return; }
	if (bSubscribed && BoundSource.Get() != Source) return;
	if (!SpeakerIsMine())
	{
		// Someone else's line. Game-code lipsync (StartLipsync) promised to run until StopLipsync, and
		// another character talking is not that.
		if (!bManualLipsync)
		{
			StopLipsync();
		}
		return;
	}

	const uint64 EntrySerial = Source != nullptr ? Source->GetDialogueEntrySerial() : 0;
	if (bLineIsMine && !LineNodeId.IsEmpty() && LineNodeId == DialogueState.NodeId && LineEntrySerial == EntrySerial)
	{
		return;
	}

	LineNodeId = DialogueState.NodeId;
	bReleasedByRestore = false;
	LineEntrySerial = EntrySerial;
	++LineStarts;
	bManualLipsync = false;
	bLineIsMine = true;
	bLineCarriesAudio = DialogueState.Audio != nullptr;

	if (bLineCarriesAudio)
	{
		// PlayDialogueAudio runs before this broadcast, so whether the plugin holds the sound is knowable now.
		// It does not when a game overrode playback — and then there is nothing to follow, so the mouth
		// follows the LINE instead, exactly as it did before the audio-follow rule existed.
		LineAudio = Source != nullptr ? Source->GetCurrentDialogueAudio() : nullptr;
		bHadTrackedAudio = LineAudio.IsValid();
		if (Driver.IsValid())
		{
			Driver->ResetLevel();
		}
	}
	else if (LineAudio.IsValid() && SourceAudioIsPlaying())
	{
		// A text-only line, but the previous line's sound is still playing (bAudioReset is off by default):
		// keep following it. Idling over audible speech is the one thing the idle mouth must never do.
		bHadTrackedAudio = true;
	}
	else
	{
		LineAudio.Reset();
		bHadTrackedAudio = false;
	}
}

void UStoryFlowLipsyncComponent::HandleDialogueEnded()
{
	if (bSubscribed && Source && Source->IsDialogueActive()) return;
	if (bManualLipsync)
	{
		return;
	}

	// The text is over even when the sound is not. Retain only the tail, never an idle text-line owner.
	bLineIsMine = false;
	LineNodeId.Reset();
	LineEntrySerial = 0;

	// A line's audio can outlive its dialogue (bStopAudioOnDialogueEnd false), and cutting the mouth here
	// would clip the audible tail. The promise is that the mouth closes when the SOUND stops, not when the
	// text does — so keep following, and let the audio-follow rule close it.
	if (LineAudio.IsValid() && SourceAudioIsPlaying())
	{
		return;
	}
	StopLipsync();
}

void UStoryFlowLipsyncComponent::HandleDialogueRestored(const FStoryFlowDialogueState& State, UStoryFlowComponent* From)
{
	if (bManualLipsync || !bSubscribed || !From || From != Source || From != BoundSource.Get() ||
		!From->IsCurrentDialogueRestored() || From->GetCurrentDialogue().NodeId != State.NodeId) return;
	StopLipsync(); StopAnalysis();
	if (Driver.IsValid()) Driver->ResetPose();
	ZeroOwnedMorphs(); bReleasedByRestore = true;
}

void UStoryFlowLipsyncComponent::UnbindSource()
{
	if (auto* Old = BoundSource.Get())
	{
		Old->OnDialogueUpdated.RemoveDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueUpdated);
		Old->OnDialogueEnded.RemoveDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueEnded);
		if (RestoredListener) Old->OnDialogueRestored.RemoveDynamic(RestoredListener, &UStoryFlowRestoredListener::Restore);
	}
	RestoredListener = nullptr; BoundSource.Reset(); bSubscribed = false;
}

/**
 * Is the current line mine? An empty CharacterId means every line is.
 *
 * FStoryFlowDialogueState carries the resolved character DATA — a struct, copied by value — with no id or
 * path on it, so there is nothing on the event to compare against, and Unity's trick of matching the runtime
 * record by reference cannot work here. The dialogue component keeps the resolved speaker path it looked the
 * character up by, and exposes it read-only; that is the id-native answer and it is immune to a localized
 * display name changing under a language switch.
 */
bool UStoryFlowLipsyncComponent::SpeakerIsMine() const
{
	if (CharacterId.IsEmpty())
	{
		return true;
	}
	if (Source == nullptr)
	{
		return false;
	}

	bool bFound = false;
	FString MyPath;
	Source->GetCharacterPathById(CharacterId, MyPath, bFound);
	if (!bFound || MyPath.IsEmpty())
	{
		// A mistyped id, a wrong-case id, or a project exported before characters carried ids: the face
		// never moves and every other diagnostic stays silent, because nothing else about it is wrong.
		if (!bWarnedUnknownCharacter)
		{
			bWarnedUnknownCharacter = true;
			UE_LOG(LogStoryFlow, Warning,
				TEXT("StoryFlow: Lipsync on '%s' is set to character id '%s', which this project has no ")
				TEXT("character for. This face will never move. Check the id, or leave it empty to move on every line."),
				*GetNameSafe(GetOwner()), *CharacterId);
		}
		return false;
	}
	return SpeakerPathsMatch(MyPath, Source->GetCurrentSpeakerPath());
}

bool UStoryFlowLipsyncComponent::SpeakerPathsMatch(const FString& APath, const FString& BPath)
{
	// Both sides normalized, because they are built by different code that disagrees about shape: the bridge
	// answers a lowercase backslash key, while a node whose id fails to resolve falls back to the authored
	// path verbatim. Same character, two spellings, and a face that never moves.
	return !APath.IsEmpty() && NormalizeCharacterPath(APath) == NormalizeCharacterPath(BPath);
}

/**
 * Find the dialogue component to listen to, and subscribe.
 *
 * Retried while nothing is found rather than done once at BeginPlay: the dialogue actor is often spawned
 * after the faces are, and a one-shot search left the mouth silently wired to nothing.
 */
void UStoryFlowLipsyncComponent::ResolveSource()
{
	if (bSubscribed && BoundSource.IsValid() && BoundSource.Get() == Source)
	{
		return;
	}
	UnbindSource();
	if (!bManualLipsync) { StopLipsync(); StopAnalysis(); if (Driver.IsValid()) Driver->ResetPose(); ZeroOwnedMorphs(); }

	if (!IsValid(Source)) Source = nullptr;
	if (Source == nullptr && SourceActor != nullptr)
	{
		Source = SourceActor->FindComponentByClass<UStoryFlowComponent>();
	}

	if (Source == nullptr)
	{
		if (const UWorld* World = GetWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (UStoryFlowComponent* Found = It->FindComponentByClass<UStoryFlowComponent>())
				{
					Source = Found;
					break;
				}
			}
		}
	}

	if (Source == nullptr)
	{
		if (!bWarnedNoSource)
		{
			bWarnedNoSource = true;
			UE_LOG(LogStoryFlow, Warning,
				TEXT("StoryFlow: Lipsync on '%s' found no StoryFlow component to listen to. Set SourceActor, ")
				TEXT("or ignore this if the dialogue actor is spawned later — the search retries every second."),
				*GetNameSafe(GetOwner()));
		}
		return;
	}

	Source->OnDialogueUpdated.AddDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueUpdated);
	Source->OnDialogueEnded.AddDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueEnded);
	BoundSource = Source;
	RestoredListener = NewObject<UStoryFlowRestoredListener>(this);
	RestoredListener->Source = Source;
	const TWeakObjectPtr<UStoryFlowLipsyncComponent> Self(this);
	UStoryFlowRestoredListener* Binding = RestoredListener;
	RestoredListener->Callback = [Self, Binding](const FStoryFlowDialogueState& State, UStoryFlowComponent* From) {
		if (Self.IsValid() && Self->RestoredListener == Binding) Self->HandleDialogueRestored(State, From);
	};
	Source->OnDialogueRestored.AddDynamic(RestoredListener, &UStoryFlowRestoredListener::Restore);
	bSubscribed = true;

	// A late subscribe misses the update for the line already on screen, and the next one may be a while
	// coming: catch up rather than sitting out the line the player is reading right now.
	const FStoryFlowDialogueState Current = Source->GetCurrentDialogue();
	if (Current.bIsValid && Source->IsDialogueActive())
	{
		HandleDialogueUpdated(Current);
	}
}

/**
 * Resolve which skeletal meshes own which of the morphs we drive, once, on begin play.
 *
 * THE FAN-OUT: on a part-assembled Sidekick character `jawOpen` lives on the head, the teeth AND the tongue,
 * and SetLeaderPoseComponent shares bones but not morph weights, so each has to be written separately. A
 * morph no mesh owns is simply absent from every target, and the rest of the pose still plays — a missing
 * `tongueOut` must never take the jaw down with it.
 */
void UStoryFlowLipsyncComponent::ResolveFace(const StoryFlowVisemeTable::FTable& Table)
{
	// Detached live parts must release the last pose. Current parts get their weights later this tick.
	ZeroOwnedMorphs();
	Targets.Reset();

	USceneComponent* Root = Cast<USceneComponent>(FaceRoot.GetComponent(GetOwner()));
	if (Root == nullptr)
	{
		Root = GetOwner() != nullptr ? GetOwner()->GetRootComponent() : nullptr;
	}
	if (Root == nullptr)
	{
		return;
	}

	// SCOPED TO THE FACE ROOT's subtree, matching the Unity arm. Collecting every skeletal mesh on the actor
	// instead would drive a held weapon or a prop that happened to carry a morph of the same name, and would
	// make FaceRoot a setting that reads as if it does something and does not.
	TArray<USkeletalMeshComponent*> Meshes;
	if (USkeletalMeshComponent* RootMesh = Cast<USkeletalMeshComponent>(Root))
	{
		Meshes.Add(RootMesh);
	}
	TArray<USceneComponent*> Children;
	Root->GetChildrenComponents(true, Children);
	for (USceneComponent* Child : Children)
	{
		if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Child))
		{
			Meshes.Add(Mesh);
		}
	}

	const TSet<FName> Owned = StoryFlowVisemeTable::OwnedMorphs(Table);
	TSet<FName> Missing = Owned;

	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		USkeletalMesh* Asset = Mesh != nullptr ? Mesh->GetSkeletalMeshAsset() : nullptr;
		if (Asset == nullptr)
		{
			continue;
		}

		FFaceTarget Target;
		Target.Mesh = Mesh;
		Target.Asset = Asset;
		for (const FName& Morph : Owned)
		{
			if (Asset->FindMorphTarget(Morph) != nullptr)
			{
				Target.Morphs.Add(Morph);
				Missing.Remove(Morph);
			}
		}

		if (Target.Morphs.Num() > 0)
		{
			Targets.Add(MoveTemp(Target));
		}
	}

	// A face was found, so the next disappearance is worth reporting again.
	bWarnedNoFace = bWarnedNoFace && Targets.Num() == 0;
	bWarnedMissingMorphs = bWarnedMissingMorphs && Targets.Num() > 0;

	if (Targets.Num() == 0)
	{
		// Once, not once per retry: RefreshFaceIfStale comes back every second while a face is missing. And
		// not on the FIRST look: that is BeginPlay, and a character assembled at runtime may not have its
		// parts yet, so a warning there tells a correctly configured user to fix something that works.
		if (!bWarnedNoFace && bFaceResolvedBefore)
		{
			bWarnedNoFace = true;
			UE_LOG(LogStoryFlow, Warning,
				TEXT("StoryFlow: Lipsync on '%s' found no morph targets on any skeletal mesh under the face root. ")
				TEXT("Point FaceRoot at the character's MESH COMPONENT — the parent the parts attach under — or ")
				TEXT("leave it empty for the actor root. Never at a single part: the parts are siblings, so the ")
				TEXT("head alone leaves the teeth and the tongue behind."),
				*GetNameSafe(GetOwner()));
		}
	}
	else if (Missing.Num() > 0 && !bWarnedMissingMorphs)
	{
		// Once: on a plain ARKit rig this names the tongue morphs, and a re-resolve happens on every part
		// swap, which is the ordinary outfit-change case rather than a reason to say it again.
		bWarnedMissingMorphs = true;
		TArray<FString> Names;
		for (const FName& Morph : Missing)
		{
			Names.Add(Morph.ToString());
		}
		UE_LOG(LogStoryFlow, Warning,
			TEXT("StoryFlow: Lipsync on '%s': the rig has no %s. Those parts of each pose are skipped; the rest still plays."),
			*GetNameSafe(GetOwner()), *FString::Join(Names, TEXT(", ")));
	}

	bFaceResolvedBefore = true;
}


/**
 * Re-resolve the face when what we cached has gone, or has been swapped underneath us.
 *
 * The case that forced this: Sidekick builds a character from part components and REBUILDS them whenever the
 * outfit changes at runtime. Sometimes the components are destroyed and replaced — a weak pointer catches
 * that. Sometimes the SAME component is handed a new mesh, which a weak pointer never notices at all: the
 * morphs the target was resolved against no longer exist, and the mouth quietly stops moving. No error, no
 * warning, just a face that used to work, which is the worst way for a feature to fail.
 *
 * Both checks run every frame. New parts do not invalidate existing targets, so also reconcile the subtree
 * once a second: a late head, teeth or tongue must be discovered without a hierarchy walk every frame.
 * Missing-face warnings remain limited to one rather than one per reconciliation.
 */
bool UStoryFlowLipsyncComponent::RefreshFaceIfStale(float DeltaSeconds)
{
	const bool bAnyStale = Targets.ContainsByPredicate([](const FFaceTarget& Target)
	{
		const USkeletalMeshComponent* Mesh = Target.Mesh.Get();
		return Mesh == nullptr || Mesh->GetSkeletalMeshAsset() != Target.Asset.Get();
	});

	SinceFaceCheck += DeltaSeconds;
	if (!bAnyStale && SinceFaceCheck < FaceRecheckSeconds)
	{
		return false;
	}

	SinceFaceCheck = 0.0f;
	ResolveFace(ResolvedTable);
	return true;
}

void UStoryFlowLipsyncComponent::ApplyWeights()
{
	const TMap<FName, float>& Weights = Driver->Current();
	for (const FFaceTarget& Target : Targets)
	{
		USkeletalMeshComponent* Mesh = Target.Mesh.Get();
		if (Mesh == nullptr)
		{
			continue;
		}
		for (const FName& Morph : Target.Morphs)
		{
			if (const float* Weight = Weights.Find(Morph))
			{
				Mesh->SetMorphTarget(Morph, *Weight);
			}
		}
	}
}

void UStoryFlowLipsyncComponent::ZeroOwnedMorphs()
{
	for (const FFaceTarget& Target : Targets)
	{
		USkeletalMeshComponent* Mesh = Target.Mesh.Get();
		if (Mesh == nullptr)
		{
			continue;
		}
		for (const FName& Morph : Target.Morphs)
		{
			Mesh->SetMorphTarget(Morph, 0.0f);
		}
	}
}

void UStoryFlowLipsyncComponent::StartAnalysis()
{
	if (bAnalysing || !bAnalysisAvailable)
	{
		return;
	}

	if (AnalysisSubmix == nullptr && !bWarnedMasterSubmix)
	{
		// Once per component: analysis now starts with every conversation, not once per session.
		bWarnedMasterSubmix = true;
		UE_LOG(LogStoryFlow, Warning,
			TEXT("StoryFlow: Lipsync on '%s' is analysing the MASTER output because no AnalysisSubmix is set. ")
			TEXT("It will hear music and effects as well as speech. Route dialogue to its own submix and name it here."),
			*GetNameSafe(GetOwner()));
	}

	// Remembered for StopAnalysis: the reference must be released on the key it was taken on, whatever
	// AnalysisSubmix says by then.
	AnalysingDeviceId = AudioDeviceIdOf(GetWorld());
	AnalysingSubmix = FObjectKey(AnalysisSubmix);

	const FAnalysisKey Key{ AnalysingDeviceId, AnalysingSubmix };
	int32& Count = AnalysisRefCounts().FindOrAdd(Key);
	if (Count++ == 0)
	{
		UAudioMixerBlueprintLibrary::StartAnalyzingOutput(this, AnalysisSubmix);
	}
	bAnalysing = true;
}

void UStoryFlowLipsyncComponent::StopAnalysis()
{
	if (!bAnalysing)
	{
		return;
	}
	bAnalysing = false;

	const FAnalysisKey Key{ AnalysingDeviceId, AnalysingSubmix };
	if (int32* Count = AnalysisRefCounts().Find(Key))
	{
		if (--(*Count) <= 0)
		{
			AnalysisRefCounts().Remove(Key);
			UAudioMixerBlueprintLibrary::StopAnalyzingOutput(this, Cast<USoundSubmix>(AnalysingSubmix.ResolveObjectPtr()));
		}
	}
}
