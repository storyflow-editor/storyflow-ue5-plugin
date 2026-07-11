// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StoryFlowTagAccumulator.generated.h"

/**
 * Test-only accumulator. A UFUNCTION handler is required to bind the dynamic
 * multicast OnDialogueTagReached delegate from C++; this records every tag it
 * receives so a runtime automation test can assert order and count.
 */
UCLASS()
class UStoryFlowTagAccumulator : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Tags;

	UFUNCTION()
	void OnTag(const FString& Tag)
	{
		Tags.Add(Tag);
	}
};
