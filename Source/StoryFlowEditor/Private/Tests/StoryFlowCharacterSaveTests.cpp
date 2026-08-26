// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StoryFlowCharacterIndexFixture.h"
#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowScopedWorld.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

/**
 * P4 character SAVE pins (contract §3 save ownership + §5): character state rides the `characters`
 * save section in the exact V2 shape — path-keyed, typed records, wholesale-replace load — no
 * matter how the writes were bound, and the V2 `dataAssets` machinery never captures a character.
 * Everything here walks the REAL SaveToSlot / LoadFromSlot pair, not the serializer alone, because
 * the §3 promise is about what a shipped game's slot actually holds.
 *
 * Third caller of the shared two-character fixture (StoryFlowCharacterIndexFixture.h); the
 * divergence test grows it by one character through the header's shared splice helpers rather
 * than forking it.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.Characters.Save", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.Characters.Save" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowCharacterSaveTestHelpers
{
	using namespace StoryFlowCharacterIndexTestHelpers;
	using StoryFlowEngineContract::JsonEquals;
	using StoryFlowTestWorld::FScopedWorld;

	// The fixture's ids, record keys, the newcomer growth (ThreeCharacterIndexJson /
	// ThreeCharacterVariablesJsonString) and the script builders all come from the shared
	// fixture header.

	/**
	 * The EXPECTED `characters` save section after the id-bound writes in the shape test: the
	 * exact V2 shape — record-key keyed (never ids), name-keyed variable tables, TYPED records
	 * (id / name / type / isArray / value), map values as ordered entry lists. Hero carries the
	 * two writes (Title, Coins); the villain — every setter's decoy path field — is byte-level
	 * untouched fixture state. A save that keyed a record by its character id, wrote bare
	 * values, or leaked a write to the decoy fails the structural compare.
	 */
	const TCHAR* ExpectedCharactersSection = TEXT(R"JSON({
		"chars\\hero.sfc":{"name":"Hero","image":"","variables":{
			"IsBrave":{"id":"var_b1","name":"IsBrave","type":"Boolean","isArray":false,"value":true},
			"Title":{"id":"var_s1","name":"Title","type":"String","isArray":false,"value":"id wrote"},
			"Coins":{"id":"var_i1","name":"Coins","type":"Integer","isArray":false,"value":21},
			"Inventory":{"id":"var_a1","name":"Inventory","type":"String","isArray":true,"value":["sword","shield"]},
			"Reputation":{"id":"var_m1","name":"Reputation","type":"Map","isArray":false,"keyType":"String","valueType":"Integer","value":[{"key":"guards","value":3},{"key":"thieves","value":5}]}}},
		"chars\\villain.sfc":{"name":"Villain","image":"","variables":{
			"IsBrave":{"id":"var_b1","name":"IsBrave","type":"Boolean","isArray":false,"value":false},
			"Title":{"id":"var_s1","name":"Title","type":"String","isArray":false,"value":"the cruel"},
			"Coins":{"id":"var_i1","name":"Coins","type":"Integer","isArray":false,"value":1},
			"Inventory":{"id":"var_a1","name":"Inventory","type":"String","isArray":true,"value":["dagger"]},
			"Reputation":{"id":"var_m1","name":"Reputation","type":"Map","isArray":false,"keyType":"String","valueType":"Integer","value":[{"key":"guards","value":-2}]}}}})JSON");

	/** The save slot every test here writes and deletes. */
	const TCHAR* SlotName = TEXT("StoryFlowCharacterSlotTest");

	TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** The slot's raw save document, through the REAL slot storage. Null when unreadable. */
	TSharedPtr<FJsonObject> ReadSlotJson(FAutomationTestBase& Test)
	{
		UStoryFlowSaveGame* Slot = Cast<UStoryFlowSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
		if (!Test.TestNotNull(TEXT("the slot reads back as a StoryFlow save"), Slot))
		{
			return nullptr;
		}
		return ParseJsonObject(Slot->SaveDataJson);
	}

	/** Delete the slot and the fixture assets — every test's exit path. */
	void CleanUpWithSlot()
	{
		UGameplayStatics::DeleteGameInSlot(SlotName, 0);
		CleanUp();
	}
}

// ============================================================================
// Id-bound writes land in the `characters` section, exact V2 shape — and the
// save -> load -> save cycle is stable (§5 double-capture + round-trip pins)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterSaveShapeTest,
	"StoryFlow.Characters.Save.IdBoundWriteShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterSaveShapeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterSaveTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportFixture(*this, TwoCharacterIndex, TwoCharacterVariablesJson);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// Two id-bound setters, each carrying the VILLAIN as its decoy path field — the same
	// id-first proof the resolution suite uses, now carried through to the slot.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	{
		FStoryFlowNode S1 = MakeCharSetter(TEXT("S1"), HeroId, VillainKey, TEXT("Title"), TEXT("string"));
		S1.Data.Value.SetString(TEXT("id wrote"));
		Script->Nodes.Add(S1.Id, S1);
	}
	{
		FStoryFlowNode S2 = MakeCharSetter(TEXT("S2"), HeroId, VillainKey, TEXT("Coins"), TEXT("integer"));
		S2.Data.Value.SetInt(21);
		Script->Nodes.Add(S2.Id, S2);
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("S1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("S1"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S1"), TEXT("S2"), StoryFlowHandles::Source(TEXT("S1"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("S2"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S2"), TEXT("End"), StoryFlowHandles::Source(TEXT("S2"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();

	Project->Scripts.Add(TEXT("charsave"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("charsave"));

	if (!TestTrue(TEXT("SaveToSlot succeeds after the id-bound writes"), W.Subsystem->SaveToSlot(SlotName, 0)))
	{
		CleanUpWithSlot();
		return false;
	}

	const TSharedPtr<FJsonObject> Root = ReadSlotJson(*this);
	if (!TestTrue(TEXT("the slot document parses"), Root.IsValid()))
	{
		CleanUpWithSlot();
		return false;
	}

	// The V2 envelope is untouched: same version, same four sibling sections.
	TestEqual(TEXT("the save format version is still 1"), Root->GetStringField(TEXT("version")), TEXT("1"));
	TestTrue(TEXT("the save still carries globalVariables"), Root->HasField(TEXT("globalVariables")));
	TestTrue(TEXT("the save still carries usedOnceOnlyOptions"), Root->HasField(TEXT("usedOnceOnlyOptions")));

	// THE §3 SAVE-OWNERSHIP PIN, half one: the characters section is byte-for-shape the V2
	// section — path-keyed records, typed variable entries, the id-bound writes landed on the
	// id's character and nowhere else.
	const TSharedPtr<FJsonObject>* CharsSection = nullptr;
	if (TestTrue(TEXT("the save carries a characters object"), Root->TryGetObjectField(TEXT("characters"), CharsSection)))
	{
		JsonEquals(*this, TEXT("characters"),
			MakeShared<FJsonValueObject>(*CharsSection),
			MakeShared<FJsonValueObject>(ParseJsonObject(ExpectedCharactersSection)));
		// Shapes the deep compare cannot pin if both sides drifted together, spelled out:
		TestEqual(TEXT("exactly the two characters appear"), (*CharsSection)->Values.Num(), 2);
		TestFalse(TEXT("records are keyed by record key, never by character id"), (*CharsSection)->HasField(FString(HeroId)));
	}

	// Half two (§5 double-capture): the V2 dataAssets machinery captured NO character. The
	// store split makes this structural — characters never enter the seed — but pinned here
	// through the real slot so it can never silently regress.
	const TSharedPtr<FJsonObject>* DataAssetsKey = nullptr;
	if (TestTrue(TEXT("the save still carries its dataAssets key"), Root->TryGetObjectField(TEXT("dataAssets"), DataAssetsKey)))
	{
		TestEqual(TEXT("and it holds no character entries"), (*DataAssetsKey)->Values.Num(), 0);
		TestFalse(TEXT("not under the character's id"), (*DataAssetsKey)->HasField(FString(HeroId)));
		TestFalse(TEXT("nor under its record key"), (*DataAssetsKey)->HasField(FString(HeroKey)));
	}

	// --- Round trip: a post-save write must not survive the load, and a re-save must
	// reproduce the document (the §5 stability pin, through the real slot pair) ---
	FStoryFlowVariant PostSaveCoins;
	PostSaveCoins.SetInt(99);
	W.Component->SetCharacterVariableById(HeroId, TEXT("Coins"), PostSaveCoins);
	TestEqual(TEXT("the post-save write landed (control)"),
		W.Component->GetCharacterVariableById(HeroId, TEXT("Coins")).GetInt(), 99);

	if (TestTrue(TEXT("LoadFromSlot succeeds"), W.Subsystem->LoadFromSlot(SlotName, 0)))
	{
		TestEqual(TEXT("the load replaced the post-save write with the saved value"),
			W.Component->GetCharacterVariableById(HeroId, TEXT("Coins")).GetInt(), 21);
		TestEqual(TEXT("and the saved string write came back through the id lane"),
			W.Component->GetCharacterVariableById(HeroId, TEXT("Title")).GetString(), FString(TEXT("id wrote")));

		TestTrue(TEXT("a re-save succeeds"), W.Subsystem->SaveToSlot(SlotName, 0));
		const TSharedPtr<FJsonObject> Resaved = ReadSlotJson(*this);
		if (TestTrue(TEXT("the re-saved document parses"), Resaved.IsValid()))
		{
			JsonEquals(*this, TEXT("resaved document"),
				MakeShared<FJsonValueObject>(Resaved), MakeShared<FJsonValueObject>(Root));
		}
	}

	CleanUpWithSlot();
	return true;
}

// ============================================================================
// An old-shape (V2) save restores against a P4 import: wholesale replace, unchanged
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterOldSaveTest,
	"StoryFlow.Characters.Save.OldSaveWholesaleReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterOldSaveTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterSaveTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportFixture(*this, TwoCharacterIndex, TwoCharacterVariablesJson);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	// A hand-built V2-era save: no dataAssets key (written before it existed), no id anywhere,
	// and only the HERO — a save's characters section replaces WHOLESALE, so the villain must
	// be gone after the load, exactly as V2 behaved.
	UStoryFlowSaveGame* OldSave = NewObject<UStoryFlowSaveGame>();
	OldSave->SaveDataJson = TEXT(R"JSON({"version":"1","globalVariables":{},"characters":{
		"chars\\hero.sfc":{"name":"Hero","image":"","variables":{
			"Title":{"id":"var_s1","name":"Title","type":"String","isArray":false,"value":"old save title"},
			"Coins":{"id":"var_i1","name":"Coins","type":"Integer","isArray":false,"value":3}}}
		},"usedOnceOnlyOptions":[]})JSON");
	if (!TestTrue(TEXT("the doctored V2 save writes to the slot"), UGameplayStatics::SaveGameToSlot(OldSave, SlotName, 0)))
	{
		CleanUpWithSlot();
		return false;
	}

	// A pre-load session write, to prove the load replaces rather than merges.
	FStoryFlowVariant PreLoadCoins;
	PreLoadCoins.SetInt(88);
	W.Component->SetCharacterVariableById(HeroId, TEXT("Coins"), PreLoadCoins);

	if (!TestTrue(TEXT("the V2 save loads against the P4 import"), W.Subsystem->LoadFromSlot(SlotName, 0)))
	{
		CleanUpWithSlot();
		return false;
	}

	// Wholesale replace: the runtime store IS the save's characters section now.
	TestEqual(TEXT("only the save's one character is loaded"), W.Subsystem->GetRuntimeCharacters().Num(), 1);
	TestFalse(TEXT("the villain did not survive the load"), W.Subsystem->GetRuntimeCharacters().Contains(VillainKey));
	if (const FStoryFlowCharacterDef* Hero = W.Subsystem->GetRuntimeCharacters().Find(HeroKey))
	{
		TestEqual(TEXT("the saved value replaced the session write"), Hero->Variables[TEXT("Coins")].Value.GetInt(), 3);
		TestEqual(TEXT("the hero carries exactly what the save carried"), Hero->Variables.Num(), 2);
	}
	else
	{
		AddError(TEXT("the load lost the hero"));
	}

	// The bridge is project-derived, not save state: untouched by the load, so the id lane
	// still reaches the loaded record...
	TestEqual(TEXT("the bridge survived the load intact"), W.Subsystem->GetCharacterIdToPath().Num(), 2);
	TestEqual(TEXT("an id-bound read reaches the loaded V2 record"),
		W.Component->GetCharacterVariableById(HeroId, TEXT("Title")).GetString(), FString(TEXT("old save title")));

	// ...while the villain's id is now the unloaded shape: bridge hit, record missing. The
	// pure bridge lookup still answers; the record lookup reports not found and warns once.
	AddExpectedError(TEXT("is not among the loaded runtime characters"), EAutomationExpectedErrorFlags::Contains, 1);
	bool bFound = false;
	FString VillainPath;
	W.Component->GetCharacterPathById(VillainId, VillainPath, bFound);
	TestTrue(TEXT("GetCharacterPathById answers for the unloaded character (pure bridge lookup)"), bFound);
	TestEqual(TEXT("with the verbatim record key"), VillainPath, FString(VillainKey));
	FStoryFlowCharacterDef VillainDef;
	W.Component->GetCharacterById(VillainId, VillainDef, bFound);
	TestFalse(TEXT("GetCharacterById reports the unloaded character as not found"), bFound);

	CleanUpWithSlot();
	return true;
}

// ============================================================================
// Enumeration reflects the LOADED set (amendment A4)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterEnumerationTest,
	"StoryFlow.Characters.Save.EnumerationReflectsLoadedSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterEnumerationTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterSaveTestHelpers;

	// GetCharacterPaths is the A4 enumeration surface, and its whole reason to exist beside
	// the asset registry is the second half of this test: after a save load it answers with
	// the LOADED set, which no asset enumeration can know.
	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportFixture(*this, TwoCharacterIndex, TwoCharacterVariablesJson);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	TArray<FString> Paths = W.Component->GetCharacterPaths();
	TestEqual(TEXT("the fresh project enumerates both characters"), Paths.Num(), 2);
	TestTrue(TEXT("the hero's record key is enumerated"), Paths.Contains(FString(HeroKey)));
	TestTrue(TEXT("the villain's record key is enumerated"), Paths.Contains(FString(VillainKey)));

	// A hero-only save (the OldSaveWholesaleReplace shape), loaded through the real slot.
	UStoryFlowSaveGame* OldSave = NewObject<UStoryFlowSaveGame>();
	OldSave->SaveDataJson = TEXT(R"JSON({"version":"1","globalVariables":{},"characters":{
		"chars\\hero.sfc":{"name":"Hero","image":"","variables":{}}
		},"usedOnceOnlyOptions":[]})JSON");
	if (!TestTrue(TEXT("the hero-only save writes to the slot"), UGameplayStatics::SaveGameToSlot(OldSave, SlotName, 0))
		|| !TestTrue(TEXT("and loads"), W.Subsystem->LoadFromSlot(SlotName, 0)))
	{
		CleanUpWithSlot();
		return false;
	}

	Paths = W.Component->GetCharacterPaths();
	TestEqual(TEXT("after the load, enumeration is the save's set"), Paths.Num(), 1);
	TestTrue(TEXT("the save-carried key is the one enumerated"), Paths.Contains(FString(HeroKey)));
	TestFalse(TEXT("the character the save did not carry is not enumerated"), Paths.Contains(FString(VillainKey)));

	CleanUpWithSlot();
	return true;
}

// ============================================================================
// THE post-load divergence: a save older than the project's newest character
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterPostLoadDivergenceTest,
	"StoryFlow.Characters.Save.PostLoadDivergence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterPostLoadDivergenceTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterSaveTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	// --- Phase A: the two-character project. Write, save through the real slot. ---
	{
		UStoryFlowProjectAsset* ProjectA = ImportFixture(*this, TwoCharacterIndex, TwoCharacterVariablesJson);
		if (!ProjectA)
		{
			return false;
		}
		FGCObjectScopeGuard ProjectAGuard(ProjectA);
		W.Subsystem->SetProject(ProjectA);

		FStoryFlowVariant Coins;
		Coins.SetInt(55);
		W.Component->SetCharacterVariableById(HeroId, TEXT("Coins"), Coins);
		if (!TestTrue(TEXT("phase A saves"), W.Subsystem->SaveToSlot(SlotName, 0)))
		{
			CleanUpWithSlot();
			return false;
		}
	}

	// --- Phase B: the project grew a character; the save predates it. ---
	UStoryFlowProjectAsset* ProjectB = ImportFixture(*this, *ThreeCharacterIndexJson(), *ThreeCharacterVariablesJsonString());
	if (!ProjectB)
	{
		CleanUpWithSlot();
		return false;
	}
	FGCObjectScopeGuard ProjectBGuard(ProjectB);
	W.Subsystem->SetProject(ProjectB);
	TestEqual(TEXT("the grown project loads three characters"), W.Subsystem->GetRuntimeCharacters().Num(), 3);
	TestEqual(TEXT("and a three-entry bridge"), W.Subsystem->GetCharacterIdToPath().Num(), 3);

	if (!TestTrue(TEXT("the phase A save loads against the grown project"), W.Subsystem->LoadFromSlot(SlotName, 0)))
	{
		CleanUpWithSlot();
		return false;
	}

	// UP1's lifecycle asymmetry, now observable through the real pair: the character store
	// holds only what the save carried, while the bridge — refreshed from the project at
	// SetProject, untouched by the load — still knows the newcomer.
	TestEqual(TEXT("the loaded store holds only the save's two characters"), W.Subsystem->GetRuntimeCharacters().Num(), 2);
	TestFalse(TEXT("the newcomer's record is not among them"), W.Subsystem->GetRuntimeCharacters().Contains(NewcomerKey));
	TestEqual(TEXT("the bridge still carries all three ids"), W.Subsystem->GetCharacterIdToPath().Num(), 3);
	TestNotNull(TEXT("the newcomer's id included"), W.Subsystem->GetCharacterIdToPath().Find(NewcomerId));
	TestEqual(TEXT("the phase A write restored"),
		W.Component->GetCharacterVariableById(HeroId, TEXT("Coins")).GetInt(), 55);

	// Two EMITTING sites share the one latch (the pure bridge lookup below deliberately does
	// not warn — amendment A3a): the record-lookup probe, then — Reset() re-arms at dialogue
	// start — exactly one across the run's two id-bound lines.
	AddExpectedError(TEXT("is not among the loaded runtime characters"), EAutomationExpectedErrorFlags::Contains, 2);

	bool bFound = false;
	FString NewcomerPath;
	W.Component->GetCharacterPathById(NewcomerId, NewcomerPath, bFound);
	TestTrue(TEXT("the pure bridge lookup still answers for the unloaded newcomer"), bFound);
	TestEqual(TEXT("with its verbatim record key"), NewcomerPath, FString(NewcomerKey));
	FStoryFlowCharacterDef NewcomerDef;
	W.Component->GetCharacterById(NewcomerId, NewcomerDef, bFound);
	TestFalse(TEXT("the record lookup reports not found"), bFound);

	// An id-bound node for the newcomer, driven through the REAL run: warns once, falls back
	// to its path field. Two lines, one warn — the latch, not luck. The second dialogue has
	// no continuation on purpose: the run must still be live when the seam is read, because
	// Reset() (dialogue stop) clears the counter with the latch.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	for (int32 LineIndex = 1; LineIndex <= 2; ++LineIndex)
	{
		FStoryFlowNode D = MakeNode(FString::FromInt(LineIndex), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		D.Data.Text = TEXT("line");
		D.Data.Character = VillainKey;      // the fall-back the id must degrade to
		D.Data.CharacterRefId = NewcomerId; // bridged, but not loaded
		Script->Nodes.Add(D.Id, D);
	}
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
	Script->Connections.Add(MakeEdge(TEXT("1"), TEXT("2"), StoryFlowHandles::Source(TEXT("1")), StoryFlowHandles::Target(TEXT("2"))));
	Script->BuildConnectionIndices();

	ProjectB->Scripts.Add(TEXT("divergence"), Script);
	W.Component->StartDialogueWithScript(TEXT("divergence"));
	TestEqual(TEXT("the unloaded id falls back to the path field (line 1)"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Villain")));
	W.Component->AdvanceDialogue();
	TestEqual(TEXT("and again on line 2"),
		W.Component->GetCurrentDialogue().Character.Name, FString(TEXT("Villain")));
	TestTrue(TEXT("the run is still live when the warn seam is read"), W.Component->IsDialogueActive());
	TestEqual(TEXT("the run emitted exactly one warn for the unloaded id"),
		W.Component->GetCharacterIdWarningsEmitted(), 1);
	W.Component->StopDialogue();

	CleanUpWithSlot();
	return true;
}

// ============================================================================
// An undeclared variable name never becomes an add — on any write lane
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterUndeclaredWriteTest,
	"StoryFlow.Characters.Save.UndeclaredNameWriteNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterUndeclaredWriteTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterSaveTestHelpers;

	// The guarantee the back-compat story leans on: a write naming a variable the character
	// does not declare NO-OPS — it must never create the variable, or a stale script (or a
	// future sweep) would grow save state the project never declared.
	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}
	UStoryFlowProjectAsset* Project = ImportFixture(*this, TwoCharacterIndex, TwoCharacterVariablesJson);
	if (!Project)
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	// The data-asset surface's character branch: SILENT no-op by the surface's own posture
	// (bFound / return value is the whole report — no log line to expect here).
	UStoryFlowDataAssetAsset* HeroHandle = NewObject<UStoryFlowDataAssetAsset>(GetTransientPackage());
	FGCObjectScopeGuard HandleGuard(HeroHandle);
	HeroHandle->AssetId = HeroId;
	TestFalse(TEXT("a DA-surface write to an undeclared name reports not written"),
		W.Component->SetDataAssetStringVariable(HeroHandle, TEXT("Nickname"), TEXT("sneaky")));
	bool bFound = false;
	W.Component->GetDataAssetStringVariable(HeroHandle, TEXT("Nickname"), bFound);
	TestFalse(TEXT("and the name still reads as not found"), bFound);

	// The public path/id lane and the node lane both warn (their pre-P4 posture) — declared
	// to the harness, and the point stays: no add.
	AddExpectedError(TEXT("not found on character"), EAutomationExpectedErrorFlags::Contains, 2);

	FStoryFlowVariant Ghost;
	Ghost.SetString(TEXT("ghost"));
	W.Component->SetCharacterVariableById(HeroId, TEXT("Nickname"), Ghost);

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	FGCObjectScopeGuard ScriptGuard(Script);
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	{
		FStoryFlowNode S1 = MakeCharSetter(TEXT("S1"), HeroId, HeroKey, TEXT("Nickname"), TEXT("string"));
		S1.Data.Value.SetString(TEXT("ghost"));
		Script->Nodes.Add(S1.Id, S1);
	}
	Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("S1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("S1"), TEXT("0"))));
	Script->Connections.Add(MakeEdge(TEXT("S1"), TEXT("End"), StoryFlowHandles::Source(TEXT("S1"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
	Script->BuildConnectionIndices();
	Project->Scripts.Add(TEXT("undeclared"), Script);
	W.Component->StartDialogueWithScript(TEXT("undeclared"));

	// No lane created the variable. Five declared variables going in, five now.
	if (const FStoryFlowCharacterDef* Hero = W.Subsystem->GetRuntimeCharacters().Find(HeroKey))
	{
		TestEqual(TEXT("the hero still declares exactly its five variables"), Hero->Variables.Num(), 5);
		TestFalse(TEXT("no lane created the undeclared variable"), Hero->Variables.Contains(TEXT("Nickname")));
	}
	else
	{
		AddError(TEXT("the hero record disappeared"));
	}

	// And therefore no such record can reach a save.
	if (TestTrue(TEXT("a save after the refused writes succeeds"), W.Subsystem->SaveToSlot(SlotName, 0)))
	{
		const TSharedPtr<FJsonObject> Root = ReadSlotJson(*this);
		const TSharedPtr<FJsonObject>* CharsSection = nullptr;
		if (Root.IsValid() && Root->TryGetObjectField(TEXT("characters"), CharsSection))
		{
			const TSharedPtr<FJsonObject>* HeroRecord = nullptr;
			if (TestTrue(TEXT("the hero record is in the save"), (*CharsSection)->TryGetObjectField(FString(HeroKey), HeroRecord)))
			{
				const TSharedPtr<FJsonObject>* HeroVars = nullptr;
				if (TestTrue(TEXT("with its variables"), (*HeroRecord)->TryGetObjectField(TEXT("variables"), HeroVars)))
				{
					TestFalse(TEXT("the refused write never reached the slot"), (*HeroVars)->HasField(TEXT("Nickname")));
				}
			}
		}
		else
		{
			AddError(TEXT("the save lost its characters section"));
		}
	}

	CleanUpWithSlot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
