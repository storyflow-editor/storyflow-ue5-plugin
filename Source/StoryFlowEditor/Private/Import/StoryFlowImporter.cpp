// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Import/StoryFlowImporter.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/TextureFactory.h"
#include "Engine/Texture2D.h"
#include "Sound/SoundWave.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Import/StoryFlowMp3Decoder.h"

namespace
{
	/** Save package if not in PIE. Returns true if saved, false if deferred. */
	bool SavePackageSafe(UPackage* Package, UObject* Asset)
	{
		// During PIE, packages are locked — skip save but keep dirty so they save later
		if (GEditor && GEditor->PlayWorld)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Deferring save of %s (PIE active)"), *Package->GetName());
			return false;
		}

		Package->FullyLoad();
		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs);
	}
}

UStoryFlowProjectAsset* UStoryFlowImporter::ImportProject(const FString& BuildDirectory, const FString& ContentPath)
{
	// Load project.json
	FString ProjectJsonPath = FPaths::Combine(BuildDirectory, TEXT("project.json"));
	TSharedPtr<FJsonObject> ProjectJson = LoadJsonFile(ProjectJsonPath);
	if (!ProjectJson.IsValid())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to load project.json from %s"), *ProjectJsonPath);
		return nullptr;
	}

	return ImportProjectFromJson(ProjectJson, BuildDirectory, ContentPath);
}

UStoryFlowScriptAsset* UStoryFlowImporter::ImportScript(const FString& JsonPath, const FString& ContentPath)
{
	TSharedPtr<FJsonObject> ScriptJson = LoadJsonFile(JsonPath);
	if (!ScriptJson.IsValid())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to load script from %s"), *JsonPath);
		return nullptr;
	}

	FString ScriptPath = FPaths::GetBaseFilename(JsonPath);
	return ImportScriptFromJson(ScriptJson, ScriptPath, ContentPath);
}

UStoryFlowProjectAsset* UStoryFlowImporter::ImportProjectFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& BuildDirectory, const FString& ContentPath)
{
	// Create project asset
	UStoryFlowProjectAsset* ProjectAsset = CreateProjectAsset(ContentPath, TEXT("SF_Project"));
	if (!ProjectAsset)
	{
		return nullptr;
	}

	// Parse basic fields
	if (JsonObject->HasField(TEXT("version")))
	{
		ProjectAsset->Version = JsonObject->GetStringField(TEXT("version"));
	}
	if (JsonObject->HasField(TEXT("apiVersion")))
	{
		ProjectAsset->ApiVersion = JsonObject->GetStringField(TEXT("apiVersion"));
	}
	if (JsonObject->HasField(TEXT("startupScript")))
	{
		ProjectAsset->StartupScript = NormalizeScriptPath(JsonObject->GetStringField(TEXT("startupScript")));
	}

	// Parse metadata
	if (JsonObject->HasField(TEXT("metadata")))
	{
		ProjectAsset->Metadata = ParseMetadata(JsonObject->GetObjectField(TEXT("metadata")));
	}

	// Load global variables
	FString GlobalVarsPath = FPaths::Combine(BuildDirectory, TEXT("global-variables.json"));
	if (FPaths::FileExists(GlobalVarsPath))
	{
		TSharedPtr<FJsonObject> GlobalVarsJson = LoadJsonFile(GlobalVarsPath);
		if (GlobalVarsJson.IsValid())
		{
			if (GlobalVarsJson->HasField(TEXT("variables")))
			{
				ParseVariables(GlobalVarsJson->GetObjectField(TEXT("variables")), ProjectAsset->GlobalVariables);
			}
			if (GlobalVarsJson->HasField(TEXT("strings")))
			{
				ParseStrings(GlobalVarsJson->GetObjectField(TEXT("strings")), ProjectAsset->GlobalStrings);
			}
		}
	}

	// Load characters
	FString CharactersPath = FPaths::Combine(BuildDirectory, TEXT("characters.json"));
	if (FPaths::FileExists(CharactersPath))
	{
		TSharedPtr<FJsonObject> CharactersJson = LoadJsonFile(CharactersPath);
		if (CharactersJson.IsValid())
		{
			// Merge character strings into global strings
			if (CharactersJson->HasField(TEXT("strings")))
			{
				TMap<FString, FString> CharStrings;
				ParseStrings(CharactersJson->GetObjectField(TEXT("strings")), CharStrings);
				for (const auto& Pair : CharStrings)
				{
					if (ProjectAsset->GlobalStrings.Contains(Pair.Key))
					{
						UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character string key '%s' overwrites existing global string"), *Pair.Key);
					}
					ProjectAsset->GlobalStrings.Add(Pair.Key, Pair.Value);
				}
			}

			// Parse character asset metadata (images, etc.) for import
			TMap<FString, FStoryFlowAsset> CharacterMediaAssets;
			if (CharactersJson->HasField(TEXT("assets")))
			{
				ParseAssets(CharactersJson->GetObjectField(TEXT("assets")), CharacterMediaAssets);
			}

			// Create per-character DataAssets
			if (CharactersJson->HasField(TEXT("characters")))
			{
				TSharedPtr<FJsonObject> CharactersObject = CharactersJson->GetObjectField(TEXT("characters"));
				FString CharacterContentPath = FPaths::Combine(ContentPath, TEXT("Characters"));

				for (const auto& CharPair : CharactersObject->Values)
				{
					FString CharPath = CharPair.Key;
					TSharedPtr<FJsonObject> CharObject = CharPair.Value->AsObject();
					if (!CharObject.IsValid())
					{
						continue;
					}

					// Normalize path for consistent lookup
					FString NormalizedPath = NormalizeCharacterPath(CharPath);

					// Create a safe asset name from the character path
					FString AssetName = NormalizeAssetPath(CharPath);

					UStoryFlowCharacterAsset* CharAsset = CreateCharacterAsset(CharacterContentPath, AssetName);
					if (!CharAsset)
					{
						continue;
					}

					CharAsset->CharacterPath = NormalizedPath;

					if (CharObject->HasField(TEXT("name")))
					{
						CharAsset->Name = CharObject->GetStringField(TEXT("name"));
					}
					if (CharObject->HasField(TEXT("image")))
					{
						CharAsset->Image = CharObject->GetStringField(TEXT("image"));
					}
					if (CharObject->HasField(TEXT("variables")))
					{
						ParseVariables(CharObject->GetObjectField(TEXT("variables")), CharAsset->Variables);
					}

					// Import media into shared top-level directories (e.g. /Game/StoryFlow/Textures/)
					// so the same image used by both a character and a script won't be duplicated.
					// The resolved reference is stored in the character asset's own ResolvedAssets.
					if (!CharAsset->Image.IsEmpty() && CharacterMediaAssets.Contains(CharAsset->Image))
					{
						TMap<FString, FStoryFlowAsset> ThisCharMedia;
						ThisCharMedia.Add(CharAsset->Image, CharacterMediaAssets[CharAsset->Image]);
						ImportMediaAssets(BuildDirectory, ContentPath, ThisCharMedia, CharAsset->ResolvedAssets);
					}

					// Save the character asset
					SavePackageSafe(CharAsset->GetOutermost(), CharAsset);

					ProjectAsset->Characters.Add(NormalizedPath, CharAsset);
					UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Created character asset '%s' at %s"), *CharPath, *CharAsset->GetPathName());
				}
			}
		}
	}

	// Find and import all script files
	TArray<FString> ScriptFiles;
	IFileManager::Get().FindFilesRecursive(ScriptFiles, *BuildDirectory, TEXT("*.json"), true, false);

	for (const FString& ScriptFile : ScriptFiles)
	{
		FString Filename = FPaths::GetCleanFilename(ScriptFile);

		// Skip non-script files
		if (Filename == TEXT("project.json") ||
			Filename == TEXT("global-variables.json") ||
			Filename == TEXT("characters.json"))
		{
			continue;
		}

		// Calculate relative path without .json extension
		FString RelativePath = ScriptFile;
		FPaths::MakePathRelativeTo(RelativePath, *(BuildDirectory + TEXT("/")));
		RelativePath = NormalizeScriptPath(RelativePath);

		// Import script
		TSharedPtr<FJsonObject> ScriptJson = LoadJsonFile(ScriptFile);
		if (ScriptJson.IsValid())
		{
			FString ScriptContentPath = FPaths::Combine(ContentPath, TEXT("Data"));
			UStoryFlowScriptAsset* ScriptAsset = ImportScriptFromJson(ScriptJson, RelativePath, ScriptContentPath);
			if (ScriptAsset)
			{
				// Import media assets referenced by this script
				ImportMediaAssets(BuildDirectory, ContentPath, ScriptAsset->Assets, ScriptAsset->ResolvedAssets);

				// Re-save script with resolved asset references
				SavePackageSafe(ScriptAsset->GetOutermost(), ScriptAsset);

				ProjectAsset->Scripts.Add(RelativePath, ScriptAsset);
			}
		}
	}

	// Save the project asset
	SavePackageSafe(ProjectAsset->GetOutermost(), ProjectAsset);

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Successfully imported project with %d scripts"), ProjectAsset->Scripts.Num());

	// Single GC pass after all assets are imported (instead of per-asset)
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	return ProjectAsset;
}

UStoryFlowScriptAsset* UStoryFlowImporter::ImportScriptFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& ScriptPath, const FString& ContentPath)
{
	// Create script asset — mirror subfolder structure under ContentPath
	// "chapters/intro.json" with ContentPath "/Game/StoryFlow/Data"
	//   → AssetName "intro", FullContentPath "/Game/StoryFlow/Data/chapters"
	FString AssetName = FPaths::GetBaseFilename(ScriptPath);
	FString SubDir = FPaths::GetPath(ScriptPath);
	FString FullContentPath = SubDir.IsEmpty() ? ContentPath : FPaths::Combine(ContentPath, SubDir);
	UStoryFlowScriptAsset* ScriptAsset = CreateScriptAsset(FullContentPath, AssetName);
	if (!ScriptAsset)
	{
		return nullptr;
	}

	ScriptAsset->ScriptPath = ScriptPath;

	// Parse startNode
	if (JsonObject->HasField(TEXT("startNode")))
	{
		ScriptAsset->StartNode = JsonObject->GetStringField(TEXT("startNode"));
	}

	// Parse nodes
	if (JsonObject->HasField(TEXT("nodes")))
	{
		ParseNodes(JsonObject->GetObjectField(TEXT("nodes")), ScriptAsset->Nodes);
	}

	// Parse connections
	if (JsonObject->HasField(TEXT("connections")))
	{
		ParseConnections(JsonObject->GetArrayField(TEXT("connections")), ScriptAsset->Connections);
	}

	// Parse variables
	if (JsonObject->HasField(TEXT("variables")))
	{
		ParseVariables(JsonObject->GetObjectField(TEXT("variables")), ScriptAsset->Variables);
	}

	// Parse strings
	if (JsonObject->HasField(TEXT("strings")))
	{
		ParseStrings(JsonObject->GetObjectField(TEXT("strings")), ScriptAsset->Strings);
	}

	// Parse assets
	if (JsonObject->HasField(TEXT("assets")))
	{
		ParseAssets(JsonObject->GetObjectField(TEXT("assets")), ScriptAsset->Assets);
	}

	// Build connection index maps for O(1) lookups at runtime
	ScriptAsset->BuildConnectionIndices();

	// Mark the object as needing save
	ScriptAsset->MarkPackageDirty();

	// Save the script asset
	bool bSaved = SavePackageSafe(ScriptAsset->GetOutermost(), ScriptAsset);

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Script save result: %s"), bSaved ? TEXT("SUCCESS") : TEXT("DEFERRED/FAILED"));
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Imported script %s with %d nodes"), *ScriptPath, ScriptAsset->Nodes.Num());

	return ScriptAsset;
}

// ============================================================================
// Parsing Helpers
// ============================================================================

void UStoryFlowImporter::ParseNodes(const TSharedPtr<FJsonObject>& NodesObject, TMap<FString, FStoryFlowNode>& OutNodes)
{
	for (const auto& NodePair : NodesObject->Values)
	{
		FString NodeId = NodePair.Key;
		if (NodeId.IsEmpty())
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Skipping node with empty ID"));
			continue;
		}
		TSharedPtr<FJsonObject> NodeObject = NodePair.Value->AsObject();
		if (NodeObject.IsValid())
		{
			OutNodes.Add(NodeId, ParseNode(NodeId, NodeObject));
		}
	}
}

FStoryFlowNode UStoryFlowImporter::ParseNode(const FString& NodeId, const TSharedPtr<FJsonObject>& NodeObject)
{
	FStoryFlowNode Node;
	Node.Id = NodeId;
	if (NodeObject->HasField(TEXT("type")))
	{
		Node.TypeString = NodeObject->GetStringField(TEXT("type"));
	}
	Node.Type = ParseNodeType(Node.TypeString);
	Node.Data = ParseNodeData(NodeObject);
	return Node;
}

FStoryFlowNodeData UStoryFlowImporter::ParseNodeData(const TSharedPtr<FJsonObject>& NodeObject)
{
	FStoryFlowNodeData Data;

	// Common fields
	if (NodeObject->HasField(TEXT("variable")))
	{
		Data.Variable = NodeObject->GetStringField(TEXT("variable"));
	}
	if (NodeObject->HasField(TEXT("isGlobal")))
	{
		Data.bIsGlobal = NodeObject->GetBoolField(TEXT("isGlobal"));
	}

	// Values
	if (NodeObject->HasField(TEXT("value")))
	{
		Data.Value = ParseVariant(NodeObject->TryGetField(TEXT("value")));
	}
	if (NodeObject->HasField(TEXT("value1")))
	{
		Data.Value1 = ParseVariant(NodeObject->TryGetField(TEXT("value1")));
	}
	if (NodeObject->HasField(TEXT("value2")))
	{
		Data.Value2 = ParseVariant(NodeObject->TryGetField(TEXT("value2")));
	}

	// Dialogue fields
	if (NodeObject->HasField(TEXT("title")))
	{
		Data.Title = NodeObject->GetStringField(TEXT("title"));
	}
	if (NodeObject->HasField(TEXT("text")))
	{
		Data.Text = NodeObject->GetStringField(TEXT("text"));
	}
	if (NodeObject->HasField(TEXT("image")))
	{
		Data.Image = NodeObject->GetStringField(TEXT("image"));
	}
	if (NodeObject->HasField(TEXT("imageReset")))
	{
		Data.bImageReset = NodeObject->GetBoolField(TEXT("imageReset"));
	}
	if (NodeObject->HasField(TEXT("audio")))
	{
		Data.Audio = NodeObject->GetStringField(TEXT("audio"));
	}
	if (NodeObject->HasField(TEXT("audioLoop")))
	{
		Data.bAudioLoop = NodeObject->GetBoolField(TEXT("audioLoop"));
	}
	if (NodeObject->HasField(TEXT("audioReset")))
	{
		Data.bAudioReset = NodeObject->GetBoolField(TEXT("audioReset"));
	}
	if (NodeObject->HasField(TEXT("character")))
	{
		Data.Character = NodeObject->GetStringField(TEXT("character"));
	}

	// Text blocks (non-interactive text displayed in dialogue)
	if (NodeObject->HasField(TEXT("textBlocks")))
	{
		ParseTextBlocks(NodeObject->GetArrayField(TEXT("textBlocks")), Data.TextBlocks);
	}

	// Elements (input options) - intentionally not imported, not supported in Unreal plugin

	// Button options
	if (NodeObject->HasField(TEXT("choices")))
	{
		ParseChoices(NodeObject->GetArrayField(TEXT("choices")), Data.Options);
	}

	// Input source flags
	if (NodeObject->HasField(TEXT("imageUseVarInput")))
	{
		Data.bImageUseVarInput = NodeObject->GetBoolField(TEXT("imageUseVarInput"));
	}
	if (NodeObject->HasField(TEXT("audioUseVarInput")))
	{
		Data.bAudioUseVarInput = NodeObject->GetBoolField(TEXT("audioUseVarInput"));
	}
	if (NodeObject->HasField(TEXT("characterUseVarInput")))
	{
		Data.bCharacterUseVarInput = NodeObject->GetBoolField(TEXT("characterUseVarInput"));
	}

	// Script execution
	if (NodeObject->HasField(TEXT("script")))
	{
		Data.Script = NormalizeScriptPath(NodeObject->GetStringField(TEXT("script")));
	}
	if (NodeObject->HasField(TEXT("flowId")))
	{
		Data.FlowId = NodeObject->GetStringField(TEXT("flowId"));
	}

	// Enum
	if (NodeObject->HasField(TEXT("enumVariable")))
	{
		Data.EnumVariable = NodeObject->GetStringField(TEXT("enumVariable"));
	}

	// Random Branch options
	if (NodeObject->HasField(TEXT("options")))
	{
		const TArray<TSharedPtr<FJsonValue>>& OptionsArray = NodeObject->GetArrayField(TEXT("options"));
		for (const TSharedPtr<FJsonValue>& OptionValue : OptionsArray)
		{
			TSharedPtr<FJsonObject> OptionObject = OptionValue->AsObject();
			if (OptionObject.IsValid())
			{
				FStoryFlowWeightedOption Option;
				if (OptionObject->HasField(TEXT("id")))
				{
					Option.Id = OptionObject->GetStringField(TEXT("id"));
				}
				if (OptionObject->HasField(TEXT("weight")))
				{
					Option.Weight = FMath::Max(1, static_cast<int32>(OptionObject->GetNumberField(TEXT("weight"))));
				}
				Data.RandomBranchOptions.Add(Option);
			}
		}
	}

	// Character Variable fields (for getCharacterVar/setCharacterVar nodes)
	// Export reuses the "variable" JSON field for the character variable name,
	// so only populate VariableName when characterPath is present (i.e., this is a character variable node)
	if (NodeObject->HasField(TEXT("characterPath")))
	{
		Data.CharacterPath = NodeObject->GetStringField(TEXT("characterPath"));
		Data.VariableName = Data.Variable;
	}
	if (NodeObject->HasField(TEXT("variableType")))
	{
		Data.VariableType = NodeObject->GetStringField(TEXT("variableType"));
	}

	return Data;
}

void UStoryFlowImporter::ParseConnections(const TArray<TSharedPtr<FJsonValue>>& ConnectionsArray, TArray<FStoryFlowConnection>& OutConnections)
{
	for (const TSharedPtr<FJsonValue>& ConnectionValue : ConnectionsArray)
	{
		TSharedPtr<FJsonObject> ConnectionObject = ConnectionValue->AsObject();
		if (ConnectionObject.IsValid())
		{
			FStoryFlowConnection Connection;
			if (ConnectionObject->HasField(TEXT("id")))
			{
				Connection.Id = ConnectionObject->GetStringField(TEXT("id"));
			}
			if (ConnectionObject->HasField(TEXT("source")))
			{
				Connection.Source = ConnectionObject->GetStringField(TEXT("source"));
			}
			if (ConnectionObject->HasField(TEXT("target")))
			{
				Connection.Target = ConnectionObject->GetStringField(TEXT("target"));
			}

			if (ConnectionObject->HasField(TEXT("sourceHandle")))
			{
				Connection.SourceHandle = ConnectionObject->GetStringField(TEXT("sourceHandle"));
			}
			if (ConnectionObject->HasField(TEXT("targetHandle")))
			{
				Connection.TargetHandle = ConnectionObject->GetStringField(TEXT("targetHandle"));
			}

			OutConnections.Add(Connection);
		}
	}
}

void UStoryFlowImporter::ParseVariables(const TSharedPtr<FJsonObject>& VariablesObject, TMap<FString, FStoryFlowVariable>& OutVariables)
{
	for (const auto& VarPair : VariablesObject->Values)
	{
		FString VariableId = VarPair.Key;
		TSharedPtr<FJsonObject> VarObject = VarPair.Value->AsObject();
		if (VarObject.IsValid())
		{
			OutVariables.Add(VariableId, ParseVariable(VariableId, VarObject));
		}
	}
}

FStoryFlowVariable UStoryFlowImporter::ParseVariable(const FString& VariableId, const TSharedPtr<FJsonObject>& VariableObject)
{
	FStoryFlowVariable Variable;
	Variable.Id = VariableId;

	if (VariableObject->HasField(TEXT("name")))
	{
		Variable.Name = VariableObject->GetStringField(TEXT("name"));
	}

	// Parse type
	FString TypeString;
	if (VariableObject->HasField(TEXT("type")))
	{
		TypeString = VariableObject->GetStringField(TEXT("type"));
	}
	if (TypeString == TEXT("boolean"))
	{
		Variable.Type = EStoryFlowVariableType::Boolean;
	}
	else if (TypeString == TEXT("integer"))
	{
		Variable.Type = EStoryFlowVariableType::Integer;
	}
	else if (TypeString == TEXT("float"))
	{
		Variable.Type = EStoryFlowVariableType::Float;
	}
	else if (TypeString == TEXT("string"))
	{
		Variable.Type = EStoryFlowVariableType::String;
	}
	else if (TypeString == TEXT("enum"))
	{
		Variable.Type = EStoryFlowVariableType::Enum;
	}
	else if (TypeString == TEXT("image"))
	{
		Variable.Type = EStoryFlowVariableType::Image;
	}
	else if (TypeString == TEXT("audio"))
	{
		Variable.Type = EStoryFlowVariableType::Audio;
	}
	else if (TypeString == TEXT("character"))
	{
		Variable.Type = EStoryFlowVariableType::Character;
	}

	// Parse value
	if (VariableObject->HasField(TEXT("value")))
	{
		Variable.Value = ParseVariant(VariableObject->TryGetField(TEXT("value")), Variable.Type);
	}

	// Parse array flag
	if (VariableObject->HasField(TEXT("isArray")))
	{
		Variable.bIsArray = VariableObject->GetBoolField(TEXT("isArray"));
	}

	// Parse enum values
	if (VariableObject->HasField(TEXT("enumValues")))
	{
		for (const TSharedPtr<FJsonValue>& EnumValue : VariableObject->GetArrayField(TEXT("enumValues")))
		{
			Variable.EnumValues.Add(EnumValue->AsString());
		}
	}

	return Variable;
}

void UStoryFlowImporter::ParseStrings(const TSharedPtr<FJsonObject>& StringsObject, TMap<FString, FString>& OutStrings)
{
	// Strings are nested by language code: { "en": { "key": "value" } }
	for (const auto& LangPair : StringsObject->Values)
	{
		FString LanguageCode = LangPair.Key;
		TSharedPtr<FJsonObject> LangStrings = LangPair.Value->AsObject();

		if (LangStrings.IsValid())
		{
			for (const auto& StringPair : LangStrings->Values)
			{
				// Store as "lang.key" -> "value"
				FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *StringPair.Key);
				OutStrings.Add(FullKey, StringPair.Value->AsString());
			}
		}
	}
}

void UStoryFlowImporter::ParseAssets(const TSharedPtr<FJsonObject>& AssetsObject, TMap<FString, FStoryFlowAsset>& OutAssets)
{
	for (const auto& AssetPair : AssetsObject->Values)
	{
		FString AssetId = AssetPair.Key;
		TSharedPtr<FJsonObject> AssetObject = AssetPair.Value->AsObject();

		if (AssetObject.IsValid())
		{
			FStoryFlowAsset Asset;
			Asset.Id = AssetId;
			if (AssetObject->HasField(TEXT("path")))
			{
				Asset.Path = AssetObject->GetStringField(TEXT("path"));
			}

			FString TypeString;
			if (AssetObject->HasField(TEXT("type")))
			{
				TypeString = AssetObject->GetStringField(TEXT("type"));
			}
			if (TypeString == TEXT("image"))
			{
				Asset.Type = EStoryFlowAssetType::Image;
			}
			else if (TypeString == TEXT("audio"))
			{
				Asset.Type = EStoryFlowAssetType::Audio;
			}
			else if (TypeString == TEXT("video"))
			{
				Asset.Type = EStoryFlowAssetType::Video;
			}

			OutAssets.Add(AssetId, Asset);
		}
	}
}

void UStoryFlowImporter::ParseTextBlocks(const TArray<TSharedPtr<FJsonValue>>& TextBlocksArray, TArray<FStoryFlowTextBlock>& OutTextBlocks)
{
	for (const TSharedPtr<FJsonValue>& BlockValue : TextBlocksArray)
	{
		TSharedPtr<FJsonObject> BlockObject = BlockValue->AsObject();
		if (BlockObject.IsValid())
		{
			FStoryFlowTextBlock TextBlock;
			if (BlockObject->HasField(TEXT("id")))
			{
				TextBlock.Id = BlockObject->GetStringField(TEXT("id"));
			}
			if (BlockObject->HasField(TEXT("text")))
			{
				TextBlock.Text = BlockObject->GetStringField(TEXT("text"));
			}

			OutTextBlocks.Add(TextBlock);
		}
	}
}

void UStoryFlowImporter::ParseChoices(const TArray<TSharedPtr<FJsonValue>>& ChoicesArray, TArray<FStoryFlowChoice>& OutOptions)
{
	for (const TSharedPtr<FJsonValue>& ChoiceValue : ChoicesArray)
	{
		TSharedPtr<FJsonObject> ChoiceObject = ChoiceValue->AsObject();
		if (ChoiceObject.IsValid())
		{
			FStoryFlowChoice Choice;
			if (ChoiceObject->HasField(TEXT("id")))
			{
				Choice.Id = ChoiceObject->GetStringField(TEXT("id"));
			}
			if (ChoiceObject->HasField(TEXT("text")))
			{
				Choice.Text = ChoiceObject->GetStringField(TEXT("text"));
			}

			if (ChoiceObject->HasField(TEXT("onceOnly")))
			{
				Choice.bOnceOnly = ChoiceObject->GetBoolField(TEXT("onceOnly"));
			}

			OutOptions.Add(Choice);
		}
	}
}

FStoryFlowVariant UStoryFlowImporter::ParseVariant(const TSharedPtr<FJsonValue>& Value, EStoryFlowVariableType ExpectedType)
{
	FStoryFlowVariant Variant;

	if (!Value.IsValid())
	{
		return Variant;
	}

	switch (Value->Type)
	{
	case EJson::Boolean:
		Variant.SetBool(Value->AsBool());
		break;

	case EJson::Number:
		if (ExpectedType == EStoryFlowVariableType::Float)
		{
			Variant.SetFloat(static_cast<float>(Value->AsNumber()));
		}
		else if (ExpectedType == EStoryFlowVariableType::Integer)
		{
			Variant.SetInt(static_cast<int32>(Value->AsNumber()));
		}
		else
		{
			// When type is unknown, check if the value has a fractional part
			double NumberValue = Value->AsNumber();
			if (FMath::Frac(NumberValue) != 0.0)
			{
				Variant.SetFloat(static_cast<float>(NumberValue));
			}
			else
			{
				Variant.SetInt(static_cast<int32>(NumberValue));
			}
		}
		break;

	case EJson::String:
		if (ExpectedType == EStoryFlowVariableType::Enum)
		{
			Variant.SetEnum(Value->AsString());
		}
		else
		{
			Variant.SetString(Value->AsString());
		}
		break;

	case EJson::Array:
	{
		TArray<FStoryFlowVariant> ArrayValues;
		for (const TSharedPtr<FJsonValue>& ArrayItem : Value->AsArray())
		{
			ArrayValues.Add(ParseVariant(ArrayItem, ExpectedType));
		}
		Variant.SetArray(ArrayValues);
		break;
	}

	default:
		break;
	}

	return Variant;
}

FStoryFlowProjectMetadata UStoryFlowImporter::ParseMetadata(const TSharedPtr<FJsonObject>& MetadataObject)
{
	FStoryFlowProjectMetadata Metadata;

	if (MetadataObject->HasField(TEXT("title")))
	{
		Metadata.Title = MetadataObject->GetStringField(TEXT("title"));
	}
	if (MetadataObject->HasField(TEXT("description")))
	{
		Metadata.Description = MetadataObject->GetStringField(TEXT("description"));
	}

	// Parse dates
	if (MetadataObject->HasField(TEXT("created")))
	{
		FDateTime::ParseIso8601(*MetadataObject->GetStringField(TEXT("created")), Metadata.Created);
	}
	if (MetadataObject->HasField(TEXT("modified")))
	{
		FDateTime::ParseIso8601(*MetadataObject->GetStringField(TEXT("modified")), Metadata.Modified);
	}

	return Metadata;
}

// ============================================================================
// Asset Creation
// ============================================================================

template<typename T>
static T* CreateAssetInternal(const FString& ContentPath, const FString& AssetName)
{
	FString PackagePath = FPaths::Combine(ContentPath, AssetName);

	// Check if asset already exists and delete it first to avoid "partially loaded" errors
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Deleting existing asset at %s for re-import"), *PackagePath);
		UEditorAssetLibrary::DeleteAsset(PackagePath);
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to create package %s"), *PackagePath);
		return nullptr;
	}

	Package->FullyLoad();

	T* Asset = NewObject<T>(Package, *AssetName, RF_Public | RF_Standalone);
	if (Asset)
	{
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
	}

	return Asset;
}

UStoryFlowProjectAsset* UStoryFlowImporter::CreateProjectAsset(const FString& ContentPath, const FString& AssetName)
{
	return CreateAssetInternal<UStoryFlowProjectAsset>(ContentPath, AssetName);
}

UStoryFlowScriptAsset* UStoryFlowImporter::CreateScriptAsset(const FString& ContentPath, const FString& AssetName)
{
	return CreateAssetInternal<UStoryFlowScriptAsset>(ContentPath, AssetName);
}

UStoryFlowCharacterAsset* UStoryFlowImporter::CreateCharacterAsset(const FString& ContentPath, const FString& AssetName)
{
	return CreateAssetInternal<UStoryFlowCharacterAsset>(ContentPath, AssetName);
}

TSharedPtr<FJsonObject> UStoryFlowImporter::LoadJsonFile(const FString& FilePath)
{
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return nullptr;
	}

	return JsonObject;
}

// ============================================================================
// Media Import
// ============================================================================

void UStoryFlowImporter::ImportMediaAssets(
	const FString& BuildDirectory,
	const FString& ContentPath,
	const TMap<FString, FStoryFlowAsset>& Assets,
	TMap<FString, TSoftObjectPtr<UObject>>& OutResolvedAssets)
{
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Importing %d media assets to %s"), Assets.Num(), *ContentPath);

	const bool bIsPIE = GEditor && GEditor->PlayWorld;

	for (const auto& AssetPair : Assets)
	{
		const FStoryFlowAsset& Asset = AssetPair.Value;
		FString SourcePath = FPaths::Combine(BuildDirectory, Asset.Path);

		// Compute type-specific target directory
		FString TypeDirectory;
		switch (Asset.Type)
		{
		case EStoryFlowAssetType::Image:
			TypeDirectory = FPaths::Combine(ContentPath, TEXT("Textures"));
			break;
		case EStoryFlowAssetType::Audio:
			TypeDirectory = FPaths::Combine(ContentPath, TEXT("Audio"));
			break;
		default:
			TypeDirectory = ContentPath;
			break;
		}

		// Normalize the path - convert forward slashes and remove extension
		FString AssetName = NormalizeAssetPath(Asset.Path);
		FString PackagePath = FPaths::Combine(TypeDirectory, AssetName);

		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Processing asset %s (type: %d) from %s"),
			*Asset.Id, static_cast<int32>(Asset.Type), *SourcePath);

		// During PIE, reuse existing assets (can't save to disk, and ImportAssets shows overwrite dialogs)
		if (bIsPIE)
		{
			if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
			{
				UObject* ExistingAsset = UEditorAssetLibrary::LoadAsset(PackagePath);
				if (ExistingAsset)
				{
					OutResolvedAssets.Add(Asset.Id, TSoftObjectPtr<UObject>(ExistingAsset));
					UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Reusing existing asset during PIE: %s"), *Asset.Path);
				}
			}
			else
			{
				UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Skipping new media import during PIE: %s"), *Asset.Path);
			}
			continue;
		}

		// Check if source file exists
		if (!FPaths::FileExists(SourcePath))
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Source file not found: %s"), *SourcePath);
			continue;
		}

		UObject* ImportedAsset = nullptr;

		switch (Asset.Type)
		{
		case EStoryFlowAssetType::Image:
			ImportedAsset = ImportImageAsset(SourcePath, TypeDirectory, AssetName);
			break;

		case EStoryFlowAssetType::Audio:
			ImportedAsset = ImportAudioAsset(SourcePath, TypeDirectory, AssetName);
			break;

		case EStoryFlowAssetType::Video:
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Video import not yet supported for %s"), *Asset.Path);
			break;

		default:
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Unknown asset type for %s"), *Asset.Path);
			break;
		}

		if (ImportedAsset)
		{
			OutResolvedAssets.Add(Asset.Id, TSoftObjectPtr<UObject>(ImportedAsset));
			UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Successfully imported %s -> %s"), *Asset.Path, *ImportedAsset->GetPathName());
		}
	}
}

UTexture2D* UStoryFlowImporter::ImportImageAsset(const FString& SourcePath, const FString& ContentPath, const FString& AssetName)
{
	// Check file extension
	FString Extension = FPaths::GetExtension(SourcePath).ToLower();
	if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg") &&
		Extension != TEXT("bmp") && Extension != TEXT("tga"))
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Unsupported image format: %s"), *Extension);
		return nullptr;
	}

	// Build target path
	FString PackagePath = FPaths::Combine(ContentPath, AssetName);

	// Check if asset already exists
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Image asset already exists at %s, loading existing"), *PackagePath);
		UObject* ExistingAsset = UEditorAssetLibrary::LoadAsset(PackagePath);
		return Cast<UTexture2D>(ExistingAsset);
	}

	// Get asset tools
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Create texture factory
	UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
	TextureFactory->SuppressImportOverwriteDialog();

	// Import the asset
	TArray<FString> FilesToImport;
	FilesToImport.Add(SourcePath);

	// Get the directory path for import
	FString TargetDirectory = FPaths::GetPath(PackagePath);

	TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(FilesToImport, TargetDirectory, TextureFactory, false);

	if (ImportedAssets.Num() > 0)
	{
		UTexture2D* Texture = Cast<UTexture2D>(ImportedAssets[0]);
		if (Texture)
		{
			// Rename to our expected name if needed
			FString CurrentName = Texture->GetName();
			if (CurrentName != AssetName)
			{
				FString NewPackagePath = FPaths::Combine(TargetDirectory, AssetName);
				UEditorAssetLibrary::RenameAsset(Texture->GetPathName(), NewPackagePath);
			}

			return Texture;
		}
	}

	UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to import image: %s"), *SourcePath);
	return nullptr;
}

USoundWave* UStoryFlowImporter::ImportAudioAsset(const FString& SourcePath, const FString& ContentPath, const FString& AssetName)
{
	// Check file extension
	FString Extension = FPaths::GetExtension(SourcePath).ToLower();

	// Unreal only imports WAV as USoundWave — convert MP3 on the fly
	FString ImportPath = SourcePath;
	FString TempWavPath;
	bool bNeedsCleanup = false;

	if (Extension == TEXT("mp3"))
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Converting MP3 to WAV for import: %s"), *SourcePath);

		if (StoryFlowAudio::ConvertMp3ToWav(SourcePath, TempWavPath))
		{
			ImportPath = TempWavPath;
			bNeedsCleanup = true;
		}
		else
		{
			UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to convert MP3 to WAV: %s"), *SourcePath);
			return nullptr;
		}
	}
	else if (Extension != TEXT("wav"))
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Unsupported audio format '%s'. Only WAV and MP3 are supported. File: %s"), *Extension, *SourcePath);
		return nullptr;
	}

	// Build target path
	FString PackagePath = FPaths::Combine(ContentPath, AssetName);

	// Check if asset already exists
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Audio asset already exists at %s, loading existing"), *PackagePath);
		if (bNeedsCleanup)
		{
			IFileManager::Get().Delete(*TempWavPath);
		}
		UObject* ExistingAsset = UEditorAssetLibrary::LoadAsset(PackagePath);
		return Cast<USoundWave>(ExistingAsset);
	}

	// Get asset tools
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Import the asset - WAV files will automatically use the Sound Factory
	TArray<FString> FilesToImport;
	FilesToImport.Add(ImportPath);

	// Get the directory path for import
	FString TargetDirectory = FPaths::GetPath(PackagePath);

	// For WAV files, auto-detection will use the correct SoundFactory
	TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(FilesToImport, TargetDirectory, nullptr, false);

	// Clean up temporary WAV file
	if (bNeedsCleanup)
	{
		IFileManager::Get().Delete(*TempWavPath);
	}

	if (ImportedAssets.Num() > 0)
	{
		USoundWave* Sound = Cast<USoundWave>(ImportedAssets[0]);
		if (Sound)
		{
			// Rename to our expected name if needed
			FString CurrentName = Sound->GetName();
			if (CurrentName != AssetName)
			{
				FString NewPackagePath = FPaths::Combine(TargetDirectory, AssetName);
				UEditorAssetLibrary::RenameAsset(Sound->GetPathName(), NewPackagePath);
			}

			UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Successfully imported audio: %s"), *AssetName);
			return Sound;
		}
		else
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Imported asset is not a SoundWave: %s (class: %s)"),
				*SourcePath, *ImportedAssets[0]->GetClass()->GetName());
		}
	}

	UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to import audio: %s"), *SourcePath);
	return nullptr;
}

FString UStoryFlowImporter::NormalizeAssetPath(const FString& Path)
{
	// Convert path to valid Unreal asset name
	// "images/elder.png" -> "images_elder"
	FString Result = Path;

	// Remove extension
	Result = FPaths::GetBaseFilename(Result, false);

	// Replace common separators with underscores
	Result = Result.Replace(TEXT("/"), TEXT("_"));
	Result = Result.Replace(TEXT("\\"), TEXT("_"));
	Result = Result.Replace(TEXT(" "), TEXT("_"));
	Result = Result.Replace(TEXT("-"), TEXT("_"));

	// Remove any characters not valid in Unreal asset names
	FString ValidChars = TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
	FString CleanResult;
	for (TCHAR Char : Result)
	{
		if (ValidChars.Contains(FString(1, &Char)))
		{
			CleanResult.AppendChar(Char);
		}
	}

	return CleanResult;
}

