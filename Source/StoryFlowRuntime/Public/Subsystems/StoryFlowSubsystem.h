// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Data/StoryFlowTypes.h"
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
	 * Reset runtime characters to their default values from the project
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Characters")
	void ResetRuntimeCharacters();

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

	/** Resolve string table keys in string-type variable initial values using global string table */
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

	/** Tracks which once-only dialogue options have been used (NodeId-OptionId keys) */
	UPROPERTY()
	TSet<FString> UsedOnceOnlyOptions;

	/** Number of currently active dialogues across all components */
	int32 ActiveDialogueCount = 0;
};
