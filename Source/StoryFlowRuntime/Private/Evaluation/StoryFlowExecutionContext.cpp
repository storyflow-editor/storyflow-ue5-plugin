// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Evaluation/StoryFlowExecutionContext.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowCharacterAsset.h"

void FStoryFlowExecutionContext::Initialize(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript)
{
	Reset();

	Project = InProject;
	CurrentScript = InScript;
	ExternalGlobalVariables = nullptr;

	if (InScript)
	{
		// Copy local variables from script
		LocalVariables = InScript->Variables;
		ResolveStringVariableValues(LocalVariables);
		CurrentNodeId = InScript->StartNode;
	}
}

void FStoryFlowExecutionContext::InitializeWithSubsystem(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript, TMap<FString, FStoryFlowVariable>* InGlobalVariables, TMap<FString, FStoryFlowCharacterDef>* InCharacters, TSet<FString>* InUsedOnceOnlyOptions)
{
	Reset();

	Project = InProject;
	CurrentScript = InScript;
	ExternalGlobalVariables = InGlobalVariables;
	ExternalCharacters = InCharacters;
	ExternalUsedOnceOnlyOptions = InUsedOnceOnlyOptions;

	if (InScript)
	{
		// Copy local variables from script
		LocalVariables = InScript->Variables;
		ResolveStringVariableValues(LocalVariables);
		CurrentNodeId = InScript->StartNode;
	}
}

void FStoryFlowExecutionContext::Reset()
{
	CurrentScript = nullptr;
	CurrentNodeId.Empty();
	bIsWaitingForInput = false;
	bIsExecuting = false;
	bIsPaused = false;
	bEnteringDialogueViaEdge = false;
	CallStack.Empty();
	FlowCallStack.Empty();
	LoopStack.Empty();
	LocalVariables.Empty();
	ExternalUsedOnceOnlyOptions = nullptr;
	CurrentDialogueState = FStoryFlowDialogueState();
	PersistentBackgroundImage = nullptr;
	EvaluationDepth = 0;
	ProcessingDepth = 0;
	NodeRuntimeStates.Empty();
	ExternalGlobalVariables = nullptr;
	ExternalCharacters = nullptr;
}

FStoryFlowNode* FStoryFlowExecutionContext::GetCurrentNode()
{
	return GetNode(CurrentNodeId);
}

FStoryFlowNode* FStoryFlowExecutionContext::GetNode(const FString& NodeId)
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		if (FStoryFlowNode* Node = Script->Nodes.Find(NodeId))
		{
			return Node;
		}
	}
	return nullptr;
}

FStoryFlowVariable* FStoryFlowExecutionContext::FindVariable(const FString& VariableId, bool bIsGlobal)
{
	if (bIsGlobal)
	{
		// Use external global variables if available (shared across all components via subsystem)
		if (ExternalGlobalVariables)
		{
			return ExternalGlobalVariables->Find(VariableId);
		}
		// Fall back to project's global variables
		if (UStoryFlowProjectAsset* Proj = Project.Get())
		{
			return Proj->GlobalVariables.Find(VariableId);
		}
		return nullptr;
	}
	return LocalVariables.Find(VariableId);
}

void FStoryFlowExecutionContext::SetVariable(const FString& VariableId, const FStoryFlowVariant& Value, bool bIsGlobal)
{
	if (FStoryFlowVariable* Variable = FindVariable(VariableId, bIsGlobal))
	{
		Variable->Value = Value;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: SetVariable failed - variable '%s' not found (bIsGlobal=%s)"), *VariableId, bIsGlobal ? TEXT("true") : TEXT("false"));
	}
}

FStoryFlowVariant FStoryFlowExecutionContext::GetVariableValue(const FString& VariableId, bool bIsGlobal)
{
	if (const FStoryFlowVariable* Variable = FindVariable(VariableId, bIsGlobal))
	{
		return Variable->Value;
	}
	return FStoryFlowVariant();
}

FStoryFlowCharacterDef* FStoryFlowExecutionContext::FindCharacter(const FString& CharacterPath)
{
	if (CharacterPath.IsEmpty())
	{
		return nullptr;
	}

	// Normalize path for lookup
	FString NormalizedPath = NormalizeCharacterPath(CharacterPath);

	// Use external characters (from subsystem - mutable runtime copies)
	if (ExternalCharacters)
	{
		return ExternalCharacters->Find(NormalizedPath);
	}

	// No external characters available - characters are now stored as UStoryFlowCharacterAsset*
	// in the project, so we can't return a mutable FStoryFlowCharacterDef* without a subsystem.
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: FindCharacter called without external characters (no subsystem). Character: %s"), *CharacterPath);
	return nullptr;
}

FStoryFlowVariable* FStoryFlowExecutionContext::FindCharacterVariable(const FString& CharacterPath, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		return nullptr;
	}

	return CharDef->Variables.Find(VariableName);
}

void FStoryFlowExecutionContext::SetCharacterVariable(const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character not found for SetCharacterVariable: %s"), *CharacterPath);
		return;
	}

	// Handle built-in "Name" field
	if (VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
	{
		CharDef->Name = Value.ToString();
		return;
	}

	// Handle built-in "Image" field
	if (VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
	{
		CharDef->Image = Value.ToString();
		return;
	}

	// Find and set custom variable
	if (FStoryFlowVariable* Variable = CharDef->Variables.Find(VariableName))
	{
		Variable->Value = Value;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *CharacterPath);
	}
}

FStoryFlowVariant FStoryFlowExecutionContext::GetCharacterVariableValue(const FString& CharacterPath, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character not found for GetCharacterVariable: %s"), *CharacterPath);
		return FStoryFlowVariant();
	}

	// Handle built-in "Name" field
	if (VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
	{
		FStoryFlowVariant Result;
		Result.SetString(CharDef->Name);
		return Result;
	}

	// Handle built-in "Image" field
	if (VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
	{
		FStoryFlowVariant Result;
		Result.SetString(CharDef->Image);
		return Result;
	}

	// Find custom variable
	if (const FStoryFlowVariable* Variable = CharDef->Variables.Find(VariableName))
	{
		return Variable->Value;
	}

	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *CharacterPath);
	return FStoryFlowVariant();
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindEdgeBySourceHandle(const FString& SourceHandle) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindEdgeBySourceHandle(SourceHandle);
	}
	return nullptr;
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindEdgeBySource(const FString& SourceNodeId) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindEdgeBySource(SourceNodeId);
	}
	return nullptr;
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindInputEdge(const FString& NodeId, const FString& HandleSuffix) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindInputEdge(NodeId, HandleSuffix);
	}
	return nullptr;
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindEdgeByTarget(const FString& TargetNodeId) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindEdgeByTarget(TargetNodeId);
	}
	return nullptr;
}

bool FStoryFlowExecutionContext::PushScript(const FString& ScriptPath, const FString& ReturnNodeId)
{
	if (IsAtMaxScriptDepth())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Max script nesting depth exceeded (%d)"), STORYFLOW_MAX_SCRIPT_DEPTH);
		return false;
	}

	UStoryFlowProjectAsset* Proj = Project.Get();
	if (!Proj)
	{
		return false;
	}

	UStoryFlowScriptAsset* NewScript = Proj->GetScriptByPath(ScriptPath);
	if (!NewScript)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Script not found: %s"), *ScriptPath);
		return false;
	}

	// Save current state
	FStoryFlowCallFrame Frame;
	Frame.ScriptPath = CurrentScript.IsValid() ? CurrentScript->ScriptPath : TEXT("");
	Frame.ReturnNodeId = ReturnNodeId;
	Frame.ScriptAsset = CurrentScript;
	Frame.SavedVariables = LocalVariables;

	// Save flow call stack (flows are in-script, so we preserve them when crossing script boundaries)
	for (const FStoryFlowFlowFrame& FlowFrame : FlowCallStack)
	{
		Frame.SavedFlowStack.Add(FlowFrame.FlowId);
	}

	CallStack.Push(Frame);

	// Clear flow stack for new script (each script has its own flow scope)
	FlowCallStack.Reset();

	// Switch to new script
	CurrentScript = NewScript;
	LocalVariables = NewScript->Variables;
	ResolveStringVariableValues(LocalVariables);
	CurrentNodeId = NewScript->StartNode;

	return true;
}

bool FStoryFlowExecutionContext::PopScript()
{
	if (CallStack.Num() == 0)
	{
		return false;
	}

	FStoryFlowCallFrame Frame = CallStack.Pop();

	// Restore state
	if (Frame.ScriptAsset.IsValid())
	{
		CurrentScript = Frame.ScriptAsset;
		LocalVariables = Frame.SavedVariables;
		CurrentNodeId = Frame.ReturnNodeId;

		// Restore flow call stack (flows are script-local)
		FlowCallStack.Reset();
		for (const FString& FlowId : Frame.SavedFlowStack)
		{
			FStoryFlowFlowFrame FlowFrame;
			FlowFrame.FlowId = FlowId;
			FlowCallStack.Push(FlowFrame);
		}
	}

	return true;
}

FString FStoryFlowExecutionContext::GetString(const FString& Key, const FString& LanguageCode) const
{
	// Try current script first (check map directly to avoid key-echo fragility)
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		const FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *Key);
		if (const FString* Value = Script->Strings.Find(FullKey))
		{
			return *Value;
		}
		if (const FString* Value = Script->Strings.Find(Key))
		{
			return *Value;
		}
	}

	// Try project global strings
	if (UStoryFlowProjectAsset* Proj = Project.Get())
	{
		const FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *Key);
		if (const FString* Value = Proj->GlobalStrings.Find(FullKey))
		{
			return *Value;
		}
		if (const FString* Value = Proj->GlobalStrings.Find(Key))
		{
			return *Value;
		}
	}

	return Key;
}

FString FStoryFlowExecutionContext::InterpolateVariables(const FString& Text) const
{
	FString Result = Text;

	// Early out if no interpolation needed
	if (!Result.Contains(TEXT("{")))
	{
		return Result;
	}

	// Build display-name -> variable lookup maps once (O(n) total instead of O(n) per token)
	// Each entry maps display name AND id to the variable pointer
	TMap<FString, const FStoryFlowVariable*> VarLookup;

	// Local variables (higher priority - added first, won't be overwritten)
	for (const auto& Pair : LocalVariables)
	{
		VarLookup.Add(Pair.Value.Name, &Pair.Value);
		VarLookup.Add(Pair.Value.Id, &Pair.Value);
	}

	// Global variables (lower priority - only added if key not already present)
	const TMap<FString, FStoryFlowVariable>* GlobalVars = ExternalGlobalVariables;
	if (!GlobalVars && Project.IsValid())
	{
		GlobalVars = &Project->GlobalVariables;
	}

	if (GlobalVars)
	{
		for (const auto& Pair : *GlobalVars)
		{
			if (!VarLookup.Contains(Pair.Value.Name))
			{
				VarLookup.Add(Pair.Value.Name, &Pair.Value);
			}
			if (!VarLookup.Contains(Pair.Value.Id))
			{
				VarLookup.Add(Pair.Value.Id, &Pair.Value);
			}
		}
	}

	// Pattern: {variableName}
	int32 StartIndex = 0;
	while (true)
	{
		int32 OpenBrace = Result.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIndex);
		if (OpenBrace == INDEX_NONE)
		{
			break;
		}

		int32 CloseBrace = Result.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenBrace);
		if (CloseBrace == INDEX_NONE)
		{
			break;
		}

		FString VarName = Result.Mid(OpenBrace + 1, CloseBrace - OpenBrace - 1);
		FString Replacement;

		// Check for Character.property pattern
		if (VarName.StartsWith(TEXT("Character.")))
		{
			FString PropertyName = VarName.RightChop(10);
			if (PropertyName.Equals(TEXT("name"), ESearchCase::IgnoreCase))
			{
				Replacement = CurrentDialogueState.Character.Name;
			}
			else if (const FStoryFlowVariant* CharVar = CurrentDialogueState.Character.Variables.Find(PropertyName))
			{
				Replacement = CharVar->ToString();
			}
		}
		else if (VarName.Contains(TEXT(".")))
		{
			// Handle nested variable (charVar.innerVar)
			// This is for character-type variables: {protagonist.Health}
			int32 DotIndex;
			VarName.FindChar(TEXT('.'), DotIndex);
			FString CharVarName = VarName.Left(DotIndex);
			FString InnerVarName = VarName.RightChop(DotIndex + 1);

			// Find the character-type variable via lookup map
			const FStoryFlowVariable* const* FoundVar = VarLookup.Find(CharVarName);
			if (FoundVar && *FoundVar && (*FoundVar)->Type == EStoryFlowVariableType::Character)
			{
				FString CharacterPath = (*FoundVar)->Value.GetString();
				if (!CharacterPath.IsEmpty())
				{
					// Normalize path for lookup
					FString NormalizedPath = NormalizeCharacterPath(CharacterPath);

					// Find character definition from runtime characters
					const FStoryFlowCharacterDef* CharDef = nullptr;
					if (ExternalCharacters)
					{
						CharDef = ExternalCharacters->Find(NormalizedPath);
					}

					if (CharDef)
					{
						// Handle built-in "Name" property (resolve through string table)
						if (InnerVarName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
						{
							Replacement = GetString(CharDef->Name);
						}
						// Handle built-in "Image" property
						else if (InnerVarName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
						{
							Replacement = CharDef->Image;
						}
						else
						{
							// Look up custom variable by name
							for (const auto& VarPair : CharDef->Variables)
							{
								if (VarPair.Value.Name == InnerVarName || VarPair.Value.Id == InnerVarName)
								{
									Replacement = VarPair.Value.Value.ToString();
									break;
								}
							}
						}
					}
				}
			}
		}
		else
		{
			// Regular variable lookup via pre-built map (O(1) instead of O(n))
			if (const FStoryFlowVariable* const* FoundVar = VarLookup.Find(VarName))
			{
				Replacement = (*FoundVar)->Value.ToString();
			}
		}

		// Replace the pattern
		FString Pattern = FString::Printf(TEXT("{%s}"), *VarName);
		Result = Result.Replace(*Pattern, *Replacement);

		StartIndex = OpenBrace + Replacement.Len();
	}

	return Result;
}

void FStoryFlowExecutionContext::ResolveStringVariableValues(TMap<FString, FStoryFlowVariable>& Variables) const
{
	for (auto& VarPair : Variables)
	{
		if (VarPair.Value.Type == EStoryFlowVariableType::String)
		{
			if (VarPair.Value.bIsArray)
			{
				for (auto& Element : VarPair.Value.Value.GetArrayMutable())
				{
					FString Key = Element.GetString();
					if (!Key.IsEmpty())
					{
						Element.SetString(GetString(Key));
					}
				}
			}
			else
			{
				FString Key = VarPair.Value.Value.GetString();
				if (!Key.IsEmpty())
				{
					VarPair.Value.Value.SetString(GetString(Key));
				}
			}
		}
	}
}

void FStoryFlowExecutionContext::ClearEvaluationCache()
{
	for (auto& Pair : NodeRuntimeStates)
	{
		Pair.Value.bHasCachedOutput = false;
		Pair.Value.CachedOutput.Reset();
	}
}
