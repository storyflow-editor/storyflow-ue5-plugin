// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Lipsync/StoryFlowLipsyncComponent.h"
#include "StoryFlowMetaHumanLipsyncComponent.generated.h"

class USkeletalMeshComponent;
class UAnimInstance;
class UAnimSequence;
class USoundWave;
class UStoryFlowMetaHumanLipsyncLibrary;
class UStoryFlowProjectAsset;

/** Add to an assembled MetaHuman. SourceActor/CharacterId are inherited from StoryFlow. */
UCLASS(ClassGroup=(StoryFlow), meta=(BlueprintSpawnableComponent, DisplayName="StoryFlow MetaHuman Lipsync"))
class STORYFLOWRUNTIME_API UStoryFlowMetaHumanLipsyncComponent : public UStoryFlowLipsyncComponent
{
	GENERATED_BODY()
public:
	UStoryFlowMetaHumanLipsyncComponent();
	/** Populated by the Bake Dialogue Voices editor action. Shared across character instances. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="StoryFlow|MetaHuman|Setup")
	TObjectPtr<UStoryFlowMetaHumanLipsyncLibrary> BakeLibrary;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="StoryFlow|MetaHuman|Setup")
	TObjectPtr<UStoryFlowProjectAsset> BakingProject;
	/** Optional Epic MetaHuman Animator exports, matched automatically to StoryFlow's voice asset.
	 * Unassigned sounds use live analysis. Export mouth-only control curves for this facial rig. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="StoryFlow|MetaHuman|Baked")
	TMap<TObjectPtr<USoundWave>, TObjectPtr<UAnimSequence>> BakedAnimations;
	UFUNCTION(BlueprintPure, Category="StoryFlow|MetaHuman|Baked")
	bool IsUsingBakedAnimation() const;
	UFUNCTION(BlueprintPure, Category="StoryFlow|MetaHuman|Baked")
	float GetBakedPlaybackTime() const { return BakedPlaybackTime; }
	UFUNCTION(BlueprintPure, Category="StoryFlow|MetaHuman")
	USkeletalMeshComponent* GetDrivenFace() const { return Face.Get(); }
	UFUNCTION(BlueprintPure, Category="StoryFlow|MetaHuman")
	float GetJawControl() const;
	virtual USoundSubmix* GetAnalysisSubmix() const override;
	static TMap<FName, float> MapRigControls(const TMap<FName, float>& Weights);
	/** Checks animation ownership without changing the face's existing setup. */
	static bool CanDriveFace(USkeletalMeshComponent* Mesh, FString& OutReason);
protected:
	virtual StoryFlowVisemeTable::FTable GetDefaultVisemeTable() const override;
	virtual void BeginPlay() override;
	virtual void Deactivate() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
	virtual void ResolveFace(const StoryFlowVisemeTable::FTable& Table) override;
	virtual void ApplyWeights() override;
	virtual void ZeroOwnedMorphs() override;
private:
	void RestoreFace();
	void TrackPlayback(UAudioComponent* Audio);
	void ReleaseRouting();
	void HandlePlaybackPercent(const UAudioComponent* Audio, const USoundWave* Wave, float Percent);
	TWeakObjectPtr<UAudioComponent> PlaybackAudio;
	uint32 PlaybackPlayOrder = 0;
	FDelegateHandle PlaybackPercentHandle;
	float BakedPlaybackTime = 0.f;
	bool bHasPlaybackPosition = false;
	TWeakObjectPtr<USkeletalMeshComponent> Face;
	TWeakObjectPtr<UAudioComponent> RoutedAudio;
	uint32 RoutedPlayOrder = 0;
	UPROPERTY(Transient) TSubclassOf<UAnimInstance> OriginalAnimClass;
	UPROPERTY(Transient, DuplicateTransient) TObjectPtr<USoundSubmix> OwnedAnalysisSubmix;
	bool bOwnAnalysisSubmix = false;
	bool bWarnedFace = false;
};
