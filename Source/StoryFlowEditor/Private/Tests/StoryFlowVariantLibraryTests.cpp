// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowVariantLibrary.h"

/**
 * Tests for UStoryFlowVariantLibrary, the Blueprint accessor layer over
 * FStoryFlowVariant.
 *
 * The variant's fields are private (no Break node), so these pure functions are
 * the only Blueprint route into values surfaced as variants — most notably the
 * dialogue state's Character Variables map, where Find returns a variant that
 * was previously a dead end in Blueprint.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.VariantLibrary", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.VariantLibrary" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

// ============================================================================
// Unset variant (the failed-Find case)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVariantLibraryUnsetTest,
	"StoryFlow.VariantLibrary.UnsetVariantReportsInvalidAndReturnsDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowVariantLibraryUnsetTest::RunTest(const FString& Parameters)
{
	FStoryFlowVariant Unset;

	TestFalse(TEXT("Unset variant is not valid"), UStoryFlowVariantLibrary::IsVariantValid(Unset));
	TestEqual(TEXT("Unset variant type is None"), UStoryFlowVariantLibrary::GetVariantType(Unset), EStoryFlowVariableType::None);
	TestEqual(TEXT("Bool getter falls back to default"), UStoryFlowVariantLibrary::GetVariantAsBool(Unset, true), true);
	TestEqual(TEXT("Int getter falls back to default"), UStoryFlowVariantLibrary::GetVariantAsInt(Unset, 42), 42);
	TestEqual(TEXT("Float getter falls back to default"), UStoryFlowVariantLibrary::GetVariantAsFloat(Unset, 1.5f), 1.5f);
	TestEqual(TEXT("String getter falls back to default"), UStoryFlowVariantLibrary::GetVariantAsString(Unset, TEXT("fallback")), FString(TEXT("fallback")));
	TestEqual(TEXT("Display string is empty"), UStoryFlowVariantLibrary::GetVariantDisplayString(Unset), FString(TEXT("")));
	TestEqual(TEXT("Array getter is empty"), UStoryFlowVariantLibrary::GetVariantArray(Unset).Num(), 0);
	TestFalse(TEXT("Unset variant is not a map"), UStoryFlowVariantLibrary::IsVariantMap(Unset));
	return true;
}

// ============================================================================
// Scalar getters: matching type reads the value, mismatched type reads the default
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVariantLibraryScalarTest,
	"StoryFlow.VariantLibrary.ScalarGettersReadMatchingTypeAndDefaultOtherwise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowVariantLibraryScalarTest::RunTest(const FString& Parameters)
{
	const FStoryFlowVariant BoolVariant = FStoryFlowVariant::FromBool(true);
	const FStoryFlowVariant IntVariant = FStoryFlowVariant::FromInt(7);
	const FStoryFlowVariant FloatVariant = FStoryFlowVariant::FromFloat(2.5f);
	const FStoryFlowVariant StringVariant = FStoryFlowVariant::FromString(TEXT("Crimson"));

	// Matching types
	TestTrue(TEXT("Bool variant is valid"), UStoryFlowVariantLibrary::IsVariantValid(BoolVariant));
	TestEqual(TEXT("Bool variant type"), UStoryFlowVariantLibrary::GetVariantType(BoolVariant), EStoryFlowVariableType::Boolean);
	TestEqual(TEXT("Bool variant reads true"), UStoryFlowVariantLibrary::GetVariantAsBool(BoolVariant, false), true);
	TestEqual(TEXT("Int variant reads 7"), UStoryFlowVariantLibrary::GetVariantAsInt(IntVariant, 0), 7);
	TestEqual(TEXT("Float variant reads 2.5"), UStoryFlowVariantLibrary::GetVariantAsFloat(FloatVariant, 0.0f), 2.5f);
	TestEqual(TEXT("String variant reads Crimson"), UStoryFlowVariantLibrary::GetVariantAsString(StringVariant, TEXT("")), FString(TEXT("Crimson")));

	// Mismatched types fall back to the caller's default
	TestEqual(TEXT("Bool getter on string variant defaults"), UStoryFlowVariantLibrary::GetVariantAsBool(StringVariant, true), true);
	TestEqual(TEXT("Int getter on float variant defaults"), UStoryFlowVariantLibrary::GetVariantAsInt(FloatVariant, -1), -1);
	TestEqual(TEXT("String getter on int variant defaults"), UStoryFlowVariantLibrary::GetVariantAsString(IntVariant, TEXT("none")), FString(TEXT("none")));

	// Integer-to-float coercion (whole-number JSON imports as Integer; float reads must see 7.0, not the default)
	TestEqual(TEXT("Float getter coerces int variant"), UStoryFlowVariantLibrary::GetVariantAsFloat(IntVariant, -1.0f), 7.0f);

	// Display strings
	TestEqual(TEXT("Bool display string"), UStoryFlowVariantLibrary::GetVariantDisplayString(BoolVariant), FString(TEXT("true")));
	TestEqual(TEXT("Int display string"), UStoryFlowVariantLibrary::GetVariantDisplayString(IntVariant), FString(TEXT("7")));
	TestEqual(TEXT("String display string"), UStoryFlowVariantLibrary::GetVariantDisplayString(StringVariant), FString(TEXT("Crimson")));
	return true;
}

// ============================================================================
// String-family types (enum, image, audio, character) all read through GetVariantAsString
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVariantLibraryStringFamilyTest,
	"StoryFlow.VariantLibrary.StringFamilyTypesReadAsString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowVariantLibraryStringFamilyTest::RunTest(const FString& Parameters)
{
	FStoryFlowVariant EnumVariant;
	EnumVariant.SetEnum(TEXT("banana"));

	TestEqual(TEXT("Enum variant type"), UStoryFlowVariantLibrary::GetVariantType(EnumVariant), EStoryFlowVariableType::Enum);
	TestEqual(TEXT("Enum variant reads as string"), UStoryFlowVariantLibrary::GetVariantAsString(EnumVariant, TEXT("")), FString(TEXT("banana")));

	// Image / audio / character variants store asset keys in the string slot; the
	// library must surface them the same way GetString does.
	FStoryFlowVariant AssetVariant;
	AssetVariant.SetString(TEXT("portrait_key"));
	TestEqual(TEXT("String-stored asset key reads back"), UStoryFlowVariantLibrary::GetVariantAsString(AssetVariant, TEXT("")), FString(TEXT("portrait_key")));
	return true;
}

// ============================================================================
// Array variants
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVariantLibraryArrayTest,
	"StoryFlow.VariantLibrary.ArrayVariantExposesElements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowVariantLibraryArrayTest::RunTest(const FString& Parameters)
{
	FStoryFlowVariant ArrayVariant;
	ArrayVariant.SetArray({ FStoryFlowVariant::FromInt(1), FStoryFlowVariant::FromInt(2), FStoryFlowVariant::FromInt(3) });

	const TArray<FStoryFlowVariant> Elements = UStoryFlowVariantLibrary::GetVariantArray(ArrayVariant);
	TestEqual(TEXT("Array has 3 elements"), Elements.Num(), 3);
	if (Elements.Num() == 3)
	{
		TestEqual(TEXT("Element 0"), UStoryFlowVariantLibrary::GetVariantAsInt(Elements[0], 0), 1);
		TestEqual(TEXT("Element 1"), UStoryFlowVariantLibrary::GetVariantAsInt(Elements[1], 0), 2);
		TestEqual(TEXT("Element 2"), UStoryFlowVariantLibrary::GetVariantAsInt(Elements[2], 0), 3);
	}

	TestEqual(TEXT("Scalar variant yields empty array"), UStoryFlowVariantLibrary::GetVariantArray(FStoryFlowVariant::FromInt(5)).Num(), 0);
	return true;
}

// ============================================================================
// Map variants
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVariantLibraryMapTest,
	"StoryFlow.VariantLibrary.MapVariantExposesKeysAndValuesInEntryOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowVariantLibraryMapTest::RunTest(const FString& Parameters)
{
	TArray<FStoryFlowMapEntry> Entries;
	Entries.Add({ FStoryFlowVariant::FromString(TEXT("first")), FStoryFlowVariant::FromInt(10) });
	Entries.Add({ FStoryFlowVariant::FromString(TEXT("second")), FStoryFlowVariant::FromInt(20) });

	FStoryFlowVariant MapVariant;
	MapVariant.SetMap(Entries);

	TestTrue(TEXT("Map variant reports as map"), UStoryFlowVariantLibrary::IsVariantMap(MapVariant));
	TestEqual(TEXT("Map variant type"), UStoryFlowVariantLibrary::GetVariantType(MapVariant), EStoryFlowVariableType::Map);

	TArray<FStoryFlowVariant> Keys;
	TArray<FStoryFlowVariant> Values;
	UStoryFlowVariantLibrary::GetVariantMap(MapVariant, Keys, Values);

	TestEqual(TEXT("2 keys"), Keys.Num(), 2);
	TestEqual(TEXT("2 values"), Values.Num(), 2);
	if (Keys.Num() == 2 && Values.Num() == 2)
	{
		TestEqual(TEXT("Key order preserved (0)"), UStoryFlowVariantLibrary::GetVariantAsString(Keys[0], TEXT("")), FString(TEXT("first")));
		TestEqual(TEXT("Key order preserved (1)"), UStoryFlowVariantLibrary::GetVariantAsString(Keys[1], TEXT("")), FString(TEXT("second")));
		TestEqual(TEXT("Value pairs with key (0)"), UStoryFlowVariantLibrary::GetVariantAsInt(Values[0], 0), 10);
		TestEqual(TEXT("Value pairs with key (1)"), UStoryFlowVariantLibrary::GetVariantAsInt(Values[1], 0), 20);
	}

	// Non-map input must clear the out arrays, not leave stale data
	TArray<FStoryFlowVariant> StaleKeys;
	StaleKeys.Add(FStoryFlowVariant::FromInt(99));
	TArray<FStoryFlowVariant> StaleValues;
	StaleValues.Add(FStoryFlowVariant::FromInt(99));
	UStoryFlowVariantLibrary::GetVariantMap(FStoryFlowVariant::FromInt(5), StaleKeys, StaleValues);
	TestEqual(TEXT("Non-map clears keys"), StaleKeys.Num(), 0);
	TestEqual(TEXT("Non-map clears values"), StaleValues.Num(), 0);
	return true;
}

// ============================================================================
// Make functions round-trip through the getters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVariantLibraryMakeTest,
	"StoryFlow.VariantLibrary.MakeFunctionsRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowVariantLibraryMakeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Bool round-trip"), UStoryFlowVariantLibrary::GetVariantAsBool(UStoryFlowVariantLibrary::MakeVariantFromBool(true), false), true);
	TestEqual(TEXT("Int round-trip"), UStoryFlowVariantLibrary::GetVariantAsInt(UStoryFlowVariantLibrary::MakeVariantFromInt(-3), 0), -3);
	TestEqual(TEXT("Float round-trip"), UStoryFlowVariantLibrary::GetVariantAsFloat(UStoryFlowVariantLibrary::MakeVariantFromFloat(0.25f), 0.0f), 0.25f);
	TestEqual(TEXT("String round-trip"), UStoryFlowVariantLibrary::GetVariantAsString(UStoryFlowVariantLibrary::MakeVariantFromString(TEXT("hello")), TEXT("")), FString(TEXT("hello")));

	const FStoryFlowVariant EnumVariant = UStoryFlowVariantLibrary::MakeVariantFromEnum(TEXT("cherry"));
	TestEqual(TEXT("Enum make sets enum type"), UStoryFlowVariantLibrary::GetVariantType(EnumVariant), EStoryFlowVariableType::Enum);
	TestEqual(TEXT("Enum round-trip"), UStoryFlowVariantLibrary::GetVariantAsString(EnumVariant, TEXT("")), FString(TEXT("cherry")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
