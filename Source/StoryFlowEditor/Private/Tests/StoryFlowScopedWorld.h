// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Components/StoryFlowComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Subsystems/StoryFlowSubsystem.h"

/**
 * A standalone game instance with a registered StoryFlow component, torn down with the scope —
 * the world every runtime-behaviour test needs before it can touch the subsystem's stores.
 *
 * Header-only and `inline`-free by construction (a struct with inline members), included by more
 * than one test file in what is a unity build, so it must define no non-inline symbols.
 *
 * Same precedent as StoryFlowEngineContractFixtures.h / StoryFlowTagAccumulator.h /
 * StoryFlowWidgetSpy.h: a test-only header beside the tests that share it, never in Public.
 *
 * SCOPE: adopted by the .sfd feature suites (node arms, save and the Blueprint surface), which
 * were written together and are the reason this was extracted. The older copies in the dialogue
 * tag and widget lifecycle suites are deliberately left alone — they are not part of this
 * feature, and folding them in would put unrelated files in a .sfd change.
 */
namespace StoryFlowTestWorld
{
	struct FScopedWorld
	{
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		UStoryFlowComponent* Component = nullptr;
		UStoryFlowSubsystem* Subsystem = nullptr;

		bool Init()
		{
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->InitializeStandalone();
			World = GameInstance->GetWorld();
			if (!World) { return false; }
			AActor* Owner = World->SpawnActor<AActor>();
			if (!Owner) { return false; }
			Component = NewObject<UStoryFlowComponent>(Owner);
			Component->RegisterComponent();
			Subsystem = GameInstance->GetSubsystem<UStoryFlowSubsystem>();
			return Component != nullptr && Subsystem != nullptr;
		}

		~FScopedWorld()
		{
			if (World) { World->DestroyWorld(false); }
		}
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
