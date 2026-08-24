// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Import/StoryFlowImporter.h"
#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowRuntime.h"
#include "StoryFlowScopedWorld.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "EditorAssetLibrary.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

/**
 * The `.sfd` accessor NODE ARMS: the wire-is-the-binding walk, the typed reads, the Set node's
 * overlay write, and the whole §6 degraded ladder against the shared golden fixture.
 *
 * Companion to StoryFlowDataAssetResolutionTests.cpp, which owns the STORE (§4/§5) and the seed,
 * resolution and writes fixtures. This file owns the fourth one:
 *  - data-assets-degraded.json ....... every §6 row, its outcome AND its once-ness
 *
 * Two suites live here on purpose:
 *  - StoryFlow.DataAssets.Degraded ... the fixture-driven ladder (reads + the Set chain)
 *  - StoryFlow.DataAssets.Nodes ...... the happy paths the fixture cannot express (a real
 *    imported graph, cascade through a live Set, copy-on-read, option gating)
 *
 * WHY OPTION GATING GETS A TEST OF ITS OWN: EvaluateBooleanFromNode's `default:` arm returns
 * FALSE, so Unreal fails CLOSED — a missing producer arm does not throw, it silently HIDES the
 * dialogue option the author gated. That failure is invisible in play-testing until someone
 * notices a line that never appears, which is why the boolean arm and its option-gating
 * regression land in the same commit as the node types themselves (contract §6.2).
 *
 * Run via: Session Frontend > Automation > "StoryFlow.DataAssets", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.DataAssets" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowDataAssetNodeTestHelpers
{
	const TCHAR* NodeTestRoot = TEXT("/Game/StoryFlowDataAssetNodeTests");

	// The seed family's ids, annotated, live in StoryFlowEngineContractFixtures.h — shared with
	// the resolution tests.
	using namespace StoryFlowEngineContract;

	FString FixtureBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowDataAssetNodeFixture"));
	}

	void CleanUp()
	{
		UEditorAssetLibrary::DeleteDirectory(NodeTestRoot);
		IFileManager::Get().DeleteDirectory(*FixtureBuildDir(), false, true);
	}

	/**
	 * Import data-assets-seed.json through the REAL importer and hand back both the project (the
	 * component tests hang their script off it, so the subsystem builds the same seed the game
	 * would) and the seed the subsystem installs. Deletes any assets a previous run left behind
	 * first — a stale one carries a matching hash and would be skipped rather than re-parsed.
	 */
	UStoryFlowProjectAsset* ImportFixtureProject(FAutomationTestBase& Test, StoryFlowDataAssets::FSeed& OutSeed)
	{
		const FString SeedPath = FixturePath(TEXT("data-assets-seed.json"));
		FString SeedJson;
		if (!Test.TestTrue(TEXT("data-assets-seed.json is readable"), !SeedPath.IsEmpty() && FFileHelper::LoadFileToString(SeedJson, *SeedPath)))
		{
			return nullptr;
		}

		UEditorAssetLibrary::DeleteDirectory(NodeTestRoot);

		const FString Dir = FixtureBuildDir();
		IFileManager::Get().MakeDirectory(*Dir, true);
		const bool bWrote = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
				*FPaths::Combine(Dir, TEXT("project.json")))
			&& FFileHelper::SaveStringToFile(SeedJson, *FPaths::Combine(Dir, TEXT("data-assets.json")));
		if (!Test.TestTrue(TEXT("the fixture build folder is writable"), bWrote))
		{
			return nullptr;
		}

		UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(Dir, NodeTestRoot);
		if (!Test.TestNotNull(TEXT("the seed fixture imports"), Project))
		{
			CleanUp();
			return nullptr;
		}
		StoryFlowDataAssets::BuildSeed(Project->DataAssets, OutSeed);
		Test.TestEqual(TEXT("the imported seed still carries 3 assets"), OutSeed.Num(), 3);
		return Project;
	}

	/**
	 * Import an arbitrary data-assets.json body, for the one shape the golden seed cannot carry:
	 * an ENUM ARRAY declaration. The shared fixtures are checked in verbatim and must not grow a
	 * row for one engine's test, so this takes the inline route the resolution suite uses.
	 */
	UStoryFlowProjectAsset* ImportInlineSeed(FAutomationTestBase& Test, const FString& DataAssetsJson, StoryFlowDataAssets::FSeed& OutSeed)
	{
		UEditorAssetLibrary::DeleteDirectory(NodeTestRoot);

		const FString Dir = FixtureBuildDir();
		IFileManager::Get().MakeDirectory(*Dir, true);
		const bool bWrote = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
				*FPaths::Combine(Dir, TEXT("project.json")))
			&& FFileHelper::SaveStringToFile(DataAssetsJson, *FPaths::Combine(Dir, TEXT("data-assets.json")));
		if (!Test.TestTrue(TEXT("the inline build folder is writable"), bWrote))
		{
			return nullptr;
		}

		UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(Dir, NodeTestRoot);
		if (!Test.TestNotNull(TEXT("the inline seed imports"), Project))
		{
			CleanUp();
			return nullptr;
		}
		StoryFlowDataAssets::BuildSeed(Project->DataAssets, OutSeed);
		return Project;
	}

	FStoryFlowNode MakeNode(const FString& Id, EStoryFlowNodeType Type, const TCHAR* TypeString)
	{
		FStoryFlowNode N;
		N.Id = Id;
		N.Type = Type;
		N.TypeString = TypeString;
		return N;
	}

	FStoryFlowConnection MakeEdge(const FString& Source, const FString& Target,
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

	/** The `.sfd` reference pill, bound to AssetId (empty = an unbound pill, contract §6 row 2). */
	FStoryFlowNode MakePill(const FString& Id, const FString& AssetId)
	{
		FStoryFlowNode N = MakeNode(Id, EStoryFlowNodeType::GetDataAsset, TEXT("getDataAsset"));
		N.Data.AssetId = AssetId;
		return N;
	}

	/** One accessor carrying a §2.2 spawn snapshot. Type picks Get vs Set; the payload is identical. */
	FStoryFlowNode MakeAccessor(const FString& Id, bool bIsSetter, const TSharedPtr<FJsonObject>& Accessor)
	{
		FStoryFlowNode N = bIsSetter
			? MakeNode(Id, EStoryFlowNodeType::SetDataAssetVariable, TEXT("setDataAssetVariable"))
			: MakeNode(Id, EStoryFlowNodeType::GetDataAssetVariable, TEXT("getDataAssetVariable"));
		Accessor->TryGetStringField(TEXT("variableId"), N.Data.VariableId);
		Accessor->TryGetStringField(TEXT("variable"), N.Data.VariableName);
		N.Data.Variable = N.Data.VariableName;
		Accessor->TryGetStringField(TEXT("variableType"), N.Data.VariableType);
		Accessor->TryGetBoolField(TEXT("isArray"), N.Data.bIsArray);
		Accessor->TryGetStringField(TEXT("keyType"), N.Data.KeyType);
		Accessor->TryGetStringField(TEXT("valueType"), N.Data.ValueType);
		return N;
	}

	/**
	 * A Set accessor with its §2.2 snapshot spelled out. The fixture-driven tests build theirs
	 * from JSON (MakeAccessor); the hand-written ones need array and map snapshots the degraded
	 * fixture never carries in a form that reaches a WRITE.
	 */
	FStoryFlowNode MakeSetter(const FString& Id, const TCHAR* VariableId, const TCHAR* Name,
		const TCHAR* VariableType, bool bIsArray = false, const TCHAR* KeyType = TEXT(""), const TCHAR* ValueType = TEXT(""))
	{
		FStoryFlowNode N = MakeNode(Id, EStoryFlowNodeType::SetDataAssetVariable, TEXT("setDataAssetVariable"));
		N.Data.VariableId = VariableId;
		N.Data.VariableName = Name;
		N.Data.Variable = Name;
		N.Data.VariableType = VariableType;
		N.Data.bIsArray = bIsArray;
		N.Data.KeyType = KeyType;
		N.Data.ValueType = ValueType;
		return N;
	}

	/** The accessor's Data Asset pin, the one edge that IS the binding. */
	FStoryFlowConnection MakePillEdge(const FString& PillId, const FString& AccessorId)
	{
		return MakeEdge(PillId, AccessorId,
			StoryFlowHandles::Source(PillId, TEXT("dataAsset-")),
			StoryFlowHandles::Target(AccessorId, StoryFlowHandles::In_DataAssetRef));
	}

	/** The typed suffix of an accessor's value/output pin for a given optionId (§2.2). */
	FString ValuePinSuffix(const FStoryFlowNodeData& Data, const FString& OptionId)
	{
		if (Data.VariableType == TEXT("map"))
		{
			return StoryFlowHandles::In_Map(Data.KeyType, Data.ValueType, OptionId);
		}
		if (Data.bIsArray)
		{
			return Data.VariableType + TEXT("-array-") + OptionId;
		}
		return Data.VariableType + TEXT("-") + OptionId;
	}

	/** Typed array read off a consumer's wired array input, dispatched on the element type. */
	TArray<FStoryFlowVariant> ReadArray(FStoryFlowEvaluator& Evaluator, FStoryFlowNode* Consumer, const FString& ElementType, const FString& Suffix)
	{
		if (ElementType == TEXT("boolean"))   { return Evaluator.EvaluateBoolArrayInput(Consumer, Suffix); }
		if (ElementType == TEXT("integer"))   { return Evaluator.EvaluateIntArrayInput(Consumer, Suffix); }
		if (ElementType == TEXT("float"))     { return Evaluator.EvaluateFloatArrayInput(Consumer, Suffix); }
		if (ElementType == TEXT("image"))     { return Evaluator.EvaluateImageArrayInput(Consumer, Suffix); }
		if (ElementType == TEXT("character")) { return Evaluator.EvaluateCharacterArrayInput(Consumer, Suffix); }
		if (ElementType == TEXT("audio"))     { return Evaluator.EvaluateAudioArrayInput(Consumer, Suffix); }
		return Evaluator.EvaluateStringArrayInput(Consumer, Suffix);
	}

	/** How many warn latches this context holds for a node (contract §6 once-ness). */
	int32 WarnLatchCount(const FStoryFlowExecutionContext& Context, const FString& NodeId)
	{
		const FString Prefix = NodeId + TEXT("|");
		int32 Count = 0;
		for (const FString& Key : Context.WarnedDataAssetNodes)
		{
			if (Key.StartsWith(Prefix))
			{
				++Count;
			}
		}
		return Count;
	}

	// FScopedWorld — the standalone game instance with a registered component — lives in
	// StoryFlowScopedWorld.h, shared with the save suite.
	using StoryFlowTestWorld::FScopedWorld;
}

// ============================================================================
// §6 degraded ladder — every read outcome and its once-ness, from the fixture
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetDegradedReadsTest,
	"StoryFlow.DataAssets.Degraded.AccessorReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetDegradedReadsTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	TSharedPtr<FJsonObject> Fixture = LoadFixture(TEXT("data-assets-degraded.json"));
	if (!TestTrue(TEXT("data-assets-degraded.json parses"), Fixture.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
	if (!TestTrue(TEXT("the fixture carries a cases array"), Fixture->TryGetArrayField(TEXT("cases"), Cases)))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, Seed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// ONE script holding every case's read graph (unique node ids per case), so the 20 records
	// cost one asset rather than 20. Each case still gets a FRESH execution context, which is
	// what makes the warn-latch count below a per-case assertion rather than a running total.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

	int32 CaseIndex = 0;
	for (const TSharedPtr<FJsonValue>& CaseValue : *Cases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		if (!Case.IsValid())
		{
			AddError(TEXT("cases: a record is not an object"));
			continue;
		}
		const FString Suffix = FString::FromInt(CaseIndex++);
		const FString PillId = TEXT("P") + Suffix;
		const FString AccessorId = TEXT("G") + Suffix;
		const FString ConsumerId = TEXT("C") + Suffix;

		const TSharedPtr<FJsonObject> AccessorJson = Case->GetObjectField(TEXT("accessor"));
		FStoryFlowNode Accessor = MakeAccessor(AccessorId, /*bIsSetter*/ false, AccessorJson);

		// A source that is NOT a ref pill must never have its data read as a binding — hence a
		// decoy node here rather than "no node" for the non-pill row (contract §6 row 1). It is
		// given the case's own assetId in a place the ladder must refuse to look.
		FStoryFlowNode Pill = Case->GetBoolField(TEXT("pillIsRefNode"))
			? MakePill(PillId, Case->GetStringField(TEXT("pillAssetId")))
			: MakeNode(PillId, EStoryFlowNodeType::GetBool, TEXT("getBool"));
		if (!Case->GetBoolField(TEXT("pillIsRefNode")))
		{
			Pill.Data.AssetId = Case->GetStringField(TEXT("pillAssetId"));
		}

		// The consumer exists only so array and map reads have a wired input handle to resolve
		// through; the scalar reads call the typed evaluators on the accessor directly.
		FStoryFlowNode Consumer = MakeNode(ConsumerId, EStoryFlowNodeType::SetBool, TEXT("setBool"));
		Consumer.Data.KeyType = Accessor.Data.KeyType;
		Consumer.Data.ValueType = Accessor.Data.ValueType;

		if (Case->GetBoolField(TEXT("pillWired")))
		{
			Script->Connections.Add(MakePillEdge(PillId, AccessorId));
		}
		// The Get's output pin is optionId "" for scalars and arrays; a map SOURCE pin carries no
		// optionId at all, while the consumer's map INPUT is the pure-read optionId "1" — so the
		// two ends of a map edge are deliberately not the same suffix.
		const bool bIsMap = Accessor.Data.VariableType == TEXT("map");
		const FString ReadSourceSuffix = bIsMap
			? FString::Printf(TEXT("map-%s-%s"), *Accessor.Data.KeyType, *Accessor.Data.ValueType)
			: ValuePinSuffix(Accessor.Data, TEXT(""));
		const FString ReadTargetSuffix = bIsMap
			? StoryFlowHandles::In_Map(Accessor.Data.KeyType, Accessor.Data.ValueType, TEXT("1"))
			: ValuePinSuffix(Accessor.Data, TEXT(""));
		Script->Connections.Add(MakeEdge(AccessorId, ConsumerId,
			StoryFlowHandles::Source(AccessorId, ReadSourceSuffix),
			StoryFlowHandles::Target(ConsumerId, ReadTargetSuffix)));

		Script->Nodes.Add(PillId, Pill);
		Script->Nodes.Add(AccessorId, Accessor);
		Script->Nodes.Add(ConsumerId, Consumer);
	}
	Script->BuildConnectionIndices();
	TestEqual(TEXT("data-assets-degraded.json still carries 20 cases"), CaseIndex, 20);
	TestEqual(TEXT("every degraded case was built"), CaseIndex, Cases->Num());

	// --- read each case twice, against a store with an EMPTY overlay ---
	StoryFlowDataAssets::FOverlay Overlay;
	int32 Asserted = 0;
	CaseIndex = 0;
	for (const TSharedPtr<FJsonValue>& CaseValue : *Cases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		if (!Case.IsValid())
		{
			continue;
		}
		const FString Suffix = FString::FromInt(CaseIndex++);
		const FString AccessorId = TEXT("G") + Suffix;
		const FString ConsumerId = TEXT("C") + Suffix;
		const FString Label = FString::Printf(TEXT("degraded case '%s'"), *Case->GetStringField(TEXT("case")));

		FStoryFlowExecutionContext Context;
		Context.CurrentScript = Script;
		Context.DataAssetStore = { &Seed, &Overlay };
		FStoryFlowEvaluator Evaluator(&Context);

		FStoryFlowNode* Accessor = Context.GetNode(AccessorId);
		FStoryFlowNode* Consumer = Context.GetNode(ConsumerId);
		if (!Accessor || !Consumer)
		{
			AddError(Label + TEXT(": nodes missing from the built script"));
			continue;
		}

		const TSharedPtr<FJsonObject> Get = Case->GetObjectField(TEXT("get"));
		const TSharedPtr<FJsonValue> Expected = Get->TryGetField(TEXT("value"));
		if (!Expected.IsValid())
		{
			AddError(Label + TEXT(": the fixture record carries no expected get value"));
			continue;
		}

		const FString Type = Accessor->Data.VariableType;
		// TWICE, always: the second read is what proves the warning LATCHED rather than the
		// first one merely having been the only read.
		int32 EmittedAfterFirstRead = 0;
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			const FString PassLabel = FString::Printf(TEXT("%s pass %d"), *Label, Pass);
			if (Accessor->Data.bIsArray)
			{
				const TArray<FStoryFlowVariant> Actual = ReadArray(Evaluator, Consumer, Type, ValuePinSuffix(Accessor->Data, TEXT("")));
				TestEqual(PassLabel + TEXT(" (array length)"), Actual.Num(), Expected->AsArray().Num());
			}
			else if (Type == TEXT("map"))
			{
				const TArray<FStoryFlowMapEntry>* Actual = Evaluator.EvaluateMapInput(Consumer, TEXT("1"));
				TestEqual(PassLabel + TEXT(" (map entry count)"), Actual ? Actual->Num() : 0, Expected->AsArray().Num());
			}
			else if (Type == TEXT("boolean"))
			{
				TestTrue(PassLabel + TEXT(" (boolean)"),
					Evaluator.EvaluateBooleanFromNode(Accessor, TEXT(""), TEXT("")) == Expected->AsBool());
			}
			else if (Type == TEXT("integer"))
			{
				TestEqual(PassLabel + TEXT(" (integer)"),
					Evaluator.EvaluateIntegerFromNode(Accessor, TEXT(""), TEXT("")), static_cast<int32>(Expected->AsNumber()));
			}
			else if (Type == TEXT("float"))
			{
				TestNearlyEqual(PassLabel + TEXT(" (float)"),
					static_cast<double>(Evaluator.EvaluateFloatFromNode(Accessor, TEXT(""), TEXT(""))), Expected->AsNumber(), 1.e-4);
			}
			else
			{
				// The whole string family (string / enum / image / character / audio) reads
				// through one evaluator, exactly as the seed stores all five in one field.
				TestEqual(PassLabel + TEXT(" (string family)"),
					Evaluator.EvaluateStringFromNode(Accessor, TEXT(""), TEXT("")), Expected->AsString());
			}

			if (Pass == 0)
			{
				EmittedAfterFirstRead = Context.DataAssetWarningsEmitted;
			}
		}

		// SUPPRESSION, not merely latching. The latch-count assertion further down cannot see
		// this: WarnedDataAssetNodes is a TSet, so a MaybeWarnDataAsset that dropped its early-out
		// would still hold exactly one key while writing a line on every single evaluation — and
		// an option condition re-evaluates every render. Only a counter that moves when a line is
		// actually written tells the two apart.
		const bool bWarnOnce = Case->GetBoolField(TEXT("warnOnce"));
		TestEqual(Label + TEXT(" emitted its warning on the first read"), EmittedAfterFirstRead, bWarnOnce ? 1 : 0);
		TestEqual(Label + TEXT(" emitted NOTHING on the second read"), Context.DataAssetWarningsEmitted, EmittedAfterFirstRead);

		// The WRITE side's gate is this same ladder, so assert its verdict per case here rather
		// than inferring it from the Set chain's end state: a case the fixture refuses for any
		// reason other than the value pin must fail the ladder outright.
		const TSharedPtr<FJsonObject> Set = Case->GetObjectField(TEXT("set"));
		FString Reason;
		Set->TryGetStringField(TEXT("reason"), Reason);
		const bool bLadderShouldPass = Set->GetStringField(TEXT("outcome")) == TEXT("written") || Reason == TEXT("novalue");
		FString ResolvedAssetId;
		TestTrue(FString::Printf(TEXT("%s binding ladder (expected %s)"), *Label, bLadderShouldPass ? TEXT("bound") : TEXT("degraded")),
			Context.TryResolveDataAssetBinding(*Accessor, ResolvedAssetId) == bLadderShouldPass);

		// Once-ness across BOTH reads and the ladder call above: one latch, or none for a case
		// the fixture marks healthy (contract §6, re-armed by a context Reset).
		TestEqual(Label + TEXT(" holds exactly one latch per node"),
			WarnLatchCount(Context, AccessorId), bWarnOnce ? 1 : 0);
		// ...and the ladder call above emitted nothing new either, since it hits the same rung.
		TestEqual(Label + TEXT(" emitted nothing more for the write-side ladder call"),
			Context.DataAssetWarningsEmitted, EmittedAfterFirstRead);
		++Asserted;
	}
	TestEqual(TEXT("every degraded case was asserted"), Asserted, Cases->Num());

	CleanUp();
	return true;
}

// ============================================================================
// §6 degraded ladder — the Set node, driven through the real handler
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetDegradedSetsTest,
	"StoryFlow.DataAssets.Degraded.SetNodeRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetDegradedSetsTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	TSharedPtr<FJsonObject> Fixture = LoadFixture(TEXT("data-assets-degraded.json"));
	if (!TestTrue(TEXT("data-assets-degraded.json parses"), Fixture.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
	if (!TestTrue(TEXT("the fixture carries a cases array"), Fixture->TryGetArrayField(TEXT("cases"), Cases)))
	{
		return false;
	}

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture world initialized"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// Every case's SET node on one exec chain, IN FIXTURE ORDER. Order is load-bearing: the
	// healthy case writes 42 first, and the nineteen refusals that follow all aim at variables
	// it would be visible through — so a refusal that quietly turned into a write shows up as a
	// changed value, not merely as an extra overlay entry.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

	// Two literal sources for the wired value pins: the fixture only ever wires an integer or a
	// string one. Their VALUES barely matter — what matters is that the EDGE exists, so a
	// refusal that stopped refusing would have something to write.
	{
		FStoryFlowVariable IntVar;
		IntVar.Id = TEXT("n");
		IntVar.Name = TEXT("n");
		IntVar.Type = EStoryFlowVariableType::Integer;
		IntVar.Value.SetInt(42);
		Script->Variables.Add(IntVar.Id, IntVar);

		FStoryFlowVariable StrVar;
		StrVar.Id = TEXT("s");
		StrVar.Name = TEXT("s");
		StrVar.Type = EStoryFlowVariableType::String;
		StrVar.Value.SetString(TEXT("written-by-a-broken-gate"));
		Script->Variables.Add(StrVar.Id, StrVar);
	}
	{
		FStoryFlowNode IntSource = MakeNode(TEXT("VInt"), EStoryFlowNodeType::GetInt, TEXT("getInt"));
		IntSource.Data.Variable = TEXT("n");
		Script->Nodes.Add(IntSource.Id, IntSource);

		FStoryFlowNode StrSource = MakeNode(TEXT("VStr"), EStoryFlowNodeType::GetString, TEXT("getString"));
		StrSource.Data.Variable = TEXT("s");
		Script->Nodes.Add(StrSource.Id, StrSource);
	}

	FString PreviousNodeId = TEXT("0");
	FString PreviousExecHandle = StoryFlowHandles::Source(TEXT("0"));
	int32 CaseIndex = 0;
	for (const TSharedPtr<FJsonValue>& CaseValue : *Cases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		if (!Case.IsValid())
		{
			AddError(TEXT("cases: a record is not an object"));
			continue;
		}
		const FString Suffix = FString::FromInt(CaseIndex++);
		const FString PillId = TEXT("P") + Suffix;
		const FString SetId = TEXT("S") + Suffix;

		FStoryFlowNode Setter = MakeAccessor(SetId, /*bIsSetter*/ true, Case->GetObjectField(TEXT("accessor")));

		FStoryFlowNode Pill = Case->GetBoolField(TEXT("pillIsRefNode"))
			? MakePill(PillId, Case->GetStringField(TEXT("pillAssetId")))
			: MakeNode(PillId, EStoryFlowNodeType::GetBool, TEXT("getBool"));
		if (!Case->GetBoolField(TEXT("pillIsRefNode")))
		{
			Pill.Data.AssetId = Case->GetStringField(TEXT("pillAssetId"));
		}
		if (Case->GetBoolField(TEXT("pillWired")))
		{
			Script->Connections.Add(MakePillEdge(PillId, SetId));
		}

		if (Case->GetBoolField(TEXT("setValuePinWired")))
		{
			const bool bWantsString = Setter.Data.VariableType != TEXT("boolean")
				&& Setter.Data.VariableType != TEXT("integer")
				&& Setter.Data.VariableType != TEXT("float");
			const FString SourceId = bWantsString ? TEXT("VStr") : TEXT("VInt");
			const FString ValueSuffix = ValuePinSuffix(Setter.Data, StoryFlowHandles::DataAssetValueOptionId);
			Script->Connections.Add(MakeEdge(SourceId, SetId,
				StoryFlowHandles::Source(SourceId, bWantsString ? TEXT("string-") : TEXT("integer-")),
				StoryFlowHandles::Target(SetId, ValueSuffix)));
		}

		Script->Connections.Add(MakeEdge(PreviousNodeId, SetId, PreviousExecHandle, StoryFlowHandles::Target(SetId, TEXT("0"))));
		PreviousNodeId = SetId;
		PreviousExecHandle = StoryFlowHandles::Source(SetId, StoryFlowHandles::Out_Flow);

		Script->Nodes.Add(PillId, Pill);
		Script->Nodes.Add(SetId, Setter);
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(PreviousNodeId, TEXT("End"), PreviousExecHandle, StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();
	TestEqual(TEXT("every degraded case joined the Set chain"), CaseIndex, Cases->Num());

	Project->Scripts.Add(TEXT("degradedsets"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("degradedsets"));

	// Exactly ONE of the twenty cases is allowed to write, and it must have written its own
	// value. Anything else in the overlay is a refusal that stopped refusing.
	const StoryFlowDataAssets::FOverlay& Overlay = W.Subsystem->GetDataAssetOverlay();
	if (TestEqual(TEXT("only the healthy case's asset has overlay entries"), Overlay.Num(), 1))
	{
		const TMap<FString, FStoryFlowVariant>* ChildEntries = Overlay.Find(ChildId);
		if (TestNotNull(TEXT("the healthy write landed on the wired child, not the declaring base"), ChildEntries))
		{
			TestEqual(TEXT("the healthy case wrote exactly one variable"), ChildEntries->Num(), 1);
			const FStoryFlowVariant* Written = ChildEntries->Find(HpId);
			if (TestNotNull(TEXT("the healthy case wrote hp"), Written))
			{
				// Integer 42, still: the value-pin refusal running later on the SAME variable
				// would blank it, and the type-changed refusal would restamp it as a string.
				TestTrue(TEXT("the write kept its integer type"), Written->GetType() == EStoryFlowVariableType::Integer);
				TestEqual(TEXT("the write kept the value from its wired pin"), Written->GetInt(), 42);
			}
		}
	}

	CleanUp();
	return true;
}

// ============================================================================
// The wire IS the binding — from a really imported graph
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetWireBindingTest,
	"StoryFlow.DataAssets.Nodes.WireIsTheBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetWireBindingTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, Seed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// REAL exported JSON, not hand-built nodes: this is the one test that also pins the
	// IMPORTER's half of §2.2 (assetId on the pill, variableId/variable/variableType/isArray/
	// keyType/valueType on the accessors) and the exact handle strings the editor writes.
	//
	// TWO accessors reading the SAME variable id through DIFFERENT pills is the whole point:
	// the accessors are byte-identical apart from their node id, so anything but the wire
	// deciding which asset they read would make them answer the same number.
	const FString Json = TEXT(R"JSON(
	{
		"startNode": "0",
		"nodes": {
			"0":   { "type": "start", "id": "0" },
			"pB":  { "type": "getDataAsset", "id": "pB", "assetId": "da_0a1b2c3d4e5f60718293a4b5c6d7e8f9" },
			"pC":  { "type": "getDataAsset", "id": "pC", "assetId": "da_1b2c3d4e5f60718293a4b5c6d7e8f90a" },
			"gB":  { "type": "getDataAssetVariable", "id": "gB", "variableId": "2e8b6d0a1f4c47d3b95e2a70c6f81d34", "variable": "hp", "variableType": "integer" },
			"gC":  { "type": "getDataAssetVariable", "id": "gC", "variableId": "2e8b6d0a1f4c47d3b95e2a70c6f81d34", "variable": "hp", "variableType": "integer" },
			"gT":  { "type": "getDataAssetVariable", "id": "gT", "variableId": "c58e2f13a0d64c9b871e3f05d2a76b48", "variable": "tags", "variableType": "string", "isArray": true },
			"gL":  { "type": "getDataAssetVariable", "id": "gL", "variableId": "6d0f39a8b21e47c5903af8d61c72e504", "variable": "loot", "variableType": "map", "keyType": "string", "valueType": "integer" },
			"sink": { "type": "setBool", "id": "sink", "keyType": "string", "valueType": "integer" }
		},
		"connections": [
			{ "id": "c1", "source": "pB", "target": "gB", "sourceHandle": "source-pB-dataAsset-", "targetHandle": "target-gB-dataAsset-asset" },
			{ "id": "c2", "source": "pC", "target": "gC", "sourceHandle": "source-pC-dataAsset-", "targetHandle": "target-gC-dataAsset-asset" },
			{ "id": "c3", "source": "pC", "target": "gT", "sourceHandle": "source-pC-dataAsset-", "targetHandle": "target-gT-dataAsset-asset" },
			{ "id": "c4", "source": "pB", "target": "gL", "sourceHandle": "source-pB-dataAsset-", "targetHandle": "target-gL-dataAsset-asset" },
			{ "id": "c5", "source": "gT", "target": "sink", "sourceHandle": "source-gT-string-array-", "targetHandle": "target-sink-string-array-" },
			{ "id": "c6", "source": "gL", "target": "sink", "sourceHandle": "source-gL-map-string-integer", "targetHandle": "target-sink-map-string-integer-1" }
		],
		"variables": {}
	}
	)JSON");

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!TestTrue(TEXT("the fixture script JSON parses"), FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid()))
	{
		CleanUp();
		return false;
	}
	UStoryFlowScriptAsset* Script = UStoryFlowImporter::ImportScriptFromJson(JsonObject, TEXT("data_asset_wire_test"), NodeTestRoot);
	if (!TestNotNull(TEXT("the fixture script imports"), Script))
	{
		CleanUp();
		return false;
	}

	// The importer's §2.2 payload, asserted before anything reads through it — a silently
	// dropped variableId would otherwise show up only as a degraded read.
	if (const FStoryFlowNode* Pill = Script->Nodes.Find(TEXT("pC")))
	{
		TestEqual(TEXT("the pill kept its assetId"), Pill->Data.AssetId, FString(ChildId));
		TestTrue(TEXT("the pill parsed as a getDataAsset node"), Pill->Type == EStoryFlowNodeType::GetDataAsset);
	}
	else
	{
		AddError(TEXT("the child pill is missing from the imported script"));
	}
	if (const FStoryFlowNode* Map = Script->Nodes.Find(TEXT("gL")))
	{
		TestTrue(TEXT("the accessor parsed as a getDataAssetVariable node"), Map->Type == EStoryFlowNodeType::GetDataAssetVariable);
		TestEqual(TEXT("the accessor kept its variableId"), Map->Data.VariableId, FString(LootId));
		TestEqual(TEXT("the accessor kept its name snapshot"), Map->Data.VariableName, TEXT("loot"));
		TestEqual(TEXT("the accessor kept its type snapshot"), Map->Data.VariableType, TEXT("map"));
		TestEqual(TEXT("the accessor kept its map key type"), Map->Data.KeyType, TEXT("string"));
		TestEqual(TEXT("the accessor kept its map value type"), Map->Data.ValueType, TEXT("integer"));
		TestTrue(TEXT("an accessor carries no assetId of its own - the wire is the binding"), Map->Data.AssetId.IsEmpty());
	}
	else
	{
		AddError(TEXT("the map accessor is missing from the imported script"));
	}
	if (const FStoryFlowNode* Tags = Script->Nodes.Find(TEXT("gT")))
	{
		TestTrue(TEXT("the array accessor kept its isArray snapshot"), Tags->Data.bIsArray);
	}

	StoryFlowDataAssets::FOverlay Overlay;
	FStoryFlowExecutionContext Context;
	Context.CurrentScript = Script;
	Context.DataAssetStore = { &Seed, &Overlay };
	FStoryFlowEvaluator Evaluator(&Context);

	// --- the same variable, two pills, two answers ---
	TestEqual(TEXT("the base-wired accessor reads the base's own hp"),
		Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("gB")), TEXT(""), TEXT("")), 100);
	TestEqual(TEXT("the child-wired accessor reads the child's override"),
		Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("gC")), TEXT(""), TEXT("")), 150);

	// --- typed reads: array and map ---
	FStoryFlowNode* Sink = Context.GetNode(TEXT("sink"));
	TArray<FStoryFlowVariant> Tags = Evaluator.EvaluateStringArrayInput(Sink, TEXT("string-array-"));
	if (TestEqual(TEXT("the array read comes through the child's override"), Tags.Num(), 2))
	{
		TestEqual(TEXT("the array read keeps its authored order"), Tags[1].GetString(), TEXT("elite"));
	}
	if (const TArray<FStoryFlowMapEntry>* Loot = Evaluator.EvaluateMapInput(Sink, TEXT("1")))
	{
		if (TestEqual(TEXT("the map read comes through the base's own entries"), Loot->Num(), 2))
		{
			TestEqual(TEXT("the map read keeps its authored key order"), (*Loot)[0].Key.GetString(), TEXT("gold"));
			TestEqual(TEXT("the map read keeps its values"), (*Loot)[1].Value.GetInt(), 1);
		}
	}
	else
	{
		AddError(TEXT("the map accessor resolved to nothing"));
	}

	// --- copy on read: graph code must not reach the store through a read (contract §3) ---
	Tags.Empty();
	TestEqual(TEXT("emptying a read array leaves the seed alone"),
		Evaluator.EvaluateStringArrayInput(Sink, TEXT("string-array-")).Num(), 2);
	if (TArray<FStoryFlowMapEntry>* LootAgain = Evaluator.EvaluateMapInput(Sink, TEXT("1")))
	{
		// The map resolver hands back a pointer by signature; for a `.sfd` source it must point
		// at a DETACHED snapshot, never into the store.
		LootAgain->Empty();
	}
	if (const TArray<FStoryFlowMapEntry>* LootThird = Evaluator.EvaluateMapInput(Sink, TEXT("1")))
	{
		TestEqual(TEXT("emptying a read map leaves the seed alone"), LootThird->Num(), 2);
	}

	CleanUp();
	return true;
}

// ============================================================================
// The Set node end to end: overlay write, cascade, and the value refusal
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSetNodeTest,
	"StoryFlow.DataAssets.Nodes.SetWritesCascadeAndRefuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetSetNodeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture world initialized"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// start -> setBase(hp = 7) -> setUnwired(hp, value pin unwired) -> end
	// Both Sets aim at the BASE's hp. The second one must refuse: if it wrote its type zero
	// instead, the cascade assertion below would read 0 rather than 7.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

	FStoryFlowVariable IntVar;
	IntVar.Id = TEXT("n");
	IntVar.Name = TEXT("n");
	IntVar.Type = EStoryFlowVariableType::Integer;
	IntVar.Value.SetInt(7);
	Script->Variables.Add(IntVar.Id, IntVar);

	FStoryFlowNode Source = MakeNode(TEXT("V"), EStoryFlowNodeType::GetInt, TEXT("getInt"));
	Source.Data.Variable = TEXT("n");
	Script->Nodes.Add(Source.Id, Source);

	Script->Nodes.Add(TEXT("pB"), MakePill(TEXT("pB"), BaseId));

	for (const TCHAR* SetId : { TEXT("sWrite"), TEXT("sRefuse") })
	{
		FStoryFlowNode Setter = MakeNode(SetId, EStoryFlowNodeType::SetDataAssetVariable, TEXT("setDataAssetVariable"));
		Setter.Data.VariableId = HpId;
		Setter.Data.VariableName = TEXT("hp");
		Setter.Data.Variable = TEXT("hp");
		Setter.Data.VariableType = TEXT("integer");
		Script->Nodes.Add(Setter.Id, Setter);
		Script->Connections.Add(MakePillEdge(TEXT("pB"), Setter.Id));
	}
	// Only the first Set gets a value pin.
	Script->Connections.Add(MakeEdge(TEXT("V"), TEXT("sWrite"),
		StoryFlowHandles::Source(TEXT("V"), TEXT("integer-")),
		StoryFlowHandles::Target(TEXT("sWrite"), TEXT("integer-2"))));

	// --- the ARRAY, MAP and ENUM setters ---
	// The degraded fixture reaches none of these: every one of its array and map cases refuses at
	// the LADDER, so the value-pin handle strings those branches build (In_Map(K,V,"2"),
	// "{type}-array-2"), the map K/V empty guard and the EvaluateMapInput copy were never once
	// executed successfully. A handle string is exactly the kind of thing that is either right or
	// silently refuses forever.
	{
		FStoryFlowVariable ArrayVar;
		ArrayVar.Id = TEXT("srcTags");
		ArrayVar.Name = TEXT("srcTags");
		ArrayVar.Type = EStoryFlowVariableType::String;
		ArrayVar.bIsArray = true;
		TArray<FStoryFlowVariant> Elements;
		Elements.Add(FStoryFlowVariant::FromString(TEXT("alpha")));
		Elements.Add(FStoryFlowVariant::FromString(TEXT("beta")));
		ArrayVar.Value.SetArray(Elements, EStoryFlowVariableType::String);
		Script->Variables.Add(ArrayVar.Id, ArrayVar);

		FStoryFlowVariable MapVar;
		MapVar.Id = TEXT("srcLoot");
		MapVar.Name = TEXT("srcLoot");
		MapVar.Type = EStoryFlowVariableType::Map;
		MapVar.KeyType = EStoryFlowVariableType::String;
		MapVar.ValueType = EStoryFlowVariableType::Integer;
		TArray<FStoryFlowMapEntry> Entries;
		// AUTHORED ORDER, deliberately not alphabetical: contract §2.1 makes map values ordered
		// entry lists, so a write that round-tripped through anything unordered shows up here.
		for (const TPair<FString, int32>& Pair : TArray<TPair<FString, int32>>{ { TEXT("zinc"), 3 }, { TEXT("amber"), 11 } })
		{
			FStoryFlowMapEntry Entry;
			Entry.Key.SetString(Pair.Key);
			Entry.Value.SetInt(Pair.Value);
			Entries.Add(Entry);
		}
		MapVar.Value.SetMap(Entries);
		Script->Variables.Add(MapVar.Id, MapVar);

		FStoryFlowVariable EnumVar;
		EnumVar.Id = TEXT("srcRank");
		EnumVar.Name = TEXT("srcRank");
		EnumVar.Type = EStoryFlowVariableType::Enum;
		EnumVar.Value.SetEnum(TEXT("Champion"));
		Script->Variables.Add(EnumVar.Id, EnumVar);
	}
	{
		FStoryFlowNode ArraySource = MakeNode(TEXT("VArr"), EStoryFlowNodeType::GetStringArray, TEXT("getStringArray"));
		ArraySource.Data.Variable = TEXT("srcTags");
		Script->Nodes.Add(ArraySource.Id, ArraySource);

		FStoryFlowNode MapSource = MakeNode(TEXT("VMap"), EStoryFlowNodeType::GetMap, TEXT("getMap"));
		MapSource.Data.Variable = TEXT("srcLoot");
		MapSource.Data.KeyType = TEXT("string");
		MapSource.Data.ValueType = TEXT("integer");
		Script->Nodes.Add(MapSource.Id, MapSource);

		FStoryFlowNode EnumSource = MakeNode(TEXT("VEnum"), EStoryFlowNodeType::GetEnum, TEXT("getEnum"));
		EnumSource.Data.Variable = TEXT("srcRank");
		Script->Nodes.Add(EnumSource.Id, EnumSource);
	}

	Script->Nodes.Add(TEXT("sTags"), MakeSetter(TEXT("sTags"), TagsId, TEXT("tags"), TEXT("string"), /*bIsArray*/ true));
	Script->Nodes.Add(TEXT("sLoot"), MakeSetter(TEXT("sLoot"), LootId, TEXT("loot"), TEXT("map"), false, TEXT("string"), TEXT("integer")));
	Script->Nodes.Add(TEXT("sRank"), MakeSetter(TEXT("sRank"), RankId, TEXT("rank"), TEXT("enum")));
	for (const TCHAR* SetId : { TEXT("sTags"), TEXT("sLoot"), TEXT("sRank") })
	{
		Script->Connections.Add(MakePillEdge(TEXT("pB"), SetId));
	}
	Script->Connections.Add(MakeEdge(TEXT("VArr"), TEXT("sTags"),
		StoryFlowHandles::Source(TEXT("VArr"), TEXT("string-array-")),
		StoryFlowHandles::Target(TEXT("sTags"), TEXT("string-array-2"))));
	Script->Connections.Add(MakeEdge(TEXT("VMap"), TEXT("sLoot"),
		StoryFlowHandles::Source(TEXT("VMap"), TEXT("map-string-integer")),
		StoryFlowHandles::Target(TEXT("sLoot"), StoryFlowHandles::In_Map(TEXT("string"), TEXT("integer"), TEXT("2")))));
	Script->Connections.Add(MakeEdge(TEXT("VEnum"), TEXT("sRank"),
		StoryFlowHandles::Source(TEXT("VEnum"), TEXT("enum-")),
		StoryFlowHandles::Target(TEXT("sRank"), TEXT("enum-2"))));

	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("sWrite"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("sWrite"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sWrite"), TEXT("sRefuse"),
		StoryFlowHandles::Source(TEXT("sWrite"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("sRefuse"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sRefuse"), TEXT("sTags"),
		StoryFlowHandles::Source(TEXT("sRefuse"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("sTags"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sTags"), TEXT("sLoot"),
		StoryFlowHandles::Source(TEXT("sTags"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("sLoot"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sLoot"), TEXT("sRank"),
		StoryFlowHandles::Source(TEXT("sLoot"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("sRank"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sRank"), TEXT("End"),
		StoryFlowHandles::Source(TEXT("sRank"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("setnodes"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("setnodes"));

	const StoryFlowDataAssets::FSeed& Seed = W.Subsystem->GetDataAssetSeed();
	const StoryFlowDataAssets::FOverlay& Overlay = W.Subsystem->GetDataAssetOverlay();

	// Every write landed at the level the PILL names — the base — and nowhere else.
	TestEqual(TEXT("the writes touched exactly one asset"), Overlay.Num(), 1);
	TestEqual(TEXT("the base's hp resolves to the written value"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, HpId).GetInt(), 7);

	// CASCADE: the child overrides hp in the seed, so it keeps 150; the grandchild inherits the
	// child's override, not the base write. A base write cascading past an override would be the
	// nearest-wins rule broken, and a base write NOT cascading at all would show up as a base
	// that changed alone.
	TestEqual(TEXT("the child's own override still wins over the base write"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, HpId).GetInt(), 150);
	TestEqual(TEXT("the grandchild still inherits the child's override"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, GrandChildId, HpId).GetInt(), 150);

	// The refusal: the second Set aimed at the same slot with NO value pin, and the base's value
	// is still 7 rather than the integer zero an inline-value fallback would have written.
	if (const TMap<FString, FStoryFlowVariant>* BaseEntries = Overlay.Find(BaseId))
	{
		// hp, tags, loot, rank — and NOT a second hp from the refusing setter.
		TestEqual(TEXT("the unwired Set added no extra overlay entry"), BaseEntries->Num(), 4);
	}

	// --- the ARRAY branch of the value read ---
	const FStoryFlowVariant WrittenTags = StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, TagsId);
	if (TestEqual(TEXT("the array Set wrote both wired elements"), WrittenTags.GetArray().Num(), 2))
	{
		TestEqual(TEXT("the array Set kept element order"), WrittenTags.GetArray()[0].GetString(), TEXT("alpha"));
		TestEqual(TEXT("and its second element"), WrittenTags.GetArray()[1].GetString(), TEXT("beta"));
	}
	TestTrue(TEXT("the array Set wrote a String-typed value"), WrittenTags.GetType() == EStoryFlowVariableType::String);
	TestEqual(TEXT("the child's own tags override still shadows the base write"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, TagsId).GetArray()[1].GetString(), TEXT("elite"));

	// --- the MAP branch: whole-value replace, authored key order preserved ---
	const FStoryFlowVariant WrittenLoot = StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, LootId);
	if (TestEqual(TEXT("the map Set REPLACED the whole value"), WrittenLoot.GetMap().Num(), 2))
	{
		TestEqual(TEXT("the map Set kept the authored key order"), WrittenLoot.GetMap()[0].Key.GetString(), TEXT("zinc"));
		TestEqual(TEXT("and did not sort it"), WrittenLoot.GetMap()[1].Key.GetString(), TEXT("amber"));
		TestEqual(TEXT("the map Set kept its integer values"), WrittenLoot.GetMap()[1].Value.GetInt(), 11);
		TestTrue(TEXT("the map values stayed Integer-typed"),
			WrittenLoot.GetMap()[0].Value.GetType() == EStoryFlowVariableType::Integer);
	}
	// The grandchild overrides loot in the seed, so nearest-wins keeps its own entries.
	TestEqual(TEXT("the grandchild's loot override still wins over the base write"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, GrandChildId, LootId).GetMap()[0].Value.GetInt(), 50);

	// --- the ENUM branch: written as Enum, not String ---
	// The seed stores rank as EStoryFlowVariableType::Enum, so an overlay entry typed String
	// would differ from the file value it shadows. Invisible to any read (both answer GetString)
	// and visible in U3's save key, which is why the type is asserted and not just the text.
	const FStoryFlowVariant WrittenRank = StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, RankId);
	TestEqual(TEXT("the enum Set wrote its wired value"), WrittenRank.GetString(), TEXT("Champion"));
	TestTrue(TEXT("the enum Set wrote an Enum-typed value, not a String one"),
		WrittenRank.GetType() == EStoryFlowVariableType::Enum);

	// A base write DOES cascade where nothing shadows it: alive is declared and valued on the
	// base only, so flipping it there must be visible from the grandchild.
	StoryFlowDataAssets::FOverlay& MutableOverlay = W.Subsystem->GetDataAssetOverlay();
	TestTrue(TEXT("the grandchild sees the base's alive before the write"),
		StoryFlowDataAssets::Resolve(Seed, MutableOverlay, GrandChildId, AliveId).GetBool(true));
	TestTrue(TEXT("a base write to an unshadowed variable lands"),
		StoryFlowDataAssets::TrySet(Seed, MutableOverlay, BaseId, AliveId, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("and cascades all the way to the grandchild"),
		StoryFlowDataAssets::Resolve(Seed, MutableOverlay, GrandChildId, AliveId).GetBool(true));

	// --- an EMPTY wired array still writes a typed value ---
	// Its own run, because it overwrites the array asserted above. SetArray infers the element
	// type from element [0], so an empty wired array is the one case with nothing to infer from,
	// and the Set node is the LAST writer before the store — an untyped value here is what U3's
	// save key would serialize.
	UStoryFlowScriptAsset* EmptyScript = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard EmptyGuard(EmptyScript);
	EmptyScript->StartNode = TEXT("0");
	EmptyScript->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	EmptyScript->Nodes.Add(TEXT("pB"), MakePill(TEXT("pB"), BaseId));
	{
		FStoryFlowVariable EmptyVar;
		EmptyVar.Id = TEXT("srcEmpty");
		EmptyVar.Name = TEXT("srcEmpty");
		EmptyVar.Type = EStoryFlowVariableType::String;
		EmptyVar.bIsArray = true;
		EmptyVar.Value.SetArray(TArray<FStoryFlowVariant>(), EStoryFlowVariableType::String);
		EmptyScript->Variables.Add(EmptyVar.Id, EmptyVar);

		FStoryFlowNode EmptySource = MakeNode(TEXT("VArr"), EStoryFlowNodeType::GetStringArray, TEXT("getStringArray"));
		EmptySource.Data.Variable = TEXT("srcEmpty");
		EmptyScript->Nodes.Add(EmptySource.Id, EmptySource);
	}
	EmptyScript->Nodes.Add(TEXT("sTags"), MakeSetter(TEXT("sTags"), TagsId, TEXT("tags"), TEXT("string"), /*bIsArray*/ true));
	EmptyScript->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	EmptyScript->Connections.Add(MakePillEdge(TEXT("pB"), TEXT("sTags")));
	EmptyScript->Connections.Add(MakeEdge(TEXT("VArr"), TEXT("sTags"),
		StoryFlowHandles::Source(TEXT("VArr"), TEXT("string-array-")),
		StoryFlowHandles::Target(TEXT("sTags"), TEXT("string-array-2"))));
	EmptyScript->Connections.Add(MakeEdge(TEXT("0"), TEXT("sTags"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("sTags"), TEXT("0"))));
	EmptyScript->Connections.Add(MakeEdge(TEXT("sTags"), TEXT("End"),
		StoryFlowHandles::Source(TEXT("sTags"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	EmptyScript->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("emptyarray"), EmptyScript);
	W.Component->StartDialogueWithScript(TEXT("emptyarray"));

	const FStoryFlowVariant EmptyWritten = StoryFlowDataAssets::Resolve(Seed, W.Subsystem->GetDataAssetOverlay(), BaseId, TagsId);
	TestEqual(TEXT("the empty wired array was written, not refused"), EmptyWritten.GetArray().Num(), 0);
	TestTrue(TEXT("an empty wired array still writes its declared element type"),
		EmptyWritten.GetType() == EStoryFlowVariableType::String);

	CleanUp();
	return true;
}

// ============================================================================
// Boolean producer: a `.sfd` Get gating a live dialogue option (contract §6.2)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetOptionGatingTest,
	"StoryFlow.DataAssets.Nodes.BooleanGetGatesAnOption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetOptionGatingTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, Seed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// Two conditioned options on one dialogue:
	//   optDirect <- the boolean accessor itself     (ProcessBooleanChain's default arm)
	//   optNot    <- notBool over the same accessor  (a cached producer ABOVE a live read)
	// The second is what proves a Set invalidates the chain: notBool DOES memoize.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	Script->Nodes.Add(TEXT("pB"), MakePill(TEXT("pB"), BaseId));

	FStoryFlowNode Getter = MakeNode(TEXT("g"), EStoryFlowNodeType::GetDataAssetVariable, TEXT("getDataAssetVariable"));
	Getter.Data.VariableId = AliveId;
	Getter.Data.VariableName = TEXT("alive");
	Getter.Data.Variable = TEXT("alive");
	Getter.Data.VariableType = TEXT("boolean");
	Script->Nodes.Add(Getter.Id, Getter);

	Script->Nodes.Add(TEXT("not"), MakeNode(TEXT("not"), EStoryFlowNodeType::NotBool, TEXT("notBool")));

	FStoryFlowNode Dialogue = MakeNode(TEXT("d"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
	Dialogue.Data.Text = TEXT("gated");
	Script->Nodes.Add(Dialogue.Id, Dialogue);

	Script->Connections.Add(MakePillEdge(TEXT("pB"), TEXT("g")));
	Script->Connections.Add(MakeEdge(TEXT("g"), TEXT("d"),
		StoryFlowHandles::Source(TEXT("g"), TEXT("boolean-")), StoryFlowHandles::Target(TEXT("d"), TEXT("boolean-optDirect"))));
	Script->Connections.Add(MakeEdge(TEXT("g"), TEXT("not"),
		StoryFlowHandles::Source(TEXT("g"), TEXT("boolean-")), StoryFlowHandles::Target(TEXT("not"), StoryFlowHandles::In_Boolean)));
	Script->Connections.Add(MakeEdge(TEXT("not"), TEXT("d"),
		StoryFlowHandles::Source(TEXT("not"), TEXT("boolean-")), StoryFlowHandles::Target(TEXT("d"), TEXT("boolean-optNot"))));
	Script->BuildConnectionIndices();

	StoryFlowDataAssets::FOverlay Overlay;
	FStoryFlowExecutionContext Context;
	Context.CurrentScript = Script;
	Context.DataAssetStore = { &Seed, &Overlay };
	FStoryFlowEvaluator Evaluator(&Context);
	FStoryFlowNode* DialogueNode = Context.GetNode(TEXT("d"));

	// THE FAIL-CLOSED REGRESSION. EvaluateBooleanFromNode's default arm returns false, so a
	// missing producer arm does not error — it HIDES this option. Asserting VISIBLE is the only
	// assertion that catches that.
	TestTrue(TEXT("a TRUE .sfd boolean keeps its option visible"),
		Evaluator.EvaluateOptionVisibility(DialogueNode, TEXT("optDirect")));
	TestFalse(TEXT("and its notBool twin hides the other option"),
		Evaluator.EvaluateOptionVisibility(DialogueNode, TEXT("optNot")));

	// A DIRECT pull, the shape a Branch condition or any boolean input takes: EvaluateBooleanInput
	// does NOT run ProcessBooleanChain first, so nothing drops the cache between these two reads.
	// This pair, and only this pair, is what pins the never-memoize rule (contract §5) — a
	// memoizing accessor answers TRUE both times and the session write is invisible until
	// something unrelated happens to clear the cache.
	TestTrue(TEXT("a direct boolean pull reads the seed value"),
		Evaluator.EvaluateBooleanFromNode(Context.GetNode(TEXT("g")), TEXT(""), TEXT("")));
	TestTrue(TEXT("a write through the base lands"),
		StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, AliveId, FStoryFlowVariant::FromBool(false)));
	TestFalse(TEXT("the next direct pull sees the session write with no cache clear in between"),
		Evaluator.EvaluateBooleanFromNode(Context.GetNode(TEXT("g")), TEXT(""), TEXT("")));

	TestFalse(TEXT("the option follows the session write"),
		Evaluator.EvaluateOptionVisibility(DialogueNode, TEXT("optDirect")));

	// notBool DOES memoize, which is why the Set handler drops the evaluation cache — the same
	// clearNotBoolCache the HTML arm performs. ProcessBooleanChain re-primes it from the live
	// read, so this must flip too.
	Evaluator.ClearCache();
	TestTrue(TEXT("the notBool option follows the write once the cache is dropped"),
		Evaluator.EvaluateOptionVisibility(DialogueNode, TEXT("optNot")));

	CleanUp();
	return true;
}

// ============================================================================
// Array ops route into the overlay instead of clobbering a same-named local
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetArrayOpTest,
	"StoryFlow.DataAssets.Nodes.ArrayOpWritesTheOverlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetArrayOpTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture world initialized"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// start -> addToStringArray(<- the child's `tags` accessor) -> end, with a LOCAL script
	// variable also named "tags". The accessor carries no isGlobal and its Data.Variable is that
	// same display name, so an arm that fell through to the name lookup would append to the
	// local array and leave the `.sfd` untouched — silently, and in the one place an author
	// would never think to look — the `.sfd` assertions below read 2 (the untouched override)
	// instead of 3 in that case.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

	FStoryFlowVariable Decoy;
	Decoy.Id = TEXT("tags");
	Decoy.Name = TEXT("tags");
	Decoy.Type = EStoryFlowVariableType::String;
	Decoy.bIsArray = true;
	Decoy.Value.SetArray(TArray<FStoryFlowVariant>());
	Script->Variables.Add(Decoy.Id, Decoy);

	Script->Nodes.Add(TEXT("pC"), MakePill(TEXT("pC"), ChildId));

	FStoryFlowNode Getter = MakeNode(TEXT("g"), EStoryFlowNodeType::GetDataAssetVariable, TEXT("getDataAssetVariable"));
	Getter.Data.VariableId = TagsId;
	Getter.Data.VariableName = TEXT("tags");
	Getter.Data.Variable = TEXT("tags");
	Getter.Data.VariableType = TEXT("string");
	Getter.Data.bIsArray = true;
	Script->Nodes.Add(Getter.Id, Getter);

	FStoryFlowNode Add = MakeNode(TEXT("add"), EStoryFlowNodeType::AddToStringArray, TEXT("addToStringArray"));
	Add.Data.Value.SetString(TEXT("boss"));
	// The op node ALSO names the decoy directly. Today's exporter writes no variable field on an
	// array-op node, so this is a graph the editor does not currently emit — but it is what makes
	// the ORDER observable: the wired `.sfd` accessor has to outrank the node's own name lookup,
	// not merely act as the fallback when that lookup misses.
	Add.Data.Variable = TEXT("tags");
	Script->Nodes.Add(Add.Id, Add);

	// The op node's OUTPUT pin, copied into a GLOBAL so the assertion can see it: a component's
	// execution context (and therefore its locals) is private, globals live on the subsystem.
	// This is the half of the node the store write does not cover — the array op both writes its
	// target AND publishes the result for anything wired downstream, and only the second half
	// went missing when the `.sfd` path dropped the evaluation cache after stamping it.
	FStoryFlowNode Sink = MakeNode(TEXT("sink"), EStoryFlowNodeType::SetStringArray, TEXT("setStringArray"));
	Sink.Data.Variable = TEXT("gSink");
	Sink.Data.bIsGlobal = true;
	Script->Nodes.Add(Sink.Id, Sink);

	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));

	Script->Connections.Add(MakePillEdge(TEXT("pC"), TEXT("g")));
	Script->Connections.Add(MakeEdge(TEXT("g"), TEXT("add"),
		StoryFlowHandles::Source(TEXT("g"), TEXT("string-array-")), StoryFlowHandles::Target(TEXT("add"), StoryFlowHandles::In_StringArray)));
	Script->Connections.Add(MakeEdge(TEXT("add"), TEXT("sink"),
		StoryFlowHandles::Source(TEXT("add"), TEXT("string-array-")), StoryFlowHandles::Target(TEXT("sink"), StoryFlowHandles::In_StringArray)));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("add"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("add"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("add"), TEXT("sink"),
		StoryFlowHandles::Source(TEXT("add"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("sink"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sink"), TEXT("End"),
		StoryFlowHandles::Source(TEXT("sink"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	{
		FStoryFlowVariable SinkVar;
		SinkVar.Id = TEXT("gSink");
		SinkVar.Name = TEXT("gSink");
		SinkVar.Type = EStoryFlowVariableType::String;
		SinkVar.bIsArray = true;
		SinkVar.Value.SetArray(TArray<FStoryFlowVariant>(), EStoryFlowVariableType::String);
		Project->GlobalVariables.Add(SinkVar.Id, SinkVar);
	}

	Project->Scripts.Add(TEXT("arrayop"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("arrayop"));

	const StoryFlowDataAssets::FSeed& Seed = W.Subsystem->GetDataAssetSeed();
	const StoryFlowDataAssets::FOverlay& Overlay = W.Subsystem->GetDataAssetOverlay();

	const FStoryFlowVariant Tags = StoryFlowDataAssets::Resolve(Seed, Overlay, ChildId, TagsId);
	if (TestEqual(TEXT("the append landed on the child's tags"), Tags.GetArray().Num(), 3))
	{
		TestEqual(TEXT("the appended element is the one the node carried"), Tags.GetArray()[2].GetString(), TEXT("boss"));
		TestEqual(TEXT("the override's own entries are still in front of it"), Tags.GetArray()[1].GetString(), TEXT("elite"));
	}
	TestEqual(TEXT("the base's tags are untouched by a child-level append"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, TagsId).GetArray().Num(), 2);

	// The op node's OUTPUT pin still carries the result. The `.sfd` path drops the evaluation
	// cache (a notBool over an arrayLength on this array would otherwise answer stale), and
	// ClearCache is a FULL drop, so doing it after the output stamp erased the stamp and every
	// downstream reader saw an empty array — on the `.sfd` path only, which is what made it easy
	// to miss. The script-variable path is the behavioral reference: it publishes its result here.
	if (const FStoryFlowVariable* Published = W.Subsystem->GetGlobalVariables().Find(TEXT("gSink")))
	{
		if (TestEqual(TEXT("the op node published its result on its output pin"), Published->Value.GetArray().Num(), 3))
		{
			TestEqual(TEXT("and it is the appended array, not some other one"),
				Published->Value.GetArray()[2].GetString(), TEXT("boss"));
		}
	}
	else
	{
		AddError(TEXT("the global sink variable is missing from the subsystem"));
	}


	// --- an op over an ALREADY-EMPTY array still writes a typed value ---
	// A separate run, because a clear would wipe the append above. FStoryFlowVariant::SetArray
	// infers its type from element [0], so an array that is empty WHEN READ is the one case where
	// the type tag has nothing to come from — and a `.sfd` value travels on its own, without an
	// FStoryFlowVariable beside it carrying the declaration. Untyped here means the same variable
	// reads back typed or untyped depending only on whether the last writer left it empty, and
	// the save key U3 writes would inherit that inconsistency.
	UStoryFlowScriptAsset* ClearScript = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ClearGuard(ClearScript);
	ClearScript->StartNode = TEXT("0");
	ClearScript->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	ClearScript->Nodes.Add(TEXT("pC"), MakePill(TEXT("pC"), ChildId));
	ClearScript->Nodes.Add(Getter.Id, Getter);
	ClearScript->Nodes.Add(TEXT("clr"), MakeNode(TEXT("clr"), EStoryFlowNodeType::ClearStringArray, TEXT("clearStringArray")));
	ClearScript->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	ClearScript->Connections.Add(MakePillEdge(TEXT("pC"), TEXT("g")));
	ClearScript->Connections.Add(MakeEdge(TEXT("g"), TEXT("clr"),
		StoryFlowHandles::Source(TEXT("g"), TEXT("string-array-")), StoryFlowHandles::Target(TEXT("clr"), StoryFlowHandles::In_StringArray)));
	ClearScript->Connections.Add(MakeEdge(TEXT("0"), TEXT("clr"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("clr"), TEXT("0"))));
	ClearScript->Connections.Add(MakeEdge(TEXT("clr"), TEXT("End"),
		StoryFlowHandles::Source(TEXT("clr"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	ClearScript->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("arrayclear"), ClearScript);

	// TWICE, and the second run is the one that asserts. The first clear still READS the three
	// appended elements, so its type tag infers correctly no matter what — only a run whose read
	// comes back already empty has nothing to infer from.
	W.Component->StartDialogueWithScript(TEXT("arrayclear"));
	TestEqual(TEXT("the first clear emptied the child's tags"),
		StoryFlowDataAssets::Resolve(Seed, W.Subsystem->GetDataAssetOverlay(), ChildId, TagsId).GetArray().Num(), 0);

	W.Component->StartDialogueWithScript(TEXT("arrayclear"));
	const FStoryFlowVariant Cleared = StoryFlowDataAssets::Resolve(Seed, W.Subsystem->GetDataAssetOverlay(), ChildId, TagsId);
	TestEqual(TEXT("the second clear left it empty"), Cleared.GetArray().Num(), 0);
	TestTrue(TEXT("an op over an already-empty .sfd array still writes its declared element type"),
		Cleared.GetType() == EStoryFlowVariableType::String);

	CleanUp();
	return true;
}

// ============================================================================
// An enum ARRAY written by the Set node lands in the importer's storage shape
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetEnumArrayWriteTest,
	"StoryFlow.DataAssets.Nodes.EnumArraySetWritesEnumElements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetEnumArrayWriteTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture world initialized"), W.Init()))
	{
		return false;
	}

	// An enum array has THREE writers into the store (importer, this node, a save load) and they
	// have to agree on one shape. An enum rides a string pin, so the value arrives through the
	// string array reader and would land String-typed unless this path re-stamps it — while the
	// importer and the load path both produce Enum. Invisible to every reader today (the whole
	// string family answers GetString) and a real divergence the moment one switches on the type.
	// The importer and load ends are pinned in StoryFlow.DataAssets.Resolution.ImportRoundTrip.
	const FString SeedJson = TEXT(R"JSON(
	{ "dataAssets": {
		"da_cc000000000000000000000000000001": {
			"id": "da_cc000000000000000000000000000001",
			"name": "EnumArrayBase",
			"parent": null,
			"variables": [
				{ "id": "v_ranks", "name": "ranks", "type": "enum", "isArray": true,
				  "enumValues": [ "Grunt", "Elite", "Champion" ], "value": [ "Grunt" ] }
			],
			"overrides": {}
		}
	} }
	)JSON");

	const FString EnumBaseId = TEXT("da_cc000000000000000000000000000001");
	StoryFlowDataAssets::FSeed Seed;
	UStoryFlowProjectAsset* Project = ImportInlineSeed(*this, SeedJson, Seed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	Script->Nodes.Add(TEXT("pB"), MakePill(TEXT("pB"), EnumBaseId));
	{
		// Enum arrays have no reader of their own — they ride the string array nodes, which is
		// the whole reason the write path had to be taught to re-stamp.
		FStoryFlowVariable Source;
		Source.Id = TEXT("srcRanks");
		Source.Name = TEXT("srcRanks");
		Source.Type = EStoryFlowVariableType::Enum;
		Source.bIsArray = true;
		TArray<FStoryFlowVariant> Elements;
		FStoryFlowVariant First;
		First.SetEnum(TEXT("Elite"));
		FStoryFlowVariant Second;
		Second.SetEnum(TEXT("Champion"));
		Elements.Add(First);
		Elements.Add(Second);
		Source.Value.SetArray(Elements, EStoryFlowVariableType::Enum);
		Script->Variables.Add(Source.Id, Source);

		FStoryFlowNode Reader = MakeNode(TEXT("VArr"), EStoryFlowNodeType::GetStringArray, TEXT("getStringArray"));
		Reader.Data.Variable = TEXT("srcRanks");
		Script->Nodes.Add(Reader.Id, Reader);
	}
	Script->Nodes.Add(TEXT("sRanks"), MakeSetter(TEXT("sRanks"), TEXT("v_ranks"), TEXT("ranks"), TEXT("enum"), /*bIsArray*/ true));
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakePillEdge(TEXT("pB"), TEXT("sRanks")));
	Script->Connections.Add(MakeEdge(TEXT("VArr"), TEXT("sRanks"),
		StoryFlowHandles::Source(TEXT("VArr"), TEXT("enum-array-")),
		StoryFlowHandles::Target(TEXT("sRanks"), TEXT("enum-array-2"))));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("sRanks"),
		StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("sRanks"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("sRanks"), TEXT("End"),
		StoryFlowHandles::Source(TEXT("sRanks"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("enumarray"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("enumarray"));

	const FStoryFlowVariant Written = StoryFlowDataAssets::Resolve(
		W.Subsystem->GetDataAssetSeed(), W.Subsystem->GetDataAssetOverlay(), EnumBaseId, TEXT("v_ranks"));
	TestTrue(TEXT("the enum array Set wrote an Enum-typed array"), Written.GetType() == EStoryFlowVariableType::Enum);
	if (TestEqual(TEXT("the enum array Set wrote both wired elements"), Written.GetArray().Num(), 2))
	{
		TestTrue(TEXT("and every element is Enum-typed, not String"),
			Written.GetArray()[0].GetType() == EStoryFlowVariableType::Enum);
		TestTrue(TEXT("the second one too"),
			Written.GetArray()[1].GetType() == EStoryFlowVariableType::Enum);
		TestEqual(TEXT("with their values intact"), Written.GetArray()[1].GetString(), TEXT("Champion"));
	}

	CleanUp();
	return true;
}

// ============================================================================
// A BLUEPRINT write invalidates the conditions built on it (contract §5)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetBlueprintWriteInvalidatesTest,
	"StoryFlow.DataAssets.Nodes.BlueprintWriteInvalidatesConditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetBlueprintWriteInvalidatesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetNodeTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture world initialized"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// One dialogue, two options:
	//   optGated  <- andBool( getDataAssetVariable(alive), inline TRUE )
	//   optAlways <- no condition at all, so the count is readable rather than boolean
	//
	// The andBool is the point. The accessor beneath it is never memoized, so a read of it alone
	// would follow a write with no help from anybody — but andBool DOES memoize, and
	// ProcessBooleanChain's andBool arm recurses into its inputs WITHOUT dropping its own cached
	// output. Only an actual cache clear makes the option follow the write.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	Script->Nodes.Add(TEXT("pB"), MakePill(TEXT("pB"), BaseId));

	{
		FStoryFlowNode Getter = MakeNode(TEXT("g"), EStoryFlowNodeType::GetDataAssetVariable, TEXT("getDataAssetVariable"));
		Getter.Data.VariableId = AliveId;
		Getter.Data.VariableName = TEXT("alive");
		Getter.Data.Variable = TEXT("alive");
		Getter.Data.VariableType = TEXT("boolean");
		Script->Nodes.Add(Getter.Id, Getter);
	}
	{
		FStoryFlowNode And = MakeNode(TEXT("and"), EStoryFlowNodeType::AndBool, TEXT("andBool"));
		// The constant half rides the inline fallback the andBool arm already reads for an
		// unwired pin, so this needs no second producer node.
		And.Data.Value2.SetBool(true);
		Script->Nodes.Add(And.Id, And);
	}
	{
		FStoryFlowNode Dialogue = MakeNode(TEXT("d"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		Dialogue.Data.Text = TEXT("gated line");
		FStoryFlowChoice Gated;
		Gated.Id = TEXT("optGated");
		Gated.Text = TEXT("gated");
		FStoryFlowChoice Always;
		Always.Id = TEXT("optAlways");
		Always.Text = TEXT("always");
		Dialogue.Data.Options.Add(Gated);
		Dialogue.Data.Options.Add(Always);
		Script->Nodes.Add(Dialogue.Id, Dialogue);
	}

	Script->Connections.Add(MakePillEdge(TEXT("pB"), TEXT("g")));
	Script->Connections.Add(MakeEdge(TEXT("g"), TEXT("and"),
		StoryFlowHandles::Source(TEXT("g"), TEXT("boolean-")),
		StoryFlowHandles::Target(TEXT("and"), StoryFlowHandles::In_Boolean1)));
	Script->Connections.Add(MakeEdge(TEXT("and"), TEXT("d"),
		StoryFlowHandles::Source(TEXT("and"), TEXT("boolean-")),
		StoryFlowHandles::Target(TEXT("d"), TEXT("boolean-optGated"))));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("d"),
		StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("d"))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("gated"), Script);
	W.Subsystem->SetProject(Project);

	// A global to poke, purely to reach the rebuild. Added before the dialogue starts so the
	// context's name index picks it up.
	{
		FStoryFlowVariable Tick;
		Tick.Id = TEXT("g_tick");
		Tick.Name = TEXT("tick");
		Tick.Type = EStoryFlowVariableType::Boolean;
		Tick.Value.SetBool(false);
		W.Subsystem->GetGlobalVariables().Add(Tick.Id, Tick);
	}

	UStoryFlowDataAssetAsset* Base = Project->DataAssets.FindRef(BaseId);
	if (!TestNotNull(TEXT("the base asset imported"), Base))
	{
		CleanUp();
		return false;
	}

	W.Component->StartDialogueWithScript(TEXT("gated"));
	TestEqual(TEXT("both options are visible while the .sfd boolean is true"),
		W.Component->GetCurrentDialogue().Options.Num(), 2);

	// THE WRITE, from Blueprint rather than from a Set node
	bool bFound = false;
	TestTrue(TEXT("a Blueprint write to the base lands"),
		W.Component->SetDataAssetBoolVariable(Base, TEXT("alive"), false));
	TestFalse(TEXT("and a fresh read through the store sees it"),
		W.Component->GetDataAssetBoolVariable(Base, TEXT("alive"), bFound));
	TestTrue(TEXT("which is a real read, not a miss"), bFound);

	// The rebuild path that does NOT clear the cache on its way in: a variable change re-runs
	// BuildDialogueState in place (NotifyVariableChanged). If the write above left the andBool's
	// memo alone, the rebuild re-asks the same stale producer and the gated option survives.
	W.Component->SetBoolVariable(TEXT("tick"), true, /*bGlobal*/ true);

	const TArray<FStoryFlowDialogueOption>& Rebuilt = W.Component->GetCurrentDialogue().Options;
	if (TestEqual(TEXT("the gated option is gone after a Blueprint write plus a rebuild"), Rebuilt.Num(), 1))
	{
		TestEqual(TEXT("and the surviving option is the unconditioned one"), Rebuilt[0].Id, TEXT("optAlways"));
	}

	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
