// Copyright 2026 StoryFlow. All Rights Reserved.

#include "UI/StoryFlowDialogueWidget.h"
#include "Components/StoryFlowComponent.h"
#include "Components/Button.h"
#include "Evaluation/StoryFlowRestoredListener.h"

void UStoryFlowDialogueWidget::InitializeWithComponent(UStoryFlowComponent* InComponent)
{
	++ComponentBindingSerial;
	// Unbind from previous component if any
	UnbindFromComponent();

	StoryFlowComponent = InComponent;

	// Bind to new component
	BindToComponent();
}

void UStoryFlowDialogueWidget::DetachFromComponent()
{
	// InitializeWithComponent already unbinds from the old component and stores
	// what it is given, so passing null is exactly a detach. Routing through it
	// keeps a single attach/detach path instead of a second one that could drift.
	InitializeWithComponent(nullptr);
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
	StoryFlowComponent->OnRollbackAvailabilityChanged.AddDynamic(this, &UStoryFlowDialogueWidget::HandleRollbackAvailabilityChanged);
	RestoredListener = NewObject<UStoryFlowRestoredListener>(this);
	RestoredListener->Source = StoryFlowComponent;
	const TWeakObjectPtr<UStoryFlowDialogueWidget> Self(this);
	UStoryFlowRestoredListener* Binding = RestoredListener;
	RestoredListener->Callback = [Self, Binding](const FStoryFlowDialogueState& State, UStoryFlowComponent* Source) {
		if (Self.IsValid() && Self->RestoredListener == Binding) Self->HandleDialogueRestored(State, Source);
	};
	StoryFlowComponent->OnDialogueRestored.AddDynamic(RestoredListener, &UStoryFlowRestoredListener::Restore);
	if (BackButton) BackButton->OnClicked.AddUniqueDynamic(this, &UStoryFlowDialogueWidget::HandleBackClicked);

	bIsBoundToComponent = true;
	HandleRollbackAvailabilityChanged(StoryFlowComponent->GetRollbackAvailability());
	if (StoryFlowComponent && StoryFlowComponent->IsCurrentDialogueRestored())
		HandleDialogueRestored(StoryFlowComponent->GetCurrentDialogue(), StoryFlowComponent);
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
	StoryFlowComponent->OnRollbackAvailabilityChanged.RemoveDynamic(this, &UStoryFlowDialogueWidget::HandleRollbackAvailabilityChanged);
	if (RestoredListener) StoryFlowComponent->OnDialogueRestored.RemoveDynamic(RestoredListener, &UStoryFlowRestoredListener::Restore);
	RestoredListener = nullptr;
	if (BackButton) { BackButton->OnClicked.RemoveDynamic(this, &UStoryFlowDialogueWidget::HandleBackClicked); BackButton->SetIsEnabled(false); }

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
	if (StoryFlowComponent && StoryFlowComponent->IsDialogueActive()) return;
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

void UStoryFlowDialogueWidget::OnDialogueRestored_Implementation(const FStoryFlowDialogueState&) {}
void UStoryFlowDialogueWidget::OnRollbackAvailabilityChanged_Implementation(const FStoryFlowRollbackAvailability&) {}

void UStoryFlowDialogueWidget::HandleDialogueRestored(const FStoryFlowDialogueState& State, UStoryFlowComponent* Source)
{
	if (!bIsBoundToComponent || Source != StoryFlowComponent || !Source || !Source->IsCurrentDialogueRestored() ||
		Source->GetCurrentDialogue().NodeId != State.NodeId) return;
	OnDialogueRestored(State);
}
void UStoryFlowDialogueWidget::HandleRollbackAvailabilityChanged(const FStoryFlowRollbackAvailability&)
{
	const auto Current = GetRollbackAvailability();
	if (BackButton) BackButton->SetIsEnabled(bBackAllowed && Current.bCanGoBack);
	OnRollbackAvailabilityChanged(Current);
}
void UStoryFlowDialogueWidget::HandleBackClicked() { if (bBackAllowed) GoBack(); }
void UStoryFlowDialogueWidget::SetBackAllowed(bool Allowed) { bBackAllowed = Allowed; HandleRollbackAvailabilityChanged(GetRollbackAvailability()); }
bool UStoryFlowDialogueWidget::CanGoBack() const { return StoryFlowComponent && StoryFlowComponent->CanGoBack(); }
FStoryFlowRollbackAvailability UStoryFlowDialogueWidget::GetRollbackAvailability() const { return StoryFlowComponent ? StoryFlowComponent->GetRollbackAvailability() : FStoryFlowRollbackAvailability(); }
FStoryFlowRollbackResult UStoryFlowDialogueWidget::GoBack()
{
	if (StoryFlowComponent) return StoryFlowComponent->GoBack();
	FStoryFlowRollbackResult Result; Result.Reason = TEXT("disabled"); return Result;
}
void UStoryFlowDialogueWidget::BlockRollback(const FString& Reason) { if (StoryFlowComponent) StoryFlowComponent->BlockRollback(Reason); }

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
