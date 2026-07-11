// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests that dialogue-node presentation tags import from .sfe JSON.
 *
 * "tags" is an OPTIONAL additive field on dialogue nodes — a non-empty array of
 * arbitrary strings (spaces, numbers, unicode all legal). Absent or empty means
 * "no tags", never an error. The importer must tolerate all three shapes and
 * preserve the strings untouched and in authored order (the runtime fires one
 * OnDialogueTagReached event per tag, in that order, when the node is entered).
 *
 * Run via: Session Frontend > Automation > "StoryFlow.DialogueTags", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.DialogueTags" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowDialogueTagTestHelpers
{
	const TCHAR* TestRoot = TEXT("/Game/StoryFlowDialogueTagTests");

	// Three dialogue nodes exercise the three shapes: tagged, untagged (field
	// absent — the backward-compat case), and an explicit empty array.
	TSharedPtr<FJsonObject> TaggedScriptJson()
	{
		const FString Json = TEXT(R"JSON(
		{
			"startNode": "0",
			"nodes": {
				"0": { "type": "start", "id": "0" },
				"1": { "type": "dialogue", "id": "1", "title": "t", "text": "hello", "tags": ["boom", "shake it", "42", "élan"] },
				"2": { "type": "dialogue", "id": "2", "title": "t", "text": "no tags here" },
				"3": { "type": "dialogue", "id": "3", "title": "t", "text": "empty tags", "tags": [] },
				"4": { "type": "end", "id": "4" }
			},
			"connections": [],
			"variables": {}
		}
		)JSON");

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, JsonObject);
		return JsonObject;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDialogueTagImportTest,
	"StoryFlow.DialogueTags.ImportParsesTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDialogueTagImportTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDialogueTagTestHelpers;

	TSharedPtr<FJsonObject> JsonObject = TaggedScriptJson();
	if (!TestTrue(TEXT("Fixture JSON parses"), JsonObject.IsValid()))
	{
		return false;
	}

	UStoryFlowScriptAsset* Script = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("dialogue_tag_import_test"), TestRoot);
	if (!TestNotNull(TEXT("Script imported"), Script))
	{
		return false;
	}

	// Tagged node: tags preserved verbatim and in authored order.
	const FStoryFlowNode* Tagged = Script->Nodes.Find(TEXT("1"));
	if (TestNotNull(TEXT("tagged dialogue node imported"), Tagged))
	{
		if (TestEqual(TEXT("all four tags imported"), Tagged->Data.Tags.Num(), 4))
		{
			TestEqual(TEXT("tag[0] preserved"), Tagged->Data.Tags[0], FString(TEXT("boom")));
			TestEqual(TEXT("tag[1] preserves spaces"), Tagged->Data.Tags[1], FString(TEXT("shake it")));
			TestEqual(TEXT("tag[2] numeric string preserved"), Tagged->Data.Tags[2], FString(TEXT("42")));
			TestEqual(TEXT("tag[3] unicode preserved"), Tagged->Data.Tags[3], FString(TEXT("élan")));
		}
	}

	// Untagged node (field absent): behaves exactly as before — no tags, no error.
	const FStoryFlowNode* Untagged = Script->Nodes.Find(TEXT("2"));
	if (TestNotNull(TEXT("untagged dialogue node imported"), Untagged))
	{
		TestEqual(TEXT("absent tags field yields empty array"), Untagged->Data.Tags.Num(), 0);
	}

	// Explicit empty array: also yields no tags.
	const FStoryFlowNode* EmptyTags = Script->Nodes.Find(TEXT("3"));
	if (TestNotNull(TEXT("empty-tags dialogue node imported"), EmptyTags))
	{
		TestEqual(TEXT("empty tags array yields empty array"), EmptyTags->Data.Tags.Num(), 0);
	}

	UEditorAssetLibrary::DeleteDirectory(TestRoot);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
