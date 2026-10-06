// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowVariableAccumulator.generated.h"

/**
 * Test-only accumulator, the OnVariableChanged twin of UStoryFlowTagAccumulator: it records each
 * change as "Name:global" or "Name:local" so a runtime test can assert which scope was reported.
 */
UCLASS()
class UStoryFlowVariableAccumulator : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Changes;

	UFUNCTION()
	void OnChanged(const FStoryFlowVariable& Variable, bool bIsGlobal)
	{
		Changes.Add(Variable.Name + (bIsGlobal ? TEXT(":global") : TEXT(":local")));
	}
};
