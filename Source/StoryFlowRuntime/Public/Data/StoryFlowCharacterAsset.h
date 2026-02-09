// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowCharacterAsset.generated.h"

/**
 * DataAsset containing a single StoryFlow character definition
 */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowCharacterAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** String table key for character name */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Asset key for default image */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Image;

	/** Character-specific variables */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariable> Variables;

	/** Resolved Unreal asset references (character images, etc.) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, TSoftObjectPtr<UObject>> ResolvedAssets;

	/** Original character path (normalized, for identification) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString CharacterPath;
};
