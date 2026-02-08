// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Evaluation/StoryFlowEvaluator.h"
#include "StoryFlowRuntime.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Data/StoryFlowScriptAsset.h"

FStoryFlowEvaluator::FStoryFlowEvaluator(FStoryFlowExecutionContext* InContext)
	: Context(InContext)
{
}

FStoryFlowEvaluator::FDepthGuard::FDepthGuard(int32& InDepth, int32 MaxDepth, bool& OutValid)
	: Depth(InDepth)
	, bValid(InDepth < MaxDepth)
{
	if (bValid)
	{
		++Depth;
	}
	else
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Max evaluation depth exceeded"));
	}
	OutValid = bValid;
}

FStoryFlowEvaluator::FDepthGuard::~FDepthGuard()
{
	if (bValid)
	{
		--Depth;
	}
}

// ============================================================================
// Boolean Evaluation
// ============================================================================

bool FStoryFlowEvaluator::EvaluateBooleanInput(FStoryFlowNode* Node, const FString& HandleSuffix, bool Fallback)
{
	if (!Context || !Node)
	{
		return Fallback;
	}

	const FStoryFlowConnection* Edge = Context->FindInputEdge(Node->Id, HandleSuffix);
	if (!Edge)
	{
		return Fallback;
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);
	if (!SourceNode)
	{
		return Fallback;
	}

	return EvaluateBooleanFromNode(SourceNode, Node->Id, Edge->SourceHandle);
}

bool FStoryFlowEvaluator::EvaluateBooleanFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle)
{
	if (!Node || !Context)
	{
		return false;
	}

	bool bDepthValid;
	FDepthGuard Guard(Context->EvaluationDepth, STORYFLOW_MAX_EVALUATION_DEPTH, bDepthValid);
	if (!bDepthValid)
	{
		return false;
	}

	// Check cache first
	FNodeRuntimeState& NodeState = Context->GetNodeState(Node->Id);
	if (NodeState.bHasCachedOutput && NodeState.CachedOutput.GetType() == EStoryFlowVariableType::Boolean)
	{
		return NodeState.CachedOutput.GetBool();
	}

	bool Result = false;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetBool:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetBool() : false;
		break;
	}

	case EStoryFlowNodeType::NotBool:
	{
		bool Input = EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
		Result = !Input;
		break;
	}

	case EStoryFlowNodeType::AndBool:
	{
		bool Input1 = EvaluateBooleanInput(Node, TEXT("boolean-1"), Node->Data.Value1.GetBool(false));
		bool Input2 = EvaluateBooleanInput(Node, TEXT("boolean-2"), Node->Data.Value2.GetBool(false));
		Result = Input1 && Input2;
		break;
	}

	case EStoryFlowNodeType::OrBool:
	{
		bool Input1 = EvaluateBooleanInput(Node, TEXT("boolean-1"), Node->Data.Value1.GetBool(false));
		bool Input2 = EvaluateBooleanInput(Node, TEXT("boolean-2"), Node->Data.Value2.GetBool(false));
		Result = Input1 || Input2;
		break;
	}

	case EStoryFlowNodeType::EqualBool:
	{
		bool Input1 = EvaluateBooleanInput(Node, TEXT("boolean-1"), Node->Data.Value1.GetBool(false));
		bool Input2 = EvaluateBooleanInput(Node, TEXT("boolean-2"), Node->Data.Value2.GetBool(false));
		Result = (Input1 == Input2);
		break;
	}

	case EStoryFlowNodeType::GreaterThan:
	case EStoryFlowNodeType::GreaterThanOrEqual:
	case EStoryFlowNodeType::LessThan:
	case EStoryFlowNodeType::LessThanOrEqual:
	case EStoryFlowNodeType::EqualInt:
		Result = EvaluateIntegerComparison(Node, Node->Type);
		break;

	case EStoryFlowNodeType::GreaterThanFloat:
	case EStoryFlowNodeType::GreaterThanOrEqualFloat:
	case EStoryFlowNodeType::LessThanFloat:
	case EStoryFlowNodeType::LessThanOrEqualFloat:
	case EStoryFlowNodeType::EqualFloat:
		Result = EvaluateFloatComparison(Node, Node->Type);
		break;

	case EStoryFlowNodeType::EqualString:
	{
		FString Input1 = EvaluateStringInput(Node, TEXT("string-1"), Node->Data.Value1.GetString());
		FString Input2 = EvaluateStringInput(Node, TEXT("string-2"), Node->Data.Value2.GetString());
		Result = Input1.Equals(Input2);
		break;
	}

	case EStoryFlowNodeType::ContainsString:
	{
		FString Haystack = EvaluateStringInput(Node, TEXT("string-1"), Node->Data.Value1.GetString());
		FString Needle = EvaluateStringInput(Node, TEXT("string-2"), Node->Data.Value2.GetString());
		Result = Haystack.Contains(Needle);
		break;
	}

	case EStoryFlowNodeType::EqualEnum:
	{
		FString Input1 = EvaluateEnumInput(Node, TEXT("enum-1"), Node->Data.Value1.GetString());
		FString Input2 = EvaluateEnumInput(Node, TEXT("enum-2"), Node->Data.Value2.GetString());
		Result = Input1.Equals(Input2);
		break;
	}

	case EStoryFlowNodeType::IntToBoolean:
	{
		int32 Input = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		Result = Input != 0;
		break;
	}

	case EStoryFlowNodeType::FloatToBoolean:
	{
		float Input = EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f));
		Result = !FMath::IsNearlyZero(Input);
		break;
	}

	case EStoryFlowNodeType::ArrayContainsBool:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
		bool Value = EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
		Result = Array.ContainsByPredicate([Value](const FStoryFlowVariant& V) { return V.GetBool() == Value; });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsInt:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, TEXT("integer-array"));
		int32 Value = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		Result = Array.ContainsByPredicate([Value](const FStoryFlowVariant& V) { return V.GetInt() == Value; });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsFloat:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, TEXT("float-array"));
		float Value = EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f));
		Result = Array.ContainsByPredicate([Value](const FStoryFlowVariant& V) { return FMath::IsNearlyEqual(V.GetFloat(), Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsString:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, TEXT("string-array"));
		FString Value = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsImage:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, TEXT("image-array"));
		FString Value = EvaluateStringInput(Node, TEXT("image"), Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsCharacter:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, TEXT("character-array"));
		FString Value = EvaluateStringInput(Node, TEXT("character"), Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsAudio:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, TEXT("audio-array"));
		FString Value = EvaluateStringInput(Node, TEXT("audio"), Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::GetBoolArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetBool();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomBoolArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetBool();
		}
		break;
	}

	default:
		Result = false;
		break;
	}

	// Cache result
	NodeState.CachedOutput.SetBool(Result);
	NodeState.bHasCachedOutput = true;

	return Result;
}

// ============================================================================
// Comparison Helpers
// ============================================================================

bool FStoryFlowEvaluator::EvaluateIntegerComparison(FStoryFlowNode* Node, EStoryFlowNodeType ComparisonType)
{
	int32 Input1 = EvaluateIntegerInput(Node, TEXT("integer-1"), Node->Data.Value1.GetInt(0));
	int32 Input2 = EvaluateIntegerInput(Node, TEXT("integer-2"), Node->Data.Value2.GetInt(0));

	switch (ComparisonType)
	{
	case EStoryFlowNodeType::GreaterThan:        return Input1 > Input2;
	case EStoryFlowNodeType::GreaterThanOrEqual: return Input1 >= Input2;
	case EStoryFlowNodeType::LessThan:           return Input1 < Input2;
	case EStoryFlowNodeType::LessThanOrEqual:    return Input1 <= Input2;
	case EStoryFlowNodeType::EqualInt:           return Input1 == Input2;
	default:                                     return false;
	}
}

bool FStoryFlowEvaluator::EvaluateFloatComparison(FStoryFlowNode* Node, EStoryFlowNodeType ComparisonType)
{
	float Input1 = EvaluateFloatInput(Node, TEXT("float-1"), Node->Data.Value1.GetFloat(0.0f));
	float Input2 = EvaluateFloatInput(Node, TEXT("float-2"), Node->Data.Value2.GetFloat(0.0f));

	switch (ComparisonType)
	{
	case EStoryFlowNodeType::GreaterThanFloat:        return Input1 > Input2;
	case EStoryFlowNodeType::GreaterThanOrEqualFloat: return Input1 >= Input2;
	case EStoryFlowNodeType::LessThanFloat:           return Input1 < Input2;
	case EStoryFlowNodeType::LessThanOrEqualFloat:    return Input1 <= Input2;
	case EStoryFlowNodeType::EqualFloat:              return FMath::IsNearlyEqual(Input1, Input2);
	default:                                          return false;
	}
}

// ============================================================================
// Integer Evaluation
// ============================================================================

int32 FStoryFlowEvaluator::EvaluateIntegerInput(FStoryFlowNode* Node, const FString& HandleSuffix, int32 Fallback)
{
	if (!Context || !Node)
	{
		return Fallback;
	}

	const FStoryFlowConnection* Edge = Context->FindInputEdge(Node->Id, HandleSuffix);
	if (!Edge)
	{
		return Fallback;
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);
	if (!SourceNode)
	{
		return Fallback;
	}

	return EvaluateIntegerFromNode(SourceNode, Node->Id, Edge->SourceHandle);
}

int32 FStoryFlowEvaluator::EvaluateIntegerFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle)
{
	if (!Node || !Context)
	{
		return 0;
	}

	bool bDepthValid;
	FDepthGuard Guard(Context->EvaluationDepth, STORYFLOW_MAX_EVALUATION_DEPTH, bDepthValid);
	if (!bDepthValid)
	{
		return 0;
	}

	int32 Result = 0;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetInt:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetInt() : 0;
		break;
	}

	case EStoryFlowNodeType::Plus:
	{
		int32 Input1 = EvaluateIntegerInput(Node, TEXT("integer-1"), Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, TEXT("integer-2"), Node->Data.Value2.GetInt(0));
		Result = Input1 + Input2;
		break;
	}

	case EStoryFlowNodeType::Minus:
	{
		int32 Input1 = EvaluateIntegerInput(Node, TEXT("integer-1"), Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, TEXT("integer-2"), Node->Data.Value2.GetInt(0));
		Result = Input1 - Input2;
		break;
	}

	case EStoryFlowNodeType::Multiply:
	{
		int32 Input1 = EvaluateIntegerInput(Node, TEXT("integer-1"), Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, TEXT("integer-2"), Node->Data.Value2.GetInt(0));
		Result = Input1 * Input2;
		break;
	}

	case EStoryFlowNodeType::Divide:
	{
		int32 Input1 = EvaluateIntegerInput(Node, TEXT("integer-1"), Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, TEXT("integer-2"), Node->Data.Value2.GetInt(1));
		Result = (Input2 != 0) ? (Input1 / Input2) : 0;
		break;
	}

	case EStoryFlowNodeType::Random:
	{
		int32 Min = EvaluateIntegerInput(Node, TEXT("integer-1"), Node->Data.Value1.GetInt(0));
		int32 Max = EvaluateIntegerInput(Node, TEXT("integer-2"), Node->Data.Value2.GetInt(100));
		if (Min > Max)
		{
			Swap(Min, Max);
		}
		Result = FMath::RandRange(Min, Max);
		break;
	}

	case EStoryFlowNodeType::BooleanToInt:
	{
		bool Input = EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
		Result = Input ? 1 : 0;
		break;
	}

	case EStoryFlowNodeType::FloatToInt:
	{
		float Input = EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f));
		Result = FMath::FloorToInt(Input);
		break;
	}

	case EStoryFlowNodeType::StringToInt:
	{
		FString Input = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = FCString::Atoi(*Input);
		break;
	}

	case EStoryFlowNodeType::ArrayLengthBool:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthInt:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, TEXT("integer-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthFloat:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, TEXT("float-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthString:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, TEXT("string-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::FindInBoolArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
		bool Value = EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
		Result = Array.IndexOfByPredicate([Value](const FStoryFlowVariant& V) { return V.GetBool() == Value; });
		break;
	}

	case EStoryFlowNodeType::FindInIntArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, TEXT("integer-array"));
		int32 Value = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		Result = Array.IndexOfByPredicate([Value](const FStoryFlowVariant& V) { return V.GetInt() == Value; });
		break;
	}

	case EStoryFlowNodeType::FindInFloatArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, TEXT("float-array"));
		float Value = EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f));
		Result = Array.IndexOfByPredicate([Value](const FStoryFlowVariant& V) { return FMath::IsNearlyEqual(V.GetFloat(), Value); });
		break;
	}

	case EStoryFlowNodeType::FindInStringArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, TEXT("string-array"));
		FString Value = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ForEachIntLoop:
	case EStoryFlowNodeType::ForEachBoolLoop:
	case EStoryFlowNodeType::ForEachFloatLoop:
	case EStoryFlowNodeType::ForEachStringLoop:
	case EStoryFlowNodeType::ForEachImageLoop:
	case EStoryFlowNodeType::ForEachCharacterLoop:
	case EStoryFlowNodeType::ForEachAudioLoop:
	{
		FNodeRuntimeState& LoopState = Context->GetNodeState(Node->Id);
		if (SourceHandle.Contains(TEXT("integer-index")))
		{
			Result = LoopState.LoopIndex;
		}
		else if (Node->Type == EStoryFlowNodeType::ForEachIntLoop && LoopState.bHasCachedOutput)
		{
			// Return current element value for ForEachIntLoop
			Result = LoopState.CachedOutput.GetInt();
		}
		break;
	}

	case EStoryFlowNodeType::GetIntArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, TEXT("integer-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetInt();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomIntArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, TEXT("integer-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetInt();
		}
		break;
	}

	case EStoryFlowNodeType::LengthString:
	{
		FString Input = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = Input.Len();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthImage:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, TEXT("image-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthCharacter:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, TEXT("character-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthAudio:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, TEXT("audio-array"));
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::FindInImageArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, TEXT("image-array"));
		FString Value = EvaluateStringInput(Node, TEXT("image"), Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::FindInCharacterArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, TEXT("character-array"));
		FString Value = EvaluateStringInput(Node, TEXT("character"), Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::FindInAudioArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, TEXT("audio-array"));
		FString Value = EvaluateStringInput(Node, TEXT("audio"), Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	default:
		Result = 0;
		break;
	}

	return Result;
}

// ============================================================================
// Float Evaluation
// ============================================================================

float FStoryFlowEvaluator::EvaluateFloatInput(FStoryFlowNode* Node, const FString& HandleSuffix, float Fallback)
{
	if (!Context || !Node)
	{
		return Fallback;
	}

	const FStoryFlowConnection* Edge = Context->FindInputEdge(Node->Id, HandleSuffix);
	if (!Edge)
	{
		return Fallback;
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);
	if (!SourceNode)
	{
		return Fallback;
	}

	return EvaluateFloatFromNode(SourceNode, Node->Id, Edge->SourceHandle);
}

float FStoryFlowEvaluator::EvaluateFloatFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle)
{
	if (!Node || !Context)
	{
		return 0.0f;
	}

	bool bDepthValid;
	FDepthGuard Guard(Context->EvaluationDepth, STORYFLOW_MAX_EVALUATION_DEPTH, bDepthValid);
	if (!bDepthValid)
	{
		return 0.0f;
	}

	float Result = 0.0f;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetFloat:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetFloat() : 0.0f;
		break;
	}

	case EStoryFlowNodeType::PlusFloat:
	{
		float Input1 = EvaluateFloatInput(Node, TEXT("float-1"), Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, TEXT("float-2"), Node->Data.Value2.GetFloat(0.0f));
		Result = Input1 + Input2;
		break;
	}

	case EStoryFlowNodeType::MinusFloat:
	{
		float Input1 = EvaluateFloatInput(Node, TEXT("float-1"), Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, TEXT("float-2"), Node->Data.Value2.GetFloat(0.0f));
		Result = Input1 - Input2;
		break;
	}

	case EStoryFlowNodeType::MultiplyFloat:
	{
		float Input1 = EvaluateFloatInput(Node, TEXT("float-1"), Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, TEXT("float-2"), Node->Data.Value2.GetFloat(0.0f));
		Result = Input1 * Input2;
		break;
	}

	case EStoryFlowNodeType::DivideFloat:
	{
		float Input1 = EvaluateFloatInput(Node, TEXT("float-1"), Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, TEXT("float-2"), Node->Data.Value2.GetFloat(1.0f));
		Result = !FMath::IsNearlyZero(Input2) ? (Input1 / Input2) : 0.0f;
		break;
	}

	case EStoryFlowNodeType::RandomFloat:
	{
		float Min = EvaluateFloatInput(Node, TEXT("float-1"), Node->Data.Value1.GetFloat(0.0f));
		float Max = EvaluateFloatInput(Node, TEXT("float-2"), Node->Data.Value2.GetFloat(1.0f));
		if (Min > Max)
		{
			Swap(Min, Max);
		}
		Result = FMath::FRandRange(Min, Max);
		break;
	}

	case EStoryFlowNodeType::BooleanToFloat:
	{
		bool Input = EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
		Result = Input ? 1.0f : 0.0f;
		break;
	}

	case EStoryFlowNodeType::IntToFloat:
	{
		int32 Input = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		Result = static_cast<float>(Input);
		break;
	}

	case EStoryFlowNodeType::StringToFloat:
	{
		FString Input = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = FCString::Atof(*Input);
		break;
	}

	case EStoryFlowNodeType::GetFloatArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, TEXT("float-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetFloat();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomFloatArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, TEXT("float-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetFloat();
		}
		break;
	}

	case EStoryFlowNodeType::ForEachFloatLoop:
	{
		// Return current element value
		FNodeRuntimeState& LoopState = Context->GetNodeState(Node->Id);
		if (LoopState.bHasCachedOutput)
		{
			Result = LoopState.CachedOutput.GetFloat();
		}
		break;
	}

	default:
		Result = 0.0f;
		break;
	}

	return Result;
}

// ============================================================================
// String Evaluation
// ============================================================================

FString FStoryFlowEvaluator::EvaluateStringInput(FStoryFlowNode* Node, const FString& HandleSuffix, const FString& Fallback)
{
	if (!Context || !Node)
	{
		return Fallback;
	}

	const FStoryFlowConnection* Edge = Context->FindInputEdge(Node->Id, HandleSuffix);
	if (!Edge)
	{
		return Fallback;
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);
	if (!SourceNode)
	{
		return Fallback;
	}

	return EvaluateStringFromNode(SourceNode, Node->Id, Edge->SourceHandle);
}

FString FStoryFlowEvaluator::EvaluateStringFromNode(FStoryFlowNode* Node, const FString& TargetNodeId, const FString& SourceHandle)
{
	if (!Node || !Context)
	{
		return TEXT("");
	}

	bool bDepthValid;
	FDepthGuard Guard(Context->EvaluationDepth, STORYFLOW_MAX_EVALUATION_DEPTH, bDepthValid);
	if (!bDepthValid)
	{
		return TEXT("");
	}

	FString Result;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetString:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::ConcatenateString:
	{
		FString Input1 = EvaluateStringInput(Node, TEXT("string-1"), Node->Data.Value1.GetString());
		FString Input2 = EvaluateStringInput(Node, TEXT("string-2"), Node->Data.Value2.GetString());
		Result = Input1 + Input2;
		break;
	}

	case EStoryFlowNodeType::ToUpperCase:
	{
		FString Input = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = Input.ToUpper();
		break;
	}

	case EStoryFlowNodeType::ToLowerCase:
	{
		FString Input = EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
		Result = Input.ToLower();
		break;
	}

	case EStoryFlowNodeType::IntToString:
	{
		int32 Input = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		Result = FString::FromInt(Input);
		break;
	}

	case EStoryFlowNodeType::FloatToString:
	{
		float Input = EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f));
		Result = FString::SanitizeFloat(Input);
		break;
	}

	case EStoryFlowNodeType::GetEnum:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::EnumToString:
	{
		FString Input = EvaluateEnumInput(Node, TEXT("enum"), Node->Data.Value.GetString());
		Result = Input;
		break;
	}

	// Asset variable getters return paths as strings
	case EStoryFlowNodeType::GetImage:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::GetAudio:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::GetCharacter:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::GetStringArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, TEXT("string-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomStringArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, TEXT("string-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetImageArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, TEXT("image-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomImageArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, TEXT("image-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetCharacterArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, TEXT("character-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomCharacterArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, TEXT("character-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetAudioArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, TEXT("audio-array"));
		int32 Index = EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomAudioArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, TEXT("audio-array"));
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	// ForEach loop element access (string-valued)
	case EStoryFlowNodeType::ForEachStringLoop:
	case EStoryFlowNodeType::ForEachImageLoop:
	case EStoryFlowNodeType::ForEachCharacterLoop:
	case EStoryFlowNodeType::ForEachAudioLoop:
	{
		FNodeRuntimeState& LoopState = Context->GetNodeState(Node->Id);
		if (LoopState.bHasCachedOutput)
		{
			Result = LoopState.CachedOutput.GetString();
		}
		break;
	}

	default:
		Result = TEXT("");
		break;
	}

	return Result;
}

// ============================================================================
// Enum Evaluation
// ============================================================================

FString FStoryFlowEvaluator::EvaluateEnumInput(FStoryFlowNode* Node, const FString& HandleSuffix, const FString& Fallback)
{
	// Enums are stored as strings
	return EvaluateStringInput(Node, HandleSuffix, Fallback);
}

// ============================================================================
// Array Evaluation
// ============================================================================

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateArrayInputGeneric(FStoryFlowNode* Node, const FString& HandleSuffix, EStoryFlowNodeType ExpectedGetArrayType)
{
	if (!Context || !Node)
	{
		return TArray<FStoryFlowVariant>();
	}

	const FStoryFlowConnection* Edge = Context->FindInputEdge(Node->Id, HandleSuffix);
	if (!Edge)
	{
		return TArray<FStoryFlowVariant>();
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);
	if (!SourceNode || SourceNode->Type != ExpectedGetArrayType)
	{
		return TArray<FStoryFlowVariant>();
	}

	FStoryFlowVariable* Var = Context->FindVariable(SourceNode->Data.Variable, SourceNode->Data.bIsGlobal);
	if (Var && Var->bIsArray)
	{
		return Var->Value.GetArray();
	}

	return TArray<FStoryFlowVariant>();
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateBoolArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetBoolArray);
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateIntArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetIntArray);
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateFloatArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetFloatArray);
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateStringArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetStringArray);
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateImageArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetImageArray);
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateCharacterArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetCharacterArray);
}

TArray<FStoryFlowVariant> FStoryFlowEvaluator::EvaluateAudioArrayInput(FStoryFlowNode* Node, const FString& HandleSuffix)
{
	return EvaluateArrayInputGeneric(Node, HandleSuffix, EStoryFlowNodeType::GetAudioArray);
}

// ============================================================================
// Boolean Chain Processing
// ============================================================================

void FStoryFlowEvaluator::ProcessBooleanChain(FStoryFlowNode* Node)
{
	if (!Node || !Context)
	{
		return;
	}

	// Use depth guard to prevent infinite recursion
	bool bDepthValid;
	FDepthGuard Guard(Context->EvaluationDepth, STORYFLOW_MAX_EVALUATION_DEPTH, bDepthValid);
	if (!bDepthValid)
	{
		return;
	}

	FNodeRuntimeState& NodeState = Context->GetNodeState(Node->Id);

	switch (Node->Type)
	{
	case EStoryFlowNodeType::NotBool:
	{
		// Process input first
		if (const FStoryFlowConnection* InputEdge = Context->FindInputEdge(Node->Id, TEXT("boolean")))
		{
			if (FStoryFlowNode* SourceNode = Context->GetNode(InputEdge->Source))
			{
				ProcessBooleanChain(SourceNode);
			}
		}
		// Then evaluate
		bool Input = EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
		NodeState.CachedOutput.SetBool(!Input);
		NodeState.bHasCachedOutput = true;
		break;
	}

	case EStoryFlowNodeType::AndBool:
	case EStoryFlowNodeType::OrBool:
	case EStoryFlowNodeType::EqualBool:
	{
		// Process both inputs recursively
		if (const FStoryFlowConnection* Edge1 = Context->FindInputEdge(Node->Id, TEXT("boolean-1")))
		{
			if (FStoryFlowNode* Source1 = Context->GetNode(Edge1->Source))
			{
				ProcessBooleanChain(Source1);
			}
		}
		if (const FStoryFlowConnection* Edge2 = Context->FindInputEdge(Node->Id, TEXT("boolean-2")))
		{
			if (FStoryFlowNode* Source2 = Context->GetNode(Edge2->Source))
			{
				ProcessBooleanChain(Source2);
			}
		}
		break;
	}

	case EStoryFlowNodeType::Branch:
	{
		// Process condition input
		if (const FStoryFlowConnection* CondEdge = Context->FindInputEdge(Node->Id, TEXT("boolean-condition")))
		{
			if (FStoryFlowNode* CondNode = Context->GetNode(CondEdge->Source))
			{
				ProcessBooleanChain(CondNode);
			}
		}
		break;
	}

	default:
	{
		// For any other type that produces a boolean (comparisons, array contains, type conversions, etc.),
		// delegate to EvaluateBooleanFromNode which handles all 47+ types.
		// This fixes the mismatch where ProcessBooleanChain handled ~12 types but IsBooleanProducer listed 30+.
		EvaluateBooleanFromNode(Node, TEXT(""), TEXT(""));
		break;
	}
	}
}

bool FStoryFlowEvaluator::EvaluateOptionVisibility(FStoryFlowNode* DialogueNode, const FString& OptionId)
{
	if (!DialogueNode || !Context)
	{
		return true; // Default to visible
	}

	// Check if there's a visibility connection for this option
	FString HandleSuffix = FString::Printf(TEXT("boolean-%s"), *OptionId);
	const FStoryFlowConnection* Edge = Context->FindInputEdge(DialogueNode->Id, HandleSuffix);

	if (!Edge)
	{
		return true; // No visibility connection means always visible
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);
	if (!SourceNode)
	{
		return true;
	}

	// Process the boolean chain to ensure all values are cached
	ProcessBooleanChain(SourceNode);

	// Evaluate the visibility
	return EvaluateBooleanFromNode(SourceNode, DialogueNode->Id, Edge->SourceHandle);
}

void FStoryFlowEvaluator::ClearCache()
{
	if (Context)
	{
		Context->ClearEvaluationCache();
	}
}

