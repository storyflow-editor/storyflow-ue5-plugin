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
 * Tests for the typed Blueprint map get/set API on UStoryFlowComponent.
 *
 * String and enum keys surface as FString; the string family of values
 * (string/enum/image/audio/character) surfaces as FString. Maps store ordered
 * FStoryFlowMapEntry; TMap is unordered, so GetMapKeysInOrder is the only ordered view.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.MapVariables", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.MapVariables" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */
namespace StoryFlowMapVariableTestHelpers
{
	/** Standalone game instance + registered component, same shape as the array tests. */
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
			if (!World) { return false; }
			AActor* Owner = World->SpawnActor<AActor>();
			if (!Owner) { return false; }
			Component = NewObject<UStoryFlowComponent>(Owner);
			Component->RegisterComponent();
			Subsystem = GameInstance->GetSubsystem<UStoryFlowSubsystem>();
			return Component != nullptr && Subsystem != nullptr;
		}

		~FScopedStoryFlowWorld()
		{
			if (World) { World->DestroyWorld(false); }
		}

		/** Register a global map variable with the given key/value types. */
		FStoryFlowVariable& AddGlobalMapVariable(const FString& Id, const FString& Name,
			EStoryFlowVariableType KeyType, EStoryFlowVariableType ValueType)
		{
			FStoryFlowVariable Var;
			Var.Id = Id;
			Var.Name = Name;
			Var.Type = EStoryFlowVariableType::Map;
			Var.KeyType = KeyType;
			Var.ValueType = ValueType;
			return Subsystem->GetGlobalVariables().Add(Id, Var);
		}

		/** Build a string-key / int-value entry. */
		static FStoryFlowMapEntry StrIntEntry(const FString& Key, int32 Value)
		{
			FStoryFlowMapEntry E;
			E.Key.SetString(Key);
			E.Value.SetInt(Value);
			return E;
		}
	};
}

// ============================================================================
// String-keyed getters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowGetStringToIntMapTest,
	"StoryFlow.MapVariables.GetStringToIntMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowGetStringToIntMapTest::RunTest(const FString& Parameters)
{
	StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init())) { return false; }
	using StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld;

	Fixture.AddGlobalMapVariable(TEXT("var_score"), TEXT("Scores"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Integer);
	Fixture.Subsystem->GetGlobalVariables()[TEXT("var_score")].Value.SetMap(
		{ FScopedStoryFlowWorld::StrIntEntry(TEXT("alice"), 10),
		  FScopedStoryFlowWorld::StrIntEntry(TEXT("bob"), 20) });

	const TMap<FString, int32> Out = Fixture.Component->GetStringToIntMap(TEXT("Scores"), true);
	TestEqual(TEXT("entry count"), Out.Num(), 2);
	TestEqual(TEXT("alice"), Out.FindRef(TEXT("alice")), 10);
	TestEqual(TEXT("bob"), Out.FindRef(TEXT("bob")), 20);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowGetStringKeyedMapsTest,
	"StoryFlow.MapVariables.GetStringKeyedBoolFloatString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowGetStringKeyedMapsTest::RunTest(const FString& Parameters)
{
	StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init())) { return false; }
	auto& Globals = Fixture.Subsystem->GetGlobalVariables();

	Fixture.AddGlobalMapVariable(TEXT("var_b"), TEXT("Flags"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Boolean);
	Fixture.AddGlobalMapVariable(TEXT("var_f"), TEXT("Weights"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Float);
	Fixture.AddGlobalMapVariable(TEXT("var_s"), TEXT("Labels"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::String);
	Fixture.AddGlobalMapVariable(TEXT("var_img"), TEXT("Portraits"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Image);

	{
		FStoryFlowMapEntry E; E.Key.SetString(TEXT("on")); E.Value.SetBool(true);
		Globals[TEXT("var_b")].Value.SetMap({ E });
	}
	{
		FStoryFlowMapEntry E; E.Key.SetString(TEXT("w")); E.Value.SetFloat(2.5f);
		Globals[TEXT("var_f")].Value.SetMap({ E });
	}
	{
		FStoryFlowMapEntry E; E.Key.SetString(TEXT("greeting")); E.Value.SetString(TEXT("hello"));
		Globals[TEXT("var_s")].Value.SetMap({ E });
	}
	{
		FStoryFlowMapEntry E; E.Key.SetString(TEXT("hero")); E.Value.SetString(TEXT("asset_image_1"));
		Globals[TEXT("var_img")].Value.SetMap({ E });
	}

	const TMap<FString, bool> Bools = Fixture.Component->GetStringToBoolMap(TEXT("Flags"), true);
	TestEqual(TEXT("bool count"), Bools.Num(), 1);
	TestTrue(TEXT("bool on"), Bools.FindRef(TEXT("on")));

	const TMap<FString, float> Floats = Fixture.Component->GetStringToFloatMap(TEXT("Weights"), true);
	TestEqual(TEXT("float count"), Floats.Num(), 1);
	TestEqual(TEXT("float w"), Floats.FindRef(TEXT("w")), 2.5f);

	const TMap<FString, FString> Strings = Fixture.Component->GetStringToStringMap(TEXT("Labels"), true);
	TestEqual(TEXT("string count"), Strings.Num(), 1);
	TestEqual(TEXT("string greeting"), Strings.FindRef(TEXT("greeting")), FString(TEXT("hello")));

	// Image values come back raw (not string-table resolved) through the same string-family getter.
	const TMap<FString, FString> Portraits = Fixture.Component->GetStringToStringMap(TEXT("Portraits"), true);
	TestEqual(TEXT("image count"), Portraits.Num(), 1);
	TestEqual(TEXT("image hero"), Portraits.FindRef(TEXT("hero")), FString(TEXT("asset_image_1")));

	return true;
}

// ============================================================================
// Integer-keyed getters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowGetIntKeyedMapsTest,
	"StoryFlow.MapVariables.GetIntKeyedMaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowGetIntKeyedMapsTest::RunTest(const FString& Parameters)
{
	StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init())) { return false; }
	auto& Globals = Fixture.Subsystem->GetGlobalVariables();

	Fixture.AddGlobalMapVariable(TEXT("var_ii"), TEXT("Lookup"),
		EStoryFlowVariableType::Integer, EStoryFlowVariableType::Integer);
	Fixture.AddGlobalMapVariable(TEXT("var_is"), TEXT("Names"),
		EStoryFlowVariableType::Integer, EStoryFlowVariableType::String);

	{
		FStoryFlowMapEntry E; E.Key.SetInt(1); E.Value.SetInt(100);
		Globals[TEXT("var_ii")].Value.SetMap({ E });
	}
	{
		FStoryFlowMapEntry E; E.Key.SetInt(7); E.Value.SetString(TEXT("seven"));
		Globals[TEXT("var_is")].Value.SetMap({ E });
	}

	const TMap<int32, int32> IntMap = Fixture.Component->GetIntToIntMap(TEXT("Lookup"), true);
	TestEqual(TEXT("int->int count"), IntMap.Num(), 1);
	TestEqual(TEXT("int->int [1]"), IntMap.FindRef(1), 100);

	const TMap<int32, FString> StrMap = Fixture.Component->GetIntToStringMap(TEXT("Names"), true);
	TestEqual(TEXT("int->string count"), StrMap.Num(), 1);
	TestEqual(TEXT("int->string [7]"), StrMap.FindRef(7), FString(TEXT("seven")));

	return true;
}

// ============================================================================
// Ordered keys + wrong-type guard
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowGetMapKeysInOrderTest,
	"StoryFlow.MapVariables.GetMapKeysInOrderPreservesOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowGetMapKeysInOrderTest::RunTest(const FString& Parameters)
{
	StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init())) { return false; }
	using StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld;

	Fixture.AddGlobalMapVariable(TEXT("var_o"), TEXT("Ordered"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Integer);
	Fixture.Subsystem->GetGlobalVariables()[TEXT("var_o")].Value.SetMap(
		{ FScopedStoryFlowWorld::StrIntEntry(TEXT("zebra"), 1),
		  FScopedStoryFlowWorld::StrIntEntry(TEXT("apple"), 2),
		  FScopedStoryFlowWorld::StrIntEntry(TEXT("mango"), 3) });

	const TArray<FString> Keys = Fixture.Component->GetMapKeysInOrder(TEXT("Ordered"), true);
	TestEqual(TEXT("key count"), Keys.Num(), 3);
	if (Keys.Num() == 3)
	{
		TestEqual(TEXT("key[0]"), Keys[0], FString(TEXT("zebra")));
		TestEqual(TEXT("key[1]"), Keys[1], FString(TEXT("apple")));
		TestEqual(TEXT("key[2]"), Keys[2], FString(TEXT("mango")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowGetMapWrongTypeTest,
	"StoryFlow.MapVariables.WrongValueTypeReturnsEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowGetMapWrongTypeTest::RunTest(const FString& Parameters)
{
	StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init())) { return false; }
	using StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld;

	// A string->int map read through the string-value getter must refuse, not coerce.
	Fixture.AddGlobalMapVariable(TEXT("var_wt"), TEXT("Scores"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Integer);
	Fixture.Subsystem->GetGlobalVariables()[TEXT("var_wt")].Value.SetMap(
		{ FScopedStoryFlowWorld::StrIntEntry(TEXT("alice"), 10) });

	AddExpectedError(TEXT("does not have string-family values"), EAutomationExpectedErrorFlags::Contains, 1);
	const TMap<FString, FString> Out = Fixture.Component->GetStringToStringMap(TEXT("Scores"), true);
	TestEqual(TEXT("wrong-value-type read yields empty"), Out.Num(), 0);
	return true;
}

// ============================================================================
// Setters (write path), verified by round trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSetMapsRoundTripTest,
	"StoryFlow.MapVariables.SetMapsRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowSetMapsRoundTripTest::RunTest(const FString& Parameters)
{
	StoryFlowMapVariableTestHelpers::FScopedStoryFlowWorld Fixture;
	if (!TestTrue(TEXT("fixture initialized"), Fixture.Init())) { return false; }

	Fixture.AddGlobalMapVariable(TEXT("var_si"), TEXT("Scores"),
		EStoryFlowVariableType::String, EStoryFlowVariableType::Integer);
	Fixture.AddGlobalMapVariable(TEXT("var_is"), TEXT("Names"),
		EStoryFlowVariableType::Integer, EStoryFlowVariableType::String);

	TMap<FString, int32> In; In.Add(TEXT("alice"), 10); In.Add(TEXT("bob"), 20);
	Fixture.Component->SetStringToIntMap(TEXT("Scores"), In, true);
	const TMap<FString, int32> ScoresBack = Fixture.Component->GetStringToIntMap(TEXT("Scores"), true);
	TestEqual(TEXT("scores count"), ScoresBack.Num(), 2);
	TestEqual(TEXT("scores alice"), ScoresBack.FindRef(TEXT("alice")), 10);

	TMap<int32, FString> InNames; InNames.Add(7, TEXT("seven"));
	Fixture.Component->SetIntToStringMap(TEXT("Names"), InNames, true);
	const TMap<int32, FString> NamesBack = Fixture.Component->GetIntToStringMap(TEXT("Names"), true);
	TestEqual(TEXT("names count"), NamesBack.Num(), 1);
	TestEqual(TEXT("names 7"), NamesBack.FindRef(7), FString(TEXT("seven")));

	// Overwrite replaces, not merges
	TMap<FString, int32> Replacement; Replacement.Add(TEXT("carol"), 30);
	Fixture.Component->SetStringToIntMap(TEXT("Scores"), Replacement, true);
	TestEqual(TEXT("overwrite count"), Fixture.Component->GetStringToIntMap(TEXT("Scores"), true).Num(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
