// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/ObjectSaveContext.h"
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

#if WITH_EDITORONLY_DATA
	/** Hash of the StoryFlow source this asset was last successfully imported
	    and saved from; lets sync skip rewriting unchanged assets. Cleared when
	    a save fails so the next sync retries. */
	UPROPERTY()
	FString ImportedSourceHash;
#endif

	/** Unpack array variables after loading from disk */
	virtual void PostLoad() override;

	/** Pack array variables before saving */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
};
