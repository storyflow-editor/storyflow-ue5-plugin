// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/**
 * Tests for the intToEnum / stringToEnum conversion nodes.
 *
 * Covers two bugs (mirrors the Godot plugin's tests/test_enum_conversions.gd):
 *   1. FStoryFlowNodeData had no EnumValues field and the importer never
 *      parsed "enumValues" from node data, so node-level enum value lists
 *      never survived import.
 *   2. Enum inputs are pulled through EvaluateStringFromNode(), which had no
 *      IntToEnum / StringToEnum cases, so the conversions always evaluated
 *      to "". Editor exports also carry no data on conversion nodes at all -
 *      the enum values must be resolved from the downstream node, matching
 *      the HTML runtime.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.EnumConversions", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.EnumConversions" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowEnumConversionTestHelpers
{
	const TCHAR* ConversionNodeId = TEXT("2");
	const TCHAR* SetEnumNodeId = TEXT("1");

	TArray<FString> AppleBananaCherry()
	{
		return { TEXT("apple"), TEXT("banana"), TEXT("cherry") };
	}

	/**
	 * Build a transient script asset with the minimal conversion graph:
	 * start(0) -> setEnum(1) -> end(3), with conversion node (2) wired into
	 * the setEnum's enum input.
	 */
	UStoryFlowScriptAsset* BuildConversionScript(
		EStoryFlowNodeType ConversionType,
		const FString& ConversionTypeString,
		const FStoryFlowVariant& InlineValue,
		const TArray<FString>& NodeEnumValues,
		const TArray<FString>& VariableEnumValues)
	{
		UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());

		FStoryFlowNode StartNode;
		StartNode.Id = TEXT("0");
		StartNode.Type = EStoryFlowNodeType::Start;
		StartNode.TypeString = TEXT("start");
		Script->Nodes.Add(StartNode.Id, StartNode);

		FStoryFlowNode SetEnumNode;
		SetEnumNode.Id = SetEnumNodeId;
		SetEnumNode.Type = EStoryFlowNodeType::SetEnum;
		SetEnumNode.TypeString = TEXT("setEnum");
		SetEnumNode.Data.Variable = TEXT("var1");
		Script->Nodes.Add(SetEnumNode.Id, SetEnumNode);

		FStoryFlowNode ConversionNode;
		ConversionNode.Id = ConversionNodeId;
		ConversionNode.Type = ConversionType;
		ConversionNode.TypeString = ConversionTypeString;
		ConversionNode.Data.Value = InlineValue;
		ConversionNode.Data.EnumValues = NodeEnumValues;
		Script->Nodes.Add(ConversionNode.Id, ConversionNode);

		FStoryFlowNode EndNode;
		EndNode.Id = TEXT("3");
		EndNode.Type = EStoryFlowNodeType::End;
		EndNode.TypeString = TEXT("end");
		Script->Nodes.Add(EndNode.Id, EndNode);

		FStoryFlowConnection FlowIn;
		FlowIn.Id = TEXT("c1");
		FlowIn.Source = TEXT("0");
		FlowIn.Target = SetEnumNodeId;
		FlowIn.SourceHandle = TEXT("source-0-");
		FlowIn.TargetHandle = TEXT("target-1-");
		Script->Connections.Add(FlowIn);

		FStoryFlowConnection EnumEdge;
		EnumEdge.Id = TEXT("c2");
		EnumEdge.Source = ConversionNodeId;
		EnumEdge.Target = SetEnumNodeId;
		EnumEdge.SourceHandle = TEXT("source-2-enum-2");
		EnumEdge.TargetHandle = TEXT("target-1-enum-1");
		Script->Connections.Add(EnumEdge);

		FStoryFlowConnection FlowOut;
		FlowOut.Id = TEXT("c3");
		FlowOut.Source = SetEnumNodeId;
		FlowOut.Target = TEXT("3");
		FlowOut.SourceHandle = TEXT("source-1-1");
		FlowOut.TargetHandle = TEXT("target-3-");
		Script->Connections.Add(FlowOut);

		FStoryFlowVariable Variable;
		Variable.Id = TEXT("var1");
		Variable.Name = TEXT("result");
		Variable.Type = EStoryFlowVariableType::Enum;
		Variable.Value.SetEnum(TEXT(""));
		Variable.EnumValues = VariableEnumValues;
		Script->Variables.Add(Variable.Id, Variable);

		Script->BuildConnectionIndices();
		return Script;
	}

	/**
	 * Evaluate the setEnum node's enum input exactly as HandleSetEnum does.
	 * Pre-fix this returns "" because EvaluateStringFromNode has no
	 * IntToEnum/StringToEnum cases.
	 */
	FString EvaluateSetEnumInput(UStoryFlowScriptAsset* Script)
	{
		FStoryFlowExecutionContext Context;
		Context.CurrentScript = Script;
		Context.LocalVariables = Script->Variables;
		Context.RebuildLocalNameIndex();

		FStoryFlowEvaluator Evaluator(&Context);
		FStoryFlowNode* SetEnumNode = Context.GetNode(SetEnumNodeId);
		return SetEnumNode ? Evaluator.EvaluateEnumInput(SetEnumNode, StoryFlowHandles::In_Enum, TEXT("")) : TEXT("<setEnum node missing>");
	}
}

// ============================================================================
// Import
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowEnumConversionImportTest,
	"StoryFlow.EnumConversions.ImportPreservesEnumValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowEnumConversionImportTest::RunTest(const FString& Parameters)
{
	// Node fields are flat on the node object (the editor's exported format).
	const FString Json = TEXT(R"JSON(
	{
		"startNode": "0",
		"nodes": {
			"0": { "type": "start", "id": "0" },
			"1": { "type": "setEnum", "id": "1", "variable": "var1", "isGlobal": false, "enumValues": ["apple", "banana", "cherry"] },
			"2": { "type": "intToEnum", "id": "2", "value": 2, "enumValues": ["apple", "banana", "cherry"] },
			"3": { "type": "end", "id": "3" }
		},
		"connections": [
			{ "id": "c1", "source": "0", "target": "1", "sourceHandle": "source-0-", "targetHandle": "target-1-" },
			{ "id": "c2", "source": "2", "target": "1", "sourceHandle": "source-2-enum-2", "targetHandle": "target-1-enum-1" },
			{ "id": "c3", "source": "1", "target": "3", "sourceHandle": "source-1-1", "targetHandle": "target-3-" }
		],
		"variables": {
			"var1": { "name": "result", "type": "enum", "value": "", "enumValues": ["apple", "banana", "cherry"] }
		}
	}
	)JSON");

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!TestTrue(TEXT("Fixture JSON parses"), FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid()))
	{
		return false;
	}

	UStoryFlowScriptAsset* Script = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("enum_conv_import_test"), TEXT("/Game/StoryFlowTests"));
	if (!TestNotNull(TEXT("Script imported"), Script))
	{
		return false;
	}

	const FStoryFlowNode* ConversionNode = Script->Nodes.Find(TEXT("2"));
	const FStoryFlowNode* SetEnumNode = Script->Nodes.Find(TEXT("1"));
	if (TestNotNull(TEXT("intToEnum node imported"), ConversionNode))
	{
		TestEqual(TEXT("intToEnum node keeps enumValues"), ConversionNode->Data.EnumValues, StoryFlowEnumConversionTestHelpers::AppleBananaCherry());
	}
	if (TestNotNull(TEXT("setEnum node imported"), SetEnumNode))
	{
		TestEqual(TEXT("setEnum node keeps enumValues"), SetEnumNode->Data.EnumValues, StoryFlowEnumConversionTestHelpers::AppleBananaCherry());
	}

	// Remove the asset the importer saved into the project.
	UEditorAssetLibrary::DeleteDirectory(TEXT("/Game/StoryFlowTests"));

	return true;
}

// ============================================================================
// Evaluation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowIntToEnumOwnValuesTest,
	"StoryFlow.EnumConversions.IntToEnumUsesOwnEnumValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowIntToEnumOwnValuesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowEnumConversionTestHelpers;

	FStoryFlowVariant IndexValue;
	IndexValue.SetInt(2);
	UStoryFlowScriptAsset* Script = BuildConversionScript(
		EStoryFlowNodeType::IntToEnum, TEXT("intToEnum"), IndexValue,
		/*NodeEnumValues=*/AppleBananaCherry(), /*VariableEnumValues=*/{});

	TestEqual(TEXT("Index 2 converts via the node's own enumValues"), EvaluateSetEnumInput(Script), FString(TEXT("cherry")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowIntToEnumDownstreamVariableTest,
	"StoryFlow.EnumConversions.IntToEnumResolvesDownstreamVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowIntToEnumDownstreamVariableTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowEnumConversionTestHelpers;

	// Editor exports write conversion nodes with no data - the values come
	// from the variable of the downstream setEnum node (HTML runtime parity).
	FStoryFlowVariant IndexValue;
	IndexValue.SetInt(1);
	UStoryFlowScriptAsset* Script = BuildConversionScript(
		EStoryFlowNodeType::IntToEnum, TEXT("intToEnum"), IndexValue,
		/*NodeEnumValues=*/{}, /*VariableEnumValues=*/AppleBananaCherry());

	TestEqual(TEXT("Index 1 converts via the downstream variable's enumValues"), EvaluateSetEnumInput(Script), FString(TEXT("banana")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowIntToEnumClampsIndexTest,
	"StoryFlow.EnumConversions.IntToEnumClampsIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowIntToEnumClampsIndexTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowEnumConversionTestHelpers;

	FStoryFlowVariant IndexValue;
	IndexValue.SetInt(99);
	UStoryFlowScriptAsset* Script = BuildConversionScript(
		EStoryFlowNodeType::IntToEnum, TEXT("intToEnum"), IndexValue,
		/*NodeEnumValues=*/AppleBananaCherry(), /*VariableEnumValues=*/{});

	TestEqual(TEXT("Out-of-range index clamps to the last value"), EvaluateSetEnumInput(Script), FString(TEXT("cherry")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowStringToEnumMatchTest,
	"StoryFlow.EnumConversions.StringToEnumPassesMatchThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowStringToEnumMatchTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowEnumConversionTestHelpers;

	FStoryFlowVariant StringValue;
	StringValue.SetString(TEXT("banana"));
	UStoryFlowScriptAsset* Script = BuildConversionScript(
		EStoryFlowNodeType::StringToEnum, TEXT("stringToEnum"), StringValue,
		/*NodeEnumValues=*/AppleBananaCherry(), /*VariableEnumValues=*/{});

	TestEqual(TEXT("Matching value passes through"), EvaluateSetEnumInput(Script), FString(TEXT("banana")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowStringToEnumNoMatchTest,
	"StoryFlow.EnumConversions.StringToEnumFallsBackToFirstValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowStringToEnumNoMatchTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowEnumConversionTestHelpers;

	FStoryFlowVariant StringValue;
	StringValue.SetString(TEXT("durian"));
	UStoryFlowScriptAsset* Script = BuildConversionScript(
		EStoryFlowNodeType::StringToEnum, TEXT("stringToEnum"), StringValue,
		/*NodeEnumValues=*/AppleBananaCherry(), /*VariableEnumValues=*/{});

	TestEqual(TEXT("Non-matching value falls back to the first value"), EvaluateSetEnumInput(Script), FString(TEXT("apple")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
