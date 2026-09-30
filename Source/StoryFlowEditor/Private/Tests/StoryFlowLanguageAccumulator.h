// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StoryFlowLanguageAccumulator.generated.h"

/**
 * Test-only accumulator. A UFUNCTION handler is required to bind the dynamic
 * multicast OnLanguageChanged delegate from C++; this records every code it
 * receives so a test can assert count and order, and reads the subsystem back
 * at broadcast time so the ordering contract is pinned by observation rather
 * than by reading the implementation.
 */
UCLASS()
class UStoryFlowLanguageAccumulator : public UObject
{
	GENERATED_BODY()

public:
	/** Every code the delegate delivered, in order. */
	TArray<FString> Codes;

	/** What GetLanguage() answered DURING each broadcast, in the same order. */
	TArray<FString> Observed;

	/** How many rows GetLanguages() held DURING each broadcast — which project was installed. */
	TArray<int32> RosterSizes;

	/** Set by the test; read back inside the handler. */
	UPROPERTY()
	TObjectPtr<class UStoryFlowSubsystem> Subsystem = nullptr;

	UFUNCTION()
	void OnLanguageChanged(const FString& LanguageCode);
};
