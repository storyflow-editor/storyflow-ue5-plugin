// Copyright 2026 StoryFlow. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncLibrary.h"
#include "StoryFlowMetaHumanBakeSubsystem.generated.h"

class UStoryFlowMetaHumanLipsyncComponent;
class UAudioComponent;

USTRUCT(BlueprintType)
struct FStoryFlowMetaHumanSetupStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="StoryFlow Editor|MetaHuman") FString Message;
	UPROPERTY(BlueprintReadOnly, Category="StoryFlow Editor|MetaHuman") int32 Ready = 0;
	UPROPERTY(BlueprintReadOnly, Category="StoryFlow Editor|MetaHuman") int32 Missing = 0;
	UPROPERTY(BlueprintReadOnly, Category="StoryFlow Editor|MetaHuman") int32 Changed = 0;
	UPROPERTY(BlueprintReadOnly, Category="StoryFlow Editor|MetaHuman") int32 Failed = 0;
	UPROPERTY(BlueprintReadOnly, Category="StoryFlow Editor|MetaHuman") bool bCanBake = false;
};

USTRUCT()
struct FStoryFlowMetaHumanBakeJob
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<UStoryFlowMetaHumanLipsyncLibrary> Library;
	UPROPERTY() TObjectPtr<USoundWave> Voice;
	UPROPERTY() TObjectPtr<USkeletalMesh> FaceMesh;
	UPROPERTY() FString Fingerprint;
};

/** Optional editor baking and setup. Runtime playback has no Epic solver dependency. */
UCLASS()
class STORYFLOWEDITOR_API UStoryFlowMetaHumanBakeSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	TArray<USoundWave*> DiscoverVoices(UStoryFlowProjectAsset* Project, const FString& CharacterId) const;
	UFUNCTION(BlueprintPure, Category="StoryFlow Editor|MetaHuman")
	TMap<FString, FString> GetCharacters(UStoryFlowProjectAsset* Project) const;
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	UStoryFlowProjectAsset* ResolveProject(UStoryFlowMetaHumanLipsyncComponent* Component) const;
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	bool Configure(UStoryFlowMetaHumanLipsyncComponent* Component, UStoryFlowProjectAsset* Project);
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	FStoryFlowMetaHumanSetupStatus GetStatus(UStoryFlowMetaHumanLipsyncComponent* Component);
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	bool Bake(UStoryFlowMetaHumanLipsyncComponent* Component);
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	void CancelBake();
	UFUNCTION(BlueprintPure, Category="StoryFlow Editor|MetaHuman") bool IsBaking() const { return Performance != nullptr || !Jobs.IsEmpty(); }
	UFUNCTION(BlueprintPure, Category="StoryFlow Editor|MetaHuman") FString GetProgressText() const { return ProgressText; }
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman")
	bool Preview(UStoryFlowMetaHumanLipsyncComponent* Component, USoundWave* Voice);
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman") void StopPreview();
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman") void RefreshProject(UStoryFlowProjectAsset* Project);
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman") void SetAutoBake(UStoryFlowMetaHumanLipsyncComponent* Component, bool bEnabled);
	UFUNCTION(BlueprintCallable, Category="StoryFlow Editor|MetaHuman") bool SelectCharacter(UStoryFlowMetaHumanLipsyncComponent* Component, const FString& CharacterId);
	/** Drop cached asset inspection after an authored setup changes. */
	void InvalidateStatusCache();
	static FString Fingerprint(USoundWave* Voice, USkeletalMesh* FaceMesh);
	static USkeletalMesh* FindFace(UStoryFlowMetaHumanLipsyncComponent* Component, FString& Issue);
private:
	bool Tick(float DeltaSeconds);
	void Queue(UStoryFlowMetaHumanLipsyncLibrary* Library, USkeletalMesh* Mesh, const FString& CharacterId);
	void StartNext();
	void CompleteActive();
	UFUNCTION() void ProcessingFinished();
	UFUNCTION() void OnSyncComplete(UStoryFlowProjectAsset* Project);
	FTSTicker::FDelegateHandle TickHandle;
	UPROPERTY(Transient) TArray<FStoryFlowMetaHumanBakeJob> Jobs;
	UPROPERTY(Transient) FStoryFlowMetaHumanBakeJob ActiveJob;
	UPROPERTY(Transient) TObjectPtr<UObject> Performance;
	UPROPERTY(Transient) TArray<TObjectPtr<UStoryFlowMetaHumanLipsyncLibrary>> Libraries;
	UPROPERTY(Transient) TArray<TObjectPtr<UStoryFlowProjectAsset>> PendingProjects;
	UPROPERTY(Transient) TObjectPtr<AActor> PreviewActor;
	UPROPERTY(Transient) TObjectPtr<UAudioComponent> PreviewAudio;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> PreviewAnimation;
	TWeakObjectPtr<class USkeletalMeshComponent> PreviewFace;
	void PreviewPercent(const UAudioComponent* Audio, const USoundWave* Voice, float Percent);
	FDelegateHandle PreviewPercentHandle;
	float PreviewTime = 0.f;
	bool bFinished = false, bCancelRequested = false;
	int32 Completed = 0, Total = 0, Failures = 0;
	FString ProgressText = TEXT("Ready");
	TMap<TWeakObjectPtr<UStoryFlowMetaHumanLipsyncComponent>, FStoryFlowMetaHumanSetupStatus> StatusCache;
	FDelegateHandle PropertyChangedHandle;
};
