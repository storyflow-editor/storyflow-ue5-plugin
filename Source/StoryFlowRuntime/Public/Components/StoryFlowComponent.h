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
class UStoryFlowSubsystem;
class UStoryFlowDialogueWidget;
class UAudioComponent;
class USoundClass;
class USoundConcurrency;
class USoundAttenuation;

// ============================================================================
// Delegates
// ============================================================================

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDialogueUpdated, const FStoryFlowDialogueState&, DialogueState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueEnded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnVariableChanged, const FStoryFlowVariable&, Variable, bool, bIsGlobal);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScriptStarted, const FString&, ScriptPath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScriptEnded, const FString&, ScriptPath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnError, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBackgroundImageChanged, const FString&, ImagePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAudioPlayRequested, const FString&, AudioPath, bool, bLoop);

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

	/** Language code for string lookup (empty = use default "en") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow")
	FString LanguageCode = TEXT("en");

	/** Enable execution trace logging ([SF-TRACE] prefix) for cross-runtime comparison */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Debug")
	bool bTraceEnabled = true;

	/** Optional dialogue widget class to auto-create when dialogue starts and destroy when it ends */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow")
	TSubclassOf<UStoryFlowDialogueWidget> DialogueWidgetClass;

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

	/** Called when dialogue execution ends */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnDialogueEnded OnDialogueEnded;

	/** Called when a variable changes */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnVariableChanged OnVariableChanged;

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

	// ========================================================================
	// Character Variable Access (by path — legacy)
	// ========================================================================

	/** Get a character variable value (raw variant — prefer typed versions below) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	FStoryFlowVariant GetCharacterVariable(const FString& CharacterPath, const FString& VariableName);

	/** Set a character variable value (raw variant — prefer typed versions below) */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Variables|Character (Legacy)")
	void SetCharacterVariable(const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value);

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
	void HandleArrayModify(FStoryFlowNode* Node);

	// Loop Handlers
	void HandleForEachLoop(FStoryFlowNode* Node);
	void ContinueForEachLoop(const FString& NodeId);

	// Enum Handlers
	void HandleSwitchOnEnum(FStoryFlowNode* Node);
	void HandleRandomBranch(FStoryFlowNode* Node);

	// Media Handlers
	void HandleSetImage(FStoryFlowNode* Node);
	void HandleSetBackgroundImage(FStoryFlowNode* Node);
	void HandleSetAudio(FStoryFlowNode* Node);
	void HandlePlayAudio(FStoryFlowNode* Node);
	void HandleSetCharacter(FStoryFlowNode* Node);

	// Character Variable Handlers
	void HandleGetCharacterVar(FStoryFlowNode* Node);
	void HandleSetCharacterVar(FStoryFlowNode* Node);

	// === Helper Functions ===

	/** Find a variable by display name, falling back to subsystem globals/characters when outside dialogue */
	FStoryFlowVariable* FindVariableByName(const FString& VariableName, bool bGlobal);

	/** Find a character def, falling back to subsystem when outside dialogue */
	FStoryFlowCharacterDef* FindCharacter(const FString& CharacterPath);

	/** Find a character def from an asset reference */
	FStoryFlowCharacterDef* FindCharacterFromAsset(UStoryFlowCharacterAsset* CharacterAsset);

	/** Resolve a string table key to localized text using LanguageCode */
	FString ResolveString(const FString& Key) const;

	/** Build dialogue state from current node */
	FStoryFlowDialogueState BuildDialogueState(FStoryFlowNode* DialogueNode);

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
	/** Member function pointer type for node handlers */
	using FNodeHandler = void (UStoryFlowComponent::*)(FStoryFlowNode*);

	/** Get the static dispatch table mapping node types to handler functions */
	static const TMap<EStoryFlowNodeType, FNodeHandler>& GetDispatchTable();

	/** Execution context */
	FStoryFlowExecutionContext ExecutionContext;

	/** Evaluator instance */
	TUniquePtr<FStoryFlowEvaluator> Evaluator;

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
