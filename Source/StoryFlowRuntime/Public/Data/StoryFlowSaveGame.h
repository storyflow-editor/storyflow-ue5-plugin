// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Data/StoryFlowDataAssetStore.h"
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
	/**
	 * Serialize global variables, runtime characters, once-only options and the Data Asset
	 * overlay to a JSON string.
	 *
	 * The overlay rides the `dataAssets` root key (contract §7): sparse, BARE values, always
	 * present and `{}` when nothing was written — the shape the HTML runtime already persists
	 * (runtime-save.js buildEnvelope), byte-shape-identical across the four runtimes.
	 *
	 * The SEED is passed for shape, never for values: an empty array and a scalar are the same
	 * variant in C++ (FStoryFlowVariant carries the ELEMENT type, with no "is an array" flag),
	 * and only the declaration can say which one an empty container is.
	 */
	STORYFLOWRUNTIME_API FString SerializeSaveData(
		const TMap<FString, FStoryFlowVariable>& GlobalVariables,
		const TMap<FString, FStoryFlowCharacterDef>& RuntimeCharacters,
		const TSet<FString>& UsedOnceOnlyOptions,
		const StoryFlowDataAssets::FSeed& DataAssetSeed,
		const StoryFlowDataAssets::FOverlay& DataAssetOverlay);

	/**
	 * Deserialize a JSON string back into global variables, runtime characters, once-only
	 * options and the Data Asset overlay.
	 *
	 * The overlay load is REPLACE, never merge (contract §7): OutDataAssetOverlay is cleared
	 * first, so an absent or malformed `dataAssets` key — every save written before this key
	 * existed — correctly restores seed state. The saved table carries bare values, so the
	 * SEED's declarations are the type authority on the way back in, the same two-pass posture
	 * the importer takes with overrides.
	 */
	STORYFLOWRUNTIME_API bool DeserializeSaveData(
		const FString& JsonString,
		TMap<FString, FStoryFlowVariable>& OutGlobalVariables,
		TMap<FString, FStoryFlowCharacterDef>& OutRuntimeCharacters,
		TSet<FString>& OutUsedOnceOnlyOptions,
		const StoryFlowDataAssets::FSeed& DataAssetSeed,
		StoryFlowDataAssets::FOverlay& OutDataAssetOverlay);
}
