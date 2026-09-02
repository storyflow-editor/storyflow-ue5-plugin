// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowLipsyncComponent.h"

#include "AudioMixerBlueprintLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowTypes.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Lipsync/StoryFlowVisemeMap.h"
#include "StoryFlowRuntime.h"

namespace
{
	/** How many frequencies to ask the mixer for across the band. Ample for a mouth, cheap to sample. */
	constexpr int32 AnalysisBands = 24;
}

UStoryFlowLipsyncComponent::UStoryFlowLipsyncComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UStoryFlowLipsyncComponent::BeginPlay()
{
	Super::BeginPlay();

	const StoryFlowVisemeTable::FTable Table = VisemeMap != nullptr ? VisemeMap->ToTable() : StoryFlowVisemeTable::Default();
	Driver = MakeUnique<FStoryFlowLipsyncDriver>(Table);
	ResolveFace(Table);

	// The frequencies the analysis is sampled at: evenly spaced across the driver's band, so the
	// magnitude-weighted mean index IS the normalised centroid the spec describes.
	AnalysisFrequencies.Reset();
	for (int32 Index = 0; Index < AnalysisBands; ++Index)
	{
		const float Alpha = static_cast<float>(Index) / (AnalysisBands - 1);
		AnalysisFrequencies.Add(FMath::Lerp(FStoryFlowLipsyncDriver::MinHz, FStoryFlowLipsyncDriver::MaxHz, Alpha));
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

	if (Source != nullptr)
	{
		Source->OnDialogueUpdated.AddDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueUpdated);
		Source->OnDialogueEnded.AddDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueEnded);
	}
}

void UStoryFlowLipsyncComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Source != nullptr)
	{
		Source->OnDialogueUpdated.RemoveDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueUpdated);
		Source->OnDialogueEnded.RemoveDynamic(this, &UStoryFlowLipsyncComponent::HandleDialogueEnded);
	}
	StopAnalysis();
	Super::EndPlay(EndPlayReason);
}

void UStoryFlowLipsyncComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Driver.IsValid())
	{
		return;
	}

	Driver->Strength = Strength;
	Driver->Sensitivity = Sensitivity;
	Driver->JawBias = JawBias;
	Driver->Smooth = Smoothing;

	if (bSpeaking && bAnalysing)
	{
		Magnitudes.Reset();
		UAudioMixerBlueprintLibrary::GetMagnitudeForFrequencies(this, AnalysisFrequencies, Magnitudes, AnalysisSubmix);
		Driver->AdvanceFromMagnitudes(Magnitudes, DeltaTime);
	}
	else if (bLineIsMine && bIdleMouthWithoutAudio)
	{
		Driver->AdvanceIdle(DeltaTime);
	}
	else
	{
		Driver->AdvanceSilent(DeltaTime);
	}

	ApplyWeights();
}

void UStoryFlowLipsyncComponent::StartLipsync()
{
	StartAnalysis();
	bSpeaking = true;
	if (Driver.IsValid())
	{
		Driver->ResetLevel();
	}
}

void UStoryFlowLipsyncComponent::StopLipsync()
{
	bSpeaking = false;
	bLineIsMine = false;
}

float UStoryFlowLipsyncComponent::GetLevel() const
{
	return Driver.IsValid() ? Driver->Level() : 0.0f;
}

void UStoryFlowLipsyncComponent::HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState)
{
	bLineIsMine = SpeakerIsMine();
	if (!bLineIsMine)
	{
		StopLipsync();
		return;
	}

	if (DialogueState.Audio != nullptr)
	{
		StartLipsync();
	}
	else
	{
		// No audio on this line: the idle mouth carries it while the text is read.
		bSpeaking = false;
	}
}

void UStoryFlowLipsyncComponent::HandleDialogueEnded()
{
	StopLipsync();
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
		return false;
	}
	return MyPath.Equals(Source->GetCurrentSpeakerPath(), ESearchCase::IgnoreCase);
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
	Targets.Reset();

	USceneComponent* Root = FaceRoot != nullptr ? FaceRoot.Get() : (GetOwner() != nullptr ? GetOwner()->GetRootComponent() : nullptr);
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
		const USkeletalMesh* Asset = Mesh != nullptr ? Mesh->GetSkeletalMeshAsset() : nullptr;
		if (Asset == nullptr)
		{
			continue;
		}

		FFaceTarget Target;
		Target.Mesh = Mesh;
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

	if (Targets.Num() == 0)
	{
		UE_LOG(LogStoryFlow, Warning,
			TEXT("StoryFlow: Lipsync on '%s' found no morph targets on any skeletal mesh. Point FaceRoot at the character's face."),
			*GetNameSafe(GetOwner()));
	}
	else if (Missing.Num() > 0)
	{
		TArray<FString> Names;
		for (const FName& Morph : Missing)
		{
			Names.Add(Morph.ToString());
		}
		UE_LOG(LogStoryFlow, Warning,
			TEXT("StoryFlow: Lipsync on '%s': the rig has no %s. Those parts of each pose are skipped; the rest still plays."),
			*GetNameSafe(GetOwner()), *FString::Join(Names, TEXT(", ")));
	}
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

void UStoryFlowLipsyncComponent::StartAnalysis()
{
	if (bAnalysing)
	{
		return;
	}

	if (AnalysisSubmix == nullptr)
	{
		UE_LOG(LogStoryFlow, Warning,
			TEXT("StoryFlow: Lipsync on '%s' is analysing the MASTER output because no AnalysisSubmix is set. ")
			TEXT("It will hear music and effects as well as speech. Route dialogue to its own submix and name it here."),
			*GetNameSafe(GetOwner()));
	}

	UAudioMixerBlueprintLibrary::StartAnalyzingOutput(this, AnalysisSubmix);
	bAnalysing = true;
}

void UStoryFlowLipsyncComponent::StopAnalysis()
{
	if (!bAnalysing)
	{
		return;
	}
	UAudioMixerBlueprintLibrary::StopAnalyzingOutput(this, AnalysisSubmix);
	bAnalysing = false;
}
