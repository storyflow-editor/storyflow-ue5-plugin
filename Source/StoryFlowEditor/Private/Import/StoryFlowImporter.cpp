// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Import/StoryFlowImporter.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Crc.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
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
#include "SourceControl/StoryFlowSourceControlUtils.h"

namespace
{
	/** Absolute-or-relative .uasset path a package saves to. False when the
	    package name is outside every mounted content root. */
	bool TryGetPackageFilename(const UPackage* Package, FString& OutFilename)
	{
		return FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), OutFilename, FPackageName::GetAssetPackageExtension());
	}

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
		FString PackageFileName;
		if (!TryGetPackageFilename(Package, PackageFileName))
		{
			UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Cannot save '%s': package name cannot be mapped to a file path"), *Package->GetName());
			return false;
		}
		const bool bFileExistedBeforeSave = FPaths::FileExists(PackageFileName);

		FString SourceControlError;
		if (!StoryFlowSourceControl::EnsureWritable(PackageFileName, SourceControlError))
		{
			UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Could not save '%s': %s. Resolve it in revision control, then sync again."), *PackageFileName, *SourceControlError);
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		// FSavePackageArgs::Error defaults to GError, whose Serialize treats any
		// message as a critical error and crashes the editor. Route save errors
		// to GWarn (as the engine's own editor save paths do) so a failed save
		// reaches the bSaved == false branch instead.
		SaveArgs.Error = GWarn;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs);
		if (!bSaved)
		{
			const FFileStatData TargetStat = IFileManager::Get().GetStatData(*PackageFileName);
			if (TargetStat.bIsValid && TargetStat.bIsReadOnly)
			{
				UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Could not save '%s': the file is not writable. Clear the read-only flag (or check the file out of source control / fix its permissions), then sync again."), *PackageFileName);
			}
			else
			{
				UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to save package '%s' to '%s'. Check file permissions and free disk space, then sync again."), *Package->GetName(), *PackageFileName);
			}
		}
		if (bSaved && !bFileExistedBeforeSave)
		{
			FString AddError;
			if (!StoryFlowSourceControl::MarkForAdd(PackageFileName, AddError))
			{
				UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Saved '%s' but could not mark it for add in revision control: %s"), *PackageFileName, *AddError);
			}
		}
		return bSaved;
	}

	/** Save an asset whose import hash was just recorded, and keep that hash only
	    if the write landed. A hash that outlived a failed save would make the next
	    sync skip an asset that was never written, so every save of a hashed asset
	    goes through here. Returns the save result. */
	bool SaveAssetRecordingHash(UPackage* Package, UObject* Asset, FString& InOutImportedSourceHash)
	{
		const bool bSaved = SavePackageSafe(Package, Asset);
		if (!bSaved)
		{
			InOutImportedSourceHash.Empty();
		}
		return bSaved;
	}

	/** Bump when import parsing or asset population changes, so assets written
	    by older plugin versions re-save once even if their source is unchanged. */
	constexpr const TCHAR* ImportHashSchemaVersion = TEXT("2");

	FString SerializeJsonCondensed(const TSharedRef<FJsonObject>& JsonObject)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(JsonObject, Writer);
		Writer->Close();
		return Out;
	}

	/** UTF-8 MD5 over the schema version and all parts. UTF-8 (not ANSI) so
	    non-Latin dialogue text hashes losslessly. The NUL after each part keeps
	    part boundaries unambiguous: without it {"ab","c"} and {"a","bc"} would
	    hash alike. */
	FString HashImportSource(const TArray<FString>& Parts)
	{
		FMD5 Md5;
		auto Feed = [&Md5](const FString& Value)
		{
			FTCHARToUTF8 Utf8(*Value);
			Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			const uint8 Separator = 0;
			Md5.Update(&Separator, 1);
		};
		Feed(ImportHashSchemaVersion);
		for (const FString& Part : Parts)
		{
			Feed(Part);
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, 16);
	}

	/** True when the package's .uasset exists on disk (skip-save requires it:
	    a deleted file must be rewritten even if the hash matches). */
	bool PackageFileExists(const UPackage* Package)
	{
		FString Filename;
		return TryGetPackageFilename(Package, Filename) && FPaths::FileExists(Filename);
	}

	/** Map an exported type string to EStoryFlowVariableType. Returns None for unknown strings. */
	EStoryFlowVariableType VariableTypeFromString(const FString& TypeString)
	{
		if (TypeString == TEXT("boolean"))
		{
			return EStoryFlowVariableType::Boolean;
		}
		if (TypeString == TEXT("integer"))
		{
			return EStoryFlowVariableType::Integer;
		}
		if (TypeString == TEXT("float"))
		{
			return EStoryFlowVariableType::Float;
		}
		if (TypeString == TEXT("string"))
		{
			return EStoryFlowVariableType::String;
		}
		if (TypeString == TEXT("enum"))
		{
			return EStoryFlowVariableType::Enum;
		}
		if (TypeString == TEXT("image"))
		{
			return EStoryFlowVariableType::Image;
		}
		if (TypeString == TEXT("audio"))
		{
			return EStoryFlowVariableType::Audio;
		}
		if (TypeString == TEXT("character"))
		{
			return EStoryFlowVariableType::Character;
		}
		if (TypeString == TEXT("map"))
		{
			return EStoryFlowVariableType::Map;
		}
		return EStoryFlowVariableType::None;
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
	// Create or reuse project asset
	UStoryFlowProjectAsset* ProjectAsset = CreateProjectAsset(ContentPath, TEXT("SF_Project"));
	if (!ProjectAsset)
	{
		return nullptr;
	}

	// Inputs that shape the project asset itself, plus (appended after the
	// scripts loop) the membership of the maps it actually ends up holding;
	// compared at the end to skip a no-op save. Script assets and character
	// assets hash separately.
	TArray<FString> ProjectHashParts;
	ProjectHashParts.Add(SerializeJsonCondensed(JsonObject.ToSharedRef()));

	// Clear all containers for in-place update (keeps the same UObject pointer)
	ProjectAsset->Scripts.Empty();
	ProjectAsset->Characters.Empty();
	ProjectAsset->GlobalVariables.Empty();
	ProjectAsset->GlobalStrings.Empty();
	ProjectAsset->ResolvedAssets.Empty();

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
			ProjectHashParts.Add(SerializeJsonCondensed(GlobalVarsJson.ToSharedRef()));

			if (GlobalVarsJson->HasField(TEXT("variables")))
			{
				ParseVariables(GlobalVarsJson->GetObjectField(TEXT("variables")), ProjectAsset->GlobalVariables);
			}
			if (GlobalVarsJson->HasField(TEXT("strings")))
			{
				ParseStrings(GlobalVarsJson->GetObjectField(TEXT("strings")), ProjectAsset->GlobalStrings);
			}
			// Import global image/audio assets (e.g. the values of global Image/Audio
			// variables) into the project pool — the shared final fallback consulted by
			// portrait, background and audio resolution. Without this a global Image
			// variable's asset key never resolves at runtime.
			if (GlobalVarsJson->HasField(TEXT("assets")))
			{
				TMap<FString, FStoryFlowAsset> GlobalAssets;
				ParseAssets(GlobalVarsJson->GetObjectField(TEXT("assets")), GlobalAssets);
				ImportMediaAssets(BuildDirectory, ContentPath, GlobalAssets, ProjectAsset->ResolvedAssets);
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
			ProjectHashParts.Add(SerializeJsonCondensed(CharactersJson.ToSharedRef()));

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

			// Parse character asset metadata (portraits, plus custom character-variable
			// image/audio values and character-map image/audio values) for import
			TMap<FString, FStoryFlowAsset> CharacterMediaAssets;
			if (CharactersJson->HasField(TEXT("assets")))
			{
				ParseAssets(CharactersJson->GetObjectField(TEXT("assets")), CharacterMediaAssets);

				// Import the ENTIRE character media set into the project pool — the shared
				// final fallback consulted by portrait, background and audio resolution
				// (there is no character-scoped audio pool). This resolves custom
				// character-variable image/audio values and character-map image/audio
				// values, not just portraits. Each character's portrait is additionally
				// imported into its own CharAsset->ResolvedAssets below, so the
				// character-pool-first portrait lookup still works; ImportMediaAssets is
				// idempotent (it loads the existing asset when one is already present),
				// so no media file is imported to disk twice.
				ImportMediaAssets(BuildDirectory, ContentPath, CharacterMediaAssets, ProjectAsset->ResolvedAssets);
			}

			// Create per-character DataAssets
			if (CharactersJson->HasField(TEXT("characters")))
			{
				TSharedPtr<FJsonObject> CharactersObject = CharactersJson->GetObjectField(TEXT("characters"));
				FString CharacterContentPath = FPaths::Combine(ContentPath, TEXT("Characters"));

				for (const auto& CharPair : CharactersObject->Values)
				{
					FString CharPath = *CharPair.Key;
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

					// A declared portrait that THIS sync could resolve but has not yet
					// been resolved must block the skip so it gets repaired; a portrait
					// the sync cannot resolve (no assets entry) must not, or the
					// character would rewrite forever. The condition mirrors the one
					// guarding the portrait import below.
					const bool bPortraitOutstanding = !CharAsset->Image.IsEmpty()
						&& CharacterMediaAssets.Contains(CharAsset->Image)
						&& !CharAsset->ResolvedAssets.Contains(CharAsset->Image);

					// Skip unchanged characters (same pattern as scripts)
					const FString CharSourceHash = HashImportSource({ SerializeJsonCondensed(CharObject.ToSharedRef()), NormalizedPath });
					if (CharAsset->ImportedSourceHash == CharSourceHash && PackageFileExists(CharAsset->GetOutermost())
						&& !bPortraitOutstanding)
					{
						CharAsset->GetOutermost()->SetDirtyFlag(false);
						ProjectAsset->Characters.Add(NormalizedPath, CharAsset);
						UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Character '%s' unchanged since last import, skipping save"), *CharPath);
						continue;
					}

					// Clear containers for in-place update
					CharAsset->Variables.Empty();
					CharAsset->ResolvedAssets.Empty();

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
						ParseVariables(CharObject->GetObjectField(TEXT("variables")), CharAsset->Variables, /*bKeyByName=*/ true);
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
					CharAsset->ImportedSourceHash = CharSourceHash;
					SaveAssetRecordingHash(CharAsset->GetOutermost(), CharAsset, CharAsset->ImportedSourceHash);

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
			bool bScriptSkippedUnchanged = false;
			UStoryFlowScriptAsset* ScriptAsset = ImportScriptFromJson(ScriptJson, RelativePath, ScriptContentPath, &bScriptSkippedUnchanged, /*bDeferSave=*/ true);
			if (ScriptAsset)
			{
				// The script's asset list derives from its JSON, and media import
				// is idempotent, so an unchanged script's ResolvedAssets are
				// already correct on the loaded asset: skip the media pass and
				// the save that exists only to persist resolved references.
				if (!bScriptSkippedUnchanged)
				{
					// Import media assets referenced by this script, then write
					// nodes, resolved references and the import hash in a single
					// save so the on-disk hash can never outrun the payload.
					ImportMediaAssets(BuildDirectory, ContentPath, ScriptAsset->Assets, ScriptAsset->ResolvedAssets);
					SaveAssetRecordingHash(ScriptAsset->GetOutermost(), ScriptAsset, ScriptAsset->ImportedSourceHash);
				}

				ProjectAsset->Scripts.Add(RelativePath, ScriptAsset);
			}
		}
	}

	// Hash the payload's actual membership, not just the source inputs: media
	// resolution and script parsing can partially fail (PIE, missing files),
	// and a skip must never certify a payload gap as up to date. The per-kind
	// prefixes keep a script path from colliding with an asset key.
	{
		TArray<FString> ScriptKeys;
		ProjectAsset->Scripts.GetKeys(ScriptKeys);
		for (const FString& Key : ScriptKeys)
		{
			ProjectHashParts.Add(TEXT("script:") + Key);
		}

		TArray<FString> CharacterKeys;
		ProjectAsset->Characters.GetKeys(CharacterKeys);
		for (const FString& Key : CharacterKeys)
		{
			ProjectHashParts.Add(TEXT("char:") + Key);
		}

		TArray<FString> AssetKeys;
		ProjectAsset->ResolvedAssets.GetKeys(AssetKeys);
		for (const FString& Key : AssetKeys)
		{
			ProjectHashParts.Add(TEXT("asset:") + Key);
		}
	}

	// Sort so the hash does not depend on file-enumeration or map order.
	// Explicitly case-sensitive: FString's operator< ignores case and
	// TArray::Sort is unstable, so parts differing only in case could otherwise
	// come out in either order and change the hash between runs.
	ProjectHashParts.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	const FString ProjectSourceHash = HashImportSource(ProjectHashParts);
	if (ProjectAsset->ImportedSourceHash == ProjectSourceHash && PackageFileExists(ProjectAsset->GetOutermost()))
	{
		ProjectAsset->GetOutermost()->SetDirtyFlag(false);
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Project asset unchanged since last import, skipping save"));
	}
	else
	{
		ProjectAsset->ImportedSourceHash = ProjectSourceHash;
		SaveAssetRecordingHash(ProjectAsset->GetOutermost(), ProjectAsset, ProjectAsset->ImportedSourceHash);
	}

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Successfully imported project with %d scripts"), ProjectAsset->Scripts.Num());

	return ProjectAsset;
}

UStoryFlowScriptAsset* UStoryFlowImporter::ImportScriptFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& ScriptPath, const FString& ContentPath, bool* bOutSkippedUnchanged, bool bDeferSave)
{
	if (bOutSkippedUnchanged)
	{
		*bOutSkippedUnchanged = false;
	}

	// Create script asset — mirror subfolder structure under ContentPath
	// "chapters/intro.json" with ContentPath "/Game/StoryFlow/Data"
	//   → AssetName "intro", FullContentPath "/Game/StoryFlow/Data/chapters"
	// User-authored file and folder names go into the package path, so segments
	// with characters invalid in package names must be sanitized: the raw names
	// produce assets the loader and cooker can never resolve (they vanish from
	// packaged builds while the editor keeps working from memory).
	const FString RawAssetName = FPaths::GetBaseFilename(ScriptPath);
	const FString RawSubDir = FPaths::GetPath(ScriptPath);
	FString AssetName = SanitizePackageNameSegment(RawAssetName);
	FString SubDir = SanitizePackageRelativePath(RawSubDir);
	if (AssetName != RawAssetName || SubDir != RawSubDir)
	{
		UE_LOG(LogStoryFlow, Warning,
			TEXT("StoryFlow: Script path '%s' contains characters that are invalid in Unreal package names; importing as '%s'. Consider renaming the script or folder in the StoryFlow editor."),
			*ScriptPath, *(SubDir.IsEmpty() ? AssetName : FPaths::Combine(SubDir, AssetName)));
	}
	FString FullContentPath = SubDir.IsEmpty() ? ContentPath : FPaths::Combine(ContentPath, SubDir);
	UStoryFlowScriptAsset* ScriptAsset = CreateScriptAsset(FullContentPath, AssetName);
	if (!ScriptAsset)
	{
		return nullptr;
	}

	// Skip the disk write when this exact source was already imported and
	// saved. CreateAssetInternal marked the reused package dirty, so clear
	// that; the loaded asset already holds identical data.
	// Connection indices (and the variant payloads PostLoad unpacks) are
	// transient, not UPROPERTYs, so they exist only because PostLoad or a full
	// import built them. Rebuilding the payloads here would be wrong, though:
	// FStoryFlowVariant::UnpackArrayFromSerialization clears the live
	// ArrayValue/MapValue before rebuilding them from the serialized blob, and
	// this asset's blob was never re-packed (PreSave does that, and there is no
	// save on this path), so re-unpacking would wipe live runtime data. Only the
	// indices are rebuilt, so the skip path returns an equally usable asset
	// whichever route produced the in-memory object.
	const FString SourceHash = HashImportSource({ SerializeJsonCondensed(JsonObject.ToSharedRef()), ScriptPath });
	if (ScriptAsset->ImportedSourceHash == SourceHash && PackageFileExists(ScriptAsset->GetOutermost()))
	{
		ScriptAsset->BuildConnectionIndices();
		ScriptAsset->GetOutermost()->SetDirtyFlag(false);
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Script '%s' unchanged since last import, skipping save"), *ScriptPath);
		if (bOutSkippedUnchanged)
		{
			*bOutSkippedUnchanged = true;
		}
		return ScriptAsset;
	}

	// Clear all containers for in-place update
	ScriptAsset->Nodes.Empty();
	ScriptAsset->Connections.Empty();
	ScriptAsset->Variables.Empty();
	ScriptAsset->Strings.Empty();
	ScriptAsset->Assets.Empty();
	ScriptAsset->ResolvedAssets.Empty();
	ScriptAsset->Flows.Empty();

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

	// Parse flows (for exit route detection)
	if (JsonObject->HasField(TEXT("flows")))
	{
		const TArray<TSharedPtr<FJsonValue>>& FlowsArray = JsonObject->GetArrayField(TEXT("flows"));
		for (const TSharedPtr<FJsonValue>& FlowValue : FlowsArray)
		{
			TSharedPtr<FJsonObject> FlowObject = FlowValue->AsObject();
			if (FlowObject.IsValid())
			{
				FStoryFlowFlowDef FlowDef;
				if (FlowObject->HasField(TEXT("id")))
				{
					FlowDef.Id = FlowObject->GetStringField(TEXT("id"));
				}
				if (FlowObject->HasField(TEXT("name")))
				{
					FlowDef.Name = FlowObject->GetStringField(TEXT("name"));
				}
				if (FlowObject->HasField(TEXT("isExit")))
				{
					FlowDef.bIsExit = FlowObject->GetBoolField(TEXT("isExit"));
				}
				ScriptAsset->Flows.Add(FlowDef);
			}
		}
	}

	// Build connection index maps for O(1) lookups at runtime
	ScriptAsset->BuildConnectionIndices();

	// Mark the object as needing save
	ScriptAsset->MarkPackageDirty();

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Imported script %s with %d nodes"), *ScriptPath, ScriptAsset->Nodes.Num());

	// Record the hash only for a successful save so a failed save (read-only
	// file, PIE deferral) retries on the next sync. In defer mode the caller
	// saves instead, and owns clearing the hash if its save fails — the package
	// is left dirty for it.
	ScriptAsset->ImportedSourceHash = SourceHash;
	if (bDeferSave)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Deferring save of script %s to the caller"), *ScriptPath);
		return ScriptAsset;
	}

	// Save the script asset
	const bool bSaved = SaveAssetRecordingHash(ScriptAsset->GetOutermost(), ScriptAsset, ScriptAsset->ImportedSourceHash);
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Script save result: %s"), bSaved ? TEXT("SUCCESS") : TEXT("DEFERRED/FAILED"));

	return ScriptAsset;
}

// ============================================================================
// Parsing Helpers
// ============================================================================

void UStoryFlowImporter::ParseNodes(const TSharedPtr<FJsonObject>& NodesObject, TMap<FString, FStoryFlowNode>& OutNodes)
{
	for (const auto& NodePair : NodesObject->Values)
	{
		FString NodeId = *NodePair.Key;
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
	if (NodeObject->HasField(TEXT("audioAdvanceOnEnd")))
	{
		Data.bAudioAdvanceOnEnd = NodeObject->GetBoolField(TEXT("audioAdvanceOnEnd"));
	}
	if (NodeObject->HasField(TEXT("audioAllowSkip")))
	{
		Data.bAudioAllowSkip = NodeObject->GetBoolField(TEXT("audioAllowSkip"));
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

	// Dialogue tags (optional array of arbitrary strings; absent or empty means no tags)
	if (NodeObject->HasField(TEXT("tags")))
	{
		for (const TSharedPtr<FJsonValue>& TagValue : NodeObject->GetArrayField(TEXT("tags")))
		{
			FString Tag;
			if (TagValue.IsValid() && TagValue->TryGetString(Tag))
			{
				Data.Tags.Add(Tag);
			}
		}
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

	// Script interface (for runScript nodes - parameters, outputs, exit routes)
	if (NodeObject->HasField(TEXT("scriptInterface")))
	{
		TSharedPtr<FJsonObject> InterfaceObject = NodeObject->GetObjectField(TEXT("scriptInterface"));
		if (InterfaceObject.IsValid())
		{
			if (InterfaceObject->HasField(TEXT("parameters")))
			{
				for (const TSharedPtr<FJsonValue>& ParamValue : InterfaceObject->GetArrayField(TEXT("parameters")))
				{
					TSharedPtr<FJsonObject> ParamObject = ParamValue->AsObject();
					if (ParamObject.IsValid())
					{
						FStoryFlowScriptInterfaceParam Param;
						if (ParamObject->HasField(TEXT("id"))) Param.Id = ParamObject->GetStringField(TEXT("id"));
						if (ParamObject->HasField(TEXT("name"))) Param.Name = ParamObject->GetStringField(TEXT("name"));
						if (ParamObject->HasField(TEXT("type"))) Param.Type = ParamObject->GetStringField(TEXT("type"));
						if (ParamObject->HasField(TEXT("isArray"))) Param.bIsArray = ParamObject->GetBoolField(TEXT("isArray"));
						// Map params carry K/V types so HandleRunScript can rebuild the K/V-bearing
						// input handle ("map-{keyType}-{valueType}-param-{id}"). Without them the
						// handle lookup misses and the map param silently passes an empty map.
						if (ParamObject->HasField(TEXT("keyType"))) Param.KeyType = ParamObject->GetStringField(TEXT("keyType"));
						if (ParamObject->HasField(TEXT("valueType"))) Param.ValueType = ParamObject->GetStringField(TEXT("valueType"));
						Data.ScriptParameters.Add(Param);
					}
				}
			}
			if (InterfaceObject->HasField(TEXT("outputs")))
			{
				for (const TSharedPtr<FJsonValue>& OutValue : InterfaceObject->GetArrayField(TEXT("outputs")))
				{
					TSharedPtr<FJsonObject> OutObject = OutValue->AsObject();
					if (OutObject.IsValid())
					{
						FStoryFlowScriptInterfaceParam Output;
						if (OutObject->HasField(TEXT("id"))) Output.Id = OutObject->GetStringField(TEXT("id"));
						if (OutObject->HasField(TEXT("name"))) Output.Name = OutObject->GetStringField(TEXT("name"));
						if (OutObject->HasField(TEXT("type"))) Output.Type = OutObject->GetStringField(TEXT("type"));
						Data.ScriptOutputs.Add(Output);
					}
				}
			}
			if (InterfaceObject->HasField(TEXT("exits")))
			{
				for (const TSharedPtr<FJsonValue>& ExitValue : InterfaceObject->GetArrayField(TEXT("exits")))
				{
					TSharedPtr<FJsonObject> ExitObject = ExitValue->AsObject();
					if (ExitObject.IsValid())
					{
						FStoryFlowScriptInterfaceExit Exit;
						if (ExitObject->HasField(TEXT("id"))) Exit.Id = ExitObject->GetStringField(TEXT("id"));
						if (ExitObject->HasField(TEXT("name"))) Exit.Name = ExitObject->GetStringField(TEXT("name"));
						Data.ScriptExits.Add(Exit);
					}
				}
			}
		}
	}

	// Enum
	if (NodeObject->HasField(TEXT("enumVariable")))
	{
		Data.EnumVariable = NodeObject->GetStringField(TEXT("enumVariable"));
	}
	if (NodeObject->HasField(TEXT("enumValues")))
	{
		for (const TSharedPtr<FJsonValue>& EnumValue : NodeObject->GetArrayField(TEXT("enumValues")))
		{
			Data.EnumValues.Add(EnumValue->AsString());
		}
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
	// Editor JSON uses "variableName", while built JSON may reuse "variable".
	// Parse independently of characterPath because the character may arrive through an input pin.
	const FString NodeType = NodeObject->HasField(TEXT("type")) ? NodeObject->GetStringField(TEXT("type")) : TEXT("");

	if (NodeType == TEXT("getCharacterVar") || NodeType == TEXT("setCharacterVar"))
	{
		if (NodeObject->HasField(TEXT("characterPath")))
		{
			Data.CharacterPath =
				NodeObject->GetStringField(TEXT("characterPath"));
		}

		if (NodeObject->HasField(TEXT("variableName")))
		{
			Data.VariableName =
				NodeObject->GetStringField(TEXT("variableName"));
		}
		else
		{
			// built StoryFlow JSON uses "variable".
			Data.VariableName = Data.Variable;
		}
	}

	if (NodeObject->HasField(TEXT("variableType")))
	{
		Data.VariableType = NodeObject->GetStringField(TEXT("variableType"));
	}
	if (NodeObject->HasField(TEXT("isArray")))
	{
		Data.bIsArray = NodeObject->GetBoolField(TEXT("isArray"));
	}

	// Map fields (per-variable map nodes and catalog op nodes)
	if (NodeObject->HasField(TEXT("keyType")))
	{
		Data.KeyType = NodeObject->GetStringField(TEXT("keyType"));
	}
	if (NodeObject->HasField(TEXT("valueType")))
	{
		Data.ValueType = NodeObject->GetStringField(TEXT("valueType"));
	}

	// Inline fallbacks for catalog op nodes (used when the corresponding input handle is unwired)
	if (NodeObject->HasField(TEXT("key")))
	{
		// Inline keys are always raw (never strings-table keys). Coerce off the DECLARED
		// keyType — not the JSON value's type — so node-inline keys and variable-entry keys
		// (ParseMapEntries) share one coercion strategy and numeric-string keys can't diverge.
		const TSharedPtr<FJsonValue> KeyField = NodeObject->TryGetField(TEXT("key"));
		if (KeyField.IsValid())
		{
			if (Data.KeyType == TEXT("integer"))
			{
				Data.MapKey.SetInt(static_cast<int32>(KeyField->AsNumber()));
			}
			else
			{
				Data.MapKey.SetString(KeyField->AsString());
			}
		}
	}
	if (!Data.ValueType.IsEmpty() && NodeObject->HasField(TEXT("value")))
	{
		// Gated on valueType because scalar nodes share the "value" JSON field. For string
		// values this stores the exported strings-table key verbatim (resolved at read time)
		Data.MapInlineValue = ParseVariant(NodeObject->TryGetField(TEXT("value")), VariableTypeFromString(Data.ValueType));
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

void UStoryFlowImporter::ParseVariables(const TSharedPtr<FJsonObject>& VariablesObject, TMap<FString, FStoryFlowVariable>& OutVariables, bool bKeyByName)
{
	for (const auto& VarPair : VariablesObject->Values)
	{
		FString VariableId = *VarPair.Key;
		TSharedPtr<FJsonObject> VarObject = VarPair.Value->AsObject();
		if (VarObject.IsValid())
		{
			FStoryFlowVariable Variable = ParseVariable(VariableId, VarObject);
			FString Key = (bKeyByName && !Variable.Name.IsEmpty()) ? Variable.Name : VariableId;
			OutVariables.Add(Key, MoveTemp(Variable));
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
	const EStoryFlowVariableType ParsedType = VariableTypeFromString(TypeString);
	if (ParsedType != EStoryFlowVariableType::None)
	{
		Variable.Type = ParsedType;
	}
	// Unknown type strings leave Type at its default, matching the old if/else chain

	// Parse map key/value types and their enum values (before the value — entry parsing depends on them)
	if (Variable.Type == EStoryFlowVariableType::Map)
	{
		if (VariableObject->HasField(TEXT("keyType")))
		{
			const EStoryFlowVariableType ParsedKeyType = VariableTypeFromString(VariableObject->GetStringField(TEXT("keyType")));
			if (ParsedKeyType != EStoryFlowVariableType::None)
			{
				Variable.KeyType = ParsedKeyType;
			}
		}
		if (VariableObject->HasField(TEXT("valueType")))
		{
			const EStoryFlowVariableType ParsedValueType = VariableTypeFromString(VariableObject->GetStringField(TEXT("valueType")));
			if (ParsedValueType != EStoryFlowVariableType::None)
			{
				Variable.ValueType = ParsedValueType;
			}
		}
		if (VariableObject->HasField(TEXT("keyEnumValues")))
		{
			for (const TSharedPtr<FJsonValue>& EnumValue : VariableObject->GetArrayField(TEXT("keyEnumValues")))
			{
				Variable.KeyEnumValues.Add(EnumValue->AsString());
			}
		}
		if (VariableObject->HasField(TEXT("valueEnumValues")))
		{
			for (const TSharedPtr<FJsonValue>& EnumValue : VariableObject->GetArrayField(TEXT("valueEnumValues")))
			{
				Variable.ValueEnumValues.Add(EnumValue->AsString());
			}
		}
	}

	// Parse value
	if (VariableObject->HasField(TEXT("value")))
	{
		if (Variable.Type == EStoryFlowVariableType::Map)
		{
			// Map values are an ordered array of {key, value} entry objects, not a scalar variant
			TArray<FStoryFlowMapEntry> Entries;
			ParseMapEntries(VariableObject->GetArrayField(TEXT("value")), Variable.KeyType, Variable.ValueType, Variable.Name.IsEmpty() ? Variable.Id : Variable.Name, Entries);
			Variable.Value.SetMap(Entries);
		}
		else
		{
			Variable.Value = ParseVariant(VariableObject->TryGetField(TEXT("value")), Variable.Type);
		}
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

	// Parse parameter/output flags (for script interface)
	if (VariableObject->HasField(TEXT("isInput")))
	{
		Variable.bIsInput = VariableObject->GetBoolField(TEXT("isInput"));
	}
	if (VariableObject->HasField(TEXT("isOutput")))
	{
		Variable.bIsOutput = VariableObject->GetBoolField(TEXT("isOutput"));
	}

	return Variable;
}

void UStoryFlowImporter::ParseMapEntries(const TArray<TSharedPtr<FJsonValue>>& EntriesArray, EStoryFlowVariableType KeyType, EStoryFlowVariableType ValueType, const FString& VariableContext, TArray<FStoryFlowMapEntry>& OutEntries)
{
	// Entry order is contractual — it is observable through mapKeys/mapValues/forEachMap
	// and must match the editor's serialized order
	for (const TSharedPtr<FJsonValue>& EntryValue : EntriesArray)
	{
		TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		if (!EntryObject.IsValid())
		{
			continue;
		}

		FStoryFlowMapEntry Entry;

		// Keys are raw values (numbers for integer keys, strings otherwise) and never
		// resolve through the strings table or asset map. An entry without a key is
		// unaddressable — skip it (matches the ParseNodes empty-id precedent).
		const TSharedPtr<FJsonValue> KeyField = EntryObject->TryGetField(TEXT("key"));
		if (!KeyField.IsValid())
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Skipping map entry with missing key in map variable '%s'"), *VariableContext);
			continue;
		}
		if (KeyType == EStoryFlowVariableType::Integer)
		{
			Entry.Key.SetInt(static_cast<int32>(KeyField->AsNumber()));
		}
		else
		{
			Entry.Key.SetString(KeyField->AsString());
		}

		// String-family values store the exported strings-table key / asset id verbatim;
		// resolution happens at read time, exactly like scalar variables
		Entry.Value = ParseVariant(EntryObject->TryGetField(TEXT("value")), ValueType);

		OutEntries.Add(Entry);
	}
}

void UStoryFlowImporter::ParseStrings(const TSharedPtr<FJsonObject>& StringsObject, TMap<FString, FString>& OutStrings)
{
	// Strings are nested by language code: { "en": { "key": "value" } }
	for (const auto& LangPair : StringsObject->Values)
	{
		FString LanguageCode = *LangPair.Key;
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
		FString AssetId = *AssetPair.Key;
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

	// Invalid package paths must not reach CreatePackage: the resulting assets
	// can never be loaded or cooked, and asset registry notification can even
	// assert (PathTree) on some malformed names, crashing the editor.
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Cannot create asset: '%s' is not a valid package path"), *PackagePath);
		return nullptr;
	}

	// Try to reuse existing asset to avoid refcount crashes during live sync
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UObject* ExistingAsset = UEditorAssetLibrary::LoadAsset(PackagePath);
		if (T* TypedAsset = Cast<T>(ExistingAsset))
		{
			UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Reusing existing asset at %s for re-import"), *PackagePath);
			TypedAsset->MarkPackageDirty();
			return TypedAsset;
		}
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
			// Fallback: asset may already exist from a previous full sync (data-only sync)
			if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
			{
				UObject* ExistingAsset = UEditorAssetLibrary::LoadAsset(PackagePath);
				if (ExistingAsset)
				{
					OutResolvedAssets.Add(Asset.Id, TSoftObjectPtr<UObject>(ExistingAsset));
					continue;
				}
			}
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
			// Configure for UI usage: no compression artifacts, no mipmaps
			Texture->CompressionSettings = TC_EditorIcon;
			Texture->MipGenSettings = TMGS_NoMipmaps;
			Texture->LODGroup = TEXTUREGROUP_UI;
			Texture->SRGB = true;
			Texture->UpdateResource();
			Texture->MarkPackageDirty();

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

FString UStoryFlowImporter::SanitizePackageNameSegment(const FString& Segment)
{
	// Object names have a stricter invalid set than package names (parens,
	// brackets, percent, ...), and the segment doubles as the asset name for
	// script files, so both sets gate the cleanup.
	const bool bValid = !FPackageName::DoesPackageNameContainInvalidCharacters(Segment)
		&& FName::IsValidXName(Segment, INVALID_OBJECTNAME_CHARACTERS);
	if (Segment.IsEmpty() || bValid)
	{
		return Segment;
	}

	FString Clean;
	Clean.Reserve(Segment.Len());
	for (TCHAR Char : Segment)
	{
		if (FChar::IsAlnum(Char) || Char == TEXT('_') || Char == TEXT('-'))
		{
			Clean.AppendChar(Char);
		}
		else if (Char == TEXT(' ') || Char == TEXT('.'))
		{
			Clean.AppendChar(TEXT('_'));
		}
		// remaining invalid characters are dropped
	}

	// Distinct source names must stay distinct after cleanup ("act 1" vs "act_1"),
	// so sanitized names carry a stable suffix derived from the original text.
	return FString::Printf(TEXT("%s_%04X"), *Clean, FCrc::StrCrc32(*Segment) & 0xFFFF);
}

FString UStoryFlowImporter::SanitizePackageRelativePath(const FString& RelPath)
{
	TArray<FString> Segments;
	RelPath.Replace(TEXT("\\"), TEXT("/")).ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/ true);
	for (FString& Segment : Segments)
	{
		Segment = SanitizePackageNameSegment(Segment);
	}
	return FString::Join(Segments, TEXT("/"));
}

