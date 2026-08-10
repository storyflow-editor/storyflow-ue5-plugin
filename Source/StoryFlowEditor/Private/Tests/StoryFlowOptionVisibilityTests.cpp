// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Dialogue option visibility - the first coverage for EvaluateOptionVisibility.
 *
 * Regression cover for the forEach-element bug: the scalar loop handler stored the
 * current element ONLY in NodeRuntimeState.CachedOutput, HandleDialogue clears the
 * whole evaluation cache right before building option state, and the boolean
 * evaluator had no ForEachBoolLoop arm - so a loop element wired into an option
 * condition always read false and the option was silently hidden. Same story one
 * step removed for the other scalar loops: their evaluator arms read CachedOutput
 * too, so a comparison on a loop element used as a condition read the type default
 * after the clear. The fix reads the element from the PERSISTENT loop fields
 * (LoopArray/LoopIndex survive ClearEvaluationCache, like forEachMap's
 * LoopKey/LoopValue).
 *
 * Run via: Session Frontend > Automation > "StoryFlow.OptionVisibility", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.OptionVisibility" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowOptionVisibilityTestsPrivate
{

const TCHAR* FixtureDir = TEXT("/Game/StoryFlowOptionVisibilityTests");

// One script, three dialogues, each with a single conditioned option:
//  - dialogue 10 optA <- forEachBoolLoop 30's element output (direct wire)
//  - dialogue 11 optB <- runScript 40's boolean output v1 (guard: worked before the fix)
//  - dialogue 12 optC <- equalInt 51 comparing forEachIntLoop 50's element against 2
UStoryFlowScriptAsset* ImportFixture()
{
	const FString Json = TEXT(R"JSON(
	{
		"startNode": "0",
		"nodes": {
			"0":  { "type": "start", "id": "0" },
			"10": { "type": "dialogue", "id": "10", "text": "bool loop" },
			"11": { "type": "dialogue", "id": "11", "text": "run script" },
			"12": { "type": "dialogue", "id": "12", "text": "int compare" },
			"30": { "type": "forEachBoolLoop", "id": "30" },
			"40": { "type": "runScript", "id": "40", "value": "helper.sfe",
				"scriptInterface": { "outputs": [ { "id": "v1", "name": "Flag", "type": "boolean" } ] } },
			"50": { "type": "forEachIntLoop", "id": "50" },
			"51": { "type": "equalInt", "id": "51", "value2": 2 }
		},
		"connections": [
			{ "id": "c1", "source": "30", "target": "10", "sourceHandle": "source-30-boolean-1", "targetHandle": "target-10-boolean-optA" },
			{ "id": "c2", "source": "40", "target": "11", "sourceHandle": "source-40-boolean-out-v1", "targetHandle": "target-11-boolean-optB" },
			{ "id": "c3", "source": "50", "target": "51", "sourceHandle": "source-50-integer-1", "targetHandle": "target-51-integer-1" },
			{ "id": "c4", "source": "51", "target": "12", "sourceHandle": "source-51-boolean-1", "targetHandle": "target-12-boolean-optC" }
		],
		"variables": {}
	}
	)JSON");

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return nullptr;
	}
	return UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("option_visibility_test"), FixtureDir);
}

void SeedBoolLoop(FStoryFlowExecutionContext& Context, const FString& NodeId, const TArray<bool>& Elements, int32 Index)
{
	FNodeRuntimeState& State = Context.GetNodeState(NodeId);
	State.LoopArray.Reset();
	for (bool bElement : Elements)
	{
		State.LoopArray.Add(FStoryFlowVariant::FromBool(bElement));
	}
	State.LoopIndex = Index;
	State.bLoopInitialized = true;
	// Mirror the live handler: the element also lands in the cache each iteration.
	State.CachedOutput = State.LoopArray[Index];
	State.bHasCachedOutput = true;
}

void SeedIntLoop(FStoryFlowExecutionContext& Context, const FString& NodeId, const TArray<int32>& Elements, int32 Index)
{
	FNodeRuntimeState& State = Context.GetNodeState(NodeId);
	State.LoopArray.Reset();
	for (int32 Element : Elements)
	{
		State.LoopArray.Add(FStoryFlowVariant::FromInt(Element));
	}
	State.LoopIndex = Index;
	State.bLoopInitialized = true;
	State.CachedOutput = State.LoopArray[Index];
	State.bHasCachedOutput = true;
}

} // namespace StoryFlowOptionVisibilityTestsPrivate

// ============================================================================
// forEachBoolLoop element wired directly as an option condition
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowOptionVisibilityBoolLoopElementTest,
	"StoryFlow.OptionVisibility.ForEachBoolLoopElementSurvivesDialogueCacheClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowOptionVisibilityBoolLoopElementTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowOptionVisibilityTestsPrivate;

	UStoryFlowScriptAsset* Script = ImportFixture();
	if (!TestNotNull(TEXT("fixture imported"), Script))
	{
		return false;
	}

	FStoryFlowExecutionContext Context;
	Context.CurrentScript = Script;
	Context.LocalVariables = Script->Variables;
	Context.RebuildLocalNameIndex();
	FStoryFlowEvaluator Evaluator(&Context);
	FStoryFlowNode* Dialogue = Context.GetNode(TEXT("10"));
	if (!TestNotNull(TEXT("dialogue node present"), Dialogue))
	{
		UEditorAssetLibrary::DeleteDirectory(FixtureDir);
		return false;
	}

	// Element TRUE at index 1, then the exact clear HandleDialogue performs before
	// building option state. The persistent loop fields must carry the read.
	SeedBoolLoop(Context, TEXT("30"), { false, true, false }, 1);
	Context.ClearEvaluationCache();
	TestTrue(TEXT("TRUE loop element keeps the option visible after the dialogue cache clear"),
		Evaluator.EvaluateOptionVisibility(Dialogue, TEXT("optA")));

	// Element FALSE at index 0 - the option must hide, proving the value is read,
	// not defaulted.
	SeedBoolLoop(Context, TEXT("30"), { false, true, false }, 0);
	Context.ClearEvaluationCache();
	TestFalse(TEXT("FALSE loop element hides the option after the dialogue cache clear"),
		Evaluator.EvaluateOptionVisibility(Dialogue, TEXT("optA")));

	UEditorAssetLibrary::DeleteDirectory(FixtureDir);
	return true;
}

// ============================================================================
// runScript boolean output as an option condition (guard - worked before the fix)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowOptionVisibilityRunScriptOutputTest,
	"StoryFlow.OptionVisibility.RunScriptBooleanOutputFiltersOption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowOptionVisibilityRunScriptOutputTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowOptionVisibilityTestsPrivate;

	UStoryFlowScriptAsset* Script = ImportFixture();
	if (!TestNotNull(TEXT("fixture imported"), Script))
	{
		return false;
	}

	FStoryFlowExecutionContext Context;
	Context.CurrentScript = Script;
	Context.LocalVariables = Script->Variables;
	Context.RebuildLocalNameIndex();
	FStoryFlowEvaluator Evaluator(&Context);
	FStoryFlowNode* Dialogue = Context.GetNode(TEXT("11"));
	if (!TestNotNull(TEXT("dialogue node present"), Dialogue))
	{
		UEditorAssetLibrary::DeleteDirectory(FixtureDir);
		return false;
	}

	FNodeRuntimeState& RunScriptState = Context.GetNodeState(TEXT("40"));
	RunScriptState.OutputValues.Add(TEXT("Flag"), FStoryFlowVariant::FromBool(false));
	RunScriptState.bHasOutputValues = true;
	Context.ClearEvaluationCache();
	TestFalse(TEXT("FALSE script output hides the option"),
		Evaluator.EvaluateOptionVisibility(Dialogue, TEXT("optB")));

	RunScriptState.OutputValues[TEXT("Flag")] = FStoryFlowVariant::FromBool(true);
	Context.ClearEvaluationCache();
	TestTrue(TEXT("TRUE script output shows the option"),
		Evaluator.EvaluateOptionVisibility(Dialogue, TEXT("optB")));

	UEditorAssetLibrary::DeleteDirectory(FixtureDir);
	return true;
}

// ============================================================================
// Integer comparison on a forEachIntLoop element as an option condition
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowOptionVisibilityIntLoopComparisonTest,
	"StoryFlow.OptionVisibility.IntComparisonOnLoopElementSurvivesDialogueCacheClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowOptionVisibilityIntLoopComparisonTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowOptionVisibilityTestsPrivate;

	UStoryFlowScriptAsset* Script = ImportFixture();
	if (!TestNotNull(TEXT("fixture imported"), Script))
	{
		return false;
	}

	FStoryFlowExecutionContext Context;
	Context.CurrentScript = Script;
	Context.LocalVariables = Script->Variables;
	Context.RebuildLocalNameIndex();
	FStoryFlowEvaluator Evaluator(&Context);
	FStoryFlowNode* Dialogue = Context.GetNode(TEXT("12"));
	if (!TestNotNull(TEXT("dialogue node present"), Dialogue))
	{
		UEditorAssetLibrary::DeleteDirectory(FixtureDir);
		return false;
	}

	// Element 2 at index 1: equalInt(element, 2) is true - option visible.
	SeedIntLoop(Context, TEXT("50"), { 1, 2, 3 }, 1);
	Context.ClearEvaluationCache();
	TestTrue(TEXT("loop element 2 == 2 keeps the option visible after the dialogue cache clear"),
		Evaluator.EvaluateOptionVisibility(Dialogue, TEXT("optC")));

	// Element 3 at index 2: comparison false - option hidden.
	SeedIntLoop(Context, TEXT("50"), { 1, 2, 3 }, 2);
	Context.ClearEvaluationCache();
	TestFalse(TEXT("loop element 3 == 2 hides the option"),
		Evaluator.EvaluateOptionVisibility(Dialogue, TEXT("optC")));

	UEditorAssetLibrary::DeleteDirectory(FixtureDir);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
