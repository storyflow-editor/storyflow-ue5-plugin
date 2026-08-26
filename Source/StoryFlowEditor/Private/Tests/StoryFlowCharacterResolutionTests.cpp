// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "StoryFlowCharacterIndexFixture.h"
#include "StoryFlowRuntime.h"
#include "StoryFlowScopedWorld.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

/**
 * P4 id-first character RESOLUTION: the one ResolveCharacterKey point, the §3 path fall-back,
 * the wired-input override, the warn-once latch, and the one-state property across the char-var
 * node lane and the data-asset Blueprint surface.
 *
 * Builds on the SAME two-character fixture the index import suite uses
 * (StoryFlowCharacterIndexFixture.h), enriched with variables: both characters carry the SAME
 * variable names with DIFFERENT values, so a read or write that resolves the wrong character
 * answers a wrong VALUE rather than a miss. Every id-bound node here deliberately carries the
 * OTHER character's path in its path field — the strongest id-first proof available, because a
 * resolution that ever consults the path field first produces a visibly wrong answer.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.Characters", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.Characters" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowCharacterResolutionTestHelpers
{
	using namespace StoryFlowCharacterIndexTestHelpers;
	using StoryFlowTestWorld::FScopedWorld;

	// The fixture's two characters: ids, record keys, and one deliberately un-normalized
	// spelling of the hero path (proves the path lane still normalizes).
	const TCHAR* HeroId = TEXT("da_hero0001");
	const TCHAR* VillainId = TEXT("da_villain1");
	const TCHAR* HeroKey = TEXT("chars\\hero.sfc");
	const TCHAR* VillainKey = TEXT("chars\\villain.sfc");
	const TCHAR* HeroPathUnnormalized = TEXT("Chars/Hero.sfc");

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

	/** A getCharacterVar node bound by (id, path) — pass empty strings for the unbound halves. */
	FStoryFlowNode MakeCharGetter(const FString& Id, const TCHAR* CharacterId, const TCHAR* CharacterPath,
		const TCHAR* VariableName, const TCHAR* VariableType, bool bIsArray = false)
	{
		FStoryFlowNode N = MakeNode(Id, EStoryFlowNodeType::GetCharacterVar, TEXT("getCharacterVar"));
		N.Data.CharacterId = CharacterId;
		N.Data.CharacterPath = CharacterPath;
		N.Data.VariableName = VariableName;
		N.Data.VariableType = VariableType;
		N.Data.bIsArray = bIsArray;
		return N;
	}

	/** Setter twin of MakeCharGetter. */
	FStoryFlowNode MakeCharSetter(const FString& Id, const TCHAR* CharacterId, const TCHAR* CharacterPath,
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

	/** Import the SHARED fixture with the variables-carrying characters and the standard index. */
	UStoryFlowProjectAsset* ImportVariablesFixture(FAutomationTestBase& Test)
	{
		return ImportFixture(Test, TwoCharacterIndex, TwoCharacterVariablesJson);
	}

	/** Wire a bare context at the subsystem's maps, the way InitializeWithSubsystem would. */
	void WireContext(FStoryFlowExecutionContext& Context, UStoryFlowScriptAsset* Script, UStoryFlowSubsystem* Subsystem)
	{
		Context.CurrentScript = Script;
		Context.ExternalCharacters = &Subsystem->GetRuntimeCharacters();
		Context.CharacterIdToPath = &Subsystem->GetCharacterIdToPath();
	}
}

// ============================================================================
// Speaker: characterRefId wins over a deliberately wrong path field
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterSpeakerIdFirstTest,
	"StoryFlow.Characters.IdResolution.SpeakerIdFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterSpeakerIdFirstTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// start -> D1 (id = hero, path field = VILLAIN, the id-first proof) -> D2 (no id, path =
	// villain — the untouched pre-P4 lane inside the same migrated script) -> end
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	{
		FStoryFlowNode D1 = MakeNode(TEXT("1"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		D1.Data.Text = TEXT("line one");
		D1.Data.Character = VillainKey;
		D1.Data.CharacterRefId = HeroId;
		Script->Nodes.Add(D1.Id, D1);
	}
	{
		FStoryFlowNode D2 = MakeNode(TEXT("2"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		D2.Data.Text = TEXT("line two");
		D2.Data.Character = VillainKey;
		Script->Nodes.Add(D2.Id, D2);
	}
	Script->Nodes.Add(TEXT("3"), MakeNode(TEXT("3"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
	Script->Connections.Add(MakeEdge(TEXT("1"), TEXT("2"), StoryFlowHandles::Source(TEXT("1")), StoryFlowHandles::Target(TEXT("2"))));
	Script->Connections.Add(MakeEdge(TEXT("2"), TEXT("3"), StoryFlowHandles::Source(TEXT("2")), StoryFlowHandles::Target(TEXT("3"))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("speakertest"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("speakertest"));

	// The id resolved the speaker; the wrong path field was never consulted.
	TestEqual(TEXT("id-bound speaker resolves the id's character, not the path field's"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Hero")));

	W.Component->AdvanceDialogue();
	TestEqual(TEXT("a path-only dialogue still resolves by path in the same run"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Villain")));

	CleanUp();
	return true;
}

// ============================================================================
// Read arms: bool / string / array / map, each id-bound with a wrong path field
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterReadArmsIdFirstTest,
	"StoryFlow.Characters.IdResolution.ReadArms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterReadArmsIdFirstTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	// Every getter is bound to the HERO id with the VILLAIN path in its path field; hero and
	// villain disagree on every value, so a path-first resolution answers wrong on all four arms.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	Script->Nodes.Add(TEXT("GB"), MakeCharGetter(TEXT("GB"), HeroId, VillainKey, TEXT("IsBrave"), TEXT("boolean")));
	Script->Nodes.Add(TEXT("GS"), MakeCharGetter(TEXT("GS"), HeroId, VillainKey, TEXT("Title"), TEXT("string")));
	Script->Nodes.Add(TEXT("GA"), MakeCharGetter(TEXT("GA"), HeroId, VillainKey, TEXT("Inventory"), TEXT("string"), /*bIsArray*/ true));
	{
		FStoryFlowNode GM = MakeCharGetter(TEXT("GM"), HeroId, VillainKey, TEXT("Reputation"), TEXT("map"));
		GM.Data.KeyType = TEXT("string");
		GM.Data.ValueType = TEXT("integer");
		Script->Nodes.Add(GM.Id, GM);
	}
	// Array and map reads pull through a consumer's wired input handle, like the .sfd suite's.
	{
		FStoryFlowNode CA = MakeNode(TEXT("CA"), EStoryFlowNodeType::SetBool, TEXT("setBool"));
		Script->Nodes.Add(CA.Id, CA);
		Script->Connections.Add(MakeEdge(TEXT("GA"), TEXT("CA"),
			StoryFlowHandles::Source(TEXT("GA"), TEXT("string-array-")),
			StoryFlowHandles::Target(TEXT("CA"), TEXT("string-array-"))));
	}
	{
		FStoryFlowNode CM = MakeNode(TEXT("CM"), EStoryFlowNodeType::SetBool, TEXT("setBool"));
		CM.Data.KeyType = TEXT("string");
		CM.Data.ValueType = TEXT("integer");
		Script->Nodes.Add(CM.Id, CM);
		Script->Connections.Add(MakeEdge(TEXT("GM"), TEXT("CM"),
			StoryFlowHandles::Source(TEXT("GM"), TEXT("map-string-integer")),
			StoryFlowHandles::Target(TEXT("CM"), StoryFlowHandles::In_Map(TEXT("string"), TEXT("integer"), TEXT("1")))));
	}
	Script->BuildConnectionIndices();

	FStoryFlowExecutionContext Context;
	WireContext(Context, Script, W.Subsystem);
	FStoryFlowEvaluator Evaluator(&Context);

	TestTrue(TEXT("boolean arm reads the id's character (hero true, villain false)"),
		Evaluator.EvaluateBooleanFromNode(Context.GetNode(TEXT("GB")), TEXT(""), TEXT("")));
	TestEqual(TEXT("string arm reads the id's character"),
		Evaluator.EvaluateStringFromNode(Context.GetNode(TEXT("GS")), TEXT(""), TEXT("")), FString(TEXT("the bold")));

	const TArray<FStoryFlowVariant> HeroInventory = Evaluator.EvaluateStringArrayInput(Context.GetNode(TEXT("CA")), TEXT("string-array-"));
	if (TestEqual(TEXT("array arm reads the id's character (2 items, villain has 1)"), HeroInventory.Num(), 2))
	{
		TestEqual(TEXT("array arm carries the hero's first item"), HeroInventory[0].GetString(), FString(TEXT("sword")));
	}

	const TArray<FStoryFlowMapEntry>* HeroReputation = Evaluator.EvaluateMapInput(Context.GetNode(TEXT("CM")), TEXT("1"));
	if (TestNotNull(TEXT("map arm resolves the id's character map"), HeroReputation)
		&& TestEqual(TEXT("map arm reads the hero's 2 entries (villain has 1)"), HeroReputation->Num(), 2))
	{
		TestEqual(TEXT("map arm entry key is the hero's"), (*HeroReputation)[0].Key.GetString(), FString(TEXT("guards")));
		TestEqual(TEXT("map arm entry value is the hero's"), (*HeroReputation)[0].Value.GetInt(), 3);
	}

	TestEqual(TEXT("healthy id resolution emitted no warnings"), Context.CharacterIdWarningsEmitted, 0);

	CleanUp();
	return true;
}

// ============================================================================
// Write arms: bool / string / array / map land on the id's character, never the path's
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterWriteArmsIdFirstTest,
	"StoryFlow.Characters.IdResolution.WriteArms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterWriteArmsIdFirstTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// Four setters in one exec chain, every one id-bound to the HERO with the VILLAIN in its
	// path field — a write that resolves paths first CORRUPTS the villain, which is exactly
	// what the final villain assertions watch for.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

	{
		FStoryFlowNode S1 = MakeCharSetter(TEXT("S1"), HeroId, VillainKey, TEXT("IsBrave"), TEXT("boolean"));
		S1.Data.Value.SetBool(false);
		Script->Nodes.Add(S1.Id, S1);
	}
	{
		FStoryFlowNode S2 = MakeCharSetter(TEXT("S2"), HeroId, VillainKey, TEXT("Title"), TEXT("string"));
		S2.Data.Value.SetString(TEXT("node wrote"));
		Script->Nodes.Add(S2.Id, S2);
	}
	{
		FStoryFlowNode S3 = MakeCharSetter(TEXT("S3"), HeroId, VillainKey, TEXT("Inventory"), TEXT("string"), /*bIsArray*/ true);
		TArray<FStoryFlowVariant> NewInventory;
		FStoryFlowVariant Torch;
		Torch.SetString(TEXT("torch"));
		NewInventory.Add(Torch);
		S3.Data.Value.SetArray(NewInventory);
		Script->Nodes.Add(S3.Id, S3);
	}
	{
		// The map setter snapshots its WIRED input: a map read off the villain BY PATH — so
		// after the run the hero's map equals the villain's original single entry.
		FStoryFlowNode S4 = MakeCharSetter(TEXT("S4"), HeroId, VillainKey, TEXT("Reputation"), TEXT("map"));
		S4.Data.KeyType = TEXT("string");
		S4.Data.ValueType = TEXT("integer");
		Script->Nodes.Add(S4.Id, S4);

		FStoryFlowNode GVM = MakeCharGetter(TEXT("GVM"), TEXT(""), VillainKey, TEXT("Reputation"), TEXT("map"));
		GVM.Data.KeyType = TEXT("string");
		GVM.Data.ValueType = TEXT("integer");
		Script->Nodes.Add(GVM.Id, GVM);
		Script->Connections.Add(MakeEdge(TEXT("GVM"), TEXT("S4"),
			StoryFlowHandles::Source(TEXT("GVM"), TEXT("map-string-integer")),
			StoryFlowHandles::Target(TEXT("S4"), StoryFlowHandles::In_Map(TEXT("string"), TEXT("integer"), TEXT("input")))));
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));

	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("S1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("S1"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S1"), TEXT("S2"), StoryFlowHandles::Source(TEXT("S1"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("S2"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S2"), TEXT("S3"), StoryFlowHandles::Source(TEXT("S2"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("S3"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S3"), TEXT("S4"), StoryFlowHandles::Source(TEXT("S3"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("S4"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S4"), TEXT("End"), StoryFlowHandles::Source(TEXT("S4"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("charwrites"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("charwrites"));

	const FStoryFlowCharacterDef* Hero = W.Subsystem->GetRuntimeCharacters().Find(HeroKey);
	const FStoryFlowCharacterDef* Villain = W.Subsystem->GetRuntimeCharacters().Find(VillainKey);
	if (!TestNotNull(TEXT("hero record exists"), Hero) || !TestNotNull(TEXT("villain record exists"), Villain))
	{
		CleanUp();
		return false;
	}

	TestFalse(TEXT("bool write landed on the id's character"), Hero->Variables[TEXT("IsBrave")].Value.GetBool());
	TestEqual(TEXT("string write landed on the id's character"), Hero->Variables[TEXT("Title")].Value.GetString(), FString(TEXT("node wrote")));
	TestEqual(TEXT("array write landed on the id's character"), Hero->Variables[TEXT("Inventory")].Value.GetArray().Num(), 1);
	if (TestEqual(TEXT("map write landed on the id's character (snapshot of the wired villain map)"),
		Hero->Variables[TEXT("Reputation")].Value.GetMap().Num(), 1))
	{
		TestEqual(TEXT("map write snapshot value"), Hero->Variables[TEXT("Reputation")].Value.GetMap()[0].Value.GetInt(), -2);
	}

	// The wrong-path character is UNTOUCHED — the mis-resolve corruption the plan warns about.
	TestEqual(TEXT("villain string uncorrupted"), Villain->Variables[TEXT("Title")].Value.GetString(), FString(TEXT("the cruel")));
	TestEqual(TEXT("villain array uncorrupted"), Villain->Variables[TEXT("Inventory")].Value.GetArray().Num(), 1);
	TestEqual(TEXT("villain array item uncorrupted"), Villain->Variables[TEXT("Inventory")].Value.GetArray()[0].GetString(), FString(TEXT("dagger")));
	TestEqual(TEXT("villain map uncorrupted"), Villain->Variables[TEXT("Reputation")].Value.GetMap().Num(), 1);

	CleanUp();
	return true;
}

// ============================================================================
// Array-ELEMENT write: Set Array Element's write-back resolves the char array id-first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterArrayElementWriteTest,
	"StoryFlow.Characters.IdResolution.ArrayElementWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterArrayElementWriteTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// HandleArraySetElement's write-back is a DISTINCT resolution site from the whole-array
	// SetCharacterVar lane: it dispatches on the SOURCE node wired to the array pin and
	// resolves that node's character binding itself. Same decoy pattern as WriteArms — the
	// getter is id-bound to the HERO with the VILLAIN in its path field, so a path-first
	// write-back corrupts the villain's array.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	Script->Nodes.Add(TEXT("GA"), MakeCharGetter(TEXT("GA"), HeroId, VillainKey, TEXT("Inventory"), TEXT("string"), /*bIsArray*/ true));
	{
		FStoryFlowNode SE = MakeNode(TEXT("SE"), EStoryFlowNodeType::SetStringArrayElement, TEXT("setStringArrayElement"));
		SE.Data.Value1.SetInt(1);                  // index (export dialect: value1)
		SE.Data.Value2.SetString(TEXT("lantern")); // element (export dialect: value2)
		Script->Nodes.Add(SE.Id, SE);
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));

	Script->Connections.Add(MakeEdge(TEXT("GA"), TEXT("SE"),
		StoryFlowHandles::Source(TEXT("GA"), TEXT("string-array-")),
		StoryFlowHandles::Target(TEXT("SE"), TEXT("string-array-2"))));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("SE"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("SE"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("SE"), TEXT("End"), StoryFlowHandles::Source(TEXT("SE"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("charelem"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("charelem"));

	const FStoryFlowCharacterDef* Hero = W.Subsystem->GetRuntimeCharacters().Find(HeroKey);
	const FStoryFlowCharacterDef* Villain = W.Subsystem->GetRuntimeCharacters().Find(VillainKey);
	if (!TestNotNull(TEXT("hero record exists"), Hero) || !TestNotNull(TEXT("villain record exists"), Villain))
	{
		CleanUp();
		return false;
	}

	const TArray<FStoryFlowVariant>& HeroInventory = Hero->Variables[TEXT("Inventory")].Value.GetArray();
	if (TestEqual(TEXT("element write kept the hero's array length"), HeroInventory.Num(), 2))
	{
		TestEqual(TEXT("element write left index 0 alone"), HeroInventory[0].GetString(), FString(TEXT("sword")));
		TestEqual(TEXT("element write landed on the id's character at index 1"), HeroInventory[1].GetString(), FString(TEXT("lantern")));
	}

	// The decoy-path character is untouched — the write-back resolved id-first.
	const TArray<FStoryFlowVariant>& VillainInventory = Villain->Variables[TEXT("Inventory")].Value.GetArray();
	if (TestEqual(TEXT("villain array uncorrupted"), VillainInventory.Num(), 1))
	{
		TestEqual(TEXT("villain array item uncorrupted"), VillainInventory[0].GetString(), FString(TEXT("dagger")));
	}

	CleanUp();
	return true;
}

// ============================================================================
// A wired character input still overrides the embedded id — and a wired ID resolves
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterWiredOverrideTest,
	"StoryFlow.Characters.IdResolution.WiredOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterWiredOverrideTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

	// Two script string variables feed the character input pins: one carries the villain's
	// PATH, the other the villain's ID — the wire must win over the embedded hero id either way.
	{
		FStoryFlowVariable PathVar;
		PathVar.Id = TEXT("pv");
		PathVar.Name = TEXT("TargetPath");
		PathVar.Type = EStoryFlowVariableType::String;
		PathVar.Value.SetString(VillainKey);
		Script->Variables.Add(PathVar.Id, PathVar);

		FStoryFlowVariable IdVar;
		IdVar.Id = TEXT("iv");
		IdVar.Name = TEXT("TargetId");
		IdVar.Type = EStoryFlowVariableType::String;
		IdVar.Value.SetString(VillainId);
		Script->Variables.Add(IdVar.Id, IdVar);
	}
	{
		FStoryFlowNode PathSource = MakeNode(TEXT("VP"), EStoryFlowNodeType::GetString, TEXT("getString"));
		PathSource.Data.Variable = TEXT("pv");
		Script->Nodes.Add(PathSource.Id, PathSource);

		FStoryFlowNode IdSource = MakeNode(TEXT("VI"), EStoryFlowNodeType::GetString, TEXT("getString"));
		IdSource.Data.Variable = TEXT("iv");
		Script->Nodes.Add(IdSource.Id, IdSource);
	}
	{
		// Embedded binding says HERO (id AND path); the wire says villain path. Wire wins.
		FStoryFlowNode S1 = MakeCharSetter(TEXT("S1"), HeroId, HeroKey, TEXT("Title"), TEXT("string"));
		S1.Data.Value.SetString(TEXT("wired path write"));
		Script->Nodes.Add(S1.Id, S1);
		Script->Connections.Add(MakeEdge(TEXT("VP"), TEXT("S1"),
			StoryFlowHandles::Source(TEXT("VP"), TEXT("string-")),
			StoryFlowHandles::Target(TEXT("S1"), StoryFlowHandles::In_CharacterInput)));
	}
	{
		// Embedded binding says HERO; the wire carries the villain's ID. The wired string
		// routes through ResolveCharacterKey inside the accessors, so an id wire resolves too.
		FStoryFlowNode S2 = MakeCharSetter(TEXT("S2"), HeroId, HeroKey, TEXT("Coins"), TEXT("integer"));
		S2.Data.Value.SetInt(42);
		Script->Nodes.Add(S2.Id, S2);
		Script->Connections.Add(MakeEdge(TEXT("VI"), TEXT("S2"),
			StoryFlowHandles::Source(TEXT("VI"), TEXT("string-")),
			StoryFlowHandles::Target(TEXT("S2"), StoryFlowHandles::In_CharacterInput)));
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));

	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("S1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("S1"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S1"), TEXT("S2"), StoryFlowHandles::Source(TEXT("S1"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("S2"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S2"), TEXT("End"), StoryFlowHandles::Source(TEXT("S2"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("wiredoverride"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("wiredoverride"));

	const FStoryFlowCharacterDef* Hero = W.Subsystem->GetRuntimeCharacters().Find(HeroKey);
	const FStoryFlowCharacterDef* Villain = W.Subsystem->GetRuntimeCharacters().Find(VillainKey);
	if (!TestNotNull(TEXT("hero record exists"), Hero) || !TestNotNull(TEXT("villain record exists"), Villain))
	{
		CleanUp();
		return false;
	}

	TestEqual(TEXT("wired PATH overrode the embedded id (write landed on the villain)"),
		Villain->Variables[TEXT("Title")].Value.GetString(), FString(TEXT("wired path write")));
	TestEqual(TEXT("hero title untouched by the wired-path write"),
		Hero->Variables[TEXT("Title")].Value.GetString(), FString(TEXT("the bold")));
	TestEqual(TEXT("wired ID overrode the embedded id and resolved through the bridge"),
		Villain->Variables[TEXT("Coins")].Value.GetInt(), 42);
	TestEqual(TEXT("hero coins untouched by the wired-id write"),
		Hero->Variables[TEXT("Coins")].Value.GetInt(), 7);

	CleanUp();
	return true;
}

// ============================================================================
// Pre-P4 fixture: no index, no id fields — the pure path world, unchanged
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterPreP4PathLaneTest,
	"StoryFlow.Characters.IdResolution.PreP4PathLane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterPreP4PathLaneTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	// NO character-index.json: the pre-P4 export shape. Nodes carry only paths.
	UStoryFlowProjectAsset* Project = ImportFixture(*this, nullptr, TwoCharacterVariablesJson);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	TestEqual(TEXT("pre-P4 import leaves the bridge empty"), Project->CharacterIdToPath.Num(), 0);

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	{
		FStoryFlowNode D1 = MakeNode(TEXT("1"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		D1.Data.Text = TEXT("line");
		D1.Data.Character = HeroPathUnnormalized; // un-normalized spelling still normalizes
		Script->Nodes.Add(D1.Id, D1);
	}
	{
		FStoryFlowNode S1 = MakeCharSetter(TEXT("S1"), TEXT(""), VillainKey, TEXT("Title"), TEXT("string"));
		S1.Data.Value.SetString(TEXT("path wrote"));
		Script->Nodes.Add(S1.Id, S1);
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Nodes.Add(TEXT("GB"), MakeCharGetter(TEXT("GB"), TEXT(""), HeroKey, TEXT("IsBrave"), TEXT("boolean")));

	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
	Script->Connections.Add(MakeEdge(TEXT("1"), TEXT("S1"), StoryFlowHandles::Source(TEXT("1")), StoryFlowHandles::Target(TEXT("S1"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S1"), TEXT("End"), StoryFlowHandles::Source(TEXT("S1"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("prep4"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("prep4"));

	TestEqual(TEXT("speaker resolves by path exactly as V2"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Hero")));

	W.Component->AdvanceDialogue(); // runs the setter to the end

	const FStoryFlowCharacterDef* Villain = W.Subsystem->GetRuntimeCharacters().Find(VillainKey);
	if (TestNotNull(TEXT("villain record exists"), Villain))
	{
		TestEqual(TEXT("path-bound write lands exactly as V2"),
			Villain->Variables[TEXT("Title")].Value.GetString(), FString(TEXT("path wrote")));
	}

	// Path-only read through a bare context: no bridge, no warns, same answer as V2.
	FStoryFlowExecutionContext Context;
	WireContext(Context, Script, W.Subsystem);
	FStoryFlowEvaluator Evaluator(&Context);
	TestTrue(TEXT("path-bound read answers exactly as V2"),
		Evaluator.EvaluateBooleanFromNode(Context.GetNode(TEXT("GB")), TEXT(""), TEXT("")));
	TestEqual(TEXT("the pure path world emits no character id warnings"), Context.CharacterIdWarningsEmitted, 0);
	// ...and the COMPONENT-driven run above (speaker + setter) warned nothing either — the
	// bare context beside it cannot see that run's latch.
	TestEqual(TEXT("the component's run emitted no character id warnings"), W.Component->GetCharacterIdWarningsEmitted(), 0);

	CleanUp();
	return true;
}

// ============================================================================
// Dangling id: ONE warn across repeated lines, path fall-back, Reset re-arms
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterDanglingIdTest,
	"StoryFlow.Characters.IdResolution.DanglingIdWarnsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterDanglingIdTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// TWO dialogue lines bound to the same dangling id, then a full restart:
	//  - within one run the latch allows exactly ONE warn across both lines;
	//  - the restart Reset()s the context, re-arming the latch for ONE more.
	AddExpectedError(TEXT("is not in this project's character index"), EAutomationExpectedErrorFlags::Contains, 2);

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	for (int32 LineIndex = 1; LineIndex <= 2; ++LineIndex)
	{
		FStoryFlowNode D = MakeNode(FString::FromInt(LineIndex), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		D.Data.Text = TEXT("line");
		D.Data.Character = HeroKey;              // valid path field
		D.Data.CharacterRefId = TEXT("da_ghost9999"); // dangling id
		Script->Nodes.Add(D.Id, D);
	}
	Script->Nodes.Add(TEXT("3"), MakeNode(TEXT("3"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
	Script->Connections.Add(MakeEdge(TEXT("1"), TEXT("2"), StoryFlowHandles::Source(TEXT("1")), StoryFlowHandles::Target(TEXT("2"))));
	Script->Connections.Add(MakeEdge(TEXT("2"), TEXT("3"), StoryFlowHandles::Source(TEXT("2")), StoryFlowHandles::Target(TEXT("3"))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("dangling"), Script);
	W.Subsystem->SetProject(Project);

	W.Component->StartDialogueWithScript(TEXT("dangling"));
	TestEqual(TEXT("dangling id falls back to the path field (line 1)"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Hero")));
	W.Component->AdvanceDialogue();
	TestEqual(TEXT("dangling id falls back to the path field (line 2, warn latched)"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Hero")));

	// Restart: Reset() re-arms the latch — the expected-error count of 2 above pins BOTH
	// properties at once (once per run, re-armed across runs).
	W.Component->StartDialogueWithScript(TEXT("dangling"));
	TestEqual(TEXT("dangling id still falls back after the restart"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Hero")));

	CleanUp();
	return true;
}

// ============================================================================
// Bridge hit whose record is missing from RuntimeCharacters = miss of the whole resolution
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterBridgeHitRecordMissingTest,
	"StoryFlow.Characters.IdResolution.BridgeHitRecordMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterBridgeHitRecordMissingTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	// Simulate the post-LoadFromSlot shape: the bridge still knows the hero (project-derived),
	// but the runtime store no longer holds the record (a loaded save carries only what it saved).
	W.Subsystem->GetRuntimeCharacters().Remove(HeroKey);
	TestNotNull(TEXT("the bridge still maps the hero id"), W.Subsystem->GetCharacterIdToPath().Find(HeroId));

	AddExpectedError(TEXT("is not among the loaded runtime characters"), EAutomationExpectedErrorFlags::Contains, 1);

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	Script->Nodes.Add(TEXT("GS"), MakeCharGetter(TEXT("GS"), HeroId, VillainKey, TEXT("Title"), TEXT("string")));
	Script->BuildConnectionIndices();

	FStoryFlowExecutionContext Context;
	WireContext(Context, Script, W.Subsystem);
	FStoryFlowEvaluator Evaluator(&Context);

	// The bridge HIT does not count as a resolution: the path field answers instead. TWICE, so
	// the second read proves the "unloaded" warn latched.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		TestEqual(FString::Printf(TEXT("bridge-hit-record-missing falls through to the path field (pass %d)"), Pass),
			Evaluator.EvaluateStringFromNode(Context.GetNode(TEXT("GS")), TEXT(""), TEXT("")), FString(TEXT("the cruel")));
	}
	TestEqual(TEXT("the unloaded warn was emitted exactly once"), Context.CharacterIdWarningsEmitted, 1);

	// Reset is the only re-arm point.
	Context.Reset();
	TestEqual(TEXT("Reset clears the warn latch"), Context.WarnedCharacterIds.Num(), 0);
	TestEqual(TEXT("Reset clears the emitted counter"), Context.CharacterIdWarningsEmitted, 0);

	CleanUp();
	return true;
}

// ============================================================================
// Id and path reach the IDENTICAL runtime record — no normalization creep
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterIdPathEqualityTest,
	"StoryFlow.Characters.IdResolution.IdAndPathReachOneDef",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterIdPathEqualityTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	FStoryFlowExecutionContext Context;
	WireContext(Context, /*Script*/ nullptr, W.Subsystem);

	FStoryFlowCharacterDef* ById = Context.FindCharacter(HeroId);
	FStoryFlowCharacterDef* ByPath = Context.FindCharacter(HeroPathUnnormalized);
	FStoryFlowCharacterDef* ByKey = Context.FindCharacter(HeroKey);

	if (TestNotNull(TEXT("id resolves"), ById) && TestNotNull(TEXT("un-normalized path resolves"), ByPath) && TestNotNull(TEXT("record key resolves"), ByKey))
	{
		// POINTER equality: the cheapest possible guard against a re-normalization (or a copy)
		// creeping into either lane — both must hand out the same mutable runtime record.
		TestTrue(TEXT("id and path reach the identical FStoryFlowCharacterDef"), ById == ByPath);
		TestTrue(TEXT("record key reaches the identical FStoryFlowCharacterDef"), ById == ByKey);
	}
	TestEqual(TEXT("healthy lookups warned nothing"), Context.CharacterIdWarningsEmitted, 0);

	CleanUp();
	return true;
}

// ============================================================================
// One state: the char-var lane and the data-asset Blueprint surface share the character
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterOneStateTest,
	"StoryFlow.Characters.OneState.DataAssetSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterOneStateTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// A char-var node writes Title on the hero, id-bound.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	{
		FStoryFlowNode S1 = MakeCharSetter(TEXT("S1"), HeroId, HeroKey, TEXT("Title"), TEXT("string"));
		S1.Data.Value.SetString(TEXT("node wrote"));
		Script->Nodes.Add(S1.Id, S1);
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("S1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("S1"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S1"), TEXT("End"), StoryFlowHandles::Source(TEXT("S1"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();
	Project->Scripts.Add(TEXT("onestate"), Script);
	W.Subsystem->SetProject(Project);

	// Contract §1 disjointness pin: a character FILE id never names a data-asset seed entry.
	// The Blueprint branch's bridge-before-seed precedence RELIES on this — an exporter that
	// let characters into data-assets.json would silently flip which branch answers.
	TestFalse(TEXT("the character id is absent from the data-asset seed"),
		StoryFlowDataAssets::HasAsset(W.Subsystem->GetDataAssetSeed(), HeroId));

	// The Blueprint surface reaches characters through a data-asset handle stamped with the
	// character FILE id — the contract §3 bridge route.
	UStoryFlowDataAssetAsset* HeroHandle = NewObject<UStoryFlowDataAssetAsset>(GetTransientPackage());
	FGCObjectScopeGuard HandleGuard(HeroHandle);
	HeroHandle->AssetId = HeroId;

	bool bFound = false;

	// Pre-write: the surface reads the character's authored value.
	TestEqual(TEXT("DA surface reads the character's authored value"),
		W.Component->GetDataAssetStringVariable(HeroHandle, TEXT("Title"), bFound), FString(TEXT("the bold")));
	TestTrue(TEXT("DA surface reports found"), bFound);

	// Write via the char-var NODE, read via the DA surface: one state.
	W.Component->StartDialogueWithScript(TEXT("onestate"));
	TestEqual(TEXT("a char-var node write is visible on the DA surface"),
		W.Component->GetDataAssetStringVariable(HeroHandle, TEXT("Title"), bFound), FString(TEXT("node wrote")));
	TestTrue(TEXT("post-write DA read reports found"), bFound);

	// The REVERSE: write via the DA surface, read via the char-var evaluator arm by id.
	TestTrue(TEXT("DA surface write on a character reports written"),
		W.Component->SetDataAssetIntVariable(HeroHandle, TEXT("Coins"), 99));
	{
		UStoryFlowScriptAsset* ReadScript = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
		FGCObjectScopeGuard ReadScriptGuard(ReadScript);
		ReadScript->StartNode = TEXT("0");
		ReadScript->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
		ReadScript->Nodes.Add(TEXT("GI"), MakeCharGetter(TEXT("GI"), HeroId, VillainKey, TEXT("Coins"), TEXT("integer")));
		ReadScript->BuildConnectionIndices();

		FStoryFlowExecutionContext Context;
		WireContext(Context, ReadScript, W.Subsystem);
		FStoryFlowEvaluator Evaluator(&Context);
		TestEqual(TEXT("a DA surface write is visible to the char-var arm"),
			Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("GI")), TEXT(""), TEXT("")), 99);
	}

	// ONE state means ONE store: nothing about a character ever lands in the .sfd overlay.
	TestEqual(TEXT("the data-asset overlay holds no character entries"),
		W.Subsystem->GetDataAssetOverlay().Num(), 0);

	// The variant getter serves arrays and maps through the same bridge, storage detached.
	FStoryFlowVariant Inventory = W.Component->GetDataAssetVariantVariable(HeroHandle, TEXT("Inventory"), bFound);
	TestTrue(TEXT("variant getter reaches a character array"), bFound);
	TestEqual(TEXT("variant getter array size"), Inventory.GetArray().Num(), 2);
	FStoryFlowVariant Reputation = W.Component->GetDataAssetVariantVariable(HeroHandle, TEXT("Reputation"), bFound);
	TestTrue(TEXT("variant getter reaches a character map"), bFound);
	TestEqual(TEXT("variant getter map size"), Reputation.GetMap().Num(), 2);

	// cf_name aliases the Name builtin on this surface too (amendment A1) — string accessor
	// only, the no-coercion rule holds.
	TestEqual(TEXT("cf_name answers the Name builtin through the DA surface"),
		W.Component->GetDataAssetStringVariable(HeroHandle, TEXT("cf_name"), bFound), FString(TEXT("Hero")));
	TestTrue(TEXT("cf_name reports found"), bFound);
	W.Component->GetDataAssetBoolVariable(HeroHandle, TEXT("cf_name"), bFound);
	TestFalse(TEXT("cf_name through the bool accessor reports not-found (no coercion)"), bFound);

	// An id that is neither seed nor bridge keeps today's not-found posture.
	UStoryFlowDataAssetAsset* UnknownHandle = NewObject<UStoryFlowDataAssetAsset>(GetTransientPackage());
	FGCObjectScopeGuard UnknownGuard(UnknownHandle);
	UnknownHandle->AssetId = TEXT("da_unknown99");
	W.Component->GetDataAssetStringVariable(UnknownHandle, TEXT("Title"), bFound);
	TestFalse(TEXT("an unknown id misses exactly as before"), bFound);

	CleanUp();
	return true;
}

// ============================================================================
// cf_name / cf_image alias the builtins in the context accessors (amendment A1)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterCfBuiltinAliasTest,
	"StoryFlow.Characters.CfBuiltinAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterCfBuiltinAliasTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterResolutionTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportVariablesFixture(*this);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	FStoryFlowExecutionContext Context;
	WireContext(Context, /*Script*/ nullptr, W.Subsystem);

	// cf_ spellings READ the builtins...
	TestEqual(TEXT("cf_name reads the Name builtin"),
		Context.GetCharacterVariableValue(HeroKey, TEXT("cf_name")).GetString(), FString(TEXT("Hero")));
	TestEqual(TEXT("cf_image reads the Image builtin"),
		Context.GetCharacterVariableValue(HeroKey, TEXT("cf_image")).GetString(), FString());

	// ...and WRITE them, cross-visible under the display spelling (one row, two names).
	FStoryFlowVariant NewName;
	NewName.SetString(TEXT("Renamed"));
	Context.SetCharacterVariable(HeroKey, TEXT("cf_name"), NewName);
	TestEqual(TEXT("a cf_name write lands on the Name builtin"),
		Context.GetCharacterVariableValue(HeroKey, TEXT("Name")).GetString(), FString(TEXT("Renamed")));

	FStoryFlowVariant NewImage;
	NewImage.SetString(TEXT("portrait.png"));
	Context.SetCharacterVariable(HeroKey, TEXT("cf_image"), NewImage);
	TestEqual(TEXT("a cf_image write lands on the Image builtin"),
		Context.GetCharacterVariableValue(HeroKey, TEXT("Image")).GetString(), FString(TEXT("portrait.png")));

	// The id lane composes with the alias: cf_name through the hero's FILE id.
	TestEqual(TEXT("cf_name through the character id"),
		Context.GetCharacterVariableValue(HeroId, TEXT("cf_name")).GetString(), FString(TEXT("Renamed")));

	// Amendment A2a: the SAME aliases hold on the public Blueprint lanes — the forbidden shape
	// is a lane where cf_name silently no-ops while another lane writes Name. Both directions:
	// a cf_ write read back under the display spelling, and a display write read back as cf_.
	FStoryFlowVariant PublicName;
	PublicName.SetString(TEXT("Public Renamed"));
	W.Component->SetCharacterVariable(HeroKey, TEXT("cf_name"), PublicName);
	TestEqual(TEXT("public SetCharacterVariable accepts cf_name"),
		W.Component->GetCharacterVariable(HeroKey, TEXT("Name")).GetString(), FString(TEXT("Public Renamed")));
	TestEqual(TEXT("public GetCharacterVariable accepts cf_image"),
		W.Component->GetCharacterVariable(HeroKey, TEXT("cf_image")).GetString(), FString(TEXT("portrait.png")));

	// The typed asset-picker lane too (A2a says EVERY name-accepting lane).
	UStoryFlowCharacterAsset* const* HeroAsset = Project->Characters.Find(HeroKey);
	if (TestNotNull(TEXT("the hero character asset exists"), HeroAsset ? *HeroAsset : nullptr))
	{
		W.Component->SetCharacterStringVariable(*HeroAsset, TEXT("cf_image"), TEXT("typed.png"));
		TestEqual(TEXT("typed setter accepts cf_image"),
			W.Component->GetCharacterStringVariable(*HeroAsset, TEXT("Image")), FString(TEXT("typed.png")));
		TestEqual(TEXT("typed getter accepts cf_name"),
			W.Component->GetCharacterStringVariable(*HeroAsset, TEXT("cf_name")), FString(TEXT("Public Renamed")));
	}

	// Interpolation's builtin branch honors the cf_ spellings too (name-or-cf ONLY: the
	// custom-variable row's id tolerance is a recorded asymmetry and stays off the builtins).
	{
		FStoryFlowVariable CharTyped;
		CharTyped.Id = TEXT("cv");
		CharTyped.Name = TEXT("protagonist");
		CharTyped.Type = EStoryFlowVariableType::Character;
		CharTyped.Value.SetString(HeroKey);
		Context.LocalVariables.Add(CharTyped.Id, CharTyped);
		TestEqual(TEXT("interpolation resolves {var.cf_name} to the Name builtin"),
			Context.InterpolateVariables(TEXT("{protagonist.cf_name}")), FString(TEXT("Public Renamed")));
		TestEqual(TEXT("interpolation resolves {var.cf_image} to the Image builtin"),
			Context.InterpolateVariables(TEXT("{protagonist.cf_image}")), FString(TEXT("typed.png")));
	}

	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
