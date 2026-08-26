// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "StoryFlowCharacterIndexFixture.h"
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
 * The fixture-writing helpers live in StoryFlowCharacterIndexFixture.h, shared with the
 * id-resolution suites that build on the same bridge. The index is imported through the REAL
 * importer against a build-folder fixture, house pattern: import is where the artifact's
 * absence semantics live (absent file = pre-P4 export = empty bridge, silently), and a test
 * that parsed the file itself would happily pass while ImportProjectFromJson never called the
 * parser at all.
 *
 * The exists-but-unusable ladder gets one pinned rung per way an index can degrade: corrupt
 * file, missing schemaVersion, unknown schemaVersion, no characters object. Every rung warns
 * exactly once with the same named consequence, leaves the bridge empty, and never touches the
 * rest of the import.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.CharacterIndex", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.CharacterIndex" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

// ============================================================================
// The bridge itself: present, absent, empty
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
	const FString* BridgedHeroKey = Project->CharacterIdToPath.Find(TEXT("da_hero0001"));
	if (TestNotNull(TEXT("the hero id is bridged"), BridgedHeroKey))
	{
		TestEqual(TEXT("the hero record key is stored verbatim"), *BridgedHeroKey, TEXT("chars\\hero.sfc"));
		// The contract's whole point: the value is a DIRECT key into the characters map
		TestTrue(TEXT("the bridged key hits the Characters map directly"), Project->Characters.Contains(*BridgedHeroKey));
	}
	const FString* BridgedVillainKey = Project->CharacterIdToPath.Find(TEXT("da_villain1"));
	if (TestNotNull(TEXT("the villain id is bridged"), BridgedVillainKey))
	{
		TestEqual(TEXT("the villain record key is stored verbatim"), *BridgedVillainKey, TEXT("chars\\villain.sfc"));
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

	// First the ABSENT baseline, whose recorded hash the empty-map import below must
	// differ from.
	UStoryFlowProjectAsset* Project = ImportFixture(*this, nullptr);
	if (!Project)
	{
		return false;
	}
	const FString AbsentHash = Project->ImportedSourceHash;

	// Now the subject: a P4 export from a project with no migrated characters — the file
	// exists, the map is empty. Same observable bridge as absence; the distinction lives
	// in the project hash, where the file's serialized JSON is a part only when the file
	// exists. WriteFixture (not ImportFixture) so the existing assets survive: the point
	// is the RE-import's skip decision, and deleting the content root would erase the
	// recorded hash the decision compares against.
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

// ============================================================================
// The exists-but-unusable ladder, one rung per test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexMalformedTest,
	"StoryFlow.CharacterIndex.MalformedFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexMalformedTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// A truncated index (interrupted export, disk trouble): corruption is NOT absence —
	// the contract defines only a missing file as pre-P4 — so unlike the absent case this
	// one warns, with the same named consequence as the other rungs. Bridge stays empty,
	// the rest of the import is untouched.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexMissingVersionTest,
	"StoryFlow.CharacterIndex.MissingSchemaVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexMissingVersionTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// Well-formed JSON with NO schemaVersion at all: a malformed artifact, not a version
	// skew, so it gets its own message rather than reporting an empty-string version.
	AddExpectedError(TEXT("character-index.json declares no schemaVersion"), EAutomationExpectedErrorFlags::Contains, 1);

	UStoryFlowProjectAsset* Project = ImportFixture(*this, TEXT(R"JSON({"characters":{"da_hero0001":"chars\\hero.sfc"}})JSON"));
	if (!Project)
	{
		return false;
	}

	TestEqual(TEXT("the versionless index leaves the bridge empty"), Project->CharacterIdToPath.Num(), 0);
	TestEqual(TEXT("the characters themselves still import"), Project->Characters.Num(), 2);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIndexNoCharactersObjectTest,
	"StoryFlow.CharacterIndex.NoCharactersObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIndexNoCharactersObjectTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterIndexTestHelpers;

	// A supported version whose characters member is the wrong shape (an array here): the
	// last rung of the ladder — same corruption posture as the parse failure, one level
	// down, and the same named consequence.
	AddExpectedError(TEXT("character-index.json has no readable characters object"), EAutomationExpectedErrorFlags::Contains, 1);

	UStoryFlowProjectAsset* Project = ImportFixture(*this, TEXT(R"JSON({"schemaVersion":"1","characters":["da_hero0001"]})JSON"));
	if (!Project)
	{
		return false;
	}

	TestEqual(TEXT("the objectless index leaves the bridge empty"), Project->CharacterIdToPath.Num(), 0);
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
		CleanUp();
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
	const FString* BridgedHeroKey = W.Subsystem->GetCharacterIdToPath().Find(TEXT("da_hero0001"));
	if (TestNotNull(TEXT("the subsystem's copy carries the hero id"), BridgedHeroKey))
	{
		// The bridge's target map is RuntimeCharacters, which keys exactly as the project's
		// Characters map does — the verbatim value must hit it directly.
		TestTrue(TEXT("the bridged key hits RuntimeCharacters directly"), W.Subsystem->GetRuntimeCharacters().Contains(*BridgedHeroKey));
	}

	W.Subsystem->SetProject(nullptr);
	TestEqual(TEXT("clearing the project clears the bridge"), W.Subsystem->GetCharacterIdToPath().Num(), 0);

	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
