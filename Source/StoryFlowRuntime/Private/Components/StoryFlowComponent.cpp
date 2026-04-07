// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Components/StoryFlowComponent.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundAttenuation.h"
#include "UI/StoryFlowDialogueWidget.h"
#include "Blueprint/UserWidget.h"

// ============================================================================
// Trace Logging Helpers
// ============================================================================

#define SF_TRACE(Ctx, Format, ...) \
	do { if ((Ctx).bTraceEnabled) { UE_LOG(LogStoryFlow, Log, TEXT("[SF-TRACE] " Format), ##__VA_ARGS__); } } while(0)

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
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: StartDialogue() called, Script='%s'"), *Script);

	if (Script.IsEmpty())
	{
		ReportError(TEXT("No script configured for StoryFlowComponent"));
		return;
	}

	StartDialogueWithScript(Script);
}

void UStoryFlowComponent::StartDialogueWithScript(const FString& ScriptPath)
{
	if (ScriptPath.IsEmpty())
	{
		ReportError(TEXT("StartDialogueWithScript called with empty ScriptPath"));
		return;
	}

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: StartDialogueWithScript('%s') called"), *ScriptPath);

	UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem();
	if (!Subsystem)
	{
		ReportError(TEXT("StoryFlow Subsystem not available"));
		return;
	}
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Subsystem found"));

	UStoryFlowProjectAsset* Project = Subsystem->GetProject();
	if (!Project)
	{
		ReportError(TEXT("No StoryFlow project loaded. Import a project to /Game/StoryFlow/ or set it via the subsystem."));
		return;
	}
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Project loaded: %s"), *Project->GetName());

	UStoryFlowScriptAsset* ScriptAsset = Project->GetScriptByPath(ScriptPath);
	if (!ScriptAsset)
	{
		ReportError(FString::Printf(TEXT("Script not found: %s"), *ScriptPath));
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Available scripts in project:"));
		for (const auto& ScriptPair : Project->Scripts)
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow:   - '%s'"), *ScriptPair.Key);
		}
		return;
	}
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Script loaded: %s (Nodes: %d, StartNode: %s)"),
		*ScriptAsset->GetName(), ScriptAsset->Nodes.Num(), *ScriptAsset->StartNode);

	// Initialize execution context with project and script
	// Pass the subsystem's global variables, runtime characters, and once-only options so they're shared across all components
	ExecutionContext.InitializeWithSubsystem(Project, ScriptAsset, &Subsystem->GetGlobalVariables(), &Subsystem->GetRuntimeCharacters(), &Subsystem->GetUsedOnceOnlyOptions());
	ExecutionContext.bIsExecuting = true;
	ExecutionContext.bTraceEnabled = bTraceEnabled;
	Subsystem->NotifyDialogueStarted();
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: ExecutionContext initialized, CurrentNodeId='%s'"), *ExecutionContext.CurrentNodeId);

	// Create evaluator
	Evaluator = MakeUnique<FStoryFlowEvaluator>(&ExecutionContext);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Evaluator created"));

	// Create dialogue widget if configured
	if (DialogueWidgetClass)
	{
		// Clean up existing widget if any
		if (ActiveDialogueWidget)
		{
			ActiveDialogueWidget->RemoveFromParent();
			ActiveDialogueWidget = nullptr;
		}

		UWorld* World = GetWorld();
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
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
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Broadcasting OnDialogueStarted"));
	OnDialogueStarted.Broadcast();
	OnScriptStarted.Broadcast(ScriptPath);

	// Find start node and begin execution
	FStoryFlowNode* StartNode = ExecutionContext.GetNode(TEXT("0"));
	if (StartNode)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Start node found, type='%s', processing..."), *StartNode->TypeString);
		ProcessNode(StartNode);
	}
	else
	{
		ReportError(TEXT("Start node (id=0) not found in script"));
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Available nodes in script:"));
		for (const auto& NodePair : ScriptAsset->Nodes)
		{
			UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow:   - id='%s' type='%s'"), *NodePair.Key, *NodePair.Value.TypeString);
		}
	}
}

void UStoryFlowComponent::SelectOption(const FString& OptionId)
{
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: SelectOption('%s') called"), *OptionId);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   bIsExecuting=%s bIsWaitingForInput=%s"),
		ExecutionContext.bIsExecuting ? TEXT("true") : TEXT("false"),
		ExecutionContext.bIsWaitingForInput ? TEXT("true") : TEXT("false"));

	if (!ExecutionContext.bIsExecuting || !ExecutionContext.bIsWaitingForInput)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: SelectOption ignored - not in valid state"));
		return;
	}

	// Validate that OptionId exists in the current dialogue's options
	bool bOptionFound = false;
	for (const FStoryFlowDialogueOption& Option : ExecutionContext.CurrentDialogueState.Options)
	{
		if (Option.Id == OptionId)
		{
			bOptionFound = true;
			break;
		}
	}
	if (!bOptionFound)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: SelectOption ignored - OptionId '%s' not found in current dialogue options"), *OptionId);
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
					if (Choice.Id == OptionId && Choice.bOnceOnly && ExecutionContext.ExternalUsedOnceOnlyOptions)
					{
						const FString OptionKey = ExecutionContext.CurrentDialogueState.NodeId + TEXT("-") + OptionId;
						ExecutionContext.ExternalUsedOnceOnlyOptions->Add(OptionKey);
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
	ProcessNextNode(StoryFlowHandles::Source(ExecutionContext.CurrentDialogueState.NodeId, OptionId));

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

void UStoryFlowComponent::AdvanceDialogue()
{
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: AdvanceDialogue() called"));

	if (!ExecutionContext.bIsExecuting || !ExecutionContext.bIsWaitingForInput)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: AdvanceDialogue ignored - not in valid state"));
		return;
	}

	FStoryFlowNode* CurrentNode = ExecutionContext.GetNode(ExecutionContext.CurrentDialogueState.NodeId);
	if (!CurrentNode || CurrentNode->Type != EStoryFlowNodeType::Dialogue)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: AdvanceDialogue ignored - current node is not a dialogue"));
		return;
	}

	if (CurrentNode->Data.Options.Num() > 0)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: AdvanceDialogue ignored - dialogue has %d defined options (use SelectOption instead)"), CurrentNode->Data.Options.Num());
		return;
	}

	// Audio advance-on-end: block manual advance if skip is not allowed
	if (bWaitingForAudioAdvance && !bAudioAdvanceAllowSkip)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: AdvanceDialogue blocked - waiting for audio to finish (allowSkip=false)"));
		return;
	}

	// Audio advance-on-end with skip: stop audio and proceed
	if (bWaitingForAudioAdvance && bAudioAdvanceAllowSkip)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: AdvanceDialogue - skipping audio (allowSkip=true)"));
		StopDialogueAudio();
		bWaitingForAudioAdvance = false;
		bAudioAdvanceAllowSkip = false;
	}

	const FString HeaderHandle = StoryFlowHandles::Source(ExecutionContext.CurrentDialogueState.NodeId);

	if (!ExecutionContext.FindEdgeBySourceHandle(HeaderHandle))
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: AdvanceDialogue - no outgoing edge for handle '%s' (terminal dialogue)"), *HeaderHandle);
		return;
	}

	ExecutionContext.bIsWaitingForInput = false;

	if (Evaluator)
	{
		Evaluator->ClearCache();
	}

	ProcessNextNode(HeaderHandle);
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

	if (UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem())
	{
		Subsystem->NotifyDialogueEnded();
	}
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

#if WITH_EDITOR
	// Try to load project for editor dropdown
	UStoryFlowProjectAsset* ProjectAsset = Cast<UStoryFlowProjectAsset>(
		StaticLoadObject(UStoryFlowProjectAsset::StaticClass(), nullptr, *UStoryFlowSubsystem::DefaultProjectPath)
	);

	if (ProjectAsset)
	{
		ProjectAsset->Scripts.GetKeys(Scripts);
	}
#endif

	return Scripts;
}

// ============================================================================
// Variable Access
// ============================================================================

FStoryFlowVariable* UStoryFlowComponent::FindVariableByName(const FString& VariableName, bool bGlobal)
{
	// During active dialogue, the execution context has everything wired up
	if (ExecutionContext.bIsExecuting)
	{
		return ExecutionContext.FindVariableByName(VariableName, bGlobal);
	}

	// Outside dialogue: local variables aren't available
	if (!bGlobal)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Cannot access local variable '%s' outside of active dialogue"), *VariableName);
		return nullptr;
	}

	// Outside dialogue: look up global variables directly from the subsystem
	if (UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem())
	{
		for (auto& Pair : Subsystem->GetGlobalVariables())
		{
			if (Pair.Value.Name == VariableName)
			{
				return &Pair.Value;
			}
		}
	}

	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Global variable '%s' not found"), *VariableName);
	return nullptr;
}

FStoryFlowCharacterDef* UStoryFlowComponent::FindCharacter(const FString& CharacterPath)
{
	if (CharacterPath.IsEmpty())
	{
		return nullptr;
	}

	// During active dialogue, the execution context has characters wired up
	if (ExecutionContext.bIsExecuting)
	{
		return ExecutionContext.FindCharacter(CharacterPath);
	}

	// Outside dialogue: look up directly from the subsystem
	if (UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem())
	{
		FString NormalizedPath = NormalizeCharacterPath(CharacterPath);
		return Subsystem->GetRuntimeCharacters().Find(NormalizedPath);
	}

	return nullptr;
}

bool UStoryFlowComponent::GetBoolVariable(const FString& VariableName, bool bGlobal)
{
	FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal);
	return Var ? Var->Value.GetBool() : false;
}

void UStoryFlowComponent::SetBoolVariable(const FString& VariableName, bool bValue, bool bGlobal)
{
	if (FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetBool(bValue);
		Var->Value = NewValue;
		NotifyVariableChanged(*Var, bGlobal);
	}
}

int32 UStoryFlowComponent::GetIntVariable(const FString& VariableName, bool bGlobal)
{
	FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal);
	return Var ? Var->Value.GetInt() : 0;
}

void UStoryFlowComponent::SetIntVariable(const FString& VariableName, int32 Value, bool bGlobal)
{
	if (FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetInt(Value);
		Var->Value = NewValue;
		NotifyVariableChanged(*Var, bGlobal);
	}
}

float UStoryFlowComponent::GetFloatVariable(const FString& VariableName, bool bGlobal)
{
	FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal);
	return Var ? Var->Value.GetFloat() : 0.0f;
}

void UStoryFlowComponent::SetFloatVariable(const FString& VariableName, float Value, bool bGlobal)
{
	if (FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetFloat(Value);
		Var->Value = NewValue;
		NotifyVariableChanged(*Var, bGlobal);
	}
}

FString UStoryFlowComponent::GetStringVariable(const FString& VariableName, bool bGlobal)
{
	FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal);
	return Var ? Var->Value.GetString() : TEXT("");
}

void UStoryFlowComponent::SetStringVariable(const FString& VariableName, const FString& Value, bool bGlobal)
{
	if (FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetString(Value);
		Var->Value = NewValue;
		NotifyVariableChanged(*Var, bGlobal);
	}
}

FString UStoryFlowComponent::GetEnumVariable(const FString& VariableName, bool bGlobal)
{
	return GetStringVariable(VariableName, bGlobal);
}

void UStoryFlowComponent::SetEnumVariable(const FString& VariableName, const FString& Value, bool bGlobal)
{
	if (FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetEnum(Value);
		Var->Value = NewValue;
		NotifyVariableChanged(*Var, bGlobal);
	}
}

FStoryFlowVariant UStoryFlowComponent::GetCharacterVariable(const FString& CharacterPath, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character not found for GetCharacterVariable: %s"), *CharacterPath);
		return FStoryFlowVariant();
	}

	// Handle built-in "Name" field (stored as string table key — resolve it)
	if (VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
	{
		FStoryFlowVariant Result;
		Result.SetString(ResolveString(CharDef->Name));
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

void UStoryFlowComponent::SetCharacterVariable(const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value)
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

// ============================================================================
// Character Variable Access (typed, with asset picker)
// ============================================================================

FStoryFlowCharacterDef* UStoryFlowComponent::FindCharacterFromAsset(UStoryFlowCharacterAsset* CharacterAsset)
{
	if (!CharacterAsset)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character asset is null"));
		return nullptr;
	}

	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterAsset->CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character not found at runtime: %s"), *CharacterAsset->CharacterPath);
	}
	return CharDef;
}

bool UStoryFlowComponent::GetCharacterBoolVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return false;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		return Var->Value.GetBool();
	}
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	return false;
}

void UStoryFlowComponent::SetCharacterBoolVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, bool bValue)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetBool(bValue);
		Var->Value = NewValue;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	}
}

int32 UStoryFlowComponent::GetCharacterIntVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return 0;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		return Var->Value.GetInt();
	}
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	return 0;
}

void UStoryFlowComponent::SetCharacterIntVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, int32 Value)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetInt(Value);
		Var->Value = NewValue;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	}
}

float UStoryFlowComponent::GetCharacterFloatVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return 0.0f;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		return Var->Value.GetFloat();
	}
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	return 0.0f;
}

void UStoryFlowComponent::SetCharacterFloatVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, float Value)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetFloat(Value);
		Var->Value = NewValue;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	}
}

FString UStoryFlowComponent::ResolveString(const FString& Key) const
{
	// During dialogue, execution context handles script + global string lookup
	if (ExecutionContext.bIsExecuting)
	{
		return ExecutionContext.GetString(Key, LanguageCode);
	}

	// Outside dialogue, resolve through the project's global strings
	if (UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem())
	{
		if (UStoryFlowProjectAsset* Project = Subsystem->GetProject())
		{
			return Project->GetGlobalString(Key, LanguageCode);
		}
	}

	return Key;
}

FString UStoryFlowComponent::GetCharacterStringVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return TEXT("");

	// Handle built-in "Name" field (stored as string table key)
	if (VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
	{
		return ResolveString(CharDef->Name);
	}
	// Handle built-in "Image" field
	if (VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
	{
		return CharDef->Image;
	}

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		return Var->Value.GetString();
	}
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	return TEXT("");
}

void UStoryFlowComponent::SetCharacterStringVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, const FString& Value)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return;

	// Handle built-in "Name" field
	if (VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
	{
		CharDef->Name = Value;
		return;
	}
	// Handle built-in "Image" field
	if (VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
	{
		CharDef->Image = Value;
		return;
	}

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetString(Value);
		Var->Value = NewValue;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	}
}

FString UStoryFlowComponent::GetCharacterEnumVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName)
{
	return GetCharacterStringVariable(Character, VariableName);
}

void UStoryFlowComponent::SetCharacterEnumVariable(UStoryFlowCharacterAsset* Character, const FString& VariableName, const FString& Value)
{
	FStoryFlowCharacterDef* CharDef = FindCharacterFromAsset(Character);
	if (!CharDef) return;

	if (FStoryFlowVariable* Var = CharDef->Variables.Find(VariableName))
	{
		FStoryFlowVariant NewValue;
		NewValue.SetEnum(Value);
		Var->Value = NewValue;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *Character->CharacterPath);
	}
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
		ExecutionContext.RebuildLocalNameIndex();
	}

	// Note: Global variables would need to be reset from the original project data
	// which would require storing the original values
}

FString UStoryFlowComponent::GetLocalizedString(const FString& Key) const
{
	return ResolveString(Key);
}

// ============================================================================
// Internal Node Processing
// ============================================================================

void UStoryFlowComponent::ProcessNode(FStoryFlowNode* Node)
{
	if (!Node)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: ProcessNode called with nullptr"));
		return;
	}

	if (!ExecutionContext.bIsExecuting)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: ProcessNode called but not executing"));
		return;
	}

	if (ExecutionContext.bIsPaused)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: ProcessNode called but paused"));
		return;
	}

	// Processing depth protection against cyclic graphs
	if (ExecutionContext.IsAtMaxProcessingDepth())
	{
		ReportError(FString::Printf(TEXT("Max processing depth exceeded (%d) - possible cyclic graph"), STORYFLOW_MAX_PROCESSING_DEPTH));
		StopDialogue();
		return;
	}
	++ExecutionContext.ProcessingDepth;

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: ProcessNode id='%s' type='%s' (%d)"),
		*Node->Id, *Node->TypeString, static_cast<int32>(Node->Type));

	SF_TRACE(ExecutionContext, "NODE %s %s", *Node->Id, *Node->TypeString);

	ExecutionContext.CurrentNodeId = Node->Id;

	const auto& Table = GetDispatchTable();
	if (const FNodeHandler* Handler = Table.Find(Node->Type))
	{
		(this->**Handler)(Node);
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Unhandled node type: %s (%d)"), *Node->TypeString, static_cast<int32>(Node->Type));
		ProcessNextNode(StoryFlowHandles::Source(Node->Id));
	}

	--ExecutionContext.ProcessingDepth;
}

const TMap<EStoryFlowNodeType, UStoryFlowComponent::FNodeHandler>& UStoryFlowComponent::GetDispatchTable()
{
	static const TMap<EStoryFlowNodeType, FNodeHandler> Table = []()
	{
		TMap<EStoryFlowNodeType, FNodeHandler> T;

		// Control flow
		T.Add(EStoryFlowNodeType::Start,      &UStoryFlowComponent::HandleStart);
		T.Add(EStoryFlowNodeType::End,        &UStoryFlowComponent::HandleEnd);
		T.Add(EStoryFlowNodeType::Branch,     &UStoryFlowComponent::HandleBranch);
		T.Add(EStoryFlowNodeType::Dialogue,   &UStoryFlowComponent::HandleDialogue);
		T.Add(EStoryFlowNodeType::RunScript,  &UStoryFlowComponent::HandleRunScript);
		T.Add(EStoryFlowNodeType::RunFlow,    &UStoryFlowComponent::HandleRunFlow);
		T.Add(EStoryFlowNodeType::EntryFlow,  &UStoryFlowComponent::HandleEntryFlow);

		// Variable get/set
		T.Add(EStoryFlowNodeType::GetBool,    &UStoryFlowComponent::HandleGetBool);
		T.Add(EStoryFlowNodeType::SetBool,    &UStoryFlowComponent::HandleSetBool);
		T.Add(EStoryFlowNodeType::GetInt,     &UStoryFlowComponent::HandleGetInt);
		T.Add(EStoryFlowNodeType::SetInt,     &UStoryFlowComponent::HandleSetInt);
		T.Add(EStoryFlowNodeType::GetFloat,   &UStoryFlowComponent::HandleGetFloat);
		T.Add(EStoryFlowNodeType::SetFloat,   &UStoryFlowComponent::HandleSetFloat);
		T.Add(EStoryFlowNodeType::GetString,  &UStoryFlowComponent::HandleGetString);
		T.Add(EStoryFlowNodeType::SetString,  &UStoryFlowComponent::HandleSetString);
		T.Add(EStoryFlowNodeType::GetEnum,    &UStoryFlowComponent::HandleGetEnum);
		T.Add(EStoryFlowNodeType::SetEnum,    &UStoryFlowComponent::HandleSetEnum);
		T.Add(EStoryFlowNodeType::SwitchOnEnum, &UStoryFlowComponent::HandleSwitchOnEnum);
		T.Add(EStoryFlowNodeType::RandomBranch, &UStoryFlowComponent::HandleRandomBranch);

		// Logic nodes (no-op, evaluated lazily)
		const FNodeHandler LogicHandler = &UStoryFlowComponent::HandleLogicNode;
		for (EStoryFlowNodeType Type : {
			EStoryFlowNodeType::AndBool, EStoryFlowNodeType::OrBool,
			EStoryFlowNodeType::NotBool, EStoryFlowNodeType::EqualBool,
			EStoryFlowNodeType::GreaterThan, EStoryFlowNodeType::GreaterThanOrEqual,
			EStoryFlowNodeType::LessThan, EStoryFlowNodeType::LessThanOrEqual,
			EStoryFlowNodeType::EqualInt,
			EStoryFlowNodeType::Plus, EStoryFlowNodeType::Minus,
			EStoryFlowNodeType::Multiply, EStoryFlowNodeType::Divide,
			EStoryFlowNodeType::Random,
			EStoryFlowNodeType::GreaterThanFloat, EStoryFlowNodeType::GreaterThanOrEqualFloat,
			EStoryFlowNodeType::LessThanFloat, EStoryFlowNodeType::LessThanOrEqualFloat,
			EStoryFlowNodeType::EqualFloat,
			EStoryFlowNodeType::PlusFloat, EStoryFlowNodeType::MinusFloat,
			EStoryFlowNodeType::MultiplyFloat, EStoryFlowNodeType::DivideFloat,
			EStoryFlowNodeType::RandomFloat,
			EStoryFlowNodeType::ConcatenateString, EStoryFlowNodeType::EqualString,
			EStoryFlowNodeType::ContainsString, EStoryFlowNodeType::ToUpperCase,
			EStoryFlowNodeType::ToLowerCase, EStoryFlowNodeType::EqualEnum,
			EStoryFlowNodeType::IntToBoolean, EStoryFlowNodeType::FloatToBoolean,
			EStoryFlowNodeType::BooleanToInt, EStoryFlowNodeType::BooleanToFloat,
			EStoryFlowNodeType::IntToString, EStoryFlowNodeType::FloatToString,
			EStoryFlowNodeType::StringToInt, EStoryFlowNodeType::StringToFloat,
			EStoryFlowNodeType::IntToEnum, EStoryFlowNodeType::StringToEnum,
			EStoryFlowNodeType::IntToFloat, EStoryFlowNodeType::FloatToInt,
			EStoryFlowNodeType::EnumToString, EStoryFlowNodeType::LengthString })
		{
			T.Add(Type, LogicHandler);
		}

		// Array set handlers
		const FNodeHandler ArraySetHandler = &UStoryFlowComponent::HandleArraySet;
		for (EStoryFlowNodeType Type : {
			EStoryFlowNodeType::SetBoolArray, EStoryFlowNodeType::SetIntArray,
			EStoryFlowNodeType::SetFloatArray, EStoryFlowNodeType::SetStringArray,
			EStoryFlowNodeType::SetImageArray, EStoryFlowNodeType::SetCharacterArray,
			EStoryFlowNodeType::SetAudioArray,
			EStoryFlowNodeType::SetBoolArrayElement, EStoryFlowNodeType::SetIntArrayElement,
			EStoryFlowNodeType::SetFloatArrayElement, EStoryFlowNodeType::SetStringArrayElement,
			EStoryFlowNodeType::SetImageArrayElement, EStoryFlowNodeType::SetCharacterArrayElement,
			EStoryFlowNodeType::SetAudioArrayElement })
		{
			T.Add(Type, ArraySetHandler);
		}

		// Array modify handlers
		const FNodeHandler ArrayModifyHandler = &UStoryFlowComponent::HandleArrayModify;
		for (EStoryFlowNodeType Type : {
			EStoryFlowNodeType::AddToBoolArray, EStoryFlowNodeType::AddToIntArray,
			EStoryFlowNodeType::AddToFloatArray, EStoryFlowNodeType::AddToStringArray,
			EStoryFlowNodeType::AddToImageArray, EStoryFlowNodeType::AddToCharacterArray,
			EStoryFlowNodeType::AddToAudioArray,
			EStoryFlowNodeType::RemoveFromBoolArray, EStoryFlowNodeType::RemoveFromIntArray,
			EStoryFlowNodeType::RemoveFromFloatArray, EStoryFlowNodeType::RemoveFromStringArray,
			EStoryFlowNodeType::RemoveFromImageArray, EStoryFlowNodeType::RemoveFromCharacterArray,
			EStoryFlowNodeType::RemoveFromAudioArray,
			EStoryFlowNodeType::ClearBoolArray, EStoryFlowNodeType::ClearIntArray,
			EStoryFlowNodeType::ClearFloatArray, EStoryFlowNodeType::ClearStringArray,
			EStoryFlowNodeType::ClearImageArray, EStoryFlowNodeType::ClearCharacterArray,
			EStoryFlowNodeType::ClearAudioArray })
		{
			T.Add(Type, ArrayModifyHandler);
		}

		// Array get handlers (data nodes, just continue)
		for (EStoryFlowNodeType Type : {
			EStoryFlowNodeType::GetBoolArray, EStoryFlowNodeType::GetIntArray,
			EStoryFlowNodeType::GetFloatArray, EStoryFlowNodeType::GetStringArray,
			EStoryFlowNodeType::GetImageArray, EStoryFlowNodeType::GetCharacterArray,
			EStoryFlowNodeType::GetAudioArray,
			EStoryFlowNodeType::GetBoolArrayElement, EStoryFlowNodeType::GetIntArrayElement,
			EStoryFlowNodeType::GetFloatArrayElement, EStoryFlowNodeType::GetStringArrayElement,
			EStoryFlowNodeType::GetImageArrayElement, EStoryFlowNodeType::GetCharacterArrayElement,
			EStoryFlowNodeType::GetAudioArrayElement,
			EStoryFlowNodeType::GetRandomBoolArrayElement, EStoryFlowNodeType::GetRandomIntArrayElement,
			EStoryFlowNodeType::GetRandomFloatArrayElement, EStoryFlowNodeType::GetRandomStringArrayElement,
			EStoryFlowNodeType::GetRandomImageArrayElement, EStoryFlowNodeType::GetRandomCharacterArrayElement,
			EStoryFlowNodeType::GetRandomAudioArrayElement,
			EStoryFlowNodeType::ArrayLengthBool, EStoryFlowNodeType::ArrayLengthInt,
			EStoryFlowNodeType::ArrayLengthFloat, EStoryFlowNodeType::ArrayLengthString,
			EStoryFlowNodeType::ArrayLengthImage, EStoryFlowNodeType::ArrayLengthCharacter,
			EStoryFlowNodeType::ArrayLengthAudio,
			EStoryFlowNodeType::ArrayContainsBool, EStoryFlowNodeType::ArrayContainsInt,
			EStoryFlowNodeType::ArrayContainsFloat, EStoryFlowNodeType::ArrayContainsString,
			EStoryFlowNodeType::ArrayContainsImage, EStoryFlowNodeType::ArrayContainsCharacter,
			EStoryFlowNodeType::ArrayContainsAudio,
			EStoryFlowNodeType::FindInBoolArray, EStoryFlowNodeType::FindInIntArray,
			EStoryFlowNodeType::FindInFloatArray, EStoryFlowNodeType::FindInStringArray,
			EStoryFlowNodeType::FindInImageArray, EStoryFlowNodeType::FindInCharacterArray,
			EStoryFlowNodeType::FindInAudioArray })
		{
			T.Add(Type, LogicHandler);
		}

		// Loop handlers
		const FNodeHandler ForEachHandler = &UStoryFlowComponent::HandleForEachLoop;
		for (EStoryFlowNodeType Type : {
			EStoryFlowNodeType::ForEachBoolLoop, EStoryFlowNodeType::ForEachIntLoop,
			EStoryFlowNodeType::ForEachFloatLoop, EStoryFlowNodeType::ForEachStringLoop,
			EStoryFlowNodeType::ForEachImageLoop, EStoryFlowNodeType::ForEachCharacterLoop,
			EStoryFlowNodeType::ForEachAudioLoop })
		{
			T.Add(Type, ForEachHandler);
		}

		// Media get handlers (data nodes)
		T.Add(EStoryFlowNodeType::GetImage,     LogicHandler);
		T.Add(EStoryFlowNodeType::GetAudio,     LogicHandler);
		T.Add(EStoryFlowNodeType::GetCharacter,  LogicHandler);

		// Media set handlers
		T.Add(EStoryFlowNodeType::SetImage,           &UStoryFlowComponent::HandleSetImage);
		T.Add(EStoryFlowNodeType::SetBackgroundImage,  &UStoryFlowComponent::HandleSetBackgroundImage);
		T.Add(EStoryFlowNodeType::SetAudio,            &UStoryFlowComponent::HandleSetAudio);
		T.Add(EStoryFlowNodeType::PlayAudio,           &UStoryFlowComponent::HandlePlayAudio);
		T.Add(EStoryFlowNodeType::SetCharacter,        &UStoryFlowComponent::HandleSetCharacter);

		// Character variable handlers
		T.Add(EStoryFlowNodeType::GetCharacterVar,  &UStoryFlowComponent::HandleGetCharacterVar);
		T.Add(EStoryFlowNodeType::SetCharacterVar,  &UStoryFlowComponent::HandleSetCharacterVar);

		return T;
	}();

	return Table;
}

void UStoryFlowComponent::ProcessNextNode(const FString& SourceHandle)
{
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: ProcessNextNode looking for edge with sourceHandle='%s'"), *SourceHandle);

	const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);
	if (!Edge)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: No edge found for sourceHandle='%s' - execution stopping"), *SourceHandle);

		// Debug: List all available connections
		if (UStoryFlowScriptAsset* DebugScript = ExecutionContext.CurrentScript.Get())
		{
			// Show what node type we're on
			FString CurrentNodeId = ExecutionContext.CurrentNodeId;
			if (FStoryFlowNode* CurrentNode = DebugScript->Nodes.Find(CurrentNodeId))
			{
				UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Current node '%s' is type '%s'"), *CurrentNodeId, *CurrentNode->TypeString);
			}

			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Available connections in script:"));
			for (const FStoryFlowConnection& Conn : DebugScript->Connections)
			{
				UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow:   source='%s' sourceHandle='%s' -> target='%s'"),
					*Conn.Source, *Conn.SourceHandle, *Conn.Target);
			}
		}
		return;
	}

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Found edge: source='%s' -> target='%s'"), *Edge->Source, *Edge->Target);

	SF_TRACE(ExecutionContext, "EDGE %s:%s -> %s", *Edge->Source, *Edge->SourceHandle, *Edge->Target);

	FStoryFlowNode* TargetNode = ExecutionContext.GetNode(Edge->Target);
	if (!TargetNode)
	{
		ReportError(FString::Printf(TEXT("Target node not found: %s"), *Edge->Target));
		return;
	}

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Continuing to node '%s' (type='%s')"), *TargetNode->Id, *TargetNode->TypeString);

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
	// Start node just continues to next
	FString Handle = StoryFlowHandles::Source(Node->Id);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleStart - Continuing to next via handle '%s'"), *Handle);
	ProcessNextNode(Handle);
}

void UStoryFlowComponent::HandleEnd(FStoryFlowNode* Node)
{
	FString ExitFlowId;

	// Pop flow call stack and check if it's an exit flow
	if (ExecutionContext.FlowCallStack.Num() > 0)
	{
		FStoryFlowFlowFrame FlowFrame = ExecutionContext.FlowCallStack.Pop();

		// If we're in a nested script, check if this flow is an exit route
		if (ExecutionContext.CallStack.Num() > 0 && !FlowFrame.FlowId.IsEmpty())
		{
			UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get();
			if (CurrentScriptAsset)
			{
				for (const FStoryFlowFlowDef& FlowDef : CurrentScriptAsset->Flows)
				{
					if (FlowDef.Id == FlowFrame.FlowId && FlowDef.bIsExit)
					{
						ExitFlowId = FlowFrame.FlowId;
						break;
					}
				}
			}
		}
	}

	// Clean up any active loop state for the ending script
	ExecutionContext.LoopStack.Empty();

	// Check if we're in a nested script (runScript call)
	if (ExecutionContext.CallStack.Num() > 0)
	{
		// If exit flow, check if exit handle is connected in calling script BEFORE popping
		if (!ExitFlowId.IsEmpty())
		{
			const FStoryFlowCallFrame& TopFrame = ExecutionContext.CallStack.Last();
			if (TopFrame.ScriptAsset.IsValid())
			{
				FString ExitHandle = FString::Printf(TEXT("source-%s-exit-%s"), *TopFrame.ReturnNodeId, *ExitFlowId);
				if (!TopFrame.ScriptAsset->FindEdgeBySourceHandle(ExitHandle))
				{
					// Exit handle not connected — don't exit, stay in called script
					return;
				}
			}
		}

		// Gather output variable values BEFORE popping (still in called script)
		// Key by variable Name so evaluators can match via ScriptOutputs name lookup
		TMap<FString, FStoryFlowVariant> OutputValues;
		for (const auto& VarPair : ExecutionContext.LocalVariables)
		{
			if (VarPair.Value.bIsOutput)
			{
				OutputValues.Add(VarPair.Value.Name, VarPair.Value.Value);
			}
		}

		// Pop call stack and restore calling script state
		FString ReturnScriptPath = ExecutionContext.CurrentScript.IsValid() ? ExecutionContext.CurrentScript->ScriptPath : TEXT("");
		SF_TRACE(ExecutionContext, "SCRIPT RETURN \"%s\"", *ReturnScriptPath);
		FStoryFlowCallFrame Frame = ExecutionContext.CallStack.Pop();
		OnScriptEnded.Broadcast(ReturnScriptPath);

		if (Frame.ScriptAsset.IsValid())
		{
			ExecutionContext.CurrentScript = Frame.ScriptAsset;
			ExecutionContext.LocalVariables = Frame.SavedVariables;
			ExecutionContext.RebuildLocalNameIndex();

			// Restore flow call stack (flows are script-local)
			ExecutionContext.FlowCallStack.Reset();
			for (const FString& FlowId : Frame.SavedFlowStack)
			{
				FStoryFlowFlowFrame FlowFrame;
				FlowFrame.FlowId = FlowId;
				ExecutionContext.FlowCallStack.Push(FlowFrame);
			}

			// Store output values on the RunScript node's runtime state
			if (OutputValues.Num() > 0)
			{
				FNodeRuntimeState& RSState = ExecutionContext.GetNodeState(Frame.ReturnNodeId);
				RSState.OutputValues = MoveTemp(OutputValues);
				RSState.bHasOutputValues = true;
			}

			// Route: exit handle if exit flow, otherwise default output
			FString Handle;
			if (!ExitFlowId.IsEmpty())
			{
				Handle = FString::Printf(TEXT("source-%s-exit-%s"), *Frame.ReturnNodeId, *ExitFlowId);
			}
			else
			{
				Handle = StoryFlowHandles::Source(Frame.ReturnNodeId, StoryFlowHandles::Out_Output);
			}

			const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(Handle);
			if (Edge)
			{
				ProcessNextNode(Handle);
			}
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
		Condition = Evaluator->EvaluateBooleanInput(Node, StoryFlowHandles::In_BooleanCondition, Node->Data.Value.GetBool(false));
	}

	SF_TRACE(ExecutionContext, "BRANCH %s condition=%s", *Node->Id, Condition ? TEXT("true") : TEXT("false"));

	// Continue based on condition
	FString Handle = StoryFlowHandles::Source(Node->Id, Condition ? StoryFlowHandles::Out_True : StoryFlowHandles::Out_False);

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
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Branch '%s' took %s path but no edge exists, stopping"),
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
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleDialogue - Building state for node '%s'"), *Node->Id);

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

	// Trace logging for dialogue media
	if (!Node->Data.Image.IsEmpty())
	{
		SF_TRACE(ExecutionContext, "IMAGE \"%s\"", *Node->Data.Image);
	}
	if (!Node->Data.Audio.IsEmpty())
	{
		SF_TRACE(ExecutionContext, "AUDIO \"%s\"", *Node->Data.Audio);
	}
	if (ExecutionContext.CurrentDialogueState.Character.Image)
	{
		FString CharImageName = ExecutionContext.CurrentDialogueState.Character.Image->GetPathName();
		SF_TRACE(ExecutionContext, "CHAR IMAGE \"%s\"", *CharImageName);
	}

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleDialogue - State built (FreshEntry=%s):"),
		bIsFreshEntry ? TEXT("true") : TEXT("false"));
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   NodeId='%s'"), *ExecutionContext.CurrentDialogueState.NodeId);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   Title='%s'"), *ExecutionContext.CurrentDialogueState.Title);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   Text='%s'"), *ExecutionContext.CurrentDialogueState.Text);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   Options=%d"), ExecutionContext.CurrentDialogueState.Options.Num());
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   bIsValid=%s"), ExecutionContext.CurrentDialogueState.bIsValid ? TEXT("true") : TEXT("false"));
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   HasImage=%s"), ExecutionContext.CurrentDialogueState.Image ? TEXT("true") : TEXT("false"));
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   HasAudio=%s"), ExecutionContext.CurrentDialogueState.Audio ? TEXT("true") : TEXT("false"));
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   Character='%s'"), *ExecutionContext.CurrentDialogueState.Character.Name);

	for (int32 i = 0; i < ExecutionContext.CurrentDialogueState.Options.Num(); i++)
	{
		const FStoryFlowDialogueOption& Opt = ExecutionContext.CurrentDialogueState.Options[i];
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   Option[%d]: id='%s' text='%s'"), i, *Opt.Id, *Opt.Text);
	}

	// Handle dialogue audio only on fresh entry (not when returning from Set* node)
	if (bIsFreshEntry)
	{
		if (ExecutionContext.CurrentDialogueState.Audio)
		{
			// Stop previous audio and play new one
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Playing dialogue audio (fresh entry, loop=%s)"),
				Node->Data.bAudioLoop ? TEXT("true") : TEXT("false"));
			PlayDialogueAudio(ExecutionContext.CurrentDialogueState.Audio, Node->Data.bAudioLoop);
		}
		else if (Node->Data.bAudioReset)
		{
			// No audio on this dialogue but audioReset is true - stop previous audio
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Stopping audio (audioReset=true, no new audio)"));
			StopDialogueAudio();
		}
		// If no audio and audioReset=false, previous audio continues playing

		// Set advance-on-end state (only on fresh entry — don't clear on Set* node return)
		bWaitingForAudioAdvance = Node->Data.bAudioAdvanceOnEnd && !Node->Data.bAudioLoop && ExecutionContext.CurrentDialogueState.Audio != nullptr;
		bAudioAdvanceAllowSkip = bWaitingForAudioAdvance && Node->Data.bAudioAllowSkip;

		// If audio advance was expected but audio failed to play, clear flags so bCanAdvance kicks in
		if (bWaitingForAudioAdvance && !CurrentDialogueAudio)
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Audio advance-on-end expected but audio failed to play, falling back to manual advance"));
			bWaitingForAudioAdvance = false;
			bAudioAdvanceAllowSkip = false;
		}
	}

	// Broadcast update
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Broadcasting OnDialogueUpdated"));
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

	// Evaluate parameter values BEFORE pushing (while still in calling script context)
	// NOTE: ParamValues is keyed by variable NAME (not Id), because the scriptInterface
	// stores editor UUIDs as IDs, but the exported JSON variable map uses hash-based IDs.
	// Matching by Name ensures correct lookup regardless of ID format.
	TMap<FString, FStoryFlowVariant> ParamValues;
	if (Evaluator && Node->Data.ScriptParameters.Num() > 0)
	{
		for (const FStoryFlowScriptInterfaceParam& Param : Node->Data.ScriptParameters)
		{
			if (Param.bIsArray)
			{
				// Array parameters use "{type}-array-param-{id}" handle suffix
				FString HandleSuffix = Param.Type + TEXT("-array-param-") + Param.Id;
				if (ExecutionContext.FindInputEdge(Node->Id, HandleSuffix))
				{
					TArray<FStoryFlowVariant> Arr;
					if (Param.Type == TEXT("boolean"))
						Arr = Evaluator->EvaluateBoolArrayInput(Node, HandleSuffix);
					else if (Param.Type == TEXT("integer"))
						Arr = Evaluator->EvaluateIntArrayInput(Node, HandleSuffix);
					else if (Param.Type == TEXT("float"))
						Arr = Evaluator->EvaluateFloatArrayInput(Node, HandleSuffix);
					else if (Param.Type == TEXT("string"))
						Arr = Evaluator->EvaluateStringArrayInput(Node, HandleSuffix);
					else if (Param.Type == TEXT("image"))
						Arr = Evaluator->EvaluateImageArrayInput(Node, HandleSuffix);
					else if (Param.Type == TEXT("character"))
						Arr = Evaluator->EvaluateCharacterArrayInput(Node, HandleSuffix);
					else if (Param.Type == TEXT("audio"))
						Arr = Evaluator->EvaluateAudioArrayInput(Node, HandleSuffix);

					FStoryFlowVariant ArrVariant;
					ArrVariant.SetArray(Arr);
					ParamValues.Add(Param.Name, MoveTemp(ArrVariant));
				}
			}
			else
			{
				// Scalar parameters use "{type}-param-{id}" handle suffix
				FString HandleSuffix = Param.Type + TEXT("-param-") + Param.Id;
				if (Param.Type == TEXT("boolean"))
				{
					if (ExecutionContext.FindInputEdge(Node->Id, HandleSuffix))
					{
						bool Val = Evaluator->EvaluateBooleanInput(Node, HandleSuffix, false);
						ParamValues.Add(Param.Name, FStoryFlowVariant::FromBool(Val));
					}
				}
				else if (Param.Type == TEXT("integer"))
				{
					if (ExecutionContext.FindInputEdge(Node->Id, HandleSuffix))
					{
						int32 Val = Evaluator->EvaluateIntegerInput(Node, HandleSuffix, 0);
						ParamValues.Add(Param.Name, FStoryFlowVariant::FromInt(Val));
					}
				}
				else if (Param.Type == TEXT("float"))
				{
					if (ExecutionContext.FindInputEdge(Node->Id, HandleSuffix))
					{
						float Val = Evaluator->EvaluateFloatInput(Node, HandleSuffix, 0.0f);
						ParamValues.Add(Param.Name, FStoryFlowVariant::FromFloat(Val));
					}
				}
				else // string, enum, image, character, audio - all string-valued
				{
					if (ExecutionContext.FindInputEdge(Node->Id, HandleSuffix))
					{
						FString Val = Evaluator->EvaluateStringInput(Node, HandleSuffix, TEXT(""));
						ParamValues.Add(Param.Name, FStoryFlowVariant::FromString(Val));
					}
				}
			}
		}
	}

	// Push current state and switch to new script
	if (ExecutionContext.PushScript(ScriptPath, Node->Id))
	{
		SF_TRACE(ExecutionContext, "SCRIPT CALL \"%s\"", *ScriptPath);
		OnScriptStarted.Broadcast(ScriptPath);

		// Apply parameter values to the called script's local variables
		// Match by variable Name since map keys (hash IDs) differ from scriptInterface IDs (UUIDs)
		for (const auto& ParamPair : ParamValues)
		{
			for (auto& VarPair : ExecutionContext.LocalVariables)
			{
				if (VarPair.Value.Name == ParamPair.Key)
				{
					VarPair.Value.Value = ParamPair.Value;
					break;
				}
			}
		}

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

	// Check if this is an exit flow (no entryFlow node - it's a termination signal)
	for (const FStoryFlowFlowDef& FlowDef : CurrentScriptAsset->Flows)
	{
		if (FlowDef.Id == FlowId && FlowDef.bIsExit)
		{
			SF_TRACE(ExecutionContext, "SCRIPT CALL \"%s\"", *FlowId);

			// Exit flow: push onto flowCallStack so the end handler detects it,
			// then trigger end logic directly
			FStoryFlowFlowFrame FlowFrame;
			FlowFrame.FlowId = FlowId;
			ExecutionContext.FlowCallStack.Push(FlowFrame);
			HandleEnd(Node);
			return;
		}
	}

	// Special case: calling the main "Start" flow
	if (FlowId.Equals(TEXT("start"), ESearchCase::IgnoreCase))
	{
		SF_TRACE(ExecutionContext, "SCRIPT CALL \"%s\"", *FlowId);

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
			SF_TRACE(ExecutionContext, "SCRIPT CALL \"%s\"", *FlowId);

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
	ProcessNextNode(StoryFlowHandles::Source(Node->Id));
}

void UStoryFlowComponent::HandleGetBool(FStoryFlowNode* Node)
{
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR GET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Var->Value.ToString());
	}
	// Data node - just continue
	ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Boolean));
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
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleGetInt(FStoryFlowNode* Node)
{
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR GET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Var->Value.ToString());
	}
	ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Integer));
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
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleGetFloat(FStoryFlowNode* Node)
{
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR GET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Var->Value.ToString());
	}
	ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Float));
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
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleGetString(FStoryFlowNode* Node)
{
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR GET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Var->Value.ToString());
	}
	ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_String));
}

void UStoryFlowComponent::HandleSetString(FStoryFlowNode* Node)
{
	FString NewValue;
	if (Evaluator)
	{
		FString ResolvedFallback = ExecutionContext.GetString(Node->Data.Value.GetString(), LanguageCode);
		NewValue = Evaluator->EvaluateStringInput(Node, TEXT("string"), ResolvedFallback);
	}

	FStoryFlowVariant Value;
	Value.SetString(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleGetEnum(FStoryFlowNode* Node)
{
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR GET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Var->Value.ToString());
	}
	ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Enum));
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
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleLogicNode(FStoryFlowNode* Node)
{
	// Logic/data nodes just continue - their values are evaluated when needed
	// Find and use the first flow output
	ProcessNextNode(StoryFlowHandles::Source(Node->Id));
}

void UStoryFlowComponent::HandleArraySet(FStoryFlowNode* Node)
{
	// Set array variable or set element at index
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
	if (!Var)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: HandleArraySet - variable '%s' not found"), *Node->Data.Variable);
		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
		return;
	}

	// Determine if this is a SetArray (whole array) or SetArrayElement (index)
	bool bIsSetElement = false;
	switch (Node->Type)
	{
	case EStoryFlowNodeType::SetBoolArrayElement:
	case EStoryFlowNodeType::SetIntArrayElement:
	case EStoryFlowNodeType::SetFloatArrayElement:
	case EStoryFlowNodeType::SetStringArrayElement:
	case EStoryFlowNodeType::SetImageArrayElement:
	case EStoryFlowNodeType::SetCharacterArrayElement:
	case EStoryFlowNodeType::SetAudioArrayElement:
		bIsSetElement = true;
		break;
	default:
		break;
	}

	if (bIsSetElement && Evaluator)
	{
		// Set element at index
		int32 Index = Evaluator->EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0));
		TArray<FStoryFlowVariant>& Arr = Var->Value.GetArrayMutable();
		if (Index >= 0 && Index < Arr.Num())
		{
			// Evaluate the value to set based on the element type
			switch (Node->Type)
			{
			case EStoryFlowNodeType::SetBoolArrayElement:
				Arr[Index].SetBool(Evaluator->EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false)));
				break;
			case EStoryFlowNodeType::SetIntArrayElement:
				Arr[Index].SetInt(Evaluator->EvaluateIntegerInput(Node, StoryFlowHandles::In_IntegerValue, Node->Data.Value.GetInt(0)));
				break;
			case EStoryFlowNodeType::SetFloatArrayElement:
				Arr[Index].SetFloat(Evaluator->EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f)));
				break;
			case EStoryFlowNodeType::SetStringArrayElement:
			{
				FString ResolvedFallback = ExecutionContext.GetString(Node->Data.Value.GetString(), LanguageCode);
				Arr[Index].SetString(Evaluator->EvaluateStringInput(Node, TEXT("string"), ResolvedFallback));
				break;
			}
			default:
				Arr[Index].SetString(Evaluator->EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString()));
				break;
			}
		}
	}
	else if (!bIsSetElement && Evaluator)
	{
		// Set the whole array variable from connected array input
		TArray<FStoryFlowVariant> NewArray;
		switch (Node->Type)
		{
		case EStoryFlowNodeType::SetBoolArray:
			NewArray = Evaluator->EvaluateBoolArrayInput(Node, TEXT("boolean-array"));
			break;
		case EStoryFlowNodeType::SetIntArray:
			NewArray = Evaluator->EvaluateIntArrayInput(Node, TEXT("integer-array"));
			break;
		case EStoryFlowNodeType::SetFloatArray:
			NewArray = Evaluator->EvaluateFloatArrayInput(Node, TEXT("float-array"));
			break;
		case EStoryFlowNodeType::SetStringArray:
			NewArray = Evaluator->EvaluateStringArrayInput(Node, TEXT("string-array"));
			break;
		case EStoryFlowNodeType::SetImageArray:
			NewArray = Evaluator->EvaluateImageArrayInput(Node, TEXT("image-array"));
			break;
		case EStoryFlowNodeType::SetCharacterArray:
			NewArray = Evaluator->EvaluateCharacterArrayInput(Node, TEXT("character-array"));
			break;
		case EStoryFlowNodeType::SetAudioArray:
			NewArray = Evaluator->EvaluateAudioArrayInput(Node, TEXT("audio-array"));
			break;
		default:
			break;
		}
		Var->Value.SetArray(NewArray);
	}

	SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=[array]", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"));
	NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleArrayModify(FStoryFlowNode* Node)
{
	// Determine the array handle suffix based on the node type
	FString ArrayHandleSuffix;
	switch (Node->Type)
	{
	case EStoryFlowNodeType::AddToBoolArray: case EStoryFlowNodeType::RemoveFromBoolArray: case EStoryFlowNodeType::ClearBoolArray:
		ArrayHandleSuffix = StoryFlowHandles::In_BoolArray; break;
	case EStoryFlowNodeType::AddToIntArray: case EStoryFlowNodeType::RemoveFromIntArray: case EStoryFlowNodeType::ClearIntArray:
		ArrayHandleSuffix = StoryFlowHandles::In_IntArray; break;
	case EStoryFlowNodeType::AddToFloatArray: case EStoryFlowNodeType::RemoveFromFloatArray: case EStoryFlowNodeType::ClearFloatArray:
		ArrayHandleSuffix = StoryFlowHandles::In_FloatArray; break;
	case EStoryFlowNodeType::AddToStringArray: case EStoryFlowNodeType::RemoveFromStringArray: case EStoryFlowNodeType::ClearStringArray:
		ArrayHandleSuffix = StoryFlowHandles::In_StringArray; break;
	case EStoryFlowNodeType::AddToImageArray: case EStoryFlowNodeType::RemoveFromImageArray: case EStoryFlowNodeType::ClearImageArray:
		ArrayHandleSuffix = StoryFlowHandles::In_ImageArray; break;
	case EStoryFlowNodeType::AddToCharacterArray: case EStoryFlowNodeType::RemoveFromCharacterArray: case EStoryFlowNodeType::ClearCharacterArray:
		ArrayHandleSuffix = StoryFlowHandles::In_CharacterArray; break;
	case EStoryFlowNodeType::AddToAudioArray: case EStoryFlowNodeType::RemoveFromAudioArray: case EStoryFlowNodeType::ClearAudioArray:
		ArrayHandleSuffix = StoryFlowHandles::In_AudioArray; break;
	default: break;
	}

	// Try direct variable reference first, then fall back to edge-based discovery.
	// Array modify nodes in the HTML runtime don't have a variable field — they discover
	// the target array through the input edge (matching getArrayInput + updateConnectedArrayVariable).
	FStoryFlowVariable* Var = nullptr;
	if (!Node->Data.Variable.IsEmpty())
	{
		Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
	}

	// Edge-based fallback: trace the array input edge to find the source variable
	if (!Var && Evaluator && !ArrayHandleSuffix.IsEmpty())
	{
		if (const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, ArrayHandleSuffix))
		{
			if (FStoryFlowNode* SourceNode = ExecutionContext.GetNode(Edge->Source))
			{
				if (!SourceNode->Data.Variable.IsEmpty())
				{
					Var = ExecutionContext.FindVariable(SourceNode->Data.Variable, SourceNode->Data.bIsGlobal);
				}
			}
		}
	}

	if (!Var)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: HandleArrayModify - variable not found for node '%s'"), *Node->Id);
		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
		return;
	}

	TArray<FStoryFlowVariant>& Arr = Var->Value.GetArrayMutable();

	// Determine operation type from node type name
	switch (Node->Type)
	{
	// Add operations
	case EStoryFlowNodeType::AddToBoolArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetBool(Evaluator ? Evaluator->EvaluateBooleanInput(Node, TEXT("boolean"), Node->Data.Value.GetBool(false)) : Node->Data.Value.GetBool(false));
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToIntArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetInt(Evaluator ? Evaluator->EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0)) : Node->Data.Value.GetInt(0));
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToFloatArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetFloat(Evaluator ? Evaluator->EvaluateFloatInput(Node, TEXT("float"), Node->Data.Value.GetFloat(0.0f)) : Node->Data.Value.GetFloat(0.0f));
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToStringArray:
	{
		FStoryFlowVariant Elem;
		FString ResolvedFallback = ExecutionContext.GetString(Node->Data.Value.GetString(), LanguageCode);
		FString EvalResult = Evaluator ? Evaluator->EvaluateStringInput(Node, TEXT("string"), ResolvedFallback) : ResolvedFallback;
		// If evaluator returned empty but we have a resolved default, use the default
		// (the input edge may evaluate a localization key that the string evaluator can't resolve)
		if (EvalResult.IsEmpty() && !ResolvedFallback.IsEmpty())
		{
			EvalResult = ResolvedFallback;
		}
		Elem.SetString(EvalResult);
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToImageArray:
	case EStoryFlowNodeType::AddToCharacterArray:
	case EStoryFlowNodeType::AddToAudioArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetString(Evaluator ? Evaluator->EvaluateStringInput(Node, TEXT("string"), Node->Data.Value.GetString()) : Node->Data.Value.GetString());
		Arr.Add(Elem);
		break;
	}

	// Remove operations
	case EStoryFlowNodeType::RemoveFromBoolArray:
	case EStoryFlowNodeType::RemoveFromIntArray:
	case EStoryFlowNodeType::RemoveFromFloatArray:
	case EStoryFlowNodeType::RemoveFromStringArray:
	case EStoryFlowNodeType::RemoveFromImageArray:
	case EStoryFlowNodeType::RemoveFromCharacterArray:
	case EStoryFlowNodeType::RemoveFromAudioArray:
	{
		int32 Index = Evaluator ? Evaluator->EvaluateIntegerInput(Node, TEXT("integer"), Node->Data.Value.GetInt(0)) : Node->Data.Value.GetInt(0);
		if (Index >= 0 && Index < Arr.Num())
		{
			Arr.RemoveAt(Index);
		}
		break;
	}

	// Clear operations
	case EStoryFlowNodeType::ClearBoolArray:
	case EStoryFlowNodeType::ClearIntArray:
	case EStoryFlowNodeType::ClearFloatArray:
	case EStoryFlowNodeType::ClearStringArray:
	case EStoryFlowNodeType::ClearImageArray:
	case EStoryFlowNodeType::ClearCharacterArray:
	case EStoryFlowNodeType::ClearAudioArray:
		Arr.Empty();
		break;

	default:
		break;
	}

	// Store result array in CachedOutput so downstream nodes connected to this
	// node's output can read it (matches HTML's setNodeOutputValue pattern)
	FNodeRuntimeState& ArrayNodeState = ExecutionContext.GetNodeState(Node->Id);
	ArrayNodeState.CachedOutput.SetArray(Arr);
	ArrayNodeState.bHasCachedOutput = true;

	bool bVarIsGlobal = !ExecutionContext.LocalVariables.Contains(Var->Id);
	SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=[array]", *Var->Name, bVarIsGlobal ? TEXT("true") : TEXT("false"));
	NotifyVariableChanged(*Var, bVarIsGlobal);
	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleForEachLoop(FStoryFlowNode* Node)
{
	FNodeRuntimeState& NodeState = ExecutionContext.GetNodeState(Node->Id);

	// Initialize loop on first entry
	if (!NodeState.bLoopInitialized)
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

		NodeState.LoopArray = Array;
		NodeState.LoopIndex = 0;
		NodeState.bLoopInitialized = true;
	}

	if (NodeState.LoopIndex < NodeState.LoopArray.Num())
	{
		// Clear evaluation caches from previous iteration so boolean chains re-evaluate
		ExecutionContext.ClearEvaluationCache();

		// Restore cached outputs for all active outer loops (nested forEach support)
		for (const FStoryFlowLoopContext& Frame : ExecutionContext.LoopStack)
		{
			FNodeRuntimeState& OuterState = ExecutionContext.GetNodeState(Frame.NodeId);
			if (OuterState.bLoopInitialized && OuterState.LoopIndex < OuterState.LoopArray.Num())
			{
				OuterState.CachedOutput = OuterState.LoopArray[OuterState.LoopIndex];
				OuterState.bHasCachedOutput = true;
			}
		}

		// Set current element as cached output
		NodeState.CachedOutput = NodeState.LoopArray[NodeState.LoopIndex];
		NodeState.bHasCachedOutput = true;

		SF_TRACE(ExecutionContext, "LOOP %s index=%d value=%s", *Node->Id, NodeState.LoopIndex, *NodeState.CachedOutput.ToString());

		// Push loop context for this iteration
		FStoryFlowLoopContext LoopContext;
		LoopContext.NodeId = Node->Id;
		LoopContext.Type = EStoryFlowLoopType::ForEach;
		LoopContext.CurrentIndex = NodeState.LoopIndex;
		ExecutionContext.LoopStack.Push(LoopContext);

		// Execute loop body
		ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_LoopBody));
	}
	else
	{
		// Loop complete - cleanup
		NodeState.bLoopInitialized = false;
		NodeState.LoopArray.Empty();
		NodeState.bHasCachedOutput = false;
		NodeState.CachedOutput.Reset();

		if (ExecutionContext.LoopStack.Num() > 0 && ExecutionContext.LoopStack.Last().NodeId == Node->Id)
		{
			ExecutionContext.LoopStack.Pop();
		}

		// Continue after loop
		ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_LoopCompleted));
	}
}

void UStoryFlowComponent::HandleSetImage(FStoryFlowNode* Node)
{
	// Evaluate image value from connected input or inline
	FString NewValue;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateStringInput(Node, TEXT("image"), Node->Data.Value.GetString());
	}
	else
	{
		NewValue = Node->Data.Value.GetString();
	}

	SF_TRACE(ExecutionContext, "IMAGE \"%s\"", *NewValue);

	FStoryFlowVariant Value;
	Value.SetString(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleSetAudio(FStoryFlowNode* Node)
{
	// Evaluate audio value from connected input or inline
	FString NewValue;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateStringInput(Node, TEXT("audio"), Node->Data.Value.GetString());
	}
	else
	{
		NewValue = Node->Data.Value.GetString();
	}

	SF_TRACE(ExecutionContext, "AUDIO \"%s\"", *NewValue);

	FStoryFlowVariant Value;
	Value.SetString(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleSetCharacter(FStoryFlowNode* Node)
{
	// Evaluate character value from connected input or inline
	FString NewValue;
	if (Evaluator)
	{
		NewValue = Evaluator->EvaluateStringInput(Node, TEXT("character"), Node->Data.Value.GetString());
	}
	else
	{
		NewValue = Node->Data.Value.GetString();
	}

	FStoryFlowVariant Value;
	Value.SetString(NewValue);
	ExecutionContext.SetVariable(Node->Data.Variable, Value, Node->Data.bIsGlobal);
	if (FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal))
	{
		SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=%s", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), *Value.ToString());
		NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	}

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
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
	FString SourceHandle = StoryFlowHandles::Source(Node->Id, EnumValue);
	const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);

	if (Edge)
	{
		ProcessNextNode(SourceHandle);
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: switchOnEnum - No output handle found for enum value '%s'"), *EnumValue);
	}
}

void UStoryFlowComponent::HandleRandomBranch(FStoryFlowNode* Node)
{
	const TArray<FStoryFlowWeightedOption>& Options = Node->Data.RandomBranchOptions;
	if (Options.Num() == 0)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: randomBranch '%s' has no options defined"), *Node->Id);
		return;
	}

	// Calculate total weight (resolve connected integer handles per option)
	TArray<int32> ResolvedWeights;
	ResolvedWeights.Reserve(Options.Num());
	int32 TotalWeight = 0;
	for (const FStoryFlowWeightedOption& Option : Options)
	{
		int32 W = Evaluator->EvaluateIntegerInput(Node, TEXT("integer-") + Option.Id, Option.Weight);
		W = FMath::Max(0, W);
		ResolvedWeights.Add(W);
		TotalWeight += W;
	}

	// If all weights are zero, fall back to first option
	if (TotalWeight <= 0)
	{
		const FStoryFlowWeightedOption& FirstOption = Options[0];
		FString SourceHandle = StoryFlowHandles::Source(Node->Id, FirstOption.Id);
		const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);
		if (Edge)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: randomBranch '%s' all weights zero, falling back to first option '%s'"),
				*Node->Id, *FirstOption.Id);
			ProcessNextNode(SourceHandle);
		}
		else
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: randomBranch '%s' - no edge connected to fallback output '%s'"),
				*Node->Id, *FirstOption.Id);
		}
		return;
	}

	// Pick a random value in [0, TotalWeight)
	const int32 Roll = FMath::RandRange(0, TotalWeight - 1);

	// Find selected option using cumulative weight
	int32 Cumulative = 0;
	int32 SelectedIndex = 0;
	for (int32 i = 0; i < Options.Num(); ++i)
	{
		Cumulative += ResolvedWeights[i];
		if (Roll < Cumulative)
		{
			SelectedIndex = i;
			break;
		}
	}
	const FStoryFlowWeightedOption& SelectedOption = Options[SelectedIndex];

	// Construct output handle: "source-{nodeId}-{optionId}"
	FString SourceHandle = StoryFlowHandles::Source(Node->Id, SelectedOption.Id);
	const FStoryFlowConnection* Edge = ExecutionContext.FindEdgeBySourceHandle(SourceHandle);

	if (Edge)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: randomBranch '%s' selected option '%s' (weight %d/%d)"),
			*Node->Id, *SelectedOption.Id, ResolvedWeights[SelectedIndex], TotalWeight);
		ProcessNextNode(SourceHandle);
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: randomBranch '%s' - no edge connected to selected output '%s'"),
			*Node->Id, *SelectedOption.Id);
	}
}

void UStoryFlowComponent::HandleSetBackgroundImage(FStoryFlowNode* Node)
{
	// Evaluate image input - check connected input first, fall back to inline value
	FString ImagePath = Node->Data.Value.GetString();

	if (Evaluator)
	{
		const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, StoryFlowHandles::In_ImageInput);
		if (Edge)
		{
			FStoryFlowNode* SourceNode = ExecutionContext.GetNode(Edge->Source);
			if (SourceNode)
			{
				ImagePath = Evaluator->EvaluateStringFromNode(SourceNode, Node->Id, Edge->SourceHandle);
			}
		}
	}

	SF_TRACE(ExecutionContext, "IMAGE \"%s\"", *ImagePath);

	// Resolve the image key to a texture and store as persistent background
	if (!ImagePath.IsEmpty())
	{
		UTexture2D* ResolvedImage = nullptr;

		// Try current script's ResolvedAssets (important for subscripts)
		if (UStoryFlowScriptAsset* CurrentScript = ExecutionContext.CurrentScript.Get())
		{
			if (TSoftObjectPtr<UObject>* Ptr = CurrentScript->ResolvedAssets.Find(ImagePath))
			{
				ResolvedImage = Cast<UTexture2D>(Ptr->LoadSynchronous());
			}
		}
		// Try project's ResolvedAssets
		if (!ResolvedImage)
		{
			if (UStoryFlowProjectAsset* Project = ExecutionContext.Project.Get())
			{
				if (TSoftObjectPtr<UObject>* Ptr = Project->ResolvedAssets.Find(ImagePath))
				{
					ResolvedImage = Cast<UTexture2D>(Ptr->LoadSynchronous());
				}
			}
		}

		ExecutionContext.PersistentBackgroundImage = ResolvedImage;
	}
	else
	{
		// Empty key = clear the background
		ExecutionContext.PersistentBackgroundImage = nullptr;
	}

	// Broadcast the background image change (string key for Blueprint handlers)
	OnBackgroundImageChanged.Broadcast(ImagePath);

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Output));
}

void UStoryFlowComponent::HandlePlayAudio(FStoryFlowNode* Node)
{
	// Evaluate audio input - check connected input first, fall back to inline value
	FString AudioPath = Node->Data.Value.GetString();

	if (Evaluator)
	{
		const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, StoryFlowHandles::In_AudioInput);
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

	SF_TRACE(ExecutionContext, "AUDIO \"%s\"", *AudioPath);

	// Resolve the audio key and play it internally
	if (!AudioPath.IsEmpty())
	{
		USoundBase* ResolvedAudio = nullptr;

		// Try current script's ResolvedAssets (important for subscripts)
		if (UStoryFlowScriptAsset* CurrentScript = ExecutionContext.CurrentScript.Get())
		{
			if (TSoftObjectPtr<UObject>* Ptr = CurrentScript->ResolvedAssets.Find(AudioPath))
			{
				ResolvedAudio = Cast<USoundBase>(Ptr->LoadSynchronous());
			}
		}
		// Try project's ResolvedAssets
		if (!ResolvedAudio)
		{
			if (UStoryFlowProjectAsset* Project = ExecutionContext.Project.Get())
			{
				if (TSoftObjectPtr<UObject>* Ptr = Project->ResolvedAssets.Find(AudioPath))
				{
					ResolvedAudio = Cast<USoundBase>(Ptr->LoadSynchronous());
				}
			}
		}

		if (ResolvedAudio)
		{
			PlayDialogueAudio(ResolvedAudio, bLoop);
		}
	}

	// Broadcast event for game code that wants additional handling
	OnAudioPlayRequested.Broadcast(AudioPath, bLoop);

	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Output));
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

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleSetCharacterVar - CharPath='%s' VarName='%s' VarType='%s' InlineValue='%s'"),
		*CharacterPath, *VariableName, *VariableType, *Node->Data.Value.ToString());

	// First check if there's a connected character input
	FString CharacterInputHandle = StoryFlowHandles::Target(Node->Id, StoryFlowHandles::In_CharacterInput);
	const FStoryFlowConnection* CharEdge = nullptr;
	if (UStoryFlowScriptAsset* CurrentScript = ExecutionContext.CurrentScript.Get())
	{
		for (const FStoryFlowConnection& Conn : CurrentScript->Connections)
		{
			if (Conn.TargetHandle == CharacterInputHandle)
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
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: SetCharacterVar has no character path"));
		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
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
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleSetCharacterVar - Found connected input from node '%s' (type='%s')"),
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
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleSetCharacterVar - Evaluated connected value: '%s'"), *NewValue.ToString());
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
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleSetCharacterVar - Using inline string: key='%s' resolved='%s'"), *StringKey, *ResolvedString);
		}
		else
		{
			NewValue = Node->Data.Value;
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleSetCharacterVar - Using inline value: '%s'"), *NewValue.ToString());
		}
	}

	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: HandleSetCharacterVar - Setting '%s' on character '%s' to '%s'"),
		*VariableName, *CharacterPath, *NewValue.ToString());

	SF_TRACE(ExecutionContext, "VAR SET \"%s.%s\" global=false value=%s", *CharacterPath, *VariableName, *NewValue.ToString());

	// Set the character variable
	ExecutionContext.SetCharacterVariable(CharacterPath, VariableName, NewValue);

	// For Image field: resolve and cache the texture NOW while still in the correct script context.
	// Cross-script lookups fail because asset keys are per-script, so we cache the resolved texture
	// for BuildDialogueState to use as fallback.
	if (VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
	{
		FString ImageKey = NewValue.GetString();
		if (!ImageKey.IsEmpty())
		{
			FStoryFlowCharacterDef* CharDef = ExecutionContext.FindCharacter(CharacterPath);
			if (CharDef)
			{
				UTexture2D* Resolved = nullptr;
				if (UStoryFlowScriptAsset* CurrentScript = ExecutionContext.CurrentScript.Get())
				{
					if (TSoftObjectPtr<UObject>* Ptr = CurrentScript->ResolvedAssets.Find(ImageKey))
					{
						Resolved = Cast<UTexture2D>(Ptr->LoadSynchronous());
					}
				}
				if (!Resolved)
				{
					if (UStoryFlowProjectAsset* Project = ExecutionContext.Project.Get())
					{
						if (TSoftObjectPtr<UObject>* Ptr = Project->ResolvedAssets.Find(ImageKey))
						{
							Resolved = Cast<UTexture2D>(Ptr->LoadSynchronous());
						}
					}
				}
				CharDef->CachedImage = Resolved;
			}
		}
	}

	// Continue execution
	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
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
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: BuildDialogueState - Looking up character '%s'"), *DialogueNode->Data.Character);

		// Use ExecutionContext.FindCharacter to get the runtime copy (mutable)
		if (FStoryFlowCharacterDef* CharDef = ExecutionContext.FindCharacter(DialogueNode->Data.Character))
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: BuildDialogueState - Found character, raw Name='%s'"), *CharDef->Name);
			State.Character.Name = ExecutionContext.GetString(CharDef->Name, LanguageCode);
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: BuildDialogueState - Resolved Name='%s'"), *State.Character.Name);

			// Load character image from the runtime character data (CharDef->Image).
			// SetCharacterVar may have updated Image to a path that lives in the character's
			// own assets, the current script's assets, or the project's global assets.
			if (!CharDef->Image.IsEmpty())
			{
				UTexture2D* ResolvedImage = nullptr;
				UStoryFlowProjectAsset* Project = ExecutionContext.Project.Get();

				// 1. Try character asset's ResolvedAssets
				if (!ResolvedImage && Project)
				{
					FString NormalizedCharPath = NormalizeCharacterPath(DialogueNode->Data.Character);
					if (UStoryFlowCharacterAsset* const* CharAsset = Project->Characters.Find(NormalizedCharPath))
					{
						if (TSoftObjectPtr<UObject>* CharImagePtr = (*CharAsset)->ResolvedAssets.Find(CharDef->Image))
						{
							ResolvedImage = Cast<UTexture2D>(CharImagePtr->LoadSynchronous());
						}
					}
				}

				// 2. Try current script's ResolvedAssets (SetCharacterVar may set image to a script-level asset)
				if (!ResolvedImage)
				{
					if (UStoryFlowScriptAsset* CurrentScriptAsset = ExecutionContext.CurrentScript.Get())
					{
						if (TSoftObjectPtr<UObject>* ScriptImagePtr = CurrentScriptAsset->ResolvedAssets.Find(CharDef->Image))
						{
							ResolvedImage = Cast<UTexture2D>(ScriptImagePtr->LoadSynchronous());
						}
					}
				}

				// 3. Try project's global ResolvedAssets
				if (!ResolvedImage && Project)
				{
					if (TSoftObjectPtr<UObject>* ProjectImagePtr = Project->ResolvedAssets.Find(CharDef->Image))
					{
						ResolvedImage = Cast<UTexture2D>(ProjectImagePtr->LoadSynchronous());
					}
				}

				// Fall back to cached texture for cross-script resolution
				if (!ResolvedImage && CharDef->CachedImage)
				{
					ResolvedImage = CharDef->CachedImage;
				}

				State.Character.Image = ResolvedImage;
			}

			// Copy character variables
			for (const auto& VarPair : CharDef->Variables)
			{
				State.Character.Variables.Add(VarPair.Key, VarPair.Value.Value);
			}
		}
		else
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: BuildDialogueState - Character NOT FOUND: '%s'"), *DialogueNode->Data.Character);
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
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Image reset (imageReset=true)"));
	}
	else
	{
		// No image and imageReset=false - keep previous image
		State.Image = ExecutionContext.PersistentBackgroundImage;
		if (State.Image)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Using persistent background image"));
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

	// Build visible text blocks (non-interactive, filtered by visibility)
	for (const FStoryFlowTextBlock& Block : DialogueNode->Data.TextBlocks)
	{
		// Check visibility condition (same mechanism as options)
		if (Evaluator && !Evaluator->EvaluateOptionVisibility(DialogueNode, Block.Id))
		{
			continue;
		}

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
		if (Choice.bOnceOnly && ExecutionContext.ExternalUsedOnceOnlyOptions && ExecutionContext.ExternalUsedOnceOnlyOptions->Contains(OnceOnlyKey))
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

	// Can advance: node defines ZERO options AND header output handle has an edge
	if (DialogueNode->Data.Options.Num() == 0)
	{
		const FString HeaderHandle = StoryFlowHandles::Source(DialogueNode->Id);
		State.bCanAdvance = (ExecutionContext.FindEdgeBySourceHandle(HeaderHandle) != nullptr);
	}

	// Pass audio advance-on-end flags to state so widgets can adjust UI
	State.bAudioAdvanceOnEnd = DialogueNode->Data.bAudioAdvanceOnEnd && !DialogueNode->Data.bAudioLoop;
	State.bAudioAllowSkip = State.bAudioAdvanceOnEnd && DialogueNode->Data.bAudioAllowSkip;

	return State;
}

void UStoryFlowComponent::NotifyVariableChanged(const FStoryFlowVariable& Variable, bool bIsGlobal)
{
	OnVariableChanged.Broadcast(Variable, bIsGlobal);

	// Live variable interpolation: If dialogue is active, re-interpolate text and update UI
	if (ExecutionContext.bIsWaitingForInput && ExecutionContext.CurrentDialogueState.bIsValid)
	{
		// Get the current dialogue node to rebuild state with fresh variable values
		FStoryFlowNode* CurrentNode = ExecutionContext.GetNode(ExecutionContext.CurrentDialogueState.NodeId);
		if (CurrentNode && CurrentNode->Type == EStoryFlowNodeType::Dialogue)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Variable '%s' changed, re-interpolating dialogue text"), *Variable.Id);

			// Rebuild dialogue state with updated variable values
			ExecutionContext.CurrentDialogueState = BuildDialogueState(CurrentNode);

			// Re-broadcast so UI updates
			OnDialogueUpdated.Broadcast(ExecutionContext.CurrentDialogueState);
		}
	}
}

void UStoryFlowComponent::ReportError(const FString& ErrorMessage)
{
	UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow Error: %s"), *ErrorMessage);
	OnError.Broadcast(ErrorMessage);
}

void UStoryFlowComponent::ContinueForEachLoop(const FString& NodeId)
{
	FStoryFlowNode* LoopNode = ExecutionContext.GetNode(NodeId);
	if (!LoopNode)
	{
		return;
	}

	FNodeRuntimeState& NodeState = ExecutionContext.GetNodeState(NodeId);
	if (!NodeState.bLoopInitialized)
	{
		return;
	}

	// Increment loop index
	NodeState.LoopIndex++;

	// Pop the loop context that was pushed for this iteration
	if (ExecutionContext.LoopStack.Num() > 0 && ExecutionContext.LoopStack.Last().NodeId == NodeId)
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
		for (const FStoryFlowConnection* ConnPtr : CurrentScriptAsset->GetEdgesByTarget(Node->Id))
		{
			// Check if this is a flow edge (not a data edge)
			bool bIsDataEdge = ConnPtr->SourceHandle.Contains(TEXT("-boolean-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-integer-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-float-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-string-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-enum-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-image-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-character-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-audio-"));

			if (!bIsDataEdge)
			{
				FStoryFlowNode* SourceNode = ExecutionContext.GetNode(ConnPtr->Source);
				if (SourceNode && SourceNode->Type == EStoryFlowNodeType::Dialogue)
				{
					ProcessNode(SourceNode);
					return;
				}
			}
		}
	}
}

// ============================================================================
// Audio Helpers
// ============================================================================

void UStoryFlowComponent::PlayDialogueAudio_Implementation(USoundBase* Sound, bool bLoop)
{
	if (!Sound)
	{
		return;
	}

	// Stop any currently playing dialogue audio
	StopDialogueAudio();

	// Spawn audio component — 3D attached to owner or 2D non-spatialized
	if (bUse3DAudio && GetOwner())
	{
		CurrentDialogueAudio = UGameplayStatics::SpawnSoundAttached(
			Sound, GetOwner()->GetRootComponent(),
			NAME_None, FVector::ZeroVector, EAttachLocation::KeepRelativeOffset,
			false, DialogueVolumeMultiplier, 1.0f, 0.0f,
			DialogueAttenuation.Get(), DialogueConcurrency.Get(), false);
	}
	else
	{
		CurrentDialogueAudio = UGameplayStatics::SpawnSound2D(this, Sound, DialogueVolumeMultiplier, 1.0f, 0.0f, DialogueConcurrency.Get(), false, false);
	}

	if (CurrentDialogueAudio)
	{
		// Apply sound class override for audio mixer categorization
		if (DialogueSoundClass)
		{
			CurrentDialogueAudio->SoundClassOverride = DialogueSoundClass;
		}

		// Configure looping before playing
		if (bLoop)
		{
			CurrentDialogueAudio->SetSound(Sound);
			if (!bUse3DAudio)
			{
				CurrentDialogueAudio->bIsUISound = true;
			}

			// Stop the auto-started playback, configure loop, then restart
			CurrentDialogueAudio->Stop();
			CurrentDialogueAudio->Sound = Sound;
		}

		// Play the audio
		CurrentDialogueAudio->Play();

		// Always bind OnAudioFinished for looping and/or advance-on-end
		CurrentDialogueAudio->OnAudioFinished.AddDynamic(this, &UStoryFlowComponent::OnDialogueAudioFinished);

		if (bLoop)
		{
			// Store loop flag for the callback
			CurrentDialogueAudio->ComponentTags.Add(FName("StoryFlowLoop"));
		}

		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Audio started (3D=%s, loop=%s)"),
			bUse3DAudio ? TEXT("true") : TEXT("false"),
			bLoop ? TEXT("true") : TEXT("false"));
	}
}

void UStoryFlowComponent::StopDialogueAudio_Implementation()
{
	if (CurrentDialogueAudio)
	{
		// Remove callback to prevent restart or advance
		CurrentDialogueAudio->OnAudioFinished.RemoveAll(this);

		if (CurrentDialogueAudio->IsPlaying())
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Stopping dialogue audio"));
			CurrentDialogueAudio->Stop();
		}

		CurrentDialogueAudio->DestroyComponent();
	}
	CurrentDialogueAudio = nullptr;

	// Clear advance-on-end state
	bWaitingForAudioAdvance = false;
	bAudioAdvanceAllowSkip = false;
}

void UStoryFlowComponent::OnDialogueAudioFinished()
{
	// Check if this audio was marked for looping
	if (CurrentDialogueAudio && CurrentDialogueAudio->ComponentTags.Contains(FName("StoryFlowLoop")))
	{
		// Restart the audio for looping
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Looping dialogue audio"));
		CurrentDialogueAudio->Play();
	}
	else if (bWaitingForAudioAdvance)
	{
		// Audio finished playing — auto-advance the dialogue
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Audio finished, auto-advancing dialogue"));
		bWaitingForAudioAdvance = false;
		bAudioAdvanceAllowSkip = false;
		AdvanceDialogue();
	}
}
