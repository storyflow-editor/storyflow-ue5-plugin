// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowDataAssetStore.h"
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
#include "StoryFlowLanguageAccumulator.h"
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
 * The FIVE inputs are written as a REAL build folder and imported through the REAL importer —
 * and, for the localization arm's source-only case, a SECOND build folder that leaves
 * localization.json out entirely, because the marker is the file existing and no emptied table
 * can stand in for a file that is not there.
 *
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
 * The LOCALIZATION arm (spec §9) rides the same run. Its three kinds resolve through the
 * engine's own chokepoint — FStoryFlowExecutionContext::GetString, the call every dialogue line
 * makes — never through a copy of the rule, and it computes no status and no hash because the
 * sidecar's tables arrive full and pre-resolved. FStoryFlowLocalizationApiTest below then
 * watches the same flip where a player would see it, in the rendered dialogue state.
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
	const TCHAR* SourceOnlyTestRoot = TEXT("/Game/StoryFlowCharacterContractSourceOnlyTests");
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

	/** The same vendored inputs MINUS localization.json — the source-only (pre-localization) export. */
	FString SourceOnlyBuildDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowCharacterContractSourceOnly"));
	}

	/**
	 * The vendored inputs, VERBATIM, as a build folder (script.json becomes the one script).
	 *
	 * bWithLocalization writes the FIFTH input or leaves it out entirely. Leaving it out is the
	 * whole absent-sidecar case: the marker is the file EXISTING, so the only honest way to test
	 * the absence branch is to import a build that genuinely does not carry the file — never by
	 * emptying a table, which in C++ is indistinguishable from a sidecar with no rows.
	 */
	bool WriteContractBuildDir(const FString& Dir, bool bWithLocalization)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		bool bOk = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(Dir, TEXT("project.json")));
		TArray<const TCHAR*> Verbatim = { TEXT("characters.json"), TEXT("character-index.json"), TEXT("data-assets.json") };
		if (bWithLocalization)
		{
			Verbatim.Add(TEXT("localization.json"));
		}
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
		UEditorAssetLibrary::DeleteDirectory(SourceOnlyTestRoot);
		IFileManager::Get().DeleteDirectory(*ContractBuildDir(), false, true);
		IFileManager::Get().DeleteDirectory(*SourceOnlyBuildDir(), false, true);
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
		/** The SAME inputs imported from a build folder with no localization.json (the absence branch). */
		UStoryFlowProjectAsset* SourceOnlyProject = nullptr;
		/** The vendored assets table: asset id -> exported path ("images/hero.png"). */
		TMap<FString, FString> AssetPaths;
		/** localization.json as vendored: `<code>` -> `<stringId>` -> text. The sidecar's own word. */
		TMap<FString, TMap<FString, FString>> SidecarTables;
		/** data-assets.json as vendored — the `unkeyed` cases read their literals out of it. */
		TSharedPtr<FJsonObject> DataAssetsFile;
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

		/** The sidecar's own row for (language, id), or empty when it carries none. */
		FString SidecarRow(const FString& Language, const FString& StringId) const
		{
			const TMap<FString, FString>* Table = SidecarTables.Find(Language);
			const FString* Text = Table ? Table->Find(StringId) : nullptr;
			return Text ? *Text : FString();
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
			Context.SeedLanguageCode = W.Subsystem->GetLanguage();
			Context.InitializeWithSubsystem(Project, Script, &W.Subsystem->GetGlobalVariables(),
				&W.Subsystem->GetRuntimeCharacters(), &W.Subsystem->GetUsedOnceOnlyOptions(),
				W.Subsystem->GetDataAssetStore(), &W.Subsystem->GetCharacterIdToPath(), &W.Subsystem->GetLanguageRef());
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
				// Host and graph consumers receive display text; the record retains its authored key.
				Test.TestEqual(CaseName + TEXT(": resolving door answers the resolved Name"), Door.GetString(), ExpectedValue->AsString());
				Test.TestEqual(CaseName + TEXT(": node lane answers the resolved Name"), NodeLane.GetString(), ExpectedValue->AsString());
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
					const FString RecordKey(ExpectedPair.Key);
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

		// ====================================================================
		// The localization arm (spec §9)
		// ====================================================================

		/**
		 * THE RESOLUTION CHOKEPOINT ITSELF, not a copy of it: the engine's own
		 * FStoryFlowExecutionContext::GetString, pointed at a project and its one script — the
		 * same object and the same call UStoryFlowComponent::ResolveString makes on every
		 * dialogue line. One context serves BOTH keying artifacts because the chain it runs is
		 * the whole chain: language table -> current script's table -> project globals (which is
		 * where the importer merges characters.json's) -> the raw id.
		 */
		FString ResolveThroughChokepoint(UStoryFlowProjectAsset* Proj, const FString& StringId, const FString& Language) const
		{
			FStoryFlowExecutionContext Context;
			Context.Project = Proj;
			Context.CurrentScript = Proj ? Proj->GetScriptByPath(TEXT("main")) : nullptr;
			return Context.GetString(StringId, Language);
		}

		/**
		 * THE REACH RULE. Read the named character's named variable and take THE KEY IT STORES —
		 * never rebuild the id from the character being read. In this engine the stored key lives
		 * on the imported character ASSET, the pre-resolution record the §5 `stored` seats already
		 * compare against, because the runtime store resolves the string family eagerly at seeding.
		 *
		 * An inherited value's key names the DECLARING ANCESTOR, so an implementation that builds
		 * `<characterBeingRead>.<variableId>.value` produces an id nothing carries, passes every
		 * non-inherited case, and resolves this one to the raw id.
		 */
		FString ReachStoredKey(const TSharedPtr<FJsonObject>& Reach) const
		{
			const FString RecordKey = ResolveStoredRecordKey(JsonStr(Reach, TEXT("characterId")), FString());
			return RawStored(RecordKey, JsonStr(Reach, TEXT("variableName"))).GetString();
		}

		void ProcessLocalized(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const FString Language = JsonStr(Case, TEXT("language"));
			const FString KeyedIn = JsonStr(Case, TEXT("keyedIn"));
			const FString Expected = JsonStr(Case, TEXT("expected"));
			bool bExpectFail = false;
			Case->TryGetBoolField(TEXT("expect_fail"), bExpectFail);

			// The id the lookup runs on: FOLLOWED through the character record when the case
			// carries a reach, taken verbatim otherwise.
			FString StringId = JsonStr(Case, TEXT("stringId"));
			const TSharedPtr<FJsonObject>* Reach = nullptr;
			if (Case->TryGetObjectField(TEXT("reach"), Reach))
			{
				StringId = ReachStoredKey(*Reach);
				Test.TestEqual(CaseName + TEXT(": the character record stores the ancestor-owned key"),
					StringId, JsonStr(*Reach, TEXT("storedKey")));
			}

			const FString Resolved = ResolveThroughChokepoint(Project, StringId, Language);

			if (bExpectFail)
			{
				// Inverted: this case's `expected` is the CURRENT SOURCE of an outdated row, which
				// is what an engine that recomputes status produces. Ruling 2 says the OLD
				// translation ships. Reporting it as passing would prove exactly that defect.
				Test.TestTrue(CaseName + TEXT(": expect_fail reported as failing (the deliberately wrong source text is not what resolved)"),
					Resolved != Expected);
				// And it misses in the ONE direction the ruling names: by shipping the OLD
				// translation, which is the row the sidecar itself carries. A resolve that
				// returned the raw id, or the source text, would also "not equal" — and each of
				// those is a different silent defect this counter-assertion refuses to accept.
				Test.TestEqual(CaseName + TEXT(": it misses by shipping the OLD translation the sidecar carries"),
					Resolved, SidecarRow(Language, StringId));
				Test.TestFalse(CaseName + TEXT(": and not by falling through to the raw id"), Resolved == StringId);
				++ExpectFailInverted;
				return;
			}

			Test.TestEqual(FString::Printf(TEXT("%s: %s[%s]"), *CaseName, *Language, *StringId), Resolved, Expected);
			// NEVER undefined and never an accidental empty string — the shape the whole contract
			// rests on, asserted per case rather than once.
			Test.TestFalse(CaseName + TEXT(": the resolve is never empty"), Resolved.IsEmpty());

			// A characters.json-keyed id must answer the same OUTSIDE dialogue too: that door
			// (the project's global table) is where a character Name resolves between scripts.
			if (KeyedIn == TEXT("characters.json"))
			{
				Test.TestEqual(CaseName + TEXT(": the outside-dialogue door agrees"),
					Project->GetGlobalString(StringId, Language), Expected);
			}
		}

		// --------------------------------------------------------------------
		// The `.sfd` lane (spec §2's amendment of 2026-08-27)
		// --------------------------------------------------------------------

		/**
		 * The DECLARATION json for a variable id, found by scanning every asset in the vendored
		 * seed rather than by walking one chain — which is not a shortcut but the id rule itself:
		 * a `.sfd` id carries no asset segment BECAUSE a variable is declared at exactly one level
		 * of exactly one chain, so a scan cannot find two. It is also what a case needs, since an
		 * `unkeyed` override case names the asset carrying the OVERRIDE while the declaration
		 * (and the accessor snapshot the §6.1 gate compares against) lives on an ancestor.
		 */
		TSharedPtr<FJsonObject> DeclarationJson(const FString& VariableId) const
		{
			const TSharedPtr<FJsonObject>* Assets = nullptr;
			if (!DataAssetsFile.IsValid() || !DataAssetsFile->TryGetObjectField(TEXT("dataAssets"), Assets))
			{
				return nullptr;
			}
			for (const auto& AssetPair : (*Assets)->Values)
			{
				const TSharedPtr<FJsonObject> Asset = AssetPair.Value->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
				if (!Asset.IsValid() || !Asset->TryGetArrayField(TEXT("variables"), Variables))
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
				{
					const TSharedPtr<FJsonObject> Variable = VariableValue->AsObject();
					if (Variable.IsValid() && JsonStr(Variable, TEXT("id")) == VariableId)
					{
						return Variable;
					}
				}
			}
			return nullptr;
		}

		/** The imported Data Asset for an id — the handle a Blueprint holds. */
		UStoryFlowDataAssetAsset* DataAssetFor(const FString& AssetId) const
		{
			return Project ? Project->DataAssets.FindRef(AssetId) : nullptr;
		}

		/**
		 * THE BLUEPRINT READ DOOR, driven the way game code drives it: by the display NAME an
		 * author typed, off an asset handle. The name is looked up from the seed's own
		 * declaration, so the id -> name -> id round trip at that boundary is exercised too.
		 */
		FStoryFlowVariant HostReadDataAsset(const FString& AssetId, const FString& VariableId, bool& bFound)
		{
			bFound = false;
			UStoryFlowDataAssetAsset* Asset = DataAssetFor(AssetId);
			const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclaration(W.Subsystem->GetDataAssetSeed(), AssetId, VariableId);
			if (!Asset || !Declaration)
			{
				return FStoryFlowVariant();
			}
			return W.Component->GetDataAssetVariantVariable(Asset, Declaration->Name, bFound);
		}

		/**
		 * THE SCRIPT LANE'S READ DOOR: a real pill -> accessor graph through the real evaluator,
		 * with the accessor's §2.2 spawn snapshot taken from the exporter's own declaration bytes
		 * so the §6.1 declMatches gate passes for the right reason. TryReadDataAssetVariable is
		 * the one function every typed arm funnels through, which makes it this lane's `read()`.
		 */
		FStoryFlowVariant NodeLaneReadDataAsset(const FString& AssetId, const FString& VariableId, bool& bFound)
		{
			bFound = false;
			const TSharedPtr<FJsonObject> Declaration = DeclarationJson(VariableId);
			if (!Declaration.IsValid())
			{
				return FStoryFlowVariant();
			}
			bool bIsArray = false;
			Declaration->TryGetBoolField(TEXT("isArray"), bIsArray);
			return NodeLaneReadWithSnapshot(AssetId, VariableId, JsonStr(Declaration, TEXT("name")), JsonStr(Declaration, TEXT("type")),
				bIsArray, JsonStr(Declaration, TEXT("keyType")), JsonStr(Declaration, TEXT("valueType")), bFound);
		}

		/**
		 * NodeLaneReadDataAsset with the §2.2 snapshot passed in rather than read out of the
		 * vendored bytes — for a seed this harness built itself, which has no vendored file to
		 * take a snapshot from.
		 */
		FStoryFlowVariant NodeLaneReadWithSnapshot(const FString& AssetId, const FString& VariableId, const FString& VariableName,
			const FString& VariableType, bool bIsArray, const FString& KeyType, const FString& ValueType, bool& bFound)
		{
			bFound = false;

			UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
			FGCObjectScopeGuard ScriptGuard(Script);
			Script->StartNode = TEXT("0");
			Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));

			FStoryFlowNode Pill = MakeNode(TEXT("pill"), EStoryFlowNodeType::GetDataAsset, TEXT("getDataAsset"));
			Pill.Data.AssetId = AssetId;
			Script->Nodes.Add(Pill.Id, Pill);

			FStoryFlowNode Getter = MakeNode(TEXT("get"), EStoryFlowNodeType::GetDataAssetVariable, TEXT("getDataAssetVariable"));
			Getter.Data.VariableId = VariableId;
			Getter.Data.VariableName = VariableName;
			Getter.Data.Variable = VariableName;
			Getter.Data.VariableType = VariableType;
			Getter.Data.bIsArray = bIsArray;
			Getter.Data.KeyType = KeyType;
			Getter.Data.ValueType = ValueType;
			Script->Nodes.Add(Getter.Id, Getter);

			Script->Connections.Add(MakeEdge(TEXT("pill"), TEXT("get"),
				StoryFlowHandles::Source(TEXT("pill"), TEXT("dataAsset-")),
				StoryFlowHandles::Target(TEXT("get"), StoryFlowHandles::In_DataAssetRef)));
			Script->BuildConnectionIndices();

			FStoryFlowExecutionContext Context;
			Context.InitializeWithSubsystem(Project, Script, &W.Subsystem->GetGlobalVariables(), &W.Subsystem->GetRuntimeCharacters(),
				&W.Subsystem->GetUsedOnceOnlyOptions(), W.Subsystem->GetDataAssetStore(), &W.Subsystem->GetCharacterIdToPath(),
				&W.Subsystem->GetLanguageRef());
			FStoryFlowEvaluator Evaluator(&Context);
			FStoryFlowVariant Value;
			bFound = Evaluator.TryReadDataAssetVariable(Context.GetNode(TEXT("get")), Value);
			return Value;
		}

		/**
		 * BOTH `.sfd` read doors at once, required to AGREE — the same discipline the character
		 * arm's `read` kind applies, and for the same reason: a rule implemented at one surface
		 * and not the other is a bug a single-surface test cannot see.
		 */
		FStoryFlowVariant ReadDataAsset(const FString& CaseName, const FString& AssetId, const FString& VariableId)
		{
			bool bHostFound = false;
			bool bNodeFound = false;
			const FStoryFlowVariant Host = HostReadDataAsset(AssetId, VariableId, bHostFound);
			const FStoryFlowVariant Node = NodeLaneReadDataAsset(AssetId, VariableId, bNodeFound);
			Test.TestTrue(FString::Printf(TEXT("%s: %s.%s resolves through the Blueprint door"), *CaseName, *AssetId, *VariableId), bHostFound);
			Test.TestTrue(FString::Printf(TEXT("%s: %s.%s resolves through the script lane"), *CaseName, *AssetId, *VariableId), bNodeFound);
			Test.TestTrue(FString::Printf(TEXT("%s: the two .sfd doors agree on %s.%s"), *CaseName, *AssetId, *VariableId),
				VariantsEqual(Host, Node));
			return Host;
		}

		/** Structural equality over the shapes a `.sfd` read hands back (scalar, array, map). */
		static bool VariantsEqual(const FStoryFlowVariant& A, const FStoryFlowVariant& B)
		{
			if (A.GetType() != B.GetType())
			{
				return false;
			}
			if (A.IsMap())
			{
				const TArray<FStoryFlowMapEntry>& EntriesA = A.GetMap();
				const TArray<FStoryFlowMapEntry>& EntriesB = B.GetMap();
				if (EntriesA.Num() != EntriesB.Num())
				{
					return false;
				}
				for (int32 Index = 0; Index < EntriesA.Num(); ++Index)
				{
					if (EntriesA[Index].Key.ToString() != EntriesB[Index].Key.ToString()
						|| EntriesA[Index].Value.ToString() != EntriesB[Index].Value.ToString())
					{
						return false;
					}
				}
				return true;
			}
			if (A.GetArray().Num() != B.GetArray().Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < A.GetArray().Num(); ++Index)
			{
				if (A.GetArray()[Index].ToString() != B.GetArray()[Index].ToString())
				{
					return false;
				}
			}
			return A.ToString() == B.ToString();
		}

		/**
		 * THE `unkeyed` KIND: a `.sfd` value that ships LITERAL, driven through THIS ENGINE'S OWN
		 * data-asset accessors once per language.
		 *
		 * The manifest is explicit that a harness which only byte-copies the value out of
		 * data-assets.json proves nothing, and it is right: every `.sfd` rule is about the ID A
		 * READ DOOR BUILDS on its way to a table, and a byte comparison never reaches that door.
		 * So this reads the bytes (a drifted literal is a stale case), then asks the real doors,
		 * then checks the absence, then — where the case carries a `collidesWith` — resolves the
		 * id a WRONG implementation would have built and confirms the doors did not answer with it.
		 * That last step is what makes the override case teeth rather than decoration: walking
		 * overrides does not miss, it serves the ancestor's prose.
		 */
		void ProcessUnkeyed(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const FString AssetId = JsonStr(Case, TEXT("dataAssetId"));
			const FString VariableId = JsonStr(Case, TEXT("variableId"));
			const FString From = JsonStr(Case, TEXT("from"));
			const FString Literal = JsonStr(Case, TEXT("literal"));

			// 1. THE BYTES, where `from` says an engine reads them.
			const TSharedPtr<FJsonObject>* Assets = nullptr;
			if (!Test.TestTrue(*(CaseName + TEXT(": data-assets.json is vendored")),
				DataAssetsFile.IsValid() && DataAssetsFile->TryGetObjectField(TEXT("dataAssets"), Assets)))
			{
				return;
			}
			const TSharedPtr<FJsonObject>* Asset = nullptr;
			if (!Test.TestTrue(*(CaseName + TEXT(": the seed carries this Data Asset")), (*Assets)->TryGetObjectField(AssetId, Asset)))
			{
				return;
			}
			FString Stored;
			if (From == TEXT("override"))
			{
				const TSharedPtr<FJsonObject>* Overrides = nullptr;
				(*Asset)->TryGetObjectField(TEXT("overrides"), Overrides);
				if (Overrides)
				{
					(*Overrides)->TryGetStringField(VariableId, Stored);
				}
			}
			else
			{
				Stored = JsonStr(DeclarationJson(VariableId), TEXT("value"));
			}
			Test.TestEqual(CaseName + TEXT(": the artifact still carries this literal"), Stored, Literal);

			// 2. THE DOORS, once per language the case names. The language is the subsystem's, so
			//    it is set and restored around the reads — a `.sfd` value resolves at READ time,
			//    which is exactly what makes driving the door per language meaningful.
			const FString LanguageBefore = W.Subsystem->GetLanguage();
			const TSharedPtr<FJsonObject> ExpectedByLanguage = Case->GetObjectField(TEXT("expected"));
			for (const auto& LanguagePair : ExpectedByLanguage->Values)
			{
				const FString Language(LanguagePair.Key);
				const FString Expected = LanguagePair.Value->AsString();
				Test.TestEqual(CaseName + TEXT(": the case expects the literal in ") + Language, Expected, Literal);
				Test.TestTrue(CaseName + TEXT(": the engine accepts ") + Language, W.Subsystem->SetLanguage(Language));

				const FStoryFlowVariant Read = ReadDataAsset(CaseName, AssetId, VariableId);
				Test.TestEqual(FString::Printf(TEXT("%s: the accessor answers the literal in %s"), *CaseName, *Language),
					Read.GetString(), Expected);

				// 4. THE TEETH. The id a walker of overrides would have built resolves to somebody
				//    ELSE's prose in this language, and the door did not hand that back.
				const TSharedPtr<FJsonObject>* Collides = nullptr;
				if (Case->TryGetObjectField(TEXT("collidesWith"), Collides))
				{
					const FString CollidingId = JsonStr(*Collides, TEXT("stringId"));
					const FString CollidingText = JsonStr((*Collides)->GetObjectField(TEXT("resolved")), *Language);
					Test.TestEqual(FString::Printf(TEXT("%s: %s resolves the colliding id %s"), *CaseName, *Language, *CollidingId),
						ResolveThroughChokepoint(Project, CollidingId, Language), CollidingText);
					Test.TestNotEqual(FString::Printf(TEXT("%s: the collision is real in %s, so the case has teeth"), *CaseName, *Language),
						CollidingText, Literal);
					Test.TestNotEqual(FString::Printf(TEXT("%s: the accessor did NOT serve the colliding text in %s"), *CaseName, *Language),
						Read.GetString(), CollidingText);
				}
			}
			W.Subsystem->SetLanguage(LanguageBefore);

			// 3. THE ABSENCE, which IS the contract: no table anywhere keys an id an implementation
			//    might have minted for this value. The raw-fallback tier answers an unkeyed id with
			//    itself, so "resolves to itself" is how a total lookup says "nothing keys this".
			const TArray<TSharedPtr<FJsonValue>>* AbsentIds = nullptr;
			if (Case->TryGetArrayField(TEXT("absentIds"), AbsentIds))
			{
				for (const TSharedPtr<FJsonValue>& AbsentValue : *AbsentIds)
				{
					const FString AbsentId = AbsentValue->AsString();
					for (const auto& TablePair : SidecarTables)
					{
						Test.TestFalse(FString::Printf(TEXT("%s: the %s table carries no row for %s"), *CaseName, *TablePair.Key, *AbsentId),
							TablePair.Value.Contains(AbsentId));
					}
					Test.TestEqual(FString::Printf(TEXT("%s: %s keys no artifact either"), *CaseName, *AbsentId),
						ResolveThroughChokepoint(Project, AbsentId, TEXT("fr")), AbsentId);
				}
			}
		}

		void ProcessLanguageTable(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			const FString Language = JsonStr(Case, TEXT("language"));
			const TSharedPtr<FJsonObject> Expected = Case->GetObjectField(TEXT("expected"));
			int32 Rows = 0;
			for (const auto& RowPair : Expected->Values)
			{
				const FString Resolved = ResolveThroughChokepoint(Project, FString(RowPair.Key), Language);
				Test.TestEqual(FString::Printf(TEXT("%s: %s[%s]"), *CaseName, *Language, *RowPair.Key),
					Resolved, RowPair.Value->AsString());
				Test.TestFalse(CaseName + TEXT(": the resolve is never empty for ") + RowPair.Key, Resolved.IsEmpty());
				++Rows;
			}
			Test.TestTrue(CaseName + TEXT(": the table carried rows to compare"), Rows > 0);
		}

		/**
		 * THE ABSENCE BRANCH, against a SECOND project imported from a build folder that genuinely
		 * carries no localization.json. Nothing is emptied and nothing is mutated: the marker is
		 * the file existing, so only a real import of a real pre-localization build proves it.
		 */
		void ProcessSourceOnly(const FString& CaseName, const TSharedPtr<FJsonObject>& Case)
		{
			if (!Test.TestNotNull(*(CaseName + TEXT(": the source-only project imports")), SourceOnlyProject))
			{
				return;
			}

			Test.TestFalse(CaseName + TEXT(": a build with no sidecar is not a localized project"), SourceOnlyProject->bHasLocalization);
			Test.TestEqual(CaseName + TEXT(": it registers no language tables"), SourceOnlyProject->LanguageStrings.Num(), 0);
			Test.TestEqual(CaseName + TEXT(": it offers no target languages"), SourceOnlyProject->Languages.Num(), 0);
			Test.TestEqual(CaseName + TEXT(": its source language is the pre-localization default"), SourceOnlyProject->SourceLanguage, FString(TEXT("en")));

			// Every shipped id, resolved as the plugin behaved before localization existed.
			const TSharedPtr<FJsonObject> Expected = Case->GetObjectField(TEXT("expected"));
			for (const auto& RowPair : Expected->Values)
			{
				const FString Resolved = ResolveThroughChokepoint(SourceOnlyProject, FString(RowPair.Key), SourceOnlyProject->SourceLanguage);
				Test.TestEqual(FString::Printf(TEXT("%s: source-only[%s]"), *CaseName, *RowPair.Key), Resolved, RowPair.Value->AsString());
			}

			// Even asked for a language, a project with no sidecar answers source text: there is
			// no table to overlay, and the fall-through is the artifact's own.
			for (const auto& RowPair : Expected->Values)
			{
				Test.TestEqual(FString::Printf(TEXT("%s: source-only[%s] asked in fr"), *CaseName, *RowPair.Key),
					ResolveThroughChokepoint(SourceOnlyProject, FString(RowPair.Key), TEXT("fr")), RowPair.Value->AsString());
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

	// The five vendored inputs as a REAL build dir, through the REAL importer.
	UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
	if (!TestTrue(TEXT("the contract build folder is writable"), WriteContractBuildDir(ContractBuildDir(), /*bWithLocalization=*/ true)))
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

	// And the SAME inputs with the sidecar left out — the pre-localization export the
	// source-only case runs against. A second real import, because the marker is the file
	// existing and nothing else can stand in for it.
	UEditorAssetLibrary::DeleteDirectory(SourceOnlyTestRoot);
	TestTrue(TEXT("the source-only build folder is writable"), WriteContractBuildDir(SourceOnlyBuildDir(), /*bWithLocalization=*/ false));
	UStoryFlowProjectAsset* SourceOnlyProject = UStoryFlowImporter::ImportProject(SourceOnlyBuildDir(), SourceOnlyTestRoot);
	FGCObjectScopeGuard SourceOnlyGuard(SourceOnlyProject);

	FContractHarness Harness(*this, W);
	Harness.Project = Project;
	Harness.SourceOnlyProject = SourceOnlyProject;

	// The vendored sidecar's own tables — the authority the outdated tripwire counter-asserts
	// against, so "it misses by shipping the OLD translation" is data and not a literal.
	{
		const TSharedPtr<FJsonObject> Sidecar = LoadPackageFile(TEXT("localization.json"));
		const TSharedPtr<FJsonObject>* Strings = nullptr;
		if (TestTrue(TEXT("localization.json is vendored and carries its strings"),
			Sidecar.IsValid() && Sidecar->TryGetObjectField(TEXT("strings"), Strings)))
		{
			for (const auto& TablePair : (*Strings)->Values)
			{
				TMap<FString, FString>& Table = Harness.SidecarTables.Add(FString(TablePair.Key));
				for (const auto& RowPair : TablePair.Value->AsObject()->Values)
				{
					Table.Add(FString(RowPair.Key), RowPair.Value->AsString());
				}
			}
		}
	}

	// The import registered the sidecar: the presence marker, the source language and one table
	// per language. Asserted here rather than inside a case because the case file describes
	// RESOLUTION, and this is the load step every one of its cases stands on.
	TestTrue(TEXT("the localized build imports as a localized project"), Project->bHasLocalization);
	TestEqual(TEXT("the source language came from the sidecar"), Project->SourceLanguage, FString(TEXT("en")));
	TestEqual(TEXT("one registered table per language the sidecar ships"), Project->LanguageStrings.Num(), Harness.SidecarTables.Num());
	TestEqual(TEXT("the target languages are registered in the author's order"), Project->Languages.Num(), 2);

	// The vendored `.sfd` seed — the third keying artifact since spec §2's amendment, and the
	// bytes the `unkeyed` cases read their literals and their accessor snapshots out of.
	Harness.DataAssetsFile = LoadPackageFile(TEXT("data-assets.json"));
	TestTrue(TEXT("data-assets.json is vendored"), Harness.DataAssetsFile.IsValid());

	// The vendored assets table, the path authority behind the builtin Image `value` seats.
	{
		const TSharedPtr<FJsonObject> CharactersFile = LoadPackageFile(TEXT("characters.json"));
		const TSharedPtr<FJsonObject>* Assets = nullptr;
		if (TestTrue(TEXT("characters.json carries its assets table"),
			CharactersFile.IsValid() && CharactersFile->TryGetObjectField(TEXT("assets"), Assets)))
		{
			for (const auto& AssetPair : (*Assets)->Values)
			{
				Harness.AssetPaths.Add(FString(AssetPair.Key), JsonStr(AssetPair.Value->AsObject(), TEXT("path")));
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
			else if (Kind == TEXT("degraded"))
			{
				Harness.ProcessDegraded(CaseName, Case);
			}
			else if (Kind == TEXT("localized"))
			{
				Harness.ProcessLocalized(CaseName, Case);
			}
			else if (Kind == TEXT("language-table"))
			{
				Harness.ProcessLanguageTable(CaseName, Case);
			}
			else if (Kind == TEXT("unkeyed"))
			{
				Harness.ProcessUnkeyed(CaseName, Case);
			}
			else if (Kind == TEXT("source-only"))
			{
				Harness.ProcessSourceOnly(CaseName, Case);
			}
			else
			{
				// A kind the MANIFEST lists and this harness has no arm for. Previously the last
				// arm was an unguarded `else`, so a newly vendored kind was silently fed to the
				// source-only processor and reported as a case-shaped mismatch rather than as the
				// missing arm it is — which is exactly what the `unkeyed` kind did on arrival. The
				// closed vocabulary check above only catches kinds the manifest does NOT list;
				// this catches the ones it does.
				AddError(FString::Printf(TEXT("%s: kind '%s' is in the manifest but this harness has no arm for it"), *CaseName, *Kind));
				continue;
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

// ============================================================================
// The language API (spec §9), driven through the real dialogue lane
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLocalizationApiTest,
	"StoryFlow.CharacterContract.LocalizationApi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

/**
 * SetLanguage / GetLanguage / GetLanguages against the vendored package, with the flip observed
 * where a player would see it: the dialogue state of the script's first line — its title, its
 * body and its speaker's name — rather than at the lookup the golden cases already pin.
 *
 * The expected texts are the vendored sidecar's own rows for node 1, which the golden case file
 * pins independently; repeating two of them here is what makes this a LIVE-WIRING test (the
 * language reaches the renderer at all) rather than a second resolution test.
 */
bool FStoryFlowLocalizationApiTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterContractTestHelpers;

	// The unknown-code no-op says so out loud, once per rejected code.
	AddExpectedError(TEXT("SetLanguage - unknown language"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Source file not found"), EAutomationExpectedErrorFlags::Contains, 0);

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
	if (!TestTrue(TEXT("the contract build folder is writable"), WriteContractBuildDir(ContractBuildDir(), /*bWithLocalization=*/ true)))
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
	W.Subsystem->SetProject(Project);

	// ONE authored line, rendered through the real dialogue lane.
	//
	// The graph is built here rather than executed out of the vendored script.json for one
	// reason: that artifact's edges carry no handles (it is a binding fixture, and the §5 cases
	// read its nodes rather than run them), so the executor stops at its start node. What
	// matters for THIS test travels verbatim from the vendored data anyway — node 1's title and
	// text ids, its characterRefId, and the imported script's own `strings` table, which is the
	// source tier every un-translated expectation below falls through to.
	auto RenderLine = [&W](UStoryFlowProjectAsset* Proj, FString& OutTitle, FString& OutText, FString& OutSpeaker)
	{
		using namespace StoryFlowCharacterIndexTestHelpers;

		UStoryFlowScriptAsset* Line = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
		FGCObjectScopeGuard LineGuard(Line);
		if (UStoryFlowScriptAsset* Imported = Proj->GetScriptByPath(TEXT("main")))
		{
			Line->Strings = Imported->Strings;
		}
		Line->StartNode = TEXT("0");
		Line->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
		{
			FStoryFlowNode D = MakeNode(TEXT("1"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
			D.Data.Title = TEXT("1.title");
			D.Data.Text = TEXT("1.text");
			D.Data.Character = TEXT("characters\\hero.sfc");
			D.Data.CharacterRefId = TEXT("da_hero0000000000000000000000000a");
			Line->Nodes.Add(D.Id, D);
		}
		Line->Connections.Add(MakeEdge(TEXT("0"), TEXT("1"), StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
		Line->BuildConnectionIndices();

		const FString ScriptName = TEXT("localization_line");
		Proj->Scripts.Add(ScriptName, Line);
		W.Component->StartDialogueWithScript(ScriptName);
		const FStoryFlowDialogueState State = W.Component->GetCurrentDialogue();
		OutTitle = State.Title;
		OutText = State.Text;
		OutSpeaker = State.Character.Name;
		W.Component->StopDialogue();
		Proj->Scripts.Remove(ScriptName);
	};

	auto FirstLine = [&RenderLine, &Project](FString& OutTitle, FString& OutText, FString& OutSpeaker)
	{
		RenderLine(Project, OutTitle, OutText, OutSpeaker);
	};

	FString Title, Text, Speaker;

	// Import defaults to the SOURCE language, and the roster is source-first.
	TestEqual(TEXT("the game starts in the project's source language"), W.Subsystem->GetLanguage(), FString(TEXT("en")));
	{
		const TArray<FStoryFlowLanguage> Languages = W.Subsystem->GetLanguages();
		if (TestEqual(TEXT("the roster is the source language plus the author's registry"), Languages.Num(), 3))
		{
			TestEqual(TEXT("the source language comes first"), Languages[0].Code, FString(TEXT("en")));
			TestEqual(TEXT("and its label is its own code"), Languages[0].Name, FString(TEXT("en")));
			TestEqual(TEXT("then the registry order, first code"), Languages[1].Code, FString(TEXT("fr")));
			TestEqual(TEXT("with the author's label"), Languages[1].Name, FString(TEXT("French")));
			TestEqual(TEXT("then the registry order, second code"), Languages[2].Code, FString(TEXT("es")));
			TestEqual(TEXT("with the author's label"), Languages[2].Name, FString(TEXT("Spanish")));
		}
	}

	FirstLine(Title, Text, Speaker);
	TestEqual(TEXT("the source line's title"), Title, FString(TEXT("Greeting")));
	TestEqual(TEXT("the source line's text"), Text, FString(TEXT("Well met.")));
	TestEqual(TEXT("the source speaker name"), Speaker, FString(TEXT("Sir Roland")));

	// Case-insensitive in, canonical casing out — a code IS a file name.
	TestTrue(TEXT("SetLanguage accepts a registered code in any casing"), W.Subsystem->SetLanguage(TEXT("FR")));
	TestEqual(TEXT("and reports the canonical casing back"), W.Subsystem->GetLanguage(), FString(TEXT("fr")));

	FirstLine(Title, Text, Speaker);
	TestEqual(TEXT("an OUTDATED row ships the old translation, not the new source"), Title, FString(TEXT("Salutations")));
	TestEqual(TEXT("a translated row ships the translation"), Text, FString(TEXT("Bien le bonjour.")));
	TestEqual(TEXT("the speaker name flips with it"), Speaker, FString(TEXT("Sire Roland")));

	// AN UNKNOWN CODE IS A NO-OP. Not a fall back to the default: a typo must never move the
	// player out of the language they picked.
	TestFalse(TEXT("SetLanguage refuses a code this project does not carry"), W.Subsystem->SetLanguage(TEXT("de")));
	TestEqual(TEXT("and leaves the player where they were"), W.Subsystem->GetLanguage(), FString(TEXT("fr")));
	FirstLine(Title, Text, Speaker);
	TestEqual(TEXT("the refused switch changed nothing on screen"), Title, FString(TEXT("Salutations")));

	// An EMPTY code is refused on the same rung rather than read as "the default".
	TestFalse(TEXT("SetLanguage refuses an empty code"), W.Subsystem->SetLanguage(FString()));
	TestEqual(TEXT("and still leaves the player where they were"), W.Subsystem->GetLanguage(), FString(TEXT("fr")));

	// The second language reads its OWN table: the id French serves outdated is Done here.
	TestTrue(TEXT("SetLanguage switches to the second language"), W.Subsystem->SetLanguage(TEXT("es")));
	FirstLine(Title, Text, Speaker);
	TestEqual(TEXT("per-language tables are independent (title)"), Title, FString(TEXT("Saludo")));
	TestEqual(TEXT("per-language tables are independent (text)"), Text, FString(TEXT("Well met.")));

	// Back to the source language: it is a legitimate choice with no table of its own.
	TestTrue(TEXT("the source language is settable"), W.Subsystem->SetLanguage(TEXT("en")));
	FirstLine(Title, Text, Speaker);
	TestEqual(TEXT("the source text comes back"), Title, FString(TEXT("Greeting")));
	TestEqual(TEXT("and so does the source speaker name"), Speaker, FString(TEXT("Sir Roland")));

	// NEVER UNDEFINED: a value that keyed no table anywhere is its own text.
	TestEqual(TEXT("an unkeyed value resolves to itself in the source language"),
		Project->GetGlobalString(TEXT("nothing keyed this"), TEXT("en")), FString(TEXT("nothing keyed this")));
	TestEqual(TEXT("and in a target language too"),
		Project->GetGlobalString(TEXT("nothing keyed this"), TEXT("fr")), FString(TEXT("nothing keyed this")));

	// A PRE-LOCALIZATION project: no roster, no switching, and the language it was already on.
	{
		TestTrue(TEXT("the source-only build folder is writable"), WriteContractBuildDir(SourceOnlyBuildDir(), /*bWithLocalization=*/ false));
		UEditorAssetLibrary::DeleteDirectory(SourceOnlyTestRoot);
		UStoryFlowProjectAsset* SourceOnly = UStoryFlowImporter::ImportProject(SourceOnlyBuildDir(), SourceOnlyTestRoot);
		FGCObjectScopeGuard SourceOnlyGuard(SourceOnly);
		if (TestNotNull(TEXT("the source-only project imports"), SourceOnly))
		{
			W.Subsystem->SetProject(SourceOnly);
			TestFalse(TEXT("it is not a localized project"), SourceOnly->bHasLocalization);
			TestEqual(TEXT("it offers no languages to pick from"), W.Subsystem->GetLanguages().Num(), 0);
			TestEqual(TEXT("and reads in the pre-localization default"), W.Subsystem->GetLanguage(), FString(TEXT("en")));
			TestFalse(TEXT("SetLanguage cannot move it off source"), W.Subsystem->SetLanguage(TEXT("fr")));
			TestTrue(TEXT("its own source language is still settable"), W.Subsystem->SetLanguage(TEXT("en")));

			RenderLine(SourceOnly, Title, Text, Speaker);
			TestEqual(TEXT("a pre-localization export renders exactly as it always did (title)"), Title, FString(TEXT("Greeting")));
			TestEqual(TEXT("a pre-localization export renders exactly as it always did (text)"), Text, FString(TEXT("Well met.")));
			TestEqual(TEXT("a pre-localization export renders exactly as it always did (speaker)"), Speaker, FString(TEXT("Sir Roland")));
		}
	}

	ContractCleanUp();
	return true;
}

// ============================================================================
// The language-changed event (design doc 2026-09-04)
// ============================================================================

void UStoryFlowLanguageAccumulator::OnLanguageChanged(const FString& LanguageCode)
{
	Codes.Add(LanguageCode);
	Observed.Add(Subsystem ? Subsystem->GetLanguage() : FString());
	RosterSizes.Add(Subsystem ? Subsystem->GetLanguages().Num() : -1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLanguageChangedEventTest,
	"StoryFlow.CharacterContract.LanguageChangedEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

/**
 * The event fires when the language ACTUALLY MOVES and never otherwise.
 *
 * The silent cases are a refused code, a no-op re-set, and an install that carries the player's
 * choice forward. The loud one is the install that SNAPS — the incoming project cannot carry the
 * code the player was on, so the language falls back to that project's source language, which is
 * a real change to what they are reading.
 *
 * That arm also pins WHERE the broadcast sits. The install sets the language FIRST, because
 * everything seeded below resolves its strings in it, so a broadcast at that line would hand a
 * handler the new language over the outgoing project's data. The roster check is what observes
 * the difference: an unlocalized project answers an empty GetLanguages(), the outgoing localized
 * one answered three rows.
 */
bool FStoryFlowLanguageChangedEventTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterContractTestHelpers;

	AddExpectedError(TEXT("SetLanguage - unknown language"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Source file not found"), EAutomationExpectedErrorFlags::Contains, 0);

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	UStoryFlowLanguageAccumulator* Spy = NewObject<UStoryFlowLanguageAccumulator>(GetTransientPackage());
	FGCObjectScopeGuard SpyGuard(Spy);
	Spy->Subsystem = W.Subsystem;
	W.Subsystem->OnLanguageChanged.AddDynamic(Spy, &UStoryFlowLanguageAccumulator::OnLanguageChanged);

	UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
	if (!TestTrue(TEXT("the contract build folder is writable"), WriteContractBuildDir(ContractBuildDir(), /*bWithLocalization=*/ true)))
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

	// CASE 5: an install that MOVES NOTHING is silent. The subsystem starts on "en" and this
	// project's source language is "en", so nothing changed and nothing should broadcast.
	W.Subsystem->SetProject(Project);
	TestEqual(TEXT("an install that moves nothing broadcasts nothing"), Spy->Codes.Num(), 0);
	TestEqual(TEXT("and it left the game in the project's source language"), W.Subsystem->GetLanguage(), FString(TEXT("en")));

	// CASE 1: a real change fires exactly once, with the new code.
	TestTrue(TEXT("the engine accepts fr"), W.Subsystem->SetLanguage(TEXT("fr")));
	if (TestEqual(TEXT("a real change broadcasts once"), Spy->Codes.Num(), 1))
	{
		TestEqual(TEXT("carrying the new code"), Spy->Codes[0], FString(TEXT("fr")));
		// CASE 4: the field is assigned BEFORE the broadcast.
		TestEqual(TEXT("and GetLanguage already answers it during the broadcast"), Spy->Observed[0], FString(TEXT("fr")));
	}

	// CASE 3: a no-op is not a change.
	TestTrue(TEXT("re-setting the active language still returns true"), W.Subsystem->SetLanguage(TEXT("fr")));
	TestEqual(TEXT("but broadcasts nothing"), Spy->Codes.Num(), 1);

	// CASE 2: a refusal is not a change.
	TestFalse(TEXT("an unknown code is refused"), W.Subsystem->SetLanguage(TEXT("de")));
	TestFalse(TEXT("an empty code is refused"), W.Subsystem->SetLanguage(TEXT("")));
	TestEqual(TEXT("and neither broadcasts"), Spy->Codes.Num(), 1);
	TestEqual(TEXT("the player is still in the language they picked"), W.Subsystem->GetLanguage(), FString(TEXT("fr")));

	// CASE 6: THE SNAP. An unlocalized project cannot carry "fr", so the install moves the
	// language to that project's source language — and THAT one broadcasts, because it is a real
	// change to what the player is reading.
	//
	// It gets its OWN build dir and asset root, the way FStoryFlowLocalizationApiTest's
	// source-only arm does: importing a second project over the first one's root would have the
	// two packages' assets share a folder.
	if (TestTrue(TEXT("the source-only build folder is writable"), WriteContractBuildDir(SourceOnlyBuildDir(), /*bWithLocalization=*/ false)))
	{
		UStoryFlowProjectAsset* Plain = UStoryFlowImporter::ImportProject(SourceOnlyBuildDir(), SourceOnlyTestRoot);
		if (TestNotNull(TEXT("the unlocalized package imports"), Plain))
		{
			FGCObjectScopeGuard PlainGuard(Plain);
			W.Subsystem->SetProject(Plain);
			TestNotEqual(TEXT("the install really did snap the language, so the case has teeth"),
				W.Subsystem->GetLanguage(), FString(TEXT("fr")));
			if (TestEqual(TEXT("an install that snaps the language broadcasts once"), Spy->Codes.Num(), 2))
			{
				TestEqual(TEXT("carrying the code it snapped to"), Spy->Codes[1], W.Subsystem->GetLanguage());
				TestEqual(TEXT("and GetLanguage already answers it during the broadcast"),
					Spy->Observed[1], W.Subsystem->GetLanguage());
				// The INCOMING project is installed by the time the broadcast lands: an
				// unlocalized project has an empty roster, the outgoing one had three rows.
				TestEqual(TEXT("and the incoming project is the one installed"), Spy->RosterSizes[1], 0);
			}
		}
	}

	W.Subsystem->OnLanguageChanged.RemoveDynamic(Spy, &UStoryFlowLanguageAccumulator::OnLanguageChanged);
	ContractCleanUp();
	return true;
}

// ============================================================================
// The `.sfd` read door (localization spec §2's amendment, 2026-08-27)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetReadDoorTest,
	"StoryFlow.CharacterContract.DataAssetReadDoor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

/**
 * WHAT A BYTE COMPARISON CANNOT SEE. The golden package's `unkeyed` cases already drive the two
 * absence shapes through the real accessors; this drives the PRESENCE shapes through the same
 * doors, because the id a door builds on the way to a table is where every `.sfd` rule is obeyed
 * or broken and a wrong id fails as a WRONG VALUE, not as a miss.
 *
 * Its expectations are computed from the VENDORED sidecar, never written by hand: a hand-written
 * expectation is one more copy of the rule under test, and it would agree with a wrong door as
 * happily as with a right one.
 *
 * The four things it asks, per language, are the four the id rule is made of:
 *   1. the three id shapes — scalar, array element by index, map entry value by key;
 *   2. an override beside the declaration it shadows, which must NOT move while the declaration
 *      does (adjacent on purpose: with no asset segment, walking overrides is a plausible reading
 *      and its damage is invisible unless the two are read together);
 *   3. a DESCENDANT'S OWN declaration, which keys — the half of the rule an asset-segment reading
 *      loses, since it is the variable that is unique, not the asset;
 *   4. the type gate, at the door: an enum whose value is a string stays literal.
 */
bool FStoryFlowDataAssetReadDoorTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterContractTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
	if (!TestTrue(TEXT("the contract build folder is writable"), WriteContractBuildDir(ContractBuildDir(), /*bWithLocalization=*/ true)))
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
	W.Subsystem->SetProject(Project);

	FContractHarness Harness(*this, W);
	Harness.Project = Project;
	Harness.DataAssetsFile = LoadPackageFile(TEXT("data-assets.json"));
	if (!TestTrue(TEXT("data-assets.json is vendored"), Harness.DataAssetsFile.IsValid()))
	{
		ContractCleanUp();
		return false;
	}

	const FString ItemBase = TEXT("da_itembase0000000000000000000000");
	const FString ItemRelic = TEXT("da_itemrelic000000000000000000000");

	for (const TCHAR* Language : { TEXT("fr"), TEXT("es") })
	{
		const FString Code(Language);
		if (!TestTrue(TEXT("the engine accepts the language ") + Code, W.Subsystem->SetLanguage(Code)))
		{
			continue;
		}
		// The sidecar's own answer for an id, through the string chokepoint — the expectation
		// side of every assertion below, derived rather than transcribed.
		const auto Resolved = [&Harness, &Project, &Code](const TCHAR* StringId)
		{
			return Harness.ResolveThroughChokepoint(Project, StringId, Code);
		};

		// 1 + 2. The override and the declaration it shadows, out of ONE store and adjacent.
		const FStoryFlowVariant Override = Harness.ReadDataAsset(Code + TEXT(" override"), ItemRelic, TEXT("v-item-desc"));
		TestEqual(Code + TEXT(": an override ships literal, in every language"),
			Override.GetString(), FString(TEXT("A blade that hums with old grief.")));
		const FStoryFlowVariant Shadowed = Harness.ReadDataAsset(Code + TEXT(" declaration"), ItemBase, TEXT("v-item-desc"));
		TestEqual(Code + TEXT(": the declaration it shadows localizes"), Shadowed.GetString(), Resolved(TEXT("v-item-desc.value")));
		TestNotEqual(Code + TEXT(": and the two are genuinely different texts"), Override.GetString(), Shadowed.GetString());

		// 1. THE THREE ID SHAPES, each read whole, as game code reads it.
		TestEqual(Code + TEXT(": a scalar keys <variableId>.value"),
			Harness.ReadDataAsset(Code + TEXT(" scalar"), ItemBase, TEXT("v-item-name")).GetString(), Resolved(TEXT("v-item-name.value")));

		const FStoryFlowVariant Tags = Harness.ReadDataAsset(Code + TEXT(" array"), ItemBase, TEXT("v-item-tags"));
		if (TestEqual(Code + TEXT(": the array read carries both elements"), Tags.GetArray().Num(), 2))
		{
			TestEqual(Code + TEXT(": element 0 keys .value.0"), Tags.GetArray()[0].GetString(), Resolved(TEXT("v-item-tags.value.0")));
			TestEqual(Code + TEXT(": element 1 keys .value.1"), Tags.GetArray()[1].GetString(), Resolved(TEXT("v-item-tags.value.1")));
		}

		const FStoryFlowVariant Slots = Harness.ReadDataAsset(Code + TEXT(" map"), ItemBase, TEXT("v-item-slots"));
		if (TestEqual(Code + TEXT(": the map read carries both entries"), Slots.GetMap().Num(), 2))
		{
			// KEYS ARE IDENTIFIERS and never resolve, whatever the keyType — asserted here and not
			// only implied, because a lookup over a key is the mistake that costs a map its shape.
			TestEqual(Code + TEXT(": the first key is untouched"), Slots.GetMap()[0].Key.GetString(), FString(TEXT("hand")));
			TestEqual(Code + TEXT(": its value keys .value.hand"), Slots.GetMap()[0].Value.GetString(), Resolved(TEXT("v-item-slots.value.hand")));
			TestEqual(Code + TEXT(": the second key is untouched"), Slots.GetMap()[1].Key.GetString(), FString(TEXT("back")));
			TestEqual(Code + TEXT(": its value keys .value.back"), Slots.GetMap()[1].Value.GetString(), Resolved(TEXT("v-item-slots.value.back")));
		}

		// 3. A DESCENDANT'S OWN declaration keys, on the same chain as the base's.
		TestEqual(Code + TEXT(": a descendant's own declaration keys too"),
			Harness.ReadDataAsset(Code + TEXT(" descendant"), ItemRelic, TEXT("v-relic-oath")).GetString(), Resolved(TEXT("v-relic-oath.value")));

		// 4. THE TYPE GATE at the door: the declared type decides, not the value's shape.
		TestEqual(Code + TEXT(": an enum whose value is a string stays literal"),
			Harness.ReadDataAsset(Code + TEXT(" enum"), ItemBase, TEXT("v-item-rarity")).GetString(), FString(TEXT("Common")));
	}

	// THE FR/ES DIVERGENCE, in one assertion: the same id answers differently in the two
	// languages, so a door that resolved once and cached across a language switch fails here.
	TestTrue(TEXT("the language actually moves the value"), W.Subsystem->SetLanguage(TEXT("fr")));
	bool bFound = false;
	const FString FrenchName = Harness.HostReadDataAsset(ItemBase, TEXT("v-item-name"), bFound).GetString();
	TestTrue(TEXT("the fr read is found"), bFound);
	TestTrue(TEXT("the engine accepts es"), W.Subsystem->SetLanguage(TEXT("es")));
	const FString SpanishName = Harness.HostReadDataAsset(ItemBase, TEXT("v-item-name"), bFound).GetString();
	TestNotEqual(TEXT("a mid-session language switch reaches the very next .sfd read"), FrenchName, SpanishName);
	TestEqual(TEXT("fr serves the translation"), FrenchName, FString(TEXT("Epee de fer")));
	TestEqual(TEXT("es has no row, so it serves the source"), SpanishName, FString(TEXT("Iron Sword")));

	ContractCleanUp();
	return true;
}

// ============================================================================
// seedVsWritten: the rule the golden package STATES but does not pin
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetSeedVsWrittenTest,
	"StoryFlow.CharacterContract.DataAssetSeedVsWritten",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

/**
 * manifest.localization.dataAssets.seedVsWritten: a seed localizes, a WRITTEN value never does,
 * including after a save/load, since the save carries the overlay and a restored write was never
 * content. The manifest states the rule and says plainly that NO CASE IN THE PACKAGE PINS IT —
 * each engine owns its own write / save / load / read case rather than reading a green package
 * run as coverage. This is Unreal's, and it walks the real slot pair a game calls.
 *
 * THE SECOND WRITE IS THE POINT. The manifest's warning is that an engine must gate on WHERE A
 * VALUE CAME FROM and never on whether it LOOKS like a key, so this writes a value that is
 * character-for-character a real string-table key. An implementation that resolved anything
 * key-shaped hands back the translation of a string the game has already redefined — and in the
 * source language it looks perfect.
 */
bool FStoryFlowDataAssetSeedVsWrittenTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterContractTestHelpers;

	const TCHAR* SeedVsWrittenSlot = TEXT("StoryFlowSeedVsWrittenSlot");

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	UEditorAssetLibrary::DeleteDirectory(ContractTestRoot);
	if (!TestTrue(TEXT("the contract build folder is writable"), WriteContractBuildDir(ContractBuildDir(), /*bWithLocalization=*/ true)))
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
	W.Subsystem->SetProject(Project);

	UStoryFlowDataAssetAsset* ItemBase = Project->DataAssets.FindRef(TEXT("da_itembase0000000000000000000000"));
	if (!TestNotNull(TEXT("the item-base Data Asset imported"), ItemBase))
	{
		ContractCleanUp();
		return false;
	}

	bool bFound = false;
	TestTrue(TEXT("the engine accepts fr"), W.Subsystem->SetLanguage(TEXT("fr")));

	// The seed, before anything is written: prose, and it localizes.
	TestEqual(TEXT("an untouched declaration localizes"),
		W.Component->GetDataAssetStringVariable(ItemBase, TEXT("Item Name"), bFound), FString(TEXT("Epee de fer")));
	TestTrue(TEXT("and reports found"), bFound);

	// THE WRITE. From here on this variable is live data, not the author's string.
	TestTrue(TEXT("a session write lands"), W.Component->SetDataAssetStringVariable(ItemBase, TEXT("Item Name"), TEXT("Runed Sword")));
	TestEqual(TEXT("and the very next read hands it back verbatim, in fr"),
		W.Component->GetDataAssetStringVariable(ItemBase, TEXT("Item Name"), bFound), FString(TEXT("Runed Sword")));

	// THE KEY-SHAPED WRITE, on the variable beside it: a value that IS a table key, byte for byte.
	TestTrue(TEXT("a key-shaped write lands"), W.Component->SetDataAssetStringVariable(ItemBase, TEXT("Description"), TEXT("v-item-desc.value")));
	TestEqual(TEXT("and is NOT translated, because provenance decides and not shape"),
		W.Component->GetDataAssetStringVariable(ItemBase, TEXT("Description"), bFound), FString(TEXT("v-item-desc.value")));

	TestTrue(TEXT("SaveToSlot succeeds with no dialogue running"), W.Subsystem->SaveToSlot(SeedVsWrittenSlot, 0));

	// A restart drops the session: the seed is back, and translated again.
	W.Subsystem->ResetDataAssetOverlay();
	TestEqual(TEXT("the reset restores the seed, which localizes"),
		W.Component->GetDataAssetStringVariable(ItemBase, TEXT("Item Name"), bFound), FString(TEXT("Epee de fer")));

	// THE LOAD, in a DIFFERENT language than the write was made in.
	TestTrue(TEXT("LoadFromSlot succeeds"), W.Subsystem->LoadFromSlot(SeedVsWrittenSlot, 0));
	TestTrue(TEXT("the engine accepts es"), W.Subsystem->SetLanguage(TEXT("es")));

	TestEqual(TEXT("a restored write is still live data, verbatim, in a third language"),
		W.Component->GetDataAssetStringVariable(ItemBase, TEXT("Item Name"), bFound), FString(TEXT("Runed Sword")));
	TestEqual(TEXT("and the key-shaped one is still not translated"),
		W.Component->GetDataAssetStringVariable(ItemBase, TEXT("Description"), bFound), FString(TEXT("v-item-desc.value")));
	// The counter-assertion that gives the previous line teeth: this is what a shape-gated
	// implementation would have handed back instead.
	TestEqual(TEXT("es really does key that id to somebody's prose"),
		Project->GetGlobalString(TEXT("v-item-desc.value"), TEXT("es")), FString(TEXT("Una hoja sencilla, bien forjada.")));

	// And a declaration the session never touched still localizes after the load — the load
	// restored an OVERLAY, not a whole store, so the seed under it is still content.
	const FStoryFlowVariant Slots = W.Component->GetDataAssetVariantVariable(ItemBase, TEXT("Slots"), bFound);
	TestTrue(TEXT("the untouched map is found"), bFound);
	if (TestEqual(TEXT("and carries both entries"), Slots.GetMap().Num(), 2))
	{
		TestEqual(TEXT("whose values still localize in es"), Slots.GetMap()[1].Value.GetString(), FString(TEXT("En la vaina")));
	}

	UGameplayStatics::DeleteGameInSlot(SeedVsWrittenSlot, 0);
	ContractCleanUp();
	return true;
}

// ============================================================================
// declarationsOnly: the override arm of the gate, in the form THIS engine meets it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataAssetOverrideNeverLocalizesTest,
	"StoryFlow.CharacterContract.DataAssetOverrideNeverLocalizes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

/**
 * WHY THIS EXISTS, WHEN THE GOLDEN PACKAGE ALREADY CARRIES sfd-override-ships-literal.
 *
 * The package's override case cannot fail this plugin, and it is worth writing down why rather
 * than reading its green as coverage. The reference implementation resolves a `.sfd` value by
 * BUILDING an id (`<variableId>.value`) at the door, so localizing an override there immediately
 * serves the ancestor's translation — the bug b18c4de0 fixed. THIS engine never builds an id: the
 * exporter already put the key in the value, and the door resolves the bytes it read. An override
 * ships as a literal, a literal keys nothing, and the ladder's total-lookup tier answers an
 * unkeyed string with itself — so on that package's bytes the gate's Override arm is unobservable,
 * and a mutation that deletes it changes no result.
 *
 * The arm is still load-bearing, because the manifest's collision is about BYTES: the moment an
 * override's stored value IS a shipped string id, a door that localized overrides hands back
 * somebody else's prose. That is exactly what walking overrides onto a bare `<variableId>.value`
 * would produce, which is the thing declarationsOnly forbids. So this builds the smallest seed
 * that carries it — a base declaring two keyed strings, a child overriding one of them with the
 * OTHER'S KEY — and reads it through the real door in a real target language.
 *
 * Inline rather than vendored: the golden package's bytes are the editor's and must not grow a row
 * for one engine's test (the same rule the .sfd node suites already follow for shapes the shared
 * fixtures cannot carry).
 */
bool FStoryFlowDataAssetOverrideNeverLocalizesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowCharacterContractTestHelpers;

	const TCHAR* GateTestRoot = TEXT("/Game/StoryFlowDataAssetGateTests");
	const FString GateBuildDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowDataAssetGate"));

	const TCHAR* DataAssetsBody = TEXT(R"JSON({
  "dataAssets": {
    "da_gatebase": {
      "id": "da_gatebase", "name": "gate-base", "parent": null,
      "variables": [
        { "id": "v-gate-desc", "name": "Description", "type": "string", "value": "v-gate-desc.value" },
        { "id": "v-gate-other", "name": "Other", "type": "string", "value": "v-gate-other.value" }
      ],
      "overrides": {}
    },
    "da_gatechild": {
      "id": "da_gatechild", "name": "gate-child", "parent": "da_gatebase",
      "variables": [],
      "overrides": { "v-gate-desc": "v-gate-other.value" }
    }
  },
  "strings": { "en": { "v-gate-desc.value": "The base description.", "v-gate-other.value": "A different authored line." } }
})JSON");

	const TCHAR* LocalizationBody = TEXT(R"JSON({
  "schemaVersion": "1",
  "sourceLanguage": "en",
  "languages": [ { "code": "fr", "name": "French" } ],
  "strings": { "fr": { "v-gate-desc.value": "La description de base.", "v-gate-other.value": "Une autre ligne." } }
})JSON");

	FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init()))
	{
		return false;
	}

	IFileManager::Get().DeleteDirectory(*GateBuildDir, false, true);
	IFileManager::Get().MakeDirectory(*GateBuildDir, true);
	const bool bWrote = FFileHelper::SaveStringToFile(TEXT(R"JSON({"version":"1.0.0","apiVersion":"1","startupScript":"main"})JSON"),
			*FPaths::Combine(GateBuildDir, TEXT("project.json")))
		&& FFileHelper::SaveStringToFile(DataAssetsBody, *FPaths::Combine(GateBuildDir, TEXT("data-assets.json")))
		&& FFileHelper::SaveStringToFile(LocalizationBody, *FPaths::Combine(GateBuildDir, TEXT("localization.json")));
	if (!TestTrue(TEXT("the gate build folder is writable"), bWrote))
	{
		return false;
	}

	UEditorAssetLibrary::DeleteDirectory(GateTestRoot);
	UStoryFlowProjectAsset* Project = UStoryFlowImporter::ImportProject(GateBuildDir, GateTestRoot);
	if (!TestNotNull(TEXT("the gate seed imports"), Project))
	{
		return false;
	}
	FGCObjectScopeGuard ProjectGuard(Project);
	W.Subsystem->SetProject(Project);

	UStoryFlowDataAssetAsset* Base = Project->DataAssets.FindRef(TEXT("da_gatebase"));
	UStoryFlowDataAssetAsset* Child = Project->DataAssets.FindRef(TEXT("da_gatechild"));
	if (!TestNotNull(TEXT("the base imported"), Base) || !TestNotNull(TEXT("the child imported"), Child))
	{
		UEditorAssetLibrary::DeleteDirectory(GateTestRoot);
		IFileManager::Get().DeleteDirectory(*GateBuildDir, false, true);
		return false;
	}

	bool bFound = false;
	TestTrue(TEXT("the engine accepts fr"), W.Subsystem->SetLanguage(TEXT("fr")));

	// The two DECLARATIONS localize — without this the test could pass on a plugin that localizes
	// nothing at all, which is the failure mode a lone negative assertion always admits.
	TestEqual(TEXT("the base's declared Description localizes"),
		W.Component->GetDataAssetStringVariable(Base, TEXT("Description"), bFound), FString(TEXT("La description de base.")));
	TestEqual(TEXT("and so does the other declaration, whose key the override carries"),
		W.Component->GetDataAssetStringVariable(Base, TEXT("Other"), bFound), FString(TEXT("Une autre ligne.")));

	// THE OVERRIDE. Its stored bytes are a real shipped key, so a door that localized overrides
	// would answer "Une autre ligne." here — the other declaration's prose, served for a text the
	// descendant deliberately replaced. Verbatim is the only right answer.
	const FString ChildDescription = W.Component->GetDataAssetStringVariable(Child, TEXT("Description"), bFound);
	TestTrue(TEXT("the child's Description is found"), bFound);
	TestEqual(TEXT("an override is handed back verbatim, key-shaped or not"), ChildDescription, FString(TEXT("v-gate-other.value")));
	TestNotEqual(TEXT("and specifically NOT the other declaration's translation"), ChildDescription, FString(TEXT("Une autre ligne.")));

	// The script lane must agree, or the rule holds at one surface only.
	FContractHarness Harness(*this, W);
	Harness.Project = Project;
	Harness.DataAssetsFile = nullptr;
	bool bNodeFound = false;
	const FStoryFlowVariant NodeValue = Harness.NodeLaneReadWithSnapshot(TEXT("da_gatechild"), TEXT("v-gate-desc"),
		TEXT("Description"), TEXT("string"), /*bIsArray=*/ false, FString(), FString(), bNodeFound);
	TestTrue(TEXT("the script lane resolves the override too"), bNodeFound);
	TestEqual(TEXT("and hands back the same verbatim bytes"), NodeValue.GetString(), FString(TEXT("v-gate-other.value")));

	UEditorAssetLibrary::DeleteDirectory(GateTestRoot);
	IFileManager::Get().DeleteDirectory(*GateBuildDir, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
