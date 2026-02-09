// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowDialogueWidget.generated.h"

class UStoryFlowComponent;

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
	 * Get the bound StoryFlow component
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowComponent* GetStoryFlowComponent() const { return StoryFlowComponent; }

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
	void OnVariableChanged(const FString& VariableId, const FStoryFlowVariant& NewValue, bool bIsGlobal);

public:
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
	/** The bound StoryFlow component */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow", meta = (ExposeOnSpawn = true))
	TObjectPtr<UStoryFlowComponent> StoryFlowComponent;

private:
	/** Bind to component events */
	void BindToComponent();

	/** Unbind from component events */
	void UnbindFromComponent();

	/** Track if we're currently bound to prevent double-binding */
	bool bIsBoundToComponent = false;

	/** Internal handlers for component events */
	UFUNCTION()
	void HandleDialogueStarted();

	UFUNCTION()
	void HandleDialogueUpdated(const FStoryFlowDialogueState& DialogueState);

	UFUNCTION()
	void HandleDialogueEnded();

	UFUNCTION()
	void HandleVariableChanged(const FString& VariableId, const FStoryFlowVariant& NewValue, bool bIsGlobal);
};
