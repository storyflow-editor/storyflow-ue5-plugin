// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Subsystems/StoryFlowSubsystem.h"

/**
 * Tests for the Blueprint-callable Set*ArrayVariable family on UStoryFlowComponent.
 *
 * The setters mirror the Unity plugin's Set*ArrayVariable API (added there in
 * v1.1.2): find the variable by display name, replace its element storage,
 * broadcast OnVariableChanged. Element typing follows this runtime's import
 * convention (ParseVariant): bool/int/float/enum elements carry their own type,
 * string/image/audio/character elements are plain strings.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.ArrayVariables", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.ArrayVariables" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowArrayVariableTestHelpers
{
	/** Standalone game instance + registered component, so the component's
	 *  subsystem lookup and the global-variable path behave exactly as in game. */
	struct FScopedStoryFlowWorld
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
			if (!World)
			{
				return false;
			}
			AActor* Owner = World->SpawnActor<AActor>();
			if (!Owner)
			{
				return false;
			}
			Component = NewObject<UStoryFlowComponent>(Owner);
			Component->RegisterComponent();
			Subsystem = GameInstance->GetSubsystem<UStoryFlowSubsystem>();
			return Component != nullptr && Subsystem != nullptr;
		}

		~FScopedStoryFlowWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
			}
		}

		/** Register a global variable the way the subsystem would hold it after import. */
		FStoryFlowVariable& AddGlobalArrayVariable(const FString& Id, const FString& Name, EStoryFlowVariableType Type)
		{
			FStoryFlowVariable Var;
			Var.Id = Id;
			Var.Name = Name;
			Var.Type = Type;
			Var.bIsArray = true;
			return Subsystem->GetGlobalVariables().Add(Id, Var);
		}
	};
}

// ============================================================================
// String arrays
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetStringArrayVariableTest,
	"StoryFlow.ArrayVariables.SetStringArrayWritesGlobal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSetStringArrayVariableTest::RunTest(const FString& Parameters)
{
	StoryFlowArrayVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init()))
	{
		return false;
	}
	Fixture.AddGlobalArrayVariable(TEXT("var_inv"), TEXT("Inventory"), EStoryFlowVariableType::String);

	Fixture.Component->SetStringArrayVariable(TEXT("Inventory"), { TEXT("sword"), TEXT("axe") }, true);

	const FStoryFlowVariable& Stored = Fixture.Subsystem->GetGlobalVariables()[TEXT("var_inv")];
	TestEqual(TEXT("stored element count"), Stored.Value.GetArray().Num(), 2);
	if (Stored.Value.GetArray().Num() == 2)
	{
		TestEqual(TEXT("element 0"), Stored.Value.GetArray()[0].GetString(), FString(TEXT("sword")));
		TestEqual(TEXT("element 1"), Stored.Value.GetArray()[1].GetString(), FString(TEXT("axe")));
	}

	// Read back through the public Blueprint getter as well
	TArray<FStoryFlowVariant> ReadBack = Fixture.Component->GetArrayVariable(TEXT("Inventory"), true);
	TestEqual(TEXT("GetArrayVariable element count"), ReadBack.Num(), 2);

	// Overwrite replaces, not appends
	Fixture.Component->SetStringArrayVariable(TEXT("Inventory"), { TEXT("bow") }, true);
	TestEqual(TEXT("overwrite leaves one element"), Fixture.Subsystem->GetGlobalVariables()[TEXT("var_inv")].Value.GetArray().Num(), 1);

	// Empty write clears
	Fixture.Component->SetStringArrayVariable(TEXT("Inventory"), {}, true);
	TestEqual(TEXT("empty write clears elements"), Fixture.Subsystem->GetGlobalVariables()[TEXT("var_inv")].Value.GetArray().Num(), 0);

	return true;
}

// ============================================================================
// Numeric and boolean arrays
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetNumericArrayVariableTest,
	"StoryFlow.ArrayVariables.SetBoolIntFloatArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSetNumericArrayVariableTest::RunTest(const FString& Parameters)
{
	StoryFlowArrayVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init()))
	{
		return false;
	}
	Fixture.AddGlobalArrayVariable(TEXT("var_b"), TEXT("Flags"), EStoryFlowVariableType::Boolean);
	Fixture.AddGlobalArrayVariable(TEXT("var_i"), TEXT("Scores"), EStoryFlowVariableType::Integer);
	Fixture.AddGlobalArrayVariable(TEXT("var_f"), TEXT("Weights"), EStoryFlowVariableType::Float);

	Fixture.Component->SetBoolArrayVariable(TEXT("Flags"), { true, false, true }, true);
	Fixture.Component->SetIntArrayVariable(TEXT("Scores"), { 10, 20, 30 }, true);
	Fixture.Component->SetFloatArrayVariable(TEXT("Weights"), { 0.5f, 2.25f }, true);

	const TArray<FStoryFlowVariant>& Flags = Fixture.Subsystem->GetGlobalVariables()[TEXT("var_b")].Value.GetArray();
	TestEqual(TEXT("bool count"), Flags.Num(), 3);
	if (Flags.Num() == 3)
	{
		TestEqual(TEXT("bool[1]"), Flags[1].GetBool(true), false);
	}

	const TArray<FStoryFlowVariant>& Scores = Fixture.Subsystem->GetGlobalVariables()[TEXT("var_i")].Value.GetArray();
	TestEqual(TEXT("int count"), Scores.Num(), 3);
	if (Scores.Num() == 3)
	{
		TestEqual(TEXT("int[2]"), Scores[2].GetInt(), 30);
	}

	const TArray<FStoryFlowVariant>& Weights = Fixture.Subsystem->GetGlobalVariables()[TEXT("var_f")].Value.GetArray();
	TestEqual(TEXT("float count"), Weights.Num(), 2);
	if (Weights.Num() == 2)
	{
		TestEqual(TEXT("float[1]"), Weights[1].GetFloat(), 2.25f);
	}

	return true;
}

// ============================================================================
// Enum element typing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetEnumArrayVariableTest,
	"StoryFlow.ArrayVariables.SetEnumArrayTypesElements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSetEnumArrayVariableTest::RunTest(const FString& Parameters)
{
	StoryFlowArrayVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init()))
	{
		return false;
	}
	Fixture.AddGlobalArrayVariable(TEXT("var_e"), TEXT("Moods"), EStoryFlowVariableType::Enum);

	Fixture.Component->SetEnumArrayVariable(TEXT("Moods"), { TEXT("happy"), TEXT("angry") }, true);

	const TArray<FStoryFlowVariant>& Moods = Fixture.Subsystem->GetGlobalVariables()[TEXT("var_e")].Value.GetArray();
	TestEqual(TEXT("enum count"), Moods.Num(), 2);
	if (Moods.Num() == 2)
	{
		TestEqual(TEXT("enum element type"), Moods[0].GetType(), EStoryFlowVariableType::Enum);
		TestEqual(TEXT("enum element value"), Moods[0].GetString(), FString(TEXT("happy")));
	}

	return true;
}

// ============================================================================
// Asset-key arrays (image / audio / character)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetAssetArrayVariableTest,
	"StoryFlow.ArrayVariables.SetImageArrayStoresAssetKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSetAssetArrayVariableTest::RunTest(const FString& Parameters)
{
	StoryFlowArrayVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init()))
	{
		return false;
	}
	Fixture.AddGlobalArrayVariable(TEXT("var_img"), TEXT("Gallery"), EStoryFlowVariableType::Image);

	Fixture.Component->SetImageArrayVariable(TEXT("Gallery"), { TEXT("asset_image_1"), TEXT("asset_image_2") }, true);

	const TArray<FStoryFlowVariant>& Gallery = Fixture.Subsystem->GetGlobalVariables()[TEXT("var_img")].Value.GetArray();
	TestEqual(TEXT("image count"), Gallery.Num(), 2);
	if (Gallery.Num() == 2)
	{
		// Plain-string storage matches the importer's ParseVariant convention;
		// GetString accepts the whole string family either way.
		TestEqual(TEXT("image element value"), Gallery[1].GetString(), FString(TEXT("asset_image_2")));
	}

	return true;
}

// ============================================================================
// Missing variable
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetArrayVariableMissingTest,
	"StoryFlow.ArrayVariables.MissingVariableWarnsAndWritesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSetArrayVariableMissingTest::RunTest(const FString& Parameters)
{
	StoryFlowArrayVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init()))
	{
		return false;
	}

	// The subsystem auto-loads the project's globals; assert relative to that.
	const int32 CountBefore = Fixture.Subsystem->GetGlobalVariables().Num();

	AddExpectedError(TEXT("Global variable 'Nope' not found"), EAutomationExpectedErrorFlags::Contains, 1);
	Fixture.Component->SetStringArrayVariable(TEXT("Nope"), { TEXT("x") }, true);

	TestEqual(TEXT("no variable was created"), Fixture.Subsystem->GetGlobalVariables().Num(), CountBefore);
	for (const auto& Pair : Fixture.Subsystem->GetGlobalVariables())
	{
		TestNotEqual(TEXT("no 'Nope' entry appeared"), Pair.Value.Name, FString(TEXT("Nope")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
