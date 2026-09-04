// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Subsystems/StoryFlowSubsystem.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetAccess.h"
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
	// The language can MOVE here (the snap branch below), and a game that swapped projects
	// mid-session needs telling. Captured before anything changes; compared at the very end.
	const FString LanguageOnEntry = CurrentLanguage;

	ProjectAsset = NewProject;

	if (ProjectAsset)
	{
		// THE LANGUAGE FIRST, because everything seeded below resolves its strings in it.
		// The player's choice SURVIVES a re-set of a project that still carries it (the HTML
		// runtime's first-wins posture: re-installing content mid-game must not undo a choice);
		// a project that does not carry the current code snaps to that project's source
		// language, so a game can never be left reading a language nothing ships. For a project
		// with no localization sidecar the only code that resolves is its source language, so
		// this line is "en" -> "en" and changes nothing.
		//
		// THE SNAP BROADCAST DOES NOT HAPPEN HERE, deliberately — see the end of this block.
		// Everything below resolves its strings in the language this line just set, so a handler
		// running at this point would read the OUTGOING project's globals, characters and .sfd
		// seed under the INCOMING project's language.
		const FString Carried = ProjectAsset->ResolveLanguageCode(CurrentLanguage);
		CurrentLanguage = Carried.IsEmpty() ? ProjectAsset->SourceLanguage : Carried;

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

		// EVERYTHING IS SEEDED, so a handler can read the project it was told about. An install
		// that carried the player's choice forward moves nothing and is silent; one that SNAPPED
		// because this project cannot carry the old code fires, because that is a real change to
		// what the player is reading. At boot this usually reaches nobody, which is fine — the
		// case it exists for is a mid-session swap, and GetLanguage is how a handler learns the
		// language it started in.
		if (CurrentLanguage != LanguageOnEntry)
		{
			OnLanguageChanged.Broadcast(CurrentLanguage);
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

UStoryFlowDataAssetAsset* UStoryFlowSubsystem::FindDataAsset(const FString& IdOrName) const
{
	if (!ProjectAsset || IdOrName.IsEmpty())
	{
		return nullptr;
	}

	// The id is the key, so try it first and answer without walking anything.
	if (UStoryFlowDataAssetAsset* const* ById = ProjectAsset->DataAssets.Find(IdOrName))
	{
		return *ById;
	}

	// Then the display name, which is NOT unique — so the whole scan runs rather than stopping at
	// the first hit. Stopping early would make the answer depend on map iteration order, which is
	// the silent, per-import-order pick this exists to refuse.
	UStoryFlowDataAssetAsset* Matched = nullptr;
	for (const TPair<FString, UStoryFlowDataAssetAsset*>& Pair : ProjectAsset->DataAssets)
	{
		if (!Pair.Value || Pair.Value->Name != IdOrName)
		{
			continue;
		}
		if (Matched)
		{
			UE_LOG(LogStoryFlow, Warning,
				TEXT("StoryFlow: Data Asset name '%s' is ambiguous - it matches at least '%s' and '%s'. Use the asset id."),
				*IdOrName, *Matched->AssetId, *Pair.Value->AssetId);
			return nullptr;
		}
		Matched = Pair.Value;
	}

	// No warning on a plain miss: "is there an asset called this" is a fair question to ask, and a
	// Blueprint asking it every tick must not spam the log.
	return Matched;
}

// ============================================================================
// Data Asset Variable Access - the subsystem's door onto the shared ladder
// ============================================================================

bool UStoryFlowSubsystem::GetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssetAccess::TryGetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Boolean, CurrentLanguage, Value);
	return bFound ? Value.GetBool() : false;
}

bool UStoryFlowSubsystem::SetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool bValue)
{
	FStoryFlowVariant NewValue;
	NewValue.SetBool(bValue);
	return StoryFlowDataAssetAccess::SetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Boolean, NewValue);
}

int32 UStoryFlowSubsystem::GetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssetAccess::TryGetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Integer, CurrentLanguage, Value);
	return bFound ? Value.GetInt() : 0;
}

bool UStoryFlowSubsystem::SetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, int32 Value)
{
	FStoryFlowVariant NewValue;
	NewValue.SetInt(Value);
	return StoryFlowDataAssetAccess::SetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Integer, NewValue);
}

float UStoryFlowSubsystem::GetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssetAccess::TryGetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Float, CurrentLanguage, Value);
	return bFound ? Value.GetFloat() : 0.0f;
}

bool UStoryFlowSubsystem::SetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, float Value)
{
	FStoryFlowVariant NewValue;
	NewValue.SetFloat(Value);
	return StoryFlowDataAssetAccess::SetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Float, NewValue);
}

FString UStoryFlowSubsystem::GetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssetAccess::TryGetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::String, CurrentLanguage, Value);
	return bFound ? Value.GetString() : FString();
}

bool UStoryFlowSubsystem::SetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value)
{
	FStoryFlowVariant NewValue;
	NewValue.SetString(Value);
	return StoryFlowDataAssetAccess::SetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::String, NewValue);
}

FString UStoryFlowSubsystem::GetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssetAccess::TryGetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Enum, CurrentLanguage, Value);
	return bFound ? Value.GetString() : FString();
}

bool UStoryFlowSubsystem::SetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value)
{
	FStoryFlowVariant NewValue;
	// SetEnum, not SetString - the same reason the component gives: the seed types an enum
	// declaration's value as Enum, and an overlay entry that differed would be invisible to a
	// read and visible in the save key.
	NewValue.SetEnum(Value);
	return StoryFlowDataAssetAccess::SetScalar(*this, DataAsset, VariableName, EStoryFlowVariableType::Enum, NewValue);
}

FStoryFlowVariant UStoryFlowSubsystem::GetDataAssetVariantVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssetAccess::TryGetVariant(*this, DataAsset, VariableName, CurrentLanguage, Value);
	return bFound ? Value : FStoryFlowVariant();
}

TArray<FString> UStoryFlowSubsystem::GetDataAssetVariableNames(UStoryFlowDataAssetAsset* DataAsset)
{
	return StoryFlowDataAssetAccess::VariableNames(*this, DataAsset);
}

bool UStoryFlowSubsystem::SetDataAssetArrayVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const TArray<FStoryFlowVariant>& Elements)
{
	return StoryFlowDataAssetAccess::SetArray(*this, DataAsset, VariableName, Elements);
}

bool UStoryFlowSubsystem::SetDataAssetMapVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
	const TArray<FStoryFlowVariant>& Keys, const TArray<FStoryFlowVariant>& Values)
{
	return StoryFlowDataAssetAccess::SetMap(*this, DataAsset, VariableName, Keys, Values);
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
		// lifetime, so a reset refreshes both together (P4 contract §1.4). One qualifier:
		// save loading repopulates RuntimeCharacters WITHOUT touching the bridge — also
		// correct, because the bridge is project-derived, not runtime state, and a save
		// round-trips the same record keys the bridge points at.
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

bool UStoryFlowSubsystem::SetLanguage(const FString& LanguageCode)
{
	const FString Next = ProjectAsset ? ProjectAsset->ResolveLanguageCode(LanguageCode) : FString();
	if (Next.IsEmpty())
	{
		// NO-OP, never a fall back to the default: a typo must not move the player out of the
		// language they picked. The caller is told, and GetLanguage still answers truthfully.
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: SetLanguage - unknown language '%s', staying on '%s'"), *LanguageCode, *CurrentLanguage);
		return false;
	}

	if (Next != CurrentLanguage)
	{
		// ASSIGN, THEN BROADCAST. A handler must never observe a half-applied switch: GetLanguage
		// has to answer the new code inside the broadcast, and a handler that re-enters
		// SetLanguage has to be measured against the new value so it no-ops instead of recursing.
		CurrentLanguage = Next;
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Language set to '%s'"), *CurrentLanguage);
		OnLanguageChanged.Broadcast(CurrentLanguage);
	}
	return true;
}

TArray<FStoryFlowLanguage> UStoryFlowSubsystem::GetLanguages() const
{
	TArray<FStoryFlowLanguage> Out;
	if (!ProjectAsset || !ProjectAsset->bHasLocalization)
	{
		return Out;
	}

	// The source row's Name is its Code — see the header. Emitted only when there IS a source
	// language, so a hand-edited sidecar with a blank one cannot produce a row a picker would
	// draw and SetLanguage would then refuse.
	if (!ProjectAsset->SourceLanguage.IsEmpty())
	{
		FStoryFlowLanguage Source;
		Source.Code = ProjectAsset->SourceLanguage;
		Source.Name = ProjectAsset->SourceLanguage;
		Out.Add(Source);
	}
	Out.Append(ProjectAsset->Languages);
	return Out;
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
						Element.SetString(ProjectAsset->GetGlobalString(Key, CurrentLanguage));
					}
				}
			}
			else
			{
				FString Key = VarPair.Value.Value.GetString();
				if (!Key.IsEmpty())
				{
					VarPair.Value.Value.SetString(ProjectAsset->GetGlobalString(Key, CurrentLanguage));
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
						Entry.Value.SetString(ProjectAsset->GetGlobalString(Key, CurrentLanguage));
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
	// CurrentLanguage deliberately survives, for the reason it survives a save load (spec §9):
	// the active language is a player SETTING, not session state.
	UsedOnceOnlyOptions.Empty();
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: All runtime state reset"));
}
