// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Shared access to the CROSS-ENGINE GOLDEN FIXTURES in TestContent/engine-contract/.
 *
 * Those files are checked in VERBATIM from the editor repo and generated from the HTML runtime
 * (src/renderer/runtime/runtime-data-assets.js), the normative implementation. Unity and Godot
 * consume the same bytes, so an engine that drifts from the reference fails in its own test suite
 * rather than in someone's game.
 *
 * Header-only, and included by more than one test file, so everything here is `inline` — the
 * editor module is a unity build and a non-inline definition would collide.
 *
 * Same precedent as StoryFlowTagAccumulator.h / StoryFlowWidgetSpy.h: a test-only header beside
 * the tests that share it, never in Public.
 */
namespace StoryFlowEngineContract
{
	/** Plugin-relative home of the shared fixtures. Empty when the plugin cannot be located. */
	inline FString FixturePath(const FString& FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("StoryFlowPlugin"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("TestContent"), TEXT("engine-contract"), FileName);
	}

	/** Load one fixture as raw JSON text. False when it is missing or unreadable. */
	inline bool LoadFixtureText(const FString& FileName, FString& OutJson)
	{
		const FString Path = FixturePath(FileName);
		return !Path.IsEmpty() && FFileHelper::LoadFileToString(OutJson, *Path);
	}

	/** Load and parse one fixture. Null when it is missing or malformed. */
	inline TSharedPtr<FJsonObject> LoadFixture(const FString& FileName)
	{
		FString JsonString;
		if (!LoadFixtureText(FileName, JsonString))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject))
		{
			return nullptr;
		}
		return JsonObject;
	}

	// ========================================================================
	// data-assets-seed.json — the ids, annotated with where they live
	// ========================================================================
	// Without these annotations every assertion below reads as unexplained hex. The seed family is
	// base -> child -> grandchild; a variable's HOME is where it is declared, and an override at a
	// level shadows that declaration for that level's subtree.

	/** CreatureBase, the root */
	inline const TCHAR* BaseId = TEXT("da_0a1b2c3d4e5f60718293a4b5c6d7e8f9");
	/** Goblin, child of the base */
	inline const TCHAR* ChildId = TEXT("da_1b2c3d4e5f60718293a4b5c6d7e8f90a");
	/** GoblinChieftain, child of the child */
	inline const TCHAR* GrandChildId = TEXT("da_2c3d4e5f60718293a4b5c6d7e8f90a1b");
	/** An id no level of the seed carries — a dead reference */
	inline const TCHAR* AbsentId = TEXT("da_ff00ff00ff00ff00ff00ff00ff00ff00");

	/** boolean, declared and valued TRUE on the base, overridden nowhere */
	inline const TCHAR* AliveId = TEXT("7f3a1c9e4b2d40518a6f0c3e7d1b5a29");
	/** integer, base 100, overridden to 150 on the child */
	inline const TCHAR* HpId = TEXT("2e8b6d0a1f4c47d3b95e2a70c6f81d34");
	/** float, declared on the base AND overridden by the base itself (the §9.1 root override) */
	inline const TCHAR* SpeedId = TEXT("9c4f7e25a3b84a19bd60e2f7c81a5d03");
	/** enum, base "Grunt", overridden to "Elite" on the child */
	inline const TCHAR* RankId = TEXT("d0a37c65e91b4f28b4c1a5e7028d63f9");
	/** string array, base ["mob","melee"], overridden to ["mob","elite"] on the child */
	inline const TCHAR* TagsId = TEXT("c58e2f13a0d64c9b871e3f05d2a76b48");
	/** map<string,integer>, base 2 entries, overridden with 2 other entries on the grandchild */
	inline const TCHAR* LootId = TEXT("6d0f39a8b21e47c5903af8d61c72e504");
	/** category, declared on the base — never resolves, and can never be written */
	inline const TCHAR* LoreId = TEXT("ae41b70c95d84e2fa3608c1b5f2d97e0");
	/** declared by no level of the chain at all */
	inline const TCHAR* NowhereId = TEXT("4c9a1e07b38f42d6a1057e2c93bd48f0");

	// ========================================================================
	// Fixture <-> engine-value comparison, shared by every fixture-driven suite
	// ========================================================================
	// These live here rather than in one test file because THREE suites compare values against
	// the same fixtures now (resolution, nodes, save) and a second copy of the comparison rules
	// is a second chance to disagree with the reference about what "equal" means.

	/**
	 * True for the string family — every one of these stores its value in StringValue.
	 *
	 * FIVE members, Enum included, because this asks about STORAGE: a resolved variant is compared
	 * against a fixture's bare JSON string, and an enum's value lives in StringValue like the rest.
	 * Deliberately NOT the same set as DataAssetAccessorTypeMatches in StoryFlowComponent.cpp,
	 * which has four and excludes Enum because it asks which DECLARATION a typed Blueprint
	 * accessor may reach — and Enum has an accessor of its own there. Same-looking lists, opposite
	 * questions; unifying them would break one side or the other.
	 */
	inline bool IsStringFamily(EStoryFlowVariableType Type)
	{
		return Type == EStoryFlowVariableType::String || Type == EStoryFlowVariableType::Enum
			|| Type == EStoryFlowVariableType::Image || Type == EStoryFlowVariableType::Audio
			|| Type == EStoryFlowVariableType::Character;
	}

	/**
	 * Compare a resolved variant against a fixture JSON value.
	 *
	 * Dispatch is on the JSON side, not the variant's type, because a resolved variant carries
	 * the ELEMENT type for arrays (FStoryFlowVariant::SetArray infers it) and the string family
	 * collapses five declared types onto one storage field. Contract §9.1 also pins that JSON
	 * cannot express 0.0 distinctly, so numbers are compared numerically across Integer/Float.
	 */
	inline bool VariantMatchesJson(FAutomationTestBase& Test, const FString& Label, const FStoryFlowVariant& Variant, const TSharedPtr<FJsonValue>& Expected)
	{
		// A missing field is a malformed fixture — fail the test rather than null-deref.
		// Guarded here, not only at the call sites, because the map-entry recursion below
		// reaches for "key"/"value" on entries this function does not otherwise validate.
		if (!Expected.IsValid())
		{
			Test.AddError(Label + TEXT(": the fixture record is missing this value"));
			return false;
		}

		switch (Expected->Type)
		{
		case EJson::Boolean:
			// TestTrue, not TestEqual: FAutomationTestBase has no bool TestEqual overload
			return Test.TestTrue(Label + TEXT(" (boolean)"),
				Variant.GetType() == EStoryFlowVariableType::Boolean && Variant.GetBool() == Expected->AsBool());

		case EJson::Number:
		{
			double Actual = 0.0;
			if (Variant.GetType() == EStoryFlowVariableType::Integer)
			{
				Actual = static_cast<double>(Variant.GetInt());
			}
			else if (Variant.GetType() == EStoryFlowVariableType::Float)
			{
				Actual = static_cast<double>(Variant.GetFloat());
			}
			else
			{
				Test.AddError(FString::Printf(TEXT("%s: expected a number, got variant type %d"), *Label, static_cast<int32>(Variant.GetType())));
				return false;
			}
			// A double literal, not UE_KINDA_SMALL_NUMBER: that macro is a float, and the mixed
			// argument list makes TestNearlyEqual's float/double/FVector overloads ambiguous.
			// The slack covers the seed's float32 round trip, nothing more.
			return Test.TestNearlyEqual(Label + TEXT(" (number)"), Actual, Expected->AsNumber(), 1.e-4);
		}

		case EJson::String:
			if (!IsStringFamily(Variant.GetType()))
			{
				Test.AddError(FString::Printf(TEXT("%s: expected a string-family value, got variant type %d"), *Label, static_cast<int32>(Variant.GetType())));
				return false;
			}
			return Test.TestEqual(Label + TEXT(" (string)"), Variant.GetString(), Expected->AsString());

		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& ExpectedItems = Expected->AsArray();

			// Map values are ORDERED ENTRY LISTS (contract §2.1) — same JSON shape as an array,
			// told apart by the variant the resolver produced. Entry ORDER is asserted, because
			// it is authored and observable.
			if (Variant.IsMap())
			{
				const TArray<FStoryFlowMapEntry>& Entries = Variant.GetMap();
				if (!Test.TestEqual(Label + TEXT(" (map entry count)"), Entries.Num(), ExpectedItems.Num()))
				{
					return false;
				}
				bool bOk = true;
				for (int32 Index = 0; Index < Entries.Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> ExpectedEntry = ExpectedItems[Index]->AsObject();
					if (!ExpectedEntry.IsValid())
					{
						Test.AddError(FString::Printf(TEXT("%s: fixture map entry %d is not an object"), *Label, Index));
						bOk = false;
						continue;
					}
					const FString EntryLabel = FString::Printf(TEXT("%s[%d]"), *Label, Index);
					bOk &= VariantMatchesJson(Test, EntryLabel + TEXT(".key"), Entries[Index].Key, ExpectedEntry->TryGetField(TEXT("key")));
					bOk &= VariantMatchesJson(Test, EntryLabel + TEXT(".value"), Entries[Index].Value, ExpectedEntry->TryGetField(TEXT("value")));
				}
				return bOk;
			}

			const TArray<FStoryFlowVariant>& Items = Variant.GetArray();
			if (!Test.TestEqual(Label + TEXT(" (array count)"), Items.Num(), ExpectedItems.Num()))
			{
				return false;
			}
			bool bOk = true;
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				bOk &= VariantMatchesJson(Test, FString::Printf(TEXT("%s[%d]"), *Label, Index), Items[Index], ExpectedItems[Index]);
			}
			return bOk;
		}

		default:
			Test.AddError(FString::Printf(TEXT("%s: unsupported fixture value type"), *Label));
			return false;
		}
	}

	/**
	 * Deep JSON equality against an expected document, reported field by field. Moved here from
	 * the .sfd save suite when the character save pins started comparing whole save sections too —
	 * a second copy of the comparison rules is a second chance to disagree about what "equal" means.
	 *
	 * Structural, not textual: key ORDER inside an object is a TMap iteration detail and carries
	 * no meaning, while ARRAY order does (map entry lists and arrays are both ordered by contract
	 * §2.1) and is compared positionally. Value TYPES are compared too — a `42` persisted as
	 * `"42"` is exactly the kind of drift the save suites exist to catch.
	 */
	inline bool JsonEquals(FAutomationTestBase& Test, const FString& Label, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
	{
		if (!Expected.IsValid())
		{
			Test.AddError(Label + TEXT(": the fixture carries no value here"));
			return false;
		}
		if (!Actual.IsValid())
		{
			Test.AddError(Label + TEXT(": the save carries no value here"));
			return false;
		}
		if (Actual->Type != Expected->Type)
		{
			Test.AddError(FString::Printf(TEXT("%s: the save holds JSON type %d where the fixture holds %d"),
				*Label, static_cast<int32>(Actual->Type), static_cast<int32>(Expected->Type)));
			return false;
		}

		switch (Expected->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> ActualObj = Actual->AsObject();
			const TSharedPtr<FJsonObject> ExpectedObj = Expected->AsObject();
			if (!ActualObj.IsValid() || !ExpectedObj.IsValid())
			{
				Test.AddError(Label + TEXT(": an object failed to read back"));
				return false;
			}
			bool bOk = Test.TestEqual(Label + TEXT(" (key count)"), ActualObj->Values.Num(), ExpectedObj->Values.Num());
			for (const auto& Pair : ExpectedObj->Values)
			{
				const TSharedPtr<FJsonValue> ActualField = ActualObj->TryGetField(Pair.Key);
				if (!ActualField.IsValid())
				{
					Test.AddError(FString::Printf(TEXT("%s: the save is missing '%s'"), *Label, *Pair.Key));
					bOk = false;
					continue;
				}
				bOk &= JsonEquals(Test, Label + TEXT(".") + Pair.Key, ActualField, Pair.Value);
			}
			for (const auto& Pair : ActualObj->Values)
			{
				if (!ExpectedObj->HasField(Pair.Key))
				{
					Test.AddError(FString::Printf(TEXT("%s: the save carries '%s', which the fixture does not"), *Label, *Pair.Key));
					bOk = false;
				}
			}
			return bOk;
		}

		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& ActualItems = Actual->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& ExpectedItems = Expected->AsArray();
			if (!Test.TestEqual(Label + TEXT(" (element count)"), ActualItems.Num(), ExpectedItems.Num()))
			{
				return false;
			}
			bool bOk = true;
			for (int32 Index = 0; Index < ExpectedItems.Num(); ++Index)
			{
				bOk &= JsonEquals(Test, FString::Printf(TEXT("%s[%d]"), *Label, Index), ActualItems[Index], ExpectedItems[Index]);
			}
			return bOk;
		}

		case EJson::Number:
			// Numeric, not textual: contract §9.1 pins that JSON cannot express 0.0 distinctly,
			// and a float32 round trip is not bit-exact against a JSON double.
			return Test.TestNearlyEqual(Label, Actual->AsNumber(), Expected->AsNumber(), 1.e-4);

		case EJson::Boolean:
			// TestTrue, not TestEqual: FAutomationTestBase has no bool TestEqual overload
			return Test.TestTrue(Label, Actual->AsBool() == Expected->AsBool());

		case EJson::String:
			return Test.TestEqual(Label, Actual->AsString(), Expected->AsString());

		case EJson::Null:
			return true;

		default:
			Test.AddError(Label + TEXT(": unsupported JSON value"));
			return false;
		}
	}

	/**
	 * Run one fixture resolution table (data-assets-resolution.json's `resolutions`, or
	 * data-assets-writes.json's `postWriteResolutions`) against the store.
	 *
	 * Returns the number of records a value or unset COMPARISON actually ran for — not the loop
	 * count. A record dropped for a structural reason (not an object, the resolved flag did not
	 * match, no value where the fixture claims one) is deliberately NOT counted, so the caller's
	 * "every record was compared" assertion can fail on its own rather than being a tautology.
	 */
	inline int32 AssertResolutionTable(FAutomationTestBase& Test, const StoryFlowDataAssets::FSeed& Seed,
		const StoryFlowDataAssets::FOverlay& Overlay, const TArray<TSharedPtr<FJsonValue>>& Records, const TCHAR* TableName)
	{
		int32 Compared = 0;
		for (const TSharedPtr<FJsonValue>& RecordValue : Records)
		{
			const TSharedPtr<FJsonObject> Record = RecordValue->AsObject();
			if (!Record.IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s: a record is not an object"), TableName));
				continue;
			}

			const FString AssetId = Record->GetStringField(TEXT("assetId"));
			const FString VariableId = Record->GetStringField(TEXT("variableId"));
			FString VariableName;
			Record->TryGetStringField(TEXT("variableName"), VariableName);
			const bool bExpectResolved = Record->GetBoolField(TEXT("resolved"));
			const FString Label = FString::Printf(TEXT("%s resolve(%s, %s /* %s */)"), TableName, *AssetId, *VariableId, *VariableName);

			FStoryFlowVariant Value;
			const bool bResolved = StoryFlowDataAssets::TryResolve(Seed, Overlay, AssetId, VariableId, Value);

			if (!Test.TestTrue(Label + FString::Printf(TEXT(" resolves (expected %s)"), bExpectResolved ? TEXT("true") : TEXT("false")), bResolved == bExpectResolved))
			{
				continue;
			}
			if (!bExpectResolved)
			{
				// Contract §9.1: an unresolvable read is an UNSET variant, and a category
				// declaration is deliberately indistinguishable from an undeclared id here
				Test.TestFalse(Label + TEXT(" hands back an unset variant"), Value.IsValid());
				++Compared;
				continue;
			}

			const TSharedPtr<FJsonValue> ExpectedValue = Record->TryGetField(TEXT("value"));
			if (!ExpectedValue.IsValid())
			{
				Test.AddError(Label + TEXT(": the fixture record is marked resolved but carries no value"));
				continue;
			}
			VariantMatchesJson(Test, Label, Value, ExpectedValue);
			++Compared;
		}
		return Compared;
	}

	/**
	 * Turn a fixture write's JSON value into the variant a node arm would hand the store.
	 *
	 * Typed against the chain's DECLARATION, exactly as the executor is: at runtime the value
	 * arrives on a typed pin, so an entry list is a map and a bare string is whatever the
	 * declaration says it is. Falls back to JSON-shape inference for a write the chain does not
	 * declare — that write is going to be refused anyway, and its value never reaches storage.
	 */
	inline FStoryFlowVariant WriteValueFromJson(const StoryFlowDataAssets::FSeed& Seed, const FString& AssetId, const FString& VariableId, const TSharedPtr<FJsonValue>& Value)
	{
		const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclaration(Seed, AssetId, VariableId);
		if (Declaration && Declaration->Type == EStoryFlowVariableType::Map)
		{
			const TArray<TSharedPtr<FJsonValue>>* EntriesJson = nullptr;
			TArray<FStoryFlowMapEntry> Entries;
			if (Value->TryGetArray(EntriesJson))
			{
				for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesJson)
				{
					const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
					if (!EntryObject.IsValid())
					{
						continue;
					}
					FStoryFlowMapEntry Entry;
					if (Declaration->KeyType == EStoryFlowVariableType::Integer)
					{
						Entry.Key.SetInt(static_cast<int32>(EntryObject->GetNumberField(TEXT("key"))));
					}
					else
					{
						Entry.Key.SetString(EntryObject->GetStringField(TEXT("key")));
					}
					const TSharedPtr<FJsonValue> EntryValueField = EntryObject->TryGetField(TEXT("value"));
					if (EntryValueField.IsValid() && Declaration->ValueType == EStoryFlowVariableType::Integer)
					{
						Entry.Value.SetInt(static_cast<int32>(EntryValueField->AsNumber()));
					}
					else if (EntryValueField.IsValid())
					{
						Entry.Value.SetString(EntryValueField->AsString());
					}
					Entries.Add(Entry);
				}
			}
			FStoryFlowVariant MapValue;
			MapValue.SetMap(Entries);
			return MapValue;
		}

		FStoryFlowVariant Result;
		switch (Value->Type)
		{
		case EJson::Boolean:
			Result.SetBool(Value->AsBool());
			break;
		case EJson::Number:
			if (Declaration && Declaration->Type == EStoryFlowVariableType::Float)
			{
				Result.SetFloat(static_cast<float>(Value->AsNumber()));
			}
			else
			{
				Result.SetInt(static_cast<int32>(Value->AsNumber()));
			}
			break;
		case EJson::String:
			// SetEnum where the chain declares one: an enum value is NOT a string in the store,
			// and an overlay entry that disagreed with the file value it shadows would be
			// invisible to a read and visible in the save key.
			if (Declaration && Declaration->Type == EStoryFlowVariableType::Enum)
			{
				Result.SetEnum(Value->AsString());
			}
			else
			{
				Result.SetString(Value->AsString());
			}
			break;
		case EJson::Array:
		{
			TArray<FStoryFlowVariant> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				FStoryFlowVariant Element;
				Element.SetString(Item->AsString());
				Items.Add(Element);
			}
			// The ELEMENT-TYPE overload: a write with an empty list would otherwise be typed
			// None, and the save key's shape depends on the type surviving.
			Result.SetArray(Items, Declaration ? Declaration->Type : EStoryFlowVariableType::String);
			break;
		}
		default:
			break;
		}
		return Result;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
