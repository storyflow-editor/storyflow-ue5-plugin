// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
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
}

#endif // WITH_DEV_AUTOMATION_TESTS
