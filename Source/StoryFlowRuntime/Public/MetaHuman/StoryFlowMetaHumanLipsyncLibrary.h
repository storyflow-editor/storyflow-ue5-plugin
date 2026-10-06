// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "StoryFlowMetaHumanLipsyncLibrary.generated.h"

class USoundWave;
class USkeletalMesh;
class UAnimSequence;
class UStoryFlowProjectAsset;

USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowMetaHumanBakeEntry
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") TObjectPtr<USoundWave> Voice;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") TObjectPtr<USkeletalMesh> FaceMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") TObjectPtr<UAnimSequence> Animation;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") FString Fingerprint;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") bool bStale = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") FString LastError;
};

USTRUCT()
struct STORYFLOWRUNTIME_API FStoryFlowMetaHumanBakeProfile
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<USkeletalMesh> FaceMesh;
	UPROPERTY() FString CharacterId;
};

/** Generated project content. Hard animation references ensure the cooked game includes its bakes. */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowMetaHumanLipsyncLibrary : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") TObjectPtr<UStoryFlowProjectAsset> Project;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Baking") bool bAutoBakeAfterSync = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="StoryFlow|MetaHuman") TArray<FStoryFlowMetaHumanBakeEntry> Entries;
	UPROPERTY() TArray<FStoryFlowMetaHumanBakeProfile> Profiles;
	UFUNCTION(BlueprintPure, Category="Lipsync")
	UAnimSequence* FindAnimation(USoundWave* Voice, USkeletalMesh* FaceMesh) const;
	FStoryFlowMetaHumanBakeEntry* FindEntry(USoundWave* Voice, USkeletalMesh* FaceMesh);
};
