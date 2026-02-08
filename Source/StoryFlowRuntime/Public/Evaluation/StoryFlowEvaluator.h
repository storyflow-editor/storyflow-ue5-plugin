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
	/** Execution context */
	FStoryFlowExecutionContext* Context;

	/** Helper to check if node type produces boolean output */
	bool IsBooleanProducer(EStoryFlowNodeType Type) const;

	/** Helper to check if node type produces integer output */
	bool IsIntegerProducer(EStoryFlowNodeType Type) const;

	/** Helper to check if node type produces float output */
	bool IsFloatProducer(EStoryFlowNodeType Type) const;

	/** Helper to check if node type produces string output */
	bool IsStringProducer(EStoryFlowNodeType Type) const;

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
