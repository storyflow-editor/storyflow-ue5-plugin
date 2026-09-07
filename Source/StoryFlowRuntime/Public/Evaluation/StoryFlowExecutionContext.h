// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowDataAssetStore.h"
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
	/** Completed operations cannot be reconstructed by evaluating their input again. */
	bool bIsExecutionOutput = false;

	/** Loop state (for forEach nodes) */
	int32 LoopIndex = -1;
	TArray<FStoryFlowVariant> LoopArray;
	bool bLoopInitialized = false;

	/**
	 * Current scalar-loop element while an iteration is live. Reads the PERSISTENT
	 * loop fields (LoopArray/LoopIndex survive ClearEvaluationCache), never
	 * CachedOutput — HandleDialogue clears the whole cache right before building
	 * option state, which used to blank loop-element reads in option conditions.
	 * The map-loop counterpart is LoopKey/LoopValue below.
	 */
	bool TryGetLoopElement(FStoryFlowVariant& OutElement) const
	{
		if (!bLoopInitialized || !LoopArray.IsValidIndex(LoopIndex))
		{
			return false;
		}
		OutElement = LoopArray[LoopIndex];
		return true;
	}

	/**
	 * Map loop state (forEachMap). LoopEntries is a SNAPSHOT taken once at loop
	 * init — body mutations land on the live map but never affect iteration.
	 * LoopKey/LoopValue expose the current entry to the typed evaluators (read
	 * via the "-key"/"-value" source handle suffixes). They live in dedicated
	 * fields rather than CachedOutput so ClearEvaluationCache (which wipes
	 * CachedOutput per iteration) leaves them intact — outer map loops then
	 * need no restore step in the nested-loop restore pass.
	 */
	TArray<FStoryFlowMapEntry> LoopEntries;
	FStoryFlowVariant LoopKey;
	FStoryFlowVariant LoopValue;

	/** Output values from a completed RunScript call (keyed by variable ID) */
	TMap<FString, FStoryFlowVariant> OutputValues;
	bool bHasOutputValues = false;

	/**
	 * Map-typed output variables from a completed RunScript call (keyed by
	 * variable Name). Kept as full variables (not variants) so the map resolver
	 * can hand out stable FStoryFlowVariable* storage with type metadata. The
	 * map storage is DETACHED from the dead invocation at capture (HandleEnd) —
	 * the HTML runtime converts _outputValues to a fresh Map at the read site,
	 * so the boundary is observably a snapshot. Read-only by contract (see
	 * EMapSourceKind::RunScriptOutput).
	 */
	TMap<FString, FStoryFlowVariable> MapOutputVariables;

	/**
	 * Scratch storage for a map-typed `.sfd` accessor's read (contract §4). The store resolves
	 * COPIES, and the map resolver's signature hands back FStoryFlowVariable* — so the copy has
	 * to live somewhere with a stable address, and this is it. DETACHED from the store by
	 * construction, exactly like MapOutputVariables is from its dead invocation: the HTML
	 * runtime builds a fresh Map off a `.sfd` read too, so mutating through this pointer is an
	 * observable no-op in BOTH runtimes (EMapSourceKind::DataAsset flags it read-only).
	 */
	FStoryFlowVariable DataAssetMapSnapshot;

	/** Detached map mutation output, retained until this execution context is reset. */
	FStoryFlowVariable MapExecutionOutput;
	bool bHasMapExecutionOutput = false;
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
	void InitializeWithSubsystem(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript, TMap<FString, FStoryFlowVariable>* InGlobalVariables, TMap<FString, FStoryFlowCharacterDef>* InCharacters = nullptr, TSet<FString>* InUsedOnceOnlyOptions = nullptr, StoryFlowDataAssets::FStoreRef InDataAssetStore = {}, const TMap<FString, FString>* InCharacterIdToPath = nullptr, const FString* InActiveLanguage = nullptr);

	/**
	 * Reset the context to initial state.
	 *
	 * THIS is the contract §6 warn re-arm point, and the only one: it clears the Data Asset warn
	 * latches (and the map / unknown-node ones) along with the evaluation cache, so the next
	 * dialogue reports a still-broken accessor again instead of staying quiet forever. It runs at
	 * dialogue stop and at the start of a fresh dialogue.
	 *
	 * Subsystem resets advance the shared read generation, invalidating derived caches on the
	 * next evaluation. They do not re-arm warning latches; only a context Reset does that.

	 */
	void Reset();

	// === Current State ===

	/** Currently executing script */
	UPROPERTY()
	TWeakObjectPtr<UStoryFlowScriptAsset> CurrentScript;

	/**
	 * The language ResolveStringVariableValues seeds a script's local string variables in
	 * (localization spec §9). Set by the owning component from its active language BEFORE
	 * Initialize, because the seed runs inside Initialize.
	 *
	 * DELIBERATELY NOT cleared by Reset: a player's language is not per-script state, and the
	 * seed of the next script pushed by a runScript node must run in the same language as the
	 * one that pushed it. Read-time lookups do not use this — they take the component's live
	 * language, so a SetLanguage lands on the very next line rendered rather than the next script.
	 */
	FString SeedLanguageCode = TEXT("en");

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

	/** Execution trace logging enabled (set from component's bTraceEnabled) */
	bool bTraceEnabled = false;

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

	/**
	 * Non-owning pointer to the character id bridge (owned by UStoryFlowSubsystem, refreshed
	 * beside RuntimeCharacters): character FILE id (`da_`) -> the exact ExternalCharacters
	 * key. Wired in InitializeWithSubsystem next to ExternalCharacters — same source, same
	 * lifetime. Null (or empty) means a pre-P4 import: ids resolve nothing and the path
	 * fields stay authoritative. Read exclusively by ResolveCharacterKey.
	 */
	const TMap<FString, FString>* CharacterIdToPath = nullptr;

	// === Data Assets (.sfd) ===

	/**
	 * Non-owning reference to the subsystem-owned Data Asset store (engine contract §3). Same
	 * idiom, and same lifetime, as ExternalGlobalVariables — valid as long as the subsystem
	 * exists (GameInstance scope). Pointing at the subsystem's maps rather than copying them
	 * is what makes a write from one component visible to every other.
	 *
	 * ONE ref, not a seed pointer beside an overlay pointer: every resolution needs both
	 * (contract §4 consults the overlay and the seed at each chain level), and two fields two
	 * callers could set independently is a mismatched pair waiting to happen.
	 *
	 * Prefer the TryResolveDataAsset / TrySetDataAsset accessors below over touching this.
	 */
	StoryFlowDataAssets::FStoreRef DataAssetStore;
	uint64 SeenSharedRevision = 0;
	void RefreshSharedState();
	void NotifyStateChanged();

	/**
	 * Non-owning pointer to the subsystem's LIVE language code — the ExternalGlobalVariables
	 * idiom again, same source and same lifetime, wired in InitializeWithSubsystem.
	 *
	 * NOT SeedLanguageCode, and the difference is the point: that field is the language a script's
	 * local string variables were seeded in and deliberately does not move afterwards, while a
	 * `.sfd` value is looked up at the moment it is READ (localization spec §2's amendment, and
	 * StoryFlowDataAssets::TryRead's note on why the seed cannot be baked). Pointing at the
	 * subsystem's own field rather than copying the code is what makes a mid-session SetLanguage
	 * land on the very next `.sfd` read instead of the next dialogue.
	 *
	 * Null for a context built by the plain Initialize (a test, or a component with no subsystem):
	 * the lookup then runs with an empty code, which no language table is keyed by, so every value
	 * answers with its source text.
	 */
	const FString* ActiveLanguage = nullptr;

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
	/** A Set captures failures only along the input evaluation it actually performs. */
	bool bCaptureReadFailures = false;
	uint64 ReadFailures = 0;
	void MarkReadFailure() { if (bCaptureReadFailures) { ++ReadFailures; } }

	/** Current processing depth (for ProcessNode/ProcessNextNode recursion protection) */
	int32 ProcessingDepth = 0;

	// === Per-Node Runtime State (isolated per execution context) ===

	/** Runtime state for each node, keyed by node ID. NOT stored on the shared asset. */
	TMap<FString, FNodeRuntimeState> NodeRuntimeStates;

	/**
	 * Tracks node ids for which an "Unknown node type" warning has already been
	 * logged from the evaluator. Used to deduplicate per dialogue run so the log
	 * does not spam when the same unsupported source is read repeatedly.
	 */
	TSet<FString> WarnedUnknownNodes;

	/**
	 * Tracks map node ids for which a "missing keyType/valueType" warning has
	 * already been logged from the evaluator. Same dedup pattern as
	 * WarnedUnknownNodes — map reads are pulled repeatedly, warn once per node.
	 */
	TSet<FString> WarnedMapNodes;

	/**
	 * Degraded Data Asset accessors already warned about, keyed "{NodeId}|{reason}" —
	 * per node AND per reason, which is what contract §6 asks for (a node that is both
	 * unwired and stale says so once each, not once ever). Same dedup family as
	 * WarnedUnknownNodes; cleared by Reset(), which is the §6 re-arm on game restart.
	 *
	 * The five §6 reasons are the ladder's, but the keyspace is not reserved to it: callers
	 * outside the ladder latch their own reasons here, and "arrayop" (HandleArrayModify's
	 * not-bound-to-an-array refusal) is one.
	 */
	TSet<FString> WarnedDataAssetNodes;

	/**
	 * How many Data Asset warnings this context has actually EMITTED — a TEST SEAM, never a
	 * runtime signal. Reset() clears it with the latches.
	 *
	 * The latch set above cannot answer this on its own: TSet::Add is idempotent, so a
	 * MaybeWarnDataAsset that dropped its early-out would still leave exactly one key per
	 * node-and-reason while logging on every single evaluation. SUPPRESSION is the property
	 * contract §6 is about — an option condition re-evaluates every render — so pinning it needs
	 * a number that only moves when a line is actually written.
	 */
	int32 DataAssetWarningsEmitted = 0;

	/**
	 * Character ids already warned about, keyed "{CharacterId}|{reason}" — the WarnedDataAssetNodes
	 * sibling for the P4 id bridge. Keyed by ID rather than node id because the same dangling id
	 * bound on five nodes is ONE authoring problem, and dialogue lines re-resolve their speaker on
	 * every advance. Two reasons exist: "dangling" (no bridge entry) and "unloaded" (bridge entry
	 * whose record is not among the loaded runtime characters). Re-armed only by Reset().
	 */
	TSet<FString> WarnedCharacterIds;

	/**
	 * How many character id warnings this context actually EMITTED — a TEST SEAM, exactly like
	 * DataAssetWarningsEmitted above and for the same reason: the latch set alone cannot prove
	 * SUPPRESSION. Reset() clears it with the latch.
	 */
	int32 CharacterIdWarningsEmitted = 0;

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

	// === Data Asset Accessors ===
	// Null-check-and-forward over DataAssetStore, the same shape FindVariable uses for the
	// external globals: a context with no store answers "nothing resolves, nothing writes"
	// instead of making every caller repeat the guard.

	/**
	 * THE SCRIPT LANE'S `.sfd` READ DOOR: the effective value of a Data Asset variable through its
	 * chain and this session's overlay (contract §4), with a DECLARED string value resolved
	 * through the string tables in the live language (localization spec §2's amendment — see
	 * StoryFlowDataAssets::TryRead, which owns every rule and which the Blueprint accessors call
	 * too, so the two surfaces cannot drift). False when there is no store, the asset is unknown,
	 * or no chain level declares the id; OutValue is untouched in that case.
	 */
	bool TryResolveDataAsset(const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue) const;

	/**
	 * Record a Data Asset write in the session overlay at the referenced asset's own level
	 * (contract §5). False when there is no store, the asset is unknown, or no chain level
	 * declares the id — the caller owns the warning.
	 */
	bool TrySetDataAsset(const FString& AssetId, const FString& VariableId, const FStoryFlowVariant& Value);

	/**
	 * The names the asset's chain DECLARES (contract §11.1) — the Get Variable Names node's
	 * answer, forwarded to StoryFlowDataAssets::VariableNames, which owns every rule (root-first
	 * order, declarations only, dedupe by id then by name). Empty when there is no store or the
	 * seed does not carry the asset. Deliberately NOT routed through the §6 ladder: the node has
	 * no variableId to be degraded about, and the reference implementation answers every broken
	 * shape with a silent empty list.
	 */
	TArray<FString> GetDataAssetVariableNames(const FString& AssetId) const;

	/**
	 * THE DEGRADATION LADDER an accessor node's binding walks (contract §6), mirroring
	 * runtime-data-assets.js `resolveBinding` reason for reason. False — with a warning latched
	 * once per node per reason — when any part of the binding is broken:
	 *
	 *   nodata  the node carries no variableId at all
	 *   unwired nothing usable on the `dataAsset` pin (unwired / not a pill / an unbound pill)
	 *   deadref the pill names an asset this seed does not carry
	 *   missing no chain level declares the id
	 *   changed the declaration moved under a live node (§6.1 DeclMatches)
	 *
	 * ONE ladder for the read arms and the write handler, ON PURPOSE. Letting Get and Set drift
	 * gives you an accessor that reads the declared default while its twin writes an overlay
	 * entry that SHADOWS that default for the rest of the session — and cascades to every
	 * descendant, if it landed on a base.
	 *
	 * The wire IS the binding (contract §2.2): the asset comes from a SINGLE HOP off the
	 * `dataAsset` pin to a getDataAsset pill. One hop is sufficient, not a limitation — the
	 * editor collapses reroute elbows before export, so a wire that ran through elbows on the
	 * canvas arrives here as a direct pill -> accessor edge.
	 */
	bool TryResolveDataAssetBinding(const FStoryFlowNode& Accessor, FString& OutAssetId);

	/**
	 * The assetId on the pill wired into `Accessor`'s Data Asset pin, or empty for every
	 * "nothing usable upstream" case. Split out of the ladder because it is the only half that
	 * reads the GRAPH rather than the store.
	 */
	FString ResolveDataAssetId(const FStoryFlowNode& Accessor) const;

	/**
	 * Log a degraded Data Asset warning ONCE per node per reason (contract §6). These nodes are
	 * read from render paths — a dialogue's option conditions re-evaluate on every render — so an
	 * unlatched warning would be a line per frame. Re-armed by Reset(), i.e. by a game restart.
	 * The Set's value-pin refusal deliberately does NOT come through here: it names a wiring
	 * mistake on an exec node the author just ran, and exec fires far less often than a condition.
	 */
	void MaybeWarnDataAsset(const FString& NodeId, const TCHAR* Reason, const FString& Message);

	/** Build name-to-ID index for local variables */
	void RebuildLocalNameIndex();

	/** Build name-to-ID index for global variables */
	void RebuildGlobalNameIndex();

	// === Character Accessors ===

	/**
	 * THE ONE id-or-path -> record-key resolution point (P4 contract §3/§4). A `da_` id (the
	 * editor's isCharacterIdRef shape) is looked up in the bridge: a hit whose record is loaded
	 * answers the record key VERBATIM — never re-normalized, the contract guarantees the shape.
	 * A dangling id (no bridge entry) or an unloaded one (bridge entry whose record is missing
	 * from the runtime characters, the post-LoadFromSlot shape — the bridge is project-derived
	 * while a loaded save carries only what it saved) warns once via MaybeWarnCharacterId and
	 * falls THROUGH to path treatment of the input. Non-id input normalizes as a path exactly
	 * as before P4. Callers holding BOTH an id field and a path field pick through
	 * ResolveCharacterRef, which adds the contract §3 path-field fall-back on top.
	 */
	FString ResolveCharacterKey(const FString& IdOrPath);

	/**
	 * ResolveCharacterKey against EXPLICIT maps — the shared core, for the one caller that
	 * resolves outside a wired context (UStoryFlowComponent::FindCharacter's outside-dialogue
	 * lane reads the subsystem's maps directly). WarnLatch supplies the warn-once state; the
	 * maps may be null (no bridge / no characters), which degrades to plain path treatment.
	 */
	static FString ResolveCharacterKeyIn(const TMap<FString, FString>* IdToPath, const TMap<FString, FStoryFlowCharacterDef>* Characters, const FString& IdOrPath, FStoryFlowExecutionContext& WarnLatch);

	/**
	 * Id-first pick across a node's additive id field and its untouched path field (P4 contract
	 * §4 with the §3 dangling-id ruling): the id wins when its whole resolution lands on a
	 * loaded character, the path field — returned VERBATIM, so pre-P4 content flows
	 * byte-identically through the path lane — is the fall-back for everything else. An empty
	 * id (pre-migration content) short-circuits to the path with no bridge consult and no warn.
	 */
	FString ResolveCharacterRef(const FString& CharacterId, const FString& CharacterPath);

	/**
	 * Log a degraded character id warning ONCE per id per reason — MaybeWarnDataAsset's sibling
	 * (same latch pattern, same Reset() re-arm) for the P4 bridge. Speakers re-resolve on every
	 * dialogue advance and char-var reads sit in option conditions, so an unlatched warn would
	 * be a line per render.
	 */
	void MaybeWarnCharacterId(const FString& CharacterId, const TCHAR* Reason, const FString& Message);

	/** Find character definition by path or character FILE id (routes through ResolveCharacterKey) */
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

	/** Resolve graph content in the active language, or the component's seed language without localization. */
	FString GetString(const FString& Key) const;

	/** Get localized string from current script or project in an explicitly requested language. */
	FString GetString(const FString& Key, const FString& LanguageCode) const;

	/** Interpolate variables in text */
	FString InterpolateVariables(const FString& Text) const;

	/** Resolve string table keys in string-type variable initial values (scalar + array + string-valued map entries; map keys never resolve) */
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
	FNodeRuntimeState& GetNodeState(const FString& NodeId) { RefreshSharedState(); return NodeRuntimeStates.FindOrAdd(NodeId); }

	// === Cache Management ===

	/** Clear all cached evaluation results */
	void ClearEvaluationCache();
};
