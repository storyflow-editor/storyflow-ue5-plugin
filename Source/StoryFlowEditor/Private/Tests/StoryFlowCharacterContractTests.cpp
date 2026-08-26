// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Import/StoryFlowImporter.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StoryFlowCharacterIndexFixture.h"
#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowRuntime.h"
#include "StoryFlowScopedWorld.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

/**
 * The P4 §5 CHARACTER GOLDEN PACKAGE reader: TestContent/character-contract/, vendored
 * byte-identical from the editor repo (generated from the HTML runtime, the contract's
 * reference implementation). The manifest's _comment is the reader contract this file
 * implements: iterate caseFiles -> cases in order, dispatch on kind, honor excluded[unreal]
 * as skip-as-data, invert expect_fail, and assert exactly case_count cases were consumed.
 *
 * The four inputs are written as a REAL build folder and imported through the REAL importer.
 * Surfaces per the §9 entry 6 (A5) seats for this engine:
 *  - resolved `value` expectations run against the RESOLVING doors — the component's
 *    character variable doors (Name resolves through the string table there) and, for the
 *    string family the importer resolves EAGERLY at store seeding, the node lane too;
 *  - `stored` expectations (string-table keys / asset ids, byte promises) run against the
 *    PRE-RESOLUTION record data (the imported character asset) and the surfaces that answer
 *    the stored key verbatim (the node-lane Name read, the Image doors);
 *  - the builtin Image `value` is the assets-table path the stored id maps to, asserted
 *    through the vendored assets table itself (this engine resolves the id to a texture,
 *    not a path string — the table is the path authority the import consumed).
 *
 * Run via: Session Frontend > Automation > "StoryFlow.CharacterContract", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.CharacterContract" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowCharacterContractTestHelpers
{
	using namespace StoryFlowCharacterIndexTestHelpers; // MakeNode / MakeEdge / MakeCharSetter
	using StoryFlowEngineContract::JsonEquals;
	using StoryFlowEngineContract::VariantMatchesJson;
	using StoryFlowTestWorld::FScopedWorld;

	const TCHAR* ContractTestRoot = TEXT("/Game/StoryFlowCharacterContractTests");
	const TCHAR* ContractSlotName = TEXT("StoryFlowCharacterContractSlot");

	FString PackageFixturePath(const FString& FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("StoryFlowPlugin"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("TestContent"), TEXT("character-contract"), FileName);
	}

	TSharedPtr<FJsonObject> LoadPackageFile(const FString& FileName)
	{
		FString JsonString;
		if (!FFileHelper::LoadFileToString(JsonString, *PackageFixturePath(FileName)))
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

	FString ContractBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowCharacterContract"));
	}

	/** The vendored inputs, VERBATIM, as a build folder (script.json becomes the one script). */
	bool WriteContractBuildDir()
	{
		const FString Dir = ContractBuildDir();
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		bool bOk = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")));
		const TCHAR* Verbatim[] = { TEXT("characters.json"), TEXT("character-index.json"), TEXT("data-assets.json") };
		for (const TCHAR* FileName : Verbatim)
		{
			FString Body;
			bOk = bOk && FFileHelper::LoadFileToString(Body, *PackageFixturePath(FileName))
				&& FFileHelper::SaveStringToFile(Body, *FPaths::Combine(Dir, FileName));
		}
		FString ScriptBody;
		bOk = bOk && FFileHelper::LoadFileToString(ScriptBody, *PackageFixturePath(TEXT("script.json")))
			&& FFileHelper::SaveStringToFile(ScriptBody, *FPaths::Combine(Dir, TEXT("main.json")));
		return bOk;
	}

	void ContractCleanUp()
	{
		UGameplayStatics::DeleteGameInSlot(ContractSlotName, 0);
		UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
		IFileManager::Get().DeleteDirectory(*ContractBuildDir(), false, true);
	}

	FString JsonStr(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	/**
	 * The whole package run, one store-owning world. Every method asserts through Test so a
	 * failing case names itself; Consumed only advances for cases that were actually visited.
	 */
	struct FContractHarness
	{
		FAutomationTestBase& Test;
		FScopedWorld& W;
		UStoryFlowProjectAsset* Project = nullptr;
		/** The vendored assets table: asset id -> exported path ("images/hero.png"). */
		TMap<FString, FString> AssetPaths;
		int32 ScriptCounter = 0;
		int32 RunCases = 0;
		int32 SkippedExcluded = 0;
		int32 ExpectFailInverted = 0;
		TMap<FString, int32> PerKind;

		FContractHarness(FAutomationTestBase& InTest, FScopedWorld& InWorld)
			: Test(InTest), W(InWorld)
		{
		}

		FString NextScriptName()
		{
			return FString::Printf(TEXT("cc_script_%d"), ++ScriptCounter);
		}

		/** assets-table path for a stored asset id; identity for anything the table does not carry
		    (an empty stored image resolves to an empty path, exactly the reference shape). */
		FString AssetPathOrSelf(const FString& AssetId) const
		{
			const FString* Path = AssetPaths.Find(AssetId);
			return Path ? *Path : AssetId;
		}

		FString ResolveStoredRecordKey(const FString& CharacterId, const FString& CharacterPath) const
		{
			if (!CharacterId.IsEmpty())
			{
				if (const FString* RecordKey = W.Subsystem->GetCharacterIdToPath().Find(CharacterId))
				{
					return *RecordKey;
				}
			}
			return CharacterPath;
		}

		/** The PRE-RESOLUTION record data: the imported character asset, keys/ids verbatim. */
		FStoryFlowVariant RawStored(const FString& RecordKey, const FString& VariableName) const
		{
			FStoryFlowVariant Out;
			UStoryFlowCharacterAsset* CharAsset = Project ? Project->Characters.FindRef(RecordKey) : nullptr;
			if (!CharAsset)
			{
				return Out;
			}
			if (IsCharacterNameBuiltin(VariableName))
			{
				Out.SetString(CharAsset->Name);
			}
			else if (IsCharacterImageBuiltin(VariableName))
			{
				Out.SetString(CharAsset->Image);
			}
			else if (const FStoryFlowVariable* Variable = CharAsset->Variables.Find(VariableName))
			{
				Out = Variable->Value;
			}
			return Out;
		}

		/** The public component door — the RESOLVING surface (Name resolves, Image answers the id). */
		FStoryFlowVariant HostRead(const FString& CharacterId, const FString& CharacterPath, const FString& VariableName)
		{
			return CharacterId.IsEmpty()
				? W.Component->GetCharacterVariable(CharacterPath, VariableName)
				: W.Component->GetCharacterVariableById(CharacterId, VariableName);
		}

		void HostWrite(const FString& CharacterId, const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value)
		{
			if (CharacterId.IsEmpty())
			{
				W.Component->SetCharacterVariable(CharacterPath, VariableName, Value);
			}
			else
			{
				W.Component->SetCharacterVariableById(CharacterId, VariableName, Value);
			}
		}

		/** Fixture JSON value -> the variant a case's declared variable types it as. */
		FStoryFlowVariant VariantFromCase(const TSharedPtr<FJsonObject>& VariableObj, const TSharedPtr<FJsonValue>& ValueJson) const
		{
			FStoryFlowVariant Out;
			const FString Type = JsonStr(VariableObj, TEXT("type"));
			bool bIsArray = false;
			VariableObj->TryGetBoolField(TEXT("isArray"), bIsArray);

			if (bIsArray)
			{
				TArray<FStoryFlowVariant> Items;
				for (const TSharedPtr<FJsonValue>& Item : ValueJson->AsArray())
				{
					FStoryFlowVariant Element;
					Element.SetString(Item->AsString());
					Items.Add(Element);
				}
				Out.SetArray(Items, EStoryFlowVariableType::String);
			}
			else if (Type == TEXT("map"))
			{
				TArray<FStoryFlowMapEntry> Entries;
				for (const TSharedPtr<FJsonValue>& EntryValue : ValueJson->AsArray())
				{
					const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
					if (!EntryObject.IsValid())
					{
						continue;
					}
					FStoryFlowMapEntry Entry;
					Entry.Key.SetString(EntryObject->GetStringField(TEXT("key")));
					Entry.Value.SetString(EntryObject->GetStringField(TEXT("value")));
					Entries.Add(Entry);
				}
				Out.SetMap(Entries);
			}
			else if (Type == TEXT("boolean"))
			{
				Out.SetBool(ValueJson->AsBool());
			}
			else if (Type == TEXT("integer"))
			{
				Out.SetInt(static_cast<int32>(ValueJson->AsNumber()));
			}
			else if (Type == TEXT("float"))
			{
				Out.SetFloat(static_cast<float>(ValueJson->AsNumber()));
			}
			else
			{
				Out.SetString(ValueJson->AsString());
			}
			return Out;
		}

		/**
		 * The NODE LANE, driven the way an authored graph drives it: a getCharacterVar node
		 * carrying the case's (id, path) binding exactly as vendored — decoy path included —
		 * evaluated through the real evaluator against the subsystem's store. Array and map
		 * reads pull through a consumer's wired input handle, like the resolution suite's.
		 * WiredCharacterRef, when set, wires a string source onto the character input pin.
		 */
		FStoryFlowVariant NodeLaneRead(const FString& CharacterId, const FString& CharacterPath,
			const TSharedPtr<FJsonObject>& VariableObj, const FString& WiredCharacterRef = FString(), int32* OutWarnsEmitted = nullptr)
		{
			const FString Name = JsonStr(VariableObj, TEXT("name"));
			const FString Type = JsonStr(VariableObj, TEXT("type"));
			const FString KeyType = JsonStr(VariableObj, TEXT("keyType"));
			const FString ValueType = JsonStr(VariableObj, TEXT("valueType"));
			bool bIsArray = false;
			VariableObj->TryGetBoolField(TEXT("isArray"), bIsArray);

			UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
			FGCObjectScopeGuard ScriptGuard(Script);
			Script->StartNode = TEXT("0");
			Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
			{
				FStoryFlowNode G = MakeNode(TEXT("G"), EStoryFlowNodeType::GetCharacterVar, TEXT("getCharacterVar"));
				G.Data.CharacterId = CharacterId;
				G.Data.CharacterPath = CharacterPath;
				G.Data.VariableName = Name;
				G.Data.VariableType = Type;
				G.Data.bIsArray = bIsArray;
				G.Data.KeyType = KeyType;
				G.Data.ValueType = ValueType;
				Script->Nodes.Add(G.Id, G);
			}
			if (bIsArray)
			{
				FStoryFlowNode CA = MakeNode(TEXT("CA"), EStoryFlowNodeType::SetBool, TEXT("setBool"));
				Script->Nodes.Add(CA.Id, CA);
				Script->Connections.Add(MakeEdge(TEXT("G"), TEXT("CA"),
					StoryFlowHandles::Source(TEXT("G"), Type + TEXT("-array-")),
					StoryFlowHandles::Target(TEXT("CA"), Type + TEXT("-array-"))));
			}
			else if (Type == TEXT("map"))
			{
				FStoryFlowNode CM = MakeNode(TEXT("CM"), EStoryFlowNodeType::SetBool, TEXT("setBool"));
				CM.Data.KeyType = KeyType;
				CM.Data.ValueType = ValueType;
				Script->Nodes.Add(CM.Id, CM);
				Script->Connections.Add(MakeEdge(TEXT("G"), TEXT("CM"),
					StoryFlowHandles::Source(TEXT("G"), FString::Printf(TEXT("map-%s-%s"), *KeyType, *ValueType)),
					StoryFlowHandles::Target(TEXT("CM"), StoryFlowHandles::In_Map(KeyType, ValueType, TEXT("1")))));
			}
			if (!WiredCharacterRef.IsEmpty())
			{
				FStoryFlowNode VI = MakeNode(TEXT("VI"), EStoryFlowNodeType::GetString, TEXT("getString"));
				VI.Data.Variable = TEXT("wv");
				Script->Nodes.Add(VI.Id, VI);
				Script->Connections.Add(MakeEdge(TEXT("VI"), TEXT("G"),
					StoryFlowHandles::Source(TEXT("VI"), TEXT("string-")),
					StoryFlowHandles::Target(TEXT("G"), StoryFlowHandles::In_CharacterInput)));
			}
			Script->BuildConnectionIndices();

			FStoryFlowExecutionContext Context;
			Context.CurrentScript = Script;
			Context.ExternalCharacters = &W.Subsystem->GetRuntimeCharacters();
			Context.CharacterIdToPath = &W.Subsystem->GetCharacterIdToPath();
			if (!WiredCharacterRef.IsEmpty())
			{
				FStoryFlowVariable Wired;
				Wired.Id = TEXT("wv");
				Wired.Name = TEXT("WiredCharacter");
				Wired.Type = EStoryFlowVariableType::String;
				Wired.Value.SetString(WiredCharacterRef);
				Context.LocalVariables.Add(Wired.Id, Wired);
			}
			FStoryFlowEvaluator Evaluator(&Context);

			FStoryFlowVariant Out;
			if (bIsArray)
			{
				Out.SetArray(Evaluator.EvaluateStringArrayInput(Context.GetNode(TEXT("CA")), Type + TEXT("-array-")), EStoryFlowVariableType::String);
			}
			else if (Type == TEXT("map"))
			{
				const TArray<FStoryFlowMapEntry>* Entries = Evaluator.EvaluateMapInput(Context.GetNode(TEXT("CM")), TEXT("1"));
				Out.SetMap(Entries ? *Entries : TArray<FStoryFlowMapEntry>());
			}
			else if (Type == TEXT("boolean"))
			{
				Out.SetBool(Evaluator.EvaluateBooleanFromNode(Context.GetNode(TEXT("G")), TEXT(""), TEXT("")));
			}
			else if (Type == TEXT("integer"))
			{
				Out.SetInt(Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("G")), TEXT(""), TEXT("")));
			}
			else if (Type == TEXT("float"))
			{
				Out.SetFloat(Evaluator.EvaluateFloatFromNode(Context.GetNode(TEXT("G")), TEXT(""), TEXT("")));
			}
			else
			{
				Out.SetString(Evaluator.EvaluateStringFromNode(Context.GetNode(TEXT("G")), TEXT(""), TEXT("")));
			}
			if (OutWarnsEmitted)
			{
				*OutWarnsEmitted = Context.CharacterIdWarningsEmitted;
			}
			return Out;
		}

		/** The node WRITE lane: a real setCharacterVar run through the component executor.
		    Map values ride a wired map input (the setter snapshots its wire, like the runtime);
		    everything else is the node's inline value, exactly the export dialect. */
		void NodeLaneWrite(const FString& CharacterId, const FString& CharacterPath,
			const TSharedPtr<FJsonObject>& VariableObj, const TSharedPtr<FJsonValue>& ValueJson, const FString& WiredCharacterRef = FString())
		{
			const FString Name = JsonStr(VariableObj, TEXT("name"));
			const FString Type = JsonStr(VariableObj, TEXT("type"));
			const FString KeyType = JsonStr(VariableObj, TEXT("keyType"));
			const FString ValueType = JsonStr(VariableObj, TEXT("valueType"));
			bool bIsArray = false;
			VariableObj->TryGetBoolField(TEXT("isArray"), bIsArray);

			const FString ScriptName = NextScriptName();
			UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
			Project->Scripts.Add(ScriptName, Script); // hard ref keeps it alive
			Script->StartNode = TEXT("0");
			Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

			FStoryFlowNode S = MakeCharSetter(TEXT("S"), *CharacterId, *CharacterPath, *Name, *Type, bIsArray);
			if (Type == TEXT("map"))
			{
				S.Data.KeyType = KeyType;
				S.Data.ValueType = ValueType;
				FStoryFlowVariable MapSeed;
				MapSeed.Id = TEXT("mv");
				MapSeed.Name = TEXT("MapSeed");
				MapSeed.Type = EStoryFlowVariableType::Map;
				MapSeed.KeyType = EStoryFlowVariableType::String;
				MapSeed.ValueType = EStoryFlowVariableType::String;
				MapSeed.Value = VariantFromCase(VariableObj, ValueJson);
				Script->Variables.Add(MapSeed.Id, MapSeed);

				FStoryFlowNode GM = MakeNode(TEXT("GM"), EStoryFlowNodeType::GetMap, TEXT("getMap"));
				GM.Data.Variable = TEXT("mv");
				Script->Nodes.Add(GM.Id, GM);
				Script->Connections.Add(MakeEdge(TEXT("GM"), TEXT("S"),
					StoryFlowHandles::Source(TEXT("GM"), FString::Printf(TEXT("map-%s-%s"), *KeyType, *ValueType)),
					StoryFlowHandles::Target(TEXT("S"), StoryFlowHandles::In_Map(KeyType, ValueType, TEXT("input")))));
			}
			else
			{
				// Inline value, the export dialect: an inline string routes through the string
				// table and falls back to the literal — runtime-written strings are literal (A5).
				S.Data.Value = VariantFromCase(VariableObj, ValueJson);
			}
			Script->Nodes.Add(S.Id, S);
			if (!WiredCharacterRef.IsEmpty())
			{
				FStoryFlowVariable Wired;
				Wired.Id = TEXT("wv");
				Wired.Name = TEXT("WiredCharacter");
				Wired.Type = EStoryFlowVariableType::String;
				Wired.Value.SetString(WiredCharacterRef);
				Script->Variables.Add(Wired.Id, Wired);

				FStoryFlowNode VI = MakeNode(TEXT("VI"), EStoryFlowNodeType::GetString, TEXT("getString"));
				VI.Data.Variable = TEXT("wv");
				Script->Nodes.Add(VI.Id, VI);
				Script->Connections.Add(MakeEdge(TEXT("VI"), TEXT("S"),
					StoryFlowHandles::Source(TEXT("VI"), TEXT("string-")),
					StoryFlowHandles::Target(TEXT("S"), StoryFlowHandles::In_CharacterInput)));
			}
			Script->Nodes.Add(TEXT("End"), MakeNode(TEXT("End"), EStoryFlowNodeType::End, TEXT("end")));
			Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("S"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("S"), TEXT("0"))));
			Script->Connections.Add(MakeEdge(TEXT("S"), TEXT("End"), StoryFlowHandles::Source(TEXT("S"), StoryFlowHandles::Out_Flow), StoryFlowHandles::Target(TEXT("End"), TEXT(""))));
			Script->BuildConnectionIndices();

			W.Component->StartDialogueWithScript(ScriptName);
		}

		/** The SPEAKER surface: a one-line dialogue bound (characterRefId, character) as authored. */
		FString SpeakerName(const FString& CharacterRefId, const FString& CharacterField)
		{
			const FString ScriptName = NextScriptName();
			UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
			Project->Scripts.Add(ScriptName, Script);
			Script->StartNode = TEXT("0");
			Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
			{
				FStoryFlowNode D = MakeNode(TEXT("D"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
				D.Data.Text = TEXT("line");
				D.Data.Character = CharacterField;
				D.Data.CharacterRefId = CharacterRefId;
				Script->Nodes.Add(D.Id, D);
			}
			Script->Connections.Add(MakeEdge(TEXT("0"), TEXT("D"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("D"))));
			Script->BuildConnectionIndices();

			W.Component->StartDialogueWithScript(ScriptName);
			const FString Name = W.Component->GetCurrentDialogue().Character.Name;
			W.Component->StopDialogue();
			return Name;
		}

		// ====================================================================
		// Kind processors
		// ====================================================================

		void ProcessRead(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const TSharedPtr<FJsonObject> Ref = Case->GetObjectField(TEXT("ref"));
			const TSharedPtr<FJsonObject> VariableObj = Case->GetObjectField(TEXT("variable"));
			const TSharedPtr<FJsonObject> Expected = Case->GetObjectField(TEXT("expected"));
			const FString Id = JsonStr(Ref, TEXT("characterId"));
			const FString Path = JsonStr(Ref, TEXT("characterPath"));
			const FString Name = JsonStr(VariableObj, TEXT("name"));
			const FString Type = JsonStr(VariableObj, TEXT("type"));
			const TSharedPtr<FJsonValue> ExpectedValue = Expected->TryGetField(TEXT("value"));
			const TSharedPtr<FJsonValue> ExpectedStored = Expected->TryGetField(TEXT("stored"));
			bool bExpectFail = false;
			Case->TryGetBoolField(TEXT("expect_fail"), bExpectFail);

			const FStoryFlowVariant Door = HostRead(Id, Path, Name);
			const FStoryFlowVariant NodeLane = NodeLaneRead(Id, Path, VariableObj);
			const FString RecordKey = ResolveStoredRecordKey(Id, Path);

			if (bExpectFail)
			{
				// The tripwire MUST be reported as failing — inverted here, so this harness
				// reports a pass exactly when the resolution does NOT match the deliberately
				// wrong (path-owner) expectation. A pass of the un-inverted comparison would
				// prove path-first resolution or a comparator that compares nothing.
				Test.TestTrue(CaseName + TEXT(": expect_fail reported as failing (resolved value mismatches the path-owner expectation)"),
					Door.GetString() != ExpectedValue->AsString());
				Test.TestTrue(CaseName + TEXT(": expect_fail reported as failing (stored byte mismatches the path-owner expectation)"),
					RawStored(RecordKey, Name).GetString() != ExpectedStored->AsString());
				++ExpectFailInverted;
				return;
			}

			if (Type == TEXT("image") || IsCharacterImageBuiltin(Name))
			{
				// stored is the characters.json asset id — the id-answering doors and the raw
				// record, byte-for-byte; value is the assets-table path that id maps to.
				const FString Stored = ExpectedStored->AsString();
				Test.TestEqual(CaseName + TEXT(": image door answers the stored asset id"), Door.GetString(), Stored);
				Test.TestEqual(CaseName + TEXT(": image node lane answers the stored asset id"), NodeLane.GetString(), Stored);
				Test.TestEqual(CaseName + TEXT(": raw record image is the stored asset id"), RawStored(RecordKey, Name).GetString(), Stored);
				Test.TestEqual(CaseName + TEXT(": the assets table maps the stored id to the expected path"),
					AssetPathOrSelf(Door.GetString()), ExpectedValue->AsString());
			}
			else if (IsCharacterNameBuiltin(Name))
			{
				// The component door RESOLVES the builtin Name; the node lane and the raw record
				// answer the stored string-table key verbatim (A5 seats for this engine).
				Test.TestEqual(CaseName + TEXT(": resolving door answers the resolved Name"), Door.GetString(), ExpectedValue->AsString());
				Test.TestEqual(CaseName + TEXT(": node lane answers the stored Name key"), NodeLane.GetString(), ExpectedStored->AsString());
				Test.TestEqual(CaseName + TEXT(": raw record Name is the stored key"), RawStored(RecordKey, Name).GetString(), ExpectedStored->AsString());
			}
			else
			{
				// Custom variables: the importer resolves the string family EAGERLY at store
				// seeding, so BOTH live surfaces answer the resolved value; stored bytes live
				// on the pre-resolution record only.
				VariantMatchesJson(Test, CaseName + TEXT(" (door)"), Door, ExpectedValue);
				VariantMatchesJson(Test, CaseName + TEXT(" (node lane)"), NodeLane, ExpectedValue);
				if (ExpectedStored.IsValid())
				{
					VariantMatchesJson(Test, CaseName + TEXT(" (raw record stored)"), RawStored(RecordKey, Name), ExpectedStored);
				}
			}
		}

		void ProcessSpeaker(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const TSharedPtr<FJsonObject> Ref = Case->GetObjectField(TEXT("ref"));
			const TSharedPtr<FJsonObject> Expected = Case->GetObjectField(TEXT("expected"));
			const FString RefId = JsonStr(Ref, TEXT("characterRefId"));
			const FString Decoy = JsonStr(Ref, TEXT("character"));
			const FString RecordKey = JsonStr(Expected, TEXT("recordKey"));
			const TSharedPtr<FJsonObject> ExpectedName = Expected->GetObjectField(TEXT("name"));
			const TSharedPtr<FJsonObject> ExpectedImage = Expected->GetObjectField(TEXT("image"));

			const FString SpokenName = SpeakerName(RefId, Decoy);
			Test.TestEqual(CaseName + TEXT(": speaker resolves the id's character name"), SpokenName, JsonStr(ExpectedName, TEXT("value")));

			const FStoryFlowCharacterDef* Record = W.Subsystem->GetRuntimeCharacters().Find(RecordKey);
			if (Test.TestNotNull(*(CaseName + TEXT(": the expected record key names a loaded record")), Record))
			{
				Test.TestEqual(CaseName + TEXT(": the record stores the Name key"), Record->Name, JsonStr(ExpectedName, TEXT("stored")));
				Test.TestEqual(CaseName + TEXT(": the record stores the image asset id"), Record->Image, JsonStr(ExpectedImage, TEXT("storedAssetId")));
				Test.TestEqual(CaseName + TEXT(": the assets table maps the image id to the expected path"),
					AssetPathOrSelf(Record->Image), JsonStr(ExpectedImage, TEXT("value")));
			}
			// The decoy proof: the path field named the OTHER character, whose name must differ.
			if (const FStoryFlowCharacterDef* DecoyRecord = W.Subsystem->GetRuntimeCharacters().Find(Decoy))
			{
				Test.TestTrue(CaseName + TEXT(": the decoy path's character was not the speaker"),
					Project->GetGlobalString(DecoyRecord->Name) != SpokenName);
			}
		}

		void ReplayReadSurface(const FString& Label, const TSharedPtr<FJsonObject>& Read, const TSharedPtr<FJsonObject>& VariableObj)
		{
			const FString Surface = JsonStr(Read, TEXT("surface"));
			const TSharedPtr<FJsonObject> Ref = Read->GetObjectField(TEXT("ref"));
			const FString Id = JsonStr(Ref, TEXT("characterId"));
			const FString Path = JsonStr(Ref, TEXT("characterPath"));
			const TSharedPtr<FJsonValue> Expected = Read->TryGetField(TEXT("expected"));
			const FString Name = JsonStr(VariableObj, TEXT("name"));

			if (Surface == TEXT("speaker"))
			{
				Test.TestEqual(Label + TEXT(" (speaker)"), SpeakerName(Id, TEXT("")), Expected->AsString());
			}
			else if (Surface == TEXT("host"))
			{
				VariantMatchesJson(Test, Label + TEXT(" (host)"), HostRead(Id, Path, Name), Expected);
			}
			else // node
			{
				FStoryFlowVariant Value = NodeLaneRead(Id, Path, VariableObj);
				if (IsCharacterNameBuiltin(Name))
				{
					// The node lane answers the runtime Name field verbatim; resolve it through
					// the engine's own string door before comparing (identity for literals).
					Value.SetString(Project->GetGlobalString(Value.GetString()));
				}
				VariantMatchesJson(Test, Label + TEXT(" (node)"), Value, Expected);
			}
		}

		void ProcessWrite(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const TSharedPtr<FJsonObject> Write = Case->GetObjectField(TEXT("write"));
			const TSharedPtr<FJsonObject> Ref = Write->GetObjectField(TEXT("ref"));
			const TSharedPtr<FJsonObject> VariableObj = Write->GetObjectField(TEXT("variable"));
			const FString Id = JsonStr(Ref, TEXT("characterId"));
			const FString Path = JsonStr(Ref, TEXT("characterPath"));
			const TSharedPtr<FJsonValue> ValueJson = Write->TryGetField(TEXT("value"));

			if (JsonStr(Write, TEXT("surface")) == TEXT("node"))
			{
				NodeLaneWrite(Id, Path, VariableObj, ValueJson);
			}
			else
			{
				HostWrite(Id, Path, JsonStr(VariableObj, TEXT("name")), VariantFromCase(VariableObj, ValueJson));
			}

			const TArray<TSharedPtr<FJsonValue>>* Reads = nullptr;
			if (Case->TryGetArrayField(TEXT("reads"), Reads))
			{
				for (const TSharedPtr<FJsonValue>& ReadValue : *Reads)
				{
					ReplayReadSurface(CaseName, ReadValue->AsObject(), VariableObj);
				}
			}
		}

		void ProcessSweep(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const TArray<TSharedPtr<FJsonValue>>& Resolutions = Case->GetArrayField(TEXT("resolutions"));
			for (const TSharedPtr<FJsonValue>& ResolutionValue : Resolutions)
			{
				const TSharedPtr<FJsonObject> Resolution = ResolutionValue->AsObject();
				const FString Id = JsonStr(Resolution, TEXT("characterId"));
				const TSharedPtr<FJsonObject> VariableObj = Resolution->GetObjectField(TEXT("variable"));
				const FString Name = JsonStr(VariableObj, TEXT("name"));
				const TSharedPtr<FJsonValue> Expected = Resolution->TryGetField(TEXT("value"));
				const FString Label = FString::Printf(TEXT("%s %s.%s"), *CaseName, *Id, *Name);

				VariantMatchesJson(Test, Label + TEXT(" (host)"), HostRead(Id, TEXT(""), Name), Expected);
				FStoryFlowVariant NodeLane = NodeLaneRead(Id, TEXT(""), VariableObj);
				if (IsCharacterNameBuiltin(Name))
				{
					NodeLane.SetString(Project->GetGlobalString(NodeLane.GetString()));
				}
				VariantMatchesJson(Test, Label + TEXT(" (node)"), NodeLane, Expected);
			}
		}

		TSharedPtr<FJsonObject> ReadSlotJson()
		{
			UStoryFlowSaveGame* Slot = Cast<UStoryFlowSaveGame>(UGameplayStatics::LoadGameFromSlot(ContractSlotName, 0));
			if (!Test.TestNotNull(TEXT("the slot reads back as a StoryFlow save"), Slot))
			{
				return nullptr;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Slot->SaveDataJson);
			FJsonSerializer::Deserialize(Reader, Root);
			return Root;
		}

		void ProcessSave(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			if (!Test.TestTrue(CaseName + TEXT(": SaveToSlot succeeds"), W.Subsystem->SaveToSlot(ContractSlotName, 0)))
			{
				return;
			}
			const TSharedPtr<FJsonObject> Root = ReadSlotJson();
			if (!Test.TestTrue(CaseName + TEXT(": the slot document parses"), Root.IsValid()))
			{
				return;
			}

			// The characters section against the reference shape. This engine's records are
			// TYPED (id/name/type/isArray/value) and its Name/Image fields hold stored keys —
			// compare each leaf through the engine's own resolving doors: names through the
			// string table (identity for runtime-written literals — A5: a save holds whatever
			// the runtime holds), images through the assets table, values structurally.
			const TSharedPtr<FJsonObject> ExpectedChars = Case->GetObjectField(TEXT("saveCharacters"));
			const TSharedPtr<FJsonObject>* SavedChars = nullptr;
			if (Test.TestTrue(CaseName + TEXT(": the save carries a characters object"), Root->TryGetObjectField(TEXT("characters"), SavedChars)))
			{
				Test.TestEqual(CaseName + TEXT(": record count"), (*SavedChars)->Values.Num(), ExpectedChars->Values.Num());
				for (const auto& ExpectedPair : ExpectedChars->Values)
				{
					const FString& RecordKey = ExpectedPair.Key;
					const TSharedPtr<FJsonObject> ExpectedRecord = ExpectedPair.Value->AsObject();
					const TSharedPtr<FJsonObject>* SavedRecord = nullptr;
					if (!Test.TestTrue(FString::Printf(TEXT("%s: the save carries '%s'"), *CaseName, *RecordKey),
						(*SavedChars)->TryGetObjectField(RecordKey, SavedRecord)))
					{
						continue;
					}
					const FString RecordLabel = CaseName + TEXT(" ") + RecordKey;
					Test.TestEqual(RecordLabel + TEXT(" name (resolved)"),
						Project->GetGlobalString(JsonStr(*SavedRecord, TEXT("name"))), JsonStr(ExpectedRecord, TEXT("name")));
					Test.TestEqual(RecordLabel + TEXT(" image (assets-table path)"),
						AssetPathOrSelf(JsonStr(*SavedRecord, TEXT("image"))), JsonStr(ExpectedRecord, TEXT("image")));

					const TSharedPtr<FJsonObject> ExpectedVars = ExpectedRecord->GetObjectField(TEXT("variables"));
					const TSharedPtr<FJsonObject>* SavedVars = nullptr;
					if (!Test.TestTrue(RecordLabel + TEXT(" carries variables"), (*SavedRecord)->TryGetObjectField(TEXT("variables"), SavedVars)))
					{
						continue;
					}
					Test.TestEqual(RecordLabel + TEXT(" variable count"), (*SavedVars)->Values.Num(), ExpectedVars->Values.Num());
					for (const auto& VarPair : ExpectedVars->Values)
					{
						const TSharedPtr<FJsonObject>* SavedVar = nullptr;
						if (!Test.TestTrue(RecordLabel + TEXT(".") + VarPair.Key + TEXT(" is in the save"),
							(*SavedVars)->TryGetObjectField(VarPair.Key, SavedVar)))
						{
							continue;
						}
						JsonEquals(Test, RecordLabel + TEXT(".") + VarPair.Key, (*SavedVar)->TryGetField(TEXT("value")), VarPair.Value);
					}
				}
			}

			// Double-capture half: the V2 dataAssets key is EMPTY of characters (and of
			// everything here — no session .sfd write ever ran).
			const TSharedPtr<FJsonObject> ExpectedDataAssets = Case->GetObjectField(TEXT("saveDataAssets"));
			const TSharedPtr<FJsonObject>* SavedDataAssets = nullptr;
			if (Test.TestTrue(CaseName + TEXT(": the save carries its dataAssets key"), Root->TryGetObjectField(TEXT("dataAssets"), SavedDataAssets)))
			{
				Test.TestEqual(CaseName + TEXT(": dataAssets entry count"), (*SavedDataAssets)->Values.Num(), ExpectedDataAssets->Values.Num());
			}
		}

		void ProcessDegraded(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const TSharedPtr<FJsonObject> Ref = Case->GetObjectField(TEXT("ref"));
			const TSharedPtr<FJsonObject> VariableObj = Case->GetObjectField(TEXT("variable"));
			const FString Id = JsonStr(Ref, TEXT("characterId"));
			const FString Path = JsonStr(Ref, TEXT("characterPath"));
			const FString Name = JsonStr(VariableObj, TEXT("name"));
			const FString SeedMutation = JsonStr(Case, TEXT("seedMutation"));
			const TSharedPtr<FJsonObject>* WiredObj = nullptr;
			Case->TryGetObjectField(TEXT("wired"), WiredObj);
			const FString WiredRef = WiredObj ? JsonStr(*WiredObj, TEXT("characterRef")) : FString();

			// One isolated freshly-seeded store per case, the seedMutation applied as DATA.
			TMap<FString, FString> BridgeBackup;
			if (SeedMutation == TEXT("inject-ghost-index-entry"))
			{
				const TSharedPtr<FJsonObject> Ghost = Case->GetObjectField(TEXT("ghost"));
				Project->CharacterIdToPath.Add(JsonStr(Ghost, TEXT("characterId")), JsonStr(Ghost, TEXT("recordKey")));
			}
			else if (SeedMutation == TEXT("no-index"))
			{
				BridgeBackup = Project->CharacterIdToPath;
				Project->CharacterIdToPath.Empty();
			}
			W.Subsystem->SetProject(Project);
			if (SeedMutation == TEXT("remove-record"))
			{
				// The save-lane door (Unreal runs this rung — §9 entry 3 excludes only the
				// merge-load engines): a REAL save with the record removed, applied wholesale
				// through the real slot pair, leaves a bridged id with no loaded record.
				const FString* RemovedKey = W.Subsystem->GetCharacterIdToPath().Find(Id);
				if (Test.TestNotNull(*(CaseName + TEXT(": the bridge maps the id to remove")), RemovedKey)
					&& Test.TestTrue(CaseName + TEXT(": the pre-removal save succeeds"), W.Subsystem->SaveToSlot(ContractSlotName, 0)))
				{
					const TSharedPtr<FJsonObject> Root = ReadSlotJson();
					if (Test.TestTrue(CaseName + TEXT(": the slot parses"), Root.IsValid()))
					{
						Root->GetObjectField(TEXT("characters"))->RemoveField(*RemovedKey);
						FString Doctored;
						const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
							TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Doctored);
						FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
						UStoryFlowSaveGame* DoctoredSave = NewObject<UStoryFlowSaveGame>();
						DoctoredSave->SaveDataJson = Doctored;
						Test.TestTrue(CaseName + TEXT(": the doctored save stores"), UGameplayStatics::SaveGameToSlot(DoctoredSave, ContractSlotName, 0));
						Test.TestTrue(CaseName + TEXT(": the doctored save loads wholesale"), W.Subsystem->LoadFromSlot(ContractSlotName, 0));
					}
				}
			}

			const int32 StoreCountBefore = W.Subsystem->GetRuntimeCharacters().Num();
			const FString PostConditionKey = ResolveStoredRecordKey(Id, Path);
			const FStoryFlowCharacterDef* PostConditionRecord =
				Case->HasField(TEXT("postCondition")) ? W.Subsystem->GetRuntimeCharacters().Find(PostConditionKey) : nullptr;
			const int32 VariablesBefore = PostConditionRecord ? PostConditionRecord->Variables.Num() : 0;

			// GET: the node-lane answer is the pinned surface for degraded reads.
			const TSharedPtr<FJsonObject>* Get = nullptr;
			if (Case->TryGetObjectField(TEXT("get"), Get))
			{
				int32 WarnsEmitted = 0;
				const FStoryFlowVariant Value = NodeLaneRead(Id, Path, VariableObj, WiredRef, &WarnsEmitted);
				VariantMatchesJson(Test, CaseName + TEXT(" (get)"), Value, (*Get)->TryGetField(TEXT("value")));
				if (!WiredRef.IsEmpty())
				{
					Test.TestEqual(CaseName + TEXT(": a wired-over dangling id never warns"), WarnsEmitted, 0);
				}
				if (SeedMutation == TEXT("no-index"))
				{
					Test.TestEqual(CaseName + TEXT(": the pre-P4 shape warns nothing"), WarnsEmitted, 0);
				}
			}

			// SET: through the real node write lane; refusal leaves the store untouched.
			const TSharedPtr<FJsonObject>* Set = nullptr;
			if (Case->TryGetObjectField(TEXT("set"), Set))
			{
				NodeLaneWrite(Id, Path, VariableObj, (*Set)->TryGetField(TEXT("value")), WiredRef);
				const FString Outcome = JsonStr(*Set, TEXT("outcome"));
				const TSharedPtr<FJsonValue> PostRead = (*Set)->TryGetField(TEXT("postRead"));
				if (Outcome == TEXT("written"))
				{
					VariantMatchesJson(Test, CaseName + TEXT(" (postRead)"), NodeLaneRead(Id, Path, VariableObj, WiredRef), PostRead);
				}
				else // refused
				{
					Test.TestEqual(CaseName + TEXT(": the refused write grew no record"),
						W.Subsystem->GetRuntimeCharacters().Num(), StoreCountBefore);
					if (PostRead.IsValid())
					{
						// The declaration's own value is untouched — read through the resolving
						// door, which types the answer by the DECLARATION, not the mismatched node.
						VariantMatchesJson(Test, CaseName + TEXT(" (postRead after refusal)"), HostRead(Id, Path, Name), PostRead);
					}
				}
			}

			if (Case->HasField(TEXT("postCondition")))
			{
				if (Test.TestNotNull(*(CaseName + TEXT(": the postCondition record exists")), PostConditionRecord))
				{
					Test.TestEqual(CaseName + TEXT(": the record declares exactly as many variables as before"),
						PostConditionRecord->Variables.Num(), VariablesBefore);
					Test.TestFalse(CaseName + TEXT(": the refused name was never created"),
						PostConditionRecord->Variables.Contains(Name));
				}
			}

			// Undo the bridge mutations so later cases reseed from the pristine project.
			if (SeedMutation == TEXT("inject-ghost-index-entry"))
			{
				const TSharedPtr<FJsonObject> Ghost = Case->GetObjectField(TEXT("ghost"));
				Project->CharacterIdToPath.Remove(JsonStr(Ghost, TEXT("characterId")));
			}
			else if (SeedMutation == TEXT("no-index"))
			{
				Project->CharacterIdToPath = BridgeBackup;
			}
		}
	};
}

// ============================================================================
// The one manifest-driven run: every case file, every case, in order
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterContractGoldenPackageTest,
	"StoryFlow.CharacterContract.GoldenPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowCharacterContractGoldenPackageTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterContractTestHelpers;

	// The degraded ladder's sanctioned noise, in this engine's pinned vocabulary (0 = at least
	// once; exact per-run counts are pinned by the dedicated resolution suites, not re-pinned
	// across this whole-package replay). The media warnings come from the vendored assets
	// table naming image files the package deliberately does not carry.
	AddExpectedError(TEXT("Source file not found"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("is not in this project's character index"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("is not among the loaded runtime characters"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("not found on character"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("SetCharacterVar type mismatch"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Character not found for GetCharacterVariable"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("SetCharacterVar has no character path"), EAutomationExpectedErrorFlags::Contains, 0);

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	// The four vendored inputs as a REAL build dir, through the REAL importer.
	UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
	if (!TestTrue(TEXT("the contract build folder is writable"), WriteContractBuildDir()))
	{
		ContractCleanUp();
		return false;
	}
	UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(ContractBuildDir(), ContractTestRoot);
	if (!TestNotNull(TEXT("the golden package imports"), Project))
	{
		ContractCleanUp();
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);

	FContractHarness Harness(*this, W);
	Harness.Project = Project;

	// The vendored assets table, the path authority behind the builtin Image `value` seats.
	{
		const TSharedPtr<FJsonObject> CharactersFile = LoadPackageFile(TEXT("characters.json"));
		const TSharedPtr<FJsonObject>* Assets = nullptr;
		if (TestTrue(TEXT("characters.json carries its assets table"),
			CharactersFile.IsValid() && CharactersFile->TryGetObjectField(TEXT("assets"), Assets)))
		{
			for (const auto& AssetPair : (*Assets)->Values)
			{
				Harness.AssetPaths.Add(AssetPair.Key, JsonStr(AssetPair.Value->AsObject(), TEXT("path")));
			}
		}
	}

	const TSharedPtr<FJsonObject> Manifest = LoadPackageFile(TEXT("manifest.json"));
	if (!TestTrue(TEXT("the manifest loads"), Manifest.IsValid()))
	{
		ContractCleanUp();
		return false;
	}
	TSet<FString> Kinds;
	for (const TSharedPtr<FJsonValue>& Kind : Manifest->GetArrayField(TEXT("kinds")))
	{
		Kinds.Add(Kind->AsString());
	}
	{
		TArray<FString> Engines;
		for (const TSharedPtr<FJsonValue>& Engine : Manifest->GetArrayField(TEXT("engines")))
		{
			Engines.Add(Engine->AsString());
		}
		TestTrue(TEXT("this engine is in the manifest's engines vocabulary"), Engines.Contains(TEXT("unreal")));
	}
	const int32 DeclaredCaseCount = Manifest->GetIntegerField(TEXT("case_count"));

	// The read-file cases run against the freshly imported store.
	W.Subsystem->SetProject(Project);
	int32 Consumed = 0;
	bool bWriteStoreSeeded = false;

	for (const TSharedPtr<FJsonValue>& CaseFileValue : Manifest->GetArrayField(TEXT("caseFiles")))
	{
		const TSharedPtr<FJsonObject> CaseFileEntry = CaseFileValue->AsObject();
		const FString FileName = JsonStr(CaseFileEntry, TEXT("file"));
		const TSharedPtr<FJsonObject> CaseFile = LoadPackageFile(FileName);
		if (!TestTrue(FString::Printf(TEXT("%s loads"), *FileName), CaseFile.IsValid()))
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>& Cases = CaseFile->GetArrayField(TEXT("cases"));
		TestEqual(FString::Printf(TEXT("%s carries its declared case count"), *FileName),
			Cases.Num(), static_cast<int32>(CaseFileEntry->GetIntegerField(TEXT("case_count"))));

		for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
		{
			const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
			const FString CaseName = JsonStr(Case, TEXT("case"));
			const FString Kind = JsonStr(Case, TEXT("kind"));

			// A kind outside the manifest's list is a FAILURE, not a skip.
			if (!Kinds.Contains(Kind))
			{
				AddError(FString::Printf(TEXT("%s: kind '%s' is not in the manifest's kinds"), *CaseName, *Kind));
				continue;
			}

			// excluded[unreal] is a skip AS DATA: visited, its §9 reason logged, counted consumed.
			const TSharedPtr<FJsonObject>* Excluded = nullptr;
			if (Case->TryGetObjectField(TEXT("excluded"), Excluded) && (*Excluded)->HasField(TEXT("unreal")))
			{
				AddInfo(FString::Printf(TEXT("%s: skipped as data - %s"), *CaseName, *JsonStr(*Excluded, TEXT("unreal"))));
				++Harness.SkippedExcluded;
				++Consumed;
				continue;
			}

			// The write file's one-state property: replay every write against ONE store,
			// seeded fresh at the first write and shared by the sweep and save cases after it.
			if (Kind == TEXT("write") && !bWriteStoreSeeded)
			{
				W.Subsystem->SetProject(Project);
				bWriteStoreSeeded = true;
			}

			if (Kind == TEXT("read"))
			{
				Harness.ProcessRead(CaseName, Case);
			}
			else if (Kind == TEXT("speaker"))
			{
				Harness.ProcessSpeaker(CaseName, Case);
			}
			else if (Kind == TEXT("write"))
			{
				Harness.ProcessWrite(CaseName, Case);
			}
			else if (Kind == TEXT("sweep"))
			{
				Harness.ProcessSweep(CaseName, Case);
			}
			else if (Kind == TEXT("save"))
			{
				Harness.ProcessSave(CaseName, Case);
			}
			else // degraded — the manifest's kinds list is closed above
			{
				Harness.ProcessDegraded(CaseName, Case);
			}
			++Harness.RunCases;
			++Harness.PerKind.FindOrAdd(Kind);
			++Consumed;
		}
	}

	TestEqual(TEXT("exactly the manifest's case_count cases were consumed"), Consumed, DeclaredCaseCount);

	FString Accounting = FString::Printf(TEXT("character-contract accounting: consumed=%d run=%d skipped-excluded=%d expect-fail-inverted=%d; per kind:"),
		Consumed, Harness.RunCases, Harness.SkippedExcluded, Harness.ExpectFailInverted);
	for (const auto& KindPair : Harness.PerKind)
	{
		Accounting += FString::Printf(TEXT(" %s=%d"), *KindPair.Key, KindPair.Value);
	}
	AddInfo(Accounting);

	ContractCleanUp();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
