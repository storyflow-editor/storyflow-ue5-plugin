// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "StoryFlowRuntime.h"
#include "StoryFlowScopedWorld.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * character-index.json import — the character id bridge (P4 contract §1.4) — plus the additive
 * characterRefId / characterId node fields it exists to serve.
 *
 * The index is imported through the REAL importer against a build-folder fixture, house
 * pattern: import is where the artifact's absence semantics live (absent file = pre-P4
 * export = empty bridge, silently), and a test that parsed the file itself would happily
 * pass while ImportProjectFromJson never called the parser at all.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.CharacterIndex", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.CharacterIndex" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowCharacterIndexTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowCharacterIndexTests");

	FString FixtureBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowCharacterIndexFixture"));
	}

	void CleanUp()
	{
		UEditorAssetLibrary::DeleteDirectory(TestRoot);
		IFileManager::Get().DeleteDirectory(*FixtureBuildDir(), false, true);
	}

	/**
	 * Write the fixture build folder: minimal project, two characters, and — when a body is
	 * given — a character-index.json. IndexJson == nullptr writes NO index file, the pre-P4
	 * export shape. Returns false if any write failed, so callers can fail fast.
	 */
	bool WriteFixture(const TCHAR* IndexJson)
	{
		const FString Dir = FixtureBuildDir();
		// Delete any leftover index first: an absent-file test after a present-file test
		// would otherwise read the previous test's artifact.
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		bool bWritten = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")));
		bWritten = bWritten && FFileHelper::SaveStringToFile(
			TEXT(R"JSON({"characters":{"chars\\hero.sfc":{"name":"Hero"},"chars\\villain.sfc":{"name":"Villain"}}})JSON"),
			*FPaths::Combine(Dir, TEXT("characters.json")));
		bWritten = bWritten && FFileHelper::SaveStringToFile(
			TEXT(R"JSON({"startNode":"0","nodes":{"0":{"type":"start","id":"0"}}})JSON"),
			*FPaths::Combine(Dir, TEXT("main.json")));
		if (IndexJson)
		{
			bWritten = bWritten && FFileHelper::SaveStringToFile(IndexJson, *FPaths::Combine(Dir, TEXT("character-index.json")));
		}
		return bWritten;
	}

	/** Import the fixture folder, deleting whatever a previous run left behind first — a
	    stale asset carries a matching hash and would be skipped rather than re-parsed. */
	UStoryFlowProjectAsset* ImportFixture(FAutomationTestBase& Test, const TCHAR* IndexJson)
	{
		UEditorAssetLibrary::DeleteDirectory(TestRoot);
		if (!Test.TestTrue(TEXT("the fixture build folder is writable"), WriteFixture(IndexJson)))
		{
			return nullptr;
		}
		UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(FixtureBuildDir(), TestRoot);
		if (!Test.TestNotNull(TEXT("the fixture imports"), Project))
		{
			CleanUp();
		}
		return Project;
	}

	/** The contract-shaped index for the two fixture characters: values VERBATIM in the
	    normalized record-key shape (lowercase, backslashes). */
	const TCHAR* TwoCharacterIndex = TEXT(R"JSON({"schemaVersion":"1","characters":{"da_hero0001":"chars\\hero.sfc","da_villain1":"chars\\villain.sfc"}})JSON");
}

// ============================================================================
// The bridge itself: present, absent, empty, unreadable
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexImportTest,
	"StoryFlow.CharacterIndex.Import",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexImportTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	UStoryFlowProjectAsset* Project = ImportFixture(*this, TwoCharacterIndex);
	if (!Project)
	{
		return false;
	}

	TestEqual(TEXT("the bridge carries both characters"), Project->CharacterIdToPath.Num(), 2);

	// VERBATIM storage: the value is byte-identical to the file's, no re-normalization pass
	const FString* HeroKey = Project->CharacterIdToPath.Find(TEXT("da_hero0001"));
	if (TestNotNull(TEXT("the hero id is bridged"), HeroKey))
	{
		TestEqual(TEXT("the hero record key is stored verbatim"), *HeroKey, TEXT("chars\\hero.sfc"));
		// The contract's whole point: the value is a DIRECT key into the characters map
		TestTrue(TEXT("the bridged key hits the Characters map directly"), Project->Characters.Contains(*HeroKey));
	}
	const FString* VillainKey = Project->CharacterIdToPath.Find(TEXT("da_villain1"));
	if (TestNotNull(TEXT("the villain id is bridged"), VillainKey))
	{
		TestEqual(TEXT("the villain record key is stored verbatim"), *VillainKey, TEXT("chars\\villain.sfc"));
	}

	// The skip-list regression: character-index.json must never be swept up as a script
	// (same precedent as data-assets.json in the resolution suite).
	TestFalse(TEXT("character-index.json was not imported as a script"), Project->Scripts.Contains(TEXT("character-index")));
	TestEqual(TEXT("only the real script imported"), Project->Scripts.Num(), 1);

	CleanUp();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexAbsentTest,
	"StoryFlow.CharacterIndex.AbsentFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexAbsentTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// No index file at all: a pre-P4 export. Empty bridge, characters untouched, and no
	// warning — absence is the normal shape of every existing project, not a defect.
	UStoryFlowProjectAsset* Project = ImportFixture(*this, nullptr);
	if (!Project)
	{
		return false;
	}

	TestEqual(TEXT("the bridge is empty"), Project->CharacterIdToPath.Num(), 0);
	TestEqual(TEXT("the characters themselves still import"), Project->Characters.Num(), 2);

	CleanUp();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexEmptyMapTest,
	"StoryFlow.CharacterIndex.EmptyMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexEmptyMapTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// A P4 export from a project with no migrated characters: the file exists, the map is
	// empty. Same observable bridge as absence — the distinction lives in the project hash,
	// where the file's serialized JSON is a part only when the file exists.
	UStoryFlowProjectAsset* Project = ImportFixture(*this, nullptr);
	if (!Project)
	{
		return false;
	}
	const FString AbsentHash = Project->ImportedSourceHash;

	// Same fixture with the empty-map index added. WriteFixture (not ImportFixture) so the
	// existing assets survive: the point is the RE-import's skip decision, and deleting the
	// content root would erase the recorded hash the decision compares against.
	if (!TestTrue(TEXT("the empty-map fixture is writable"), WriteFixture(TEXT(R"JSON({"schemaVersion":"1","characters":{}})JSON"))))
	{
		CleanUp();
		return false;
	}
	Project = UStoryFlowImporter::ImportProject(FixtureBuildDir(), TestRoot);
	if (!TestNotNull(TEXT("the empty-map fixture imports"), Project))
	{
		CleanUp();
		return false;
	}

	TestEqual(TEXT("the bridge is empty"), Project->CharacterIdToPath.Num(), 0);
	TestEqual(TEXT("the characters themselves still import"), Project->Characters.Num(), 2);
	// The file's presence IS a hash input, even when it maps nothing: an exporter starting
	// to ship the artifact must dirty the project asset, not be skipped as unchanged.
	TestNotEqual(TEXT("an empty-map index is distinguishable from an absent one in the hash"), Project->ImportedSourceHash, AbsentHash);

	CleanUp();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexUnknownVersionTest,
	"StoryFlow.CharacterIndex.UnknownSchemaVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexUnknownVersionTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// A future editor's index: exactly one warning, an empty bridge, and the rest of the
	// import untouched — the degraded posture, never a crash and never a guess.
	AddExpectedError(TEXT("character-index.json declares schemaVersion"), EAutomationExpectedErrorFlags::Contains, 1);

	UStoryFlowProjectAsset* Project = ImportFixture(*this, TEXT(R"JSON({"schemaVersion":"2","characters":{"da_hero0001":"chars\\hero.sfc"}})JSON"));
	if (!Project)
	{
		return false;
	}

	TestEqual(TEXT("the unreadable index leaves the bridge empty"), Project->CharacterIdToPath.Num(), 0);
	TestEqual(TEXT("the characters themselves still import"), Project->Characters.Num(), 2);

	CleanUp();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexMalformedTest,
	"StoryFlow.CharacterIndex.MalformedFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexMalformedTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// A truncated index (interrupted export, disk trouble): corruption is NOT absence —
	// the contract defines only a missing file as pre-P4 — so unlike the absent case this
	// one warns, with the same named consequence as the unknown-version gate. Bridge stays
	// empty, the rest of the import is untouched.
	AddExpectedError(TEXT("character-index.json exists but could not be read"), EAutomationExpectedErrorFlags::Contains, 1);

	UStoryFlowProjectAsset* Project = ImportFixture(*this, TEXT(R"JSON({"schemaVersion":"1","characters":{"da_hero0001":"chars\\he)JSON"));
	if (!Project)
	{
		return false;
	}

	TestEqual(TEXT("the corrupt index leaves the bridge empty"), Project->CharacterIdToPath.Num(), 0);
	TestEqual(TEXT("the characters themselves still import"), Project->Characters.Num(), 2);

	CleanUp();
	return true;
}

// ============================================================================
// The additive node fields
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexNodeFieldsTest,
	"StoryFlow.CharacterIndex.NodeFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexNodeFieldsTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	IFileManager::Get().MakeDirectory(*FixtureBuildDir(), /*Tree*/ true);

	// One migrated node of each kind beside one pre-migration node of each kind, so the
	// present and absent shapes are parsed by the SAME import.
	const FString ScriptJsonPath = FPaths::Combine(FixtureBuildDir(), TEXT("nodes.json"));
	const bool bWritten = FFileHelper::SaveStringToFile(TEXT(R"JSON(
	{
		"startNode": "0",
		"nodes": {
			"0": { "type": "start", "id": "0" },
			"1": { "type": "dialogue", "id": "1", "character": "chars/hero.sfc", "characterRefId": "da_hero0001" },
			"2": { "type": "dialogue", "id": "2", "character": "chars/hero.sfc" },
			"3": { "type": "setCharacterVar", "id": "3", "variable": "Mood", "characterPath": "chars/hero.sfc", "characterId": "da_hero0001" },
			"4": { "type": "getCharacterVar", "id": "4", "variable": "Mood", "characterPath": "chars/hero.sfc" }
		}
	}
	)JSON"), *ScriptJsonPath);
	if (!TestTrue(TEXT("the node fixture is writable"), bWritten))
	{
		return false;
	}

	UStoryFlowScriptAsset* Script = UStoryFlowImporter::ImportScript(ScriptJsonPath, TestRoot);
	if (!TestNotNull(TEXT("the node fixture imports"), Script))
	{
		CleanUp();
		return false;
	}

	if (const FStoryFlowNode* Migrated = Script->Nodes.Find(TEXT("1")))
	{
		TestEqual(TEXT("a migrated dialogue carries its characterRefId"), Migrated->Data.CharacterRefId, TEXT("da_hero0001"));
		TestEqual(TEXT("the path field stays untouched beside it"), Migrated->Data.Character, TEXT("chars/hero.sfc"));
	}
	else
	{
		AddError(TEXT("the migrated dialogue node is missing"));
	}
	if (const FStoryFlowNode* PreMigration = Script->Nodes.Find(TEXT("2")))
	{
		TestTrue(TEXT("a pre-migration dialogue leaves characterRefId empty"), PreMigration->Data.CharacterRefId.IsEmpty());
	}
	if (const FStoryFlowNode* MigratedVar = Script->Nodes.Find(TEXT("3")))
	{
		TestEqual(TEXT("a migrated char-var node carries its characterId"), MigratedVar->Data.CharacterId, TEXT("da_hero0001"));
		TestEqual(TEXT("the path field stays untouched beside it"), MigratedVar->Data.CharacterPath, TEXT("chars/hero.sfc"));
		TestTrue(TEXT("the character id never leaks into the data-asset vocabulary"), MigratedVar->Data.AssetId.IsEmpty());
	}
	else
	{
		AddError(TEXT("the migrated char-var node is missing"));
	}
	if (const FStoryFlowNode* PreMigrationVar = Script->Nodes.Find(TEXT("4")))
	{
		TestTrue(TEXT("a pre-migration char-var node leaves characterId empty"), PreMigrationVar->Data.CharacterId.IsEmpty());
	}

	CleanUp();
	return true;
}

// ============================================================================
// The subsystem's copy
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexSubsystemBridgeTest,
	"StoryFlow.CharacterIndex.SubsystemBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexSubsystemBridgeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;
	using namespace StoryFlowTestWorld;

	UStoryFlowProjectAsset* Project = ImportFixture(*this, TwoCharacterIndex);
	if (!Project)
	{
		return false;
	}

	FScopedWorld W;
	if (!TestTrue(TEXT("the scoped world initializes"), W.Init()))
	{
		CleanUp();
		return false;
	}

	// SetProject clears the bridge on a null project — that clear logs the standing warning
	AddExpectedError(TEXT("Project cleared"), EAutomationExpectedErrorFlags::Contains, 1);

	W.Subsystem->SetProject(Project);
	TestEqual(TEXT("SetProject copies the bridge beside RuntimeCharacters"), W.Subsystem->GetCharacterIdToPath().Num(), 2);
	const FString* HeroKey = W.Subsystem->GetCharacterIdToPath().Find(TEXT("da_hero0001"));
	if (TestNotNull(TEXT("the subsystem's copy carries the hero id"), HeroKey))
	{
		// The bridge's target map is RuntimeCharacters, which keys exactly as the project's
		// Characters map does — the verbatim value must hit it directly.
		TestTrue(TEXT("the bridged key hits RuntimeCharacters directly"), W.Subsystem->GetRuntimeCharacters().Contains(*HeroKey));
	}

	W.Subsystem->SetProject(nullptr);
	TestEqual(TEXT("clearing the project clears the bridge"), W.Subsystem->GetCharacterIdToPath().Num(), 0);

	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
