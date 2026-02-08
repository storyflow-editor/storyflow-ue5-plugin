// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "Components/AudioComponent.h"
#include "UI/StoryFlowDialogueWidget.h"
#include "Blueprint/UserWidget.h"

UStoryFlowComponent::UStoryFlowComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

UStoryFlowComponent::~UStoryFlowComponent()
{
	// Destructor defined here where FStoryFlowEvaluator is complete type
	// Required for TUniquePtr to work with forward-declared type
}

void UStoryFlowComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoStart && !Script.IsEmpty())
	{
		StartDialogue();
	}
}

void UStoryFlowComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Always stop audio when component is destroyed (regardless of bStopAudioOnDialogueEnd setting)
	StopDialogueAudio();
	StopDialogue();
	Super::EndPlay(EndPlayReason);
}

// ============================================================================
// Control Functions
// ============================================================================

void UStoryFlowComponent::StartDialogue()
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: StartDialogue() called, Script='%s'"), *Script);

	if (Script.IsEmpty())
	{
		ReportError(TEXT("No script configured for StoryFlowComponent"));
		return;
	}

	StartDialogueWithScript(Script);
}

void UStoryFlowComponent::StartDialogueWithScript(const FString& ScriptPath)
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: StartDialogueWithScript('%s') called"), *ScriptPath);

	UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem();
	if (!Subsystem)
	{
		ReportError(TEXT("StoryFlow Subsystem not available"));
		return;
	}
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Subsystem found"));

	UStoryFlowProjectAsset* Project = Subsystem->GetProject();
	if (!Project)
	{
		ReportError(TEXT("No StoryFlow project loaded. Import a project to /Game/StoryFlow/ or set it via the subsystem."));
		return;
	}
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Project loaded: %s"), *Project->GetName());

	UStoryFlowScriptAsset* ScriptAsset = Project->GetScriptByPath(ScriptPath);
	if (!ScriptAsset)
	{
		ReportError(FString::Printf(TEXT("Script not found: %s"), *ScriptPath));
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Available scripts in project:"));
		for (const auto& ScriptPair : Project->Scripts)
		{
			UE_LOG(LogTemp, Warning, TEXT("StoryFlow:   - '%s'"), *ScriptPair.Key);
		}
		return;
	}
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Script loaded: %s (Nodes: %d, StartNode: %s)"),
		*ScriptAsset->GetName(), ScriptAsset->Nodes.Num(), *ScriptAsset->StartNode);

	// Initialize execution context with project and script
	// Pass the subsystem's global variables and runtime characters so they're shared across all components
	ExecutionContext.InitializeWithSubsystem(Project, ScriptAsset, &Subsystem->GetGlobalVariables(), &Subsystem->GetRuntimeCharacters());
	ExecutionContext.bIsExecuting = true;
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: ExecutionContext initialized, CurrentNodeId='%s'"), *ExecutionContext.CurrentNodeId);

	// Create evaluator
	Evaluator = MakeUnique<FStoryFlowEvaluator>(&ExecutionContext);
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Evaluator created"));

	// Create dialogue widget if configured
	if (DialogueWidgetClass)
	{
		// Clean up existing widget if any
		if (ActiveDialogueWidget)
		{
			ActiveDialogueWidget->RemoveFromParent();
			ActiveDialogueWidget = nullptr;
		}

		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC)
		{
			ActiveDialogueWidget = CreateWidget<UStoryFlowDialogueWidget>(PC, DialogueWidgetClass);
			if (ActiveDialogueWidget)
			{
				ActiveDialogueWidget->InitializeWithComponent(this);
				ActiveDialogueWidget->AddToViewport();
			}
		}
	}

	// Broadcast start event
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Broadcasting OnDialogueStarted"));
	OnDialogueStarted.Broadcast();
	OnScriptStarted.Broadcast(ScriptPath);

	// Find start node and begin execution
	FStoryFlowNode* StartNode = ExecutionContext.GetNode(TEXT("0"));
	if (StartNode)
	{
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Start node found, type='%s', processing..."), *StartNode->TypeString);
		ProcessNode(StartNode);
	}
	else
	{
		ReportError(TEXT("Start node (id=0) not found in script"));
		UE_LOG(LogTemp, Error, TEXT("StoryFlow: Available nodes in script:"));
		for (const auto& NodePair : ScriptAsset->Nodes)
		{
			UE_LOG(LogTemp, Error, TEXT("StoryFlow:   - id='%s' type='%s'"), *NodePair.Key, *NodePair.Value.TypeString);
		}
	}
}

void UStoryFlowComponent::SelectOption(const FString& OptionId)
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: SelectOption('%s') called"), *OptionId);
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   bIsExecuting=%s bIsWaitingForInput=%s"),
		ExecutionContext.bIsExecuting ? TEXT("true") : TEXT("false"),
		ExecutionContext.bIsWaitingForInput ? TEXT("true") : TEXT("false"));

	if (!ExecutionContext.bIsExecuting || !ExecutionContext.bIsWaitingForInput)
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: SelectOption ignored - not in valid state"));
		return;
	}

	// Mark once-only options as used
	for (const FStoryFlowDialogueOption& Option : ExecutionContext.CurrentDialogueState.Options)
	{
		if (Option.Id == OptionId)
		{
			// Check if this was a once-only option
			FStoryFlowNode* CurrentNode = ExecutionContext.GetNode(ExecutionContext.CurrentDialogueState.NodeId);
			if (CurrentNode)
			{
				for (const FStoryFlowChoice& Choice : CurrentNode->Data.Options)
				{
					if (Choice.Id == OptionId && Choice.bOnceOnly)
					{
						const FString OptionKey = ExecutionContext.CurrentDialogueState.NodeId + TEXT("-") + OptionId;
						ExecutionContext.UsedOnceOnlyOptions.Add(OptionKey);
						break;
					}
				}
			}
			break;
		}
	}

	// Save current dialogue node ID for potential re-render
	const FString DialogueNodeId = ExecutionContext.CurrentDialogueState.NodeId;

	// Clear waiting state
	ExecutionContext.bIsWaitingForInput = false;

	// Clear evaluation cache for fresh evaluation
	if (Evaluator)
	{
		Evaluator->ClearCache();
	}

	// Continue from the selected option
	FString SourceHandle = FString::Printf(TEXT("source-%s-%s"), *ExecutionContext.CurrentDialogueState.NodeId, *OptionId);
	ProcessNextNode(SourceHandle);

	// If no edge was found (dead end) and we're still executing but not waiting for input,
	// return to the current dialogue to re-render (hides once-only options, updates text, etc.)
	if (!ExecutionContext.bIsWaitingForInput && ExecutionContext.bIsExecuting)
	{
		FStoryFlowNode* DialogueNode = ExecutionContext.GetNode(DialogueNodeId);
		if (DialogueNode && DialogueNode->Type == EStoryFlowNodeType::Dialogue)
		{
			ExecutionContext.CurrentDialogueState = BuildDialogueState(DialogueNode);
			ExecutionContext.bIsWaitingForInput = true;
			OnDialogueUpdated.Broadcast(ExecutionContext.CurrentDialogueState);
		}
	}
}

void UStoryFlowComponent::StopDialogue()
{
	if (!ExecutionContext.bIsExecuting)
	{
		return;
	}

	// Stop audio if configured
	if (bStopAudioOnDialogueEnd)
	{
		StopDialogueAudio();
	}

	FString CurrentScriptPath = ExecutionContext.CurrentScript.IsValid() ? ExecutionContext.CurrentScript->ScriptPath : TEXT("");

	ExecutionContext.Reset();
	Evaluator.Reset();

	OnScriptEnded.Broadcast(CurrentScriptPath);
	OnDialogueEnded.Broadcast();

	// Destroy dialogue widget after broadcasting so it receives OnDialogueEnded
	if (ActiveDialogueWidget)
	{
		ActiveDialogueWidget->RemoveFromParent();
		ActiveDialogueWidget = nullptr;
	}
}

void UStoryFlowComponent::PauseDialogue()
{
	ExecutionContext.bIsPaused = true;
}

void UStoryFlowComponent::ResumeDialogue()
{
	if (!ExecutionContext.bIsPaused)
	{
		return;
	}

	ExecutionContext.bIsPaused = false;

	// If we're waiting for input, broadcast the current state again
	if (ExecutionContext.bIsWaitingForInput)
	{
		OnDialogueUpdated.Broadcast(ExecutionContext.CurrentDialogueState);
	}
}

// ============================================================================
// State Access
// ============================================================================

FStoryFlowDialogueState UStoryFlowComponent::GetCurrentDialogue() const
{
	return ExecutionContext.CurrentDialogueState;
}

bool UStoryFlowComponent::IsDialogueActive() const
{
	return ExecutionContext.bIsExecuting;
}

bool UStoryFlowComponent::IsWaitingForInput() const
{
	return ExecutionContext.bIsWaitingForInput;
}

bool UStoryFlowComponent::IsPaused() const
{
	return ExecutionContext.bIsPaused;
}

UStoryFlowSubsystem* UStoryFlowComponent::GetStoryFlowSubsystem() const
{
	if (CachedSubsystem)
	{
		return CachedSubsystem;
	}

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			CachedSubsystem = GameInstance->GetSubsystem<UStoryFlowSubsystem>();
			return CachedSubsystem;
		}
	}

	return nullptr;
}

UStoryFlowProjectAsset* UStoryFlowComponent::GetProject() const
{
	if (UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem())
	{
		return Subsystem->GetProject();
	}
	return nullptr;
}

TArray<FString> UStoryFlowComponent::GetAvailableScripts() const
{
	TArray<FString> Scripts;

	// Try to load project for editor dropdown
	UStoryFlowProjectAsset* Project = Cast<UStoryFlowProjectAsset>(
		StaticLoadObject(UStoryFlowProjectAsset::StaticClass(), nullptr, *UStoryFlowSubsystem::DefaultProjectPath)
	);

	if (Project)
	{
		Project->Scripts.GetKeys(Scripts);
	}

	return Scripts;
}

// ============================================================================
// Variable Access
// ============================================================================

bool UStoryFlowComponent::GetBoolVariable(const FString& VariableId, bool bGlobal)
{
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(VariableId, bGlobal);
	return Var ? Var->Value.GetBool() : false;
}

void UStoryFlowComponent::SetBoolVariable(const FString& VariableId, bool bValue, bool bGlobal)
{
	FStoryFlowVariant NewValue;
	NewValue.SetBool(bValue);
	ExecutionContext.SetVariable(VariableId, NewValue, bGlobal);
	NotifyVariableChanged(VariableId, NewValue, bGlobal);
}

int32 UStoryFlowComponent::GetIntVariable(const FString& VariableId, bool bGlobal)
{
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(VariableId, bGlobal);
	return Var ? Var->Value.GetInt() : 0;
}

void UStoryFlowComponent::SetIntVariable(const FString& VariableId, int32 Value, bool bGlobal)
{
	FStoryFlowVariant NewValue;
	NewValue.SetInt(Value);
	ExecutionContext.SetVariable(VariableId, NewValue, bGlobal);
	NotifyVariableChanged(VariableId, NewValue, bGlobal);
}

float UStoryFlowComponent::GetFloatVariable(const FString& VariableId, bool bGlobal)
{
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(VariableId, bGlobal);
	return Var ? Var->Value.GetFloat() : 0.0f;
}

void UStoryFlowComponent::SetFloatVariable(const FString& VariableId, float Value, bool bGlobal)
{
	FStoryFlowVariant NewValue;
	NewValue.SetFloat(Value);
	ExecutionContext.SetVariable(VariableId, NewValue, bGlobal);
	NotifyVariableChanged(VariableId, NewValue, bGlobal);
}

FString UStoryFlowComponent::GetStringVariable(const FString& VariableId, bool bGlobal)
{
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(VariableId, bGlobal);
	return Var ? Var->Value.GetString() : TEXT("");
}

void UStoryFlowComponent::SetStringVariable(const FString& VariableId, const FString& Value, bool bGlobal)
{
	FStoryFlowVariant NewValue;
	NewValue.SetString(Value);
	ExecutionContext.SetVariable(VariableId, NewValue, bGlobal);
	NotifyVariableChanged(VariableId, NewValue, bGlobal);
}

FString UStoryFlowComponent::GetEnumVariable(const FString& VariableId, bool bGlobal)
{
	return GetStringVariable(VariableId, bGlobal);
}

void UStoryFlowComponent::SetEnumVariable(const FString& VariableId, const FString& Value, bool bGlobal)
{
	FStoryFlowVariant NewValue;
	NewValue.SetEnum(Value);
	ExecutionContext.SetVariable(VariableId, NewValue, bGlobal);
	NotifyVariableChanged(VariableId, NewValue, bGlobal);
}

// ============================================================================
// Utility Functions
// ============================================================================

void UStoryFlowComponent::ResetVariables()
{
	// Reset local variables from current script
	if (UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get())
	{
		ExecutionContext.LocalVariables = CurrentScriptAsset->Variables;
	}

	// Note: Global variables would need to be reset from the original project data
	// which would require storing the original values
}

FString UStoryFlowComponent::GetLocalizedString(const FString& Key) const
{
	return ExecutionContext.GetString(Key, LanguageCode);
}

// ============================================================================
// Internal Node Processing
// ============================================================================

void UStoryFlowComponent::ProcessNode(FStoryFlowNode* Node)
{
	if (!Node)
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: ProcessNode called with nullptr"));
		return;
	}

	if (!ExecutionContext.bIsExecuting)
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: ProcessNode called but not executing"));
		return;
	}

	if (ExecutionContext.bIsPaused)
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: ProcessNode called but paused"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("StoryFlow: ProcessNode id='%s' type='%s' (%d)"),
		*Node->Id, *Node->TypeString, static_cast<int32>(Node->Type));

	ExecutionContext.CurrentNodeId = Node->Id;

	switch (Node->Type)
	{
	case EStoryFlowNodeType::Start:
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Handling Start node"));
		HandleStart(Node);
		break;

	case EStoryFlowNodeType::End:
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Handling End node"));
		HandleEnd(Node);
		break;

	case EStoryFlowNodeType::Branch:
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Handling Branch node"));
		HandleBranch(Node);
		break;

	case EStoryFlowNodeType::Dialogue:
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Handling Dialogue node"));
		HandleDialogue(Node);
		break;

	case EStoryFlowNodeType::RunScript:
		HandleRunScript(Node);
		break;

	case EStoryFlowNodeType::RunFlow:
		HandleRunFlow(Node);
		break;

	case EStoryFlowNodeType::EntryFlow:
		HandleEntryFlow(Node);
		break;

	// Boolean Variable Handlers
	case EStoryFlowNodeType::GetBool:
		HandleGetBool(Node);
		break;

	case EStoryFlowNodeType::SetBool:
		HandleSetBool(Node);
		break;

	// Integer Variable Handlers
	case EStoryFlowNodeType::GetInt:
		HandleGetInt(Node);
		break;

	case EStoryFlowNodeType::SetInt:
		HandleSetInt(Node);
		break;

	// Float Variable Handlers
	case EStoryFlowNodeType::GetFloat:
		HandleGetFloat(Node);
		break;

	case EStoryFlowNodeType::SetFloat:
		HandleSetFloat(Node);
		break;

	// String Variable Handlers
	case EStoryFlowNodeType::GetString:
		HandleGetString(Node);
		break;

	case EStoryFlowNodeType::SetString:
		HandleSetString(Node);
		break;

	// Enum Variable Handlers
	case EStoryFlowNodeType::GetEnum:
		HandleGetEnum(Node);
		break;

	case EStoryFlowNodeType::SetEnum:
		HandleSetEnum(Node);
		break;

	case EStoryFlowNodeType::SwitchOnEnum:
		HandleSwitchOnEnum(Node);
		break;

	// Logic nodes (no-op, just continue)
	case EStoryFlowNodeType::AndBool:
	case EStoryFlowNodeType::OrBool:
	case EStoryFlowNodeType::NotBool:
	case EStoryFlowNodeType::EqualBool:
	case EStoryFlowNodeType::GreaterThan:
	case EStoryFlowNodeType::GreaterThanOrEqual:
	case EStoryFlowNodeType::LessThan:
	case EStoryFlowNodeType::LessThanOrEqual:
	case EStoryFlowNodeType::EqualInt:
	case EStoryFlowNodeType::Plus:
	case EStoryFlowNodeType::Minus:
	case EStoryFlowNodeType::Multiply:
	case EStoryFlowNodeType::Divide:
	case EStoryFlowNodeType::Random:
	case EStoryFlowNodeType::GreaterThanFloat:
	case EStoryFlowNodeType::GreaterThanOrEqualFloat:
	case EStoryFlowNodeType::LessThanFloat:
	case EStoryFlowNodeType::LessThanOrEqualFloat:
	case EStoryFlowNodeType::EqualFloat:
	case EStoryFlowNodeType::PlusFloat:
	case EStoryFlowNodeType::MinusFloat:
	case EStoryFlowNodeType::MultiplyFloat:
	case EStoryFlowNodeType::DivideFloat:
	case EStoryFlowNodeType::RandomFloat:
	case EStoryFlowNodeType::ConcatenateString:
	case EStoryFlowNodeType::EqualString:
	case EStoryFlowNodeType::ContainsString:
	case EStoryFlowNodeType::ToUpperCase:
	case EStoryFlowNodeType::ToLowerCase:
	case EStoryFlowNodeType::EqualEnum:
	case EStoryFlowNodeType::IntToBoolean:
	case EStoryFlowNodeType::FloatToBoolean:
	case EStoryFlowNodeType::BooleanToInt:
	case EStoryFlowNodeType::BooleanToFloat:
	case EStoryFlowNodeType::IntToString:
	case EStoryFlowNodeType::FloatToString:
	case EStoryFlowNodeType::StringToInt:
	case EStoryFlowNodeType::StringToFloat:
	case EStoryFlowNodeType::IntToEnum:
	case EStoryFlowNodeType::StringToEnum:
	case EStoryFlowNodeType::IntToFloat:
	case EStoryFlowNodeType::FloatToInt:
	case EStoryFlowNodeType::EnumToString:
	case EStoryFlowNodeType::LengthString:
		HandleLogicNode(Node);
		break;

	// Array Set Handlers
	case EStoryFlowNodeType::SetBoolArray:
	case EStoryFlowNodeType::SetIntArray:
	case EStoryFlowNodeType::SetFloatArray:
	case EStoryFlowNodeType::SetStringArray:
	case EStoryFlowNodeType::SetImageArray:
	case EStoryFlowNodeType::SetCharacterArray:
	case EStoryFlowNodeType::SetAudioArray:
	case EStoryFlowNodeType::SetBoolArrayElement:
	case EStoryFlowNodeType::SetIntArrayElement:
	case EStoryFlowNodeType::SetFloatArrayElement:
	case EStoryFlowNodeType::SetStringArrayElement:
	case EStoryFlowNodeType::SetImageArrayElement:
	case EStoryFlowNodeType::SetCharacterArrayElement:
	case EStoryFlowNodeType::SetAudioArrayElement:
		HandleArraySet(Node);
		break;

	// Array Modify Handlers
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
		HandleArrayModify(Node);
		break;

	// Array Get Handlers (data nodes, just continue)
	case EStoryFlowNodeType::GetBoolArray:
	case EStoryFlowNodeType::GetIntArray:
	case EStoryFlowNodeType::GetFloatArray:
	case EStoryFlowNodeType::GetStringArray:
	case EStoryFlowNodeType::GetImageArray:
	case EStoryFlowNodeType::GetCharacterArray:
	case EStoryFlowNodeType::GetAudioArray:
	case EStoryFlowNodeType::GetBoolArrayElement:
	case EStoryFlowNodeType::GetIntArrayElement:
	case EStoryFlowNodeType::GetFloatArrayElement:
	case EStoryFlowNodeType::GetStringArrayElement:
	case EStoryFlowNodeType::GetImageArrayElement:
	case EStoryFlowNodeType::GetCharacterArrayElement:
	case EStoryFlowNodeType::GetAudioArrayElement:
	case EStoryFlowNodeType::GetRandomBoolArrayElement:
	case EStoryFlowNodeType::GetRandomIntArrayElement:
	case EStoryFlowNodeType::GetRandomFloatArrayElement:
	case EStoryFlowNodeType::GetRandomStringArrayElement:
	case EStoryFlowNodeType::GetRandomImageArrayElement:
	case EStoryFlowNodeType::GetRandomCharacterArrayElement:
	case EStoryFlowNodeType::GetRandomAudioArrayElement:
	case EStoryFlowNodeType::ArrayLengthBool:
	case EStoryFlowNodeType::ArrayLengthInt:
	case EStoryFlowNodeType::ArrayLengthFloat:
	case EStoryFlowNodeType::ArrayLengthString:
	case EStoryFlowNodeType::ArrayLengthImage:
	case EStoryFlowNodeType::ArrayLengthCharacter:
	case EStoryFlowNodeType::ArrayLengthAudio:
	case EStoryFlowNodeType::ArrayContainsBool:
	case EStoryFlowNodeType::ArrayContainsInt:
	case EStoryFlowNodeType::ArrayContainsFloat:
	case EStoryFlowNodeType::ArrayContainsString:
	case EStoryFlowNodeType::ArrayContainsImage:
	case EStoryFlowNodeType::ArrayContainsCharacter:
	case EStoryFlowNodeType::ArrayContainsAudio:
	case EStoryFlowNodeType::FindInBoolArray:
	case EStoryFlowNodeType::FindInIntArray:
	case EStoryFlowNodeType::FindInFloatArray:
	case EStoryFlowNodeType::FindInStringArray:
	case EStoryFlowNodeType::FindInImageArray:
	case EStoryFlowNodeType::FindInCharacterArray:
	case EStoryFlowNodeType::FindInAudioArray:
		HandleLogicNode(Node);
		break;

	// Loop Handlers
	case EStoryFlowNodeType::ForEachBoolLoop:
	case EStoryFlowNodeType::ForEachIntLoop:
	case EStoryFlowNodeType::ForEachFloatLoop:
	case EStoryFlowNodeType::ForEachStringLoop:
	case EStoryFlowNodeType::ForEachImageLoop:
	case EStoryFlowNodeType::ForEachCharacterLoop:
	case EStoryFlowNodeType::ForEachAudioLoop:
		HandleForEachLoop(Node);
		break;

	// Media Handlers
	case EStoryFlowNodeType::GetImage:
	case EStoryFlowNodeType::GetAudio:
	case EStoryFlowNodeType::GetCharacter:
		HandleLogicNode(Node);
		break;

	case EStoryFlowNodeType::SetImage:
		HandleSetImage(Node);
		break;

	case EStoryFlowNodeType::SetBackgroundImage:
		HandleSetBackgroundImage(Node);
		break;

	case EStoryFlowNodeType::SetAudio:
		HandleSetAudio(Node);
		break;

	case EStoryFlowNodeType::PlayAudio:
		HandlePlayAudio(Node);
		break;

	case EStoryFlowNodeType::SetCharacter:
		HandleSetCharacter(Node);
		break;

	// Character Variable Handlers
	case EStoryFlowNodeType::GetCharacterVar:
		HandleGetCharacterVar(Node);
		break;

	case EStoryFlowNodeType::SetCharacterVar:
		HandleSetCharacterVar(Node);
		break;

	default:
		ReportError(FString::Printf(TEXT("Unknown node type: %s"), *Node->TypeString));
		break;
	}
}

void UStoryFlowComponent::ProcessNextNode(const FString& SourceHandle)
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: ProcessNextNode looking for edge with sourceHandle='%s'"), *SourceHandle);

	const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);
	if (!Edge)
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: No edge found for sourceHandle='%s' - execution stopping"), *SourceHandle);

		// Debug: List all available connections
		if (UStoryFlowScriptAsset* DebugScript = ExecutionContext.CurrentScript.Get())
		{
			// Show what node type we're on
			FString CurrentNodeId = ExecutionContext.CurrentNodeId;
			if (FStoryFlowNode* CurrentNode = DebugScript->Nodes.Find(CurrentNodeId))
			{
				UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Current node '%s' is type '%s'"), *CurrentNodeId, *CurrentNode->TypeString);
			}

			UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Available connections in script:"));
			for (const FStoryFlowConnection& Conn : DebugScript->Connections)
			{
				UE_LOG(LogTemp, Warning, TEXT("StoryFlow:   source='%s' sourceHandle='%s' -> target='%s'"),
					*Conn.Source, *Conn.SourceHandle, *Conn.Target);
			}
		}
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Found edge: source='%s' -> target='%s'"), *Edge->Source, *Edge->Target);

	FStoryFlowNode* TargetNode = ExecutionContext.GetNode(Edge->Target);
	if (!TargetNode)
	{
		ReportError(FString::Printf(TEXT("Target node not found: %s"), *Edge->Target));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Continuing to node '%s' (type='%s')"), *TargetNode->Id, *TargetNode->TypeString);

	// Mark that we're entering via edge (fresh entry) - dialogue uses this to know whether to play audio
	if (TargetNode->Type == EStoryFlowNodeType::Dialogue)
	{
		ExecutionContext.bEnteringDialogueViaEdge = true;
	}

	ProcessNode(TargetNode);
}

// ============================================================================
// Node Handlers
// ============================================================================

void UStoryFlowComponent::HandleStart(FStoryFlowNode* Node)
{
	// Start node just continues to next - handle format is "source-{nodeId}-" (empty suffix)
	FString Handle = FString::Printf(TEXT("source-%s-"), *Node->Id);
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleStart - Continuing to next via handle '%s'"), *Handle);
	ProcessNextNode(Handle);
}

void UStoryFlowComponent::HandleEnd(FStoryFlowNode* Node)
{
	// Pop flow call stack for depth tracking (flows don't return, they just end)
	// This must happen first because flows are in-script and don't affect script stack
	if (ExecutionContext.FlowCallStack.Num() > 0)
	{
		ExecutionContext.FlowCallStack.Pop();
	}

	// Check if we're in a nested script (runScript call)
	if (ExecutionContext.CallStack.Num() > 0)
	{
		// Pop and return to the runScript node's output
		FStoryFlowCallFrame Frame = ExecutionContext.CallStack.Pop();
		OnScriptEnded.Broadcast(ExecutionContext.CurrentScript.IsValid() ? ExecutionContext.CurrentScript->ScriptPath : TEXT(""));

		// Restore state
		if (Frame.ScriptAsset.IsValid())
		{
			ExecutionContext.CurrentScript = Frame.ScriptAsset;
			ExecutionContext.LocalVariables = Frame.SavedVariables;

			// Restore flow call stack (flows are script-local)
			ExecutionContext.FlowCallStack.Reset();
			for (const FString& FlowId : Frame.SavedFlowStack)
			{
				FStoryFlowFlowFrame FlowFrame;
				FlowFrame.FlowId = FlowId;
				ExecutionContext.FlowCallStack.Push(FlowFrame);
			}

			// Continue from runScript node's output
			ProcessNextNode(FString::Printf(TEXT("source-%s-output"), *Frame.ReturnNodeId));
		}
	}
	else
	{
		// Main script complete
		StopDialogue();
	}
}

void UStoryFlowComponent::HandleBranch(FStoryFlowNode* Node)
{
	// Process boolean chain to cache results
	if (Evaluator)
	{
		Evaluator->ProcessBooleanChain(Node);
	}

	// Evaluate condition
	bool Condition = false;
	if (Evaluator)
	{
		Condition = Evaluator->EvaluateBooleanInput(Node, TEXT("boolean-condition"), Node->Data.Value.GetBool(false));
	}

	// Continue based on condition
	FString Handle = Condition
		? FString::Printf(TEXT("source-%s-true"), *Node->Id)
		: FString::Printf(TEXT("source-%s-false"), *Node->Id);

	// Check if the edge exists
	const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(Handle);
	if (Edge)
	{
		ProcessNextNode(Handle);
	}
	else
	{
		// No edge for taken branch - just stop execution (matches HTML runtime behavior)
		// Unlike Set* nodes, branches do NOT return to dialogue to re-render
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Branch '%s' took %s path but no edge exists, stopping"),
			*Node->Id, Condition ? TEXT("true") : TEXT("false"));

		// Check if inside forEach loop - if so, continue the loop
		if (ExecutionContext.LoopStack.Num() > 0)
		{
			FStoryFlowLoopContext& LoopContext = ExecutionContext.LoopStack.Last();
			if (LoopContext.Type == EStoryFlowLoopType::ForEach)
			{
				ContinueForEachLoop(LoopContext.NodeId);
				return;
			}
		}

		// Otherwise just stop - execution ends here but dialogue stays active
		// The user can continue interacting with the dialogue (typing, clicking other options)
		// bIsWaitingForInput should still be true from when the dialogue was entered
	}
}

void UStoryFlowComponent::HandleDialogue(FStoryFlowNode* Node)
{
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleDialogue - Building state for node '%s'"), *Node->Id);

	// Check if this is a fresh entry (via edge) or returning from a Set* node
	// When returning from Set*, we only update text/options but don't re-trigger audio
	const bool bIsFreshEntry = ExecutionContext.bEnteringDialogueViaEdge;
	ExecutionContext.bEnteringDialogueViaEdge = false; // Reset the flag

	// Clear evaluation cache to ensure fresh evaluation of option visibility conditions
	// This is important when returning to dialogue after a Set* node changes a variable
	if (Evaluator)
	{
		Evaluator->ClearCache();
	}

	// Build dialogue state
	ExecutionContext.CurrentDialogueState = BuildDialogueState(Node);
	ExecutionContext.bIsWaitingForInput = true;

	UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleDialogue - State built (FreshEntry=%s):"),
		bIsFreshEntry ? TEXT("true") : TEXT("false"));
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   NodeId='%s'"), *ExecutionContext.CurrentDialogueState.NodeId);
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   Title='%s'"), *ExecutionContext.CurrentDialogueState.Title);
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   Text='%s'"), *ExecutionContext.CurrentDialogueState.Text);
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   Options=%d"), ExecutionContext.CurrentDialogueState.Options.Num());
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   bIsValid=%s"), ExecutionContext.CurrentDialogueState.bIsValid ? TEXT("true") : TEXT("false"));
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   HasImage=%s"), ExecutionContext.CurrentDialogueState.Image ? TEXT("true") : TEXT("false"));
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   HasAudio=%s"), ExecutionContext.CurrentDialogueState.Audio ? TEXT("true") : TEXT("false"));
	UE_LOG(LogTemp, Log, TEXT("StoryFlow:   Character='%s'"), *ExecutionContext.CurrentDialogueState.Character.Name);

	for (int32 i = 0; i < ExecutionContext.CurrentDialogueState.Options.Num(); i++)
	{
		const FStoryFlowDialogueOption& Opt = ExecutionContext.CurrentDialogueState.Options[i];
		UE_LOG(LogTemp, Log, TEXT("StoryFlow:   Option[%d]: id='%s' text='%s'"), i, *Opt.Id, *Opt.Text);
	}

	// Handle dialogue audio only on fresh entry (not when returning from Set* node)
	if (bIsFreshEntry)
	{
		if (ExecutionContext.CurrentDialogueState.Audio)
		{
			// Stop previous audio and play new one
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: Playing dialogue audio (fresh entry, loop=%s)"),
				Node->Data.bAudioLoop ? TEXT("true") : TEXT("false"));
			PlayDialogueAudio(ExecutionContext.CurrentDialogueState.Audio, Node->Data.bAudioLoop);
		}
		else if (Node->Data.bAudioReset)
		{
			// No audio on this dialogue but audioReset is true - stop previous audio
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: Stopping audio (audioReset=true, no new audio)"));
			StopDialogueAudio();
		}
		// If no audio and audioReset=false, previous audio continues playing
	}

	// Broadcast update
	UE_LOG(LogTemp, Log, TEXT("StoryFlow: Broadcasting OnDialogueUpdated"));
	OnDialogueUpdated.Broadcast(ExecutionContext.CurrentDialogueState);
}

void UStoryFlowComponent::HandleRunScript(FStoryFlowNode* Node)
{
	if (ExecutionContext.IsAtMaxScriptDepth())
	{
		ReportError(FString::Printf(TEXT("Max script nesting depth exceeded (%d)"), STORYFLOW_MAX_SCRIPT_DEPTH));
		return;
	}

	FString ScriptPath = Node->Data.Script;
	if (ScriptPath.IsEmpty())
	{
		ReportError(TEXT("RunScript node has no script path"));
		return;
	}

	// Push current state and switch to new script
	if (ExecutionContext.PushScript(ScriptPath, Node->Id))
	{
		OnScriptStarted.Broadcast(ScriptPath);

		// Start from node 0 in new script
		FStoryFlowNode* StartNode = ExecutionContext.GetNode(TEXT("0"));
		if (StartNode)
		{
			ProcessNode(StartNode);
		}
		else
		{
			ReportError(FString::Printf(TEXT("Start node not found in script: %s"), *ScriptPath));
		}
	}
}

void UStoryFlowComponent::HandleRunFlow(FStoryFlowNode* Node)
{
	// Flow execution within same script
	// NOTE: Flows are like jumps/goto - they do NOT return to the runFlow node
	// The flow stack is only for tracking depth to prevent infinite recursion

	FString FlowId = Node->Data.FlowId;
	if (FlowId.IsEmpty())
	{
		ReportError(TEXT("RunFlow node has no flow ID"));
		return;
	}

	// Check flow depth limit (prevent infinite recursion)
	if (ExecutionContext.IsAtMaxFlowDepth())
	{
		ReportError(TEXT("Too many nested flows - possible infinite loop"));
		return;
	}

	// Find the entryFlow node with matching flowId
	UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get();
	if (!CurrentScriptAsset)
	{
		return;
	}

	// Special case: calling the main "Start" flow
	if (FlowId.Equals(TEXT("start"), ESearchCase::IgnoreCase))
	{
		// Push flow frame for depth tracking
		FStoryFlowFlowFrame FlowFrame;
		FlowFrame.FlowId = FlowId;
		ExecutionContext.FlowCallStack.Push(FlowFrame);

		// Find and process the start node
		FStoryFlowNode* StartNode = ExecutionContext.GetNode(TEXT("0"));
		if (StartNode)
		{
			ProcessNode(StartNode);
		}
		return;
	}

	// Find entryFlow node with matching flowId
	for (auto& NodePair : CurrentScriptAsset->Nodes)
	{
		if (NodePair.Value.Type == EStoryFlowNodeType::EntryFlow && NodePair.Value.Data.FlowId == FlowId)
		{
			// Push flow frame for depth tracking (flows don't return, this is just for recursion protection)
			FStoryFlowFlowFrame FlowFrame;
			FlowFrame.FlowId = FlowId;
			ExecutionContext.FlowCallStack.Push(FlowFrame);

			// Process the entry flow node
			ProcessNode(&NodePair.Value);
			return;
		}
	}

	ReportError(FString::Printf(TEXT("EntryFlow not found for flowId: %s"), *FlowId));
}

void UStoryFlowComponent::HandleEntryFlow(FStoryFlowNode* Node)
{
	// Just continue to next node
	ProcessNextNode(FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleGetBool(FStoryFlowNode* Node)
{
	// Data node - just continue
	ProcessNextNode(FString::Printf(TEXT("source-%s-boolean-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetBool(FStoryFlowNode* Node)
{
	bool NewValue = false;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false));
	}

	FStoryFlowVariant Value;
	Value.SetBool(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	NotifyVariableChanged(Node->Data.Variable, Value, Node->Data.bIsGlobal);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleGetInt(FStoryFlowNode* Node)
{
	ProcessNextNode(FString::Printf(TEXT("source-%s-integer-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetInt(FStoryFlowNode* Node)
{
	int32 NewValue = 0;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
	}

	FStoryFlowVariant Value;
	Value.SetInt(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	NotifyVariableChanged(Node->Data.Variable, Value, Node->Data.bIsGlobal);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleGetFloat(FStoryFlowNode* Node)
{
	ProcessNextNode(FString::Printf(TEXT("source-%s-float-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetFloat(FStoryFlowNode* Node)
{
	float NewValue = 0.0f;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f));
	}

	FStoryFlowVariant Value;
	Value.SetFloat(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	NotifyVariableChanged(Node->Data.Variable, Value, Node->Data.bIsGlobal);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleGetString(FStoryFlowNode* Node)
{
	ProcessNextNode(FString::Printf(TEXT("source-%s-string-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetString(FStoryFlowNode* Node)
{
	FString NewValue;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString());
	}

	FStoryFlowVariant Value;
	Value.SetString(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	NotifyVariableChanged(Node->Data.Variable, Value, Node->Data.bIsGlobal);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleGetEnum(FStoryFlowNode* Node)
{
	ProcessNextNode(FString::Printf(TEXT("source-%s-enum-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetEnum(FStoryFlowNode* Node)
{
	FString NewValue;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateEnumInput(Node, TEXT("enum"), Node->Data.Value.GetString());
	}

	FStoryFlowVariant Value;
	Value.SetEnum(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	NotifyVariableChanged(Node->Data.Variable, Value, Node->Data.bIsGlobal);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleLogicNode(FStoryFlowNode* Node)
{
	// Logic/data nodes just continue - their values are evaluated when needed
	// Find and use the first flow output
	ProcessNextNode(FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleArraySet(FStoryFlowNode* Node)
{
	// Array set operations
	// Implementation depends on specific node type
	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleArrayModify(FStoryFlowNode* Node)
{
	// Array modification operations (add, remove, clear)
	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleForEachLoop(FStoryFlowNode* Node)
{
	// Initialize loop on first entry
	if (!Node->Data.bLoopInitialized)
	{
		// Get array from input
		TArray<FStoryFlowVariant> Array;
		if (Evaluator)
		{
			switch (Node->Type)
			{
			case EStoryFlowNodeType::ForEachBoolLoop:
				Array = Evaluator->EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
				break;
			case EStoryFlowNodeType::ForEachIntLoop:
				Array = Evaluator->EvaluateIntArrayInput(Node, TEXT("integer-array"));
				break;
			case EStoryFlowNodeType::ForEachFloatLoop:
				Array = Evaluator->EvaluateFloatArrayInput(Node, TEXT("float-array"));
				break;
			case EStoryFlowNodeType::ForEachStringLoop:
				Array = Evaluator->EvaluateStringArrayInput(Node, TEXT("string-array"));
				break;
			case EStoryFlowNodeType::ForEachImageLoop:
				Array = Evaluator->EvaluateImageArrayInput(Node, TEXT("image-array"));
				break;
			case EStoryFlowNodeType::ForEachCharacterLoop:
				Array = Evaluator->EvaluateCharacterArrayInput(Node, TEXT("character-array"));
				break;
			case EStoryFlowNodeType::ForEachAudioLoop:
				Array = Evaluator->EvaluateAudioArrayInput(Node, TEXT("audio-array"));
				break;
			default:
				break;
			}
		}

		Node->Data.LoopArray = Array;
		Node->Data.LoopIndex = 0;
		Node->Data.bLoopInitialized = true;

		// Push loop context
		FStoryFlowLoopContext LoopContext;
		LoopContext.NodeId = Node->Id;
		LoopContext.Type = EStoryFlowLoopType::ForEach;
		LoopContext.CurrentIndex = 0;
		ExecutionContext.LoopStack.Push(LoopContext);
	}

	if (Node->Data.LoopIndex < Node->Data.LoopArray.Num())
	{
		// Set current element as cached output
		Node->Data.CachedOutput = Node->Data.LoopArray[Node->Data.LoopIndex];
		Node->Data.bHasCachedOutput = true;

		// Execute loop body
		ProcessNextNode(FString::Printf(TEXT("source-%s-loopBody"), *Node->Id));
	}
	else
	{
		// Loop complete - cleanup
		Node->Data.bLoopInitialized = false;
		Node->Data.LoopArray.Empty();

		if (ExecutionContext.LoopStack.Num() > 0)
		{
			ExecutionContext.LoopStack.Pop();
		}

		// Continue after loop
		ProcessNextNode(FString::Printf(TEXT("source-%s-completed"), *Node->Id));
	}
}

void UStoryFlowComponent::HandleSetImage(FStoryFlowNode* Node)
{
	// Set image variable
	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetAudio(FStoryFlowNode* Node)
{
	// Set audio variable
	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleSetCharacter(FStoryFlowNode* Node)
{
	// Set character variable
	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

void UStoryFlowComponent::HandleSwitchOnEnum(FStoryFlowNode* Node)
{
	// Get the enum variable value
	FString EnumValue;
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
	if (Var)
	{
		EnumValue = Var->Value.GetString();
	}

	// Construct output handle matching the enum value
	FString SourceHandle = FString::Printf(TEXT("source-%s-%s"), *Node->Id, *EnumValue);
	const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);

	if (Edge)
	{
		ProcessNextNode(SourceHandle);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: switchOnEnum - No output handle found for enum value '%s'"), *EnumValue);
	}
}

void UStoryFlowComponent::HandleSetBackgroundImage(FStoryFlowNode* Node)
{
	// Evaluate image input - check connected input first, fall back to inline value
	FString ImagePath = Node->Data.Value.GetString();

	if (Evaluator)
	{
		const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, TEXT("image-image-input"));
		if (Edge)
		{
			FStoryFlowNode* SourceNode = ExecutionContext.GetNode(Edge->Source);
			if (SourceNode)
			{
				ImagePath = Evaluator->EvaluateStringFromNode(SourceNode, Node->Id, Edge->SourceHandle);
			}
		}
	}

	// Broadcast the background image change
	OnBackgroundImageChanged.Broadcast(ImagePath);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-output"), *Node->Id));
}

void UStoryFlowComponent::HandlePlayAudio(FStoryFlowNode* Node)
{
	// Evaluate audio input - check connected input first, fall back to inline value
	FString AudioPath = Node->Data.Value.GetString();

	if (Evaluator)
	{
		const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, TEXT("audio-audio-input"));
		if (Edge)
		{
			FStoryFlowNode* SourceNode = ExecutionContext.GetNode(Edge->Source);
			if (SourceNode)
			{
				AudioPath = Evaluator->EvaluateStringFromNode(SourceNode, Node->Id, Edge->SourceHandle);
			}
		}
	}

	bool bLoop = Node->Data.bAudioLoop;

	// Broadcast audio play request
	OnAudioPlayRequested.Broadcast(AudioPath, bLoop);

	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-output"), *Node->Id));
}

void UStoryFlowComponent::HandleGetCharacterVar(FStoryFlowNode* Node)
{
	// GetCharacterVar is a logic node - outputs a value based on a character's variable
	// It's evaluated lazily by the evaluator when connected to another node
	HandleLogicNode(Node);
}

void UStoryFlowComponent::HandleSetCharacterVar(FStoryFlowNode* Node)
{
	// SetCharacterVar sets a variable on a character
	// Node->Data contains: CharacterPath, VariableName, VariableType, Value (or connected input)

	FString CharacterPath = Node->Data.CharacterPath;
	FString VariableName = Node->Data.VariableName;
	FString VariableType = Node->Data.VariableType;

	UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleSetCharacterVar - CharPath='%s' VarName='%s' VarType='%s' InlineValue='%s'"),
		*CharacterPath, *VariableName, *VariableType, *Node->Data.Value.ToString());

	// First check if there's a connected character input
	FString CharacterInputHandle = FString::Printf(TEXT("target-%s-character-character-input"), *Node->Id);
	const FStoryFlowConnection* CharEdge = nullptr;
	if (UStoryFlowScriptAsset* CurrentScript = ExecutionContext.CurrentScript.Get())
	{
		for (const FStoryFlowConnection& Conn : CurrentScript->Connections)
		{
			if (Conn.TargetHandle.Contains(CharacterInputHandle))
			{
				CharEdge = &Conn;
				break;
			}
		}
	}

	if (CharEdge)
	{
		// Evaluate the connected character node to get the path (character paths are strings)
		FStoryFlowNode* CharNode = ExecutionContext.GetNode(CharEdge->Source);
		if (CharNode && Evaluator)
		{
			CharacterPath = Evaluator->EvaluateStringFromNode(CharNode, Node->Id, CharEdge->SourceHandle);
		}
	}

	if (CharacterPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: SetCharacterVar has no character path"));
		ProcessNextNode(FString::Printf(TEXT("source-%s-"), *Node->Id));
		return;
	}

	// Get the value to set - check for connected input edge
	FStoryFlowVariant NewValue;
	FString InputHandleSuffix = VariableType + TEXT("-input");
	const FStoryFlowConnection* InputEdge = ExecutionContext.FindInputEdge(Node->Id, InputHandleSuffix);

	if (InputEdge)
	{
		// Evaluate the connected node
		FStoryFlowNode* SourceNode = ExecutionContext.GetNode(InputEdge->Source);
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleSetCharacterVar - Found connected input from node '%s' (type='%s')"),
			SourceNode ? *SourceNode->Id : TEXT("null"), SourceNode ? *SourceNode->TypeString : TEXT("null"));

		if (SourceNode && Evaluator)
		{
			if (VariableType == TEXT("boolean"))
			{
				NewValue.SetBool(Evaluator->EvaluateBooleanFromNode(SourceNode, Node->Id, InputEdge->SourceHandle));
			}
			else if (VariableType == TEXT("integer"))
			{
				NewValue.SetInt(Evaluator->EvaluateIntegerFromNode(SourceNode, Node->Id, InputEdge->SourceHandle));
			}
			else if (VariableType == TEXT("float"))
			{
				NewValue.SetFloat(Evaluator->EvaluateFloatFromNode(SourceNode, Node->Id, InputEdge->SourceHandle));
			}
			else
			{
				// String, image, audio, character - all stored as string paths
				NewValue.SetString(Evaluator->EvaluateStringFromNode(SourceNode, Node->Id, InputEdge->SourceHandle));
			}
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleSetCharacterVar - Evaluated connected value: '%s'"), *NewValue.ToString());
		}
	}
	else
	{
		// Use inline value from node data
		// For string type, the value is a string table key that needs to be looked up
		if (VariableType == TEXT("string"))
		{
			FString StringKey = Node->Data.Value.GetString();
			FString ResolvedString = ExecutionContext.GetString(StringKey, LanguageCode);
			NewValue.SetString(ResolvedString);
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleSetCharacterVar - Using inline string: key='%s' resolved='%s'"), *StringKey, *ResolvedString);
		}
		else
		{
			NewValue = Node->Data.Value;
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleSetCharacterVar - Using inline value: '%s'"), *NewValue.ToString());
		}
	}

	UE_LOG(LogTemp, Log, TEXT("StoryFlow: HandleSetCharacterVar - Setting '%s' on character '%s' to '%s'"),
		*VariableName, *CharacterPath, *NewValue.ToString());

	// Set the character variable
	ExecutionContext.SetCharacterVariable(CharacterPath, VariableName, NewValue);

	// Continue execution
	HandleSetNodeEnd(Node, FString::Printf(TEXT("source-%s-"), *Node->Id));
}

// ============================================================================
// Helper Functions
// ============================================================================

FStoryFlowDialogueState UStoryFlowComponent::BuildDialogueState(FStoryFlowNode* DialogueNode)
{
	FStoryFlowDialogueState State;
	State.bIsValid = true;
	State.NodeId = DialogueNode->Id;

	// IMPORTANT: Resolve character FIRST so {Character.Name} interpolation works
	// The character must be set in CurrentDialogueState BEFORE interpolating text
	if (!DialogueNode->Data.Character.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: BuildDialogueState - Looking up character '%s'"), *DialogueNode->Data.Character);

		// Use ExecutionContext.FindCharacter to get the runtime copy (mutable)
		if (FStoryFlowCharacterDef* CharDef = ExecutionContext.FindCharacter(DialogueNode->Data.Character))
		{
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: BuildDialogueState - Found character, raw Name='%s'"), *CharDef->Name);
			State.Character.Name = ExecutionContext.GetString(CharDef->Name, LanguageCode);
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: BuildDialogueState - Resolved Name='%s'"), *State.Character.Name);

			// Load character image if specified (character assets are stored in Project->ResolvedAssets)
			if (!CharDef->Image.IsEmpty())
			{
				UStoryFlowProjectAsset* Project = ExecutionContext.Project.Get();
				if (Project)
				{
					if (TSoftObjectPtr<UObject>* CharImagePtr = Project->ResolvedAssets.Find(CharDef->Image))
					{
						State.Character.Image = Cast<UTexture2D>(CharImagePtr->LoadSynchronous());
					}
				}
			}

			// Copy character variables
			for (const auto& VarPair : CharDef->Variables)
			{
				State.Character.Variables.Add(VarPair.Key, VarPair.Value.Value);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("StoryFlow: BuildDialogueState - Character NOT FOUND: '%s'"), *DialogueNode->Data.Character);
		}
	}

	// Update CurrentDialogueState.Character BEFORE interpolation so {Character.Name} works
	ExecutionContext.CurrentDialogueState.Character = State.Character;

	// Get title and text from string table, then interpolate variables
	FString TitleKey = DialogueNode->Data.Title;
	FString TextKey = DialogueNode->Data.Text;

	State.Title = ExecutionContext.GetString(TitleKey, LanguageCode);
	State.Text = ExecutionContext.InterpolateVariables(ExecutionContext.GetString(TextKey, LanguageCode));

	// Resolve image asset with persistence logic
	if (!DialogueNode->Data.Image.IsEmpty())
	{
		// Dialogue has an image - use it and update persistent image
		if (UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get())
		{
			if (TSoftObjectPtr<UObject>* ImagePtr = CurrentScriptAsset->ResolvedAssets.Find(DialogueNode->Data.Image))
			{
				State.Image = Cast<UTexture2D>(ImagePtr->LoadSynchronous());
				ExecutionContext.PersistentBackgroundImage = State.Image;
			}
		}
	}
	else if (DialogueNode->Data.bImageReset)
	{
		// No image and imageReset=true - clear image
		State.Image = nullptr;
		ExecutionContext.PersistentBackgroundImage = nullptr;
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Image reset (imageReset=true)"));
	}
	else
	{
		// No image and imageReset=false - keep previous image
		State.Image = ExecutionContext.PersistentBackgroundImage;
		if (State.Image)
		{
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: Using persistent background image"));
		}
	}

	// Resolve audio asset
	if (!DialogueNode->Data.Audio.IsEmpty())
	{
		if (UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get())
		{
			if (TSoftObjectPtr<UObject>* AudioPtr = CurrentScriptAsset->ResolvedAssets.Find(DialogueNode->Data.Audio))
			{
				State.Audio = Cast<USoundBase>(AudioPtr->LoadSynchronous());
			}
		}
	}

	// Build text blocks (non-interactive, always visible)
	for (const FStoryFlowTextBlock& Block : DialogueNode->Data.TextBlocks)
	{
		FStoryFlowDialogueOption TextBlock;
		TextBlock.Id = Block.Id;
		TextBlock.Text = ExecutionContext.InterpolateVariables(ExecutionContext.GetString(Block.Text, LanguageCode));

		State.TextBlocks.Add(TextBlock);
	}

	// Build visible options (buttons, filtered by once-only and visibility)
	for (const FStoryFlowChoice& Choice : DialogueNode->Data.Options)
	{
		// Check once-only (use composite key NodeId-OptionId to handle copied nodes)
		const FString OnceOnlyKey = DialogueNode->Id + TEXT("-") + Choice.Id;
		if (Choice.bOnceOnly && ExecutionContext.UsedOnceOnlyOptions.Contains(OnceOnlyKey))
		{
			continue;
		}

		// Check visibility
		if (Evaluator && !Evaluator->EvaluateOptionVisibility(DialogueNode, Choice.Id))
		{
			continue;
		}

		FStoryFlowDialogueOption Option;
		Option.Id = Choice.Id;
		Option.Text = ExecutionContext.InterpolateVariables(ExecutionContext.GetString(Choice.Text, LanguageCode));

		State.Options.Add(Option);
	}

	return State;
}

void UStoryFlowComponent::NotifyVariableChanged(const FString& VariableId, const FStoryFlowVariant& Value, bool bIsGlobal)
{
	OnVariableChanged.Broadcast(VariableId, Value, bIsGlobal);

	// Live variable interpolation: If dialogue is active, re-interpolate text and update UI
	if (ExecutionContext.bIsWaitingForInput && ExecutionContext.CurrentDialogueState.bIsValid)
	{
		// Get the current dialogue node to rebuild state with fresh variable values
		FStoryFlowNode* CurrentNode = ExecutionContext.GetNode(ExecutionContext.CurrentDialogueState.NodeId);
		if (CurrentNode && CurrentNode->Type == EStoryFlowNodeType::Dialogue)
		{
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: Variable '%s' changed, re-interpolating dialogue text"), *VariableId);

			// Rebuild dialogue state with updated variable values
			ExecutionContext.CurrentDialogueState = BuildDialogueState(CurrentNode);

			// Re-broadcast so UI updates
			OnDialogueUpdated.Broadcast(ExecutionContext.CurrentDialogueState);
		}
	}
}

void UStoryFlowComponent::ReportError(const FString& ErrorMessage)
{
	UE_LOG(LogTemp, Error, TEXT("StoryFlow Error: %s"), *ErrorMessage);
	OnError.Broadcast(ErrorMessage);
}

void UStoryFlowComponent::ContinueForEachLoop(const FString& NodeId)
{
	FStoryFlowNode* LoopNode = ExecutionContext.GetNode(NodeId);
	if (!LoopNode || !LoopNode->Data.bLoopInitialized)
	{
		return;
	}

	// Increment loop index
	LoopNode->Data.LoopIndex++;

	// Pop the loop context that was pushed for this iteration
	if (ExecutionContext.LoopStack.Num() > 0)
	{
		ExecutionContext.LoopStack.Pop();
	}

	// Re-process the loop node to continue
	ProcessNode(LoopNode);
}

void UStoryFlowComponent::HandleSetNodeEnd(FStoryFlowNode* Node, const FString& SourceHandle)
{
	// Check if there's an outgoing edge
	const FStoryFlowConnection* OutEdge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);
	if (OutEdge)
	{
		ProcessNextNode(SourceHandle);
		return;
	}

	// No outgoing edge - check for special cases
	// First: If we're in a forEach loop body, continue the loop
	if (ExecutionContext.LoopStack.Num() > 0)
	{
		FStoryFlowLoopContext& LoopContext = ExecutionContext.LoopStack.Last();
		if (LoopContext.Type == EStoryFlowLoopType::ForEach)
		{
			ContinueForEachLoop(LoopContext.NodeId);
			return;
		}
	}

	// Second: If we came from a dialogue via flow edge, go back to re-render it
	// Need to find a FLOW edge (not data edge) from a dialogue
	if (UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get())
	{
		for (const FStoryFlowConnection& Conn : CurrentScriptAsset->Connections)
		{
			if (Conn.Target == Node->Id)
			{
				// Check if this is a flow edge (not a data edge)
				bool bIsDataEdge = Conn.SourceHandle.Contains(TEXT("-boolean-")) ||
								   Conn.SourceHandle.Contains(TEXT("-integer-")) ||
								   Conn.SourceHandle.Contains(TEXT("-float-")) ||
								   Conn.SourceHandle.Contains(TEXT("-string-")) ||
								   Conn.SourceHandle.Contains(TEXT("-enum-")) ||
								   Conn.SourceHandle.Contains(TEXT("-image-")) ||
								   Conn.SourceHandle.Contains(TEXT("-character-")) ||
								   Conn.SourceHandle.Contains(TEXT("-audio-"));

				if (!bIsDataEdge)
				{
					FStoryFlowNode* SourceNode = ExecutionContext.GetNode(Conn.Source);
					if (SourceNode && SourceNode->Type == EStoryFlowNodeType::Dialogue)
					{
						ProcessNode(SourceNode);
						return;
					}
				}
			}
		}
	}
}

// ============================================================================
// Audio Helpers
// ============================================================================

void UStoryFlowComponent::PlayDialogueAudio(USoundBase* Sound, bool bLoop)
{
	if (!Sound)
	{
		return;
	}

	// Stop any currently playing dialogue audio
	StopDialogueAudio();

	// Spawn a new audio component for this dialogue
	// bAutoDestroy=false so we can track and stop it later
	CurrentDialogueAudio = UGameplayStatics::SpawnSound2D(this, Sound, 1.0f, 1.0f, 0.0f, nullptr, false, false);

	if (CurrentDialogueAudio)
	{
		// Configure looping before playing
		if (bLoop)
		{
			CurrentDialogueAudio->SetSound(Sound);
			CurrentDialogueAudio->bIsUISound = true;

			// Stop the auto-started playback, configure loop, then restart
			CurrentDialogueAudio->Stop();
			CurrentDialogueAudio->Sound = Sound;

			// Create a looping sound by manually handling it
			// Note: We'll use OnAudioFinished delegate to restart if needed
			// But first, let's try setting the sound to loop via the component
		}

		// Play the audio
		CurrentDialogueAudio->Play();

		// For looping, we bind to OnAudioFinished to restart playback
		if (bLoop)
		{
			CurrentDialogueAudio->OnAudioFinished.AddDynamic(this, &UStoryFlowComponent::OnDialogueAudioFinished);
			// Store loop flag for the callback
			CurrentDialogueAudio->ComponentTags.Add(FName("StoryFlowLoop"));
		}

		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Audio started (loop=%s)"), bLoop ? TEXT("true") : TEXT("false"));
	}
}

void UStoryFlowComponent::StopDialogueAudio()
{
	if (CurrentDialogueAudio)
	{
		// Remove callback to prevent restart
		CurrentDialogueAudio->OnAudioFinished.RemoveAll(this);

		if (CurrentDialogueAudio->IsPlaying())
		{
			UE_LOG(LogTemp, Log, TEXT("StoryFlow: Stopping dialogue audio"));
			CurrentDialogueAudio->Stop();
		}

		CurrentDialogueAudio->DestroyComponent();
	}
	CurrentDialogueAudio = nullptr;
}

void UStoryFlowComponent::OnDialogueAudioFinished()
{
	// Check if this audio was marked for looping
	if (CurrentDialogueAudio && CurrentDialogueAudio->ComponentTags.Contains(FName("StoryFlowLoop")))
	{
		// Restart the audio for looping
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Looping dialogue audio"));
		CurrentDialogueAudio->Play();
	}
}
