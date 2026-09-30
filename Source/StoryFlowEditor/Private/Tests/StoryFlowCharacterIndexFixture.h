// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowTypes.h"
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

	/**
	 * The same two characters WITH variables, for the id-resolution suites: both carry the
	 * SAME variable names (bool/string/integer/array/map) with different values, so a read or
	 * write that resolves the wrong character answers a wrong VALUE rather than a miss —
	 * which is what makes the wrong-path fixtures conclusive.
	 */
	inline const TCHAR* TwoCharacterVariablesJson = TEXT(R"JSON({"characters":{
		"chars\\hero.sfc":{"name":"Hero","variables":{
			"var_b1":{"name":"IsBrave","type":"boolean","value":true},
			"var_s1":{"name":"Title","type":"string","value":"the bold"},
			"var_i1":{"name":"Coins","type":"integer","value":7},
			"var_a1":{"name":"Inventory","type":"string","isArray":true,"value":["sword","shield"]},
			"var_m1":{"name":"Reputation","type":"map","keyType":"string","valueType":"integer","value":[{"key":"guards","value":3},{"key":"thieves","value":5}]}}},
		"chars\\villain.sfc":{"name":"Villain","variables":{
			"var_b1":{"name":"IsBrave","type":"boolean","value":false},
			"var_s1":{"name":"Title","type":"string","value":"the cruel"},
			"var_i1":{"name":"Coins","type":"integer","value":1},
			"var_a1":{"name":"Inventory","type":"string","isArray":true,"value":["dagger"]},
			"var_m1":{"name":"Reputation","type":"map","keyType":"string","valueType":"integer","value":[{"key":"guards","value":-2}]}}}}})JSON");

	// The two fixture characters' ids and record keys, shared by every suite that asserts on them.
	inline const TCHAR* HeroId = TEXT("da_hero0001");
	inline const TCHAR* VillainId = TEXT("da_villain1");
	inline const TCHAR* HeroKey = TEXT("chars\\hero.sfc");
	inline const TCHAR* VillainKey = TEXT("chars\\villain.sfc");

	// The divergence suites grow the fixture by ONE character — a save taken before it existed
	// cannot carry it. The newcomer is defined as FRAGMENTS spliced into the two-character
	// literals above (GrowFixtureJson), so the hero and villain halves can never drift from the
	// shared fixture.
	inline const TCHAR* NewcomerId = TEXT("da_newcomer1");
	inline const TCHAR* NewcomerKey = TEXT("chars\\newcomer.sfc");
	inline const TCHAR* NewcomerIndexEntry = TEXT(R"JSON("da_newcomer1":"chars\\newcomer.sfc")JSON");
	inline const TCHAR* NewcomerVariablesRecord = TEXT(R"JSON("chars\\newcomer.sfc":{"name":"Newcomer","variables":{"var_s1":{"name":"Title","type":"string","value":"the new"}}})JSON");

	/**
	 * Append one more record inside a fixture literal's outermost object: both literals above
	 * end with exactly two closing braces (the keyed collection, then the root), so stripping
	 * those, appending, and re-closing grows the collection without forking the literal.
	 */
	inline FString GrowFixtureJson(const TCHAR* TwoCharacterJson, const TCHAR* AppendedRecord)
	{
		FString Json = TwoCharacterJson;
		verify(Json.RemoveFromEnd(TEXT("}}")));
		return Json + TEXT(",") + AppendedRecord + TEXT("}}");
	}

	/** TwoCharacterIndex grown by the newcomer's entry. */
	inline FString ThreeCharacterIndexJson()
	{
		return GrowFixtureJson(TwoCharacterIndex, NewcomerIndexEntry);
	}

	/** TwoCharacterVariablesJson grown by the newcomer's record. */
	inline FString ThreeCharacterVariablesJsonString()
	{
		return GrowFixtureJson(TwoCharacterVariablesJson, NewcomerVariablesRecord);
	}

	// ========================================================================
	// Script builders shared by the resolution and save suites
	// ========================================================================

	inline FStoryFlowNode MakeNode(const FString& Id, EStoryFlowNodeType Type, const TCHAR* TypeString)
	{
		FStoryFlowNode N;
		N.Id = Id;
		N.Type = Type;
		N.TypeString = TypeString;
		return N;
	}

	inline FStoryFlowConnection MakeEdge(const FString& Source, const FString& Target,
		const FString& SourceHandle, const FString& TargetHandle)
	{
		FStoryFlowConnection C;
		C.Id = Source + TEXT("->") + Target + TEXT("@") + TargetHandle;
		C.Source = Source;
		C.Target = Target;
		C.SourceHandle = SourceHandle;
		C.TargetHandle = TargetHandle;
		return C;
	}

	/** A setCharacterVar node bound by (id, path) — pass empty strings for the unbound halves. */
	inline FStoryFlowNode MakeCharSetter(const FString& Id, const TCHAR* CharacterId, const TCHAR* CharacterPath,
		const TCHAR* VariableName, const TCHAR* VariableType, bool bIsArray = false)
	{
		FStoryFlowNode N = MakeNode(Id, EStoryFlowNodeType::SetCharacterVar, TEXT("setCharacterVar"));
		N.Data.CharacterId = CharacterId;
		N.Data.CharacterPath = CharacterPath;
		N.Data.VariableName = VariableName;
		N.Data.VariableType = VariableType;
		N.Data.bIsArray = bIsArray;
		return N;
	}

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
	 * export shape. CharactersJson == nullptr keeps the original name-only character records;
	 * the resolution suites pass TwoCharacterVariablesJson. Returns false if any write
	 * failed, so callers can fail fast.
	 */
	inline bool WriteFixture(const TCHAR* IndexJson, const TCHAR* CharactersJson = nullptr)
	{
		const FString Dir = FixtureBuildDir();
		// Delete any leftover index first: an absent-file test after a present-file test
		// would otherwise read the previous test's artifact.
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		bool bWritten = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")));
		bWritten = bWritten && FFileHelper::SaveStringToFile(
			CharactersJson ? CharactersJson : TEXT(R"JSON({"characters":{"chars\\hero.sfc":{"name":"Hero"},"chars\\villain.sfc":{"name":"Villain"}}})JSON"),
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
	inline UStoryFlowProjectAsset* ImportFixture(FAutomationTestBase& Test, const TCHAR* IndexJson, const TCHAR* CharactersJson = nullptr)
	{
		UEditorAssetLibrary::DeleteDirectory(TestRoot);
		if (!Test.TestTrue(TEXT("the fixture build folder is writable"), WriteFixture(IndexJson, CharactersJson)))
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
