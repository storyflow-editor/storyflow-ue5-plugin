// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowDialogueWidget.generated.h"

class UStoryFlowComponent;
class UStoryFlowRestoredListener;
class UButton;

/**
 * Base widget class for StoryFlow dialogue UI
 *
 * Extend this class in Blueprint to create custom dialogue UIs.
 * The widget automatically binds to a StoryFlowComponent and receives
 * dialogue updates via the OnDialogueUpdated event.
 */
UCLASS(BlueprintType, Blueprintable)
class STORYFLOWRUNTIME_API UStoryFlowDialogueWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Initialize the widget with a StoryFlow component
	 * Call this after creating the widget to bind it to a component
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void InitializeWithComponent(UStoryFlowComponent* InComponent);

	/**
	 * Stop following the component: unsubscribe from its events and forget it.
	 *
	 * The component calls this when it lets a widget go while the game owns the
	 * widget's lifecycle (see UStoryFlowComponent::bAutoAddWidgetToViewport), so a
	 * widget that is still fading out is not driven by the next dialogue.
	 *
	 * Clearing the component pointer is part of the contract rather than tidiness:
	 * NativeConstruct re-binds whenever the pointer is still set, so re-parenting a
	 * merely unsubscribed widget would silently resubscribe it.
	 *
	 * Afterwards GetStoryFlowComponent returns null and the helpers that route
	 * through it (SelectOption, AdvanceDialogue, GetCurrentDialogueState) do
	 * nothing, so a fade-out that still needs to talk to the component has to use
	 * the game's own reference. Call InitializeWithComponent again to reattach.
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void DetachFromComponent();

	/**
	 * Get the bound StoryFlow component
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowComponent* GetStoryFlowComponent() const { return StoryFlowComponent; }
	uint64 GetComponentBindingSerial() const { return ComponentBindingSerial; }

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	// ========================================================================
	// Blueprint Events (Override these in your widget Blueprint)
	// ========================================================================

	/**
	 * Called when dialogue state updates (new text, options, etc.)
	 * Override this in Blueprint to update your UI
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow")
	void OnDialogueUpdated(const FStoryFlowDialogueState& DialogueState);

	/** Fully reveal this restored entry and cancel the consuming widget's auto/typewriter timers. */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow|Rollback")
	void OnDialogueRestored(const FStoryFlowDialogueState& DialogueState);
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow|Rollback")
	void OnRollbackAvailabilityChanged(const FStoryFlowRollbackAvailability& Availability);

	/**
	 * Called when dialogue starts
	 * Override this in Blueprint to show your dialogue UI
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow")
	void OnDialogueStarted();

	/**
	 * Called when dialogue ends
	 * Override this in Blueprint to hide your dialogue UI
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow")
	void OnDialogueEnded();

	/**
	 * Called when a variable changes
	 * Override this in Blueprint to react to variable changes
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "StoryFlow")
	void OnVariableChanged(const FStoryFlowVariable& Variable, bool bIsGlobal);

public:
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Rollback")
	bool CanGoBack() const;
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Rollback")
	FStoryFlowRollbackAvailability GetRollbackAvailability() const;
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Rollback")
	FStoryFlowRollbackResult GoBack();
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Rollback")
	void BlockRollback(const FString& Reason = TEXT(""));
	/** Additional author control. Availability never re-enables a forbidden Back control. */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow|Rollback")
	void SetBackAllowed(bool bAllowed);
	// ========================================================================
	// Blueprint Callable Helper Functions
	// ========================================================================

	/**
	 * Select a dialogue option by ID
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void SelectOption(const FString& OptionId);

	/**
	 * Advance a narrative-only dialogue (no options defined)
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void AdvanceDialogue();

	/**
	 * Get the current dialogue state
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FStoryFlowDialogueState GetCurrentDialogueState() const;

	/**
	 * Check if dialogue is currently active
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool IsDialogueActive() const;

	/**
	 * Get a localized string by key
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetLocalizedString(const FString& Key) const;

protected:
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow|Rollback", meta = (BindWidgetOptional))
	TObjectPtr<UButton> BackButton;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StoryFlow|Rollback")
	bool bBackAllowed = true;
	/** The bound StoryFlow component */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow", meta = (ExposeOnSpawn = true))
	TObjectPtr<UStoryFlowComponent> StoryFlowComponent;

private:
	UPROPERTY(Transient)
	TObjectPtr<UStoryFlowRestoredListener> RestoredListener;
	void HandleDialogueRestored(const FStoryFlowDialogueState& State, UStoryFlowComponent* Source);
	UFUNCTION() void HandleRollbackAvailabilityChanged(const FStoryFlowRollbackAvailability& Availability);
	UFUNCTION() void HandleBackClicked();
	/** Bind to component events */
	void BindToComponent();

	/** Unbind from component events */
	void UnbindFromComponent();

	/** Track if we're currently bound to prevent double-binding */
	bool bIsBoundToComponent = false;
	uint64 ComponentBindingSerial = 0;

	/** Internal handlers for component events */
	UFUNCTION()
	void HandleDialogueStarted();

	UFUNCTION()
	void HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState);

	UFUNCTION()
	void HandleDialogueEnded();

	UFUNCTION()
	void HandleVariableChanged(const FStoryFlowVariable& Variable, bool bIsGlobal);
};
