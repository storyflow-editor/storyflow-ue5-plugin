// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/StoryFlowTypes.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "StoryFlowComponent.generated.h"

class UStoryFlowProjectAsset;
class UStoryFlowScriptAsset;
class UStoryFlowCharacterAsset;
class UStoryFlowDataAssetAsset;
class UStoryFlowSubsystem;
class UStoryFlowDialogueWidget;
class UAudioComponent;
class USoundClass;
class USoundConcurrency;
class USoundAttenuation;
class FStoryFlowRollbackController;

// ============================================================================
// Delegates
// ============================================================================

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDialogueUpdated, const FStoryFlowDialogueState&, DialogueState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRollbackAvailabilityChanged, const FStoryFlowRollbackAvailability&, Availability);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueEnded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDialogueTagReached, const FString&, Tag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnVariableChanged, const FStoryFlowVariable&, Variable, bool, bIsGlobal);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCharacterVariableChanged, const FString&, CharacterPath, const FString&, VariableName, const FStoryFlowVariant&, Value);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScriptStarted, const FString&, ScriptPath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScriptEnded, const FString&, ScriptPath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnError, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBackgroundImageChanged, const FString&, ImagePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAudioPlayRequested, const FString&, AudioPath, bool, bLoop);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDialogueWidgetCreated, UStoryFlowDialogueWidget*, Widget);

/**
 * Main runtime component for executing StoryFlow dialogues
 *
 * Add this component to any actor that should be able to run StoryFlow scripts.
 * Select the script to use in the Details panel, then call StartDialogue() to begin.
 *
 * The global project is loaded automatically from /Game/StoryFlow/SF_Project
 * or can be set manually via the StoryFlowSubsystem.
 */
UCLASS(BlueprintType, ClassGroup=(StoryFlow), meta=(BlueprintSpawnableComponent))
class STORYFLOWRUNTIME_API UStoryFlowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStoryFlowComponent();
	~UStoryFlowComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	// ========================================================================
	// Configuration
	// ========================================================================

	/**
	 * The script to run for this actor/character.
	 * This is the path relative to the project (e.g., "npcs/elder" or "main.json")
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow", meta=(GetOptions="GetAvailableScripts"))
	FString Script;

	/**
	 * Language code for string lookup (empty = use default "en").
	 *
	 * PRE-LOCALIZATION ONLY. It is the prefix into an artifact `strings` block that carries more
	 * than one language, and it is still honored for a project exported before localization
	 * existed. Once a project ships a `localization.json`, the language is the PLAYER'S and is
	 * game-wide: UStoryFlowSubsystem::SetLanguage owns it and this field is ignored. See
	 * ActiveLanguageCode.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow")
	FString LanguageCode = TEXT("en");

	/** Enable execution trace logging ([SF-TRACE] prefix) for cross-runtime comparison */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Debug")
	bool bTraceEnabled = true;

	/** Optional dialogue widget class to auto-create when dialogue starts and destroy when it ends */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow")
	TSubclassOf<UStoryFlowDialogueWidget> DialogueWidgetClass;

	/**
	 * Who owns the auto-created dialogue widget once it exists.
	 *
	 * True (default): the component adds the widget to the viewport when dialogue
	 * starts and removes it when dialogue ends.
	 *
	 * False: the component only creates and initializes the widget, then hands it
	 * over via OnDialogueWidgetCreated. Placing it is yours (a HUD, a widget
	 * stack, a 3D widget component) and so is destroying it: at dialogue end the
	 * component drops its reference without touching the widget's placement, and
	 * OnDialogueEnded is your cue to tear it down. A dialogue that starts while an
	 * earlier widget still exists likewise only lets go of it, because that widget
	 * may still be animating out and is yours to finish. No OnDialogueEnded fires
	 * on that path (the dialogue restarted rather than ended), so the cue there is
	 * OnDialogueWidgetCreated arriving with a different widget.
	 *
	 * Two consequences of the handover. The component's reference is the only one
	 * it keeps, so a widget it lets go of is garbage collected unless you parented
	 * it or hold a reference of your own. And a widget it lets go of is detached
	 * (see UStoryFlowDialogueWidget::DetachFromComponent): it stops receiving
	 * dialogue events, and GetStoryFlowComponent returns null on it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow")
	bool bAutoAddWidgetToViewport = true;

	// ========================================================================
	// Audio Settings
	// ========================================================================

	/** Play audio as 3D sound attached to the owning actor (false = 2D non-spatialized) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Audio")
	bool bUse3DAudio = false;

	/** Stop any playing dialogue audio when dialogue ends */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Audio")
	bool bStopAudioOnDialogueEnd = true;

	/** Sound class for dialogue audio (controls volume category in audio mixer — e.g., separate "Dialogue" slider in settings) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Audio")
	TObjectPtr<USoundClass> DialogueSoundClass;

	/** Volume multiplier for dialogue audio (stacks with SoundClass mix) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Audio", meta=(ClampMin="0.0", ClampMax="2.0", UIMin="0.0", UIMax="2.0"))
	float DialogueVolumeMultiplier = 1.0f;

	/** Concurrency settings for dialogue audio (controls overlap behavior) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Audio")
	TObjectPtr<USoundConcurrency> DialogueConcurrency;

	/** Attenuation settings for 3D dialogue audio (controls distance falloff, spatialization, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Audio", meta=(EditCondition="bUse3DAudio"))
	TObjectPtr<USoundAttenuation> DialogueAttenuation;

	// ========================================================================
	// Events (Blueprint Assignable)
	// ========================================================================

	/** Called when dialogue execution starts */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnDialogueStarted OnDialogueStarted;

	/** Called when dialogue state updates (new text, options, etc.) */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnDialogueUpdated OnDialogueUpdated;
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Rollback")
	FOnDialogueUpdated OnDialogueRestored;
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Rollback")
	FOnRollbackAvailabilityChanged OnRollbackAvailabilityChanged;
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Rollback")
	bool CanGoBack() const;
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Rollback")
	FStoryFlowRollbackAvailability GetRollbackAvailability() const;
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Rollback")
	FStoryFlowRollbackResult GoBack();
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Rollback")
	void BlockRollback(const FString& Reason = TEXT(""));
	bool IsCurrentDialogueRestored() const { return ExecutionContext.bIsExecuting && ExecutionContext.CurrentDialogueState.bIsValid && RestoredEntry.IsSet() && RestoredEntry.GetValue() == DialogueEntrySerial; }
	bool IsCurrentRestoredDelivery() const { return IsCurrentDialogueRestored() && RestoredDeliveryEntry == DialogueEntrySerial; }

	/** Called when dialogue execution ends */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnDialogueEnded OnDialogueEnded;

	/** Called once per tag, in authored order, when a tagged dialogue node is entered */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnDialogueTagReached OnDialogueTagReached;

	/** Called when a variable changes */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnVariableChanged OnVariableChanged;

	/**
	 * Called when a character variable changes (Name, Image, or custom). Lets non-speaker UIs
	 * react to setCharacterVar mutations. NODE-lane writes only, a contract property per P4
	 * amendment A2(b): Blueprint/script-API writes — SetCharacterVariable and its ById twin,
	 * the typed setters, and the data-asset surface's character branch — never raise it.
	 * CharacterPath is the RESOLVED record key: an id-bound write (P4) broadcasts the key its
	 * character id bridged to (so a handler comparing the payload against a da_ id never
	 * matches), path-bound and wired writes broadcast the string the node carried exactly as
	 * before.
	 */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnCharacterVariableChanged OnCharacterVariableChanged;

	/** Called when a script starts executing */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnScriptStarted OnScriptStarted;

	/** Called when a script finishes executing */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnScriptEnded OnScriptEnded;

	/** Called when an error occurs */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnError OnError;

	/** Called when a setBackgroundImage node changes the background */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnBackgroundImageChanged OnBackgroundImageChanged;

	/** Called when a playAudio node requests audio playback */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnAudioPlayRequested OnAudioPlayRequested;

	/**
	 * Called when the component has created and initialized the dialogue widget,
	 * after it has been added to the viewport (or not, per bAutoAddWidgetToViewport),
	 * so handlers see its final placement. With auto-add off, this is where you
	 * put the widget wherever your UI keeps it.
	 */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnDialogueWidgetCreated OnDialogueWidgetCreated;

	// ========================================================================
	// Control Functions
	// ========================================================================

	/**
	 * Start dialogue execution using the configured Script
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void StartDialogue();

	/**
	 * Start dialogue with a specific script path (overrides configured Script)
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void StartDialogueWithScript(const FString& ScriptPath);

	/** Select a dialogue option by ID */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void SelectOption(const FString& OptionId);

	/** Advance a narrative-only dialogue (no options defined). Uses the header output edge. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void AdvanceDialogue();

	/** Stop dialogue execution */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void StopDialogue();

	/** Pause dialogue execution */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void PauseDialogue();

	/** Resume paused dialogue execution */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void ResumeDialogue();

	// ========================================================================
	// State Access
	// ========================================================================

	/** Get the current dialogue state */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FStoryFlowDialogueState GetCurrentDialogue() const;

	/** Changes on each fresh line entry, including repeated nodes and script calls; stable on redraws. */
	uint64 GetDialogueEntrySerial() const { return DialogueEntrySerial; }

	/**
	 * The character PATH the current line's speaker was resolved to, empty when the line has no speaker.
	 *
	 * The dialogue state carries the speaker's resolved DATA (name, portrait, variables) but nothing that
	 * identifies WHICH character it is, and the display name is localized, so it cannot be matched against.
	 * This is the id-native answer: pair it with GetCharacterPathById to ask "is my character speaking",
	 * which is what the lipsync component does. Read-only, and additive — the dialogue state's shape, which
	 * every engine plugin mirrors, is untouched.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetCurrentSpeakerPath() const { return CurrentSpeakerPath; }

	/**
	 * True while the audio THIS component started for the current line is still playing.
	 *
	 * The cue lipsync closes a mouth on. A two second line sits on a screen the player reads for twelve, and
	 * a face that keeps moving for the other ten is mouthing whatever else the mix is carrying. Answers
	 * false — not "unknown" — when the game overrode PlayDialogueAudio, which is why the audio component
	 * itself is exposed beside this: a caller that must tell "silent" from "not mine to know" can ask.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Audio")
	bool IsDialogueAudioPlaying() const;

	/** The audio component playing the current line, null when nothing is playing or the game plays its own. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Audio")
	UAudioComponent* GetCurrentDialogueAudio() const { return CurrentDialogueAudio; }

	/** Get the current dialogue's presentation tags (empty when untagged) */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	TArray<FString> GetCurrentDialogueTags() const;

	/**
	 * How many character id warnings this component's execution context has emitted — a TEST
	 * SEAM mirroring FStoryFlowExecutionContext::CharacterIdWarningsEmitted (the context member
	 * is private here, and the pre-P4 pins assert a component-driven run warned nothing).
	 */
	int32 GetCharacterIdWarningsEmitted() const { return ExecutionContext.CharacterIdWarningsEmitted; }

	/** Check if dialogue is currently active */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool IsDialogueActive() const;

	/** Check if dialogue is waiting for input */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool IsWaitingForInput() const;

	/** Check if dialogue is paused */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool IsPaused() const;

	/** Get the StoryFlow subsystem */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowSubsystem* GetStoryFlowSubsystem() const;

	/** Get the global project (from subsystem) */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowProjectAsset* GetProject() const;

	/**
	 * The dialogue widget this component created from DialogueWidgetClass, or null
	 * when no dialogue is running (or no widget class is set).
	 *
	 * Valid from OnDialogueWidgetCreated until dialogue end. Every dialogue creates
	 * a fresh widget, so do not cache the result across dialogues — ask again.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|UI")
	UStoryFlowDialogueWidget* GetDialogueWidget() const;

	// ========================================================================
	// Variable Access (by display name)
	// ========================================================================

	/** Get a boolean variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	bool GetBoolVariable(const FString& VariableName, bool bGlobal = false);

	/** Set a boolean variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetBoolVariable(const FString& VariableName, bool bValue, bool bGlobal = false);

	/** Get an integer variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	int32 GetIntVariable(const FString& VariableName, bool bGlobal = false);

	/** Set an integer variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetIntVariable(const FString& VariableName, int32 Value, bool bGlobal = false);

	/** Get a float variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	float GetFloatVariable(const FString& VariableName, bool bGlobal = false);

	/** Set a float variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetFloatVariable(const FString& VariableName, float Value, bool bGlobal = false);

	/** Get a string variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	FString GetStringVariable(const FString& VariableName, bool bGlobal = false);

	/** Set a string variable value by name */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetStringVariable(const FString& VariableName, const FString& Value, bool bGlobal = false);

	/** Get an enum variable value by name (as string) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	FString GetEnumVariable(const FString& VariableName, bool bGlobal = false);

	/** Set an enum variable value by name (as string) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetEnumVariable(const FString& VariableName, const FString& Value, bool bGlobal = false);

	/**
	 * Read any array variable by display name.
	 *
	 * Returns the elements as FStoryFlowVariant copies so callers can use the typed getters
	 * (GetBool, GetInt, GetFloat, GetString). String and enum element values are routed through
	 * the string table so callers receive localized text rather than raw keys. Image, audio,
	 * and character elements are stored as plain strings (asset keys / paths) and pass through
	 * unchanged. Returns an empty array if the variable is missing or is not an array.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<FStoryFlowVariant> GetArrayVariable(const FString& VariableName, bool bGlobal = false);

	/**
	 * Read a boolean array variable by display name as a native Blueprint array.
	 *
	 * Mirrors GetArrayVariable's scoping (locals during dialogue, then globals) but unpacks
	 * each element to bool in C++, so Blueprint never has to handle a variant. The variable
	 * must be a boolean array; a missing, non-array, or wrong-typed variable warns and returns
	 * an empty array. Counterpart to SetBoolArrayVariable.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<bool> GetBoolArrayVariable(const FString& VariableName, bool bGlobal = false);

	/** Read an integer array variable as a native Blueprint array. See GetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<int32> GetIntArrayVariable(const FString& VariableName, bool bGlobal = false);

	/** Read a float array variable as a native Blueprint array. See GetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<float> GetFloatArrayVariable(const FString& VariableName, bool bGlobal = false);

	/**
	 * Read a string array variable as a native Blueprint array. Elements are resolved through
	 * the string table so callers receive localized text, matching GetStringVariable and
	 * GetArrayVariable. See GetBoolArrayVariable for the shared rules.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<FString> GetStringArrayVariable(const FString& VariableName, bool bGlobal = false);

	/** Read an enum array variable as native enum-option strings, resolved through the string table. See GetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<FString> GetEnumArrayVariable(const FString& VariableName, bool bGlobal = false);

	/**
	 * Read an image array variable as native asset-key strings. Keys are returned raw (not
	 * string-table resolved), matching how image elements are stored. See GetBoolArrayVariable
	 * for the shared rules.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<FString> GetImageArrayVariable(const FString& VariableName, bool bGlobal = false);

	/** Read an audio array variable as native asset-key strings (returned raw). See GetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<FString> GetAudioArrayVariable(const FString& VariableName, bool bGlobal = false);

	/**
	 * Write a boolean array variable by display name.
	 *
	 * Replaces the variable's elements and fires OnVariableChanged, like the scalar
	 * setters. Local variables are only reachable while a dialogue is executing
	 * (same rule as the scalar setters). Mirrors the Unity plugin's
	 * Set*ArrayVariable API.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetBoolArrayVariable(const FString& VariableName, const TArray<bool>& Values, bool bGlobal = false);

	/** Write an integer array variable by display name. See SetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetIntArrayVariable(const FString& VariableName, const TArray<int32>& Values, bool bGlobal = false);

	/** Write a float array variable by display name. See SetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetFloatArrayVariable(const FString& VariableName, const TArray<float>& Values, bool bGlobal = false);

	/**
	 * Write a string array variable by display name. Elements are stored verbatim —
	 * no string-table key is created, so they are language-locked and bypass
	 * localization. See SetBoolArrayVariable for the shared rules.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetStringArrayVariable(const FString& VariableName, const TArray<FString>& Values, bool bGlobal = false);

	/**
	 * Write an enum array variable by display name. Values are enum option strings;
	 * they are stored verbatim without validation against the variable's option
	 * list. See SetBoolArrayVariable for the shared rules.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetEnumArrayVariable(const FString& VariableName, const TArray<FString>& Values, bool bGlobal = false);

	/**
	 * Write an image array variable by display name. Each entry is an asset key
	 * resolvable through the standard asset pools. See SetBoolArrayVariable for
	 * the shared rules.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetImageArrayVariable(const FString& VariableName, const TArray<FString>& AssetKeys, bool bGlobal = false);

	/** Write an audio array variable by display name. Each entry is an asset key. See SetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetAudioArrayVariable(const FString& VariableName, const TArray<FString>& AssetKeys, bool bGlobal = false);

	/** Write a character array variable by display name. Each entry is a character path. See SetBoolArrayVariable for the shared rules. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetCharacterArrayVariable(const FString& VariableName, const TArray<FString>& CharacterPaths, bool bGlobal = false);

	/**
	 * Read a map variable by display name.
	 *
	 * Entries are projected as two parallel arrays — Keys[i] pairs with Values[i], in the
	 * map's entry order — because the entry struct holds variants recursively and cannot
	 * cross into Blueprint. Elements are FStoryFlowVariant copies so callers can use the
	 * typed getters (GetBool, GetInt, GetFloat, GetString). String and enum VALUES are
	 * routed through the string table so callers receive localized text; KEYS are raw
	 * identifiers and never resolve. Image, audio, and character values pass through
	 * unchanged (asset keys / paths). Empty arrays if the variable is missing or not a map.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void GetMapVariable(const FString& VariableName, TArray<FStoryFlowVariant>& Keys, TArray<FStoryFlowVariant>& Values, bool bGlobal = false);

	// ========================================================================
	// Typed Map Access (read/write map variables as native TMaps)
	// ========================================================================

	/**
	 * Read a string-or-enum-keyed, integer-valued map as a native TMap. Mirrors the scalar and
	 * array getters' scoping (locals during dialogue, then globals). Returns an empty map and
	 * warns when the variable is missing, is not a map, or has a different key or value type.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<FString, int32> GetStringToIntMap(const FString& VariableName, bool bGlobal = false);

	/** Read a string-or-enum-keyed, boolean-valued map as a native TMap. See GetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<FString, bool> GetStringToBoolMap(const FString& VariableName, bool bGlobal = false);

	/** Read a string-or-enum-keyed, float-valued map as a native TMap. See GetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<FString, float> GetStringToFloatMap(const FString& VariableName, bool bGlobal = false);

	/**
	 * Read a string-or-enum-keyed, string-family-valued map as a native TMap. Value types string,
	 * enum, image, audio and character are accepted. String and enum values are resolved through
	 * the string table; image, audio and character values are returned as raw asset keys.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<FString, FString> GetStringToStringMap(const FString& VariableName, bool bGlobal = false);

	/** Read an integer-keyed, boolean-valued map as a native TMap. See GetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<int32, bool> GetIntToBoolMap(const FString& VariableName, bool bGlobal = false);

	/** Read an integer-keyed, integer-valued map as a native TMap. See GetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<int32, int32> GetIntToIntMap(const FString& VariableName, bool bGlobal = false);

	/** Read an integer-keyed, float-valued map as a native TMap. See GetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<int32, float> GetIntToFloatMap(const FString& VariableName, bool bGlobal = false);

	/** Read an integer-keyed, string-family-valued map as a native TMap. Value resolution matches GetStringToStringMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TMap<int32, FString> GetIntToStringMap(const FString& VariableName, bool bGlobal = false);

	/**
	 * Read a map's keys in entry order. String and enum keys come back verbatim, integer keys are
	 * stringified. The typed map getters return an unordered TMap; this is the only ordered view.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	TArray<FString> GetMapKeysInOrder(const FString& VariableName, bool bGlobal = false);

	/**
	 * Write a string-or-enum-keyed, integer-valued map from a native TMap. No-op and warns on a
	 * missing, non-map, or mistyped variable. Fires OnVariableChanged like the scalar and array
	 * setters. Entry order is not preserved (TMap is unordered).
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetStringToIntMap(const FString& VariableName, const TMap<FString, int32>& Values, bool bGlobal = false);

	/** Write a string-or-enum-keyed, boolean-valued map. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetStringToBoolMap(const FString& VariableName, const TMap<FString, bool>& Values, bool bGlobal = false);

	/** Write a string-or-enum-keyed, float-valued map. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetStringToFloatMap(const FString& VariableName, const TMap<FString, float>& Values, bool bGlobal = false);

	/** Write a string-or-enum-keyed, string-family-valued map. Values are stored verbatim. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetStringToStringMap(const FString& VariableName, const TMap<FString, FString>& Values, bool bGlobal = false);

	/** Write an integer-keyed, boolean-valued map. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetIntToBoolMap(const FString& VariableName, const TMap<int32, bool>& Values, bool bGlobal = false);

	/** Write an integer-keyed, integer-valued map. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetIntToIntMap(const FString& VariableName, const TMap<int32, int32>& Values, bool bGlobal = false);

	/** Write an integer-keyed, float-valued map. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetIntToFloatMap(const FString& VariableName, const TMap<int32, float>& Values, bool bGlobal = false);

	/** Write an integer-keyed, string-family-valued map. Values stored verbatim. See SetStringToIntMap. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables")
	void SetIntToStringMap(const FString& VariableName, const TMap<int32, FString>& Values, bool bGlobal = false);

	// ========================================================================
	// Character Variable Access (by path — legacy)
	// ========================================================================

	/** Get a character variable value (raw variant — prefer typed versions below) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	FStoryFlowVariant GetCharacterVariable(const FString& CharacterPath, const FString& VariableName);

	/** Set a character variable value (raw variant — prefer typed versions below) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	void SetCharacterVariable(const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value);

	/**
	 * Read a script or global variable whose element type is character-array.
	 *
	 * Returns the array of character paths stored in the variable. Each path is suitable
	 * for GetCharacter, GetCharacterVariable, or GetCharacterPortrait. Returns an empty
	 * array if the variable is missing or is not a character array.
	 *
	 * Note: this reads a *script variable whose element type is character*, which is distinct
	 * from GetCharacterVariable, which reads a variable that lives *on* a character.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	TArray<FString> GetCharacterArrayVariable(const FString& VariableName);

	/**
	 * Return the live runtime character data for a path (the same struct the runtime mutates
	 * via setCharacterVar). Useful when you want to read several fields without separate
	 * variable calls. Returns a default-constructed struct if no character is registered at that path.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	FStoryFlowCharacterDef GetCharacter(const FString& CharacterPath);

	/**
	 * Return the names of all custom variables defined on a character.
	 *
	 * Does not include the built-in "Name" and "Image" fields, which are always available via
	 * GetCharacterVariable regardless of declaration. Empty array if the character is missing.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	TArray<FString> GetCharacterVariableNames(const FString& CharacterPath);

	/**
	 * Resolve a character's portrait to a Texture2D.
	 *
	 * When AssetKey is empty (default), uses the character's current Image field, which reflects
	 * any runtime mutations from setCharacterVar("Image", ...). Pass a non-empty AssetKey to
	 * resolve an alternate pose, e.g. one stored in a custom image-typed character variable.
	 *
	 * Walks the standard three asset pools in priority order: character → script → project.
	 * Returns nullptr if nothing resolves.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	UTexture2D* GetCharacterPortrait(const FString& CharacterPath, const FString& AssetKey = TEXT(""));

	// ========================================================================
	// Character Access (by character FILE id — P4)
	// ========================================================================
	//
	// The id-taking twins of the path APIs above (P4 contract §4). THIN WRAPPERS by rule:
	// FindCharacter already routes a `da_` id through ResolveCharacterKey (bridge lookup,
	// warn-once degraded fall-back), so these delegate to the path APIs and must never
	// re-resolve or normalize the id themselves — a wrapper that does resolves twice, the
	// exact drift the IdAndPathReachOneDef pin exists to catch.
	//
	// Per amendment A2(b), none of these raise OnCharacterVariableChanged — that delegate
	// is node-lane only (see its declaration above).

	/**
	 * The live runtime character a character FILE id resolves to, through the id bridge.
	 * bFound is false for a dangling id (no bridge entry) and for an id whose record is not
	 * among the loaded runtime characters (the post-save-load shape) — GetCharacterPathById
	 * can still answer in that second case, because the bridge itself is project-derived.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (By Id)")
	void GetCharacterById(const FString& CharacterId, FStoryFlowCharacterDef& OutCharacter, bool& bFound);

	/**
	 * The character record key (the path the runtime keys the record by) for a character
	 * FILE id — a PURE BRIDGE LOOKUP, deliberately unlike GetCharacterById: it requires no
	 * loaded runtime record, so after a save load it still answers for a character the save
	 * did not carry. The key comes back verbatim (lowercase, backslashes) and is valid
	 * input to every path-taking character API.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (By Id)")
	void GetCharacterPathById(const FString& CharacterId, FString& OutPath, bool& bFound);

	/**
	 * Record keys of every LOADED character, in map order (no sort promise) — the amendment A4
	 * enumeration surface. Ids serve stable BINDING, record keys serve enumeration and the
	 * path-taking APIs above, so by-id enumeration is deliberately not provided: the bridge is
	 * an implementation mapping, not an enumeration surface. After a save load this reflects
	 * the LOADED set — which the asset registry cannot tell you, because the project's
	 * character assets outlive what a save carried.
	 *
	 * The block's one NON-wrapper (a direct read of the loaded set, no resolution involved),
	 * and pure: a list query mutates nothing and warrants no exec pins.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variables|Character (By Id)")
	TArray<FString> GetCharacterPaths() const;

	/** Id twin of GetCharacterVariable. cf_name / cf_image alias the Name / Image builtins here too (amendment A2a). */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (By Id)")
	FStoryFlowVariant GetCharacterVariableById(const FString& CharacterId, const FString& VariableName);

	/** Id twin of SetCharacterVariable. Warns and no-ops when the character does not declare the variable. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (By Id)")
	void SetCharacterVariableById(const FString& CharacterId, const FString& VariableName, const FStoryFlowVariant& Value);

	// ========================================================================
	// Character Variable Access (typed, with asset picker)
	// ========================================================================

	/** Get a character's boolean variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	bool GetCharacterBoolVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName);

	/** Set a character's boolean variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	void SetCharacterBoolVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, bool bValue);

	/** Get a character's integer variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	int32 GetCharacterIntVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName);

	/** Set a character's integer variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	void SetCharacterIntVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, int32 Value);

	/** Get a character's float variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	float GetCharacterFloatVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName);

	/** Set a character's float variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	void SetCharacterFloatVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, float Value);

	/** Get a character's string variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	FString GetCharacterStringVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName);

	/** Set a character's string variable */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	void SetCharacterStringVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, const FString& Value);

	/** Get a character's enum variable (as string) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	FString GetCharacterEnumVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName);

	/** Set a character's enum variable (as string) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character")
	void SetCharacterEnumVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, const FString& Value);

	// ========================================================================
	// Data Asset Variable Access (typed, with asset picker)
	// ========================================================================
	//
	// The Blueprint half of the .sfd system (engine contract §4/§5), mirroring the character
	// accessors above: an asset reference plus the variable NAME an author typed in the editor.
	//
	// Four things differ from the character shape, on purpose:
	//  - READS GO THROUGH THE RESOLVER, so a Blueprint sees the same value a dialogue does:
	//    inherited defaults, ancestor overrides, and this session's writes, with an ancestor's
	//    write cascading down. Never a cached copy of anything.
	//  - FAILURE IS REPORTED, not logged. A Blueprint call has no node id, so it cannot join the
	//    graph accessors' warn-once ladder (contract §6, which is latched PER NODE) — an
	//    unlatched log line on a getter a Blueprint ticks would be a line per frame. The `bFound`
	//    / return-value flag is the whole report.
	//  - THE DECLARED TYPE MUST MATCH the accessor, with no coercion (the §6.1 rule at this
	//    surface): reading an integer variable through the float getter reports not-found rather
	//    than converting, because within the string family especially a value carries no
	//    evidence of the type it was declared as.
	//  - THE SURFACE IS ASYMMETRIC: read ANY type, write SCALARS ONLY. Every declaration is
	//    readable (scalars typed, arrays and maps through GetDataAssetVariantVariable), but there
	//    is no SetDataAssetVariantVariable — arrays and maps are written by the graph's Set node
	//    alone. A variant setter cannot use the scalar gate: it would have to check the
	//    declaration's SHAPE too (isArray, and a map's key/value types) or a Blueprint could drop
	//    a scalar over a declared map and leave a value nothing can read. That gate is buildable
	//    and was deliberately left out of V2 scope rather than half-built.
	//
	// Writes land at the referenced asset's OWN level and cascade to its descendants (§5), which
	// is also why there is no "write to the base" variant: reference the base to write there.

	/** Get a Data Asset's boolean variable, resolved through its parent chain and this session's writes */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool GetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Set a Data Asset's boolean variable for this session. False when the variable is unknown or not a boolean. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool bValue);

	/** Get a Data Asset's integer variable, resolved through its parent chain and this session's writes */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	int32 GetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Set a Data Asset's integer variable for this session. False when the variable is unknown or not an integer. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, int32 Value);

	/** Get a Data Asset's float variable, resolved through its parent chain and this session's writes */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	float GetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Set a Data Asset's float variable for this session. False when the variable is unknown or not a float. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, float Value);

	/**
	 * Get a Data Asset's string variable, resolved through its parent chain and this session's writes.
	 *
	 * Also serves image, character and audio variables: all four are declared distinctly in the
	 * editor but hold a plain string (an asset key or a path) at runtime. ENUM is the exception —
	 * it stores a distinct type tag, so it has its own accessor rather than being coerced here.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	FString GetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Set a Data Asset's string (or image / character / audio) variable for this session. False when the variable is unknown or a different type. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value);

	/** Get a Data Asset's enum variable (as string), resolved through its parent chain and this session's writes */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	FString GetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/** Set a Data Asset's enum variable (as string) for this session. False when the variable is unknown or not an enum. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value);

	/**
	 * Get a Data Asset's variable as a raw variant — the ARRAY and MAP path.
	 *
	 * Arrays and maps have no typed accessor of their own: feed the variant to
	 * UStoryFlowVariantLibrary (GetVariantArray / GetVariantMap / GetVariantAs*), the same
	 * library the character and script variable nodes use, rather than growing a second set of
	 * container nodes here. Scalars come through it too, untyped.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	FStoryFlowVariant GetDataAssetVariantVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound);

	/**
	 * Every variable name the asset's chain DECLARES, root-most ancestor first (contract §11.1).
	 *
	 * The accessors above all need a name the caller already knew. This is how a Blueprint learns
	 * the names — driving an inventory row per variable, a debug readout, a data-driven UI — and
	 * it is the SAME answer the Get Variable Names graph node gives, because both forward to
	 * StoryFlowDataAssets::VariableNames, which owns every rule: root-first order, declarations
	 * only (overrides shadow a name, they never add one), dedupe by id and then by name.
	 *
	 * Walking `Parent` yourself is the thing this exists to prevent: that walk re-implements those
	 * rules, and a re-implementation that disagrees produces a plausible list nobody notices is
	 * wrong. Empty when the asset is null, there is no store, or the seed does not carry it.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	TArray<FString> GetDataAssetVariableNames(UStoryFlowDataAssetAsset* DataAsset);

	/**
	 * Replace a Data Asset's ARRAY variable with these elements. True when the write landed.
	 *
	 * The container half of the surface, which used to be read-only: every type could be READ
	 * (scalars typed, arrays and maps through GetDataAssetVariantVariable) and only scalars could
	 * be written, so a Data Asset holding a list was a list a Blueprint could not edit.
	 *
	 * TWO SETTERS, NOT ONE VARIANT SETTER, and that is the whole design. A variant cannot say
	 * whether it is an array: FStoryFlowVariant::SetArray infers its Type from the FIRST element,
	 * so an empty array and a scalar of that type are the same value. A variant setter would
	 * therefore be unable to tell "write an empty array" from "the caller passed a scalar by
	 * mistake" — and writing the second over an array declaration leaves a value nothing can
	 * read, which is exactly why the V2 contract left it out rather than half-building it. Here
	 * the shape is in the SIGNATURE, so there is nothing to infer.
	 *
	 * THE SHAPE GATE. The declaration must be an array (never a map, never a scalar) and every
	 * element must match its declared type, with the same string-family tolerance the scalar
	 * accessors use. A mismatch refuses the whole write rather than landing a partial one: half a
	 * list is a shape no author declared.
	 *
	 * An EMPTY array is a legitimate write and clears the variable. Writes land at the referenced
	 * asset's own level and cascade to its descendants, like every other Data Asset write.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetArrayVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const TArray<FStoryFlowVariant>& Elements);

	/**
	 * Replace a Data Asset's MAP variable with these entries. True when the write landed.
	 *
	 * The map twin of SetDataAssetArrayVariable — see it for why the shape lives in the signature
	 * rather than in a variant.
	 *
	 * THE SHAPE GATE is one step wider here: the declaration must be a map, every KEY must match
	 * its declared key type and every VALUE its declared value type. Key order is the caller's and
	 * is preserved, matching the ordered entry lists the format ships.
	 *
	 * PARALLEL ARRAYS, mirroring GetMapVariable's out-params rather than taking entry structs:
	 * FStoryFlowMapEntry is a plain struct and cannot cross the Blueprint boundary. Keys and
	 * Values must be the same length; a mismatch refuses the write rather than truncating to the
	 * shorter one, which would silently drop entries the caller listed.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Data Assets")
	bool SetDataAssetMapVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const TArray<FStoryFlowVariant>& Keys, const TArray<FStoryFlowVariant>& Values);

	// ========================================================================
	// Utility Functions
	// ========================================================================

	/** Reset all local variables to their initial values */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void ResetVariables();

	/** Get a localized string by key */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetLocalizedString(const FString& Key) const;

	/**
	 * Get available scripts for the dropdown (editor only)
	 * Used by GetOptions meta specifier
	 */
	UFUNCTION()
	TArray<FString> GetAvailableScripts() const;

protected:
	// ========================================================================
	// Internal Node Processing
	// ========================================================================

	/** Process the current node */
	void ProcessNode(FStoryFlowNode* Node);

	/** Process the next node based on source handle */
	void ProcessNextNode(const FString& SourceHandle);

	// === Node Handlers ===

	void HandleStart(FStoryFlowNode* Node);
	void HandleEnd(FStoryFlowNode* Node);
	void HandleBranch(FStoryFlowNode* Node);
	void HandleDialogue(FStoryFlowNode* Node);
	void HandleRunScript(FStoryFlowNode* Node);
	void HandleRunFlow(FStoryFlowNode* Node);
	void HandleEntryFlow(FStoryFlowNode* Node);

	// Variable Handlers
	void HandleGetBool(FStoryFlowNode* Node);
	void HandleSetBool(FStoryFlowNode* Node);
	void HandleGetInt(FStoryFlowNode* Node);
	void HandleSetInt(FStoryFlowNode* Node);
	void HandleGetFloat(FStoryFlowNode* Node);
	void HandleSetFloat(FStoryFlowNode* Node);
	void HandleGetString(FStoryFlowNode* Node);
	void HandleSetString(FStoryFlowNode* Node);
	void HandleGetEnum(FStoryFlowNode* Node);
	void HandleSetEnum(FStoryFlowNode* Node);

	// Logic Handlers (no-op, evaluation happens when needed)
	void HandleLogicNode(FStoryFlowNode* Node);

	// Array Handlers
	void HandleArraySet(FStoryFlowNode* Node);

	/**
	 * Set Array Element, resolved EDGE-FIRST rather than by variable name.
	 *
	 * These nodes carry no `variable` field in the export at all, so the name lookup
	 * HandleArraySet uses for whole-array writes cannot resolve one and every such node was a
	 * silent no-op. The array comes off the `<type>-array-2` pin and the result is written back
	 * through whatever that pin is wired to (a `.sfd` accessor, a character array, or a script
	 * variable), mirroring the reference runtime's updateConnectedArrayVariable.
	 */
	void HandleArraySetElement(FStoryFlowNode* Node);

	void HandleArrayModify(FStoryFlowNode* Node);

	// Loop Handlers
	void HandleForEachLoop(FStoryFlowNode* Node);
	void HandleForEachMap(FStoryFlowNode* Node);
	void ContinueForEachLoop(const FString& NodeId);

	// Enum Handlers
	void HandleSwitchOnEnum(FStoryFlowNode* Node);
	void HandleRandomBranch(FStoryFlowNode* Node);

	// Media Handlers
	void HandleSetImage(FStoryFlowNode* Node);
	void HandleSetBackgroundImage(FStoryFlowNode* Node);
	void HandleSetAudio(FStoryFlowNode* Node);
	void HandlePlayAudio(FStoryFlowNode* Node);
	void HandleSetDataAssetRef(FStoryFlowNode* Node);
	void HandleSetCharacter(FStoryFlowNode* Node);

	// Character Variable Handlers
	void HandleGetCharacterVar(FStoryFlowNode* Node);
	void HandleSetCharacterVar(FStoryFlowNode* Node);

	// Data Asset (.sfd) Handlers
	/**
	 * Execute a Set Data Asset Variable node: record the wired value in the session overlay
	 * (engine contract §5). Every degraded path is a NO-OP that still continues exec — writing
	 * anything would be worse than doing nothing, because an overlay entry SHADOWS the declared
	 * default for the rest of the session, and cascades to every descendant when it lands on a
	 * base.
	 */
	void HandleSetDataAssetVariable(FStoryFlowNode* Node);

	/**
	 * The value a Set Data Asset Variable node is writing, or false when its value pin is
	 * unwired (contract §5 REFUSES that — there is no inline literal to fall back to, unlike
	 * setCharacterVar). The unwired check is an explicit FindInputEdge on every branch,
	 * deliberately: the typed evaluators substitute their own type zero for an unwired pin, so
	 * trusting one here would write a 0 / "" / false over the declared default.
	 */
	bool TryReadDataAssetSetInput(FStoryFlowNode* Node, FStoryFlowVariant& OutValue);

	// Map Variable Handlers
	void HandleSetMap(FStoryFlowNode* Node);
	void HandleMapModify(FStoryFlowNode* Node);
	void HandleMapPureNode(FStoryFlowNode* Node);

	/**
	 * Evaluate a node's wired array input, dispatching to the evaluator's typed array reader for
	 * the given element type. TArray value semantics return a container copy, so the destination
	 * never aliases the source array (matches the HTML runtime's .slice() semantics).
	 * Caller must ensure Evaluator is valid.
	 *
	 * Shared by the setCharacterVar and setDataAssetVariable handlers — it knows nothing of
	 * either, only of element types and handle suffixes.
	 */
	TArray<FStoryFlowVariant> EvaluateTypedArrayInput(FStoryFlowNode* Node, const FString& VariableType, const FString& HandleSuffix);

	// === Helper Functions ===

	/** Find a variable by display name, falling back to subsystem globals/characters when outside dialogue */
	FStoryFlowVariable* FindVariableByName(const FString& VariableName, bool bGlobal);

	/**
	 * Shared tail of the Set*ArrayVariable family: find the variable, replace its
	 * element storage, and broadcast the change. Assigns ArrayValue only — the
	 * variant's scalar fields and type stay untouched, matching the Unity plugin.
	 */
	void ApplyArrayVariable(const FString& VariableName, bool bGlobal, TArray<FStoryFlowVariant>&& Items);

	/**
	 * Find an array variable of the expected element type for the typed Get*ArrayVariable
	 * readers. Applies GetArrayVariable's scoping (locals during dialogue, then globals).
	 * Warns and returns nullptr when the variable is missing, is not an array, or holds a
	 * different element type. TypeLabel is the human-readable element name used in the warning.
	 */
	FStoryFlowVariable* FindArrayVariableForRead(const FString& VariableName, bool bGlobal, EStoryFlowVariableType ExpectedType, const TCHAR* TypeLabel);

	/**
	 * Find a map variable for the typed Get/Set*Map functions. Applies the same local/global
	 * scoping as FindArrayVariableForRead and requires Type == Map, warning and returning nullptr
	 * otherwise. The caller performs the key/value family checks (it knows the requested native types).
	 */
	FStoryFlowVariable* FindMapVariableForAccess(const FString& VariableName, bool bGlobal);

	/** Find a character def, falling back to subsystem when outside dialogue */
	FStoryFlowCharacterDef* FindCharacter(const FString& CharacterPath);

	/** Find a character def from an asset reference */
	FStoryFlowCharacterDef* FindCharacterFromAsset(UStoryFlowCharacterAsset* CharacterAsset);

	/**
	 * The one thing this surface still does itself after a Data Asset write: drop the evaluator's
	 * memo so a condition above the written value re-evaluates. Returns what it was given, so the
	 * setters can return it in one expression. The ladder itself lives in StoryFlowDataAssetAccess,
	 * shared with the subsystem's mirror of these accessors.
	 */
	bool DropCachesAfterDataAssetWrite(bool bWritten);
	void InvalidateVariableReads();

	/**
	 * THE LANGUAGE every lookup on this component runs in (localization spec §9).
	 *
	 * The SUBSYSTEM owns it whenever the loaded project carries a localization sidecar, because a
	 * language is the player's and game-wide, not a per-actor setting. Without a sidecar there is
	 * nothing to switch to and the per-component LanguageCode keeps its pre-localization meaning,
	 * so a project exported before localization existed behaves EXACTLY as it did — the presence
	 * of the file is the only branch, never a key count.
	 */
	FString ActiveLanguageCode() const;

	/** Resolve a string table key to localized text using ActiveLanguageCode */
	FString ResolveString(const FString& Key) const;

	/** Build dialogue state from current node */
	FStoryFlowDialogueState BuildDialogueState(FStoryFlowNode* DialogueNode);

	/**
	 * Resolve a character image asset key to a texture by walking the standard three asset pools.
	 * Priority: character asset's ResolvedAssets → current script's ResolvedAssets → project's ResolvedAssets.
	 * Falls back to the character's CachedImage for cross-script resolution. Returns nullptr if nothing resolves.
	 */
	UTexture2D* ResolveCharacterPortraitTexture(const FString& CharacterPath, const FString& AssetKey, FStoryFlowCharacterDef* CharDef);

	/** Notify variable change */
	void NotifyVariableChanged(const FStoryFlowVariable& Variable, bool bIsGlobal);

	/** Report an error */
	void ReportError(const FString& ErrorMessage);

	/** Handle the end of a Set* node (checks for loops and dialogue return) */
	void HandleSetNodeEnd(FStoryFlowNode* Node, const FString& SourceHandle);

	// === Audio Helpers ===

	/** Play dialogue audio with optional looping (override in Blueprint for custom audio systems) */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow|Audio")
	void PlayDialogueAudio(USoundBase* Sound, bool bLoop);

	/** Stop currently playing dialogue audio (override in Blueprint for custom audio systems) */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow|Audio")
	void StopDialogueAudio();

	/** Callback when dialogue audio finishes (for looping) */
	UFUNCTION()
	void OnDialogueAudioFinished();

private:
	friend struct FStoryFlowRollbackTestAccess;
	friend class FStoryFlowRollbackController;
	bool bPublishingRollbackAvailability = false;
	bool bPendingRollbackAvailability = false;
	void PublishRollbackAvailability();
	TSharedPtr<FStoryFlowRollbackController> Rollback;
	TWeakObjectPtr<UStoryFlowSubsystem> RollbackSubsystem;
	TOptional<uint64> RestoredEntry;
	uint64 RestoredDeliveryEntry = 0;
	uint64 AudioGeneration = 0;
	uint64 SessionGeneration = 0;
	float CurrentAudioPosition = 0.0f;
	void PlayDialogueAudioNative(USoundBase* Sound, bool bLoop, float Position, float Volume, bool bNativeOnly);
	FString ResolveCharacterName(const FStoryFlowCharacterDef& Character) const;
	void DetachRollback();
	void BindAudioFinished();
	void HandleBlockRollback(FStoryFlowNode* Node);

	/** A For Each iterating on the native stack right now, found by ContinueForEachLoop. */
	struct FLoopDriver
	{
		uint64 Session;
		int32 CallDepth;
		FString NodeId;
		bool bContinue;
	};
	TArray<FLoopDriver> LoopDrivers;

	/**
	 * The character path the CURRENT line's speaker resolved to — see GetCurrentSpeakerPath. Set wherever
	 * the state is built, from the same resolution the character data itself came from, so the two can never
	 * disagree about who is talking.
	 */
	FString CurrentSpeakerPath;

	/** Component-lifetime identity; not restored from saves or reset between conversations. */
	uint64 DialogueEntrySerial = 0;
	/** Member function pointer type for node handlers */
	using FNodeHandler = void (UStoryFlowComponent::*)(FStoryFlowNode*);

	/** Get the static dispatch table mapping node types to handler functions */
	static const TMap<EStoryFlowNodeType, FNodeHandler>& GetDispatchTable();

	/** Execution context */
	FStoryFlowExecutionContext ExecutionContext;

	/** Evaluator instance */
	TUniquePtr<FStoryFlowEvaluator> Evaluator;

	/**
	 * Has this component contributed to the subsystem's ActiveDialogueCount?
	 *
	 * The counter is a REFERENCE COUNT across components and it gates LoadFromSlot, so the
	 * start/end notify pair has to be idempotent per component: restarting a dialogue starts a
	 * second one without ending the first (deliberately — no end event fires, existing projects
	 * depend on that), and an unconditional increment would leave the count stuck above zero
	 * after the single stop that eventually follows, permanently refusing every load.
	 *
	 * Not a substitute for ExecutionContext.bIsExecuting: that flag is cleared by Reset() from
	 * paths which never touch the counter, so it cannot be trusted to say whether the increment
	 * happened.
	 */
	bool bCountedActiveDialogue = false;

	/** Cached subsystem reference */
	UPROPERTY()
	mutable TObjectPtr<UStoryFlowSubsystem> CachedSubsystem;

	/** Currently playing dialogue audio component */
	UPROPERTY()
	TObjectPtr<UAudioComponent> CurrentDialogueAudio;

	/** True when waiting for audio to finish before auto-advancing */
	bool bWaitingForAudioAdvance = false;

	/** True when the player is allowed to skip audio during advance-on-end */
	bool bAudioAdvanceAllowSkip = false;

	/** Active dialogue widget instance */
	UPROPERTY()
	TObjectPtr<UStoryFlowDialogueWidget> ActiveDialogueWidget;
};
