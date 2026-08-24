// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Components/StoryFlowComponent.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetStore.h"
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
	ExecutionContext.InitializeWithSubsystem(Project, ScriptAsset, &Subsystem->GetGlobalVariables(), &Subsystem->GetRuntimeCharacters(), &Subsystem->GetUsedOnceOnlyOptions(), Subsystem->GetDataAssetStore());
	ExecutionContext.bIsExecuting = true;
	ExecutionContext.bTraceEnabled = bTraceEnabled;
	// ONCE PER COMPONENT, not once per start. Restarting a dialogue (StartDialogueWithScript on a
	// component that is already running one) does NOT stop the old one — no end event fires, by
	// design, because existing projects restart mid-dialogue and do not expect one. Counting the
	// second start would leave ActiveDialogueCount permanently above zero after the single stop
	// that follows, and that counter is the ONLY gate on LoadFromSlot: one restart anywhere would
	// disable loading for the rest of the session.
	if (!bCountedActiveDialogue)
	{
		Subsystem->NotifyDialogueStarted();
		bCountedActiveDialogue = true;
	}
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: ExecutionContext initialized, CurrentNodeId='%s'"), *ExecutionContext.CurrentNodeId);

	// Create evaluator
	Evaluator = MakeUnique<FStoryFlowEvaluator>(&ExecutionContext);
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Evaluator created"));

	// Create dialogue widget if configured
	if (DialogueWidgetClass)
	{
		// Clean up existing widget if any. When the game owns placement the old
		// widget may still be animating out, so only let go of it — finishing it
		// is the game's job, same as at dialogue end.
		if (ActiveDialogueWidget)
		{
			if (bAutoAddWidgetToViewport)
			{
				ActiveDialogueWidget->RemoveFromParent();
			}
			else
			{
				// Restarting does not stop the dialogue, so no OnDialogueEnded fires
				// here: detaching is the old widget's only cue to stop following this
				// component, and without it the new dialogue would drive it.
				ActiveDialogueWidget->DetachFromComponent();
			}
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
				if (bAutoAddWidgetToViewport)
				{
					ActiveDialogueWidget->AddToViewport();
				}
				// Announce after the placement decision so handlers see where the
				// widget ended up (and can place it themselves when it went nowhere)
				OnDialogueWidgetCreated.Broadcast(ActiveDialogueWidget);
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

	// The other half of the pair: decrement only what this component actually counted. The
	// bIsExecuting early-out above already makes a second StopDialogue a no-op, but that flag is
	// also cleared by ExecutionContext.Reset() from other paths, so the count needs a witness of
	// its own rather than a proxy for one.
	if (UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem())
	{
		if (bCountedActiveDialogue)
		{
			Subsystem->NotifyDialogueEnded();
		}
	}
	bCountedActiveDialogue = false;
	Evaluator.Reset();

	OnScriptEnded.Broadcast(CurrentScriptPath);
	OnDialogueEnded.Broadcast();

	// Destroy dialogue widget after broadcasting so it receives OnDialogueEnded.
	// This is the only teardown funnel — natural end, StopDialogue and EndPlay all
	// arrive here — so it is also where component destruction lands. Removing the
	// widget is right only when the component added it: with auto-add off it was
	// never in the viewport, so there is nothing to leak there, and if the game
	// parented it into its own UI, that tree is the game's to unwind. The
	// OnDialogueEnded broadcast just above is the signal it does that on, and it
	// fires even when the component is being destroyed.
	if (ActiveDialogueWidget)
	{
		if (bAutoAddWidgetToViewport)
		{
			ActiveDialogueWidget->RemoveFromParent();
		}
		else
		{
			// After the OnDialogueEnded broadcast above, so the widget still gets its
			// ending cue before it stops following this component.
			ActiveDialogueWidget->DetachFromComponent();
		}
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

TArray<FString> UStoryFlowComponent::GetCurrentDialogueTags() const
{
	return ExecutionContext.CurrentDialogueState.Tags;
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

UStoryFlowDialogueWidget* UStoryFlowComponent::GetDialogueWidget() const
{
	return ActiveDialogueWidget;
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
	return Var ? ResolveString(Var->Value.GetString()) : TEXT("");
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

void UStoryFlowComponent::ApplyArrayVariable(const FString& VariableName, bool bGlobal, TArray<FStoryFlowVariant>&& Items)
{
	if (FStoryFlowVariable* Var = FindVariableByName(VariableName, bGlobal))
	{
		// Replace only the element storage: the variant's scalar fields and
		// type stay untouched, matching the Unity plugin's Set*ArrayVariable.
		Var->Value.GetArrayMutable() = MoveTemp(Items);
		NotifyVariableChanged(*Var, bGlobal);
	}
}

void UStoryFlowComponent::SetBoolArrayVariable(const FString& VariableName, const TArray<bool>& Values, bool bGlobal)
{
	TArray<FStoryFlowVariant> Items;
	Items.Reserve(Values.Num());
	for (bool bValue : Values)
	{
		FStoryFlowVariant Item;
		Item.SetBool(bValue);
		Items.Add(Item);
	}
	ApplyArrayVariable(VariableName, bGlobal, MoveTemp(Items));
}

void UStoryFlowComponent::SetIntArrayVariable(const FString& VariableName, const TArray<int32>& Values, bool bGlobal)
{
	TArray<FStoryFlowVariant> Items;
	Items.Reserve(Values.Num());
	for (int32 Value : Values)
	{
		FStoryFlowVariant Item;
		Item.SetInt(Value);
		Items.Add(Item);
	}
	ApplyArrayVariable(VariableName, bGlobal, MoveTemp(Items));
}

void UStoryFlowComponent::SetFloatArrayVariable(const FString& VariableName, const TArray<float>& Values, bool bGlobal)
{
	TArray<FStoryFlowVariant> Items;
	Items.Reserve(Values.Num());
	for (float Value : Values)
	{
		FStoryFlowVariant Item;
		Item.SetFloat(Value);
		Items.Add(Item);
	}
	ApplyArrayVariable(VariableName, bGlobal, MoveTemp(Items));
}

void UStoryFlowComponent::SetStringArrayVariable(const FString& VariableName, const TArray<FString>& Values, bool bGlobal)
{
	TArray<FStoryFlowVariant> Items;
	Items.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		FStoryFlowVariant Item;
		Item.SetString(Value);
		Items.Add(Item);
	}
	ApplyArrayVariable(VariableName, bGlobal, MoveTemp(Items));
}

void UStoryFlowComponent::SetEnumArrayVariable(const FString& VariableName, const TArray<FString>& Values, bool bGlobal)
{
	TArray<FStoryFlowVariant> Items;
	Items.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		FStoryFlowVariant Item;
		Item.SetEnum(Value);
		Items.Add(Item);
	}
	ApplyArrayVariable(VariableName, bGlobal, MoveTemp(Items));
}

void UStoryFlowComponent::SetImageArrayVariable(const FString& VariableName, const TArray<FString>& AssetKeys, bool bGlobal)
{
	// Asset keys are stored as plain strings, matching how ParseVariant imports
	// image/audio/character array elements (GetString accepts them either way).
	SetStringArrayVariable(VariableName, AssetKeys, bGlobal);
}

void UStoryFlowComponent::SetAudioArrayVariable(const FString& VariableName, const TArray<FString>& AssetKeys, bool bGlobal)
{
	SetStringArrayVariable(VariableName, AssetKeys, bGlobal);
}

void UStoryFlowComponent::SetCharacterArrayVariable(const FString& VariableName, const TArray<FString>& CharacterPaths, bool bGlobal)
{
	SetStringArrayVariable(VariableName, CharacterPaths, bGlobal);
}

TArray<FStoryFlowVariant> UStoryFlowComponent::GetArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<FStoryFlowVariant> Out;

	// When bGlobal is false, search locals (during dialogue) then fall back to globals.
	// When bGlobal is true, search only globals. Matches Unity's GetArrayVariable parity.
	FStoryFlowVariable* Var = nullptr;
	if (!bGlobal && ExecutionContext.bIsExecuting)
	{
		Var = ExecutionContext.FindVariableByName(VariableName, /*bIsGlobal=*/false);
	}
	if (!Var)
	{
		Var = FindVariableByName(VariableName, /*bGlobal=*/true);
	}
	if (!Var)
	{
		return Out;
	}

	if (!Var->bIsArray)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not an array"), *VariableName);
		return Out;
	}

	// Return resolved copies so callers see localized text for String and Enum
	// element types. Image, audio, and character elements pass through unchanged
	// because their values are asset keys / paths, not string-table keys.
	for (const FStoryFlowVariant& Elem : Var->Value.GetArray())
	{
		FStoryFlowVariant Copy = Elem;
		if (Elem.GetType() == EStoryFlowVariableType::String)
		{
			Copy.SetString(ResolveString(Elem.GetString()));
		}
		else if (Elem.GetType() == EStoryFlowVariableType::Enum)
		{
			Copy.SetEnum(ResolveString(Elem.GetString()));
		}
		Out.Add(Copy);
	}
	return Out;
}

FStoryFlowVariable* UStoryFlowComponent::FindArrayVariableForRead(const FString& VariableName, bool bGlobal, EStoryFlowVariableType ExpectedType, const TCHAR* TypeLabel)
{
	// Scoping mirrors GetArrayVariable: locals during dialogue, then globals.
	FStoryFlowVariable* Var = nullptr;
	if (!bGlobal && ExecutionContext.bIsExecuting)
	{
		Var = ExecutionContext.FindVariableByName(VariableName, /*bIsGlobal=*/false);
	}
	if (!Var)
	{
		Var = FindVariableByName(VariableName, /*bGlobal=*/true);
	}
	if (!Var)
	{
		return nullptr;
	}
	if (!Var->bIsArray)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not an array"), *VariableName);
		return nullptr;
	}
	if (Var->Type != ExpectedType)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not a %s array"), *VariableName, TypeLabel);
		return nullptr;
	}
	return Var;
}

TArray<bool> UStoryFlowComponent::GetBoolArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<bool> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::Boolean, TEXT("boolean")))
	{
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(Elem.GetBool());
		}
	}
	return Out;
}

TArray<int32> UStoryFlowComponent::GetIntArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<int32> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::Integer, TEXT("integer")))
	{
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(Elem.GetInt());
		}
	}
	return Out;
}

TArray<float> UStoryFlowComponent::GetFloatArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<float> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::Float, TEXT("float")))
	{
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(Elem.GetFloat());
		}
	}
	return Out;
}

TArray<FString> UStoryFlowComponent::GetStringArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<FString> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::String, TEXT("string")))
	{
		// Resolve through the string table so callers get localized text, like GetStringVariable.
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(ResolveString(Elem.GetString()));
		}
	}
	return Out;
}

TArray<FString> UStoryFlowComponent::GetEnumArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<FString> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::Enum, TEXT("enum")))
	{
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(ResolveString(Elem.GetString()));
		}
	}
	return Out;
}

TArray<FString> UStoryFlowComponent::GetImageArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<FString> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::Image, TEXT("image")))
	{
		// Asset keys pass through raw (not string-table resolved), matching GetCharacterArrayVariable.
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(Elem.GetString());
		}
	}
	return Out;
}

TArray<FString> UStoryFlowComponent::GetAudioArrayVariable(const FString& VariableName, bool bGlobal)
{
	TArray<FString> Out;
	if (const FStoryFlowVariable* Var = FindArrayVariableForRead(VariableName, bGlobal, EStoryFlowVariableType::Audio, TEXT("audio")))
	{
		const TArray<FStoryFlowVariant>& Elems = Var->Value.GetArray();
		Out.Reserve(Elems.Num());
		for (const FStoryFlowVariant& Elem : Elems)
		{
			Out.Add(Elem.GetString());
		}
	}
	return Out;
}

FStoryFlowVariable* UStoryFlowComponent::FindMapVariableForAccess(const FString& VariableName, bool bGlobal)
{
	// Scoping mirrors FindArrayVariableForRead: locals during dialogue, then globals.
	FStoryFlowVariable* Var = nullptr;
	if (!bGlobal && ExecutionContext.bIsExecuting)
	{
		Var = ExecutionContext.FindVariableByName(VariableName, /*bIsGlobal=*/false);
	}
	if (!Var)
	{
		Var = FindVariableByName(VariableName, /*bGlobal=*/true);
	}
	if (!Var)
	{
		return nullptr;
	}
	if (Var->Type != EStoryFlowVariableType::Map)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not a map"), *VariableName);
		return nullptr;
	}
	return Var;
}

TMap<FString, bool> UStoryFlowComponent::GetStringToBoolMap(const FString& VariableName, bool bGlobal)
{
	TMap<FString, bool> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return Out;
	}
	if (Var->ValueType != EStoryFlowVariableType::Boolean)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have boolean values"), *VariableName);
		return Out;
	}
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Out.Add(Entry.Key.GetString(), Entry.Value.GetBool());
	}
	return Out;
}

TMap<FString, int32> UStoryFlowComponent::GetStringToIntMap(const FString& VariableName, bool bGlobal)
{
	TMap<FString, int32> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return Out;
	}
	if (Var->ValueType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer values"), *VariableName);
		return Out;
	}
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Out.Add(Entry.Key.GetString(), Entry.Value.GetInt());
	}
	return Out;
}

TMap<FString, float> UStoryFlowComponent::GetStringToFloatMap(const FString& VariableName, bool bGlobal)
{
	TMap<FString, float> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return Out;
	}
	if (Var->ValueType != EStoryFlowVariableType::Float)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have float values"), *VariableName);
		return Out;
	}
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Out.Add(Entry.Key.GetString(), Entry.Value.GetFloat());
	}
	return Out;
}

TMap<FString, FString> UStoryFlowComponent::GetStringToStringMap(const FString& VariableName, bool bGlobal)
{
	TMap<FString, FString> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return Out;
	}
	const EStoryFlowVariableType VT = Var->ValueType;
	const bool bStringFamily = (VT == EStoryFlowVariableType::String || VT == EStoryFlowVariableType::Enum ||
		VT == EStoryFlowVariableType::Image || VT == EStoryFlowVariableType::Audio || VT == EStoryFlowVariableType::Character);
	if (!bStringFamily)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string-family values"), *VariableName);
		return Out;
	}
	const bool bResolve = (VT == EStoryFlowVariableType::String || VT == EStoryFlowVariableType::Enum);
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		const FString Value = bResolve ? ResolveString(Entry.Value.GetString()) : Entry.Value.GetString();
		Out.Add(Entry.Key.GetString(), Value);
	}
	return Out;
}

TMap<int32, bool> UStoryFlowComponent::GetIntToBoolMap(const FString& VariableName, bool bGlobal)
{
	TMap<int32, bool> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return Out;
	}
	if (Var->ValueType != EStoryFlowVariableType::Boolean)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have boolean values"), *VariableName);
		return Out;
	}
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Out.Add(Entry.Key.GetInt(), Entry.Value.GetBool());
	}
	return Out;
}

TMap<int32, int32> UStoryFlowComponent::GetIntToIntMap(const FString& VariableName, bool bGlobal)
{
	TMap<int32, int32> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return Out;
	}
	if (Var->ValueType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer values"), *VariableName);
		return Out;
	}
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Out.Add(Entry.Key.GetInt(), Entry.Value.GetInt());
	}
	return Out;
}

TMap<int32, float> UStoryFlowComponent::GetIntToFloatMap(const FString& VariableName, bool bGlobal)
{
	TMap<int32, float> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return Out;
	}
	if (Var->ValueType != EStoryFlowVariableType::Float)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have float values"), *VariableName);
		return Out;
	}
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Out.Add(Entry.Key.GetInt(), Entry.Value.GetFloat());
	}
	return Out;
}

TMap<int32, FString> UStoryFlowComponent::GetIntToStringMap(const FString& VariableName, bool bGlobal)
{
	TMap<int32, FString> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return Out;
	}
	const EStoryFlowVariableType VT = Var->ValueType;
	const bool bStringFamily = (VT == EStoryFlowVariableType::String || VT == EStoryFlowVariableType::Enum ||
		VT == EStoryFlowVariableType::Image || VT == EStoryFlowVariableType::Audio || VT == EStoryFlowVariableType::Character);
	if (!bStringFamily)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string-family values"), *VariableName);
		return Out;
	}
	const bool bResolve = (VT == EStoryFlowVariableType::String || VT == EStoryFlowVariableType::Enum);
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		const FString Value = bResolve ? ResolveString(Entry.Value.GetString()) : Entry.Value.GetString();
		Out.Add(Entry.Key.GetInt(), Value);
	}
	return Out;
}

TArray<FString> UStoryFlowComponent::GetMapKeysInOrder(const FString& VariableName, bool bGlobal)
{
	TArray<FString> Out;
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return Out; }
	const TArray<FStoryFlowMapEntry>& Entries = Var->Value.GetMap();
	Out.Reserve(Entries.Num());
	for (const FStoryFlowMapEntry& Entry : Entries)
	{
		// String/enum keys come back verbatim from GetString; integer keys are stringified.
		Out.Add(Var->KeyType == EStoryFlowVariableType::Integer
			? FString::FromInt(Entry.Key.GetInt())
			: Entry.Key.GetString());
	}
	return Out;
}

void UStoryFlowComponent::SetStringToBoolMap(const FString& VariableName, const TMap<FString, bool>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return;
	}
	if (Var->ValueType != EStoryFlowVariableType::Boolean)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have boolean values"), *VariableName);
		return;
	}
	const bool bEnumKey = (Var->KeyType == EStoryFlowVariableType::Enum);
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<FString, bool>& P : Values)
	{
		FStoryFlowMapEntry E;
		if (bEnumKey) { E.Key.SetEnum(P.Key); } else { E.Key.SetString(P.Key); }
		E.Value.SetBool(P.Value);
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetStringToIntMap(const FString& VariableName, const TMap<FString, int32>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return;
	}
	if (Var->ValueType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer values"), *VariableName);
		return;
	}
	const bool bEnumKey = (Var->KeyType == EStoryFlowVariableType::Enum);
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<FString, int32>& P : Values)
	{
		FStoryFlowMapEntry E;
		if (bEnumKey) { E.Key.SetEnum(P.Key); } else { E.Key.SetString(P.Key); }
		E.Value.SetInt(P.Value);
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetStringToFloatMap(const FString& VariableName, const TMap<FString, float>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return;
	}
	if (Var->ValueType != EStoryFlowVariableType::Float)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have float values"), *VariableName);
		return;
	}
	const bool bEnumKey = (Var->KeyType == EStoryFlowVariableType::Enum);
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<FString, float>& P : Values)
	{
		FStoryFlowMapEntry E;
		if (bEnumKey) { E.Key.SetEnum(P.Key); } else { E.Key.SetString(P.Key); }
		E.Value.SetFloat(P.Value);
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetStringToStringMap(const FString& VariableName, const TMap<FString, FString>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::String && Var->KeyType != EStoryFlowVariableType::Enum)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string keys"), *VariableName);
		return;
	}
	const EStoryFlowVariableType VT = Var->ValueType;
	const bool bStringFamily = (VT == EStoryFlowVariableType::String || VT == EStoryFlowVariableType::Enum ||
		VT == EStoryFlowVariableType::Image || VT == EStoryFlowVariableType::Audio || VT == EStoryFlowVariableType::Character);
	if (!bStringFamily)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string-family values"), *VariableName);
		return;
	}
	const bool bEnumKey = (Var->KeyType == EStoryFlowVariableType::Enum);
	const bool bEnumValue = (VT == EStoryFlowVariableType::Enum);
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<FString, FString>& P : Values)
	{
		FStoryFlowMapEntry E;
		if (bEnumKey) { E.Key.SetEnum(P.Key); } else { E.Key.SetString(P.Key); }
		if (bEnumValue) { E.Value.SetEnum(P.Value); } else { E.Value.SetString(P.Value); }
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetIntToBoolMap(const FString& VariableName, const TMap<int32, bool>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return;
	}
	if (Var->ValueType != EStoryFlowVariableType::Boolean)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have boolean values"), *VariableName);
		return;
	}
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<int32, bool>& P : Values)
	{
		FStoryFlowMapEntry E;
		E.Key.SetInt(P.Key);
		E.Value.SetBool(P.Value);
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetIntToIntMap(const FString& VariableName, const TMap<int32, int32>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return;
	}
	if (Var->ValueType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer values"), *VariableName);
		return;
	}
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<int32, int32>& P : Values)
	{
		FStoryFlowMapEntry E;
		E.Key.SetInt(P.Key);
		E.Value.SetInt(P.Value);
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetIntToFloatMap(const FString& VariableName, const TMap<int32, float>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return;
	}
	if (Var->ValueType != EStoryFlowVariableType::Float)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have float values"), *VariableName);
		return;
	}
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<int32, float>& P : Values)
	{
		FStoryFlowMapEntry E;
		E.Key.SetInt(P.Key);
		E.Value.SetFloat(P.Value);
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::SetIntToStringMap(const FString& VariableName, const TMap<int32, FString>& Values, bool bGlobal)
{
	FStoryFlowVariable* Var = FindMapVariableForAccess(VariableName, bGlobal);
	if (!Var) { return; }
	if (Var->KeyType != EStoryFlowVariableType::Integer)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have integer keys"), *VariableName);
		return;
	}
	const EStoryFlowVariableType VT = Var->ValueType;
	const bool bStringFamily = (VT == EStoryFlowVariableType::String || VT == EStoryFlowVariableType::Enum ||
		VT == EStoryFlowVariableType::Image || VT == EStoryFlowVariableType::Audio || VT == EStoryFlowVariableType::Character);
	if (!bStringFamily)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Map '%s' does not have string-family values"), *VariableName);
		return;
	}
	const bool bEnumValue = (VT == EStoryFlowVariableType::Enum);
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Reserve(Values.Num());
	for (const TPair<int32, FString>& P : Values)
	{
		FStoryFlowMapEntry E;
		E.Key.SetInt(P.Key);
		if (bEnumValue) { E.Value.SetEnum(P.Value); } else { E.Value.SetString(P.Value); }
		Entries.Add(E);
	}
	Var->Value.SetMap(Entries);
	NotifyVariableChanged(*Var, bGlobal);
}

void UStoryFlowComponent::GetMapVariable(const FString& VariableName, TArray<FStoryFlowVariant>& Keys, TArray<FStoryFlowVariant>& Values, bool bGlobal)
{
	Keys.Reset();
	Values.Reset();

	// Scoping mirrors GetArrayVariable: when bGlobal is false, search locals
	// (during dialogue) then fall back to globals; when true, globals only.
	FStoryFlowVariable* Var = nullptr;
	if (!bGlobal && ExecutionContext.bIsExecuting)
	{
		Var = ExecutionContext.FindVariableByName(VariableName, /*bIsGlobal=*/false);
	}
	if (!Var)
	{
		Var = FindVariableByName(VariableName, /*bGlobal=*/true);
	}
	if (!Var)
	{
		return;
	}

	if (Var->Type != EStoryFlowVariableType::Map)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not a map"), *VariableName);
		return;
	}

	// Return resolved copies so callers see localized text for string and enum
	// VALUE types — keys are raw identifiers and never resolve (the runtime-wide
	// map rule: values resolve via the strings table, keys never do). Image,
	// audio, and character values pass through unchanged because their values
	// are asset keys / paths, not string-table keys.
	for (const FStoryFlowMapEntry& Entry : Var->Value.GetMap())
	{
		Keys.Add(Entry.Key);
		FStoryFlowVariant ValueCopy = Entry.Value;
		if (Entry.Value.GetType() == EStoryFlowVariableType::String)
		{
			ValueCopy.SetString(ResolveString(Entry.Value.GetString()));
		}
		else if (Entry.Value.GetType() == EStoryFlowVariableType::Enum)
		{
			ValueCopy.SetEnum(ResolveString(Entry.Value.GetString()));
		}
		Values.Add(ValueCopy);
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

TArray<FString> UStoryFlowComponent::GetCharacterArrayVariable(const FString& VariableName)
{
	TArray<FString> Out;

	// Mirror _find_variable_by_display_name: try locals (during dialogue) then globals.
	FStoryFlowVariable* Var = nullptr;
	if (ExecutionContext.bIsExecuting)
	{
		Var = ExecutionContext.FindVariableByName(VariableName, /*bIsGlobal=*/false);
	}
	if (!Var)
	{
		Var = FindVariableByName(VariableName, /*bGlobal=*/true);
	}

	if (!Var)
	{
		return Out;
	}

	if (!Var->bIsArray)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not an array"), *VariableName);
		return Out;
	}

	if (Var->Type != EStoryFlowVariableType::Character)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' is not a character array"), *VariableName);
		return Out;
	}

	for (const FStoryFlowVariant& Elem : Var->Value.GetArray())
	{
		Out.Add(Elem.GetString());
	}
	return Out;
}

FStoryFlowCharacterDef UStoryFlowComponent::GetCharacter(const FString& CharacterPath)
{
	if (FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath))
	{
		return *CharDef;
	}
	return FStoryFlowCharacterDef();
}

TArray<FString> UStoryFlowComponent::GetCharacterVariableNames(const FString& CharacterPath)
{
	TArray<FString> Out;
	if (FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath))
	{
		CharDef->Variables.GetKeys(Out);
	}
	return Out;
}

UTexture2D* UStoryFlowComponent::GetCharacterPortrait(const FString& CharacterPath, const FString& AssetKey)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		return nullptr;
	}

	const FString Key = AssetKey.IsEmpty() ? CharDef->Image : AssetKey;
	if (Key.IsEmpty())
	{
		return nullptr;
	}

	return ResolveCharacterPortraitTexture(CharacterPath, Key, CharDef);
}

UTexture2D* UStoryFlowComponent::ResolveCharacterPortraitTexture(const FString& CharacterPath, const FString& AssetKey, FStoryFlowCharacterDef* CharDef)
{
	if (AssetKey.IsEmpty())
	{
		return nullptr;
	}

	UTexture2D* ResolvedImage = nullptr;
	UStoryFlowProjectAsset* Project = ExecutionContext.bIsExecuting ? ExecutionContext.Project.Get() : GetProject();

	// 1. Try character asset's ResolvedAssets
	if (Project)
	{
		const FString NormalizedCharPath = NormalizeCharacterPath(CharacterPath);
		if (UStoryFlowCharacterAsset* const* CharAsset = Project->Characters.Find(NormalizedCharPath))
		{
			if (TSoftObjectPtr<UObject>* CharImagePtr = (*CharAsset)->ResolvedAssets.Find(AssetKey))
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
			if (TSoftObjectPtr<UObject>* ScriptImagePtr = CurrentScriptAsset->ResolvedAssets.Find(AssetKey))
			{
				ResolvedImage = Cast<UTexture2D>(ScriptImagePtr->LoadSynchronous());
			}
		}
	}

	// 3. Try project's global ResolvedAssets
	if (!ResolvedImage && Project)
	{
		if (TSoftObjectPtr<UObject>* ProjectImagePtr = Project->ResolvedAssets.Find(AssetKey))
		{
			ResolvedImage = Cast<UTexture2D>(ProjectImagePtr->LoadSynchronous());
		}
	}

	// Fall back to cached texture for cross-script resolution
	if (!ResolvedImage && CharDef && CharDef->CachedImage)
	{
		ResolvedImage = CharDef->CachedImage;
	}

	return ResolvedImage;
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
// Data Asset Variable Access (typed, with asset picker)
// ============================================================================

namespace
{
	/**
	 * Can a declaration of `DeclaredType` be reached through the `ExpectedType` accessor?
	 *
	 * Exact, except that the STRING accessor also serves image / character / audio: those three
	 * are declared distinctly in the editor but hold a plain string at runtime (the importer
	 * stores all three through FStoryFlowVariant::SetString), so reading one as a string is not a
	 * coercion — it is the value. Enum is NOT in that set: it carries its own type tag, and
	 * folding it in here would make a Blueprint write land in the overlay typed String while the
	 * file value it shadows is typed Enum — invisible to a read, visible in the save key.
	 *
	 * NOT the same set as StoryFlowEngineContract::IsStringFamily (the test fixtures' helper),
	 * which has FIVE members because it answers a different question: it asks what STORAGE a
	 * value ended up in, where Enum does live in StringValue alongside the other four. This one
	 * asks which DECLARATION an accessor may reach, and Enum has its own accessor. The two must
	 * not be merged: doing it would either open enum writes to the string setter (above) or shut
	 * the fixture comparator out of every enum value it checks.
	 */
	bool DataAssetAccessorTypeMatches(EStoryFlowVariableType DeclaredType, EStoryFlowVariableType ExpectedType)
	{
		if (ExpectedType == EStoryFlowVariableType::String)
		{
			return DeclaredType == EStoryFlowVariableType::String
				|| DeclaredType == EStoryFlowVariableType::Image
				|| DeclaredType == EStoryFlowVariableType::Character
				|| DeclaredType == EStoryFlowVariableType::Audio;
		}
		return DeclaredType == ExpectedType;
	}
}

const FStoryFlowVariable* UStoryFlowComponent::FindDataAssetDeclaration(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, StoryFlowDataAssets::FStoreRef& OutStore) const
{
	OutStore = StoryFlowDataAssets::FStoreRef();

	if (!DataAsset)
	{
		// Verbose, not Warning: these accessors report failure through their return value, and a
		// Blueprint may well call one every tick (contract §6's warn-once ladder is keyed on a
		// NODE id, which a Blueprint call does not have).
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset accessor called with no asset"));
		return nullptr;
	}

	UStoryFlowSubsystem* Subsystem = GetStoryFlowSubsystem();
	if (!Subsystem)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset accessor found no StoryFlow subsystem"));
		return nullptr;
	}

	OutStore = Subsystem->GetDataAssetStore();
	if (!OutStore.IsValid())
	{
		return nullptr;
	}

	// Names are resolved to ids exactly HERE, at the Blueprint boundary — everything downstream
	// (the resolver, the overlay, the save key) is keyed by id, as the contract keys the system.
	const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclarationByName(*OutStore.Seed, DataAsset->AssetId, VariableName);
	if (!Declaration)
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset '%s' declares no variable named '%s' on its chain"), *DataAsset->AssetId, *VariableName);
	}
	return Declaration;
}

const FStoryFlowVariable* UStoryFlowComponent::FindDataAssetScalarDeclaration(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
	EStoryFlowVariableType ExpectedType, StoryFlowDataAssets::FStoreRef& OutStore) const
{
	const FStoryFlowVariable* Declaration = FindDataAssetDeclaration(DataAsset, VariableName, OutStore);
	if (!Declaration)
	{
		return nullptr;
	}

	// NO SILENT COERCION (contract §6.1's rule, applied at this surface): an array read through a
	// scalar accessor, or an integer read as a float, reports not-found rather than converting.
	// Arrays and maps travel through GetDataAssetVariantVariable and the variant library.
	if (Declaration->bIsArray || !DataAssetAccessorTypeMatches(Declaration->Type, ExpectedType))
	{
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset variable '%s' is declared as a different type than the accessor reading it"), *VariableName);
		return nullptr;
	}
	return Declaration;
}

bool UStoryFlowComponent::SetDataAssetScalar(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
	EStoryFlowVariableType ExpectedType, const FStoryFlowVariant& Value)
{
	StoryFlowDataAssets::FStoreRef Store;
	const FStoryFlowVariable* Declaration = FindDataAssetScalarDeclaration(DataAsset, VariableName, ExpectedType, Store);
	if (!Declaration)
	{
		return false;
	}

	// The write lands at THE REFERENCED ASSET'S OWN LEVEL, always (contract §5) — the same store
	// call the Set node makes, so a Blueprint write cascades to descendants exactly as a scripted
	// one does and rides the next save the same way.
	const bool bWritten = StoryFlowDataAssets::TrySet(*Store.Seed, *Store.Overlay, DataAsset->AssetId, Declaration->Id, Value);

	// ...and it invalidates the same caches the Set node's arm does (contract §5). The accessor
	// nodes are never memoized themselves, but the notBool / andBool / comparison ABOVE one is,
	// and nothing else drops that memo for a write made from Blueprint: BuildDialogueState via
	// NotifyVariableChanged rebuilds the dialogue WITHOUT clearing first, so a stale condition
	// would survive the rebuild and keep an option hidden that the write just opened.
	//
	// Component-local, like every other clear here. A write is subsystem-wide, so another
	// component mid-dialogue re-evaluates on its own next dialogue rebuild rather than instantly
	// — the same asymmetry global variables have always had. See the note on FStoreRef.
	if (bWritten && Evaluator)
	{
		Evaluator->ClearCache();
	}
	return bWritten;
}

bool UStoryFlowComponent::TryGetDataAssetScalar(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
	EStoryFlowVariableType ExpectedType, FStoryFlowVariant& OutValue) const
{
	StoryFlowDataAssets::FStoreRef Store;
	const FStoryFlowVariable* Declaration = FindDataAssetScalarDeclaration(DataAsset, VariableName, ExpectedType, Store);
	if (!Declaration)
	{
		return false;
	}

	// Through the RESOLVER, never a cached copy: chain defaults, ancestor overrides and this
	// session's writes all have to be visible here (contract §4).
	return StoryFlowDataAssets::TryResolve(*Store.Seed, *Store.Overlay, DataAsset->AssetId, Declaration->Id, OutValue);
}

bool UStoryFlowComponent::GetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = TryGetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Boolean, Value);
	return bFound ? Value.GetBool() : false;
}

bool UStoryFlowComponent::SetDataAssetBoolVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool bValue)
{
	FStoryFlowVariant Value;
	Value.SetBool(bValue);
	return SetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Boolean, Value);
}

int32 UStoryFlowComponent::GetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = TryGetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Integer, Value);
	return bFound ? Value.GetInt() : 0;
}

bool UStoryFlowComponent::SetDataAssetIntVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, int32 Value)
{
	FStoryFlowVariant NewValue;
	NewValue.SetInt(Value);
	return SetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Integer, NewValue);
}

float UStoryFlowComponent::GetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = TryGetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Float, Value);
	return bFound ? Value.GetFloat() : 0.0f;
}

bool UStoryFlowComponent::SetDataAssetFloatVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, float Value)
{
	FStoryFlowVariant NewValue;
	NewValue.SetFloat(Value);
	return SetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Float, NewValue);
}

FString UStoryFlowComponent::GetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = TryGetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::String, Value);
	return bFound ? Value.GetString() : FString();
}

bool UStoryFlowComponent::SetDataAssetStringVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value)
{
	FStoryFlowVariant NewValue;
	NewValue.SetString(Value);
	return SetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::String, NewValue);
}

FString UStoryFlowComponent::GetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	FStoryFlowVariant Value;
	bFound = TryGetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Enum, Value);
	return bFound ? Value.GetString() : FString();
}

bool UStoryFlowComponent::SetDataAssetEnumVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, const FString& Value)
{
	FStoryFlowVariant NewValue;
	// SetEnum, not SetString: the seed types an enum declaration's value as Enum, and an overlay
	// entry that differed would be invisible to a read and visible in the save key.
	NewValue.SetEnum(Value);
	return SetDataAssetScalar(DataAsset, VariableName, EStoryFlowVariableType::Enum, NewValue);
}

FStoryFlowVariant UStoryFlowComponent::GetDataAssetVariantVariable(UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, bool& bFound)
{
	bFound = false;
	StoryFlowDataAssets::FStoreRef Store;
	// No type gate here: this IS the untyped accessor, and the caller picks the value apart with
	// UStoryFlowVariantLibrary. Arrays and maps have no other Blueprint path.
	const FStoryFlowVariable* Declaration = FindDataAssetDeclaration(DataAsset, VariableName, Store);
	if (!Declaration)
	{
		return FStoryFlowVariant();
	}

	FStoryFlowVariant Value;
	bFound = StoryFlowDataAssets::TryResolve(*Store.Seed, *Store.Overlay, DataAsset->AssetId, Declaration->Id, Value);
	// TryResolve copies out with map storage detached, so what a Blueprint gets can be held or
	// mutated without reaching into the store (contract §3).
	return bFound ? Value : FStoryFlowVariant();
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
		DeepCopyMapVariables(ExecutionContext.LocalVariables); // detach shared map storage from the asset
		// Resolve string-table keys to localized text, exactly like Initialize does at
		// every other asset->runtime copy (after the detach, so resolution mutates the
		// runtime copy and never the asset). Without this, string variables display raw
		// keys like "var_3_value" after a reset.
		ExecutionContext.ResolveStringVariableValues(ExecutionContext.LocalVariables);
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

	// Trace parity: the HTML runtime never processes start nodes — every entry
	// point (initial load, runScript, flows) follows the edge out of "0" and
	// processes its TARGET directly, so HTML traces never contain a start hop.
	// UE routes through the start node; suppress its NODE line (and the matching
	// EDGE line in ProcessNextNode) so traces diff 1:1 against the editor's
	// map-trace-fixture snapshot.
	if (Node->Type != EStoryFlowNodeType::Start)
	{
		SF_TRACE(ExecutionContext, "NODE %s %s", *Node->Id, *Node->TypeString);
	}

	ExecutionContext.CurrentNodeId = Node->Id;

	const auto& Table = GetDispatchTable();
	if (const FNodeHandler* Handler = Table.Find(Node->Type))
	{
		(this->**Handler)(Node);
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Unsupported node type '%s' at node %s, skipping"), *Node->TypeString, *Node->Id);
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
			EStoryFlowNodeType::Modulo,
			EStoryFlowNodeType::Random,
			EStoryFlowNodeType::GreaterThanFloat, EStoryFlowNodeType::GreaterThanOrEqualFloat,
			EStoryFlowNodeType::LessThanFloat, EStoryFlowNodeType::LessThanOrEqualFloat,
			EStoryFlowNodeType::EqualFloat,
			EStoryFlowNodeType::PlusFloat, EStoryFlowNodeType::MinusFloat,
			EStoryFlowNodeType::MultiplyFloat, EStoryFlowNodeType::DivideFloat,
			EStoryFlowNodeType::ModuloFloat,
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

		// Data Asset (.sfd) handlers. The reference pill and the Get accessor are PURE: neither
		// carries an exec pin at all, so the exec walk can never legally arrive at one. Both are
		// registered anyway (as the ordinary logic-node continuation) so a hand-edited or
		// future-migrated graph that DOES wire one into an exec chain flows on quietly instead of
		// tripping ProcessNode's "Unknown node type" branch and bricking the session on an error.
		T.Add(EStoryFlowNodeType::GetDataAsset,          LogicHandler);
		T.Add(EStoryFlowNodeType::GetDataAssetVariable,  LogicHandler);
		T.Add(EStoryFlowNodeType::SetDataAssetVariable,  &UStoryFlowComponent::HandleSetDataAssetVariable);

		// Map variable handlers
		T.Add(EStoryFlowNodeType::SetMap,        &UStoryFlowComponent::HandleSetMap);
		T.Add(EStoryFlowNodeType::SetMapValue,   &UStoryFlowComponent::HandleMapModify);
		T.Add(EStoryFlowNodeType::RemoveMapKey,  &UStoryFlowComponent::HandleMapModify);
		T.Add(EStoryFlowNodeType::ClearMap,      &UStoryFlowComponent::HandleMapModify);

		// Map pure reads (evaluated lazily on data pull; handler only routes exec)
		T.Add(EStoryFlowNodeType::GetMap,       &UStoryFlowComponent::HandleMapPureNode);
		T.Add(EStoryFlowNodeType::GetMapValue,  &UStoryFlowComponent::HandleMapPureNode);
		T.Add(EStoryFlowNodeType::HasMapKey,    &UStoryFlowComponent::HandleMapPureNode);
		T.Add(EStoryFlowNodeType::MapSize,      &UStoryFlowComponent::HandleMapPureNode);
		T.Add(EStoryFlowNodeType::MapKeys,      &UStoryFlowComponent::HandleMapPureNode);
		T.Add(EStoryFlowNodeType::MapValues,    &UStoryFlowComponent::HandleMapPureNode);

		// Map entry iteration (snapshot-at-init semantics — see HandleForEachMap)
		T.Add(EStoryFlowNodeType::ForEachMap, &UStoryFlowComponent::HandleForEachMap);

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

	// Trace parity: suppress the edge OUT of a start node — the HTML runtime
	// follows it without tracing (see the matching NODE gate in ProcessNode).
	{
		FStoryFlowNode* EdgeSourceNode = ExecutionContext.GetNode(Edge->Source);
		if (!EdgeSourceNode || EdgeSourceNode->Type != EStoryFlowNodeType::Start)
		{
			SF_TRACE(ExecutionContext, "EDGE %s:%s -> %s", *Edge->Source, *Edge->SourceHandle, *Edge->Target);
		}
	}

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
		TMap<FString, FStoryFlowVariable> MapOutputVariables;
		for (const auto& VarPair : ExecutionContext.LocalVariables)
		{
			if (!VarPair.Value.bIsOutput)
			{
				continue;
			}
			if (VarPair.Value.Type == EStoryFlowVariableType::Map)
			{
				// Map outputs DETACH (a plain variant copy would share the dead
				// invocation's TSharedPtr storage) — the HTML runtime converts
				// _outputValues to a fresh Map at the read site, so the boundary
				// is observably a snapshot. Stored as full variables so the map
				// resolver can hand out stable storage with type metadata.
				FStoryFlowVariable OutVar = VarPair.Value;
				OutVar.Value.DeepCopyMap();
				MapOutputVariables.Add(OutVar.Name, MoveTemp(OutVar));
			}
			else
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
			if (OutputValues.Num() > 0 || MapOutputVariables.Num() > 0)
			{
				FNodeRuntimeState& RSState = ExecutionContext.GetNodeState(Frame.ReturnNodeId);
				RSState.OutputValues = MoveTemp(OutputValues);
				RSState.MapOutputVariables = MoveTemp(MapOutputVariables);
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

	// Snapshot the entered node's tags from the built state before broadcasting. A re-entrant
	// Blueprint handler may synchronously advance/select/stop/restart mid-loop; the snapshot
	// contract fires the entered node's tags fully regardless (interleaving with the next node's
	// tags is accepted). Reading the built state also keeps us off the raw Node* the broadcast
	// handlers can invalidate.
	const TArray<FString> TagsToFire = ExecutionContext.CurrentDialogueState.Tags;

	// Broadcast update
	UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Broadcasting OnDialogueUpdated"));
	OnDialogueUpdated.Broadcast(ExecutionContext.CurrentDialogueState);

	// Fire one tag event per tag, in authored order — only on fresh entry so re-renders
	// (returning from a Set* node, live re-interpolation) never re-fire the same line's tags.
	// Fired after the dialogue state is applied/broadcast so handlers can touch the presented UI.
	if (bIsFreshEntry)
	{
		for (const FString& Tag : TagsToFire)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Broadcasting OnDialogueTagReached '%s'"), *Tag);
			OnDialogueTagReached.Broadcast(Tag);
		}
	}
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
				else if (Param.Type == TEXT("map"))
				{
					// Map parameters use the K/V-bearing handle
					// "map-{keyType}-{valueType}-param-{id}", mirroring the editor's
					// buildMapHandleId and the HTML runtime. The editor renders the handle
					// from the param's declared key/value types, defaulting to "string" when
					// absent, so match that fallback below. (A plain "map-param-{id}" never
					// matches the wired edge, so the map param would silently pass nothing.)
					// Maps cross the call boundary BY VALUE: SetMap allocates fresh
					// storage for the entries, so the callee's variable never aliases the
					// caller's (and entry values are scalar, so the copy is a full
					// snapshot). Wired-but-unresolved passes an empty map (the eventual
					// HTML getMapInput empty-Map fallback).
					const FString MapKeyType = Param.KeyType.IsEmpty() ? TEXT("string") : Param.KeyType;
					const FString MapValueType = Param.ValueType.IsEmpty() ? TEXT("string") : Param.ValueType;
					const FString MapHandleSuffix = StoryFlowHandles::In_Map(MapKeyType, MapValueType, FString(TEXT("param-")) + Param.Id);
					if (ExecutionContext.FindInputEdge(Node->Id, MapHandleSuffix))
					{
						TArray<FStoryFlowMapEntry> Entries;
						if (FStoryFlowVariable* SourceVar = Evaluator->ResolveMapInputVariableByHandle(Node, MapHandleSuffix))
						{
							Entries = SourceVar->Value.GetMap();
						}
						FStoryFlowVariant MapVariant;
						MapVariant.SetMap(Entries);
						ParamValues.Add(Param.Name, MoveTemp(MapVariant));
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
	// Data node - just continue. No VAR GET trace here: the HTML runtime emits
	// VAR GET on data-pull EVALUATION of get/set variable nodes (see the
	// evaluator's Get*/Set* arms), never when a get node sits in the exec chain.
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
	// No VAR GET trace — see HandleGetBool
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
	// No VAR GET trace — see HandleGetBool
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
	// No VAR GET trace — see HandleGetBool
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
	// No VAR GET trace — see HandleGetBool
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

/** The element-type token ("boolean", "integer", ...) a Set Array Element node's pins are built from. */
static FString SetArrayElementTypeToken(EStoryFlowNodeType Type)
{
	switch (Type)
	{
	case EStoryFlowNodeType::SetBoolArrayElement:      return TEXT("boolean");
	case EStoryFlowNodeType::SetIntArrayElement:       return TEXT("integer");
	case EStoryFlowNodeType::SetFloatArrayElement:     return TEXT("float");
	case EStoryFlowNodeType::SetStringArrayElement:    return TEXT("string");
	case EStoryFlowNodeType::SetImageArrayElement:     return TEXT("image");
	case EStoryFlowNodeType::SetCharacterArrayElement: return TEXT("character");
	case EStoryFlowNodeType::SetAudioArrayElement:     return TEXT("audio");
	default:                                           return FString();
	}
}

void UStoryFlowComponent::HandleArraySetElement(FStoryFlowNode* Node)
{
	// EDGE-FIRST, like HandleArrayModify — and for this node type it is the ONLY way that works.
	//
	// The export writes a set*ArrayElement node as { type, id, value1, value2 }: no `variable`
	// field at all (json-export-strategy's baseNode is type+id, and only the WHOLE-array
	// set*Array cases add one). The importer fills Data.Variable from that field and nothing
	// back-fills it, so the old lookup at the top of HandleArraySet could never find a variable
	// and every Set Array Element node in every project was a warn-and-bail no-op, whatever it
	// was wired to. The reference runtime never had a variable to look up either: its
	// setArrayElement reads the array off the `<type>-array-2` edge and writes the result back
	// through updateConnectedArrayVariable, which dispatches on the SOURCE node.
	const FString FlowHandle = StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow);
	const FString ElementType = SetArrayElementTypeToken(Node->Type);
	if (!Evaluator || ElementType.IsEmpty())
	{
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	const FString ArraySuffix = ElementType + TEXT("-array-2");
	FStoryFlowNode* Source = nullptr;
	if (const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, ArraySuffix))
	{
		Source = ExecutionContext.GetNode(Edge->Source);
	}
	if (!Source)
	{
		// Nothing wired to the array pin: the reference's updateConnectedArrayVariable returns
		// early on a missing edge, so this is a silent no-op there too. Verbose, not Warning —
		// an unwired op is an authoring state, not a runtime fault.
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Set Array Element node %s has nothing wired to its array input - no write"), *Node->Id);
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	// Read the array through the same typed readers the reference's getArrayInput uses. What
	// comes back is a COPY in every branch (script variable, character variable, `.sfd` store),
	// so the mutation below cannot reach any of them without the write-back.
	TArray<FStoryFlowVariant> Arr = EvaluateTypedArrayInput(Node, ElementType, ArraySuffix);

	// Index "integer-3" and value "<type>-4". The export dialect renames the inline fallbacks:
	// the .sfe "index" is exported as "value1" and "value" as "value2" on set*ArrayElement;
	// add/remove ops use plain "value".
	const int32 Index = Evaluator->EvaluateIntegerInput(Node, TEXT("integer-3"), Node->Data.Value1.GetInt(0));

	FStoryFlowVariant NewValue;
	switch (Node->Type)
	{
	case EStoryFlowNodeType::SetBoolArrayElement:
		NewValue.SetBool(Evaluator->EvaluateBooleanInput(Node, TEXT("boolean-4"), Node->Data.Value2.GetBool(false)));
		break;
	case EStoryFlowNodeType::SetIntArrayElement:
		NewValue.SetInt(Evaluator->EvaluateIntegerInput(Node, TEXT("integer-4"), Node->Data.Value2.GetInt(0)));
		break;
	case EStoryFlowNodeType::SetFloatArrayElement:
		NewValue.SetFloat(Evaluator->EvaluateFloatInput(Node, TEXT("float-4"), Node->Data.Value2.GetFloat(0.0f)));
		break;
	case EStoryFlowNodeType::SetStringArrayElement:
	{
		const FString ResolvedFallback = ExecutionContext.GetString(Node->Data.Value2.GetString(), LanguageCode);
		NewValue.SetString(Evaluator->EvaluateStringInput(Node, TEXT("string-4"), ResolvedFallback));
		break;
	}
	case EStoryFlowNodeType::SetImageArrayElement:
		NewValue.SetString(Evaluator->EvaluateStringInput(Node, TEXT("image-4"), Node->Data.Value2.GetString()));
		break;
	case EStoryFlowNodeType::SetCharacterArrayElement:
		NewValue.SetString(Evaluator->EvaluateStringInput(Node, TEXT("character-4"), Node->Data.Value2.GetString()));
		break;
	case EStoryFlowNodeType::SetAudioArrayElement:
		NewValue.SetString(Evaluator->EvaluateStringInput(Node, TEXT("audio-4"), Node->Data.Value2.GetString()));
		break;
	default:
		break;
	}

	if (Index >= 0 && Index < Arr.Num())
	{
		Arr[Index] = NewValue;
	}
	// An out-of-range index still writes the (unchanged) array back, exactly as the reference
	// does — updateConnectedArrayVariable is called unconditionally there. Keeping the call
	// rather than the effect is what makes the two runtimes agree about a `.sfd` target, where
	// the write-back is observable as an overlay entry even when no element moved.

	// --- write back, dispatching on the SOURCE node (the reference's updateConnectedArrayVariable) ---

	if (FStoryFlowEvaluator::IsDataAssetAccessor(Source->Type))
	{
		// Same ladder and same refusal as HandleArrayModify's `.sfd` branch. An accessor carries
		// no isGlobal and its Data.Variable is the `.sfd` variable's display NAME, so the script
		// lookup at the tail would clobber a same-named LOCAL array instead.
		if (!Source->Data.bIsArray)
		{
			ExecutionContext.MaybeWarnDataAsset(Source->Id, TEXT("arrayop"),
				FString::Printf(TEXT("Data Asset array op refused: node %s is not bound to an array variable"), *Source->Id));
			HandleSetNodeEnd(Node, FlowHandle);
			return;
		}

		FString AssetId;
		if (ExecutionContext.TryResolveDataAssetBinding(*Source, AssetId))
		{
			// The element type is STATED, not inferred: an emptied array leaves the plain
			// SetArray typed None, and the ladder above has just proved DeclMatches, so the
			// snapshot type IS the chain's declared type.
			FStoryFlowVariant Written;
			Written.SetArray(Arr, ParseVariableType(Source->Data.VariableType));
			SF_TRACE(ExecutionContext, "DA SET \"%s.%s\" size=%d", *AssetId, *Source->Data.VariableId, Arr.Num());
			ExecutionContext.TrySetDataAsset(AssetId, Source->Data.VariableId, Written);
			// Boolean producers built on this array (arrayLength / arrayContains) memoize, so the
			// write has to drop the evaluation cache — the same clearNotBoolCache the reference
			// performs here and the same drop HandleArrayModify makes.
			Evaluator->ClearCache();
		}
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	if ((Source->Type == EStoryFlowNodeType::GetCharacterVar || Source->Type == EStoryFlowNodeType::SetCharacterVar)
		&& Source->Data.bIsArray)
	{
		// The character path may itself arrive on a pin, so resolve it the way the array READER
		// does rather than trusting the embedded path alone.
		FString CharPath = Source->Data.CharacterPath;
		if (const FStoryFlowConnection* CharEdge = ExecutionContext.FindInputEdge(Source->Id, StoryFlowHandles::In_CharacterInput))
		{
			if (FStoryFlowNode* CharNode = ExecutionContext.GetNode(CharEdge->Source))
			{
				CharPath = Evaluator->EvaluateStringFromNode(CharNode, Source->Id, CharEdge->SourceHandle);
			}
		}
		FStoryFlowVariant Written;
		Written.SetArray(Arr, ParseVariableType(Source->Data.VariableType));
		ExecutionContext.SetCharacterVariable(CharPath, Source->Data.VariableName, Written);
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	// Otherwise the source node's OWN variable, local or global as it declares itself.
	FStoryFlowVariable* Var = Source->Data.Variable.IsEmpty()
		? nullptr
		: ExecutionContext.FindVariable(Source->Data.Variable, Source->Data.bIsGlobal);
	if (!Var)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Set Array Element node %s could not resolve the array its input is wired to"), *Node->Id);
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	Var->Value.SetArray(Arr, Var->Type);
	SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=[array]", *Var->Name, Source->Data.bIsGlobal ? TEXT("true") : TEXT("false"));
	NotifyVariableChanged(*Var, Source->Data.bIsGlobal);
	HandleSetNodeEnd(Node, FlowHandle);
}

void UStoryFlowComponent::HandleArraySet(FStoryFlowNode* Node)
{
	// Set element at index: an entirely different resolution path (see HandleArraySetElement),
	// and it must run BEFORE the variable lookup below, which these nodes can never satisfy.
	switch (Node->Type)
	{
	case EStoryFlowNodeType::SetBoolArrayElement:
	case EStoryFlowNodeType::SetIntArrayElement:
	case EStoryFlowNodeType::SetFloatArrayElement:
	case EStoryFlowNodeType::SetStringArrayElement:
	case EStoryFlowNodeType::SetImageArrayElement:
	case EStoryFlowNodeType::SetCharacterArrayElement:
	case EStoryFlowNodeType::SetAudioArrayElement:
		HandleArraySetElement(Node);
		return;
	default:
		break;
	}

	// Set the whole array variable. This one DOES carry a `variable` field in the export, so the
	// name lookup is the right resolution for it.
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
	if (!Var)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: HandleArraySet - variable '%s' not found"), *Node->Data.Variable);
		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
		return;
	}

	if (Evaluator)
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

	// Resolve the array input edge FIRST and ask what is on the far end, because a `.sfd`
	// accessor there outranks EVERY name-based lookup — including this node's own Data.Variable.
	// That is the reference's guard shape (updateConnectedArrayVariable / clearArray both test
	// the source node's type immediately after resolving the edge, ahead of any name lookup).
	// Array modify nodes carry no variable field today, so ordering them the other way round
	// happens to behave identically — but only until an exporter emits one, at which point
	// Unreal would write a script variable where the reference writes the store.
	// The `Evaluator &&` guard is the old edge-based fallback's, kept verbatim: without an
	// evaluator none of the ops below can read their inputs anyway, so discovering a target here
	// would only produce a half-applied one.
	FStoryFlowNode* ArrayInputSource = nullptr;
	if (Evaluator && !ArrayHandleSuffix.IsEmpty())
	{
		if (const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, ArrayHandleSuffix))
		{
			ArrayInputSource = ExecutionContext.GetNode(Edge->Source);
		}
	}

	FStoryFlowVariable* Var = nullptr;
	FStoryFlowVariable DataAssetScratch;
	FStoryFlowNode* DataAssetAccessor = nullptr;
	FString DataAssetId;

	if (ArrayInputSource && FStoryFlowEvaluator::IsDataAssetAccessor(ArrayInputSource->Type))
	{
		// The whole op routes INTO the session overlay: the array is read out of the store (a
		// copy), mutated below like any other, and written back through the same guarded path
		// the Set node uses. An accessor carries no isGlobal and its Data.Variable is the `.sfd`
		// variable's display NAME, so a fall-through to a name lookup would silently clobber a
		// same-named LOCAL script array — which is why nothing below this branch runs for one.
		//
		// A bound-but-not-array accessor keeps the refusal: its pins could not have fed this op
		// an array, and writing one over a scalar the declaration promises is exactly what the
		// store's callers must never do. Var stays null and the miss below ends the node.
		if (!ArrayInputSource->Data.bIsArray)
		{
			ExecutionContext.MaybeWarnDataAsset(ArrayInputSource->Id, TEXT("arrayop"),
				FString::Printf(TEXT("Data Asset array op refused: node %s is not bound to an array variable"), *ArrayInputSource->Id));
		}
		else if (ExecutionContext.TryResolveDataAssetBinding(*ArrayInputSource, DataAssetId))
		{
			FStoryFlowVariant Resolved;
			if (ExecutionContext.TryResolveDataAsset(DataAssetId, ArrayInputSource->Data.VariableId, Resolved))
			{
				// The ELEMENT TYPE is stated, not inferred: an empty resolved array leaves the
				// plain SetArray typed None, so a `.sfd` array would read back typed or untyped
				// purely by whether the last writer emptied it. The accessor's snapshot is the
				// right source for it — the ladder above has just proved DeclMatches, so the
				// snapshot type IS the chain's declared type.
				const EStoryFlowVariableType ElementType = ParseVariableType(ArrayInputSource->Data.VariableType);
				DataAssetScratch.Id = ArrayInputSource->Data.VariableId;
				DataAssetScratch.Name = ArrayInputSource->Data.VariableName;
				DataAssetScratch.Type = ElementType;
				DataAssetScratch.bIsArray = true;
				DataAssetScratch.Value.SetArray(Resolved.GetArray(), ElementType);
				DataAssetAccessor = ArrayInputSource;
				Var = &DataAssetScratch;
			}
		}
	}
	else
	{
		// Try this node's direct variable reference first, then fall back to the edge source's.
		// Array modify nodes in the HTML runtime don't have a variable field — they discover the
		// target array through the input edge (matching getArrayInput + updateConnectedArrayVariable).
		if (!Node->Data.Variable.IsEmpty())
		{
			Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
		}
		if (!Var && ArrayInputSource && !ArrayInputSource->Data.Variable.IsEmpty())
		{
			Var = ExecutionContext.FindVariable(ArrayInputSource->Data.Variable, ArrayInputSource->Data.bIsGlobal);
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
	// Add operations (editor handles: array "<type>-array-2", value "<type>-3")
	case EStoryFlowNodeType::AddToBoolArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetBool(Evaluator ? Evaluator->EvaluateBooleanInput(Node, TEXT("boolean-3"), Node->Data.Value.GetBool(false)) : Node->Data.Value.GetBool(false));
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToIntArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetInt(Evaluator ? Evaluator->EvaluateIntegerInput(Node, TEXT("integer-3"), Node->Data.Value.GetInt(0)) : Node->Data.Value.GetInt(0));
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToFloatArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetFloat(Evaluator ? Evaluator->EvaluateFloatInput(Node, TEXT("float-3"), Node->Data.Value.GetFloat(0.0f)) : Node->Data.Value.GetFloat(0.0f));
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToStringArray:
	{
		FStoryFlowVariant Elem;
		FString ResolvedFallback = ExecutionContext.GetString(Node->Data.Value.GetString(), LanguageCode);
		FString EvalResult = Evaluator ? Evaluator->EvaluateStringInput(Node, TEXT("string-3"), ResolvedFallback) : ResolvedFallback;
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
	{
		FStoryFlowVariant Elem;
		Elem.SetString(Evaluator ? Evaluator->EvaluateStringInput(Node, TEXT("image-3"), Node->Data.Value.GetString()) : Node->Data.Value.GetString());
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToCharacterArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetString(Evaluator ? Evaluator->EvaluateStringInput(Node, TEXT("character-3"), Node->Data.Value.GetString()) : Node->Data.Value.GetString());
		Arr.Add(Elem);
		break;
	}
	case EStoryFlowNodeType::AddToAudioArray:
	{
		FStoryFlowVariant Elem;
		Elem.SetString(Evaluator ? Evaluator->EvaluateStringInput(Node, TEXT("audio-3"), Node->Data.Value.GetString()) : Node->Data.Value.GetString());
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

	// A `.sfd` target is not a script variable: it gets the store write and the evaluation-cache
	// drop (arrayLength / arrayContains producers feed boolean chains) instead of the variable
	// trace and the variable-changed broadcast, which name a script/global scope it has none of.
	//
	// ORDER IS LOAD-BEARING: the drop lands BEFORE this node stamps its own output below.
	// ClearCache is a full evaluation-cache drop, wider than the reference's clearNotBoolCache
	// (which drops only the notBool memo and leaves node outputs alone) - so running it after the
	// stamp wiped the stamp, leaving anything wired to this op's OUTPUT pin reading an empty
	// array, and only ever when the target was a `.sfd` accessor.
	if (DataAssetAccessor)
	{
		SF_TRACE(ExecutionContext, "DA SET \"%s.%s\" size=%d", *DataAssetId, *DataAssetAccessor->Data.VariableId, Arr.Num());
		ExecutionContext.TrySetDataAsset(DataAssetId, DataAssetAccessor->Data.VariableId, Var->Value);
		if (Evaluator)
		{
			Evaluator->ClearCache();
		}
	}

	// Store result array in CachedOutput so downstream nodes connected to this
	// node's output can read it (matches HTML's setNodeOutputValue pattern)
	FNodeRuntimeState& ArrayNodeState = ExecutionContext.GetNodeState(Node->Id);
	ArrayNodeState.CachedOutput.SetArray(Arr);
	ArrayNodeState.bHasCachedOutput = true;

	if (DataAssetAccessor)
	{
		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
		return;
	}

	bool bVarIsGlobal = !ExecutionContext.LocalVariables.Contains(Var->Id);
	SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s value=[array]", *Var->Name, bVarIsGlobal ? TEXT("true") : TEXT("false"));
	NotifyVariableChanged(*Var, bVarIsGlobal);
	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleSetMap(FStoryFlowNode* Node)
{
	// Mirrors the HTML runtime's setMap → updateMapVariable (runtime-variables.js):
	// resolve the wired map input ("2") and ALIAS the bound variable's storage to
	// the origin variable's live storage — HTML assigns the _runtimeMap REFERENCE,
	// so after setMap(b ← chain from getMap(a)) a later clearMap(a) also empties b.
	// Copy-on-set would break the cross-runtime aliasing pin.
	FStoryFlowVariable* Var = ExecutionContext.FindVariable(Node->Data.Variable, Node->Data.bIsGlobal);
	if (!Var || Var->Type != EStoryFlowVariableType::Map)
	{
		// HTML returns early without trace/dispatch but still continues exec
		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
		return;
	}

	// Missing K/V types: the map input handle cannot be built — HTML behaves as
	// disconnected (keeps the current value, still traces and dispatches).
	if (Evaluator && !Node->Data.KeyType.IsEmpty() && !Node->Data.ValueType.IsEmpty())
	{
		const FString HandleSuffix = StoryFlowHandles::In_Map(Node->Data.KeyType, Node->Data.ValueType, TEXT("2"));
		if (ExecutionContext.FindInputEdge(Node->Id, HandleSuffix))
		{
			EMapSourceKind SourceKind = EMapSourceKind::Unresolved;
			if (FStoryFlowVariable* SourceVar = Evaluator->ResolveMapInputVariable(Node, TEXT("2"), &SourceKind))
			{
				if (SourceKind == EMapSourceKind::CharacterVariable || SourceKind == EMapSourceKind::RunScriptOutput ||
					SourceKind == EMapSourceKind::DataAsset)
				{
					// Read-only-terminal chain (charvar, runScript output or `.sfd`
					// accessor): HTML's setMap SNAPSHOTS the entries into fresh
					// objects — charvars get a throwaway snapshot
					// (runtime-variables.js builds a new Map from the charvar's
					// entries), runScript _outputValues are converted to a fresh Map
					// at the read site, and a `.sfd` read is a store COPY parked on
					// node state. None aliases live storage, and aliasing the `.sfd`
					// snapshot would additionally bind a script variable to scratch
					// storage the next read overwrites. SetMap allocates fresh
					// storage, and entry values are scalar (no nested maps), so this
					// copy is the full deep-copy snapshot.
					Var->Value.SetMap(SourceVar->Value.GetMap());
				}
				else
				{
					// Wired and resolved: share the origin variable's live map storage
					Var->Value.AliasMap(SourceVar->Value);
				}
			}
			else
			{
				// Wired but unresolved: HTML assigns a fresh empty Map
				Var->Value.SetMap(TArray<FStoryFlowMapEntry>());
			}
		}
		// No edge: keep the current value (maps have no inline fallback)
	}

	// Trace shape pinned by the cross-runtime fixture: size=, not value=
	SF_TRACE(ExecutionContext, "VAR SET \"%s\" global=%s size=%d", *Var->Name, Node->Data.bIsGlobal ? TEXT("true") : TEXT("false"), Var->Value.GetMap().Num());
	NotifyVariableChanged(*Var, Node->Data.bIsGlobal);
	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

void UStoryFlowComponent::HandleMapModify(FStoryFlowNode* Node)
{
	// setMapValue / removeMapKey / clearMap — one handler, three ops (mirrors the
	// HTML runtime's map mutator handlers). Mutates the ORIGIN variable's live map
	// storage in place so downstream chained ops observe the mutation, then fires
	// the variable's change notification (HTML's updateConnectedMapVariable
	// dispatch). NOTE: HTML mutators emit NO "VAR SET" trace line — only setMap
	// does (see the map-trace-fixture snapshot) — so none is emitted here.
	const FString FlowHandle = StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow);

	// Missing K/V types: HTML skips the mutation but still continues exec
	if (!Evaluator || Node->Data.KeyType.IsEmpty() || Node->Data.ValueType.IsEmpty())
	{
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	// Resolve ALL non-map inputs FIRST (pointer-lifetime rule on EvaluateMapInput):
	// key (input "3"), and for setMapValue the typed value (input "4" with the
	// inline Data.MapInlineValue fallback)
	FStoryFlowVariant Key;
	if (Node->Type != EStoryFlowNodeType::ClearMap)
	{
		Key = Evaluator->EvaluateMapOpKeyInput(Node, TEXT("3"));
	}

	FStoryFlowVariant NewValue;
	if (Node->Type == EStoryFlowNodeType::SetMapValue)
	{
		NewValue = Evaluator->EvaluateMapOpValueInput(Node, TEXT("4"));
	}

	// THEN resolve the live map (input "2") and mutate immediately
	EMapSourceKind SourceKind = EMapSourceKind::Unresolved;
	FStoryFlowVariable* Var = Evaluator->ResolveMapInputVariable(Node, TEXT("2"), &SourceKind);
	if (!Var)
	{
		// Unresolved map: HTML mutates a throwaway empty Map — no effect, no dispatch.
		// Silent no-op is HTML parity, but leave a breadcrumb for authors at Verbose.
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Map mutator node %s could not resolve its map input - mutation skipped"), *Node->Id);
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	if (SourceKind == EMapSourceKind::CharacterVariable || SourceKind == EMapSourceKind::RunScriptOutput ||
		SourceKind == EMapSourceKind::DataAsset)
	{
		// Read-only-terminal chain (charvar, runScript output or `.sfd` accessor): HTML hands the
		// mutator a THROWAWAY fresh Map — the charvar's stored variable / the
		// dead invocation's output is observably unchanged and no variable-change
		// dispatch fires. Skip the mutation AND the notify (observable no-op):
		// these sources are read-only per contract — use setCharacterVar to
		// write charvars; runScript outputs are a snapshot of a dead invocation.
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Map mutator node %s resolves to a read-only map source (character variable, runScript output or Data Asset) - mutation skipped"), *Node->Id);
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	TArray<FStoryFlowMapEntry>& Map = Var->Value.GetMapMutable();
	switch (Node->Type)
	{
	case EStoryFlowNodeType::SetMapValue:
	{
		const int32 EntryIndex = FStoryFlowEvaluator::FindMapEntryByKey(Map, Node->Data.KeyType, Key);
		if (EntryIndex != INDEX_NONE)
		{
			// Existing key: overwrite in place, keeping its position
			Map[EntryIndex].Value = NewValue;
		}
		else
		{
			// New key: append — JS Map insertion-order semantics
			FStoryFlowMapEntry Entry;
			Entry.Key = Key;
			Entry.Value = NewValue;
			Map.Add(MoveTemp(Entry));
		}
		break;
	}

	case EStoryFlowNodeType::RemoveMapKey:
	{
		const int32 EntryIndex = FStoryFlowEvaluator::FindMapEntryByKey(Map, Node->Data.KeyType, Key);
		if (EntryIndex != INDEX_NONE)
		{
			// RemoveAt preserves the order of the remaining entries
			Map.RemoveAt(EntryIndex);
		}
		break;
	}

	case EStoryFlowNodeType::ClearMap:
		Map.Empty();
		break;

	default:
		break;
	}

	NotifyVariableChanged(*Var, SourceKind == EMapSourceKind::GlobalVariable);
	HandleSetNodeEnd(Node, FlowHandle);
}

void UStoryFlowComponent::HandleMapPureNode(FStoryFlowNode* Node)
{
	// Pure map reads are evaluated lazily on data pull (map reads are never
	// memoized — see the evaluator). This handler only routes exec, mirroring
	// the HTML handlers' continuation handles: getMapValue flows on via its
	// typed value output; hasMapKey/mapSize via their typed data outputs.
	// getMap (no exec ports — HTML registers no handler) and mapKeys/mapValues
	// (HTML handlers end without processNextNode) dead-end deliberately.
	switch (Node->Type)
	{
	case EStoryFlowNodeType::GetMapValue:
	{
		const FString ValueType = Node->Data.ValueType.IsEmpty() ? TEXT("string") : Node->Data.ValueType;
		ProcessNextNode(StoryFlowHandles::Source(Node->Id, FString::Printf(TEXT("%s-value"), *ValueType)));
		break;
	}
	case EStoryFlowNodeType::HasMapKey:
		ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Boolean));
		break;
	case EStoryFlowNodeType::MapSize:
		ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Integer));
		break;
	default:
		// GetMap, MapKeys, MapValues: no exec continuation
		break;
	}
}

void UStoryFlowComponent::HandleForEachLoop(FStoryFlowNode* Node)
{
	// RESOLVE FIRST, THEN TAKE THE REFERENCE, for the reason spelled out in HandleForEachMap:
	// GetNodeState is a TMap FindOrAdd, and the array readers below can reach a first-time node
	// state — a mapKeys / mapValues source over a `.sfd` map accessor parks a snapshot on the
	// accessor's state, and the RunScript and array-modify arms touch their source's state too.
	// A rehash there would move the state this handler's loop bookkeeping is about to be written
	// through.
	const bool bAlreadyInitialized = ExecutionContext.GetNodeState(Node->Id).bLoopInitialized;

	TArray<FStoryFlowVariant> Array;
	if (!bAlreadyInitialized)
	{
		// Get array from input
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
	}

	FNodeRuntimeState& NodeState = ExecutionContext.GetNodeState(Node->Id);

	// Initialize loop on first entry
	if (!bAlreadyInitialized)
	{
		NodeState.LoopArray = MoveTemp(Array);
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

void UStoryFlowComponent::HandleForEachMap(FStoryFlowNode* Node)
{
	// Mirrors the HTML runtime's processForEachMap: iterate map entries (Key +
	// Value) in insertion order. Entries are SNAPSHOT once at loop init — body
	// mutations (even removeMapKey of the current key) land on the live map but
	// neither skip, repeat, nor extend iteration. ContinueForEachLoop re-enters
	// here via the dispatch table (it reuses LoopIndex/bLoopInitialized).

	// Missing K/V types: the map input handle cannot be built — HTML follows
	// "completed" immediately with zero iterations.
	if (Node->Data.KeyType.IsEmpty() || Node->Data.ValueType.IsEmpty())
	{
		ProcessNextNode(StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_LoopCompleted));
		return;
	}

	// RESOLVE FIRST, THEN TAKE THE REFERENCE — the ordering HandleMapModify already uses, and
	// here it is a correctness requirement, not a style. NodeRuntimeStates is a TMap and
	// GetNodeState is FindOrAdd, so ANY call that reaches a first-time node state can rehash the
	// table and move every element. EvaluateMapInput does exactly that on a `.sfd` map accessor
	// (it parks a detached snapshot on the accessor's own state, and a pill-bound accessor never
	// executes, so its state is guaranteed absent before the first read). A reference taken
	// before the call would then point into the freed block, and the loop bookkeeping below —
	// LoopEntries, LoopIndex, bLoopInitialized — would land there instead of on the live state.
	// The loop would re-initialize from entry 0 on every re-entry and never terminate.
	//
	// The flag is read BY VALUE for the same reason: it is the only thing needed before the
	// evaluator call, and a reference held across it is the whole bug.
	const bool bAlreadyInitialized = ExecutionContext.GetNodeState(Node->Id).bLoopInitialized;

	// Entries are copied immediately (pointer-lifetime rule on EvaluateMapInput — never hold the
	// live pointer; mutators realloc the storage in place)
	TArray<FStoryFlowMapEntry> Entries;
	if (!bAlreadyInitialized && Evaluator)
	{
		if (const TArray<FStoryFlowMapEntry>* Map = Evaluator->EvaluateMapInput(Node, TEXT("map")))
		{
			Entries = *Map;
		}
	}

	FNodeRuntimeState& NodeState = ExecutionContext.GetNodeState(Node->Id);

	// Initialize loop on first entry
	if (!bAlreadyInitialized)
	{
		NodeState.LoopEntries = MoveTemp(Entries);
		NodeState.LoopIndex = 0;
		NodeState.bLoopInitialized = true;
	}

	if (NodeState.LoopIndex < NodeState.LoopEntries.Num())
	{
		// Clear evaluation caches from previous iteration so boolean chains re-evaluate
		ExecutionContext.ClearEvaluationCache();

		// Restore cached outputs for all active outer ARRAY loops (nested forEach
		// support — mirrors HandleForEachLoop). Outer MAP loops need no restore:
		// their LoopKey/LoopValue live outside the evaluation cache.
		for (const FStoryFlowLoopContext& Frame : ExecutionContext.LoopStack)
		{
			FNodeRuntimeState& OuterState = ExecutionContext.GetNodeState(Frame.NodeId);
			if (OuterState.bLoopInitialized && OuterState.LoopIndex < OuterState.LoopArray.Num())
			{
				OuterState.CachedOutput = OuterState.LoopArray[OuterState.LoopIndex];
				OuterState.bHasCachedOutput = true;
			}
		}

		// Expose the current entry's Key/Value (read by the typed evaluators
		// via the "-key"/"-value" source handle suffixes)
		const FStoryFlowMapEntry& Entry = NodeState.LoopEntries[NodeState.LoopIndex];
		NodeState.LoopKey = Entry.Key;
		NodeState.LoopValue = Entry.Value;

		SF_TRACE(ExecutionContext, "LOOP %s index=%d key=%s value=%s", *Node->Id, NodeState.LoopIndex, *NodeState.LoopKey.ToString(), *NodeState.LoopValue.ToString());

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
		NodeState.LoopEntries.Empty();
		NodeState.LoopKey.Reset();
		NodeState.LoopValue.Reset();
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

	// Map-typed character variables take a dedicated path: resolve the wired map
	// input and SNAPSHOT it into the character's own storage. HTML parity
	// (updateCharacterVariable → setCharacterVariableValue): the character stores
	// the serialized entry-array form — an independent copy, never an alias of
	// the source variable's live map (map-character.test.ts pins this).
	if (VariableType == TEXT("map"))
	{
		// Missing K/V types: the map input handle cannot be built — HTML
		// short-circuits with NO write (and no trace), but exec still continues.
		if (Node->Data.KeyType.IsEmpty() || Node->Data.ValueType.IsEmpty())
		{
			HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
			return;
		}

		// Resolve the wired map input (optionId "input": target-{id}-map-{K}-{V}-input,
		// see SecondaryMapHandle in SetCharacterVariableNode) and copy its entries
		// immediately (pointer-lifetime rule on EvaluateMapInput). Unwired or
		// unresolved → empty (HTML defaults the new value to a fresh Map).
		TArray<FStoryFlowMapEntry> NewEntries;
		if (Evaluator)
		{
			if (const TArray<FStoryFlowMapEntry>* SourceMap = Evaluator->EvaluateMapInput(Node, TEXT("input")))
			{
				NewEntries = *SourceMap;
			}
		}

		// Trace shape matches the map pin on HandleSetMap (size=, not value=).
		// HTML traces before the write check — trace-then-gate order is parity,
		// don't reorder.
		SF_TRACE(ExecutionContext, "VAR SET \"%s.%s\" global=false size=%d", *CharacterPath, *VariableName, NewEntries.Num());

		// Write only when the variable exists and is map-typed (HTML's
		// setCharacterVariableValue type-mismatch → false, no write). Name/Image
		// built-ins are never map-typed, so the custom-variable lookup suffices.
		FStoryFlowVariable* CharVar = ExecutionContext.FindCharacterVariable(CharacterPath, VariableName);
		if (CharVar && CharVar->Type == EStoryFlowVariableType::Map)
		{
			CharVar->Value.SetMap(NewEntries); // fresh storage — never aliases the source
			OnCharacterVariableChanged.Broadcast(CharacterPath, VariableName, CharVar->Value);
		}
		else
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: SetCharacterVar map write skipped - variable '%s' on '%s' missing or not map-typed"), *VariableName, *CharacterPath);
		}

		HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
		return;
	}

	// Get the value to set - check for connected input edge
	FStoryFlowVariant NewValue;
	const bool bIsArray = Node->Data.bIsArray;
	// Array variables wire through "<type>-array-input", scalars through "<type>-input"
	FString InputHandleSuffix = VariableType + (bIsArray ? TEXT("-array-input") : TEXT("-input"));
	const FStoryFlowConnection* InputEdge = ExecutionContext.FindInputEdge(Node->Id, InputHandleSuffix);

	if (bIsArray)
	{
		if (InputEdge && Evaluator)
		{
			NewValue.SetArray(EvaluateTypedArrayInput(Node, VariableType, InputHandleSuffix));
		}
		else if (Node->Data.Value.GetArray().Num() > 0)
		{
			// Use inline array value from node data
			NewValue = Node->Data.Value;
		}
		// Unwired with no inline array: leave NewValue empty (matches the HTML runtime's [] default)
	}
	else if (InputEdge)
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

	FString ValueStr = bIsArray ? FString::Printf(TEXT("[%d elements]"), NewValue.GetArray().Num()) : NewValue.ToString();
	SF_TRACE(ExecutionContext, "VAR SET \"%s.%s\" global=false value=%s", *CharacterPath, *VariableName, *ValueStr);

	// Detect whether the mutation will succeed so we can fire OnCharacterVariableChanged
	// after the fact. The branches mirror ExecutionContext::SetCharacterVariable.
	bool bMutated = false;
	if (FStoryFlowCharacterDef* PreCharDef = ExecutionContext.FindCharacter(CharacterPath))
	{
		if (VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase) ||
			VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase))
		{
			bMutated = true;
		}
		else if (PreCharDef->Variables.Contains(VariableName))
		{
			bMutated = true;
		}
	}

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

	// Notify listeners that the character variable changed (after mutation + image caching).
	if (bMutated)
	{
		OnCharacterVariableChanged.Broadcast(CharacterPath, VariableName, NewValue);
	}

	// Continue execution
	HandleSetNodeEnd(Node, StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow));
}

bool UStoryFlowComponent::TryReadDataAssetSetInput(FStoryFlowNode* Node, FStoryFlowVariant& OutValue)
{
	if (!Evaluator)
	{
		return false;
	}

	const FStoryFlowNodeData& Data = Node->Data;
	const FString ValueOptionId = StoryFlowHandles::DataAssetValueOptionId;

	// MAP: the value pin bakes K/V into its handle id, so a snapshot missing either cannot even
	// name its pin — treat that as unwired rather than guessing a shape.
	if (Data.VariableType == TEXT("map"))
	{
		if (Data.KeyType.IsEmpty() || Data.ValueType.IsEmpty())
		{
			return false;
		}
		const FString MapSuffix = StoryFlowHandles::In_Map(Data.KeyType, Data.ValueType, ValueOptionId);
		if (!ExecutionContext.FindInputEdge(Node->Id, MapSuffix))
		{
			return false;
		}
		// Copy immediately: EvaluateMapInput hands back a pointer whose lifetime ends at the next
		// evaluation, and the store deep-copies again on the way in (contract §5).
		TArray<FStoryFlowMapEntry> Entries;
		if (const TArray<FStoryFlowMapEntry>* SourceMap = Evaluator->EvaluateMapInput(Node, ValueOptionId))
		{
			Entries = *SourceMap;
		}
		OutValue.SetMap(Entries);
		return true;
	}

	// ARRAY: "{type}-array-2". EvaluateTypedArrayInput is a plain element-type dispatch over the
	// evaluator's typed array readers, and its TArray return is already a copy — exactly what the
	// store wants handed to it.
	if (Data.bIsArray)
	{
		const FString ArraySuffix = Data.VariableType + TEXT("-array-") + ValueOptionId;
		if (!ExecutionContext.FindInputEdge(Node->Id, ArraySuffix))
		{
			return false;
		}
		// The element type is STATED, not inferred: an empty wired array has no element [0] for
		// FStoryFlowVariant::SetArray to read one off, and this node is the LAST writer before the
		// store, so an untyped value here is what U3's save key would serialize. The ladder has
		// already proved DeclMatches, so the snapshot type IS the chain's declared type.
		OutValue.SetArray(EvaluateTypedArrayInput(Node, Data.VariableType, ArraySuffix), ParseVariableType(Data.VariableType));
		return true;
	}

	// SCALAR: "{type}-2". The string family (string / enum / image / character / audio) all
	// travel as strings, exactly as the setCharacterVar scalar branch reads them.
	const FString ScalarSuffix = Data.VariableType + TEXT("-") + ValueOptionId;
	const FStoryFlowConnection* Edge = ExecutionContext.FindInputEdge(Node->Id, ScalarSuffix);
	if (!Edge)
	{
		return false;
	}
	FStoryFlowNode* SourceNode = ExecutionContext.GetNode(Edge->Source);
	if (!SourceNode)
	{
		return false;
	}

	if (Data.VariableType == TEXT("boolean"))
	{
		OutValue.SetBool(Evaluator->EvaluateBooleanFromNode(SourceNode, Node->Id, Edge->SourceHandle));
	}
	else if (Data.VariableType == TEXT("integer"))
	{
		OutValue.SetInt(Evaluator->EvaluateIntegerFromNode(SourceNode, Node->Id, Edge->SourceHandle));
	}
	else if (Data.VariableType == TEXT("float"))
	{
		OutValue.SetFloat(Evaluator->EvaluateFloatFromNode(SourceNode, Node->Id, Edge->SourceHandle));
	}
	else if (Data.VariableType == TEXT("enum"))
	{
		// Enum travels as a string but is NOT String-typed: the seed stores an enum declaration's
		// value as EStoryFlowVariableType::Enum, so writing one as String makes the overlay entry
		// differ in type from the file value it shadows - invisible to a read (both answer
		// GetString) and visible in U3's save key. image / character / audio genuinely ARE stored
		// as String by the importer, so they keep the string branch below.
		OutValue.SetEnum(Evaluator->EvaluateStringFromNode(SourceNode, Node->Id, Edge->SourceHandle));
	}
	else
	{
		OutValue.SetString(Evaluator->EvaluateStringFromNode(SourceNode, Node->Id, Edge->SourceHandle));
	}
	return true;
}

void UStoryFlowComponent::HandleSetDataAssetVariable(FStoryFlowNode* Node)
{
	const FString FlowHandle = StoryFlowHandles::Source(Node->Id, StoryFlowHandles::Out_Flow);

	// THE LADDER FIRST (contract §6): an unwired / dead / stale binding is a NO-OP with a
	// once-per-node warning, and exec still continues. Shared with every read arm on purpose —
	// a reason honored on the read path but not here gives you an accessor that reads the
	// declared default while its twin writes an overlay entry shadowing it for the session.
	FString AssetId;
	if (!ExecutionContext.TryResolveDataAssetBinding(*Node, AssetId))
	{
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	// NO INLINE-VALUE FALLBACK, ever (contract §5). The editor persists no literal on this
	// node's value pin — its face is the binding, not an editor — so an unwired pin has nothing
	// to offer and must REFUSE rather than write a type zero over the declared default. This is
	// the getTypedInput trap the character Set arm walks into: its helpers substitute 0 / "" /
	// false for an unwired pin, which is why TryReadDataAssetSetInput checks the edge itself on
	// every branch instead of trusting an evaluator's fallback.
	FStoryFlowVariant NewValue;
	if (!TryReadDataAssetSetInput(Node, NewValue))
	{
		// NOT latched, unlike the ladder's reasons: this names a wiring mistake on an EXEC node
		// the author just ran, and an exec node fires far less often than a condition
		// re-evaluates (contract §6, the value-refusal row).
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Set Data Asset Variable refused an undefined value (nothing wired to its value pin): node %s"), *Node->Id);
		HandleSetNodeEnd(Node, FlowHandle);
		return;
	}

	// Trace BEFORE the write, matching the HTML arm's order (trace lines are parity artifacts —
	// never reorder trace-vs-write). Arrays and entry lists trace by size, like the map pins do.
	if (NewValue.IsMap())
	{
		SF_TRACE(ExecutionContext, "DA SET \"%s.%s\" size=%d", *AssetId, *Node->Data.VariableId, NewValue.GetMap().Num());
	}
	else if (Node->Data.bIsArray)
	{
		SF_TRACE(ExecutionContext, "DA SET \"%s.%s\" size=%d", *AssetId, *Node->Data.VariableId, NewValue.GetArray().Num());
	}
	else
	{
		SF_TRACE(ExecutionContext, "DA SET \"%s.%s\" value=%s", *AssetId, *Node->Data.VariableId, *NewValue.ToString());
	}

	// The store deep-copies on the way in and REPLACES a map's whole value; the write lands at
	// the wired asset's OWN level, always (contract §5). The ladder already proved the asset and
	// the declaration, so a false here would be a store bug, not an authoring one.
	ExecutionContext.TrySetDataAsset(AssetId, Node->Data.VariableId, NewValue);

	// Drop cached results so option conditions re-evaluate against the new value (contract §5).
	// Data asset reads are never memoized themselves, but a notBool / comparison ABOVE one is,
	// and that is what the HTML arm's clearNotBoolCache exists for. ClearCache is WIDER than
	// that: it drops the whole evaluation cache, not just the notBool memo. Strictly safe (every
	// dropped entry is recomputed on demand), and this node has no output stamp for the drop to
	// erase, unlike HandleArrayModify - see the ordering note there.
	if (Evaluator)
	{
		Evaluator->ClearCache();
	}

	HandleSetNodeEnd(Node, FlowHandle);
}

TArray<FStoryFlowVariant> UStoryFlowComponent::EvaluateTypedArrayInput(FStoryFlowNode* Node, const FString& VariableType, const FString& HandleSuffix)
{
	if (VariableType == TEXT("boolean"))
	{
		return Evaluator->EvaluateBoolArrayInput(Node, HandleSuffix);
	}
	if (VariableType == TEXT("integer"))
	{
		return Evaluator->EvaluateIntArrayInput(Node, HandleSuffix);
	}
	if (VariableType == TEXT("float"))
	{
		return Evaluator->EvaluateFloatArrayInput(Node, HandleSuffix);
	}
	if (VariableType == TEXT("image"))
	{
		return Evaluator->EvaluateImageArrayInput(Node, HandleSuffix);
	}
	if (VariableType == TEXT("character"))
	{
		return Evaluator->EvaluateCharacterArrayInput(Node, HandleSuffix);
	}
	if (VariableType == TEXT("audio"))
	{
		return Evaluator->EvaluateAudioArrayInput(Node, HandleSuffix);
	}
	if (VariableType == TEXT("enum"))
	{
		// An enum travels on a string pin, so it is READ through the string reader — but it is
		// STORED with its own type tag, the same distinction the scalar enum branch makes. The
		// importer types an enum array's elements Enum (ParseVariant's Enum arm) and a save
		// restores them Enum from the declaration, so a write that left them String would be the
		// only one of the three writers disagreeing. Invisible today (every reader answers
		// GetString for the whole string family) and exactly the kind of thing that stops being
		// invisible the moment something switches on the element type.
		TArray<FStoryFlowVariant> Elements = Evaluator->EvaluateStringArrayInput(Node, HandleSuffix);
		for (FStoryFlowVariant& Element : Elements)
		{
			Element.SetEnum(Element.GetString());
		}
		return Elements;
	}
	// String / image / character / audio - string-keyed storage (matches the HTML runtime's default branch)
	return Evaluator->EvaluateStringArrayInput(Node, HandleSuffix);
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
				State.Character.Image = ResolveCharacterPortraitTexture(DialogueNode->Data.Character, CharDef->Image, CharDef);
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

	// Presentation tags pass through untouched (raw authored strings, in array order)
	State.Tags = DialogueNode->Data.Tags;

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
							   ConnPtr->SourceHandle.Contains(TEXT("-audio-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-map-")) ||
							   ConnPtr->SourceHandle.Contains(TEXT("-dataAsset-"));

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
