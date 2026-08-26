// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * The character-index build-folder fixture: a minimal project with two characters and an
 * optional character-index.json, imported through the REAL importer (house pattern — import
 * is where the artifact's absence semantics live).
 *
 * Shared between the index import suite and the id-resolution suites that build on the same
 * bridge, which is why it left StoryFlowCharacterIndexTests.cpp.
 *
 * Header-only, and included by more than one test file, so everything here is `inline` — the
 * editor module is a unity build and a non-inline definition would collide.
 *
 * Same precedent as StoryFlowEngineContractFixtures.h / StoryFlowTagAccumulator.h /
 * StoryFlowWidgetSpy.h: a test-only header beside the tests that share it, never in Public.
 */
namespace StoryFlowCharacterIndexTestHelpers
{
	inline const TCHAR* TestRoot = TEXT("/Game/StoryFlowCharacterIndexTests");

	/** The contract-shaped index for the two fixture characters: values VERBATIM in the
	    normalized record-key shape (lowercase, backslashes). */
	inline const TCHAR* TwoCharacterIndex = TEXT(R"JSON({"schemaVersion":"1","characters":{"da_hero0001":"chars\\hero.sfc","da_villain1":"chars\\villain.sfc"}})JSON");

	inline FString FixtureBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowCharacterIndexFixture"));
	}

	inline void CleanUp()
	{
		UEditorAssetLibrary::DeleteDirectory(TestRoot);
		IFileManager::Get().DeleteDirectory(*FixtureBuildDir(), false, true);
	}

	/**
	 * Write the fixture build folder: minimal project, two characters, and — when a body is
	 * given — a character-index.json. IndexJson == nullptr writes NO index file, the pre-P4
	 * export shape. Returns false if any write failed, so callers can fail fast.
	 */
	inline bool WriteFixture(const TCHAR* IndexJson)
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
	inline UStoryFlowProjectAsset* ImportFixture(FAutomationTestBase& Test, const TCHAR* IndexJson)
	{
		UEditorAssetLibrary::DeleteDirectory(TestRoot);
		if (!Test.TestTrue(TEXT("the fixture build folder is writable"), WriteFixture(IndexJson)))
		{
			CleanUp();
			return nullptr;
		}
		UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(FixtureBuildDir(), TestRoot);
		if (!Test.TestNotNull(TEXT("the fixture imports"), Project))
		{
			CleanUp();
		}
		return Project;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
