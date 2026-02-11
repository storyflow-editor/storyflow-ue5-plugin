// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowSaveGame.generated.h"

/**
 * Save game object for StoryFlow state persistence.
 *
 * Stores global variables, runtime characters, and once-only option tracking
 * as a JSON string. JSON is used because FStoryFlowVariant::ArrayValue is not
 * a UPROPERTY (UHT can't handle recursive struct arrays), so Unreal's built-in
 * serialization silently drops array data.
 */
UCLASS()
class STORYFLOWRUNTIME_API UStoryFlowSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** JSON-encoded save data containing globals, characters, and once-only options */
	UPROPERTY()
	FString SaveDataJson;

	/** Save format version for future compatibility */
	UPROPERTY()
	FString SaveVersion = TEXT("1");
};

/** JSON serialization helpers for StoryFlow save data */
namespace StoryFlowSaveHelpers
{
	/** Serialize global variables, runtime characters, and once-only options to a JSON string */
	FString SerializeSaveData(
		const TMap<FString, FStoryFlowVariable>& GlobalVariables,
		const TMap<FString, FStoryFlowCharacterDef>& RuntimeCharacters,
		const TSet<FString>& UsedOnceOnlyOptions);

	/** Deserialize a JSON string back into global variables, runtime characters, and once-only options */
	bool DeserializeSaveData(
		const FString& JsonString,
		TMap<FString, FStoryFlowVariable>& OutGlobalVariables,
		TMap<FString, FStoryFlowCharacterDef>& OutRuntimeCharacters,
		TSet<FString>& OutUsedOnceOnlyOptions);
}
