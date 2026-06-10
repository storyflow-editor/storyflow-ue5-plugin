// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Evaluation/StoryFlowEvaluator.h"
#include "StoryFlowRuntime.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowHandles.h"

#define SF_EVAL_TRACE(Format, ...) \
	do { if (Context && Context->bTraceEnabled) { UE_LOG(LogStoryFlow, Log, TEXT("[SF-TRACE] " Format), ##__VA_ARGS__); } } while(0)

static bool IsArrayModifyNode(EStoryFlowNodeType Type)
{
	switch (Type)
	{
	case EStoryFlowNodeType::AddToBoolArray:
	case EStoryFlowNodeType::AddToIntArray:
	case EStoryFlowNodeType::AddToFloatArray:
	case EStoryFlowNodeType::AddToStringArray:
	case EStoryFlowNodeType::AddToImageArray:
	case EStoryFlowNodeType::AddToCharacterArray:
	case EStoryFlowNodeType::AddToAudioArray:
	case EStoryFlowNodeType::RemoveFromBoolArray:
	case EStoryFlowNodeType::RemoveFromIntArray:
	case EStoryFlowNodeType::RemoveFromFloatArray:
	case EStoryFlowNodeType::RemoveFromStringArray:
	case EStoryFlowNodeType::RemoveFromImageArray:
	case EStoryFlowNodeType::RemoveFromCharacterArray:
	case EStoryFlowNodeType::RemoveFromAudioArray:
	case EStoryFlowNodeType::ClearBoolArray:
	case EStoryFlowNodeType::ClearIntArray:
	case EStoryFlowNodeType::ClearFloatArray:
	case EStoryFlowNodeType::ClearStringArray:
	case EStoryFlowNodeType::ClearImageArray:
	case EStoryFlowNodeType::ClearCharacterArray:
	case EStoryFlowNodeType::ClearAudioArray:
		return true;
	default:
		return false;
	}
}

// Set*Array nodes expose the variable they set on their array output port,
// exactly like their Get*Array twin (HTML pairs 'getBoolArray'/'setBoolArray', etc.)
static EStoryFlowNodeType SetArrayTwinOf(EStoryFlowNodeType GetArrayType)
{
	switch (GetArrayType)
	{
	case EStoryFlowNodeType::GetBoolArray:      return EStoryFlowNodeType::SetBoolArray;
	case EStoryFlowNodeType::GetIntArray:       return EStoryFlowNodeType::SetIntArray;
	case EStoryFlowNodeType::GetFloatArray:     return EStoryFlowNodeType::SetFloatArray;
	case EStoryFlowNodeType::GetStringArray:    return EStoryFlowNodeType::SetStringArray;
	case EStoryFlowNodeType::GetImageArray:     return EStoryFlowNodeType::SetImageArray;
	case EStoryFlowNodeType::GetCharacterArray: return EStoryFlowNodeType::SetCharacterArray;
	case EStoryFlowNodeType::GetAudioArray:     return EStoryFlowNodeType::SetAudioArray;
	default:                                    return GetArrayType;
	}
}

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

void FStoryFlowEvaluator::MaybeWarnUnknownNode(const FStoryFlowNode* Node)
{
	if (!Node || !Context)
	{
		return;
	}
	if (Node->Type != EStoryFlowNodeType::Unknown)
	{
		return;
	}
	if (Context->WarnedUnknownNodes.Contains(Node->Id))
	{
		return;
	}
	Context->WarnedUnknownNodes.Add(Node->Id);
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Unsupported node type '%s' at node %s, returning default value"), *Node->TypeString, *Node->Id);
}

void FStoryFlowEvaluator::MaybeWarnMissingMapTypes(const FStoryFlowNode* Node)
{
	if (!Node || !Context)
	{
		return;
	}
	if (Context->WarnedMapNodes.Contains(Node->Id))
	{
		return;
	}
	Context->WarnedMapNodes.Add(Node->Id);
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map node '%s' at node %s is missing keyType/valueType data, returning default value"), *Node->TypeString, *Node->Id);
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

	// Forward-compat: warn (once) and fall through to default if the source
	// node is a type the plugin does not understand.
	MaybeWarnUnknownNode(Node);

	// Map reads are never memoized: maps resolve to LIVE variable storage and
	// in-place mutations (setMapValue/removeMapKey/clearMap) must be observable
	// on the next read. The HTML runtime recomputes map reads inline the same way.
	const bool bIsMapRead = (Node->Type == EStoryFlowNodeType::GetMapValue || Node->Type == EStoryFlowNodeType::HasMapKey);

	// Check cache first
	FNodeRuntimeState& NodeState = Context->GetNodeState(Node->Id);
	if (!bIsMapRead && NodeState.bHasCachedOutput && NodeState.CachedOutput.GetType() == EStoryFlowVariableType::Boolean)
	{
		return NodeState.CachedOutput.GetBool();
	}

	bool Result = false;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetBool:
	case EStoryFlowNodeType::SetBool:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetBool() : false;
		break;
	}

	case EStoryFlowNodeType::NotBool:
	{
		bool Input = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean, Node->Data.Value.GetBool(false));
		Result = !Input;
		break;
	}

	case EStoryFlowNodeType::AndBool:
	{
		bool Input1 = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean1, Node->Data.Value1.GetBool(false));
		bool Input2 = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean2, Node->Data.Value2.GetBool(false));
		Result = Input1 && Input2;
		break;
	}

	case EStoryFlowNodeType::OrBool:
	{
		bool Input1 = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean1, Node->Data.Value1.GetBool(false));
		bool Input2 = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean2, Node->Data.Value2.GetBool(false));
		Result = Input1 || Input2;
		break;
	}

	case EStoryFlowNodeType::EqualBool:
	{
		bool Input1 = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean1, Node->Data.Value1.GetBool(false));
		bool Input2 = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean2, Node->Data.Value2.GetBool(false));
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
		FString Input1 = EvaluateStringInput(Node, StoryFlowHandles::In_String1, Context->GetString(Node->Data.Value1.GetString()));
		FString Input2 = EvaluateStringInput(Node, StoryFlowHandles::In_String2, Context->GetString(Node->Data.Value2.GetString()));
		Result = Input1.Equals(Input2);
		break;
	}

	case EStoryFlowNodeType::ContainsString:
	{
		FString Haystack = EvaluateStringInput(Node, StoryFlowHandles::In_String1, Context->GetString(Node->Data.Value1.GetString()));
		FString Needle = EvaluateStringInput(Node, StoryFlowHandles::In_String2, Context->GetString(Node->Data.Value2.GetString()));
		Result = Haystack.Contains(Needle);
		break;
	}

	case EStoryFlowNodeType::EqualEnum:
	{
		FString Input1 = EvaluateEnumInput(Node, StoryFlowHandles::In_Enum1, Node->Data.Value1.GetString());
		FString Input2 = EvaluateEnumInput(Node, StoryFlowHandles::In_Enum2, Node->Data.Value2.GetString());
		Result = Input1.Equals(Input2);
		break;
	}

	case EStoryFlowNodeType::IntToBoolean:
	{
		int32 Input = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		Result = Input != 0;
		break;
	}

	case EStoryFlowNodeType::FloatToBoolean:
	{
		float Input = EvaluateFloatInput(Node, StoryFlowHandles::In_Float, Node->Data.Value.GetFloat(0.0f));
		Result = !FMath::IsNearlyZero(Input);
		break;
	}

	case EStoryFlowNodeType::ArrayContainsBool:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, StoryFlowHandles::In_BoolArray);
		bool Value = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean, Node->Data.Value.GetBool(false));
		Result = Array.ContainsByPredicate([Value](const FStoryFlowVariant& V) { return V.GetBool() == Value; });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsInt:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, StoryFlowHandles::In_IntArray);
		int32 Value = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		Result = Array.ContainsByPredicate([Value](const FStoryFlowVariant& V) { return V.GetInt() == Value; });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsFloat:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, StoryFlowHandles::In_FloatArray);
		float Value = EvaluateFloatInput(Node, StoryFlowHandles::In_Float, Node->Data.Value.GetFloat(0.0f));
		Result = Array.ContainsByPredicate([Value](const FStoryFlowVariant& V) { return FMath::IsNearlyEqual(V.GetFloat(), Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsString:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, StoryFlowHandles::In_StringArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_String, Context->GetString(Node->Data.Value.GetString()));
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsImage:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, StoryFlowHandles::In_ImageArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_Image, Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsCharacter:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, StoryFlowHandles::In_CharacterArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_Character, Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::ArrayContainsAudio:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, StoryFlowHandles::In_AudioArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_Audio, Node->Data.Value.GetString());
		Result = Array.ContainsByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::GetBoolArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, StoryFlowHandles::In_BoolArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetBool();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomBoolArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, StoryFlowHandles::In_BoolArray);
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetBool();
		}
		break;
	}

	// Map op arms branch on the node's OWN Data.KeyType/Data.ValueType strings —
	// a NEW pattern for this evaluator: catalog map ops carry K/V in node data,
	// unlike array ops which encode the element type in the node type itself.
	case EStoryFlowNodeType::GetMapValue:
	{
		// getMapValue exposes two outputs sharing one node:
		//   "source-{id}-{valueType}-value" and "source-{id}-boolean-isValid".
		// Discriminate by SourceHandle suffix (precedent: runScript "-out-" parsing).
		FStoryFlowVariant Value;
		const bool bFound = ComputeGetMapValue(Node, Value);
		if (SourceHandle.EndsWith(TEXT("-isValid")))
		{
			// IsValid is always boolean, regardless of the node's ValueType
			Result = bFound;
		}
		else if (Node->Data.ValueType == TEXT("boolean"))
		{
			Result = Value.GetBool(false);
		}
		break;
	}

	case EStoryFlowNodeType::HasMapKey:
	{
		// Key FIRST: the map pointer may alias node-state storage that any
		// further evaluation (GetNodeState/FindOrAdd) can relocate, so resolve
		// the map last and consume it immediately.
		const FStoryFlowVariant Key = EvaluateMapOpKeyInput(Node, TEXT("2"));
		if (const TArray<FStoryFlowMapEntry>* Map = EvaluateMapInput(Node, TEXT("1")))
		{
			Result = FindMapEntryByKey(*Map, Node->Data.KeyType, Key) != INDEX_NONE;
		}
		break;
	}

	case EStoryFlowNodeType::RunScript:
	{
		if (NodeState.bHasOutputValues && !SourceHandle.IsEmpty())
		{
			int32 OutIdx = SourceHandle.Find(TEXT("-out-"));
			if (OutIdx != INDEX_NONE)
			{
				FString VarId = SourceHandle.Mid(OutIdx + 5); // UUID from handle
				// Map UUID to variable Name via ScriptOutputs (OutputValues is keyed by Name)
				FString VarName;
				for (const auto& Output : Node->Data.ScriptOutputs)
				{
					if (Output.Id == VarId)
					{
						VarName = Output.Name;
						break;
					}
				}
				if (!VarName.IsEmpty())
				{
					if (const FStoryFlowVariant* Val = NodeState.OutputValues.Find(VarName))
					{
						Result = Val->GetBool();
					}
				}
			}
		}
		break;
	}

	case EStoryFlowNodeType::GetCharacterVar:
	case EStoryFlowNodeType::SetCharacterVar:
	{
		FString CharPath = Node->Data.CharacterPath;
		if (const FStoryFlowConnection* CharEdge = Context->FindInputEdge(Node->Id, StoryFlowHandles::In_CharacterInput))
		{
			if (FStoryFlowNode* CharNode = Context->GetNode(CharEdge->Source))
			{
				CharPath = EvaluateStringFromNode(CharNode, Node->Id, CharEdge->SourceHandle);
			}
		}
		FStoryFlowVariant CharVal = Context->GetCharacterVariableValue(CharPath, Node->Data.VariableName);
		Result = CharVal.GetBool();
		break;
	}

	default:
		Result = false;
		break;
	}

	SF_EVAL_TRACE("EVAL %s %s result=%s", *Node->Id, *Node->TypeString, Result ? TEXT("true") : TEXT("false"));

	// Cache result (map reads excluded — see bIsMapRead above)
	if (!bIsMapRead)
	{
		NodeState.CachedOutput.SetBool(Result);
		NodeState.bHasCachedOutput = true;
	}

	return Result;
}

// ============================================================================
// Comparison Helpers
// ============================================================================

bool FStoryFlowEvaluator::EvaluateIntegerComparison(FStoryFlowNode* Node, EStoryFlowNodeType ComparisonType)
{
	int32 Input1 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer1, Node->Data.Value1.GetInt(0));
	int32 Input2 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer2, Node->Data.Value2.GetInt(0));

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
	float Input1 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float1, Node->Data.Value1.GetFloat(0.0f));
	float Input2 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float2, Node->Data.Value2.GetFloat(0.0f));

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

	// Forward-compat: warn (once) and fall through to default if the source
	// node is a type the plugin does not understand.
	MaybeWarnUnknownNode(Node);

	int32 Result = 0;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetInt:
	case EStoryFlowNodeType::SetInt:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetInt() : 0;
		break;
	}

	case EStoryFlowNodeType::Plus:
	{
		int32 Input1 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer1, Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer2, Node->Data.Value2.GetInt(0));
		Result = Input1 + Input2;
		break;
	}

	case EStoryFlowNodeType::Minus:
	{
		int32 Input1 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer1, Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer2, Node->Data.Value2.GetInt(0));
		Result = Input1 - Input2;
		break;
	}

	case EStoryFlowNodeType::Multiply:
	{
		int32 Input1 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer1, Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer2, Node->Data.Value2.GetInt(0));
		Result = Input1 * Input2;
		break;
	}

	case EStoryFlowNodeType::Divide:
	{
		int32 Input1 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer1, Node->Data.Value1.GetInt(0));
		int32 Input2 = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer2, Node->Data.Value2.GetInt(1));
		Result = (Input2 != 0) ? (Input1 / Input2) : 0;
		break;
	}

	case EStoryFlowNodeType::Random:
	{
		int32 Min = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer1, Node->Data.Value1.GetInt(0));
		int32 Max = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer2, Node->Data.Value2.GetInt(100));
		if (Min > Max)
		{
			Swap(Min, Max);
		}
		Result = FMath::RandRange(Min, Max);
		break;
	}

	case EStoryFlowNodeType::BooleanToInt:
	{
		bool Input = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean, Node->Data.Value.GetBool(false));
		Result = Input ? 1 : 0;
		break;
	}

	case EStoryFlowNodeType::FloatToInt:
	{
		float Input = EvaluateFloatInput(Node, StoryFlowHandles::In_Float, Node->Data.Value.GetFloat(0.0f));
		Result = FMath::FloorToInt(Input);
		break;
	}

	case EStoryFlowNodeType::StringToInt:
	{
		FString Input = EvaluateStringInput(Node, StoryFlowHandles::In_String, Node->Data.Value.GetString());
		Result = FCString::Atoi(*Input);
		break;
	}

	case EStoryFlowNodeType::ArrayLengthBool:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, StoryFlowHandles::In_BoolArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthInt:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, StoryFlowHandles::In_IntArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthFloat:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, StoryFlowHandles::In_FloatArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthString:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, StoryFlowHandles::In_StringArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::FindInBoolArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateBoolArrayInput(Node, StoryFlowHandles::In_BoolArray);
		bool Value = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean, Node->Data.Value.GetBool(false));
		Result = Array.IndexOfByPredicate([Value](const FStoryFlowVariant& V) { return V.GetBool() == Value; });
		break;
	}

	case EStoryFlowNodeType::FindInIntArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, StoryFlowHandles::In_IntArray);
		int32 Value = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		Result = Array.IndexOfByPredicate([Value](const FStoryFlowVariant& V) { return V.GetInt() == Value; });
		break;
	}

	case EStoryFlowNodeType::FindInFloatArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, StoryFlowHandles::In_FloatArray);
		float Value = EvaluateFloatInput(Node, StoryFlowHandles::In_Float, Node->Data.Value.GetFloat(0.0f));
		Result = Array.IndexOfByPredicate([Value](const FStoryFlowVariant& V) { return FMath::IsNearlyEqual(V.GetFloat(), Value); });
		break;
	}

	case EStoryFlowNodeType::FindInStringArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, StoryFlowHandles::In_StringArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_String, Context->GetString(Node->Data.Value.GetString()));
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
		if (SourceHandle.Contains(StoryFlowHandles::In_IntegerIndex))
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
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, StoryFlowHandles::In_IntArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetInt();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomIntArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateIntArrayInput(Node, StoryFlowHandles::In_IntArray);
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetInt();
		}
		break;
	}

	// Map ops branch on the node's Data.KeyType/Data.ValueType (K/V in node data
	// — see the boolean evaluator's map arms for the pattern note)
	case EStoryFlowNodeType::MapSize:
	{
		// Unresolved/missing-K-V map input falls through to 0 (HTML runtime parity)
		const TArray<FStoryFlowMapEntry>* Map = EvaluateMapInput(Node, TEXT("1"));
		Result = Map ? Map->Num() : 0;
		break;
	}

	case EStoryFlowNodeType::GetMapValue:
	{
		if (Node->Data.ValueType == TEXT("integer"))
		{
			FStoryFlowVariant Value;
			ComputeGetMapValue(Node, Value);
			Result = Value.GetInt(0);
		}
		break;
	}

	case EStoryFlowNodeType::LengthString:
	{
		FString Input = EvaluateStringInput(Node, StoryFlowHandles::In_String, Node->Data.Value.GetString());
		Result = Input.Len();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthImage:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, StoryFlowHandles::In_ImageArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthCharacter:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, StoryFlowHandles::In_CharacterArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::ArrayLengthAudio:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, StoryFlowHandles::In_AudioArray);
		Result = Array.Num();
		break;
	}

	case EStoryFlowNodeType::FindInImageArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, StoryFlowHandles::In_ImageArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_Image, Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::FindInCharacterArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, StoryFlowHandles::In_CharacterArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_Character, Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::FindInAudioArray:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, StoryFlowHandles::In_AudioArray);
		FString Value = EvaluateStringInput(Node, StoryFlowHandles::In_Audio, Node->Data.Value.GetString());
		Result = Array.IndexOfByPredicate([&Value](const FStoryFlowVariant& V) { return V.GetString().Equals(Value); });
		break;
	}

	case EStoryFlowNodeType::RunScript:
	{
		FNodeRuntimeState& RSState = Context->GetNodeState(Node->Id);
		if (RSState.bHasOutputValues && !SourceHandle.IsEmpty())
		{
			int32 OutIdx = SourceHandle.Find(TEXT("-out-"));
			if (OutIdx != INDEX_NONE)
			{
				FString VarId = SourceHandle.Mid(OutIdx + 5);
				FString VarName;
				for (const auto& Output : Node->Data.ScriptOutputs)
				{
					if (Output.Id == VarId)
					{
						VarName = Output.Name;
						break;
					}
				}
				if (!VarName.IsEmpty())
				{
					if (const FStoryFlowVariant* Val = RSState.OutputValues.Find(VarName))
					{
						Result = Val->GetInt();
					}
				}
			}
		}
		break;
	}

	case EStoryFlowNodeType::GetCharacterVar:
	case EStoryFlowNodeType::SetCharacterVar:
	{
		FString CharPath = Node->Data.CharacterPath;
		if (UStoryFlowScriptAsset* Script = Context->CurrentScript.Get())
		{
			if (const FStoryFlowConnection* CharEdge = Script->FindInputEdge(Node->Id, StoryFlowHandles::In_CharacterInput))
			{
				if (FStoryFlowNode* CharNode = Context->GetNode(CharEdge->Source))
				{
					CharPath = EvaluateStringFromNode(CharNode, Node->Id, CharEdge->SourceHandle);
				}
			}
		}
		FStoryFlowVariant CharVal = Context->GetCharacterVariableValue(CharPath, Node->Data.VariableName);
		Result = CharVal.GetInt();
		break;
	}

	default:
		Result = 0;
		break;
	}

	SF_EVAL_TRACE("EVAL %s %s result=%d", *Node->Id, *Node->TypeString, Result);

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

	// Forward-compat: warn (once) and fall through to default if the source
	// node is a type the plugin does not understand.
	MaybeWarnUnknownNode(Node);

	float Result = 0.0f;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetFloat:
	case EStoryFlowNodeType::SetFloat:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetFloat() : 0.0f;
		break;
	}

	case EStoryFlowNodeType::PlusFloat:
	{
		float Input1 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float1, Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float2, Node->Data.Value2.GetFloat(0.0f));
		Result = Input1 + Input2;
		break;
	}

	case EStoryFlowNodeType::MinusFloat:
	{
		float Input1 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float1, Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float2, Node->Data.Value2.GetFloat(0.0f));
		Result = Input1 - Input2;
		break;
	}

	case EStoryFlowNodeType::MultiplyFloat:
	{
		float Input1 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float1, Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float2, Node->Data.Value2.GetFloat(0.0f));
		Result = Input1 * Input2;
		break;
	}

	case EStoryFlowNodeType::DivideFloat:
	{
		float Input1 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float1, Node->Data.Value1.GetFloat(0.0f));
		float Input2 = EvaluateFloatInput(Node, StoryFlowHandles::In_Float2, Node->Data.Value2.GetFloat(1.0f));
		Result = !FMath::IsNearlyZero(Input2) ? (Input1 / Input2) : 0.0f;
		break;
	}

	case EStoryFlowNodeType::RandomFloat:
	{
		float Min = EvaluateFloatInput(Node, StoryFlowHandles::In_Float1, Node->Data.Value1.GetFloat(0.0f));
		float Max = EvaluateFloatInput(Node, StoryFlowHandles::In_Float2, Node->Data.Value2.GetFloat(1.0f));
		if (Min > Max)
		{
			Swap(Min, Max);
		}
		Result = FMath::FRandRange(Min, Max);
		break;
	}

	case EStoryFlowNodeType::BooleanToFloat:
	{
		bool Input = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean, Node->Data.Value.GetBool(false));
		Result = Input ? 1.0f : 0.0f;
		break;
	}

	case EStoryFlowNodeType::IntToFloat:
	{
		int32 Input = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		Result = static_cast<float>(Input);
		break;
	}

	case EStoryFlowNodeType::StringToFloat:
	{
		FString Input = EvaluateStringInput(Node, StoryFlowHandles::In_String, Node->Data.Value.GetString());
		Result = FCString::Atof(*Input);
		break;
	}

	case EStoryFlowNodeType::GetFloatArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, StoryFlowHandles::In_FloatArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetFloat();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomFloatArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateFloatArrayInput(Node, StoryFlowHandles::In_FloatArray);
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetFloat();
		}
		break;
	}

	// Map op branches on the node's Data.ValueType (K/V in node data — see the
	// boolean evaluator's map arms for the pattern note)
	case EStoryFlowNodeType::GetMapValue:
	{
		if (Node->Data.ValueType == TEXT("float"))
		{
			FStoryFlowVariant Value;
			ComputeGetMapValue(Node, Value);
			Result = Value.GetFloat(0.0f);
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

	case EStoryFlowNodeType::RunScript:
	{
		FNodeRuntimeState& RSState = Context->GetNodeState(Node->Id);
		if (RSState.bHasOutputValues && !SourceHandle.IsEmpty())
		{
			int32 OutIdx = SourceHandle.Find(TEXT("-out-"));
			if (OutIdx != INDEX_NONE)
			{
				FString VarId = SourceHandle.Mid(OutIdx + 5);
				FString VarName;
				for (const auto& Output : Node->Data.ScriptOutputs)
				{
					if (Output.Id == VarId)
					{
						VarName = Output.Name;
						break;
					}
				}
				if (!VarName.IsEmpty())
				{
					if (const FStoryFlowVariant* Val = RSState.OutputValues.Find(VarName))
					{
						Result = Val->GetFloat();
					}
				}
			}
		}
		break;
	}

	case EStoryFlowNodeType::GetCharacterVar:
	case EStoryFlowNodeType::SetCharacterVar:
	{
		FString CharPath = Node->Data.CharacterPath;
		if (UStoryFlowScriptAsset* Script = Context->CurrentScript.Get())
		{
			if (const FStoryFlowConnection* CharEdge = Script->FindInputEdge(Node->Id, StoryFlowHandles::In_CharacterInput))
			{
				if (FStoryFlowNode* CharNode = Context->GetNode(CharEdge->Source))
				{
					CharPath = EvaluateStringFromNode(CharNode, Node->Id, CharEdge->SourceHandle);
				}
			}
		}
		FStoryFlowVariant CharVal = Context->GetCharacterVariableValue(CharPath, Node->Data.VariableName);
		Result = CharVal.GetFloat();
		break;
	}

	default:
		Result = 0.0f;
		break;
	}

	SF_EVAL_TRACE("EVAL %s %s result=%s", *Node->Id, *Node->TypeString, *FString::SanitizeFloat(Result));

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

	// Forward-compat: warn (once) and fall through to default if the source
	// node is a type the plugin does not understand.
	MaybeWarnUnknownNode(Node);

	FString Result;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetString:
	case EStoryFlowNodeType::SetString:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::ConcatenateString:
	{
		FString Input1 = EvaluateStringInput(Node, StoryFlowHandles::In_String1, Context->GetString(Node->Data.Value1.GetString()));
		FString Input2 = EvaluateStringInput(Node, StoryFlowHandles::In_String2, Context->GetString(Node->Data.Value2.GetString()));
		Result = Input1 + Input2;
		break;
	}

	case EStoryFlowNodeType::ToUpperCase:
	{
		FString Input = EvaluateStringInput(Node, StoryFlowHandles::In_String, Node->Data.Value.GetString());
		Result = Input.ToUpper();
		break;
	}

	case EStoryFlowNodeType::ToLowerCase:
	{
		FString Input = EvaluateStringInput(Node, StoryFlowHandles::In_String, Node->Data.Value.GetString());
		Result = Input.ToLower();
		break;
	}

	case EStoryFlowNodeType::IntToString:
	{
		int32 Input = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		Result = FString::FromInt(Input);
		break;
	}

	case EStoryFlowNodeType::FloatToString:
	{
		float Input = EvaluateFloatInput(Node, StoryFlowHandles::In_Float, Node->Data.Value.GetFloat(0.0f));
		Result = FString::SanitizeFloat(Input);
		break;
	}

	case EStoryFlowNodeType::GetEnum:
	case EStoryFlowNodeType::SetEnum:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::EnumToString:
	{
		FString Input = EvaluateEnumInput(Node, StoryFlowHandles::In_Enum, Node->Data.Value.GetString());
		Result = Input;
		break;
	}

	case EStoryFlowNodeType::IntToEnum:
	{
		Result = EvaluateIntToEnum(Node);
		break;
	}

	case EStoryFlowNodeType::StringToEnum:
	{
		Result = EvaluateStringToEnum(Node);
		break;
	}

	// Asset variable getters return paths as strings
	case EStoryFlowNodeType::GetImage:
	case EStoryFlowNodeType::SetImage:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::SetBackgroundImage:
	{
		// As an image source the node exposes the image it sets: connected
		// image input first, then the dropdown value (matches HTML runtime).
		Result = EvaluateStringInput(Node, StoryFlowHandles::In_ImageInput, Node->Data.Value.GetString());
		break;
	}

	case EStoryFlowNodeType::GetAudio:
	case EStoryFlowNodeType::SetAudio:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::GetCharacter:
	case EStoryFlowNodeType::SetCharacter:
	{
		FStoryFlowVariable* Var = Context->FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		Result = Var ? Var->Value.GetString() : TEXT("");
		break;
	}

	case EStoryFlowNodeType::GetStringArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, StoryFlowHandles::In_StringArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomStringArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateStringArrayInput(Node, StoryFlowHandles::In_StringArray);
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	// Map op branches on the node's Data.ValueType (K/V in node data — see the
	// boolean evaluator's map arms for the pattern note). All string-family value
	// types (string/enum/image/character/audio) read through this evaluator —
	// enums included, mirroring scalar enum reads (EvaluateEnumInput delegates
	// here). Stored strings are returned VERBATIM; strings-table resolution for
	// string-typed map values is Task 7.
	case EStoryFlowNodeType::GetMapValue:
	{
		const FString& MapValueType = Node->Data.ValueType;
		if (MapValueType == TEXT("string") || MapValueType == TEXT("enum") || MapValueType == TEXT("image") ||
			MapValueType == TEXT("character") || MapValueType == TEXT("audio"))
		{
			FStoryFlowVariant Value;
			ComputeGetMapValue(Node, Value);
			Result = Value.GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetImageArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, StoryFlowHandles::In_ImageArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomImageArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateImageArrayInput(Node, StoryFlowHandles::In_ImageArray);
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetCharacterArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, StoryFlowHandles::In_CharacterArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomCharacterArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateCharacterArrayInput(Node, StoryFlowHandles::In_CharacterArray);
		if (Array.Num() > 0)
		{
			Result = Array[FMath::RandRange(0, Array.Num() - 1)].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetAudioArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, StoryFlowHandles::In_AudioArray);
		int32 Index = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
		if (Index >= 0 && Index < Array.Num())
		{
			Result = Array[Index].GetString();
		}
		break;
	}

	case EStoryFlowNodeType::GetRandomAudioArrayElement:
	{
		TArray<FStoryFlowVariant> Array = EvaluateAudioArrayInput(Node, StoryFlowHandles::In_AudioArray);
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

	case EStoryFlowNodeType::RunScript:
	{
		FNodeRuntimeState& RSState = Context->GetNodeState(Node->Id);
		if (RSState.bHasOutputValues && !SourceHandle.IsEmpty())
		{
			int32 OutIdx = SourceHandle.Find(TEXT("-out-"));
			if (OutIdx != INDEX_NONE)
			{
				FString VarId = SourceHandle.Mid(OutIdx + 5);
				FString VarName;
				for (const auto& Output : Node->Data.ScriptOutputs)
				{
					if (Output.Id == VarId)
					{
						VarName = Output.Name;
						break;
					}
				}
				if (!VarName.IsEmpty())
				{
					if (const FStoryFlowVariant* Val = RSState.OutputValues.Find(VarName))
					{
						Result = Val->GetString();
					}
				}
			}
		}
		break;
	}

	case EStoryFlowNodeType::GetCharacterVar:
	case EStoryFlowNodeType::SetCharacterVar:
	{
		FString CharPath = Node->Data.CharacterPath;
		if (UStoryFlowScriptAsset* Script = Context->CurrentScript.Get())
		{
			if (const FStoryFlowConnection* CharEdge = Script->FindInputEdge(Node->Id, StoryFlowHandles::In_CharacterInput))
			{
				if (FStoryFlowNode* CharNode = Context->GetNode(CharEdge->Source))
				{
					CharPath = EvaluateStringFromNode(CharNode, Node->Id, CharEdge->SourceHandle);
				}
			}
		}
		FStoryFlowVariant CharVal = Context->GetCharacterVariableValue(CharPath, Node->Data.VariableName);
		Result = CharVal.GetString();
		break;
	}

	default:
		Result = TEXT("");
		break;
	}

	SF_EVAL_TRACE("EVAL %s %s result=%s", *Node->Id, *Node->TypeString, *Result);

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

FString FStoryFlowEvaluator::EvaluateIntToEnum(FStoryFlowNode* Node)
{
	int32 Input = EvaluateIntegerInput(Node, StoryFlowHandles::In_Integer, Node->Data.Value.GetInt(0));
	TArray<FString> EnumValues = ResolveConversionEnumValues(Node);
	if (EnumValues.Num() == 0)
	{
		return TEXT("");
	}
	int32 ClampedIndex = FMath::Clamp(Input, 0, EnumValues.Num() - 1);
	return EnumValues[ClampedIndex];
}

FString FStoryFlowEvaluator::EvaluateStringToEnum(FStoryFlowNode* Node)
{
	FString Input = EvaluateStringInput(Node, StoryFlowHandles::In_String, Node->Data.Value.GetString());
	TArray<FString> EnumValues = ResolveConversionEnumValues(Node);
	if (EnumValues.Contains(Input))
	{
		return Input;
	}
	return EnumValues.Num() > 0 ? EnumValues[0] : TEXT("");
}

TArray<FString> FStoryFlowEvaluator::ResolveConversionEnumValues(FStoryFlowNode* Node)
{
	// Values stored on the conversion node itself win when present.
	if (Node->Data.EnumValues.Num() > 0)
	{
		return Node->Data.EnumValues;
	}

	// Editor exports store no data on conversion nodes - mirror the HTML
	// runtime and resolve the values from the node the enum output feeds:
	// the target's variable for getEnum/setEnum, otherwise the target node's
	// own enumValues.
	UStoryFlowScriptAsset* Script = Context ? Context->CurrentScript.Get() : nullptr;
	if (!Script)
	{
		return TArray<FString>();
	}

	const FString EnumOutPrefix = StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Enum);
	for (const FStoryFlowConnection* Conn : Script->GetEdgesFromSource(Node->Id))
	{
		if (!Conn || !Conn->SourceHandle.StartsWith(EnumOutPrefix))
		{
			continue;
		}
		FStoryFlowNode* TargetNode = Context->GetNode(Conn->Target);
		if (!TargetNode)
		{
			continue;
		}
		if (TargetNode->Type == EStoryFlowNodeType::GetEnum || TargetNode->Type == EStoryFlowNodeType::SetEnum)
		{
			FStoryFlowVariable* Variable = Context->FindVariable(TargetNode->Data.Variable, TargetNode->Data.bIsGlobal);
			if (Variable && Variable->EnumValues.Num() > 0)
			{
				return Variable->EnumValues;
			}
		}
		if (TargetNode->Data.EnumValues.Num() > 0)
		{
			return TargetNode->Data.EnumValues;
		}
	}

	return TArray<FString>();
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
	if (!SourceNode)
	{
		return TArray<FStoryFlowVariant>();
	}

	// Forward-compat: warn (once) and fall through to empty-array default if
	// the source node is a type the plugin does not understand.
	MaybeWarnUnknownNode(SourceNode);

	// Handle getCharacterVar/setCharacterVar nodes that can return arrays
	if (SourceNode->Type == EStoryFlowNodeType::GetCharacterVar || SourceNode->Type == EStoryFlowNodeType::SetCharacterVar)
	{
		FString CharPath = SourceNode->Data.CharacterPath;
		if (UStoryFlowScriptAsset* Script = Context->CurrentScript.Get())
		{
			if (const FStoryFlowConnection* CharEdge = Script->FindInputEdge(SourceNode->Id, StoryFlowHandles::In_CharacterInput))
			{
				if (FStoryFlowNode* CharNode = Context->GetNode(CharEdge->Source))
				{
					CharPath = EvaluateStringFromNode(CharNode, SourceNode->Id, CharEdge->SourceHandle);
				}
			}
		}
		FStoryFlowVariant CharVal = Context->GetCharacterVariableValue(CharPath, SourceNode->Data.VariableName);
		return CharVal.GetArray();
	}

	// mapKeys / mapValues: pure ops that project a map into an array. Recomputed
	// fresh on every pull — maps mutate in place, so a cached output would go
	// stale (the HTML runtime recomputes these inline too). Entries are already
	// typed variants (Int32 or string keys; per-ValueType values), so they copy
	// straight into the result regardless of which typed wrapper the consumer
	// used (int array for integer keys, string array for string/enum keys, etc.).
	if (SourceNode->Type == EStoryFlowNodeType::MapKeys || SourceNode->Type == EStoryFlowNodeType::MapValues)
	{
		TArray<FStoryFlowVariant> Result;
		if (const TArray<FStoryFlowMapEntry>* Map = EvaluateMapInput(SourceNode, TEXT("1")))
		{
			Result.Reserve(Map->Num());
			for (const FStoryFlowMapEntry& Entry : *Map)
			{
				Result.Add(SourceNode->Type == EStoryFlowNodeType::MapKeys ? Entry.Key : Entry.Value);
			}
		}
		return Result;
	}

	// Handle array modify nodes (add/remove/clear) that output their result array.
	// These nodes store their result in CachedOutput rather than a variable field.
	if (IsArrayModifyNode(SourceNode->Type))
	{
		FNodeRuntimeState& State = Context->GetNodeState(SourceNode->Id);
		if (State.bHasCachedOutput)
		{
			return State.CachedOutput.GetArray();
		}
		return TArray<FStoryFlowVariant>();
	}

	if (SourceNode->Type != ExpectedGetArrayType && SourceNode->Type != SetArrayTwinOf(ExpectedGetArrayType))
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
// Map Evaluation
// ============================================================================

TArray<FStoryFlowMapEntry>* FStoryFlowEvaluator::EvaluateMapInput(FStoryFlowNode* Node, const FString& OptionId)
{
	FStoryFlowVariable* Var = ResolveMapInputVariable(Node, OptionId);
	return Var ? &Var->Value.GetMapMutable() : nullptr;
}

FStoryFlowVariable* FStoryFlowEvaluator::ResolveMapInputVariable(FStoryFlowNode* Node, const FString& OptionId, bool* bOutIsGlobal)
{
	if (!Context || !Node)
	{
		return nullptr;
	}

	// Map handles bake the key/value types into the handle ID:
	// "target-{nodeId}-map-{keyType}-{valueType}-{optionId}". Catalog map op
	// nodes carry K/V in NODE DATA — without them the input handle cannot be
	// built, so resolution fails to defaults (warn once per node).
	if (Node->Data.KeyType.IsEmpty() || Node->Data.ValueType.IsEmpty())
	{
		MaybeWarnMissingMapTypes(Node);
		return nullptr;
	}

	const FString HandleSuffix = StoryFlowHandles::In_Map(Node->Data.KeyType, Node->Data.ValueType, OptionId);
	const FStoryFlowConnection* Edge = Context->FindInputEdge(Node->Id, HandleSuffix);
	if (!Edge)
	{
		return nullptr;
	}

	FStoryFlowNode* SourceNode = Context->GetNode(Edge->Source);

	// Walk upstream through chained mutators (mirrors the HTML runtime's
	// evaluateMapFromNode chain arm): a mutator mutates the origin variable's
	// live storage in place, so its map output IS its own map input ("2") —
	// follow that edge until a terminal variable-bound node. Bounded to defend
	// against cyclic graphs (HTML recursion has no guard; we fail to unresolved).
	int32 Hops = 0;
	while (SourceNode &&
		(SourceNode->Type == EStoryFlowNodeType::SetMapValue ||
		 SourceNode->Type == EStoryFlowNodeType::RemoveMapKey ||
		 SourceNode->Type == EStoryFlowNodeType::ClearMap))
	{
		if (++Hops > STORYFLOW_MAX_EVALUATION_DEPTH)
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map mutator chain too deep at node %s - possible cycle"), *SourceNode->Id);
			return nullptr;
		}
		if (SourceNode->Data.KeyType.IsEmpty() || SourceNode->Data.ValueType.IsEmpty())
		{
			MaybeWarnMissingMapTypes(SourceNode);
			return nullptr;
		}
		const FString UpstreamSuffix = StoryFlowHandles::In_Map(SourceNode->Data.KeyType, SourceNode->Data.ValueType, TEXT("2"));
		const FStoryFlowConnection* UpstreamEdge = Context->FindInputEdge(SourceNode->Id, UpstreamSuffix);
		if (!UpstreamEdge)
		{
			return nullptr;
		}
		SourceNode = Context->GetNode(UpstreamEdge->Source);
	}

	if (!SourceNode)
	{
		return nullptr;
	}

	// Forward-compat: warn (once) and fall through to unresolved if the terminal
	// node is a type the plugin does not understand.
	MaybeWarnUnknownNode(SourceNode);

	switch (SourceNode->Type)
	{
	case EStoryFlowNodeType::GetMap:
	case EStoryFlowNodeType::SetMap:
	{
		// Resolve the bound variable and return it with LIVE map storage
		// established. Maps alias: mutators write through this storage and every
		// later read must observe the change, so never hand out a copy here.
		FStoryFlowVariable* Var = Context->FindVariable(SourceNode->Data.Variable, SourceNode->Data.bIsGlobal);
		if (Var && Var->Type == EStoryFlowVariableType::Map)
		{
			if (!Var->Value.IsMap())
			{
				// Variable imported without an initial value — establish the map
				// type so the returned storage is correctly typed for mutation.
				Var->Value.SetMap(TArray<FStoryFlowMapEntry>());
			}
			if (bOutIsGlobal)
			{
				*bOutIsGlobal = SourceNode->Data.bIsGlobal;
			}
			return Var;
		}
		return nullptr;
	}

	case EStoryFlowNodeType::GetCharacterVar:
	case EStoryFlowNodeType::SetCharacterVar:
		// TODO(Task 6): resolve map-typed character variables here (mirror the
		// CharacterPath + VariableName lookup the scalar evaluators use).
		return nullptr;

	case EStoryFlowNodeType::RunScript:
		// TODO(Task 8): runScript map outputs — HTML resolves _outputValues maps;
		// until then this is unresolved. NOTE: HandleSetMap's wired-but-unresolved
		// branch wipes the target to empty — T8 must also revisit that interaction.
		return nullptr;

	default:
		return nullptr;
	}
}

FStoryFlowVariant FStoryFlowEvaluator::EvaluateMapOpKeyInput(FStoryFlowNode* Node, const FString& OptionId)
{
	// Key inputs are typed by the node's keyType: "target-{nodeId}-{keyType}-{optionId}".
	// Wired input wins; inline Data.MapKey (coerced off the declared keyType at
	// import) is the fallback.
	FStoryFlowVariant Key;
	if (!Context || !Node)
	{
		return Key;
	}

	const FString HandleSuffix = FString::Printf(TEXT("%s-%s"), *Node->Data.KeyType, *OptionId);
	if (Node->Data.KeyType == TEXT("integer"))
	{
		Key.SetInt(EvaluateIntegerInput(Node, HandleSuffix, Node->Data.MapKey.GetInt(0)));
	}
	else
	{
		// string and enum keys both flow through the string evaluator
		Key.SetString(EvaluateStringInput(Node, HandleSuffix, Node->Data.MapKey.GetString()));
	}
	return Key;
}

FStoryFlowVariant FStoryFlowEvaluator::EvaluateMapOpValueInput(FStoryFlowNode* Node, const FString& OptionId)
{
	// Value inputs are typed by the node's valueType: "target-{nodeId}-{valueType}-{optionId}".
	// Wired input wins; inline Data.MapInlineValue (typed off the declared valueType
	// at import) is the fallback. Mirrors the HTML runtime's getMapOpValueInput.
	FStoryFlowVariant Value;
	if (!Context || !Node)
	{
		return Value;
	}

	const FString& ValueType = Node->Data.ValueType;
	const FString HandleSuffix = FString::Printf(TEXT("%s-%s"), *ValueType, *OptionId);
	if (ValueType == TEXT("boolean"))
	{
		Value.SetBool(EvaluateBooleanInput(Node, HandleSuffix, Node->Data.MapInlineValue.GetBool(false)));
	}
	else if (ValueType == TEXT("integer"))
	{
		Value.SetInt(EvaluateIntegerInput(Node, HandleSuffix, Node->Data.MapInlineValue.GetInt(0)));
	}
	else if (ValueType == TEXT("float"))
	{
		Value.SetFloat(EvaluateFloatInput(Node, HandleSuffix, Node->Data.MapInlineValue.GetFloat(0.0f)));
	}
	else if (ValueType == TEXT("enum"))
	{
		Value.SetEnum(EvaluateEnumInput(Node, HandleSuffix, Node->Data.MapInlineValue.GetString()));
	}
	else
	{
		// string, image, character, and audio values all flow through the string
		// evaluator (matches the scalar Set* handler precedent)
		Value.SetString(EvaluateStringInput(Node, HandleSuffix, Node->Data.MapInlineValue.GetString()));
	}
	return Value;
}

int32 FStoryFlowEvaluator::FindMapEntryByKey(const TArray<FStoryFlowMapEntry>& Entries, const FString& KeyType, const FStoryFlowVariant& Key)
{
	// Integer keys compare as Int32; string/enum keys as exact (case-sensitive) strings
	if (KeyType == TEXT("integer"))
	{
		const int32 KeyInt = Key.GetInt(0);
		return Entries.IndexOfByPredicate([KeyInt](const FStoryFlowMapEntry& Entry) { return Entry.Key.GetInt(0) == KeyInt; });
	}
	const FString KeyString = Key.GetString();
	return Entries.IndexOfByPredicate([&KeyString](const FStoryFlowMapEntry& Entry) { return Entry.Key.GetString().Equals(KeyString); });
}

bool FStoryFlowEvaluator::ComputeGetMapValue(FStoryFlowNode* Node, FStoryFlowVariant& OutValue)
{
	// Mirrors the HTML runtime's computeGetMapValue: resolve the key (input "2"
	// / inline fallback) FIRST — the map pointer may alias node-state storage
	// that any further evaluation can relocate — then resolve the map (input
	// "1") and consume it immediately. Miss/unresolved leaves OutValue
	// untouched and returns IsValid=false.
	const FStoryFlowVariant Key = EvaluateMapOpKeyInput(Node, TEXT("2"));
	const TArray<FStoryFlowMapEntry>* Map = EvaluateMapInput(Node, TEXT("1"));
	if (!Map)
	{
		return false;
	}
	const int32 EntryIndex = FindMapEntryByKey(*Map, Node->Data.KeyType, Key);
	if (EntryIndex != INDEX_NONE)
	{
		OutValue = (*Map)[EntryIndex].Value;
		return true;
	}
	return false;
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
		if (const FStoryFlowConnection* InputEdge = Context->FindInputEdge(Node->Id, StoryFlowHandles::In_Boolean))
		{
			if (FStoryFlowNode* SourceNode = Context->GetNode(InputEdge->Source))
			{
				ProcessBooleanChain(SourceNode);
			}
		}
		// Then evaluate
		bool Input = EvaluateBooleanInput(Node, StoryFlowHandles::In_Boolean, Node->Data.Value.GetBool(false));
		NodeState.CachedOutput.SetBool(!Input);
		NodeState.bHasCachedOutput = true;
		break;
	}

	case EStoryFlowNodeType::AndBool:
	case EStoryFlowNodeType::OrBool:
	case EStoryFlowNodeType::EqualBool:
	{
		// Process both inputs recursively
		if (const FStoryFlowConnection* Edge1 = Context->FindInputEdge(Node->Id, StoryFlowHandles::In_Boolean1))
		{
			if (FStoryFlowNode* Source1 = Context->GetNode(Edge1->Source))
			{
				ProcessBooleanChain(Source1);
			}
		}
		if (const FStoryFlowConnection* Edge2 = Context->FindInputEdge(Node->Id, StoryFlowHandles::In_Boolean2))
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
		if (const FStoryFlowConnection* CondEdge = Context->FindInputEdge(Node->Id, StoryFlowHandles::In_BooleanCondition))
		{
			if (FStoryFlowNode* CondNode = Context->GetNode(CondEdge->Source))
			{
				ProcessBooleanChain(CondNode);
			}
		}
		break;
	}

	case EStoryFlowNodeType::RunScript:
	{
		// RunScript nodes need the real SourceHandle to extract the output variable ID.
		// Just clear the cache here so the subsequent EvaluateBooleanFromNode (called with
		// the correct SourceHandle from EvaluateBooleanInput) gets a fresh evaluation.
		NodeState.bHasCachedOutput = false;
		NodeState.CachedOutput.Reset();
		break;
	}

	default:
	{
		// For any other type that produces a boolean (comparisons, array contains, type conversions, etc.),
		// clear cached output first to force fresh evaluation (matches HTML runtime which always overwrites cache).
		NodeState.bHasCachedOutput = false;
		NodeState.CachedOutput.Reset();
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

