// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowProjectAsset.generated.h"

class UStoryFlowScriptAsset;
class UStoryFlowCharacterAsset;

/**
 * DataAsset containing a StoryFlow project with all its scripts
 */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowProjectAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** StoryFlow editor version */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Version;

	/** Export format version */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString ApiVersion;

	/** Project metadata */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowProjectMetadata Metadata;

	/** Entry point script path */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString StartupScript;

	/** All scripts in the project, keyed by normalized path */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, UStoryFlowScriptAsset*> Scripts;

	/** Global variables (shared across scripts) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariable> GlobalVariables;

	/** Characters (each is a separate DataAsset) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, UStoryFlowCharacterAsset*> Characters;

	/** Global string table */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FString> GlobalStrings;

	/** Resolved Unreal asset references */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, TSoftObjectPtr<UObject>> ResolvedAssets;

public:
	/** Get the startup script asset */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowScriptAsset* GetStartupScriptAsset() const;

	/** Get a script by path */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowScriptAsset* GetScriptByPath(const FString& ScriptPath) const;

	/** Get a global variable by name */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FStoryFlowVariable GetGlobalVariable(const FString& VariableName) const;

	/** Check if a global variable exists by name */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool HasGlobalVariable(const FString& VariableName) const;

	/** Get a character asset by path */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowCharacterAsset* GetCharacterAsset(const FString& CharacterPath) const;

	/** Get a global string */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetGlobalString(const FString& Key, const FString& LanguageCode = TEXT("en")) const;
};
