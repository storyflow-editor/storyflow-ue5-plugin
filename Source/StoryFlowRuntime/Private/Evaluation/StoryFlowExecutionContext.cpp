// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Evaluation/StoryFlowExecutionContext.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowProjectAsset.h"

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
		CurrentNodeId = InScript->StartNode;
	}
}

void FStoryFlowExecutionContext::InitializeWithSubsystem(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript, TMap<FString, FStoryFlowVariable>* InGlobalVariables, TMap<FString, FStoryFlowCharacterDef>* InCharacters)
{
	Reset();

	Project = InProject;
	CurrentScript = InScript;
	ExternalGlobalVariables = InGlobalVariables;
	ExternalCharacters = InCharacters;

	if (InScript)
	{
		// Copy local variables from script
		LocalVariables = InScript->Variables;
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
	UsedOnceOnlyOptions.Empty();
	CurrentDialogueState = FStoryFlowDialogueState();
	PersistentBackgroundImage = nullptr;
	EvaluationDepth = 0;
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

FStoryFlowNode* FStoryFlowExecutionContext::GetNodeMutable(const FString& NodeId)
{
	return GetNode(NodeId);
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
	FString NormalizedPath = CharacterPath.ToLower().Replace(TEXT("/"), TEXT("\\"));

	// Use external characters if available (from subsystem - mutable runtime copies)
	if (ExternalCharacters)
	{
		return ExternalCharacters->Find(NormalizedPath);
	}

	// Fall back to project's characters (read-only)
	if (UStoryFlowProjectAsset* Proj = Project.Get())
	{
		return Proj->Characters.Find(NormalizedPath);
	}

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
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Character not found for SetCharacterVariable: %s"), *CharacterPath);
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
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *CharacterPath);
	}
}

FStoryFlowVariant FStoryFlowExecutionContext::GetCharacterVariableValue(const FString& CharacterPath, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Character not found for GetCharacterVariable: %s"), *CharacterPath);
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

	UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *CharacterPath);
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
		for (const FStoryFlowConnection& Conn : Script->Connections)
		{
			if (Conn.Target == TargetNodeId)
			{
				return &Conn;
			}
		}
	}
	return nullptr;
}

bool FStoryFlowExecutionContext::PushScript(const FString& ScriptPath, const FString& ReturnNodeId)
{
	if (IsAtMaxScriptDepth())
	{
		UE_LOG(LogTemp, Error, TEXT("StoryFlow: Max script nesting depth exceeded (%d)"), STORYFLOW_MAX_SCRIPT_DEPTH);
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
		UE_LOG(LogTemp, Error, TEXT("StoryFlow: Script not found: %s"), *ScriptPath);
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
	}

	return true;
}

FString FStoryFlowExecutionContext::GetString(const FString& Key, const FString& LanguageCode) const
{
	// Try current script first
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		FString Result = Script->GetString(Key, LanguageCode);
		if (Result != Key)
		{
			return Result;
		}
	}

	// Try project global strings
	if (UStoryFlowProjectAsset* Proj = Project.Get())
	{
		return Proj->GetGlobalString(Key, LanguageCode);
	}

	return Key;
}

FString FStoryFlowExecutionContext::InterpolateVariables(const FString& Text) const
{
	FString Result = Text;

	// Pattern: {variableName}
	// Simple replacement - find all {xxx} patterns
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
			// Handle character variable - would need current character context
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

			// Find the character-type variable by display name
			FString CharacterPath;
			bool bFoundCharVar = false;

			// Search local variables first
			for (const auto& Pair : LocalVariables)
			{
				FString VarDisplayName = GetString(Pair.Value.Name);
				if ((VarDisplayName == CharVarName || Pair.Value.Id == CharVarName) && Pair.Value.Type == EStoryFlowVariableType::Character)
				{
					CharacterPath = Pair.Value.Value.GetString();
					bFoundCharVar = true;
					break;
				}
			}

			// Search global variables if not found
			if (!bFoundCharVar)
			{
				const TMap<FString, FStoryFlowVariable>* GlobalVars = ExternalGlobalVariables;
				if (!GlobalVars && Project.IsValid())
				{
					GlobalVars = &Project->GlobalVariables;
				}

				if (GlobalVars)
				{
					for (const auto& Pair : *GlobalVars)
					{
						FString VarDisplayName = GetString(Pair.Value.Name);
						if ((VarDisplayName == CharVarName || Pair.Value.Id == CharVarName) && Pair.Value.Type == EStoryFlowVariableType::Character)
						{
							CharacterPath = Pair.Value.Value.GetString();
							bFoundCharVar = true;
							break;
						}
					}
				}
			}

			// If we found a character-type variable, look up the character and inner variable
			if (bFoundCharVar && !CharacterPath.IsEmpty())
			{
				// Normalize path for lookup
				FString NormalizedPath = CharacterPath.ToLower().Replace(TEXT("/"), TEXT("\\"));

				// Find character definition
				const FStoryFlowCharacterDef* CharDef = nullptr;
				if (ExternalCharacters)
				{
					CharDef = ExternalCharacters->Find(NormalizedPath);
				}
				else if (Project.IsValid())
				{
					CharDef = Project->Characters.Find(NormalizedPath);
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
						// Look up custom variable by display name (variables are keyed by ID, not name)
						for (const auto& VarPair : CharDef->Variables)
						{
							FString VarDisplayName = GetString(VarPair.Value.Name);
							if (VarDisplayName == InnerVarName || VarPair.Value.Id == InnerVarName)
							{
								Replacement = VarPair.Value.Value.ToString();
								break;
							}
						}
					}
				}
			}
		}
		else
		{
			// Regular variable lookup
			bool bFound = false;

			// Search local variables first
			for (const auto& Pair : LocalVariables)
			{
				// Try to match by name (from string table)
				FString VarDisplayName = GetString(Pair.Value.Name);
				if (VarDisplayName == VarName || Pair.Value.Id == VarName)
				{
					Replacement = Pair.Value.Value.ToString();
					bFound = true;
					break;
				}
			}

			// Search global variables if not found
			if (!bFound)
			{
				// Use external global variables if available
				const TMap<FString, FStoryFlowVariable>* GlobalVars = ExternalGlobalVariables;
				if (!GlobalVars && Project.IsValid())
				{
					GlobalVars = &Project->GlobalVariables;
				}

				if (GlobalVars)
				{
					for (const auto& Pair : *GlobalVars)
					{
						FString VarDisplayName = GetString(Pair.Value.Name);
						if (VarDisplayName == VarName || Pair.Value.Id == VarName)
						{
							Replacement = Pair.Value.Value.ToString();
							bFound = true;
							break;
						}
					}
				}
			}
		}

		// Replace the pattern
		FString Pattern = FString::Printf(TEXT("{%s}"), *VarName);
		Result = Result.Replace(*Pattern, *Replacement);

		StartIndex = OpenBrace + Replacement.Len();
	}

	return Result;
}

void FStoryFlowExecutionContext::ClearEvaluationCache()
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		for (auto& NodePair : Script->Nodes)
		{
			NodePair.Value.Data.bHasCachedOutput = false;
			NodePair.Value.Data.CachedOutput.Reset();
		}
	}
}
