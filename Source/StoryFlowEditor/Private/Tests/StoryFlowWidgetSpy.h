// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/StoryFlowDialogueWidget.h"
#include "UObject/Object.h"
#include "StoryFlowWidgetSpy.generated.h"

/**
 * Test-only dialogue widget that counts the component's teardown calls.
 *
 * RemoveFromParent is the only half of the auto-add contract observable in a
 * headless test: AddToViewport needs a game viewport, which -nullrhi automation
 * has none of, so it no-ops. RemoveFromParent is virtual, so a subclass can
 * record whether the component tore the widget down or left it to its owner.
 */
UCLASS()
class UStoryFlowWidgetSpy : public UStoryFlowDialogueWidget
{
	GENERATED_BODY()

public:
	int32 RemoveFromParentCount = 0;

	/** Dialogue updates this widget received, i.e. how live its subscription still is. */
	int32 DialogueUpdatedCount = 0;

	virtual void RemoveFromParent() override
	{
		++RemoveFromParentCount;
		Super::RemoveFromParent();
	}

	virtual void OnDialogueUpdated_Implementation(const FStoryFlowDialogueState& DialogueState) override
	{
		++DialogueUpdatedCount;
	}
};

/**
 * Test-only recorder for OnDialogueWidgetCreated. A UFUNCTION handler is
 * required to bind a dynamic multicast delegate from C++; this keeps every
 * widget it is handed so a test can assert the broadcast count and identity.
 */
UCLASS()
class UStoryFlowWidgetCreationRecorder : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<TObjectPtr<UStoryFlowDialogueWidget>> Created;

	UFUNCTION()
	void OnWidgetCreated(UStoryFlowDialogueWidget* Widget)
	{
		Created.Add(Widget);
	}
};
