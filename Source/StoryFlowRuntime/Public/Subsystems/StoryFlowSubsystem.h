// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "StoryFlowSubsystem.generated.h"

class UStoryFlowProjectAsset;
class UStoryFlowScriptAsset;
class UStoryFlowDataAssetAsset;

/**
 * Fired when the language actually moves, carrying the NEW code.
 *
 * Unprefixed like the component's delegates (FOnDialogueStarted and its siblings), because a
 * second naming convention in one plugin costs more than the collision risk it avoids.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLanguageChanged, const FString&, LanguageCode);

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
	// Data Assets — finding one by id or by the name an author typed
	// ========================================================================

	/**
	 * The `.sfd` Data Asset with this id, or with this display NAME when the string is not an id.
	 *
	 * Every Data Asset accessor takes an asset REFERENCE, which a Blueprint gets by wiring the
	 * asset into a variable or pin. That is the right shape when the asset is known at design
	 * time and no shape at all when it is not — a save-slot screen, a data-driven inventory, or
	 * anything picking an asset from a string. The alternative was reaching into
	 * ProjectAsset->DataAssets with a raw `da_` id, which is the id-only half of this and asks a
	 * designer to paste hex.
	 *
	 * AN AMBIGUOUS NAME RESOLVES TO NOTHING, deliberately, and warns: two assets can share a
	 * display name, and picking one of them would be picking silently and differently per import
	 * order. Ids are unique, so an id never has this problem — which is what the warning tells the
	 * caller to use. Null for an unknown string, with no warning: asking whether an asset exists
	 * is a legitimate question, and this is how a Blueprint asks it.
	 *
	 * Matches the Godot plugin, whose accessors have always taken an id-or-name string.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variables|Data Assets")
	UStoryFlowDataAssetAsset* FindDataAsset(const FString& IdOrName) const;

	// ========================================================================
	// Data Asset Variable Access - the subsystem's own door (typed, with asset picker)
	// ========================================================================
	//
	// THE SAME ACCESSORS UStoryFlowComponent CARRIES, reachable with no component object: a pause
	// menu, an inventory screen or a save-slot list gets the subsystem in one line and reads its
	// data tables there. Both surfaces run ONE ladder (StoryFlowDataAssetAccess), so they cannot
	// answer a question two ways - the rule the engine contract states for mirrored surfaces.
	//
	// Two deliberate differences from the component's copies. NO EVALUATOR CACHE DROP after a
	// write, because this class owns no evaluator; a component with a live dialogue drops its own
	// on its own writes, and a subsystem write reaches that dialogue's memo at its next rebuild -
	// the asymmetry global-variable writes have always had. And reads run in THIS class's
	// language, where a component on a project with no sidecar falls back to its own LanguageCode
	// export; a localized project ignores the difference.

	/** Get a Data Asset's boolean variable, resolved through its parent chain and this session's writes. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool GetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Set a Data Asset's boolean variable at the referenced asset's own level; cascades to descendants. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool bValue);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	int32 GetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, int32 Value);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	float GetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, float Value);

	/** String-family read: string, image, audio or character. Enum is NOT reachable here. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	FString GetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	FString GetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value);

	/** The untyped door: any declared type, the array and map route. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	FStoryFlowVariant GetDataAssetVariantVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Every variable name the asset's chain declares, root-most first. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	TArray<FString> GetDataAssetVariableNames(UStoryFlowDataAssetAsset* DataAsset);

	/** Replace an ARRAY variable's elements, shape-gated. See the component's twin for the rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetArrayVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const TArray<FStoryFlowVariant>& Elements);

	/** Replace a MAP variable's entries from parallel key/value lists, shape-gated. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetMapVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const TArray<FStoryFlowVariant>& Keys, const TArray<FStoryFlowVariant>& Values);

	// ========================================================================
	// Localization (spec §9) — the player's language, game-wide
	// ========================================================================

	/**
	 * Switch the language every StoryFlow string is read in. True when the game is now reading
	 * `LanguageCode`.
	 *
	 * AN UNKNOWN CODE IS A NO-OP: it warns, changes nothing and returns false. Falling back to
	 * the default instead would let a typo in a Blueprint silently move the player out of the
	 * language they picked, and a caller that wants to know can read GetLanguage. The codes this
	 * accepts are exactly the rows GetLanguages returns, matched case-insensitively; a project
	 * with no localization sidecar accepts only its source language, so this is a no-op there by
	 * construction rather than by a special case.
	 *
	 * WHAT MOVES, AND WHEN. Everything resolved AT READ TIME follows immediately: dialogue titles
	 * and text, option labels, speaker names, character Name/Image doors, string values read
	 * through the string table. What a script SEEDED stays as it was seeded — the initial values
	 * of local, global and character string variables are resolved once when the script or the
	 * project is loaded (the A5 seats the character contract pins), so a mid-session switch
	 * reaches them at the next ResetGlobalVariables / ResetRuntimeCharacters / dialogue start,
	 * not before. Switching from a menu before play begins therefore lands everywhere.
	 *
	 * PERSISTENCE IS THE GAME'S. This plugin keeps the choice for the SESSION only, deliberately.
	 * It has no player-settings lane of its own: the save envelope carries story state (globals,
	 * characters, once-only options, the .sfd overlay) that a slot owns, and a language is not
	 * that kind of thing — it must survive with no save file at all, apply before any save is
	 * loaded, and not differ per slot. The HTML runtime reaches the same conclusion and stores it
	 * beside its volume settings rather than in the envelope. In Unreal that lane already exists
	 * and belongs to the game: persist the code with your own settings (UGameUserSettings or your
	 * own USaveGame) and call this once at boot.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Localization")
	bool SetLanguage(const FString& LanguageCode);

	/** The language code every StoryFlow string is currently read in. The source language until set. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Localization")
	FString GetLanguage() const { return CurrentLanguage; }

	/**
	 * The same code as a LIVE reference, for the one caller that must see a SetLanguage it was not
	 * told about: an execution context holds this by pointer (FStoryFlowExecutionContext's
	 * ActiveLanguage) so that a `.sfd` read done by a graph node mid-dialogue resolves in the
	 * language the player is in NOW, not the one the dialogue started in. C++ only, deliberately —
	 * Blueprint gets the value copy above, which is what a Blueprint can hold safely.
	 *
	 * WHY IT IS SAFE, since a raw reference to mutable state is not obviously so: it binds to the
	 * MEMBER, not to the string, and SetLanguage assigns through that member rather than replacing
	 * it — so a reassignment leaves every holder valid and looking at the new code. It is valid for
	 * the subsystem's lifetime (GameInstance scope) and NOT ONE MOMENT LONGER; nothing outliving the
	 * subsystem may hold it.
	 *
	 * DO NOT DEFENSIVELY COPY IT. A copy taken once at wiring time is precisely the bug the pointer
	 * exists to avoid: it freezes the language a context was initialized with, and every `.sfd` value
	 * that context reads afterwards answers in it, silently, for the rest of the dialogue. Sparing
	 * the per-read FString copy is a side benefit and never the reason.
	 */
	const FString& GetLanguageRef() const { return CurrentLanguage; }

	/**
	 * Every language the player can be switched to: the SOURCE language first, then the author's
	 * registry order — the list a game's own language picker draws.
	 *
	 * The source row's Name is its Code: the registry stores a display label for target languages
	 * only, because the source language is a label and its text lives in the documents themselves.
	 * EMPTY for a project with no localization sidecar, which is how a game asks "is this project
	 * localized at all" without reading a key count.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Localization")
	TArray<FStoryFlowLanguage> GetLanguages() const;

	/**
	 * Fired when the language MOVES, with the code it moved to. The one signal a game's own
	 * language menu needs: nothing in this plugin repaints text that is already on screen, so a
	 * switch reaches a read at the next read and everything else is the game's to refresh.
	 *
	 * IT FIRES WHEN THE LANGUAGE ACTUALLY MOVES, AND NEVER OTHERWISE. A refused code broadcasts
	 * nothing (it changed nothing) and neither does re-setting the language already active.
	 * SetProject broadcasts only when the install MOVED the language — which happens when the
	 * incoming project cannot carry the code the player was on, so it snaps to that project's
	 * source language.
	 *
	 * ORDERING: whatever a handler can observe is already the new state. The language is assigned
	 * before the broadcast, so GetLanguage answers the new code; and from SetProject the WHOLE
	 * install has run first, so a handler reading a .sfd value, a global or a character sees the
	 * project it was just told about. A handler that re-enters SetLanguage is measured against
	 * the new value, so it either no-ops or changes again and fires again.
	 *
	 * It lives here rather than on StoryFlowComponent, where every other event lives, for the
	 * same reason SetLanguage does: the language is one game-wide value, and a component-side
	 * event would fire once per component for one change.
	 */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Localization")
	FOnLanguageChanged OnLanguageChanged;

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

	/**
	 * The language every string lookup runs in (spec §9). "en" before a project is loaded, which
	 * is what every pre-localization export's strings are keyed by; SetProject then points it at
	 * the project's SOURCE language unless the player has already chosen a language the new
	 * project also carries.
	 */
	UPROPERTY()
	FString CurrentLanguage = TEXT("en");

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
