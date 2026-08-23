// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "StoryFlowRuntime.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

/**
 * The Data Asset (.sfd) store and chain resolver against the SHARED GOLDEN FIXTURES — the
 * cross-engine parity gate for contract §4 and §5.
 *
 * TestContent/engine-contract/data-assets-*.json are checked in VERBATIM from the editor repo
 * and were GENERATED from the HTML runtime (src/renderer/runtime/runtime-data-assets.js), the
 * normative implementation. Every engine's plugin consumes the same files, so a store that
 * drifts from the reference fails here rather than in someone's game.
 *
 * WHICH FIXTURES THIS FILE OWNS, and which it deliberately leaves alone:
 *  - data-assets-seed.json ............ read here (the seed family every test below builds on)
 *  - data-assets-resolution.json ...... read here (§4, empty overlay)
 *  - data-assets-writes.json .......... `writes` + `postWriteResolutions` read here (§5).
 *      Its `saveKey` member belongs to TASK U3, which adds the sparse `dataAssets` save key —
 *      the Writes test below deliberately stops at the post-write resolution table.
 *  - data-assets-degraded.json ........ belongs to TASK U2, which adds the accessor node arms
 *      and the §6 degraded ladder. Nothing here reads it, and that is not an oversight.
 *
 * The seed is loaded through the REAL IMPORTER, not a parser written for the test: import is
 * where the seed's shape (ordered map entry lists, typed overrides, skipped category rows)
 * turns into engine data, and a test that parsed the fixture itself would happily pass while
 * the importer mangled it.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.DataAssets", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.DataAssets.Resolution" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowDataAssetTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowDataAssetTests");

	// The seed fixture's three assets, base -> child -> grandchild (contract §9.1)
	const TCHAR* BaseId = TEXT("da_0a1b2c3d4e5f60718293a4b5c6d7e8f9");
	const TCHAR* ChildId = TEXT("da_1b2c3d4e5f60718293a4b5c6d7e8f90a");
	const TCHAR* GrandChildId = TEXT("da_2c3d4e5f60718293a4b5c6d7e8f90a1b");
	const TCHAR* AbsentId = TEXT("da_ff00ff00ff00ff00ff00ff00ff00ff00");

	// The seed fixture's variable ids, each annotated with where it lives on the chain —
	// without this the assertions below are unreadable hex.
	/** boolean, declared on the base, overridden nowhere */
	const TCHAR* AliveId = TEXT("7f3a1c9e4b2d40518a6f0c3e7d1b5a29");
	/** float, declared on the base AND overridden by the base itself (the §9.1 root override) */
	const TCHAR* SpeedId = TEXT("9c4f7e25a3b84a19bd60e2f7c81a5d03");
	/** string array, declared on the base, overridden on the child */
	const TCHAR* TagsId = TEXT("c58e2f13a0d64c9b871e3f05d2a76b48");
	/** map<string,integer>, declared on the base, overridden on the grandchild */
	const TCHAR* LootId = TEXT("6d0f39a8b21e47c5903af8d61c72e504");
	/** category, declared on the base — never resolves, and can never be written */
	const TCHAR* LoreId = TEXT("ae41b70c95d84e2fa3608c1b5f2d97e0");
	/** declared by no level of the chain at all */
	const TCHAR* NowhereId = TEXT("4c9a1e07b38f42d6a1057e2c93bd48f0");

	FString FixtureBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowDataAssetFixture"));
	}

	/** The plugin-relative home of the shared cross-engine fixtures. */
	FString GoldenFixturePath(const FString& FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("StoryFlowPlugin"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("TestContent"), TEXT("engine-contract"), FileName);
	}

	/** Load one golden fixture. Mirrors UStoryFlowImporter::LoadJsonFile, which is private. */
	TSharedPtr<FJsonObject> LoadGoldenFixture(const FString& FileName)
	{
		const FString Path = GoldenFixturePath(FileName);
		FString JsonString;
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(JsonString, *Path))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject))
		{
			return nullptr;
		}
		return JsonObject;
	}

	/** The smallest project.json ImportProject accepts, so a seed can ride a real import. */
	bool WriteMinimalProject(const FString& Dir)
	{
		return FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")));
	}

	/**
	 * Import a build folder holding the given data-assets.json body, and hand back the seed the
	 * subsystem would install (StoryFlowDataAssets::BuildSeed — the same call SetProject makes).
	 */
	UStoryFlowProjectAsset* ImportSeedJson(const FString& DataAssetsJson, StoryFlowDataAssets::FSeed& OutSeed)
	{
		const FString Dir = FixtureBuildDir();
		IFileManager::Get().MakeDirectory(*Dir, true);
		if (!WriteMinimalProject(Dir) || !FFileHelper::SaveStringToFile(DataAssetsJson, *FPaths::Combine(Dir, TEXT("data-assets.json"))))
		{
			return nullptr;
		}

		UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(Dir, TestRoot);
		if (Project)
		{
			StoryFlowDataAssets::BuildSeed(Project->DataAssets, OutSeed);
		}
		return Project;
	}

	void CleanUp()
	{
		UEditorAssetLibrary::DeleteDirectory(TestRoot);
		IFileManager::Get().DeleteDirectory(*FixtureBuildDir(), false, true);
	}

	/**
	 * The preamble every fixture-driven test shares: read data-assets-seed.json off disk, clear
	 * any assets a previous run left behind (a stale one carries a matching hash and would be
	 * skipped, never re-parsed), and import it. False means the test cannot proceed; it has
	 * already reported why.
	 */
	bool LoadFixtureSeed(FAutomationTestBase& Test, StoryFlowDataAssets::FSeed& OutSeed)
	{
		const FString SeedPath = GoldenFixturePath(TEXT("data-assets-seed.json"));
		FString SeedJson;
		if (!Test.TestTrue(TEXT("data-assets-seed.json is readable"), !SeedPath.IsEmpty() && FFileHelper::LoadFileToString(SeedJson, *SeedPath)))
		{
			return false;
		}

		UEditorAssetLibrary::DeleteDirectory(TestRoot);

		if (!Test.TestNotNull(TEXT("the seed fixture imports"), ImportSeedJson(SeedJson, OutSeed)))
		{
			CleanUp();
			return false;
		}
		return true;
	}

	/** True for the string family — every one of these stores its value in StringValue. */
	bool IsStringFamily(EStoryFlowVariableType Type)
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
	bool VariantMatchesJson(FAutomationTestBase& Test, const FString& Label, const FStoryFlowVariant& Variant, const TSharedPtr<FJsonValue>& Expected)
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
	 * Run one fixture resolution table (data-assets-resolution.json's `resolutions`, or
	 * data-assets-writes.json's `postWriteResolutions`) against the store.
	 *
	 * Returns the number of records a value or unset COMPARISON actually ran for — not the loop
	 * count. A record dropped for a structural reason (not an object, the resolved flag did not
	 * match, no value where the fixture claims one) is deliberately NOT counted, so the caller's
	 * "every record was compared" assertion can fail on its own rather than being a tautology.
	 */
	int32 AssertResolutionTable(FAutomationTestBase& Test, const StoryFlowDataAssets::FSeed& Seed,
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
}

// ============================================================================
// The golden resolution table (§4, empty overlay)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetResolutionFixtureTest,
	"StoryFlow.DataAssets.Resolution.GoldenFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetResolutionFixtureTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	TSharedPtr<FJsonObject> ResolutionFixture = LoadGoldenFixture(TEXT("data-assets-resolution.json"));
	if (!TestTrue(TEXT("data-assets-resolution.json parses"), ResolutionFixture.IsValid()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed Seed;
	if (!LoadFixtureSeed(*this, Seed))
	{
		return false;
	}

	// Cardinality assertions against the fixture. If a fixture regen moves any of these, the
	// failure should read as "the fixture changed", not as "the importer broke".
	TestEqual(TEXT("data-assets-seed.json still carries 3 assets, all imported"), Seed.Num(), 3);
	if (const FStoryFlowDataAssetDef* Base = Seed.Find(BaseId))
	{
		TestEqual(TEXT("the seed fixture's base is still named CreatureBase"), Base->Name, TEXT("CreatureBase"));
		TestTrue(TEXT("the seed fixture's base is still a root (no parent)"), Base->Parent.IsEmpty());
		// The fixture's base declares 12 rows, one of them the category that must not survive
		TestEqual(TEXT("the seed fixture's base keeps 11 of its 12 rows, the category dropped"), Base->Variables.Num(), 11);
	}
	else
	{
		AddError(TEXT("the base asset is missing from the seed"));
	}
	if (const FStoryFlowDataAssetDef* Child = Seed.Find(ChildId))
	{
		TestEqual(TEXT("the seed fixture's child still points at the base"), Child->Parent, FString(BaseId));
		TestEqual(TEXT("the seed fixture's child still carries 3 overrides"), Child->Overrides.Num(), 3);
	}
	else
	{
		AddError(TEXT("the child asset is missing from the seed"));
	}

	// Every record is asserted against an EMPTY overlay: this fixture is pure file state
	const StoryFlowDataAssets::FOverlay EmptyOverlay;

	const TArray<TSharedPtr<FJsonValue>>* Resolutions = nullptr;
	if (!TestTrue(TEXT("the fixture carries a resolutions array"), ResolutionFixture->TryGetArrayField(TEXT("resolutions"), Resolutions)))
	{
		CleanUp();
		return false;
	}

	const int32 Compared = AssertResolutionTable(*this, Seed, EmptyOverlay, *Resolutions, TEXT("resolutions"));
	TestEqual(TEXT("every data-assets-resolution.json record was compared"), Compared, Resolutions->Num());
	TestTrue(TEXT("data-assets-resolution.json is not empty"), Resolutions->Num() > 0);

	CleanUp();
	return true;
}

// ============================================================================
// The golden write sequence (§5) and the resolution table it leaves behind
// ============================================================================

namespace StoryFlowDataAssetTestHelpers
{
	/**
	 * Turn a fixture write's JSON value into the variant a node arm would hand the store.
	 *
	 * Typed against the chain's DECLARATION, exactly as the executor will be: at runtime the
	 * value arrives on a typed pin, so an entry list is a map and a bare string is whatever the
	 * declaration says it is. Falls back to JSON-shape inference for a write the chain does not
	 * declare — that write is going to be refused anyway, and its value never reaches storage.
	 */
	FStoryFlowVariant WriteValueFromJson(const StoryFlowDataAssets::FSeed& Seed, const FString& AssetId, const FString& VariableId, const TSharedPtr<FJsonValue>& Value)
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
			Result.SetString(Value->AsString());
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
			Result.SetArray(Items);
			break;
		}
		default:
			break;
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetWritesFixtureTest,
	"StoryFlow.DataAssets.Resolution.Writes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetWritesFixtureTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	TSharedPtr<FJsonObject> WritesFixture = LoadGoldenFixture(TEXT("data-assets-writes.json"));
	if (!TestTrue(TEXT("data-assets-writes.json parses"), WritesFixture.IsValid()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed Seed;
	if (!LoadFixtureSeed(*this, Seed))
	{
		return false;
	}

	StoryFlowDataAssets::FOverlay Overlay;

	// --- Replay the scripted writes in order ---
	const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr;
	if (!TestTrue(TEXT("the fixture carries a writes array"), WritesFixture->TryGetArrayField(TEXT("writes"), Writes)))
	{
		CleanUp();
		return false;
	}

	int32 Replayed = 0;
	for (const TSharedPtr<FJsonValue>& WriteValue : *Writes)
	{
		const TSharedPtr<FJsonObject> Write = WriteValue->AsObject();
		if (!Write.IsValid())
		{
			AddError(TEXT("writes: a record is not an object"));
			continue;
		}

		const FString AssetId = Write->GetStringField(TEXT("assetId"));
		const FString VariableId = Write->GetStringField(TEXT("variableId"));
		const FString Expect = Write->GetStringField(TEXT("expect"));
		FString Note;
		Write->TryGetStringField(TEXT("note"), Note);

		const FStoryFlowVariant Value = WriteValueFromJson(Seed, AssetId, VariableId, Write->TryGetField(TEXT("value")));
		const bool bWritten = StoryFlowDataAssets::TrySet(Seed, Overlay, AssetId, VariableId, Value);

		TestTrue(FString::Printf(TEXT("write %d is %s -- %s"), Replayed, *Expect, *Note), bWritten == (Expect == TEXT("written")));
		++Replayed;
	}
	TestEqual(TEXT("every data-assets-writes.json write was replayed"), Replayed, Writes->Num());
	TestTrue(TEXT("data-assets-writes.json is not empty"), Writes->Num() > 0);

	// The refused write must not have reached the overlay at all — three assets were written to
	TestEqual(TEXT("only the assets the fixture writes to have overlay entries"), Overlay.Num(), 3);

	// --- Then the whole post-write resolution table ---
	const TArray<TSharedPtr<FJsonValue>>* PostWrite = nullptr;
	if (!TestTrue(TEXT("the fixture carries a postWriteResolutions array"), WritesFixture->TryGetArrayField(TEXT("postWriteResolutions"), PostWrite)))
	{
		CleanUp();
		return false;
	}

	const int32 Compared = AssertResolutionTable(*this, Seed, Overlay, *PostWrite, TEXT("postWriteResolutions"));
	TestEqual(TEXT("every data-assets-writes.json postWriteResolutions record was compared"), Compared, PostWrite->Num());
	TestTrue(TEXT("postWriteResolutions is not empty"), PostWrite->Num() > 0);

	// The fixture's `saveKey` is TASK U3's (the sparse dataAssets save key, contract §7) and is
	// deliberately not read here.

	CleanUp();
	return true;
}

// ============================================================================
// Store behaviour the fixtures cannot express: aliasing and reset
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetOverlayTest,
	"StoryFlow.DataAssets.Resolution.Overlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetOverlayTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	// Precedence and cascade are the WRITES FIXTURE's job (see the Writes test). What is left
	// here is what a value-table fixture structurally cannot say: that reads and writes COPY,
	// and that reset drops the overlay without touching the seed.
	StoryFlowDataAssets::FSeed Seed;
	if (!LoadFixtureSeed(*this, Seed))
	{
		return false;
	}

	StoryFlowDataAssets::FOverlay Overlay;

	// --- Write refusals (contract §5) ---
	TestFalse(TEXT("a write to an unknown asset is refused"),
		StoryFlowDataAssets::TrySet(Seed, Overlay, AbsentId, AliveId, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("a write to an undeclared id is refused"),
		StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, NowhereId, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("a write to a category row is refused"),
		StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, LoreId, FStoryFlowVariant::FromBool(false)));
	TestEqual(TEXT("no refused write touched the overlay"), Overlay.Num(), 0);

	// --- Copy on read: graph code must not reach the store through a read (§3) ---
	{
		FStoryFlowVariant Read = StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, TagsId);
		TestEqual(TEXT("the array resolves through the child's override"), Read.GetArray().Num(), 2);
		Read.GetArrayMutable().Empty();
		TestEqual(TEXT("mutating a read array leaves the seed alone"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, TagsId).GetArray().Num(), 2);

		FStoryFlowVariant ReadMap = StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, LootId);
		TestEqual(TEXT("the map resolves with its authored entries"), ReadMap.GetMap().Num(), 2);
		ReadMap.GetMapMutable().Empty();
		TestEqual(TEXT("mutating a read map leaves the seed alone"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, LootId).GetMap().Num(), 2);
	}

	// --- Deep copy on write: the caller's container must not alias the overlay (§5) ---
	{
		TArray<FStoryFlowMapEntry> Entries;
		FStoryFlowMapEntry Entry;
		Entry.Key.SetString(TEXT("bones"));
		Entry.Value.SetInt(3);
		Entries.Add(Entry);
		FStoryFlowVariant MapWrite;
		MapWrite.SetMap(Entries);
		TestTrue(TEXT("a map write through the base lands"), StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, LootId, MapWrite));
		MapWrite.GetMapMutable().Empty();
		// Gates the indexed read below — an unguarded [0] on a regressed empty result aborts
		// the suite instead of reporting the failure.
		if (TestEqual(TEXT("mutating the written map afterwards leaves the overlay alone"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, LootId).GetMap().Num(), 1))
		{
			// Map writes REPLACE the whole value, never merge
			TestEqual(TEXT("the map write replaced the whole value"),
				StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, LootId).GetMap()[0].Key.GetString(), TEXT("bones"));
		}
	}

	// --- Reset clears the overlay ONLY (§3) ---
	TestTrue(TEXT("a boolean write through the base lands"),
		StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, AliveId, FStoryFlowVariant::FromBool(false)));
	StoryFlowDataAssets::ResetOverlay(Overlay);
	TestEqual(TEXT("reset empties the overlay"), Overlay.Num(), 0);
	TestTrue(TEXT("the seed's own value is back after a reset"), StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, AliveId).GetBool(false));
	TestEqual(TEXT("and so is the seed's map, untouched by the write above"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, LootId).GetMap().Num(), 2);

	CleanUp();
	return true;
}

// ============================================================================
// DeclMatches — the §6.1 snapshot rule, mirrored from the editor/runtime parity table
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetDeclMatchesTest,
	"StoryFlow.DataAssets.Resolution.DeclMatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetDeclMatchesTest::RunTest(const FString& Parameters)
{
	// Row for row, this is the editor repo's GOLDEN PAIR table in
	// src/__tests__/runtime/data-asset-nodes.test.ts ("editor/runtime parity:
	// dataAssetDeclMatches vs matchesSnapshot"), which runs one fixture list through the HTML
	// runtime's dataAssetDeclMatches AND the editor's matchesSnapshot and requires they agree.
	// This is the third copy of that rule, so it gets the same table: drift here is a binding
	// the editor paints broken that Unreal keeps resolving, or one painted healthy that
	// silently reads a type default.
	struct FCase
	{
		const TCHAR* Name;
		// The chain's declaration
		EStoryFlowVariableType DeclType;
		bool bDeclIsArray;
		EStoryFlowVariableType DeclKeyType;
		EStoryFlowVariableType DeclValueType;
		// The accessor node's spawn-time snapshot
		EStoryFlowVariableType SnapType;
		bool bSnapIsArray;
		EStoryFlowVariableType SnapKeyType;
		EStoryFlowVariableType SnapValueType;
		bool bAgree;
	};

	const EStoryFlowVariableType Str = EStoryFlowVariableType::String;
	const EStoryFlowVariableType Int = EStoryFlowVariableType::Integer;
	const EStoryFlowVariableType Map = EStoryFlowVariableType::Map;

	const FCase Cases[] = {
		{ TEXT("identical scalar"),                       Str, false, Str, Str,  Str, false, Str, Str,  true  },
		{ TEXT("type moved"),                             Int, false, Str, Str,  Str, false, Str, Str,  false },
		{ TEXT("identical array"),                        Str, true,  Str, Str,  Str, true,  Str, Str,  true  },
		{ TEXT("scalar binding over an array declaration"), Str, true,  Str, Str,  Str, false, Str, Str,  false },
		{ TEXT("array binding over a scalar declaration"), Str, false, Str, Str,  Str, true,  Str, Str,  false },
		// The JS row "isArray false vs absent are the same thing" is structural in C++: bIsArray
		// is a bool with no absent state, so an unset snapshot IS false. Kept as a row anyway so
		// the two tables line up one for one.
		{ TEXT("isArray false vs absent are the same thing"), Str, false, Str, Str,  Str, false, Str, Str,  true  },
		{ TEXT("identical map"),                          Map, false, Str, Int,  Map, false, Str, Int,  true  },
		{ TEXT("map value type moved"),                   Map, false, Str, Str,  Map, false, Str, Int,  false },
		{ TEXT("map key type moved"),                     Map, false, Int, Int,  Map, false, Str, Int,  false },
		// The K/V pair is baked into the MAP pin's handle id and means nothing off a scalar, so
		// it must not be compared for one. This is the row that catches a DeclMatches which
		// forgot to gate its K/V comparison on the map type.
		{ TEXT("K/V ignored off a non-map binding"),       Str, false, Int, Int,  Str, false, Str, Str,  true  },
	};

	for (const FCase& Case : Cases)
	{
		FStoryFlowVariable Declaration;
		Declaration.Id = TEXT("v");
		Declaration.Type = Case.DeclType;
		Declaration.bIsArray = Case.bDeclIsArray;
		Declaration.KeyType = Case.DeclKeyType;
		Declaration.ValueType = Case.DeclValueType;

		const bool bMatches = StoryFlowDataAssets::DeclMatches(Declaration, Case.SnapType, Case.bSnapIsArray, Case.SnapKeyType, Case.SnapValueType);
		TestTrue(FString::Printf(TEXT("declMatches: %s (expected %s)"), Case.Name, Case.bAgree ? TEXT("match") : TEXT("stale")),
			bMatches == Case.bAgree);
	}

	return true;
}

// ============================================================================
// Chain guards: cycles and the depth cap
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetChainGuardTest,
	"StoryFlow.DataAssets.Resolution.ChainGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetChainGuardTest::RunTest(const FString& Parameters)
{
	// Hand-built seeds: none of these chains is authorable in the editor, so no fixture carries
	// them, but contract §4.4 fixes what the resolver does with a malformed one.
	{
		// A -> B -> A, with BOTH levels declaring the same id at different values. The walk
		// must end silently, not spin.
		//
		// The values are what make the VISITED SET observable rather than merely
		// load-bearing. Root-most declaration wins (§4.3), which the walk implements as
		// "the last level visited that declares it". With the guard, A's walk is [A, B] and
		// answers B's 2. Without it, only the depth cap stops the walk, and it runs
		// [A, B, A, B, ...] for 65 levels — an ODD count, so it ends back on A and answers
		// A's 1 instead. Deleting the visited set therefore flips both assertions below,
		// which a same-value cycle fixture would have let through.
		StoryFlowDataAssets::FSeed Seed;
		FStoryFlowVariable DeclaredA;
		DeclaredA.Id = TEXT("v");
		DeclaredA.Type = EStoryFlowVariableType::Integer;
		DeclaredA.Value.SetInt(1);
		FStoryFlowVariable DeclaredB = DeclaredA;
		DeclaredB.Value.SetInt(2);

		FStoryFlowDataAssetDef A;
		A.Id = TEXT("A");
		A.Parent = TEXT("B");
		A.Variables.Add(DeclaredA);
		FStoryFlowDataAssetDef B;
		B.Id = TEXT("B");
		B.Parent = TEXT("A");
		B.Variables.Add(DeclaredB);
		Seed.Add(A.Id, A);
		Seed.Add(B.Id, B);

		const StoryFlowDataAssets::FOverlay Overlay;
		TestEqual(TEXT("a cyclic chain stops at the repeat, taking the far level's declaration"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, TEXT("A"), TEXT("v")).GetInt(), 2);
		TestEqual(TEXT("the guard holds from the other end of the cycle too"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, TEXT("B"), TEXT("v")).GetInt(), 1);
	}

	{
		// A 200-deep chain with the declaration at the ROOT: past the 65-level budget
		// (MaxChainDepth ancestors plus the starting level) the walk gives up and the read
		// answers "no value" rather than running forever.
		StoryFlowDataAssets::FSeed Seed;
		const int32 Depth = 200;
		for (int32 Index = 0; Index < Depth; ++Index)
		{
			FStoryFlowDataAssetDef Level;
			Level.Id = FString::Printf(TEXT("L%d"), Index);
			Level.Parent = (Index + 1 < Depth) ? FString::Printf(TEXT("L%d"), Index + 1) : FString();
			Seed.Add(Level.Id, Level);
		}
		FStoryFlowVariable Declared;
		Declared.Id = TEXT("v");
		Declared.Type = EStoryFlowVariableType::Integer;
		Declared.Value.SetInt(42);

		const StoryFlowDataAssets::FOverlay Overlay;

		// Exactly at the boundary: 64 ancestors above the start, so level 64 is the last visited
		Seed[FString::Printf(TEXT("L%d"), StoryFlowDataAssets::MaxChainDepth)].Variables.Add(Declared);
		FStoryFlowVariant AtBoundary;
		TestTrue(TEXT("the last admitted level still resolves"), StoryFlowDataAssets::TryResolve(Seed, Overlay, TEXT("L0"), TEXT("v"), AtBoundary));
		TestEqual(TEXT("the last admitted level's value comes through"), AtBoundary.GetInt(), 42);

		// One level past it: out of budget
		Seed[FString::Printf(TEXT("L%d"), StoryFlowDataAssets::MaxChainDepth)].Variables.Empty();
		Seed[FString::Printf(TEXT("L%d"), StoryFlowDataAssets::MaxChainDepth + 1)].Variables.Add(Declared);
		FStoryFlowVariant PastBoundary;
		TestFalse(TEXT("a declaration past the depth cap is unreachable"), StoryFlowDataAssets::TryResolve(Seed, Overlay, TEXT("L0"), TEXT("v"), PastBoundary));
	}

	return true;
}

// ============================================================================
// Import round trip, the packed-blob save/load contract, and parent-driven skips
// ============================================================================

namespace StoryFlowDataAssetTestHelpers
{
	const TCHAR* RoundTripBaseId = TEXT("da_aa000000000000000000000000000001");
	const TCHAR* RoundTripChildId = TEXT("da_aa000000000000000000000000000002");

	/**
	 * The inline seed the round-trip test imports. The BASE_HP_TYPE token is the BASE's
	 * declaration of v_hp, which the CHILD only overrides — moving it is how the test proves a
	 * parent's type change dirties a child whose own JSON never moved.
	 *
	 * Token replacement rather than FString::Printf: this is a JSON fixture that will grow, and
	 * a future percent sign anywhere in it would silently corrupt a Printf-built string.
	 */
	FString RoundTripSeedJson(const TCHAR* BaseHpType)
	{
		const FString Template = TEXT(R"JSON(
		{ "dataAssets": {
			"da_aa000000000000000000000000000001": {
				"id": "da_aa000000000000000000000000000001",
				"name": "RoundTripBase",
				"parent": null,
				"variables": [
					{ "id": "v_hp", "name": "hp", "type": "BASE_HP_TYPE", "value": 10 },
					{ "id": "v_tags", "name": "tags", "type": "string", "isArray": true, "value": [ "mob", "melee" ] },
					{ "id": "v_loot", "name": "loot", "type": "map", "keyType": "string", "valueType": "integer",
					  "value": [ { "key": "gold", "value": 1 }, { "key": "gems", "value": 2 } ] },
					{ "id": "v_lore", "name": "lore", "type": "category" }
				],
				"overrides": {}
			},
			"da_aa000000000000000000000000000002": {
				"id": "da_aa000000000000000000000000000002",
				"name": "RoundTripChild",
				"parent": "da_aa000000000000000000000000000001",
				"variables": [],
				"overrides": {
					"v_hp": 25,
					"v_tags": [ "elite", "boss" ],
					"v_loot": [ { "key": "gems", "value": 9 }, { "key": "gold", "value": 8 } ]
				}
			}
		} }
		)JSON");
		return Template.Replace(TEXT("BASE_HP_TYPE"), BaseHpType, ESearchCase::CaseSensitive);
	}

	/**
	 * Push an asset through UE's own tagged-property archive and hand back a FRESH object that
	 * has been PostLoaded — the save/load boundary, without needing to unload a package.
	 *
	 * This is the only way to exercise the packed blobs. FStoryFlowVariant::ArrayValue and
	 * MapValue are NOT UPROPERTYs (UHT cannot do recursive struct arrays), so no array or map
	 * value reaches disk except through the SerializedArrayData blob PreSave packs it into and
	 * PostLoad unpacks it from. Re-importing proves nothing here: the unchanged path returns
	 * the same in-memory UObject and never reloads a package, so a broken pack/unpack pair
	 * stays invisible until the next editor launch, as silently emptied arrays and maps.
	 *
	 * The target starts with empty non-UPROPERTY containers by construction, so anything that
	 * comes back came back through the blob.
	 *
	 * The returned object is rooted in nothing — the CALLER must keep it alive across any
	 * assertion that could tick GC, with FGCObjectScopeGuard.
	 */
	UStoryFlowDataAssetAsset* SerializeAndPostLoad(UStoryFlowDataAssetAsset* Source)
	{
		TArray<uint8> Bytes;
		FObjectWriter(Source, Bytes, /*bIgnoreClassRef*/ false, /*bIgnoreArchetypeRef*/ false, /*bDoDelta*/ false);

		UStoryFlowDataAssetAsset* Reloaded = NewObject<UStoryFlowDataAssetAsset>(GetTransientPackage());
		FObjectReader(Reloaded, Bytes);
		Reloaded->PostLoad();
		return Reloaded;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetImportRoundTripTest,
	"StoryFlow.DataAssets.Resolution.ImportRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetImportRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportSeedJson(RoundTripSeedJson(TEXT("integer")), Seed);
	if (!TestNotNull(TEXT("the inline seed imports"), Project))
	{
		CleanUp();
		return false;
	}

	TestEqual(TEXT("both data assets are on the project"), Project->DataAssets.Num(), 2);

	// The skip-list edit: data-assets.json must never be swept up as a script
	TestFalse(TEXT("data-assets.json was not imported as a script"), Project->Scripts.Contains(TEXT("data-assets")));

	UStoryFlowDataAssetAsset* const* BaseAsset = Project->DataAssets.Find(RoundTripBaseId);
	if (TestNotNull(TEXT("the base data asset exists"), BaseAsset ? *BaseAsset : nullptr))
	{
		TestEqual(TEXT("the base keeps its display name"), (*BaseAsset)->Name, TEXT("RoundTripBase"));
		TestTrue(TEXT("a null parent imports as a root"), (*BaseAsset)->Parent.IsEmpty());
		TestEqual(TEXT("the category row is skipped, the other three survive"), (*BaseAsset)->Variables.Num(), 3);
	}

	UStoryFlowDataAssetAsset* const* ChildAsset = Project->DataAssets.Find(RoundTripChildId);
	if (TestNotNull(TEXT("the child data asset exists"), ChildAsset ? *ChildAsset : nullptr))
	{
		TestEqual(TEXT("the child keeps its parent link"), (*ChildAsset)->Parent, FString(RoundTripBaseId));
		// The map override is typed against the ANCESTOR's declaration, which is the whole
		// reason import runs two passes — a one-pass import would leave it an untyped array
		if (const FStoryFlowVariant* LootOverride = (*ChildAsset)->Overrides.Find(TEXT("v_loot")))
		{
			TestTrue(TEXT("the inherited map override imported as a map"), LootOverride->IsMap());
		}
		else
		{
			AddError(TEXT("the child's map override is missing"));
		}
	}

	// Resolution through the imported assets, with entry order preserved
	const StoryFlowDataAssets::FOverlay Overlay;
	TestEqual(TEXT("the child sees its own hp override"), StoryFlowDataAssets::Resolve(Seed, Overlay, RoundTripChildId, TEXT("v_hp")).GetInt(), 25);
	TestEqual(TEXT("the base keeps its declared hp"), StoryFlowDataAssets::Resolve(Seed, Overlay, RoundTripBaseId, TEXT("v_hp")).GetInt(), 10);
	const FStoryFlowVariant ChildLoot = StoryFlowDataAssets::Resolve(Seed, Overlay, RoundTripChildId, TEXT("v_loot"));
	if (TestEqual(TEXT("the overridden map has both entries"), ChildLoot.GetMap().Num(), 2))
	{
		TestEqual(TEXT("the override's authored key order survives"), ChildLoot.GetMap()[0].Key.GetString(), TEXT("gems"));
		TestEqual(TEXT("the override's second key too"), ChildLoot.GetMap()[1].Key.GetString(), TEXT("gold"));
		TestEqual(TEXT("the override's values survive"), ChildLoot.GetMap()[0].Value.GetInt(), 9);
	}
	TestFalse(TEXT("the category row never resolves"), StoryFlowDataAssets::Resolve(Seed, Overlay, RoundTripBaseId, TEXT("v_lore")).IsValid());

	// ------------------------------------------------------------------
	// The packed blobs survive a real save and load
	// ------------------------------------------------------------------
	// The import above already SAVED both assets, which ran the real PreSave -> pack. What
	// follows is the load half: serialize, then PostLoad -> unpack. Deleting either the pack
	// or the unpack of EITHER blob empties the containers below.
	if (ChildAsset && *ChildAsset)
	{
		// The OVERRIDE blob (PackVariantsForSerialization / UnpackVariantsFromSerialization)
		UStoryFlowDataAssetAsset* Reloaded = SerializeAndPostLoad(*ChildAsset);
		FGCObjectScopeGuard ReloadedGuard(Reloaded);

		TestEqual(TEXT("the reloaded child keeps its override keys"), Reloaded->Overrides.Num(), 3);

		if (const FStoryFlowVariant* Loot = Reloaded->Overrides.Find(TEXT("v_loot")))
		{
			TestTrue(TEXT("the reloaded map override is still a map"), Loot->IsMap());
			if (TestEqual(TEXT("the map override's entries survive save and load"), Loot->GetMap().Num(), 2))
			{
				TestEqual(TEXT("the map override's key order survives save and load"), Loot->GetMap()[0].Key.GetString(), TEXT("gems"));
				TestEqual(TEXT("the map override's values survive save and load"), Loot->GetMap()[0].Value.GetInt(), 9);
			}
		}
		else
		{
			AddError(TEXT("the reloaded child lost its map override entirely"));
		}

		if (const FStoryFlowVariant* Tags = Reloaded->Overrides.Find(TEXT("v_tags")))
		{
			if (TestEqual(TEXT("the array override's elements survive save and load"), Tags->GetArray().Num(), 2))
			{
				TestEqual(TEXT("the array override's order survives save and load"), Tags->GetArray()[0].GetString(), TEXT("elite"));
			}
		}
		else
		{
			AddError(TEXT("the reloaded child lost its array override entirely"));
		}

		// A scalar override needs no blob at all — it rides its own UPROPERTYs. Asserted so a
		// failure above reads as "the blob broke", not "serialization broke".
		if (const FStoryFlowVariant* Hp = Reloaded->Overrides.Find(TEXT("v_hp")))
		{
			TestEqual(TEXT("a scalar override survives without needing the blob"), Hp->GetInt(), 25);
		}
	}

	if (BaseAsset && *BaseAsset)
	{
		// The DECLARATION blob (the new TArray overload of Pack/UnpackVariablesForSerialization)
		UStoryFlowDataAssetAsset* Reloaded = SerializeAndPostLoad(*BaseAsset);
		FGCObjectScopeGuard ReloadedGuard(Reloaded);

		if (TestEqual(TEXT("the reloaded base keeps its declarations in order"), Reloaded->Variables.Num(), 3))
		{
			TestEqual(TEXT("declaration order survives save and load"), Reloaded->Variables[2].Id, TEXT("v_loot"));
			if (TestEqual(TEXT("the declared map's entries survive save and load"), Reloaded->Variables[2].Value.GetMap().Num(), 2))
			{
				TestEqual(TEXT("the declared map's key order survives save and load"), Reloaded->Variables[2].Value.GetMap()[0].Key.GetString(), TEXT("gold"));
			}
			TestEqual(TEXT("the declared array's elements survive save and load"), Reloaded->Variables[1].Value.GetArray().Num(), 2);
		}
	}

	// ------------------------------------------------------------------
	// Skip-unchanged, and the parent-declaration dependency it has to honour
	// ------------------------------------------------------------------
	UStoryFlowProjectAsset* Reimported = UStoryFlowImporter::ImportProject(FixtureBuildDir(), TestRoot);
	if (TestNotNull(TEXT("the unchanged re-import succeeds"), Reimported))
	{
		TestEqual(TEXT("the unchanged re-import still keys both assets"), Reimported->DataAssets.Num(), 2);
		for (const auto& AssetPair : Reimported->DataAssets)
		{
			if (AssetPair.Value)
			{
				TestFalse(FString::Printf(TEXT("'%s' is left clean by the unchanged re-import"), *AssetPair.Key),
					AssetPair.Value->GetOutermost()->IsDirty());
				// A skip must not short-circuit past the payload
				TestEqual(FString::Printf(TEXT("'%s' keeps its id through the skip"), *AssetPair.Key),
					AssetPair.Value->AssetId, AssetPair.Key);
			}
		}
	}

	// Now move ONLY the base's declaration of v_hp, integer -> float. The child's JSON is
	// byte-identical, but its v_hp override is parsed against that declaration, so a skip
	// keyed on the child's own JSON alone would leave a stale Integer override behind. The
	// skip hash folds in the resolved ancestor declarations precisely to prevent that.
	StoryFlowDataAssets::FSeed RetypedSeed;
	UStoryFlowProjectAsset* Retyped = ImportSeedJson(RoundTripSeedJson(TEXT("float")), RetypedSeed);
	if (TestNotNull(TEXT("the re-typed seed imports"), Retyped))
	{
		UStoryFlowDataAssetAsset* const* RetypedChild = Retyped->DataAssets.Find(RoundTripChildId);
		if (TestNotNull(TEXT("the child survives the parent's type change"), RetypedChild ? *RetypedChild : nullptr))
		{
			if (const FStoryFlowVariant* Hp = (*RetypedChild)->Overrides.Find(TEXT("v_hp")))
			{
				// Integer here means the child was skipped as unchanged and kept a stale parse
				TestTrue(TEXT("the child re-imported after its parent's declaration changed type"),
					Hp->GetType() == EStoryFlowVariableType::Float);
				TestNearlyEqual(TEXT("and its override re-parsed against the new declaration"), Hp->GetFloat(), 25.0f, 1.e-4f);
			}
			else
			{
				AddError(TEXT("the re-typed child lost its hp override"));
			}
		}
		TestNearlyEqual(TEXT("the re-typed chain resolves as a float"),
			StoryFlowDataAssets::Resolve(RetypedSeed, Overlay, RoundTripChildId, TEXT("v_hp")).GetFloat(), 25.0f, 1.e-4f);
	}

	CleanUp();
	return true;
}

// ============================================================================
// A malformed override the seed cannot express
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetMalformedOverrideTest,
	"StoryFlow.DataAssets.Resolution.MalformedOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetMalformedOverrideTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	// A map-declared variable overridden with a bare number. The collector never emits this, so
	// it can only arrive from a legacy or hand-edited export — but trusting the seed cannot mean
	// storing a value that is not one. The override must be DROPPED, leaving the declared
	// default to resolve, because the alternative (an unset variant in the overrides table)
	// would make TryResolve answer true with nothing usable in it, and reach the accessor arms
	// as a silent type default.
	const TCHAR* SeedJson = TEXT(R"JSON(
	{ "dataAssets": {
		"da_bb000000000000000000000000000001": {
			"id": "da_bb000000000000000000000000000001",
			"name": "MalformedBase",
			"parent": null,
			"variables": [
				{ "id": "v_loot", "name": "loot", "type": "map", "keyType": "string", "valueType": "integer",
				  "value": [ { "key": "gold", "value": 1 } ] }
			],
			"overrides": {}
		},
		"da_bb000000000000000000000000000002": {
			"id": "da_bb000000000000000000000000000002",
			"name": "MalformedChild",
			"parent": "da_bb000000000000000000000000000001",
			"variables": [],
			"overrides": { "v_loot": 5 }
		}
	} }
	)JSON");

	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportSeedJson(SeedJson, Seed);
	if (!TestNotNull(TEXT("the malformed seed still imports"), Project))
	{
		CleanUp();
		return false;
	}

	const FString ChildAssetId = TEXT("da_bb000000000000000000000000000002");
	if (UStoryFlowDataAssetAsset* const* Child = Project->DataAssets.Find(ChildAssetId))
	{
		TestFalse(TEXT("the unusable map override is not stored at all"), (*Child)->Overrides.Contains(TEXT("v_loot")));
		TestEqual(TEXT("and nothing else was stored in its place"), (*Child)->Overrides.Num(), 0);
	}
	else
	{
		AddError(TEXT("the malformed child asset is missing"));
	}

	// Resolution falls through to the base's declared default, intact
	const StoryFlowDataAssets::FOverlay Overlay;
	FStoryFlowVariant Resolved;
	if (TestTrue(TEXT("the variable still resolves through the chain"),
		StoryFlowDataAssets::TryResolve(Seed, Overlay, ChildAssetId, TEXT("v_loot"), Resolved)))
	{
		TestTrue(TEXT("a successful resolve always yields a usable value"), Resolved.IsValid());
		TestTrue(TEXT("and it is the declared map, not an unset variant"), Resolved.IsMap());
		// The count assertion GATES the indexed read: when this regresses the variant is empty,
		// and an unguarded [0] would abort the whole suite instead of reporting the failure.
		if (TestEqual(TEXT("the declared default's entries come through"), Resolved.GetMap().Num(), 1))
		{
			TestEqual(TEXT("the declared default's key comes through"), Resolved.GetMap()[0].Key.GetString(), TEXT("gold"));
		}
	}

	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
