// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"

struct FStoryFlowExecutionContext;
struct FStoryFlowNode;
class UStoryFlowScriptAsset;

/**
 * Evaluator for StoryFlow node graph expressions
 *
 * Evaluators retrieve data from nodes WITHOUT executing them.
 * They recursively resolve input connections to compute values.
 */
struct STORYFLOWRUNTIME_API FStoryFlowEvaluator
{
public:
	FStoryFlowEvaluator(FStoryFlowExecutionContext* InContext);

	// === Boolean Evaluation ===

	/** Evaluate boolean from a connected input */
	bool EvaluateBooleanInput(FStoryFlowNode* Node, const FString& HandleSuffix, bool Fallback = false);

	/** Evaluate boolean from a specific node */
	bool EvaluateBooleanFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle);

	// === Integer Evaluation ===

	/** Evaluate integer from a connected input */
	int32 EvaluateIntegerInput(FStoryFlowNode* Node, const FString& HandleSuffix, int32 Fallback = 0);

	/** Evaluate integer from a specific node */
	int32 EvaluateIntegerFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle);

	// === Float Evaluation ===

	/** Evaluate float from a connected input */
	float EvaluateFloatInput(FStoryFlowNode* Node, const FString& HandleSuffix, float Fallback = 0.0f);

	/** Evaluate float from a specific node */
	float EvaluateFloatFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle);

	// === String Evaluation ===

	/** Evaluate string from a connected input */
	FString EvaluateStringInput(FStoryFlowNode* Node, const FString& HandleSuffix, const FString& Fallback = TEXT(""));

	/** Evaluate string from a specific node */
	FString EvaluateStringFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle);

	// === Enum Evaluation ===

	/** Evaluate enum value from a connected input */
	FString EvaluateEnumInput(FStoryFlowNode* Node, const FString& HandleSuffix, const FString& Fallback = TEXT(""));

	// === Array Evaluation ===

	/** Evaluate boolean array from a connected input */
	TArray<FStoryFlowVariant> EvaluateBoolArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	/** Evaluate integer array from a connected input */
	TArray<FStoryFlowVariant> EvaluateIntArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	/** Evaluate float array from a connected input */
	TArray<FStoryFlowVariant> EvaluateFloatArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	/** Evaluate string array from a connected input */
	TArray<FStoryFlowVariant> EvaluateStringArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	/** Evaluate image array from a connected input */
	TArray<FStoryFlowVariant> EvaluateImageArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	/** Evaluate character array from a connected input */
	TArray<FStoryFlowVariant> EvaluateCharacterArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	/** Evaluate audio array from a connected input */
	TArray<FStoryFlowVariant> EvaluateAudioArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix);

	// === Map Evaluation ===

	/**
	 * Resolve the map wired into one of Node's map inputs and return a pointer
	 * to the LIVE entry storage (nullptr = unresolved). Maps alias: mutators
	 * (setMapValue/removeMapKey/clearMap) write through this pointer and every
	 * later read must observe the change, so this never returns a copy.
	 *
	 * The full target handle is "target-{nodeId}-map-{keyType}-{valueType}-{optionId}";
	 * K/V come from Node's own Data.KeyType/Data.ValueType (catalog map op nodes carry
	 * them in node data), the caller passes only the OptionId ("1" pure reads,
	 * "2" mutators, "map" forEachMap).
	 *
	 * The returned pointer may be invalidated by any subsequent node-state
	 * allocation (GetNodeState/FindOrAdd). Resolve all other inputs FIRST, then
	 * resolve the map and consume it immediately — never hold it across another
	 * evaluation.
	 */
	TArray<FStoryFlowMapEntry>* EvaluateMapInput(FStoryFlowNode* Node, const FString& OptionId);

	/**
	 * Resolve a map op's key: wired key input first ("target-{nodeId}-{keyType}-{optionId}"),
	 * else the inline Data.MapKey fallback. The returned variant is normalized per the
	 * node's KeyType string (Int32 for integer keys, string otherwise).
	 */
	FStoryFlowVariant EvaluateMapOpKeyInput(FStoryFlowNode* Node, const FString& OptionId);

	/**
	 * Find a map entry's index by key (INDEX_NONE on miss). Integer keys compare
	 * as Int32; string/enum keys as exact (case-sensitive) strings. KeyType is
	 * the node/variable's key type string ("integer", "string", "enum").
	 * Returns an index (not a pointer) so mutators can write entries in place.
	 */
	static int32 FindMapEntryByKey(const TArray<FStoryFlowMapEntry>& Entries, const FString& KeyType, const FStoryFlowVariant& Key);

	// === Boolean Chain Processing ===

	/** Pre-process boolean chain to cache results */
	void ProcessBooleanChain(FStoryFlowNode* Node);

	// === Option Visibility ===

	/** Evaluate visibility of a dialogue option */
	bool EvaluateOptionVisibility(FStoryFlowNode* DialogueNode, const FString& OptionId);

	// === Cache Management ===

	/** Clear all evaluation caches */
	void ClearCache();

private:
	/** Evaluate an integer comparison (GT, GTE, LT, LTE, EQ) */
	bool EvaluateIntegerComparison(FStoryFlowNode* Node, EStoryFlowNodeType ComparisonType);

	/** Evaluate a float comparison (GT, GTE, LT, LTE, EQ with NearlyEqual) */
	bool EvaluateFloatComparison(FStoryFlowNode* Node, EStoryFlowNodeType ComparisonType);

	/** Evaluate an intToEnum conversion (integer input clamped into the enum value list as an index) */
	FString EvaluateIntToEnum(FStoryFlowNode* Node);

	/** Evaluate a stringToEnum conversion (exact match passes through, else falls back to the first value) */
	FString EvaluateStringToEnum(FStoryFlowNode* Node);

	/** Resolve the enum value list an intToEnum/stringToEnum node converts into */
	TArray<FString> ResolveConversionEnumValues(FStoryFlowNode* Node);

	/** Generic array input evaluator — all typed array evaluators delegate to this */
	TArray<FStoryFlowVariant> EvaluateArrayInputGeneric(FStoryFlowNode* Node, const FString& HandleSuffix, EStoryFlowNodeType ExpectedGetArrayType);

	/**
	 * Compute a getMapValue lookup: resolve the map (input "1") and key (input "2"
	 * / inline fallback) and report whether the key was found. OutValue receives
	 * the stored value on a hit and is left untouched on a miss — callers return
	 * their own type defaults.
	 */
	bool ComputeGetMapValue(FStoryFlowNode* Node, FStoryFlowVariant& OutValue);

	/**
	 * Log a one-time warning when a map node is missing its keyType/valueType
	 * node data (the map input handle cannot be built without them). Same
	 * dedup pattern as MaybeWarnUnknownNode, via Context->WarnedMapNodes.
	 */
	void MaybeWarnMissingMapTypes(const FStoryFlowNode* Node);

	/**
	 * Log a one-time warning if the source node's type is Unknown. The default
	 * return value (false/0/empty) is preserved by the caller; this only
	 * surfaces visibility when the plugin encounters a node type from a newer
	 * editor version that it does not understand. Deduplicated per dialogue
	 * run via Context->WarnedUnknownNodes.
	 */
	void MaybeWarnUnknownNode(const FStoryFlowNode* Node);

	/** Execution context */
	FStoryFlowExecutionContext* Context;

	/** RAII depth tracker for recursion protection */
	struct FDepthGuard
	{
		FDepthGuard(int32& InDepth, int32 MaxDepth, bool& OutValid);
		~FDepthGuard();

		bool IsValid() const { return bValid; }

	private:
		int32& Depth;
		bool bValid;
	};
};
