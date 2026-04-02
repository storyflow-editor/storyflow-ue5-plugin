// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowImporter.generated.h"

class UStoryFlowProjectAsset;
class UStoryFlowScriptAsset;
class UStoryFlowCharacterAsset;

/**
 * JSON importer for StoryFlow project and script files
 */
UCLASS()
class STORYFLOWEDITOR_API UStoryFlowImporter : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Import a StoryFlow project from a directory containing exported JSON files
	 *
	 * @param BuildDirectory Path to the build directory (containing project.json)
	 * @param ContentPath Unreal content path where assets will be created
	 * @return The imported project asset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Import")
	static UStoryFlowProjectAsset* ImportProject(const FString& BuildDirectory, const FString& ContentPath);

	/**
	 * Import a single StoryFlow script from JSON
	 *
	 * @param JsonPath Path to the script JSON file
	 * @param ContentPath Unreal content path where the asset will be created
	 * @return The imported script asset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Import")
	static UStoryFlowScriptAsset* ImportScript(const FString& JsonPath, const FString& ContentPath);

	/**
	 * Import a StoryFlow project from JSON data
	 *
	 * @param JsonObject The parsed JSON object
	 * @param BuildDirectory Base directory for resolving script paths
	 * @param ContentPath Unreal content path where assets will be created
	 * @return The imported project asset, or nullptr on failure
	 */
	static UStoryFlowProjectAsset* ImportProjectFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& BuildDirectory, const FString& ContentPath);

	/**
	 * Import a StoryFlow script from JSON data
	 *
	 * @param JsonObject The parsed JSON object
	 * @param ScriptPath Relative path of the script (for identification)
	 * @param ContentPath Unreal content path where the asset will be created
	 * @return The imported script asset, or nullptr on failure
	 */
	static UStoryFlowScriptAsset* ImportScriptFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& ScriptPath, const FString& ContentPath);

private:
	// === Parsing Helpers ===

	/** Parse nodes from JSON object */
	static void ParseNodes(const TSharedPtr<FJsonObject>& NodesObject, TMap<FString, FStoryFlowNode>& OutNodes);

	/** Parse a single node from JSON */
	static FStoryFlowNode ParseNode(const FString& NodeId, const TSharedPtr<FJsonObject>& NodeObject);

	/** Parse node data from JSON */
	static FStoryFlowNodeData ParseNodeData(const TSharedPtr<FJsonObject>& NodeObject);

	/** Parse connections from JSON array */
	static void ParseConnections(const TArray<TSharedPtr<FJsonValue>>& ConnectionsArray, TArray<FStoryFlowConnection>& OutConnections);

	/** Parse variables from JSON object. When bKeyByName is true, variables are keyed by their display name (for character variables) instead of their ID. */
	static void ParseVariables(const TSharedPtr<FJsonObject>& VariablesObject, TMap<FString, FStoryFlowVariable>& OutVariables, bool bKeyByName = false);

	/** Parse a single variable from JSON */
	static FStoryFlowVariable ParseVariable(const FString& VariableId, const TSharedPtr<FJsonObject>& VariableObject);

	/** Parse string table from JSON object */
	static void ParseStrings(const TSharedPtr<FJsonObject>& StringsObject, TMap<FString, FString>& OutStrings);

	/** Parse assets from JSON object */
	static void ParseAssets(const TSharedPtr<FJsonObject>& AssetsObject, TMap<FString, FStoryFlowAsset>& OutAssets);

	/** Parse text blocks from JSON array (non-interactive text displayed in dialogue) */
	static void ParseTextBlocks(const TArray<TSharedPtr<FJsonValue>>& TextBlocksArray, TArray<FStoryFlowTextBlock>& OutTextBlocks);

	/** Parse choices from JSON array (button options) */
	static void ParseChoices(const TArray<TSharedPtr<FJsonValue>>& ChoicesArray, TArray<FStoryFlowChoice>& OutOptions);

	/** Parse a variant value from JSON */
	static FStoryFlowVariant ParseVariant(const TSharedPtr<FJsonValue>& Value, EStoryFlowVariableType ExpectedType = EStoryFlowVariableType::None);

	/** Parse project metadata from JSON */
	static FStoryFlowProjectMetadata ParseMetadata(const TSharedPtr<FJsonObject>& MetadataObject);

	// === Asset Creation ===

	/** Create a new project asset */
	static UStoryFlowProjectAsset* CreateProjectAsset(const FString& ContentPath, const FString& AssetName);

	/** Create a new script asset */
	static UStoryFlowScriptAsset* CreateScriptAsset(const FString& ContentPath, const FString& AssetName);

	/** Create a new character asset */
	static UStoryFlowCharacterAsset* CreateCharacterAsset(const FString& ContentPath, const FString& AssetName);

	// === File Helpers ===

	/** Load and parse a JSON file */
	static TSharedPtr<FJsonObject> LoadJsonFile(const FString& FilePath);

	// === Media Import ===

	/**
	 * Import media assets from the build directory into Unreal content
	 *
	 * @param BuildDirectory Source directory containing exported media files
	 * @param ContentPath Base Unreal content path (type-specific subdirectories Textures/Audio are created automatically)
	 * @param Assets Map of asset metadata to process
	 * @param OutResolvedAssets Output map of imported asset references
	 */
	static void ImportMediaAssets(
		const FString& BuildDirectory,
		const FString& ContentPath,
		const TMap<FString, FStoryFlowAsset>& Assets,
		TMap<FString, TSoftObjectPtr<UObject>>& OutResolvedAssets);

	/**
	 * Import a single image file as UTexture2D
	 *
	 * @param SourcePath Full path to source image file
	 * @param ContentPath Target Unreal content path
	 * @param AssetName Name for the imported asset
	 * @return The imported texture, or nullptr on failure
	 */
	static UTexture2D* ImportImageAsset(const FString& SourcePath, const FString& ContentPath, const FString& AssetName);

	/**
	 * Import a single audio file as USoundWave
	 *
	 * @param SourcePath Full path to source audio file
	 * @param ContentPath Target Unreal content path
	 * @param AssetName Name for the imported asset
	 * @return The imported sound wave, or nullptr on failure
	 */
	static USoundWave* ImportAudioAsset(const FString& SourcePath, const FString& ContentPath, const FString& AssetName);

	/** Convert asset path to valid Unreal asset name */
	static FString NormalizeAssetPath(const FString& Path);
};
