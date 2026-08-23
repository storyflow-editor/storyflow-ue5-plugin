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
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * The Data Asset (.sfd) chain resolver against the SHARED GOLDEN FIXTURES — the cross-engine
 * parity gate for contract §4.
 *
 * TestContent/engine-contract/data-assets-seed.json and data-assets-resolution.json are checked
 * in VERBATIM from the editor repo and were GENERATED from the HTML runtime
 * (src/renderer/runtime/runtime-data-assets.js), the normative implementation. Every engine's
 * plugin consumes the same two files, so a resolver that drifts from the reference fails here
 * rather than in someone's game.
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

	/** The smallest project.json ImportProject accepts, so the fixture seed can ride a real import. */
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
}

// ============================================================================
// The golden resolution table
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetResolutionFixtureTest,
	"StoryFlow.DataAssets.Resolution.GoldenFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetResolutionFixtureTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	const FString SeedPath = GoldenFixturePath(TEXT("data-assets-seed.json"));
	FString SeedJson;
	if (!TestTrue(TEXT("the seed fixture is readable"), !SeedPath.IsEmpty() && FFileHelper::LoadFileToString(SeedJson, *SeedPath)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ResolutionFixture = LoadGoldenFixture(TEXT("data-assets-resolution.json"));
	if (!TestTrue(TEXT("the resolution fixture parses"), ResolutionFixture.IsValid()))
	{
		return false;
	}

	// A stale asset from an earlier run would be skipped as unchanged and never re-parsed
	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportSeedJson(SeedJson, Seed);
	if (!TestNotNull(TEXT("the fixture seed imports"), Project))
	{
		CleanUp();
		return false;
	}

	// The seed family is base -> child -> grandchild (contract §9.1)
	TestEqual(TEXT("every fixture asset was imported"), Seed.Num(), 3);
	if (const FStoryFlowDataAssetDef* Base = Seed.Find(TEXT("da_0a1b2c3d4e5f60718293a4b5c6d7e8f9")))
	{
		TestEqual(TEXT("the base carries its display name"), Base->Name, TEXT("CreatureBase"));
		TestTrue(TEXT("the base is a root (no parent)"), Base->Parent.IsEmpty());
		// The category row must not survive import: it has no value and can never resolve
		TestEqual(TEXT("the category declaration is skipped at import"), Base->Variables.Num(), 11);
	}
	else
	{
		AddError(TEXT("the base asset is missing from the seed"));
	}
	if (const FStoryFlowDataAssetDef* Child = Seed.Find(TEXT("da_1b2c3d4e5f60718293a4b5c6d7e8f90a")))
	{
		TestEqual(TEXT("the child keeps its parent link"), Child->Parent, TEXT("da_0a1b2c3d4e5f60718293a4b5c6d7e8f9"));
		TestEqual(TEXT("the child keeps all three of its overrides"), Child->Overrides.Num(), 3);
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

	int32 Asserted = 0;
	for (const TSharedPtr<FJsonValue>& RecordValue : *Resolutions)
	{
		const TSharedPtr<FJsonObject> Record = RecordValue->AsObject();
		if (!Record.IsValid())
		{
			AddError(TEXT("a resolution record is not an object"));
			continue;
		}

		const FString AssetId = Record->GetStringField(TEXT("assetId"));
		const FString VariableId = Record->GetStringField(TEXT("variableId"));
		FString VariableName;
		Record->TryGetStringField(TEXT("variableName"), VariableName);
		const bool bExpectResolved = Record->GetBoolField(TEXT("resolved"));
		const FString Label = FString::Printf(TEXT("resolve(%s, %s /* %s */)"), *AssetId, *VariableId, *VariableName);

		FStoryFlowVariant Value;
		const bool bResolved = StoryFlowDataAssets::TryResolve(Seed, EmptyOverlay, AssetId, VariableId, Value);

		if (!TestTrue(Label + FString::Printf(TEXT(" resolves (expected %s)"), bExpectResolved ? TEXT("true") : TEXT("false")), bResolved == bExpectResolved))
		{
			++Asserted;
			continue;
		}
		if (!bExpectResolved)
		{
			// Contract §9.1: an unresolvable read is an UNSET variant, and a category
			// declaration is deliberately indistinguishable from an undeclared id here
			TestFalse(Label + TEXT(" hands back an unset variant"), Value.IsValid());
			++Asserted;
			continue;
		}

		// A resolved record without a value is a malformed fixture, not a passing case
		const TSharedPtr<FJsonValue> ExpectedValue = Record->TryGetField(TEXT("value"));
		if (ExpectedValue.IsValid())
		{
			VariantMatchesJson(*this, Label, Value, ExpectedValue);
		}
		else
		{
			AddError(Label + TEXT(": the fixture record is marked resolved but carries no value"));
		}
		++Asserted;
	}

	TestEqual(TEXT("every fixture record was asserted"), Asserted, Resolutions->Num());
	TestTrue(TEXT("the fixture is not empty"), Resolutions->Num() > 0);

	CleanUp();
	return true;
}

// ============================================================================
// Copy-on-read and the session overlay
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetOverlayTest,
	"StoryFlow.DataAssets.Resolution.Overlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetOverlayTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetTestHelpers;

	const FString Base = TEXT("da_0a1b2c3d4e5f60718293a4b5c6d7e8f9");
	const FString Child = TEXT("da_1b2c3d4e5f60718293a4b5c6d7e8f90a");
	const FString GrandChild = TEXT("da_2c3d4e5f60718293a4b5c6d7e8f90a1b");
	const FString Alive = TEXT("7f3a1c9e4b2d40518a6f0c3e7d1b5a29");
	const FString Speed = TEXT("9c4f7e25a3b84a19bd60e2f7c81a5d03");   // declared AND overridden on the base
	const FString Tags = TEXT("c58e2f13a0d64c9b871e3f05d2a76b48");
	const FString Loot = TEXT("6d0f39a8b21e47c5903af8d61c72e504");
	const FString Lore = TEXT("ae41b70c95d84e2fa3608c1b5f2d97e0");   // the category row
	const FString Nowhere = TEXT("4c9a1e07b38f42d6a1057e2c93bd48f0");

	const FString SeedPath = GoldenFixturePath(TEXT("data-assets-seed.json"));
	FString SeedJson;
	if (!TestTrue(TEXT("the seed fixture is readable"), !SeedPath.IsEmpty() && FFileHelper::LoadFileToString(SeedJson, *SeedPath)))
	{
		return false;
	}

	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	StoryFlowDataAssets::FSeed Seed;
	if (!TestNotNull(TEXT("the fixture seed imports"), ImportSeedJson(SeedJson, Seed)))
	{
		CleanUp();
		return false;
	}

	StoryFlowDataAssets::FOverlay Overlay;

	// --- Write refusals (contract §5) ---
	TestFalse(TEXT("a write to an unknown asset is refused"),
		StoryFlowDataAssets::Set(Seed, Overlay, TEXT("da_ff00ff00ff00ff00ff00ff00ff00ff00"), Alive, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("a write to an undeclared id is refused"),
		StoryFlowDataAssets::Set(Seed, Overlay, Base, Nowhere, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("a write to a category row is refused"),
		StoryFlowDataAssets::Set(Seed, Overlay, Base, Lore, FStoryFlowVariant::FromBool(false)));
	TestEqual(TEXT("no refused write touched the overlay"), Overlay.Num(), 0);

	// --- Set on the BASE cascades to every descendant that does not shadow it (§5) ---
	TestTrue(TEXT("a write through the base lands"),
		StoryFlowDataAssets::Set(Seed, Overlay, Base, Alive, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("the base sees its own session write"), StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Alive).GetBool(true));
	TestFalse(TEXT("the child inherits the base's session write"), StoryFlowDataAssets::Resolve(Seed, Overlay, Child, Alive).GetBool(true));
	TestFalse(TEXT("the grandchild inherits it too"), StoryFlowDataAssets::Resolve(Seed, Overlay, GrandChild, Alive).GetBool(true));

	// --- A nearer level shadows it, and only for its own subtree (§4.2) ---
	TestTrue(TEXT("a write through the child lands"),
		StoryFlowDataAssets::Set(Seed, Overlay, Child, Alive, FStoryFlowVariant::FromBool(true)));
	TestTrue(TEXT("the child sees its own write"), StoryFlowDataAssets::Resolve(Seed, Overlay, Child, Alive).GetBool(false));
	TestTrue(TEXT("the grandchild sees the nearer write"), StoryFlowDataAssets::Resolve(Seed, Overlay, GrandChild, Alive).GetBool(false));
	TestFalse(TEXT("the base is unaffected by its descendant's write"), StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Alive).GetBool(true));

	// --- WITHIN one level, the overlay entry beats that level's OWN file override (§4.1) ---
	// speed is declared on the base at 1.5 AND overridden on the base itself at 2.25 (the
	// root-level override contract §9.1 pins), so this is the only shape that separates the
	// two Find calls in TryResolve: check the override first and the session write is lost,
	// silently, for the rest of the game. Mirrors write #3 of data-assets-writes.json.
	TestNearlyEqual(TEXT("the base's own file override is the pre-write value"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Speed).GetFloat(), 2.25f, 1.e-4f);
	TestTrue(TEXT("a write over the base's own override lands"),
		StoryFlowDataAssets::Set(Seed, Overlay, Base, Speed, FStoryFlowVariant::FromFloat(9.5f)));
	TestNearlyEqual(TEXT("the session write beats the same level's file override"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Speed).GetFloat(), 9.5f, 1.e-4f);
	TestNearlyEqual(TEXT("and it cascades to a descendant that does not shadow it"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, GrandChild, Speed).GetFloat(), 9.5f, 1.e-4f);

	// --- An ancestor's OVERLAY entry beats a descendant's inherited file value (§4.2) ---
	// tags is declared on the base and overridden on the child; a base-level session write must
	// NOT win over the child's nearer file override.
	TArray<FStoryFlowVariant> NewTags;
	NewTags.Add(FStoryFlowVariant::FromString(TEXT("boss")));
	FStoryFlowVariant TagsWrite;
	TagsWrite.SetArray(NewTags);
	TestTrue(TEXT("an array write through the base lands"), StoryFlowDataAssets::Set(Seed, Overlay, Base, Tags, TagsWrite));
	TestEqual(TEXT("the base sees its array write"), StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Tags).GetArray().Num(), 1);
	TestEqual(TEXT("the child's nearer file override still wins"), StoryFlowDataAssets::Resolve(Seed, Overlay, Child, Tags).GetArray().Num(), 2);

	// --- Copy on read: graph code must not reach the store through a read (§3) ---
	{
		FStoryFlowVariant Read = StoryFlowDataAssets::Resolve(Seed, Overlay, Child, Tags);
		Read.GetArrayMutable().Empty();
		TestEqual(TEXT("mutating a read array leaves the seed alone"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, Child, Tags).GetArray().Num(), 2);

		FStoryFlowVariant ReadMap = StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Loot);
		TestEqual(TEXT("the map resolves with its authored entries"), ReadMap.GetMap().Num(), 2);
		ReadMap.GetMapMutable().Empty();
		TestEqual(TEXT("mutating a read map leaves the seed alone"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Loot).GetMap().Num(), 2);
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
		TestTrue(TEXT("a map write through the base lands"), StoryFlowDataAssets::Set(Seed, Overlay, Base, Loot, MapWrite));
		MapWrite.GetMapMutable().Empty();
		TestEqual(TEXT("mutating the written map afterwards leaves the overlay alone"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Loot).GetMap().Num(), 1);
		// Map writes REPLACE the whole value, never merge
		TestEqual(TEXT("the map write replaced the whole value"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Loot).GetMap()[0].Key.GetString(), TEXT("bones"));
		// ... and the grandchild's own file override still shadows it
		TestEqual(TEXT("the grandchild's map override still wins"),
			StoryFlowDataAssets::Resolve(Seed, Overlay, GrandChild, Loot).GetMap().Num(), 2);
	}

	// --- Reset clears the overlay ONLY (§3) ---
	StoryFlowDataAssets::ResetOverlay(Overlay);
	TestEqual(TEXT("reset empties the overlay"), Overlay.Num(), 0);
	TestTrue(TEXT("the seed's own value is back after a reset"), StoryFlowDataAssets::Resolve(Seed, Overlay, Base, Alive).GetBool(false));

	CleanUp();
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
	/**
	 * The inline seed the round-trip test imports. `BaseHpType` is the BASE's declaration of
	 * v_hp, which the CHILD only overrides — moving it is how the test proves a parent's type
	 * change dirties a child whose own JSON never moved.
	 */
	FString RoundTripSeedJson(const TCHAR* BaseHpType)
	{
		return FString::Printf(TEXT(R"JSON(
		{ "dataAssets": {
			"da_aa000000000000000000000000000001": {
				"id": "da_aa000000000000000000000000000001",
				"name": "RoundTripBase",
				"parent": null,
				"variables": [
					{ "id": "v_hp", "name": "hp", "type": "%s", "value": 10 },
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
		)JSON"), BaseHpType);
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

	const FString BaseId = TEXT("da_aa000000000000000000000000000001");
	const FString ChildId = TEXT("da_aa000000000000000000000000000002");

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

	UStoryFlowDataAssetAsset* const* BaseAsset = Project->DataAssets.Find(BaseId);
	if (TestNotNull(TEXT("the base data asset exists"), BaseAsset ? *BaseAsset : nullptr))
	{
		TestEqual(TEXT("the base keeps its display name"), (*BaseAsset)->Name, TEXT("RoundTripBase"));
		TestTrue(TEXT("a null parent imports as a root"), (*BaseAsset)->Parent.IsEmpty());
		TestEqual(TEXT("the category row is skipped, the other three survive"), (*BaseAsset)->Variables.Num(), 3);
	}

	UStoryFlowDataAssetAsset* const* ChildAsset = Project->DataAssets.Find(ChildId);
	if (TestNotNull(TEXT("the child data asset exists"), ChildAsset ? *ChildAsset : nullptr))
	{
		TestEqual(TEXT("the child keeps its parent link"), (*ChildAsset)->Parent, BaseId);
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
	TestEqual(TEXT("the child sees its own hp override"), StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, TEXT("v_hp")).GetInt(), 25);
	TestEqual(TEXT("the base keeps its declared hp"), StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, TEXT("v_hp")).GetInt(), 10);
	const FStoryFlowVariant ChildLoot = StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, TEXT("v_loot"));
	if (TestEqual(TEXT("the overridden map has both entries"), ChildLoot.GetMap().Num(), 2))
	{
		TestEqual(TEXT("the override's authored key order survives"), ChildLoot.GetMap()[0].Key.GetString(), TEXT("gems"));
		TestEqual(TEXT("the override's second key too"), ChildLoot.GetMap()[1].Key.GetString(), TEXT("gold"));
		TestEqual(TEXT("the override's values survive"), ChildLoot.GetMap()[0].Value.GetInt(), 9);
	}
	TestFalse(TEXT("the category row never resolves"), StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, TEXT("v_lore")).IsValid());

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
		UStoryFlowDataAssetAsset* const* RetypedChild = Retyped->DataAssets.Find(ChildId);
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
			StoryFlowDataAssets::Resolve(RetypedSeed, Overlay, ChildId, TEXT("v_hp")).GetFloat(), 25.0f, 1.e-4f);
	}

	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
