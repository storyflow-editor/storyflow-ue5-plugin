// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "StoryFlowRuntime.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests that re-importing a script whose source JSON is unchanged does not
 * rewrite the .uasset (and leaves the package clean), while changed source
 * still saves, and a failed save retries on the next import.
 *
 * The read-only trick makes save attempts observable without timestamps:
 * with the target file read-only, any save attempt logs the plugin's
 * "Clear the read-only flag" error exactly once. So while the subject file is
 * read-only, the count of that error equals the number of save attempts (the
 * test's last few imports run against a writable file and contribute none).
 *
 * Run via: Session Frontend > Automation > "StoryFlow.Import", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.Import.SkipUnchanged" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowSkipUnchangedTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowSkipUnchangedTests");

	TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, JsonObject);
		return JsonObject;
	}

	TSharedPtr<FJsonObject> ScriptV1()
	{
		return ParseJson(TEXT(R"JSON(
		{
			"startNode": "0",
			"nodes": {
				"0": { "type": "start", "id": "0" }
			}
		}
		)JSON"));
	}

	TSharedPtr<FJsonObject> ScriptV2()
	{
		return ParseJson(TEXT(R"JSON(
		{
			"startNode": "0",
			"nodes": {
				"0": { "type": "start", "id": "0" },
				"1": { "type": "end", "id": "1" }
			}
		}
		)JSON"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowImportSkipUnchangedScriptTest,
	"StoryFlow.Import.SkipUnchanged.Script",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowImportSkipUnchangedScriptTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowSkipUnchangedTestHelpers;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Clear read-only leftovers from a previous run that died mid-test
	{
		FString LeftoverFileName;
		if (FPackageName::TryConvertLongPackageNameToFilename(FString(TestRoot) + TEXT("/skip/subject"), LeftoverFileName, FPackageName::GetAssetPackageExtension())
			&& FPaths::FileExists(LeftoverFileName))
		{
			PlatformFile.SetReadOnly(*LeftoverFileName, false);
		}
	}

	// Start from no asset at all: a subject left over from a previous run would
	// already carry a matching hash, so the first import below would skip and
	// pass its "wrote the .uasset" check on the stale file.
	UEditorAssetLibrary::DeleteDirectory(TestRoot);

	TSharedPtr<FJsonObject> V1 = ScriptV1();
	TSharedPtr<FJsonObject> V2 = ScriptV2();
	if (!TestTrue(TEXT("Fixture JSON parses"), V1.IsValid() && V2.IsValid()))
	{
		return false;
	}

	// Save attempts over the read-only file must total exactly 2:
	// one for the first V2 import, one for the V2 retry after its failed save.
	// The unchanged V1 re-import must contribute zero.
	AddExpectedError(TEXT("it is read only"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Error saving"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Clear the read-only flag"), EAutomationExpectedErrorFlags::Contains, 2);

	// Import V1 normally
	UStoryFlowScriptAsset* First = UStoryFlowImporter::ImportScriptFromJson(V1, TEXT("skip/subject"), TestRoot);
	if (!TestNotNull(TEXT("initial import succeeds"), First))
	{
		return false;
	}

	FString PackageFileName;
	if (!TestTrue(TEXT("package maps to a filename"),
		FPackageName::TryConvertLongPackageNameToFilename(First->GetOutermost()->GetName(), PackageFileName, FPackageName::GetAssetPackageExtension())))
	{
		return false;
	}
	if (!TestTrue(TEXT("initial import wrote the .uasset"), FPaths::FileExists(PackageFileName)))
	{
		return false;
	}

	PlatformFile.SetReadOnly(*PackageFileName, true);

	// Unchanged re-import: no save attempt, package left clean, and the asset
	// handed back must be fully populated — a skip that short-circuits past the
	// parse must not return an empty or stale asset.
	UStoryFlowScriptAsset* Unchanged = UStoryFlowImporter::ImportScriptFromJson(V1, TEXT("skip/subject"), TestRoot);
	if (TestNotNull(TEXT("unchanged re-import returns the asset"), Unchanged))
	{
		TestFalse(TEXT("unchanged re-import leaves the package dirty"), Unchanged->GetOutermost()->IsDirty());
		TestEqual(TEXT("skipped asset keeps its nodes"), Unchanged->Nodes.Num(), 1);
		TestEqual(TEXT("skipped asset keeps its start node"), Unchanged->StartNode, TEXT("0"));
		TestEqual(TEXT("skipped asset keeps its script path"), Unchanged->ScriptPath, TEXT("skip/subject"));
	}

	// Changed source must attempt the save (fails: read-only) ...
	UStoryFlowScriptAsset* Changed = UStoryFlowImporter::ImportScriptFromJson(V2, TEXT("skip/subject"), TestRoot);
	TestNotNull(TEXT("changed import survives the failed save"), Changed);

	// ... and a failed save must not poison the hash: importing V2 again
	// must retry the save, not skip it as "unchanged".
	UStoryFlowScriptAsset* Retry = UStoryFlowImporter::ImportScriptFromJson(V2, TEXT("skip/subject"), TestRoot);
	TestNotNull(TEXT("retry import survives the failed save"), Retry);

	PlatformFile.SetReadOnly(*PackageFileName, false);

	// A matching hash is not enough on its own: with the file writable, import
	// V2 so the hash is recorded, then delete the .uasset behind the plugin's
	// back. The next import must notice the missing file and rewrite it.
	// Both of these saves succeed, so the read-only counts above are unaffected.
	UStoryFlowScriptAsset* Recorded = UStoryFlowImporter::ImportScriptFromJson(V2, TEXT("skip/subject"), TestRoot);
	if (TestNotNull(TEXT("import over a writable file succeeds"), Recorded))
	{
		// The rewrite pin below only means anything if this save actually landed
		TestTrue(TEXT("hash-recording import wrote the .uasset"), FPaths::FileExists(PackageFileName));
		if (TestTrue(TEXT("the .uasset can be deleted"), IFileManager::Get().Delete(*PackageFileName)))
		{
			UStoryFlowScriptAsset* Rewritten = UStoryFlowImporter::ImportScriptFromJson(V2, TEXT("skip/subject"), TestRoot);
			TestNotNull(TEXT("import after the file was deleted returns the asset"), Rewritten);
			TestTrue(TEXT("import after the file was deleted rewrites the .uasset"), FPaths::FileExists(PackageFileName));
		}
	}

	// Cleanup
	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

namespace StoryFlowSkipUnchangedTestHelpers
{
	const TCHAR* ProjectTestRoot = TEXT("/Game/StoryFlowSkipProjectTests");

	FString ProjectFixtureDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowSkipProjectFixture"));
	}

	/** The character path the fixture declares, as the importer keys it. */
	FString FixtureCharacterKey()
	{
		return NormalizeCharacterPath(TEXT("chars/hero.sfc"));
	}

	/** Write the three-file fixture. bScriptEndNode adds a node to main.json so
	    the script's source hash changes without touching any other input.
	    Returns false if any write failed, so callers can fail fast. */
	bool WriteProjectFixture(const FString& HeroName, bool bScriptEndNode = false)
	{
		const FString Dir = ProjectFixtureDir();
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		const bool bProjectWritten = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")));
		const bool bCharactersWritten = FFileHelper::SaveStringToFile(FString::Printf(TEXT(R"JSON({"characters":{"chars/hero.sfc":{"name":"%s"}}})JSON"), *HeroName),
			*FPaths::Combine(Dir, TEXT("characters.json")));
		const FString ScriptJson = bScriptEndNode
			? TEXT(R"JSON({"startNode":"0","nodes":{"0":{"type":"start","id":"0"},"1":{"type":"end","id":"1"}}})JSON")
			: TEXT(R"JSON({"startNode":"0","nodes":{"0":{"type":"start","id":"0"}}})JSON");
		const bool bScriptWritten = FFileHelper::SaveStringToFile(ScriptJson, *FPaths::Combine(Dir, TEXT("main.json")));
		return bProjectWritten && bCharactersWritten && bScriptWritten;
	}

	/** Set or clear read-only on every .uasset under the test root's disk folder. */
	void SetTestContentReadOnly(const TCHAR* ContentRoot, bool bReadOnly)
	{
		FString DiskRoot = FPackageName::LongPackageNameToFilename(FString(ContentRoot) + TEXT("/"));
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *DiskRoot, TEXT("*.uasset"), true, false);
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& File : Files)
		{
			PlatformFile.SetReadOnly(*File, bReadOnly);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowImportSkipUnchangedProjectTest,
	"StoryFlow.Import.SkipUnchanged.Project",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowImportSkipUnchangedProjectTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowSkipUnchangedTestHelpers;

	// Clear leftovers from a previous run that died mid-test
	SetTestContentReadOnly(ProjectTestRoot, false);
	UEditorAssetLibrary::DeleteDirectory(ProjectTestRoot);

	if (!TestTrue(TEXT("fixture written"), WriteProjectFixture(TEXT("Hero"))))
	{
		return false;
	}

	// Three save attempts against read-only files are expected for the whole
	// test, one per phase that changes something:
	//   - the unchanged full re-import: zero (every asset skips)
	//   - the character rename: two, the character asset and the project asset
	//     (whose hash covers characters.json); the script still skips
	//   - the script content change: exactly one, the script's SINGLE save.
	//     Two would mean the parse save and the resolved-media save are still
	//     separate, which is the split-save hazard. The project asset must skip
	//     here: script content is not a project hash input and the payload
	//     membership it does hash is unchanged.
	AddExpectedError(TEXT("it is read only"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Error saving"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Clear the read-only flag"), EAutomationExpectedErrorFlags::Contains, 3);

	UStoryFlowProjectAsset* First = UStoryFlowImporter::ImportProject(ProjectFixtureDir(), ProjectTestRoot);
	if (!TestNotNull(TEXT("initial project import succeeds"), First))
	{
		return false;
	}

	SetTestContentReadOnly(ProjectTestRoot, true);

	// Unchanged full re-import: zero save attempts
	UStoryFlowProjectAsset* Unchanged = UStoryFlowImporter::ImportProject(ProjectFixtureDir(), ProjectTestRoot);
	if (TestNotNull(TEXT("unchanged re-import returns the project"), Unchanged))
	{
		TestFalse(TEXT("unchanged re-import leaves the project package dirty"), Unchanged->GetOutermost()->IsDirty());
		TestEqual(TEXT("unchanged re-import still lists the script"), Unchanged->Scripts.Num(), 1);
		TestEqual(TEXT("unchanged re-import still lists the character"), Unchanged->Characters.Num(), 1);

		// A skipped character must be handed back populated and clean, not
		// emptied by the in-place-update clears the skip jumps over.
		UStoryFlowCharacterAsset* const* SkippedChar = Unchanged->Characters.Find(FixtureCharacterKey());
		if (TestNotNull(TEXT("unchanged re-import keys the character by its normalized path"), SkippedChar ? *SkippedChar : nullptr))
		{
			TestEqual(TEXT("skipped character keeps its name"), (*SkippedChar)->Name, TEXT("Hero"));
			TestFalse(TEXT("skipped character leaves its package dirty"), (*SkippedChar)->GetOutermost()->IsDirty());
		}
	}

	// Change only the character: character asset + project asset save (fail
	// read-only), script asset still skips
	if (!TestTrue(TEXT("renamed fixture written"), WriteProjectFixture(TEXT("Hero Renamed"))))
	{
		return false;
	}
	UStoryFlowProjectAsset* Changed = UStoryFlowImporter::ImportProject(ProjectFixtureDir(), ProjectTestRoot);
	if (TestNotNull(TEXT("changed re-import survives the failed saves"), Changed))
	{
		UStoryFlowCharacterAsset* const* RenamedChar = Changed->Characters.Find(FixtureCharacterKey());
		if (TestNotNull(TEXT("changed re-import still keys the character"), RenamedChar ? *RenamedChar : nullptr))
		{
			TestEqual(TEXT("changed character was really re-parsed"), (*RenamedChar)->Name, TEXT("Hero Renamed"));
		}
	}

	// Settle the failed saves before the next phase: those saves cleared their
	// hashes on purpose, so they would retry and pollute the count of the
	// script-only phase below. With the files writable they succeed and every
	// hash is recorded again; successful saves log no read-only error.
	SetTestContentReadOnly(ProjectTestRoot, false);
	TestNotNull(TEXT("settling re-import over writable files succeeds"),
		UStoryFlowImporter::ImportProject(ProjectFixtureDir(), ProjectTestRoot));
	SetTestContentReadOnly(ProjectTestRoot, true);

	// Change only the script's content: exactly one save attempt, the script's.
	if (!TestTrue(TEXT("script-changed fixture written"), WriteProjectFixture(TEXT("Hero Renamed"), /*bScriptEndNode*/ true)))
	{
		return false;
	}
	UStoryFlowProjectAsset* ScriptChanged = UStoryFlowImporter::ImportProject(ProjectFixtureDir(), ProjectTestRoot);
	if (TestNotNull(TEXT("script-changed re-import survives the failed save"), ScriptChanged))
	{
		TestFalse(TEXT("script-changed re-import leaves the project package dirty"), ScriptChanged->GetOutermost()->IsDirty());
		UStoryFlowScriptAsset* const* ReparsedScript = ScriptChanged->Scripts.Find(TEXT("main"));
		if (TestNotNull(TEXT("script-changed re-import still keys the script"), ReparsedScript ? *ReparsedScript : nullptr))
		{
			TestEqual(TEXT("changed script was really re-parsed"), (*ReparsedScript)->Nodes.Num(), 2);
		}
	}

	// Cleanup
	SetTestContentReadOnly(ProjectTestRoot, false);
	UEditorAssetLibrary::DeleteDirectory(ProjectTestRoot);
	IFileManager::Get().DeleteDirectory(*ProjectFixtureDir(), false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
