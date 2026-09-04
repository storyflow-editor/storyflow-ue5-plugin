// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Import/StoryFlowImporter.h"
#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowRuntime.h"
#include "StoryFlowScopedWorld.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "EditorAssetLibrary.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * PERSISTENCE of the `.sfd` overlay — the sparse `dataAssets` save key (engine contract §7) — and
 * the Blueprint surface that reads and writes the same store.
 *
 * Third suite over the shared golden fixtures, and the one that owns the LAST member of
 * data-assets-writes.json:
 *  - `writes` + `postWriteResolutions` .. owned by StoryFlow.DataAssets.Resolution (§5)
 *  - `saveKey` .......................... OWNED HERE: the exact sparse table snapshot() produces
 *    in the HTML runtime after those same six writes. It is the first envelope section all four
 *    runtimes share, so a drift here is a save an engine cannot read. SHAPE-identical, not
 *    byte-identical — see JsonEquals below for what is compared and what is not.
 *
 * WHY A GOLDEN COMPARE AND NOT SPOT CHECKS: the key carries BARE values (contract §7) — no types,
 * no declarations. Every way of getting that wrong (writing the typed record globals use, writing
 * a map as a JSON object instead of an ordered entry list, persisting a RESOLVED value instead of
 * the session write) still round trips inside Unreal and still resolves correctly here. Only a
 * comparison against the reference's own bytes catches it.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.DataAssets", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.DataAssets.Save" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowDataAssetSaveTestHelpers
{
	const TCHAR* SaveTestRoot = TEXT("/Game/StoryFlowDataAssetSaveTests");

	// The seed family's ids, annotated, plus the fixture loaders and value comparators, live in
	// StoryFlowEngineContractFixtures.h — shared with the resolution and node suites.
	using namespace StoryFlowEngineContract;

	FString FixtureBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowDataAssetSaveFixture"));
	}

	void CleanUp()
	{
		UEditorAssetLibrary::DeleteDirectory(SaveTestRoot);
		IFileManager::Get().DeleteDirectory(*FixtureBuildDir(), false, true);
	}

	/**
	 * Import data-assets-seed.json through the REAL importer and hand back both the project (the
	 * Blueprint test needs the asset objects a picker would give an author) and the seed the
	 * subsystem installs. Deletes any assets a previous run left behind first — a stale one
	 * carries a matching hash and would be skipped rather than re-parsed.
	 */
	UStoryFlowProjectAsset* ImportFixtureProject(FAutomationTestBase& Test, StoryFlowDataAssets::FSeed& OutSeed)
	{
		FString SeedJson;
		if (!Test.TestTrue(TEXT("data-assets-seed.json is readable"), LoadFixtureText(TEXT("data-assets-seed.json"), SeedJson)))
		{
			return nullptr;
		}

		UEditorAssetLibrary::DeleteDirectory(SaveTestRoot);

		const FString Dir = FixtureBuildDir();
		IFileManager::Get().MakeDirectory(*Dir, true);
		const bool bWrote = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
				*FPaths::Combine(Dir, TEXT("project.json")))
			&& FFileHelper::SaveStringToFile(SeedJson, *FPaths::Combine(Dir, TEXT("data-assets.json")));
		if (!Test.TestTrue(TEXT("the fixture build folder is writable"), bWrote))
		{
			return nullptr;
		}

		UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(Dir, SaveTestRoot);
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
	 * Replay data-assets-writes.json's six writes against `Overlay`, the same way the Resolution
	 * suite does (WriteValueFromJson types each value against the chain's declaration, exactly as
	 * a typed pin would). Returns false when the fixture cannot drive the replay.
	 */
	bool ReplayFixtureWrites(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Fixture,
		const StoryFlowDataAssets::FSeed& Seed, StoryFlowDataAssets::FOverlay& Overlay)
	{
		const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr;
		if (!Test.TestTrue(TEXT("the fixture carries a writes array"), Fixture->TryGetArrayField(TEXT("writes"), Writes)))
		{
			return false;
		}

		int32 Replayed = 0;
		for (const TSharedPtr<FJsonValue>& WriteValue : *Writes)
		{
			const TSharedPtr<FJsonObject> Write = WriteValue->AsObject();
			if (!Write.IsValid())
			{
				Test.AddError(TEXT("writes: a record is not an object"));
				continue;
			}
			const FString AssetId = Write->GetStringField(TEXT("assetId"));
			const FString VariableId = Write->GetStringField(TEXT("variableId"));
			const FStoryFlowVariant Value = WriteValueFromJson(Seed, AssetId, VariableId, Write->TryGetField(TEXT("value")));
			const bool bWritten = StoryFlowDataAssets::TrySet(Seed, Overlay, AssetId, VariableId, Value);
			Test.TestTrue(FString::Printf(TEXT("write %d is %s"), Replayed, *Write->GetStringField(TEXT("expect"))),
				bWritten == (Write->GetStringField(TEXT("expect")) == TEXT("written")));
			++Replayed;
		}
		return Test.TestEqual(TEXT("every data-assets-writes.json write was replayed"), Replayed, Writes->Num());
	}

	/** Serialize a state document carrying ONLY the overlay — the other sections have their own tests. */
	FString SerializeOverlayOnly(const StoryFlowDataAssets::FSeed& Seed, const StoryFlowDataAssets::FOverlay& Overlay)
	{
		const TMap<FString, FStoryFlowVariable> NoGlobals;
		const TMap<FString, FStoryFlowCharacterDef> NoCharacters;
		const TSet<FString> NoOnceOnly;
		return StoryFlowSaveHelpers::SerializeSaveData(NoGlobals, NoCharacters, NoOnceOnly, Seed, Overlay);
	}

	/** Load the overlay half of a state document, discarding the sections this suite does not own. */
	bool DeserializeOverlayOnly(const FString& Json, const StoryFlowDataAssets::FSeed& Seed, StoryFlowDataAssets::FOverlay& OutOverlay)
	{
		TMap<FString, FStoryFlowVariable> Globals;
		TMap<FString, FStoryFlowCharacterDef> Characters;
		TSet<FString> OnceOnly;
		return StoryFlowSaveHelpers::DeserializeSaveData(Json, Globals, Characters, OnceOnly, Seed, OutOverlay);
	}

	/**
	 * A hand-doctored save document, with the fixture's ids substituted for NAMED TOKENS.
	 *
	 * Token replacement rather than FString::Printf, the same idiom RoundTripSeedJson uses in the
	 * resolution suite: a positional %s list over a JSON literal is unreadable at five arguments
	 * (which id is the undeclared one?) and a single future percent sign anywhere in the body
	 * would silently corrupt a Printf-built string.
	 */
	FString DoctorSaveJson(const FString& Template)
	{
		return Template
			.Replace(TEXT("BASE_ID"), BaseId, ESearchCase::CaseSensitive)
			.Replace(TEXT("ABSENT_ID"), AbsentId, ESearchCase::CaseSensitive)
			.Replace(TEXT("HP_ID"), HpId, ESearchCase::CaseSensitive)
			.Replace(TEXT("NOWHERE_ID"), NowhereId, ESearchCase::CaseSensitive)
			.Replace(TEXT("LORE_ID"), LoreId, ESearchCase::CaseSensitive);
	}

	/** The JSON type of one field, or EJson::None when the field is absent — never a null deref. */
	EJson JsonTypeOf(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		return Value.IsValid() ? Value->Type : EJson::None;
	}

	TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** The serialized document's `dataAssets` key, or null when it is absent or not an object. */
	TSharedPtr<FJsonObject> SavedDataAssetKey(const FString& SaveJson)
	{
		const TSharedPtr<FJsonObject> Root = ParseJsonObject(SaveJson);
		const TSharedPtr<FJsonObject>* Key = nullptr;
		if (Root.IsValid() && Root->TryGetObjectField(TEXT("dataAssets"), Key))
		{
			return *Key;
		}
		return nullptr;
	}

	// JsonEquals — the deep structural JSON compare — moved to StoryFlowEngineContractFixtures.h
	// when the character save pins started comparing whole save sections too; the `using` above
	// keeps every call site here reading as before.

	// FScopedWorld — the standalone game instance with a registered component — lives in
	// StoryFlowScopedWorld.h, shared with the node suite.
	using StoryFlowTestWorld::FScopedWorld;

	/** The save slot the two real-slot tests write and delete. */
	const TCHAR* SlotName = TEXT("StoryFlowDataAssetSlotTest");

	/**
	 * start -> dialogue with one option -> (nothing).
	 *
	 * An option means the dialogue WAITS rather than running to an end node, which is what keeps
	 * it counted as active. Deliberately tiny: the restart test cares about the start/stop pair,
	 * not about anything the dialogue says.
	 */
	UStoryFlowScriptAsset* MakeWaitingScript()
	{
		UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
		Script->StartNode = TEXT("0");

		FStoryFlowNode Start;
		Start.Id = TEXT("0");
		Start.Type = EStoryFlowNodeType::Start;
		Start.TypeString = TEXT("start");
		Script->Nodes.Add(Start.Id, Start);

		FStoryFlowNode Dialogue;
		Dialogue.Id = TEXT("d");
		Dialogue.Type = EStoryFlowNodeType::Dialogue;
		Dialogue.TypeString = TEXT("dialogue");
		Dialogue.Data.Text = TEXT("waiting");
		FStoryFlowChoice Choice;
		Choice.Id = TEXT("opt");
		Choice.Text = TEXT("stay");
		Dialogue.Data.Options.Add(Choice);
		Script->Nodes.Add(Dialogue.Id, Dialogue);

		FStoryFlowConnection Edge;
		Edge.Id = TEXT("0->d");
		Edge.Source = TEXT("0");
		Edge.Target = TEXT("d");
		Edge.SourceHandle = StoryFlowHandles::Source(TEXT("0"));
		Edge.TargetHandle = StoryFlowHandles::Target(TEXT("d"));
		Script->Connections.Add(Edge);

		Script->BuildConnectionIndices();
		return Script;
	}
}

// ============================================================================
// The golden save key (§7) — the fixture's `saveKey`, compared structurally
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSaveKeyFixtureTest,
	"StoryFlow.DataAssets.Save.GoldenKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetSaveKeyFixtureTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	TSharedPtr<FJsonObject> Fixture = LoadFixture(TEXT("data-assets-writes.json"));
	if (!TestTrue(TEXT("data-assets-writes.json parses"), Fixture.IsValid()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed Seed;
	if (!ImportFixtureProject(*this, Seed))
	{
		return false;
	}

	StoryFlowDataAssets::FOverlay Overlay;
	if (!ReplayFixtureWrites(*this, Fixture, Seed, Overlay))
	{
		CleanUp();
		return false;
	}

	const FString SaveJson = SerializeOverlayOnly(Seed, Overlay);
	const TSharedPtr<FJsonObject> Root = ParseJsonObject(SaveJson);
	if (!TestTrue(TEXT("the serialized save parses"), Root.IsValid()))
	{
		CleanUp();
		return false;
	}

	// The key is ADDITIVE (contract §7): the surrounding v1 document is unchanged, and the format
	// version does not move for it — an older plugin build loads this save by ignoring the key.
	TestEqual(TEXT("the save format version is still 1"), Root->GetStringField(TEXT("version")), TEXT("1"));
	TestTrue(TEXT("the save still carries its globalVariables section"), Root->HasField(TEXT("globalVariables")));
	TestTrue(TEXT("the save still carries its characters section"), Root->HasField(TEXT("characters")));
	TestTrue(TEXT("the save still carries its usedOnceOnlyOptions section"), Root->HasField(TEXT("usedOnceOnlyOptions")));

	const TSharedPtr<FJsonObject>* ActualKey = nullptr;
	if (!TestTrue(TEXT("the save carries a dataAssets object"), Root->TryGetObjectField(TEXT("dataAssets"), ActualKey)))
	{
		CleanUp();
		return false;
	}
	const TSharedPtr<FJsonObject>* ExpectedKey = nullptr;
	if (!TestTrue(TEXT("the fixture carries a saveKey object"), Fixture->TryGetObjectField(TEXT("saveKey"), ExpectedKey)))
	{
		CleanUp();
		return false;
	}

	// THE golden comparison
	JsonEquals(*this, TEXT("dataAssets"),
		MakeShared<FJsonValueObject>(*ActualKey), MakeShared<FJsonValueObject>(*ExpectedKey));

	// Assertions the deep compare cannot make on its own, because a compare of two things that
	// drifted TOGETHER still passes: the shapes the contract fixes, spelled out.
	TestEqual(TEXT("exactly the three written-to assets appear"), (*ActualKey)->Values.Num(), 3);
	TestFalse(TEXT("the refused write reached no asset at all"),
		(*ActualKey)->HasField(FString(AbsentId)));

	const TSharedPtr<FJsonObject>* BaseEntry = nullptr;
	if (TestTrue(TEXT("the base's writes are in the key"), (*ActualKey)->TryGetObjectField(FString(BaseId), BaseEntry)))
	{
		TestEqual(TEXT("the base carries exactly its two writes"), (*BaseEntry)->Values.Num(), 2);
		// BARE values, not the typed { id, name, type, value } record globals persist as
		TestEqual(TEXT("a boolean write persists as a bare JSON boolean"),
			static_cast<int32>(JsonTypeOf(*BaseEntry, FString(AliveId))), static_cast<int32>(EJson::Boolean));
		TestEqual(TEXT("an integer write persists as a bare JSON number"),
			static_cast<int32>(JsonTypeOf(*BaseEntry, FString(HpId))), static_cast<int32>(EJson::Number));
		// SPARSE: the base was written twice and declares eleven variables
		TestFalse(TEXT("a variable nobody wrote is not in the key"), (*BaseEntry)->HasField(FString(SpeedId)));
		TestFalse(TEXT("and neither is one only its descendant wrote"), (*BaseEntry)->HasField(FString(TagsId)));
	}

	const TSharedPtr<FJsonObject>* ChildEntry = nullptr;
	if (TestTrue(TEXT("the child's writes are in the key"), (*ActualKey)->TryGetObjectField(FString(ChildId), ChildEntry)))
	{
		const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
		if (TestTrue(TEXT("the array write persists as a bare JSON array"), (*ChildEntry)->TryGetArrayField(FString(TagsId), Tags)))
		{
			// The value written, in the order written — NOT the seed's ["mob","melee"]
			if (TestEqual(TEXT("the array write kept its three elements"), Tags->Num(), 3))
			{
				TestEqual(TEXT("the array write kept its order"), (*Tags)[1]->AsString(), TEXT("elite"));
			}
		}
	}

	const TSharedPtr<FJsonObject>* GrandChildEntry = nullptr;
	if (TestTrue(TEXT("the grandchild's map write is in the key"), (*ActualKey)->TryGetObjectField(FString(GrandChildId), GrandChildEntry)))
	{
		const TArray<TSharedPtr<FJsonValue>>* Loot = nullptr;
		if (TestTrue(TEXT("a map write persists as an ORDERED ENTRY LIST, never a JSON object"),
			(*GrandChildEntry)->TryGetArrayField(FString(LootId), Loot)))
		{
			if (TestEqual(TEXT("the map write kept both entries"), Loot->Num(), 2))
			{
				const TSharedPtr<FJsonObject> First = (*Loot)[0]->AsObject();
				if (TestTrue(TEXT("a map entry is a {key, value} object"), First.IsValid()))
				{
					// The AUTHORED order of the write, gems before gold — not the seed's
					TestEqual(TEXT("the map write kept its authored key order"), First->GetStringField(TEXT("key")), TEXT("gems"));
					TestEqual(TEXT("the map write kept its values"), static_cast<int32>(First->GetNumberField(TEXT("value"))), 2);
				}
			}
		}
	}

	CleanUp();
	return true;
}

// ============================================================================
// Round trip: save -> clear -> load, and load REPLACES
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSaveRoundTripTest,
	"StoryFlow.DataAssets.Save.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetSaveRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	TSharedPtr<FJsonObject> Fixture = LoadFixture(TEXT("data-assets-writes.json"));
	if (!TestTrue(TEXT("data-assets-writes.json parses"), Fixture.IsValid()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed Seed;
	if (!ImportFixtureProject(*this, Seed))
	{
		return false;
	}

	StoryFlowDataAssets::FOverlay Overlay;
	if (!ReplayFixtureWrites(*this, Fixture, Seed, Overlay))
	{
		CleanUp();
		return false;
	}

	const FString SaveJson = SerializeOverlayOnly(Seed, Overlay);

	// --- Writes made AFTER the save must not survive the load (contract §7: replace, not merge) ---
	TestTrue(TEXT("a post-save write lands"), StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, AliveId, FStoryFlowVariant::FromBool(true)));
	{
		FStoryFlowVariant PostSaveEnum;
		PostSaveEnum.SetEnum(TEXT("Warlord"));
		TestTrue(TEXT("a second post-save write, on a variable the save does not carry, lands"),
			StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, RankId, PostSaveEnum));
	}

	if (!TestTrue(TEXT("the save deserializes"), DeserializeOverlayOnly(SaveJson, Seed, Overlay)))
	{
		CleanUp();
		return false;
	}

	// The WHOLE post-write resolution table again, now reached through the loaded overlay. This
	// is the round trip: every value the writes produced, back from JSON with no types in it.
	const TArray<TSharedPtr<FJsonValue>>* PostWrite = nullptr;
	if (TestTrue(TEXT("the fixture carries a postWriteResolutions array"), Fixture->TryGetArrayField(TEXT("postWriteResolutions"), PostWrite)))
	{
		const int32 Compared = AssertResolutionTable(*this, Seed, Overlay, *PostWrite, TEXT("postLoadResolutions"));
		TestEqual(TEXT("every postWriteResolutions record was compared after the load"), Compared, PostWrite->Num());
		TestTrue(TEXT("postWriteResolutions is not empty"), PostWrite->Num() > 0);
	}

	// Load REPLACED: the post-save writes are gone, both the one that shadowed a saved value and
	// the one on a variable the save does not mention at all. A merging load keeps either.
	TestEqual(TEXT("the post-save boolean write did not survive the load"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, AliveId).GetBool(true), false);
	if (const TMap<FString, FStoryFlowVariant>* BaseEntries = Overlay.Find(BaseId))
	{
		TestFalse(TEXT("the post-save enum write is not in the loaded overlay"), BaseEntries->Contains(RankId));
		TestEqual(TEXT("the loaded base carries exactly the two saved writes"), BaseEntries->Num(), 2);
	}
	else
	{
		AddError(TEXT("the loaded overlay lost the base entirely"));
	}
	TestEqual(TEXT("the post-save enum write is not visible to a read either"),
		StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, RankId).GetString(), TEXT("Grunt"));

	// --- TYPE TAGS survive the trip, restored from the DECLARATION (the save carries none) ---
	// A wrong tag is invisible to a read — an enum and a string both answer GetString — and shows
	// up as a differently shaped NEXT save, so a save/load/save cycle would not be stable.
	if (const TMap<FString, FStoryFlowVariant>* BaseEntries = Overlay.Find(BaseId))
	{
		if (const FStoryFlowVariant* Alive = BaseEntries->Find(AliveId))
		{
			TestTrue(TEXT("a loaded boolean is typed Boolean"), Alive->GetType() == EStoryFlowVariableType::Boolean);
		}
		if (const FStoryFlowVariant* Hp = BaseEntries->Find(HpId))
		{
			TestTrue(TEXT("a loaded integer is typed Integer, not Float"), Hp->GetType() == EStoryFlowVariableType::Integer);
		}
	}
	if (const TMap<FString, FStoryFlowVariant>* ChildEntries = Overlay.Find(ChildId))
	{
		if (const FStoryFlowVariant* Tags = ChildEntries->Find(TagsId))
		{
			TestTrue(TEXT("a loaded array keeps its declared ELEMENT type"), Tags->GetType() == EStoryFlowVariableType::String);
			TestEqual(TEXT("a loaded array keeps its elements"), Tags->GetArray().Num(), 3);
		}
	}
	if (const TMap<FString, FStoryFlowVariant>* GrandChildEntries = Overlay.Find(GrandChildId))
	{
		if (const FStoryFlowVariant* Loot = GrandChildEntries->Find(LootId))
		{
			TestTrue(TEXT("a loaded map is typed Map"), Loot->IsMap());
			if (TestEqual(TEXT("a loaded map keeps its entries"), Loot->GetMap().Num(), 2))
			{
				TestEqual(TEXT("a loaded map keeps its authored key order"), Loot->GetMap()[0].Key.GetString(), TEXT("gems"));
				TestTrue(TEXT("a loaded map entry value is typed from the declaration's valueType"),
					Loot->GetMap()[0].Value.GetType() == EStoryFlowVariableType::Integer);
			}
		}
	}

	// --- STABILITY: saving the loaded overlay produces the same document again ---
	// The cheapest possible proof that no type was lost on the way in: a lost tag changes the
	// shape of the next save, whether or not any read notices.
	const FString ResavedJson = SerializeOverlayOnly(Seed, Overlay);
	JsonEquals(*this, TEXT("resaved dataAssets"),
		MakeShared<FJsonValueObject>(SavedDataAssetKey(ResavedJson)),
		MakeShared<FJsonValueObject>(SavedDataAssetKey(SaveJson)));

	CleanUp();
	return true;
}

// ============================================================================
// Value shapes the writes fixture does not carry: enum, empty array, float
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSaveValueShapesTest,
	"StoryFlow.DataAssets.Save.ValueShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetSaveValueShapesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	StoryFlowDataAssets::FSeed Seed;
	if (!ImportFixtureProject(*this, Seed))
	{
		return false;
	}

	StoryFlowDataAssets::FOverlay Overlay;

	FStoryFlowVariant EnumWrite;
	EnumWrite.SetEnum(TEXT("Warlord"));
	TestTrue(TEXT("an enum write lands"), StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, RankId, EnumWrite));

	FStoryFlowVariant FloatWrite;
	FloatWrite.SetFloat(3.5f);
	TestTrue(TEXT("a float write lands"), StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, SpeedId, FloatWrite));

	// THE EMPTY ARRAY. In C++ an empty array and a scalar are the same variant (the variant
	// carries the ELEMENT type and has no array flag), so only the declaration can say which one
	// this is — and a cleared array that persisted as `""` would reload as a scalar.
	FStoryFlowVariant EmptyArrayWrite;
	EmptyArrayWrite.SetArray(TArray<FStoryFlowVariant>(), EStoryFlowVariableType::String);
	TestTrue(TEXT("an empty array write lands"), StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, TagsId, EmptyArrayWrite));

	// AND THE OTHER DIRECTION: the declaration must be able to say "not an array" too. A variant
	// that once held an array and was re-set as a scalar KEEPS its old elements — only SetArray
	// and SetMap clear the containers, the scalar setters do not — so a serializer that trusted
	// "there are elements" over the declaration would write this scalar as a JSON array, and it
	// would reload as a scalar, silently losing the value.
	FStoryFlowVariant ResidualArrayWrite;
	{
		TArray<FStoryFlowVariant> Leftovers;
		FStoryFlowVariant Leftover;
		Leftover.SetInt(9);
		Leftovers.Add(Leftover);
		ResidualArrayWrite.SetArray(Leftovers, EStoryFlowVariableType::Integer);
		ResidualArrayWrite.SetInt(11);
	}
	TestEqual(TEXT("a re-set variant really does keep its old array elements"), ResidualArrayWrite.GetArray().Num(), 1);
	TestTrue(TEXT("a scalar write carrying residual array storage lands"),
		StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, HpId, ResidualArrayWrite));

	const FString SaveJson = SerializeOverlayOnly(Seed, Overlay);
	const TSharedPtr<FJsonObject> Key = SavedDataAssetKey(SaveJson);
	if (!TestTrue(TEXT("the save carries a dataAssets key"), Key.IsValid()))
	{
		CleanUp();
		return false;
	}

	const TSharedPtr<FJsonObject>* BaseEntry = nullptr;
	if (TestTrue(TEXT("the base's writes are in the key"), Key->TryGetObjectField(FString(BaseId), BaseEntry)))
	{
		TestEqual(TEXT("an enum persists as a bare string, like every string-family value"),
			(*BaseEntry)->GetStringField(FString(RankId)), TEXT("Warlord"));
		TestNearlyEqual(TEXT("a float persists as a bare number"), (*BaseEntry)->GetNumberField(FString(SpeedId)), 3.5, 1.e-4);
		const TArray<TSharedPtr<FJsonValue>>* EmptyTags = nullptr;
		if (TestTrue(TEXT("an EMPTY array still persists as an array"), (*BaseEntry)->TryGetArrayField(FString(TagsId), EmptyTags)))
		{
			TestEqual(TEXT("and it is empty"), EmptyTags->Num(), 0);
		}
		// The declaration vetoing the variant's residual array storage
		TestEqual(TEXT("a scalar under a scalar declaration persists as a bare number, residue or not"),
			static_cast<int32>(JsonTypeOf(*BaseEntry, FString(HpId))), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("with its scalar value, not its leftover element"),
			static_cast<int32>((*BaseEntry)->GetNumberField(FString(HpId))), 11);
	}

	StoryFlowDataAssets::FOverlay Loaded;
	if (TestTrue(TEXT("the save deserializes"), DeserializeOverlayOnly(SaveJson, Seed, Loaded)))
	{
		const TMap<FString, FStoryFlowVariant>* BaseEntries = Loaded.Find(BaseId);
		if (TestNotNull(TEXT("the loaded overlay carries the base"), BaseEntries))
		{
			if (const FStoryFlowVariant* Rank = BaseEntries->Find(RankId))
			{
				// Enum, not String: the declaration is the type authority on the way back in
				TestTrue(TEXT("a loaded enum is typed Enum, not String"), Rank->GetType() == EStoryFlowVariableType::Enum);
				TestEqual(TEXT("a loaded enum keeps its value"), Rank->GetString(), TEXT("Warlord"));
			}
			if (const FStoryFlowVariant* Speed = BaseEntries->Find(SpeedId))
			{
				TestTrue(TEXT("a loaded float is typed Float, not Integer"), Speed->GetType() == EStoryFlowVariableType::Float);
				TestNearlyEqual(TEXT("a loaded float keeps its value"), Speed->GetFloat(), 3.5f, 1.e-4f);
			}
			if (const FStoryFlowVariant* Hp = BaseEntries->Find(HpId))
			{
				TestTrue(TEXT("the residual-array scalar comes back a scalar"), Hp->GetType() == EStoryFlowVariableType::Integer);
				TestEqual(TEXT("with its value intact"), Hp->GetInt(), 11);
				TestEqual(TEXT("and no array storage behind it"), Hp->GetArray().Num(), 0);
			}
			if (const FStoryFlowVariant* Tags = BaseEntries->Find(TagsId))
			{
				TestTrue(TEXT("a loaded empty array is still typed with its element type"), Tags->GetType() == EStoryFlowVariableType::String);
				TestEqual(TEXT("a loaded empty array is still empty"), Tags->GetArray().Num(), 0);
				TestFalse(TEXT("and it is not a map"), Tags->IsMap());
			}
			// The cascade the empty write is FOR: the child's file override no longer wins,
			// because an overlay entry on the base is nearer than nothing at the child... and the
			// child DOES override tags, so the child keeps its own. The grandchild does not.
			TestEqual(TEXT("the emptied array cascades to a descendant that does not override it"),
				StoryFlowDataAssets::Resolve(Seed, Loaded, BaseId, TagsId).GetArray().Num(), 0);
		}
	}

	CleanUp();
	return true;
}

// ============================================================================
// Load rules: absent key, malformed key, dropped entries
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSaveLoadRulesTest,
	"StoryFlow.DataAssets.Save.LoadRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetSaveLoadRulesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	StoryFlowDataAssets::FSeed Seed;
	if (!ImportFixtureProject(*this, Seed))
	{
		return false;
	}

	// A session with writes in it, re-established before each load below
	auto SeedOverlay = [&Seed](StoryFlowDataAssets::FOverlay& Overlay)
	{
		StoryFlowDataAssets::ResetOverlay(Overlay);
		StoryFlowDataAssets::TrySet(Seed, Overlay, BaseId, AliveId, FStoryFlowVariant::FromBool(false));
	};

	StoryFlowDataAssets::FOverlay Overlay;

	// --- An OLD SAVE, written before this key existed, clears (contract §7) ---
	SeedOverlay(Overlay);
	const FString OldSave = TEXT(R"JSON({"version":"1","globalVariables":{},"characters":{},"usedOnceOnlyOptions":[]})JSON");
	TestTrue(TEXT("a save from before the dataAssets key still loads"), DeserializeOverlayOnly(OldSave, Seed, Overlay));
	TestEqual(TEXT("and it CLEARS the overlay rather than leaving this session's writes"), Overlay.Num(), 0);
	TestTrue(TEXT("so the seed's own value is what resolves"), StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, AliveId).GetBool(false));

	// --- A malformed key clears the same way ---
	SeedOverlay(Overlay);
	const FString MalformedSave = TEXT(R"JSON({"version":"1","dataAssets":5})JSON");
	TestTrue(TEXT("a save with a malformed dataAssets key still loads"), DeserializeOverlayOnly(MalformedSave, Seed, Overlay));
	TestEqual(TEXT("and clears"), Overlay.Num(), 0);

	// --- Entries the current project cannot reach are DROPPED ---
	SeedOverlay(Overlay);
	// A real write, an undeclared id and a category row on a reachable asset, then a whole asset
	// this project does not carry
	const FString DoctoredSave = DoctorSaveJson(TEXT(R"JSON({"version":"1","dataAssets":{
			"BASE_ID": { "HP_ID": 3, "NOWHERE_ID": true, "LORE_ID": false },
			"ABSENT_ID": { "HP_ID": 5 }
		}})JSON"));
	// The dropped-asset line is a Warning by design (a save that outlived an asset is worth
	// saying out loud), so the harness is told to expect it rather than failing on it.
	AddExpectedError(TEXT("Save load dropped Data Asset"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("a doctored save loads"), DeserializeOverlayOnly(DoctoredSave, Seed, Overlay));

	TestEqual(TEXT("only the one reachable asset survives the load"), Overlay.Num(), 1);
	TestFalse(TEXT("an entry for an asset this project does not carry is dropped"), Overlay.Contains(FString(AbsentId)));
	if (const TMap<FString, FStoryFlowVariant>* BaseEntries = Overlay.Find(BaseId))
	{
		TestEqual(TEXT("only the declared variable survived"), BaseEntries->Num(), 1);
		TestTrue(TEXT("the declared write is the one that survived"), BaseEntries->Contains(FString(HpId)));
		TestFalse(TEXT("a variable no chain level declares is dropped"), BaseEntries->Contains(FString(NowhereId)));
		// A category declaration is skipped at import, so it has no declaration to type against
		TestFalse(TEXT("a category row is dropped"), BaseEntries->Contains(FString(LoreId)));
	}
	else
	{
		AddError(TEXT("the doctored save lost the base entirely"));
	}
	TestEqual(TEXT("the surviving write resolves"), StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, HpId).GetInt(), 3);
	TestTrue(TEXT("and the pre-load write it replaced is gone"), StoryFlowDataAssets::Resolve(Seed, Overlay, BaseId, AliveId).GetBool(false));

	// --- An asset whose every entry was dropped leaves NO entry behind ---
	SeedOverlay(Overlay);
	const FString AllDroppedSave = DoctorSaveJson(TEXT(R"JSON({"version":"1","dataAssets":{ "BASE_ID": { "NOWHERE_ID": 1 } }})JSON"));
	TestTrue(TEXT("a save whose only entry is undeclared loads"), DeserializeOverlayOnly(AllDroppedSave, Seed, Overlay));
	TestEqual(TEXT("and leaves an empty overlay, not an empty asset table"), Overlay.Num(), 0);

	// --- The key is additive: the other sections load beside it ---
	StoryFlowDataAssets::FOverlay Mixed;
	StoryFlowDataAssets::TrySet(Seed, Mixed, BaseId, AliveId, FStoryFlowVariant::FromBool(false));
	TMap<FString, FStoryFlowVariable> Globals;
	FStoryFlowVariable Global;
	Global.Id = TEXT("g1");
	Global.Name = TEXT("Chapter");
	Global.Type = EStoryFlowVariableType::Integer;
	Global.Value.SetInt(4);
	Globals.Add(Global.Id, Global);
	TMap<FString, FStoryFlowCharacterDef> Characters;
	TSet<FString> OnceOnly;
	OnceOnly.Add(TEXT("node1-opt2"));

	const FString MixedJson = StoryFlowSaveHelpers::SerializeSaveData(Globals, Characters, OnceOnly, Seed, Mixed);
	TMap<FString, FStoryFlowVariable> LoadedGlobals;
	TMap<FString, FStoryFlowCharacterDef> LoadedCharacters;
	TSet<FString> LoadedOnceOnly;
	StoryFlowDataAssets::FOverlay LoadedOverlay;
	if (TestTrue(TEXT("a document carrying every section loads"),
		StoryFlowSaveHelpers::DeserializeSaveData(MixedJson, LoadedGlobals, LoadedCharacters, LoadedOnceOnly, Seed, LoadedOverlay)))
	{
		if (const FStoryFlowVariable* LoadedGlobal = LoadedGlobals.Find(TEXT("g1")))
		{
			TestEqual(TEXT("the globals section is untouched by the new key"), LoadedGlobal->Value.GetInt(), 4);
		}
		else
		{
			AddError(TEXT("the globals section did not survive a document carrying dataAssets"));
		}
		TestTrue(TEXT("the once-only section is untouched by the new key"), LoadedOnceOnly.Contains(TEXT("node1-opt2")));
		TestEqual(TEXT("and the overlay came back too"), LoadedOverlay.Num(), 1);
	}

	// --- A record naming a type this build cannot parse is DROPPED, the document still loads ---
	// The save format's names are CAPITALIZED ("Boolean") and matched EXACTLY, the same rule the
	// exporter's lowercase wire tokens ("boolean") follow — two vocabularies, both exact. Unity
	// parses these with Enum.TryParse(ignoreCase: false) and Godot with an exact dictionary
	// match, so a spelling this reader accepted leniently would be a record the siblings drop.
	// FString's == is case-INSENSITIVE, which is what made that possible here. "None" is in the
	// same bucket: it is what the writer emits for a type it cannot name, so it names none
	// coming back. Unusable records are skipped, never stored untyped and never allowed to
	// shadow the declaration the project already carries.
	const FString DoctoredTypeNames = TEXT(R"JSON({"version":"1","globalVariables":{
			"keep":  { "id": "keep",  "name": "Chapter", "type": "Integer", "isArray": false, "value": 4 },
			"wire":  { "id": "wire",  "name": "Alive",   "type": "boolean", "isArray": false, "value": true },
			"shout": { "id": "shout", "name": "Loud",    "type": "BOOLEAN", "isArray": false, "value": true },
			"none":  { "id": "none",  "name": "Untyped", "type": "None",    "isArray": false, "value": true }
		},"characters":{
			"hero": { "name": "Hero", "image": "", "variables": {
				"mood":  { "id": "mood",  "name": "Mood",  "type": "String", "isArray": false, "value": "calm" },
				"weird": { "id": "weird", "name": "Weird", "type": "sTRING", "isArray": false, "value": "x" }
			} }
		},"usedOnceOnlyOptions":[]})JSON");

	TMap<FString, FStoryFlowVariable> TypedGlobals;
	TMap<FString, FStoryFlowCharacterDef> TypedCharacters;
	TSet<FString> TypedOnceOnly;
	StoryFlowDataAssets::FOverlay TypedOverlay;
	if (TestTrue(TEXT("a save carrying unparseable type names still loads"),
		StoryFlowSaveHelpers::DeserializeSaveData(DoctoredTypeNames, TypedGlobals, TypedCharacters, TypedOnceOnly, Seed, TypedOverlay)))
	{
		TestEqual(TEXT("only the exactly named global survives"), TypedGlobals.Num(), 1);
		TestTrue(TEXT("and it is the canonical one"), TypedGlobals.Contains(TEXT("keep")));
		TestFalse(TEXT("the wire vocabulary's lowercase spelling is not a save type name"), TypedGlobals.Contains(TEXT("wire")));
		TestFalse(TEXT("neither is an upper-cased variant"), TypedGlobals.Contains(TEXT("shout")));
		TestFalse(TEXT("and None names no type at all"), TypedGlobals.Contains(TEXT("none")));
		if (const FStoryFlowCharacterDef* Hero = TypedCharacters.Find(TEXT("hero")))
		{
			TestEqual(TEXT("a character's unparseable variable is dropped the same way"), Hero->Variables.Num(), 1);
			TestTrue(TEXT("leaving the exactly named one"), Hero->Variables.Contains(TEXT("mood")));
		}
		else
		{
			AddError(TEXT("the doctored save lost the character entirely"));
		}
	}

	// --- ...and the nine names the writer DOES emit all still parse ---
	// Round trip rather than direct calls: the name table is file-local to the reader. A name
	// dropped from it would fail nothing above and silently discard every record of that type.
	const EStoryFlowVariableType EveryType[] = {
		EStoryFlowVariableType::Boolean, EStoryFlowVariableType::Integer, EStoryFlowVariableType::Float,
		EStoryFlowVariableType::String, EStoryFlowVariableType::Enum, EStoryFlowVariableType::Image,
		EStoryFlowVariableType::Audio, EStoryFlowVariableType::Character, EStoryFlowVariableType::Map,
	};
	TMap<FString, FStoryFlowVariable> EveryTypeGlobals;
	for (const EStoryFlowVariableType Type : EveryType)
	{
		FStoryFlowVariable Typed;
		Typed.Id = FString::Printf(TEXT("v%d"), static_cast<int32>(Type));
		Typed.Name = Typed.Id;
		Typed.Type = Type;
		// A value of the record's own type: a default variant would serialize as JSON null,
		// which the reader is entitled to complain about and this test is not about.
		switch (Type)
		{
		case EStoryFlowVariableType::Boolean: Typed.Value.SetBool(true); break;
		case EStoryFlowVariableType::Integer: Typed.Value.SetInt(7); break;
		case EStoryFlowVariableType::Float:   Typed.Value.SetFloat(1.5f); break;
		case EStoryFlowVariableType::Enum:    Typed.Value.SetEnum(TEXT("A")); break;
		case EStoryFlowVariableType::Map:     Typed.Value.SetMap(TArray<FStoryFlowMapEntry>()); break;
		default:                              Typed.Value.SetString(TEXT("x")); break;
		}
		EveryTypeGlobals.Add(Typed.Id, Typed);
	}

	const StoryFlowDataAssets::FOverlay NoWrites;
	TMap<FString, FStoryFlowCharacterDef> NoCharacters;
	const TSet<FString> NoOnceOnly;
	const FString EveryTypeJson = StoryFlowSaveHelpers::SerializeSaveData(
		EveryTypeGlobals, NoCharacters, NoOnceOnly, Seed, NoWrites);

	TMap<FString, FStoryFlowVariable> EveryTypeLoaded;
	TSet<FString> EveryTypeOnceOnly;
	StoryFlowDataAssets::FOverlay EveryTypeOverlay;
	if (TestTrue(TEXT("a document naming every type loads"),
		StoryFlowSaveHelpers::DeserializeSaveData(EveryTypeJson, EveryTypeLoaded, NoCharacters, EveryTypeOnceOnly, Seed, EveryTypeOverlay)))
	{
		TestEqual(TEXT("all nine typed records survive the round trip"), EveryTypeLoaded.Num(), 9);
		for (const EStoryFlowVariableType Type : EveryType)
		{
			const FString Id = FString::Printf(TEXT("v%d"), static_cast<int32>(Type));
			const FStoryFlowVariable* Loaded = EveryTypeLoaded.Find(Id);
			TestTrue(FString::Printf(TEXT("the save name for type %d round trips"), static_cast<int32>(Type)),
				Loaded != nullptr && Loaded->Type == Type);
		}
	}

	// --- An untouched session serializes an EMPTY key, not a missing one ---
	const StoryFlowDataAssets::FOverlay Untouched;
	const TSharedPtr<FJsonObject> EmptyKey = SavedDataAssetKey(SerializeOverlayOnly(Seed, Untouched));
	if (TestTrue(TEXT("an untouched session still writes the dataAssets key"), EmptyKey.IsValid()))
	{
		TestEqual(TEXT("and it is an empty table"), EmptyKey->Values.Num(), 0);
	}

	CleanUp();
	return true;
}

// ============================================================================
// `.sfd` media (engine contract §2.1's amendment, 2026-09-04)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetMediaTest,
	"StoryFlow.DataAssets.Save.Media",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

/**
 * A `.sfd` image value resolves to a real imported asset.
 *
 * The value ships as an asset KEY and `data-assets.json` carries its own `assets` registry; this
 * pins the engine half of that bargain — the registry is imported into the PROJECT's resolved-asset
 * pool, so the key an accessor hands back names something the build contains. Before the amendment
 * a `.sfd` portrait was a path to a file nothing had copied.
 *
 * The fixture writes a REAL 1x1 PNG rather than a stub: `ImportMediaAssets` runs the engine's image
 * import, and a few bytes of nonsense would fail that import and leave the pool empty — the test
 * would then fail for a reason that has nothing to do with what it checks.
 */
bool FStoryFlowDataAssetMediaTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	UEditorAssetLibrary::DeleteDirectory(SaveTestRoot);
	const FString Dir = FixtureBuildDir();
	IFileManager::Get().MakeDirectory(*Dir, true);

	// A 1x1 PNG, the smallest input the engine's importer actually accepts.
	TArray<uint8> Png;
	if (!TestTrue(TEXT("the 1x1 PNG fixture decodes"),
			FBase64::Decode(TEXT("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="), Png)))
	{
		return false;
	}

	const FString MediaRelative = TEXT("images/items/icon.png");
	const bool bWrote = FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Dir, MediaRelative))
		&& FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")))
		&& FFileHelper::SaveStringToFile(
			TEXT(R"JSON({"dataAssets":{"da_mediabase00000000000000000000":{"id":"da_mediabase00000000000000000000","name":"ItemBase","parent":null,"variables":[{"id":"v-icon","name":"Icon","type":"image","value":"asset_image_900"}],"overrides":{}}},"assets":{"asset_image_900":{"id":"asset_image_900","type":"image","path":"images/items/icon.png"}}})JSON"),
			*FPaths::Combine(Dir, TEXT("data-assets.json")));
	if (!TestTrue(TEXT("the media fixture build folder is writable"), bWrote))
	{
		CleanUp();
		return false;
	}

	UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(Dir, SaveTestRoot);
	if (!TestNotNull(TEXT("the media fixture imports"), Project))
	{
		CleanUp();
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	// The value stays the KEY — the `.sfd` surface learns nothing about assets.
	UStoryFlowDataAssetAsset* Base = Project->DataAssets.FindRef(TEXT("da_mediabase00000000000000000000"));
	if (TestNotNull(TEXT("the seeded asset imported"), Base) && Base->Variables.Num() > 0)
	{
		TestEqual(TEXT("the image value is the asset key, not a path"),
			Base->Variables[0].Value.GetString(), FString(TEXT("asset_image_900")));
	}

	// THE HALF THAT WAS BROKEN: the key has to resolve to something the build contains.
	TestTrue(TEXT("the .sfd registry landed in the project's resolved-asset pool"),
		Project->ResolvedAssets.Contains(TEXT("asset_image_900")));

	CleanUp();
	return true;
}

// ============================================================================
// The Blueprint surface
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetBlueprintSurfaceTest,
	"StoryFlow.DataAssets.Save.BlueprintSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("the test world came up"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	// SetProject installs the subsystem's own seed — the store a Blueprint call reaches
	W.Subsystem->SetProject(Project);

	UStoryFlowDataAssetAsset* Base = Project->DataAssets.FindRef(BaseId);
	UStoryFlowDataAssetAsset* Child = Project->DataAssets.FindRef(ChildId);
	UStoryFlowDataAssetAsset* GrandChild = Project->DataAssets.FindRef(GrandChildId);
	if (!TestTrue(TEXT("all three asset objects imported"), Base && Child && GrandChild))
	{
		CleanUp();
		return false;
	}

	bool bFound = false;

	// --- Reads go through the RESOLVER: inherited defaults and ancestor overrides included ---
	TestTrue(TEXT("a boolean inherited from the base reads through the child"), W.Component->GetDataAssetBoolVariable(Child, TEXT("alive"), bFound));
	TestTrue(TEXT("and reports found"), bFound);
	TestEqual(TEXT("the child's own file override wins over the base's value"), W.Component->GetDataAssetIntVariable(Child, TEXT("hp"), bFound), 150);
	TestEqual(TEXT("the base keeps its own declared value"), W.Component->GetDataAssetIntVariable(Base, TEXT("hp"), bFound), 100);
	TestNearlyEqual(TEXT("a float reads through the root's own override"), W.Component->GetDataAssetFloatVariable(Base, TEXT("speed"), bFound), 2.25f, 1.e-4f);
	TestEqual(TEXT("an enum reads through the child's override"), W.Component->GetDataAssetEnumVariable(Child, TEXT("rank"), bFound), TEXT("Elite"));
	TestEqual(TEXT("a string reads from the base"), W.Component->GetDataAssetStringVariable(Base, TEXT("title"), bFound), TEXT("Grunt"));
	// Root-most declaration owns the slot (§4.3): the grandchild re-declares `title` under the
	// SAME id with a different value, and the base's wins. This is the shared chain walk, not the
	// name lookup's tiebreak — with one id there is nothing for a name to disambiguate. That
	// tiebreak (one name, two different ids) needs a seed the editor cannot author and is pinned
	// in StoryFlow.DataAssets.Resolution.NameLookup.
	TestEqual(TEXT("a re-declared id still resolves to the ROOT-most declaration"),
		W.Component->GetDataAssetStringVariable(GrandChild, TEXT("title"), bFound), TEXT("Grunt"));
	// image / character / audio are declared distinctly and stored as plain strings
	TestEqual(TEXT("an image variable reads through the string accessor"),
		W.Component->GetDataAssetStringVariable(Base, TEXT("portrait"), bFound), TEXT("images/creatures/grunt.png"));
	TestEqual(TEXT("and the grandchild's override of it"),
		W.Component->GetDataAssetStringVariable(GrandChild, TEXT("portrait"), bFound), TEXT("images/creatures/chieftain.png"));

	// --- Writes land at the referenced asset's own level and cascade ---
	TestTrue(TEXT("a Blueprint write to the child lands"), W.Component->SetDataAssetIntVariable(Child, TEXT("hp"), 7));
	TestEqual(TEXT("and the next read sees it, not a cached value"), W.Component->GetDataAssetIntVariable(Child, TEXT("hp"), bFound), 7);
	TestEqual(TEXT("it cascades to the grandchild, which does not override hp"), W.Component->GetDataAssetIntVariable(GrandChild, TEXT("hp"), bFound), 7);
	TestEqual(TEXT("and leaves the base alone — the write landed at the CHILD"), W.Component->GetDataAssetIntVariable(Base, TEXT("hp"), bFound), 100);

	TestTrue(TEXT("a Blueprint write to the base lands"), W.Component->SetDataAssetBoolVariable(Base, TEXT("alive"), false));
	TestFalse(TEXT("and cascades all the way to the grandchild"), W.Component->GetDataAssetBoolVariable(GrandChild, TEXT("alive"), bFound));
	TestTrue(TEXT("which still reports found"), bFound);

	// A Blueprint write is the SAME overlay the graph reads — same subsystem-owned store
	const StoryFlowDataAssets::FSeed& Seed = W.Subsystem->GetDataAssetSeed();
	TestEqual(TEXT("a Blueprint write is visible to the resolver the nodes use"),
		StoryFlowDataAssets::Resolve(Seed, W.Subsystem->GetDataAssetOverlay(), ChildId, HpId).GetInt(), 7);
	// ...and therefore rides a save
	const TSharedPtr<FJsonObject> Key = SavedDataAssetKey(SerializeOverlayOnly(Seed, W.Subsystem->GetDataAssetOverlay()));
	if (TestTrue(TEXT("the save carries a dataAssets key"), Key.IsValid()))
	{
		const TSharedPtr<FJsonObject>* ChildEntry = nullptr;
		if (TestTrue(TEXT("a Blueprint write reaches the save key"), Key->TryGetObjectField(FString(ChildId), ChildEntry)))
		{
			TestEqual(TEXT("with its value"), static_cast<int32>((*ChildEntry)->GetNumberField(FString(HpId))), 7);
		}
	}

	// --- Arrays and maps ride the variant library ---
	const FStoryFlowVariant Tags = W.Component->GetDataAssetVariantVariable(Child, TEXT("tags"), bFound);
	TestTrue(TEXT("an array variable is found through the variant accessor"), bFound);
	if (TestEqual(TEXT("and carries the child's override"), Tags.GetArray().Num(), 2))
	{
		TestEqual(TEXT("in order"), Tags.GetArray()[1].GetString(), TEXT("elite"));
	}
	const FStoryFlowVariant Loot = W.Component->GetDataAssetVariantVariable(GrandChild, TEXT("loot"), bFound);
	TestTrue(TEXT("a map variable is found through the variant accessor"), bFound);
	TestTrue(TEXT("and is a map"), Loot.IsMap());

	// --- Failures are REPORTED, never latched, never guessed ---
	TestEqual(TEXT("an unknown variable name reads the type default"), W.Component->GetDataAssetIntVariable(Child, TEXT("nope"), bFound), 0);
	TestFalse(TEXT("and reports not found"), bFound);
	TestFalse(TEXT("a write to an unknown variable name is refused"), W.Component->SetDataAssetIntVariable(Child, TEXT("nope"), 1));

	// NO COERCION: a float read as an integer, an enum read as a string, an array read as a scalar
	TestEqual(TEXT("reading a float through the integer accessor is not found"), W.Component->GetDataAssetIntVariable(Base, TEXT("speed"), bFound), 0);
	TestFalse(TEXT("and reports it"), bFound);
	TestEqual(TEXT("reading an enum through the string accessor is not found"), W.Component->GetDataAssetEnumVariable(Base, TEXT("title"), bFound), FString());
	TestFalse(TEXT("and reports it too"), bFound);
	W.Component->GetDataAssetStringVariable(Child, TEXT("rank"), bFound);
	TestFalse(TEXT("nor does an enum come back through the string accessor"), bFound);
	W.Component->GetDataAssetStringVariable(Child, TEXT("tags"), bFound);
	TestFalse(TEXT("nor an array through a scalar accessor"), bFound);
	TestFalse(TEXT("and a mistyped write is refused rather than coerced"), W.Component->SetDataAssetFloatVariable(Base, TEXT("hp"), 1.5f));
	TestEqual(TEXT("so the value it would have clobbered is intact"), W.Component->GetDataAssetIntVariable(Base, TEXT("hp"), bFound), 100);

	// A category row is declared but valueless — it must behave as if it were not declared
	W.Component->GetDataAssetStringVariable(Base, TEXT("lore"), bFound);
	TestFalse(TEXT("a category row never resolves"), bFound);

	// A null asset reference is a Blueprint's most likely mistake and must not crash
	TestFalse(TEXT("a null asset reads as not found"), W.Component->GetDataAssetBoolVariable(nullptr, TEXT("alive"), bFound));
	TestFalse(TEXT("and reports it"), bFound);
	TestFalse(TEXT("a null asset write is refused"), W.Component->SetDataAssetBoolVariable(nullptr, TEXT("alive"), true));

	// --- The enumeration door (design 2026-09-04) ---
	//
	// The WALK is pinned by StoryFlow.DataAssets.Resolution's own coverage; what belongs HERE is
	// that the Blueprint door reaches that same walk, because until this existed a Blueprint could
	// only enumerate by re-walking Parent itself.
	{
		const TArray<FString> Walk = StoryFlowDataAssets::VariableNames(W.Subsystem->GetDataAssetSeed(), BaseId);
		const TArray<FString> Door = W.Component->GetDataAssetVariableNames(Base);
		if (TestTrue(TEXT("the fixture chain declares something to list"), Walk.Num() > 0))
		{
			TestEqual(TEXT("the door answers the shared walk verbatim"), Door, Walk);
		}

		// Root-most FIRST, and an override adds no name: a child only shadows values.
		const TArray<FString> ChildNames = W.Component->GetDataAssetVariableNames(Child);
		if (ChildNames.Num() > 0 && Walk.Num() > 0)
		{
			TestEqual(TEXT("a child's list still opens with the root's first declaration"), ChildNames[0], Walk[0]);
		}
		TestTrue(TEXT("a child never loses an inherited name"), ChildNames.Num() >= Walk.Num());

		// A category row is valueless and never enters the resolvable surface, so it cannot ride
		// the list either — the same rule the read above pins from the other side.
		TestFalse(TEXT("no category row rides the list"), Door.Contains(TEXT("lore")));

		TestEqual(TEXT("a null asset enumerates to empty rather than crashing"),
			W.Component->GetDataAssetVariableNames(nullptr).Num(), 0);
	}

	// --- Finding an asset by id or by the name an author typed (design 2026-09-04) ---
	{
		TestEqual(TEXT("an id resolves to the asset object"), W.Subsystem->FindDataAsset(BaseId), Base);
		TestEqual(TEXT("and so does its unambiguous display name"),
			W.Subsystem->FindDataAsset(Base->Name), Base);
		TestNull(TEXT("an unknown string resolves to nothing"), W.Subsystem->FindDataAsset(TEXT("NotAnAsset")));
		TestNull(TEXT("an empty string resolves to nothing"), W.Subsystem->FindDataAsset(FString()));

		// The found asset is usable, which is the whole point of handing one back.
		TestEqual(TEXT("the found asset reads through the ordinary accessors"),
			W.Component->GetDataAssetIntVariable(W.Subsystem->FindDataAsset(Base->Name), TEXT("hp"), bFound), 100);
		TestTrue(TEXT("and reports found"), bFound);

		// AMBIGUITY: two assets, one display name. Neither wins, and the log says why.
		AddExpectedError(TEXT("is ambiguous"), EAutomationExpectedErrorFlags::Contains, 0);
		const FString TwinName = TEXT("Twin");
		const FString SavedBaseName = Base->Name;
		const FString SavedChildName = Child->Name;
		Base->Name = TwinName;
		Child->Name = TwinName;
		TestNull(TEXT("an ambiguous display name resolves to nothing rather than picking"),
			W.Subsystem->FindDataAsset(TwinName));
		TestEqual(TEXT("while each id still resolves"), W.Subsystem->FindDataAsset(ChildId), Child);
		Base->Name = SavedBaseName;
		Child->Name = SavedChildName;
		TestEqual(TEXT("and the unambiguous name resolves again once the clash is gone"),
			W.Subsystem->FindDataAsset(SavedBaseName), Base);
	}

	CleanUp();
	return true;
}

// ============================================================================
// The REAL slot pair, and the dialogue counter that gates it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSlotRoundTripTest,
	"StoryFlow.DataAssets.Save.SlotRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetSlotRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	// Every other test in this file drives SerializeSaveData / DeserializeSaveData directly.
	// This one walks the pair a game actually calls — SaveToSlot / LoadFromSlot — because that is
	// where the overlay meets the subsystem's own state, its active-dialogue gate and the slot.
	FScopedWorld W;
	if (!TestTrue(TEXT("the test world came up"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	W.Subsystem->SetProject(Project);

	UStoryFlowDataAssetAsset* Child = Project->DataAssets.FindRef(ChildId);
	if (!TestNotNull(TEXT("the child asset imported"), Child))
	{
		CleanUp();
		return false;
	}

	bool bFound = false;
	TestTrue(TEXT("a session write lands"), W.Component->SetDataAssetIntVariable(Child, TEXT("hp"), 77));
	TestEqual(TEXT("and reads back before the save"), W.Component->GetDataAssetIntVariable(Child, TEXT("hp"), bFound), 77);

	TestTrue(TEXT("SaveToSlot succeeds with no dialogue running"), W.Subsystem->SaveToSlot(SlotName, 0));

	// Wipe the session the way a game restart does, then load it back
	W.Subsystem->ResetDataAssetOverlay();
	TestEqual(TEXT("the reset dropped the write back to the file value"),
		W.Component->GetDataAssetIntVariable(Child, TEXT("hp"), bFound), 150);

	TestTrue(TEXT("LoadFromSlot succeeds with no dialogue running"), W.Subsystem->LoadFromSlot(SlotName, 0));
	TestEqual(TEXT("and the .sfd write came back through the real slot"),
		W.Component->GetDataAssetIntVariable(Child, TEXT("hp"), bFound), 77);
	TestTrue(TEXT("as a found value, not a default"), bFound);

	// The cascade survives the trip too: the grandchild does not override hp
	TestEqual(TEXT("the loaded write still cascades to the grandchild"),
		StoryFlowDataAssets::Resolve(W.Subsystem->GetDataAssetSeed(), W.Subsystem->GetDataAssetOverlay(), GrandChildId, HpId).GetInt(), 77);

	UGameplayStatics::DeleteGameInSlot(SlotName, 0);
	CleanUp();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetRestartKeepsLoadingTest,
	"StoryFlow.DataAssets.Save.RestartKeepsLoadingPossible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDataAssetRestartKeepsLoadingTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDataAssetSaveTestHelpers;

	// ActiveDialogueCount is the ONLY gate on LoadFromSlot, and it is a count across components.
	// Restarting a dialogue does not stop the old one (no end event fires, by design), so an
	// unconditional increment on every start leaves the count stuck above zero after the single
	// stop that follows — and every load for the rest of the session is refused. The notify pair
	// is idempotent per component precisely so that cannot happen.
	FScopedWorld W;
	if (!TestTrue(TEXT("the test world came up"), W.Init()))
	{
		return false;
	}

	StoryFlowDataAssets::FSeed ImportedSeed;
	UStoryFlowProjectAsset* Project = ImportFixtureProject(*this, ImportedSeed);
	if (!Project)
	{
		return false;
	}
	Project->Scripts.Add(TEXT("waiting"), MakeWaitingScript());
	W.Subsystem->SetProject(Project);

	TestTrue(TEXT("SaveToSlot succeeds before any dialogue"), W.Subsystem->SaveToSlot(SlotName, 0));

	W.Component->StartDialogueWithScript(TEXT("waiting"));
	TestTrue(TEXT("a running dialogue is reported active"), W.Subsystem->IsDialogueActive());
	// The refusal logs an Error by design (a game asking to load mid-dialogue has a bug), so the
	// harness is told to expect exactly one rather than failing on it.
	AddExpectedError(TEXT("Cannot load while a dialogue is active"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("and it refuses a load, which is the whole point of the counter"),
		W.Subsystem->LoadFromSlot(SlotName, 0));

	// THE RESTART. Same component, second start, no stop in between.
	W.Component->StartDialogueWithScript(TEXT("waiting"));
	TestTrue(TEXT("the restarted dialogue is still just one active dialogue"), W.Subsystem->IsDialogueActive());

	W.Component->StopDialogue();
	TestFalse(TEXT("one stop ends it, however many times it was started"), W.Subsystem->IsDialogueActive());
	TestTrue(TEXT("so loading works again after a restart"), W.Subsystem->LoadFromSlot(SlotName, 0));

	// And a second stop must not push the count below zero into a state where a LATER dialogue
	// cannot gate a load at all.
	W.Component->StopDialogue();
	W.Component->StartDialogueWithScript(TEXT("waiting"));
	TestTrue(TEXT("a fresh dialogue after all that still gates loading"), W.Subsystem->IsDialogueActive());
	W.Component->StopDialogue();
	TestFalse(TEXT("and still releases it"), W.Subsystem->IsDialogueActive());

	UGameplayStatics::DeleteGameInSlot(SlotName, 0);
	CleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
