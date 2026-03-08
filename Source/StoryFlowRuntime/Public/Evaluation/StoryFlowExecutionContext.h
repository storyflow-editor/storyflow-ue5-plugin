// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowExecutionContext.generated.h"

class UStoryFlowScriptAsset;
class UStoryFlowProjectAsset;

// Forward declaration
struct FStoryFlowEvaluator;

/** Maximum depth for script nesting */
constexpr int32 STORYFLOW_MAX_SCRIPT_DEPTH = 20;

/** Maximum depth for flow nesting */
constexpr int32 STORYFLOW_MAX_FLOW_DEPTH = 50;

/** Maximum evaluation depth for recursion protection */
constexpr int32 STORYFLOW_MAX_EVALUATION_DEPTH = 100;

/** Maximum processing depth for ProcessNode/ProcessNextNode recursion */
constexpr int32 STORYFLOW_MAX_PROCESSING_DEPTH = 1000;

/**
 * Per-node runtime state that is NOT stored on the shared asset.
 * Each execution context has its own map, preventing cross-contamination
 * when multiple components run the same script.
 */
struct FNodeRuntimeState
{
	/** Cached output value (for evaluators) */
	FStoryFlowVariant CachedOutput;
	bool bHasCachedOutput = false;

	/** Loop state (for forEach nodes) */
	int32 LoopIndex = -1;
	TArray<FStoryFlowVariant> LoopArray;
	bool bLoopInitialized = false;

	/** Output values from a completed RunScript call (keyed by variable ID) */
	TMap<FString, FStoryFlowVariant> OutputValues;
	bool bHasOutputValues = false;
};

/**
 * Runtime execution context for StoryFlow
 */
USTRUCT()
struct STORYFLOWRUNTIME_API FStoryFlowExecutionContext
{
	GENERATED_BODY()

public:
	/** Initialize the context (legacy - uses project's own global variables) */
	void Initialize(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript);

	/** Initialize the context with external global variables, characters, and once-only options (from subsystem) */
	void InitializeWithSubsystem(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript, TMap<FString, FStoryFlowVariable>* InGlobalVariables, TMap<FString, FStoryFlowCharacterDef>* InCharacters = nullptr, TSet<FString>* InUsedOnceOnlyOptions = nullptr);

	/** Reset the context to initial state */
	void Reset();

	// === Current State ===

	/** Currently executing script */
	UPROPERTY()
	TWeakObjectPtr<UStoryFlowScriptAsset> CurrentScript;

	/** Current node ID */
	UPROPERTY()
	FString CurrentNodeId;

	/** Waiting for user input */
	bool bIsWaitingForInput = false;

	/** Execution active */
	bool bIsExecuting = false;

	/** Execution paused */
	bool bIsPaused = false;

	/** Flag to track if we're entering a dialogue via edge (fresh) or direct call (returning from Set*) */
	bool bEnteringDialogueViaEdge = false;

	// === Call Stack (for runScript - returns to caller) ===

	/** Call stack for nested scripts */
	UPROPERTY()
	TArray<FStoryFlowCallFrame> CallStack;

	// === Flow Stack (for runFlow - just tracks depth, no return) ===

	/** Flow call stack for in-script flows (depth tracking only, flows don't return) */
	UPROPERTY()
	TArray<FStoryFlowFlowFrame> FlowCallStack;

	// === Loop Stack ===

	/** Loop stack for nested loops */
	UPROPERTY()
	TArray<FStoryFlowLoopContext> LoopStack;

	// === Variable Storage ===

	/** Local variables for current script */
	UPROPERTY()
	TMap<FString, FStoryFlowVariable> LocalVariables;

	/** Reference to project */
	UPROPERTY()
	TWeakObjectPtr<UStoryFlowProjectAsset> Project;

	/**
	 * Non-owning pointer to external global variables (owned by UStoryFlowSubsystem).
	 * When set, global variable operations use this instead of Project->GlobalVariables.
	 * This allows multiple components to share the same global state.
	 * Lifetime: valid as long as the subsystem exists (GameInstance scope).
	 */
	TMap<FString, FStoryFlowVariable>* ExternalGlobalVariables = nullptr;

	// === Variable Name Index (Name -> ID for O(1) lookup by name) ===

	/** Name-to-ID index for local variables. Rebuilt when local vars change. */
	TMap<FString, FString> LocalVariableNameIndex;

	/** Name-to-ID index for global variables. Rebuilt when global vars change. */
	TMap<FString, FString> GlobalVariableNameIndex;

	/**
	 * Non-owning pointer to external runtime characters (owned by UStoryFlowSubsystem).
	 * Character data assets (UStoryFlowCharacterAsset) are copied into this mutable map at startup.
	 * This allows character variable modifications to persist across scripts.
	 * Lifetime: valid as long as the subsystem exists (GameInstance scope).
	 */
	TMap<FString, FStoryFlowCharacterDef>* ExternalCharacters = nullptr;

	// === Once-Only Tracking ===

	/**
	 * Non-owning pointer to external once-only option set (owned by UStoryFlowSubsystem).
	 * Tracks which once-only dialogue options have been used (NodeId-OptionId keys).
	 * Persists across dialogues so once-only options stay hidden.
	 * Lifetime: valid as long as the subsystem exists (GameInstance scope).
	 */
	TSet<FString>* ExternalUsedOnceOnlyOptions = nullptr;

	// === Current Dialogue State ===

	/** Current dialogue state for UI */
	UPROPERTY()
	FStoryFlowDialogueState CurrentDialogueState;

	// === Persistent Media State ===

	/** Persistent background image (carries over between dialogues unless imageReset=true) */
	UPROPERTY()
	TObjectPtr<UTexture2D> PersistentBackgroundImage;

	// === Evaluation State ===

	/** Current evaluation depth (for recursion protection) */
	int32 EvaluationDepth = 0;

	/** Current processing depth (for ProcessNode/ProcessNextNode recursion protection) */
	int32 ProcessingDepth = 0;

	// === Per-Node Runtime State (isolated per execution context) ===

	/** Runtime state for each node, keyed by node ID. NOT stored on the shared asset. */
	TMap<FString, FNodeRuntimeState> NodeRuntimeStates;

public:
	// === Node Accessors ===

	/** Get current node */
	FStoryFlowNode* GetCurrentNode();

	/** Get node by ID from current script */
	FStoryFlowNode* GetNode(const FString& NodeId);

	// === Variable Accessors ===

	/** Find variable by ID (internal use — node handlers and evaluators) */
	FStoryFlowVariable* FindVariable(const FString& VariableId, bool bIsGlobal);

	/** Find variable by display name (for public Blueprint API). Uses name-to-ID index with lazy fallback. */
	FStoryFlowVariable* FindVariableByName(const FString& VariableName, bool bIsGlobal);

	/** Set variable value by ID (internal use) */
	void SetVariable(const FString& VariableId, const FStoryFlowVariant& Value, bool bIsGlobal);

	/** Get variable value by ID (internal use) */
	FStoryFlowVariant GetVariableValue(const FString& VariableId, bool bIsGlobal);

	/** Build name-to-ID index for local variables */
	void RebuildLocalNameIndex();

	/** Build name-to-ID index for global variables */
	void RebuildGlobalNameIndex();

	// === Character Accessors ===

	/** Find character definition by path */
	FStoryFlowCharacterDef* FindCharacter(const FString& CharacterPath);

	/** Find a variable within a character */
	FStoryFlowVariable* FindCharacterVariable(const FString& CharacterPath, const FString& VariableName);

	/** Set a character variable value */
	void SetCharacterVariable(const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value);

	/** Get a character variable value */
	FStoryFlowVariant GetCharacterVariableValue(const FString& CharacterPath, const FString& VariableName);

	// === Edge Accessors ===

	/** Find edge by source handle */
	const FStoryFlowConnection* FindEdgeBySourceHandle(const FString& SourceHandle) const;

	/** Find edge by source node ID */
	const FStoryFlowConnection* FindEdgeBySource(const FString& SourceNodeId) const;

	/** Find input edge to a node */
	const FStoryFlowConnection* FindInputEdge(const FString& NodeId, const FString& HandleSuffix) const;

	/** Find edge by target node ID (any edge going to this node) */
	const FStoryFlowConnection* FindEdgeByTarget(const FString& TargetNodeId) const;

	// === Script Navigation ===

	/** Push current state to call stack and switch to new script */
	bool PushScript(const FString& ScriptPath, const FString& ReturnNodeId);

	/** Pop from call stack and restore state */
	bool PopScript();

	// === String Resolution ===

	/** Get localized string from current script or project */
	FString GetString(const FString& Key, const FString& LanguageCode = TEXT("en")) const;

	/** Interpolate variables in text */
	FString InterpolateVariables(const FString& Text) const;

	/** Resolve string table keys in string-type variable initial values (scalar + array) */
	void ResolveStringVariableValues(TMap<FString, FStoryFlowVariable>& Variables) const;

	// === Validation ===

	/** Check if we're at max script depth */
	bool IsAtMaxScriptDepth() const { return CallStack.Num() >= STORYFLOW_MAX_SCRIPT_DEPTH; }

	/** Check if we're at max flow depth */
	bool IsAtMaxFlowDepth() const { return FlowCallStack.Num() >= STORYFLOW_MAX_FLOW_DEPTH; }

	/** Check if we're at max evaluation depth */
	bool IsAtMaxEvaluationDepth() const { return EvaluationDepth >= STORYFLOW_MAX_EVALUATION_DEPTH; }

	/** Check if we're at max processing depth */
	bool IsAtMaxProcessingDepth() const { return ProcessingDepth >= STORYFLOW_MAX_PROCESSING_DEPTH; }

	/** Get per-node runtime state (lazily created) */
	FNodeRuntimeState& GetNodeState(const FString& NodeId) { return NodeRuntimeStates.FindOrAdd(NodeId); }

	// === Cache Management ===

	/** Clear all cached evaluation results */
	void ClearEvaluationCache();
};
