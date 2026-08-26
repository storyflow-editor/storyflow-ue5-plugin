// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Subsystems/StoryFlowSubsystem.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Engine/AssetManager.h"
#include "Kismet/GameplayStatics.h"

const FString UStoryFlowSubsystem::DefaultProjectPath = TEXT("/Game/StoryFlow/SF_Project");

void UStoryFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Subsystem initializing..."));

	// Try to auto-load project from default location
	TryAutoLoadProject();
}

void UStoryFlowSubsystem::Deinitialize()
{
	ProjectAsset = nullptr;
	GlobalVariables.Empty();
	RuntimeCharacters.Empty();
	CharacterIdToPath.Empty();
	DataAssetSeed.Empty();
	StoryFlowDataAssets::ResetOverlay(DataAssetOverlay);
	UsedOnceOnlyOptions.Empty();

	Super::Deinitialize();
}

void UStoryFlowSubsystem::SetProject(UStoryFlowProjectAsset* NewProject)
{
	ProjectAsset = NewProject;

	if (ProjectAsset)
	{
		// Initialize global variables from project. Detach shared map storage so
		// runtime map mutations never write into the project asset (HTML inflates
		// fresh maps from project data on LOAD_CONTENT).
		GlobalVariables = ProjectAsset->GlobalVariables;
		DeepCopyMapVariables(GlobalVariables);
		ResolveStringVariableValues(GlobalVariables);

		// Initialize runtime characters from character assets (mutable copies)
		ResetRuntimeCharacters();

		// Install the Data Asset seed and clear the overlay — contract §3 init ("runs once
		// per game session"). A fresh seed is a fresh session.
		ResetDataAssetSeed();

		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Project set: %s (%d scripts, %d global variables, %d characters, %d data assets)"),
			*ProjectAsset->GetName(),
			ProjectAsset->Scripts.Num(),
			GlobalVariables.Num(),
			RuntimeCharacters.Num(),
			DataAssetSeed.Num());

		// Log available scripts
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Available scripts:"));
		for (const auto& ScriptPair : ProjectAsset->Scripts)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   '%s' -> %s"),
				*ScriptPair.Key,
				ScriptPair.Value ? *ScriptPair.Value->GetName() : TEXT("NULL"));
		}
	}
	else
	{
		GlobalVariables.Empty();
		RuntimeCharacters.Empty();
		CharacterIdToPath.Empty();
		DataAssetSeed.Empty();
		StoryFlowDataAssets::ResetOverlay(DataAssetOverlay);
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Project cleared"));
	}
}

UStoryFlowScriptAsset* UStoryFlowSubsystem::GetScript(const FString& ScriptPath) const
{
	if (!ProjectAsset)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Cannot get script - no project loaded"));
		return nullptr;
	}

	return ProjectAsset->GetScriptByPath(ScriptPath);
}

TArray<FString> UStoryFlowSubsystem::GetAllScriptPaths() const
{
	TArray<FString> Paths;

	if (ProjectAsset)
	{
		ProjectAsset->Scripts.GetKeys(Paths);
	}

	return Paths;
}

void UStoryFlowSubsystem::ResetGlobalVariables()
{
	if (ProjectAsset)
	{
		GlobalVariables = ProjectAsset->GlobalVariables;
		DeepCopyMapVariables(GlobalVariables); // detach shared map storage from the asset
		ResolveStringVariableValues(GlobalVariables);
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Global variables reset to defaults"));
	}
}

void UStoryFlowSubsystem::ResetRuntimeCharacters()
{
	if (ProjectAsset)
	{
		RuntimeCharacters.Empty();
		// The id bridge travels with the characters it indexes: same source asset, same
		// lifetime, so a reset refreshes both together (P4 contract §1.4).
		CharacterIdToPath = ProjectAsset->CharacterIdToPath;
		for (const auto& CharPair : ProjectAsset->Characters)
		{
			if (CharPair.Value)
			{
				FStoryFlowCharacterDef CharDef;
				CharDef.Name = CharPair.Value->Name;
				CharDef.Image = CharPair.Value->Image;
				CharDef.Variables = CharPair.Value->Variables;
				DeepCopyMapVariables(CharDef.Variables); // detach shared map storage from the asset
				ResolveStringVariableValues(CharDef.Variables);
				RuntimeCharacters.Add(CharPair.Key, CharDef);
			}
		}
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Runtime characters reset to defaults"));
	}
}

void UStoryFlowSubsystem::ResetDataAssetSeed()
{
	// Contract §3: init installs the seed AND clears the overlay. Reseeding without clearing
	// would leave session writes pointing at a table that no longer describes them.
	StoryFlowDataAssets::ResetOverlay(DataAssetOverlay);

	if (!ProjectAsset)
	{
		DataAssetSeed.Empty();
		return;
	}

	StoryFlowDataAssets::BuildSeed(ProjectAsset->DataAssets, DataAssetSeed);
}

void UStoryFlowSubsystem::ResetDataAssetOverlay()
{
	StoryFlowDataAssets::ResetOverlay(DataAssetOverlay);
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Data Asset session writes cleared"));
}

void UStoryFlowSubsystem::ResolveStringVariableValues(TMap<FString, FStoryFlowVariable>& Variables)
{
	if (!ProjectAsset)
	{
		return;
	}

	for (auto& VarPair : Variables)
	{
		if (VarPair.Value.Type == EStoryFlowVariableType::String)
		{
			if (VarPair.Value.bIsArray)
			{
				for (auto& Element : VarPair.Value.Value.GetArrayMutable())
				{
					FString Key = Element.GetString();
					if (!Key.IsEmpty())
					{
						Element.SetString(ProjectAsset->GetGlobalString(Key));
					}
				}
			}
			else
			{
				FString Key = VarPair.Value.Value.GetString();
				if (!Key.IsEmpty())
				{
					VarPair.Value.Value.SetString(ProjectAsset->GetGlobalString(Key));
				}
			}
		}
		else if (VarPair.Value.Type == EStoryFlowVariableType::Map &&
			VarPair.Value.ValueType == EStoryFlowVariableType::String)
		{
			// Map entry VALUES of string valueType store exported strings-table keys
			// verbatim (importer ParseMapEntries) — resolve them at this load boundary,
			// the identical path/timing scalar string variables use. KEYS are raw
			// identifiers and must NEVER resolve, regardless of keyType. In-place
			// value rewrite preserves entry order.
			if (VarPair.Value.Value.IsMap())
			{
				for (FStoryFlowMapEntry& Entry : VarPair.Value.Value.GetMapMutable())
				{
					FString Key = Entry.Value.GetString();
					if (!Key.IsEmpty())
					{
						Entry.Value.SetString(ProjectAsset->GetGlobalString(Key));
					}
				}
			}
		}
	}
}

void UStoryFlowSubsystem::TryAutoLoadProject()
{
	// Try to load the asset
	UStoryFlowProjectAsset* LoadedProject = Cast<UStoryFlowProjectAsset>(
		StaticLoadObject(UStoryFlowProjectAsset::StaticClass(), nullptr, *DefaultProjectPath)
	);

	if (LoadedProject)
	{
		SetProject(LoadedProject);
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Auto-loaded project from %s"), *DefaultProjectPath);
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: No project found at %s. Use SetProject() or import a project to /Game/StoryFlow/"), *DefaultProjectPath);
	}
}

// ============================================================================
// Save / Load
// ============================================================================

bool UStoryFlowSubsystem::SaveToSlot(const FString& SlotName, int32 UserIndex)
{
	UStoryFlowSaveGame* SaveGameInstance = NewObject<UStoryFlowSaveGame>();
	// The Data Asset overlay joins the same call (contract §7): the SEED goes with it for shape
	// only — an empty array and a scalar are the same variant, and only a declaration tells them
	// apart. The seed's VALUES never reach a save; the export carries those.
	SaveGameInstance->SaveDataJson = StoryFlowSaveHelpers::SerializeSaveData(GlobalVariables, RuntimeCharacters, UsedOnceOnlyOptions, DataAssetSeed, DataAssetOverlay);

	if (UGameplayStatics::SaveGameToSlot(SaveGameInstance, SlotName, UserIndex))
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Saved to slot '%s' (%d globals, %d characters, %d once-only, %d data assets written)"),
			*SlotName, GlobalVariables.Num(), RuntimeCharacters.Num(), UsedOnceOnlyOptions.Num(), DataAssetOverlay.Num());
		return true;
	}

	UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to save to slot '%s'"), *SlotName);
	return false;
}

bool UStoryFlowSubsystem::LoadFromSlot(const FString& SlotName, int32 UserIndex)
{
	if (ActiveDialogueCount > 0)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Cannot load while a dialogue is active. Stop all dialogues before loading."));
		return false;
	}

	USaveGame* LoadedSave = UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex);
	if (!LoadedSave)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: No save found in slot '%s'"), *SlotName);
		return false;
	}

	UStoryFlowSaveGame* SaveGameInstance = Cast<UStoryFlowSaveGame>(LoadedSave);
	if (!SaveGameInstance)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Save in slot '%s' is not a StoryFlow save"), *SlotName);
		return false;
	}

	// The overlay is REPLACED in place, never merged (contract §7): DeserializeSaveData clears it
	// before applying the saved table, so a save from before this key existed — or one whose key
	// is malformed — restores seed state rather than leaving this session's writes layered over
	// freshly loaded state. The seed goes in as the type authority for the saved bare values.
	if (!StoryFlowSaveHelpers::DeserializeSaveData(SaveGameInstance->SaveDataJson, GlobalVariables, RuntimeCharacters, UsedOnceOnlyOptions, DataAssetSeed, DataAssetOverlay))
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to parse save data from slot '%s'"), *SlotName);
		return false;
	}

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Loaded from slot '%s' (%d globals, %d characters, %d once-only, %d data assets written)"),
		*SlotName, GlobalVariables.Num(), RuntimeCharacters.Num(), UsedOnceOnlyOptions.Num(), DataAssetOverlay.Num());
	return true;
}

bool UStoryFlowSubsystem::DoesSaveExist(const FString& SlotName, int32 UserIndex)
{
	return UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex);
}

bool UStoryFlowSubsystem::DeleteSave(const FString& SlotName, int32 UserIndex)
{
	if (!UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex))
	{
		return false;
	}

	return UGameplayStatics::DeleteGameInSlot(SlotName, UserIndex);
}

void UStoryFlowSubsystem::ResetAllState()
{
	ResetGlobalVariables();
	ResetRuntimeCharacters();
	// Contract §3: reset clears the OVERLAY only — the seed is content, not state.
	ResetDataAssetOverlay();
	UsedOnceOnlyOptions.Empty();
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: All runtime state reset"));
}
