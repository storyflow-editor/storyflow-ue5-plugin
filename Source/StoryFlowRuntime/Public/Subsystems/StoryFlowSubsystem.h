// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "StoryFlowSubsystem.generated.h"

class UStoryFlowProjectAsset;
class UStoryFlowScriptAsset;

/**
 * Game Instance Subsystem for StoryFlow
 *
 * Manages the global project asset and shared state across all dialogue components.
 * Auto-loads project from /Game/StoryFlow/SF_Project if available.
 */
UCLASS()
class STORYFLOWRUNTIME_API UStoryFlowSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ========================================================================
	// Project Management
	// ========================================================================

	/**
	 * Get the current project asset
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Project")
	UStoryFlowProjectAsset* GetProject() const { return ProjectAsset; }

	/**
	 * Set the project asset manually (overrides auto-detect)
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Project")
	void SetProject(UStoryFlowProjectAsset* NewProject);

	/**
	 * Check if a project is loaded
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Project")
	bool HasProject() const { return ProjectAsset != nullptr; }

	/**
	 * Get a script by path from the loaded project
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Scripts")
	UStoryFlowScriptAsset* GetScript(const FString& ScriptPath) const;

	/**
	 * Get all available script paths in the project
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Scripts")
	TArray<FString> GetAllScriptPaths() const;

	// ========================================================================
	// Global Variables (shared across all components)
	// ========================================================================

	/**
	 * Get the global variables map (runtime copy)
	 */
	TMap<FString, FStoryFlowVariable>& GetGlobalVariables() { return GlobalVariables; }
	const TMap<FString, FStoryFlowVariable>& GetGlobalVariables() const { return GlobalVariables; }

	/**
	 * Reset global variables to their default values from the project
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void ResetGlobalVariables();

	// ========================================================================
	// Runtime Characters (mutable copies for character variable modifications)
	// ========================================================================

	/**
	 * Get the runtime characters map (mutable copies)
	 */
	TMap<FString, FStoryFlowCharacterDef>& GetRuntimeCharacters() { return RuntimeCharacters; }
	const TMap<FString, FStoryFlowCharacterDef>& GetRuntimeCharacters() const { return RuntimeCharacters; }

	/**
	 * The character id bridge: character FILE id (`da_`) -> RuntimeCharacters key, copied
	 * from the project's character-index.json (P4 contract §1.4). Empty on pre-P4 imports —
	 * ids then resolve nothing and the path fields stay authoritative. Const-only: the
	 * bridge is import data, never runtime state.
	 */
	const TMap<FString, FString>& GetCharacterIdToPath() const { return CharacterIdToPath; }

	/**
	 * Reset runtime characters to their default values from the project
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Characters")
	void ResetRuntimeCharacters();

	// ========================================================================
	// Data Assets (.sfd) — the seed + session overlay store (engine contract §3)
	// ========================================================================

	/**
	 * The read-only Data Asset SEED, keyed by assetId. Rebuilt from the project's imported
	 * assets at SetProject and NEVER mutated afterwards (contract §3) — session writes go to
	 * the overlay. Const-only on purpose: that rule is the whole reason the overlay exists.
	 */
	const StoryFlowDataAssets::FSeed& GetDataAssetSeed() const { return DataAssetSeed; }

	/** This session's Data Asset writes, keyed (assetId -> variableId). Cleared on reset. */
	StoryFlowDataAssets::FOverlay& GetDataAssetOverlay() { return DataAssetOverlay; }
	const StoryFlowDataAssets::FOverlay& GetDataAssetOverlay() const { return DataAssetOverlay; }

	/**
	 * Both halves as one non-owning reference, for everything that needs the pair.
	 *
	 * WRITES ARE SUBSYSTEM-WIDE, CACHE INVALIDATION IS COMPONENT-LOCAL. Every component shares
	 * these two maps, so a write through one is immediately visible to a read through any other.
	 * The evaluation cache is not shared: the component that made the write clears its own, and
	 * a different component already mid-dialogue keeps whatever its notBool / andBool memo held
	 * until its next dialogue rebuild clears it. Data Asset reads are never memoized themselves,
	 * so this only reaches conditions built ON one.
	 *
	 * This is the same asymmetry global variables have always had, not something the .sfd system
	 * introduced, and it is documented rather than fixed: the subsystem holds no evaluator
	 * handles, and a cross-component sweep would be new machinery for a case (two components in
	 * simultaneous dialogues sharing one condition) no shipping project has.
	 */
	StoryFlowDataAssets::FStoreRef GetDataAssetStore() { return { &DataAssetSeed, &DataAssetOverlay }; }

	/**
	 * Rebuild the seed from the project's imported Data Assets and clear the overlay
	 * (contract §3 init). Called by SetProject; a game restart wants ResetDataAssetOverlay.
	 */
	void ResetDataAssetSeed();

	/**
	 * Drop every session Data Asset write, leaving the seed alone (contract §3 reset).
	 * This is the game-restart semantic; the seed only changes when the project does.
	 *
	 * A BETWEEN-DIALOGUE call. It clears no evaluation cache (the subsystem holds no evaluator)
	 * and re-arms no warning latch (that is FStoryFlowExecutionContext::Reset, which runs at
	 * dialogue stop). Called mid-dialogue it still drops the writes, but a condition already
	 * memoized above a Data Asset accessor keeps its old answer until the next dialogue rebuild.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|DataAssets")
	void ResetDataAssetOverlay();

	// ========================================================================
	// Once-Only Options (persists across dialogues)
	// ========================================================================

	/**
	 * Get the used once-only options set
	 */
	TSet<FString>& GetUsedOnceOnlyOptions() { return UsedOnceOnlyOptions; }
	const TSet<FString>& GetUsedOnceOnlyOptions() const { return UsedOnceOnlyOptions; }

	// ========================================================================
	// Save / Load
	// ========================================================================

	/**
	 * Save current global state (variables, characters, once-only options) to a save slot.
	 * Only saves between-dialogue state — not mid-dialogue execution state.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Save")
	bool SaveToSlot(const FString& SlotName, int32 UserIndex = 0);

	/**
	 * Load global state from a save slot, replacing current variables, characters, and once-only tracking.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Save")
	bool LoadFromSlot(const FString& SlotName, int32 UserIndex = 0);

	/**
	 * Check if a save exists in the given slot.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Save")
	static bool DoesSaveExist(const FString& SlotName, int32 UserIndex = 0);

	/**
	 * Delete a save from the given slot.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Save")
	static bool DeleteSave(const FString& SlotName, int32 UserIndex = 0);

	// ========================================================================
	// Reset All State (for "New Game")
	// ========================================================================

	/**
	 * Reset all runtime state to defaults: global variables, runtime characters, and once-only options.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void ResetAllState();

	// ========================================================================
	// Active Dialogue Tracking
	// ========================================================================

	/**
	 * Notify the subsystem that a dialogue has started (called by StoryFlowComponent).
	 * Used to guard against loading mid-dialogue.
	 */
	void NotifyDialogueStarted() { ++ActiveDialogueCount; }

	/**
	 * Notify the subsystem that a dialogue has ended (called by StoryFlowComponent).
	 */
	void NotifyDialogueEnded() { ActiveDialogueCount = FMath::Max(0, ActiveDialogueCount - 1); }

	/**
	 * Check if any dialogue is currently active across all components.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Save")
	bool IsDialogueActive() const { return ActiveDialogueCount > 0; }

	/** Default content path for auto-loading project */
	static const FString DefaultProjectPath;

	/** Resolve string table keys in string-type variable initial values (scalar + array + string-valued map entries; map keys never resolve) using global string table */
	void ResolveStringVariableValues(TMap<FString, FStoryFlowVariable>& Variables);

private:
	/** Try to auto-load project from default location */
	void TryAutoLoadProject();

	/** The loaded project asset */
	UPROPERTY()
	TObjectPtr<UStoryFlowProjectAsset> ProjectAsset;

	/** Runtime copy of global variables (shared across all dialogues) */
	UPROPERTY()
	TMap<FString, FStoryFlowVariable> GlobalVariables;

	/** Runtime copy of characters (mutable, for character variable modifications) */
	UPROPERTY()
	TMap<FString, FStoryFlowCharacterDef> RuntimeCharacters;

	/** Character id -> RuntimeCharacters key bridge (read-only copy of the project's index) */
	UPROPERTY()
	TMap<FString, FString> CharacterIdToPath;

	/** Read-only Data Asset seed, keyed by assetId (contract §2.1 / §3) */
	UPROPERTY()
	TMap<FString, FStoryFlowDataAssetDef> DataAssetSeed;

	/**
	 * Session Data Asset writes, keyed (assetId -> variableId). Not a UPROPERTY: UHT rejects
	 * a nested TMap, and there is nothing here for the GC to keep alive — FStoryFlowVariant
	 * holds no UObject references. Serialization is manual either way (contract §7).
	 */
	StoryFlowDataAssets::FOverlay DataAssetOverlay;

	/** Tracks which once-only dialogue options have been used (NodeId-OptionId keys) */
	UPROPERTY()
	TSet<FString> UsedOnceOnlyOptions;

	/** Number of currently active dialogues across all components */
	int32 ActiveDialogueCount = 0;
};
