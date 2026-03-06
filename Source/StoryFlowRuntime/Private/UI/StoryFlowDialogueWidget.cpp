// Copyright 2026 StoryFlow. All Rights Reserved.

#include "UI/StoryFlowDialogueWidget.h"
#include "Components/StoryFlowComponent.h"

void UStoryFlowDialogueWidget::InitializeWithComponent(UStoryFlowComponent* InComponent)
{
	// Unbind from previous component if any
	UnbindFromComponent();

	StoryFlowComponent = InComponent;

	// Bind to new component
	BindToComponent();
}

void UStoryFlowDialogueWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Bind to component if already set
	if (StoryFlowComponent)
	{
		BindToComponent();
	}
}

void UStoryFlowDialogueWidget::NativeDestruct()
{
	UnbindFromComponent();
	Super::NativeDestruct();
}

void UStoryFlowDialogueWidget::BindToComponent()
{
	if (!StoryFlowComponent || bIsBoundToComponent)
	{
		return;
	}

	StoryFlowComponent->OnDialogueStarted.AddDynamic(this, &UStoryFlowDialogueWidget::HandleDialogueStarted);
	StoryFlowComponent->OnDialogueUpdated.AddDynamic(this, &UStoryFlowDialogueWidget::HandleDialogueUpdated);
	StoryFlowComponent->OnDialogueEnded.AddDynamic(this, &UStoryFlowDialogueWidget::HandleDialogueEnded);
	StoryFlowComponent->OnVariableChanged.AddDynamic(this, &UStoryFlowDialogueWidget::HandleVariableChanged);

	bIsBoundToComponent = true;
}

void UStoryFlowDialogueWidget::UnbindFromComponent()
{
	if (!StoryFlowComponent || !bIsBoundToComponent)
	{
		return;
	}

	StoryFlowComponent->OnDialogueStarted.RemoveDynamic(this, &UStoryFlowDialogueWidget::HandleDialogueStarted);
	StoryFlowComponent->OnDialogueUpdated.RemoveDynamic(this, &UStoryFlowDialogueWidget::HandleDialogueUpdated);
	StoryFlowComponent->OnDialogueEnded.RemoveDynamic(this, &UStoryFlowDialogueWidget::HandleDialogueEnded);
	StoryFlowComponent->OnVariableChanged.RemoveDynamic(this, &UStoryFlowDialogueWidget::HandleVariableChanged);

	bIsBoundToComponent = false;
}

// ============================================================================
// Event Handlers
// ============================================================================

void UStoryFlowDialogueWidget::HandleDialogueStarted()
{
	OnDialogueStarted();
}

void UStoryFlowDialogueWidget::HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState)
{
	OnDialogueUpdated(DialogueState);
}

void UStoryFlowDialogueWidget::HandleDialogueEnded()
{
	OnDialogueEnded();
}

void UStoryFlowDialogueWidget::HandleVariableChanged(const FStoryFlowVariable& Variable, bool bIsGlobal)
{
	OnVariableChanged(Variable, bIsGlobal);
}

// ============================================================================
// Blueprint Native Event Implementations
// ============================================================================

void UStoryFlowDialogueWidget::OnDialogueUpdated_Implementation(const FStoryFlowDialogueState& DialogueState)
{
	// Default implementation does nothing
	// Override in Blueprint to update UI
}

void UStoryFlowDialogueWidget::OnDialogueStarted_Implementation()
{
	// Default implementation does nothing
	// Override in Blueprint to show UI
}

void UStoryFlowDialogueWidget::OnDialogueEnded_Implementation()
{
	// Default implementation does nothing
	// Override in Blueprint to hide UI
}

void UStoryFlowDialogueWidget::OnVariableChanged_Implementation(const FStoryFlowVariable& Variable, bool bIsGlobal)
{
	// Default implementation does nothing
	// Override in Blueprint to react to variable changes
}

// ============================================================================
// Helper Functions
// ============================================================================

void UStoryFlowDialogueWidget::SelectOption(const FString& OptionId)
{
	if (StoryFlowComponent)
	{
		StoryFlowComponent->SelectOption(OptionId);
	}
}

void UStoryFlowDialogueWidget::AdvanceDialogue()
{
	if (StoryFlowComponent)
	{
		StoryFlowComponent->AdvanceDialogue();
	}
}

FStoryFlowDialogueState UStoryFlowDialogueWidget::GetCurrentDialogueState() const
{
	if (StoryFlowComponent)
	{
		return StoryFlowComponent->GetCurrentDialogue();
	}
	return FStoryFlowDialogueState();
}

bool UStoryFlowDialogueWidget::IsDialogueActive() const
{
	if (StoryFlowComponent)
	{
		return StoryFlowComponent->IsDialogueActive();
	}
	return false;
}

FString UStoryFlowDialogueWidget::GetLocalizedString(const FString& Key) const
{
	if (StoryFlowComponent)
	{
		return StoryFlowComponent->GetLocalizedString(Key);
	}
	return Key;
}
