// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests for passing MAP variables as runScript parameters.
 *
 * Regression cover for the Unreal-only bug where a map passed into a sub-script's
 * map parameter arrived empty (scalars/arrays and the global map read worked, only
 * the map param hand-off failed). Two root causes, both guarded here:
 *   1. The importer parsed only id/name/type/isArray for scriptInterface params and
 *      dropped keyType/valueType, so the runtime could not rebuild the map handle.
 *   2. HandleRunScript looked up the map param edge with the plain "map-param-{id}"
 *      suffix, but the editor wires it with the K/V-bearing
 *      "map-{keyType}-{valueType}-param-{id}" (buildMapHandleId), so FindInputEdge
 *      missed and the param silently passed an empty map.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.RunScriptParams", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.RunScriptParams" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

// ============================================================================
// Import
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRunScriptMapParamImportTest,
	"StoryFlow.RunScriptParams.ImportParsesMapParamKeyValueTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowRunScriptMapParamImportTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT(R"JSON(
	{
		"startNode": "0",
		"nodes": {
			"0": { "type": "start", "id": "0" },
			"1": {
				"type": "runScript", "id": "1", "value": "child.sfe",
				"scriptInterface": {
					"parameters": [
						{ "id": "p1", "name": "MapThatWasAdded", "type": "map", "isArray": false, "keyType": "string", "valueType": "string" }
					]
				}
			},
			"2": { "type": "end", "id": "2" }
		},
		"connections": [
			{ "id": "c1", "source": "0", "target": "1", "sourceHandle": "source-0-", "targetHandle": "target-1-0" },
			{ "id": "c2", "source": "1", "target": "2", "sourceHandle": "source-1-output", "targetHandle": "target-2-" }
		],
		"variables": {}
	}
	)JSON");

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!TestTrue(TEXT("Fixture JSON parses"), FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid()))
	{
		return false;
	}

	UStoryFlowScriptAsset* Script = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("runscript_param_import_test"), TEXT("/Game/StoryFlowRunScriptParamTests"));
	if (!TestNotNull(TEXT("Script imported"), Script))
	{
		return false;
	}

	const FStoryFlowNode* RunScriptNode = Script->Nodes.Find(TEXT("1"));
	if (TestNotNull(TEXT("runScript node imported"), RunScriptNode))
	{
		if (TestEqual(TEXT("one map parameter imported"), RunScriptNode->Data.ScriptParameters.Num(), 1))
		{
			const FStoryFlowScriptInterfaceParam& Param = RunScriptNode->Data.ScriptParameters[0];
			TestEqual(TEXT("param name preserved"), Param.Name, FString(TEXT("MapThatWasAdded")));
			TestEqual(TEXT("param type preserved"), Param.Type, FString(TEXT("map")));
			// The regression: these two were dropped by the importer.
			TestEqual(TEXT("map param keeps keyType"), Param.KeyType, FString(TEXT("string")));
			TestEqual(TEXT("map param keeps valueType"), Param.ValueType, FString(TEXT("string")));
		}
	}

	// Remove the asset the importer saved into the project.
	UEditorAssetLibrary::DeleteDirectory(TEXT("/Game/StoryFlowRunScriptParamTests"));

	return true;
}

// ============================================================================
// Handle suffix contract
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowRunScriptMapParamHandleTest,
	"StoryFlow.RunScriptParams.MapParamHandleUsesKeyValueSuffix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowRunScriptMapParamHandleTest::RunTest(const FString& Parameters)
{
	// Build the minimal wiring the editor produces: a getMap source feeding a
	// runScript node's map parameter. The editor wires the target handle via
	// buildMapHandleId -> "target-{rs}-map-{keyType}-{valueType}-param-{id}".
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());

	FStoryFlowNode RunScriptNode;
	RunScriptNode.Id = TEXT("1");
	RunScriptNode.Type = EStoryFlowNodeType::RunScript;
	RunScriptNode.TypeString = TEXT("runScript");
	Script->Nodes.Add(RunScriptNode.Id, RunScriptNode);

	FStoryFlowNode GetMapNode;
	GetMapNode.Id = TEXT("2");
	GetMapNode.Type = EStoryFlowNodeType::GetMap;
	GetMapNode.TypeString = TEXT("getMap");
	Script->Nodes.Add(GetMapNode.Id, GetMapNode);

	FStoryFlowConnection MapEdge;
	MapEdge.Id = TEXT("c1");
	MapEdge.Source = TEXT("2");
	MapEdge.Target = TEXT("1");
	MapEdge.SourceHandle = TEXT("source-2-map-string-string");
	MapEdge.TargetHandle = TEXT("target-1-map-string-string-param-p1");
	Script->Connections.Add(MapEdge);

	Script->BuildConnectionIndices();

	// The fix: HandleRunScript rebuilds this K/V-bearing suffix and must find the edge.
	const FString FixedSuffix = StoryFlowHandles::In_Map(TEXT("string"), TEXT("string"), TEXT("param-p1"));
	TestNotNull(TEXT("K/V-bearing suffix finds the wired map param edge"),
		Script->FindInputEdge(TEXT("1"), FixedSuffix));

	// Regression guard: the old plain "map-param-{id}" suffix must NOT match the
	// editor's wire — that mismatch was the bug.
	TestNull(TEXT("old plain map-param suffix does not match the wired edge"),
		Script->FindInputEdge(TEXT("1"), TEXT("map-param-p1")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
