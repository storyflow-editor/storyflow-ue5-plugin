// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowVariantLibrary.h"
#include "EditorAssetLibrary.h"
#include "Engine/GameInstance.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "HAL/FileManager.h"
#include "Import/StoryFlowImporter.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowScopedWorld.h"
#include "Subsystems/StoryFlowSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAArraySourceShapeHardening, "StoryFlow.DataAssets.Hardening.ArraySourceShape",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAArraySourceShapeHardening::RunTest(const FString&)
{
	auto Project = NewObject<UStoryFlowProjectAsset>();
	auto Script = NewObject<UStoryFlowScriptAsset>();
	FStoryFlowNode Source;
	Source.Id = TEXT("source");
	Source.Type = EStoryFlowNodeType::GetCharacterVar;
	Source.Data.CharacterPath = TEXT("hero");
	Source.Data.VariableName = TEXT("array");
	Source.Data.VariableType = TEXT("integer");
	Source.Data.bIsArray = true;
	Script->Nodes.Add(Source.Id, Source);
	FStoryFlowNode Sink;
	Sink.Id = TEXT("sink");
	Sink.Type = EStoryFlowNodeType::SetDataAssetVariable;
	Script->Nodes.Add(Sink.Id, Sink);
	FStoryFlowConnection Edge;
	Edge.Source = Source.Id;
	Edge.Target = Sink.Id;
	Edge.SourceHandle = TEXT("source-source-integer-array-");
	Edge.TargetHandle = TEXT("target-sink-float-array-2");
	Script->Connections.Add(Edge);
	Script->BuildConnectionIndices();
	TMap<FString, FStoryFlowCharacterDef> Characters;
	FStoryFlowVariable Variable;
	Variable.Id = TEXT("array");
	Variable.Type = EStoryFlowVariableType::Integer;
	Variable.bIsArray = true;
	Variable.Value.SetArray({}, Variable.Type);
	Characters.FindOrAdd(TEXT("hero")).Variables.Add(TEXT("array"), Variable);
	FStoryFlowExecutionContext Context;
	Context.InitializeWithSubsystem(Project, Script, nullptr, &Characters);
	Context.bCaptureReadFailures = true;
	FStoryFlowEvaluator Evaluator(&Context);
	Evaluator.EvaluateFloatArrayInput(&Script->Nodes[Sink.Id], TEXT("float-array-2"));
	TestTrue(TEXT("empty character integer array cannot feed float Set"), Context.ReadFailures > 0);
	Script->Nodes[Source.Id].Type = EStoryFlowNodeType::RunScript;
	FStoryFlowScriptInterfaceParam Output;
	Output.Id = TEXT("out");
	Output.Name = TEXT("array");
	Output.Type = TEXT("integer");
	Output.bIsArray = true;
	Script->Nodes[Source.Id].Data.ScriptOutputs.Add(Output);
	Script->Connections[0].SourceHandle = TEXT("source-source-out-out");
	Script->BuildConnectionIndices();
	auto& State = Context.GetNodeState(Source.Id);
	State.bHasOutputValues = true;
	State.OutputValues.Add(TEXT("array"), Variable.Value);
	const uint64 Before = Context.ReadFailures;
	Evaluator.EvaluateFloatArrayInput(&Script->Nodes[Sink.Id], TEXT("float-array-2"));
	TestTrue(TEXT("empty completed integer array cannot feed float Set"), Context.ReadFailures > Before);
	Script->Nodes[Source.Id].Data.ScriptOutputs[0].Type = TEXT("float");
	State.OutputValues[TEXT("array")].SetArray({}, EStoryFlowVariableType::Float);
	const uint64 BeforeValid = Context.ReadFailures;
	Evaluator.EvaluateFloatArrayInput(&Script->Nodes[Sink.Id], TEXT("float-array-2"));
	TestEqual(TEXT("matching empty output stays valid"), Context.ReadFailures, BeforeValid);
	Script->Nodes[Source.Id].Data.ScriptOutputs[0].Type = TEXT("integer");
	Script->Nodes[Source.Id].Data.ScriptOutputs[0].bIsArray = false;
	State.OutputValues.Empty();
	const uint64 BeforeMissingScalar = Context.ReadFailures;
	Evaluator.EvaluateIntegerFromNode(&Script->Nodes[Source.Id], Sink.Id, TEXT("source-source-out-out"));
	TestTrue(TEXT("missing scalar runScript output refuses"), Context.ReadFailures > BeforeMissingScalar);
	State.OutputValues.Add(TEXT("array"), FStoryFlowVariant::FromString(TEXT("wrong")));
	const uint64 BeforeWrongScalar = Context.ReadFailures;
	Evaluator.EvaluateIntegerFromNode(&Script->Nodes[Source.Id], Sink.Id, TEXT("source-source-out-out"));
	TestTrue(TEXT("wrong typed scalar output refuses"), Context.ReadFailures > BeforeWrongScalar);
	State.OutputValues[TEXT("array")].SetArray({}, EStoryFlowVariableType::Integer);
	const uint64 BeforeArrayScalar = Context.ReadFailures;
	Evaluator.EvaluateIntegerFromNode(&Script->Nodes[Source.Id], Sink.Id, TEXT("source-source-out-out"));
	TestTrue(TEXT("array output cannot become scalar zero"), Context.ReadFailures > BeforeArrayScalar);
	State.OutputValues[TEXT("array")].SetInt(0);
	const uint64 BeforeZero = Context.ReadFailures;
	Evaluator.EvaluateIntegerFromNode(&Script->Nodes[Source.Id], Sink.Id, TEXT("source-source-out-out"));
	TestEqual(TEXT("actual scalar zero is valid"), Context.ReadFailures, BeforeZero);
	FStoryFlowVariable Map;
	Map.Id = TEXT("map");
	Map.Type = EStoryFlowVariableType::Map;
	Map.KeyType = EStoryFlowVariableType::String;
	Map.ValueType = EStoryFlowVariableType::Integer;
	Map.Value.SetMap({});
	Context.LocalVariables.Add(Map.Id, Map);
	FStoryFlowNode MapGetter;
	MapGetter.Id = TEXT("mapGetter");
	MapGetter.Type = EStoryFlowNodeType::GetMap;
	MapGetter.Data.Variable = Map.Id;
	MapGetter.Data.KeyType = TEXT("string");
	MapGetter.Data.ValueType = TEXT("integer");
	Script->Nodes.Add(MapGetter.Id, MapGetter);
	Script->Nodes[Source.Id].Data.KeyType = TEXT("string");
	Script->Nodes[Source.Id].Data.ValueType = TEXT("integer");
	FStoryFlowConnection MapEdge;
	MapEdge.Source = MapGetter.Id;
	MapEdge.Target = Source.Id;
	MapEdge.SourceHandle = TEXT("source-mapGetter-map-string-integer");
	MapEdge.TargetHandle = TEXT("target-source-map-string-integer-1");
	Script->Connections.Add(MapEdge);
	Script->BuildConnectionIndices();
	for (EStoryFlowNodeType Projection : {EStoryFlowNodeType::MapKeys, EStoryFlowNodeType::MapValues})
	{
		Script->Nodes[Source.Id].Type = Projection;
		const uint64 BeforeProjection = Context.ReadFailures;
		Evaluator.EvaluateFloatArrayInput(&Script->Nodes[Sink.Id], TEXT("float-array-2"));
		TestTrue(TEXT("empty map projection must retain element type"), Context.ReadFailures > BeforeProjection);
	}
	Script->Nodes[Source.Id].Type = EStoryFlowNodeType::AddToStringArray;
	auto& OperationState = Context.GetNodeState(Source.Id);
	OperationState.CachedOutput.SetArray({}, EStoryFlowVariableType::String);
	OperationState.bHasCachedOutput = true;
	OperationState.bIsExecutionOutput = true;
	const uint64 BeforeOperation = Context.ReadFailures;
	Evaluator.EvaluateFloatArrayInput(&Script->Nodes[Sink.Id], TEXT("float-array-2"));
	TestTrue(TEXT("empty completed array operation retains element type"), Context.ReadFailures > BeforeOperation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAOutsideWriteHardening, "StoryFlow.DataAssets.Hardening.OutsideDialogueWrite",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAOutsideWriteHardening::RunTest(const FString&)
{
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("world"), W.Init()))
	{
		return false;
	}
	auto Project = NewObject<UStoryFlowProjectAsset>();
	FStoryFlowVariable Flag;
	Flag.Id = TEXT("flag");
	Flag.Name = TEXT("flag");
	Flag.Type = EStoryFlowVariableType::Boolean;
	Flag.Value.SetBool(false);
	Project->GlobalVariables.Add(Flag.Id, Flag);
	W.Subsystem->SetProject(Project);
	auto Script = NewObject<UStoryFlowScriptAsset>();
	FStoryFlowNode Getter;
	Getter.Id = TEXT("get");
	Getter.Type = EStoryFlowNodeType::GetBool;
	Getter.Data.Variable = Flag.Id;
	Getter.Data.bIsGlobal = true;
	Script->Nodes.Add(Getter.Id, Getter);
	FStoryFlowExecutionContext Context;
	Context.InitializeWithSubsystem(Project, Script, &W.Subsystem->GetGlobalVariables(),
									&W.Subsystem->GetRuntimeCharacters(), &W.Subsystem->GetUsedOnceOnlyOptions(),
									W.Subsystem->GetDataAssetStore());
	FStoryFlowEvaluator Evaluator(&Context);
	TestFalse(TEXT("reader initial memo"),
			  Evaluator.EvaluateBooleanFromNode(&Script->Nodes[Getter.Id], TEXT(""), TEXT("")));
	W.Component->SetBoolVariable(TEXT("flag"), true, true);
	TestTrue(TEXT("host without its own dialogue invalidates another reader"),
			 Evaluator.EvaluateBooleanFromNode(&Script->Nodes[Getter.Id], TEXT(""), TEXT("")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAMigrationFixtureHardening, "StoryFlow.DataAssets.Hardening.SharedMigrationFixture",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAMigrationFixtureHardening::RunTest(const FString&)
{
	using namespace StoryFlowEngineContract;
	const auto Fixture = LoadFixture(TEXT("data-assets-migration.json"));
	if (!TestTrue(TEXT("shared migration fixture loads"), Fixture.IsValid()))
		return false;
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/StoryFlowMigrationHardening"));
	const FString Content = TEXT("/Game/StoryFlowMigrationHardening");
	ON_SCOPE_EXIT
	{
		UEditorAssetLibrary::DeleteDirectory(Content);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	};
	IFileManager::Get().MakeDirectory(*Dir, true);
	UEditorAssetLibrary::DeleteDirectory(Content);
	FString SeedJson;
	FJsonSerializer::Serialize(Fixture.ToSharedRef(), TJsonWriterFactory<>::Create(&SeedJson));
	TestTrue(TEXT("write seed"),
			 FFileHelper::SaveStringToFile(SeedJson, *FPaths::Combine(Dir, TEXT("data-assets.json"))));
	TestTrue(TEXT("write project"), FFileHelper::SaveStringToFile(
										TEXT("{\"version\":\"1.0.0\",\"apiVersion\":\"1\",\"startupScript\":\"main\"}"),
										*FPaths::Combine(Dir, TEXT("project.json"))));
	auto Project = UStoryFlowImporter::ImportProject(Dir, Content);
	if (!TestNotNull(TEXT("real fixture import"), Project))
		return false;
	auto Subsystem = NewObject<UStoryFlowSubsystem>(NewObject<UGameInstance>());
	Subsystem->SetProject(Project);
	auto Saved = MakeShared<FJsonObject>();
	Saved->SetObjectField(TEXT("dataAssets"), Fixture->GetObjectField(TEXT("saved")));
	FString SavedJson;
	FJsonSerializer::Serialize(Saved, TJsonWriterFactory<>::Create(&SavedJson));
	TestTrue(TEXT("real overlay restore"),
			 StoryFlowSaveHelpers::DeserializeSaveData(
				 SavedJson, Subsystem->GetGlobalVariables(), Subsystem->GetRuntimeCharacters(),
				 Subsystem->GetUsedOnceOnlyOptions(), Subsystem->GetDataAssetSeed(), Subsystem->GetDataAssetOverlay()));
	const FString RestoredJson = StoryFlowSaveHelpers::SerializeSaveData(
		Subsystem->GetGlobalVariables(), Subsystem->GetRuntimeCharacters(), Subsystem->GetUsedOnceOnlyOptions(),
		Subsystem->GetDataAssetSeed(), Subsystem->GetDataAssetOverlay());
	TSharedPtr<FJsonObject> Restored;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RestoredJson), Restored);
	JsonEquals(*this, TEXT("compatible overlay only"), Restored->TryGetField(TEXT("dataAssets")),
			   Fixture->TryGetField(TEXT("expectedOverlay")));
	for (const auto& Read : Fixture->GetArrayField(TEXT("expectedReads")))
	{
		const auto Row = Read->AsObject();
		const FString AssetId = Row->GetStringField(TEXT("assetId"));
		const FString VariableId = Row->GetStringField(TEXT("variableId"));
		FStoryFlowVariant Value;
		TestTrue(TEXT("resolved inherited/default read"),
				 StoryFlowDataAssets::TryResolve(Subsystem->GetDataAssetSeed(), Subsystem->GetDataAssetOverlay(),
												 AssetId, VariableId, Value));
		VariantMatchesJson(*this, AssetId + TEXT(".") + VariableId, Value, Row->TryGetField(TEXT("value")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAExecutionOutputHardening,
								 "StoryFlow.DataAssets.Hardening.ExecutionOutputSurvivesWrite",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAExecutionOutputHardening::RunTest(const FString&)
{
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("world"), W.Init()))
		return false;
	auto Project = NewObject<UStoryFlowProjectAsset>();
	auto Asset = NewObject<UStoryFlowDataAssetAsset>();
	Asset->AssetId = TEXT("asset");
	Project->DataAssets.Add(Asset->AssetId, Asset);
	FStoryFlowVariable Items;
	Items.Id = TEXT("items");
	Items.Name = TEXT("items");
	Items.Type = EStoryFlowVariableType::String;
	Items.bIsArray = true;
	Items.Value.SetArray({FStoryFlowVariant::FromString(TEXT("sword"))}, EStoryFlowVariableType::String);
	Asset->Variables.Add(Items);
	FStoryFlowVariable Count;
	Count.Id = TEXT("count");
	Count.Name = TEXT("count");
	Count.Type = EStoryFlowVariableType::Integer;
	Count.Value.SetInt(7);
	Asset->Variables.Add(Count);
	auto Script = NewObject<UStoryFlowScriptAsset>();
	Script->StartNode = TEXT("0");
	auto Add = [&](const TCHAR* Id, EStoryFlowNodeType Type) -> FStoryFlowNode& {
		FStoryFlowNode N;
		N.Id = Id;
		N.Type = Type;
		return Script->Nodes.Add(Id, N);
	};
	Add(TEXT("0"), EStoryFlowNodeType::Start);
	Add(TEXT("pill"), EStoryFlowNodeType::GetDataAsset).Data.AssetId = TEXT("asset");
	auto& Source = Add(TEXT("source"), EStoryFlowNodeType::GetDataAssetVariable);
	Source.Data.VariableId = TEXT("items");
	Source.Data.VariableType = TEXT("string");
	Source.Data.bIsArray = true;
	Add(TEXT("add"), EStoryFlowNodeType::AddToStringArray).Data.Value.SetString(TEXT("shield"));
	auto& OtherSet = Add(TEXT("other"), EStoryFlowNodeType::SetDataAssetVariable);
	OtherSet.Data.VariableId = TEXT("count");
	OtherSet.Data.VariableType = TEXT("integer");
	Add(TEXT("zero"), EStoryFlowNodeType::Plus);
	auto& Set = Add(TEXT("set"), EStoryFlowNodeType::SetDataAssetVariable);
	Set.Data.VariableId = TEXT("items");
	Set.Data.VariableType = TEXT("string");
	Set.Data.bIsArray = true;
	auto Edge = [&](const TCHAR* SourceId, const TCHAR* Target, const TCHAR* SH, const TCHAR* TH) {
		FStoryFlowConnection C;
		C.Source = SourceId;
		C.Target = Target;
		C.SourceHandle = SH;
		C.TargetHandle = TH;
		Script->Connections.Add(C);
	};
	Edge(TEXT("0"), TEXT("add"), TEXT("source-0-"), TEXT("target-add-0"));
	Edge(TEXT("add"), TEXT("other"), TEXT("source-add-1"), TEXT("target-other-0"));
	Edge(TEXT("other"), TEXT("set"), TEXT("source-other-1"), TEXT("target-set-0"));
	Edge(TEXT("pill"), TEXT("other"), TEXT("source-pill-dataAsset-asset"), TEXT("target-other-dataAsset-asset"));
	Edge(TEXT("zero"), TEXT("other"), TEXT("source-zero-integer-"), TEXT("target-other-integer-2"));
	Edge(TEXT("pill"), TEXT("source"), TEXT("source-pill-dataAsset-asset"), TEXT("target-source-dataAsset-asset"));
	Edge(TEXT("pill"), TEXT("set"), TEXT("source-pill-dataAsset-asset"), TEXT("target-set-dataAsset-asset"));
	Edge(TEXT("source"), TEXT("add"), TEXT("source-source-string-array-"), TEXT("target-add-string-array-2"));
	Edge(TEXT("add"), TEXT("set"), TEXT("source-add-string-array-"), TEXT("target-set-string-array-2"));
	Script->BuildConnectionIndices();
	Project->Scripts.Add(TEXT("output"), Script);
	W.Subsystem->SetProject(Project);
	W.Component->StartDialogueWithScript(TEXT("output"));
	bool Found = false;
	auto Value = W.Subsystem->GetDataAssetVariantVariable(Asset, TEXT("items"), Found);
	TestEqual(TEXT("completed add output survives its shared write and can be written back"), Value.GetArray().Num(),
			  2);
	return true;
}

namespace DataAssetHardening
{
struct FFixture
{
	UStoryFlowProjectAsset* Project = NewObject<UStoryFlowProjectAsset>();
	UStoryFlowDataAssetAsset* Asset = NewObject<UStoryFlowDataAssetAsset>();
	UStoryFlowSubsystem* Subsystem = NewObject<UStoryFlowSubsystem>(NewObject<UGameInstance>());
	FFixture()
	{
		Asset->AssetId = TEXT("asset");
		Project->DataAssets.Add(Asset->AssetId, Asset);
		FStoryFlowVariable Map;
		Map.Id = TEXT("rewards");
		Map.Name = TEXT("rewards");
		Map.Type = EStoryFlowVariableType::Map;
		Map.KeyType = EStoryFlowVariableType::Enum;
		Map.ValueType = EStoryFlowVariableType::Integer;
		Map.Value.SetMap({{FStoryFlowVariant::FromString(TEXT("gold")), FStoryFlowVariant::FromInt(1)}});
		Asset->Variables.Add(Map);
		FStoryFlowVariable Text;
		Text.Id = TEXT("title");
		Text.Name = TEXT("title");
		Text.Type = EStoryFlowVariableType::String;
		Text.Value.SetString(TEXT("title.value"));
		Asset->Variables.Add(Text);
		FStoryFlowVariable Array;
		Array.Id = TEXT("items");
		Array.Name = TEXT("items");
		Array.Type = EStoryFlowVariableType::String;
		Array.bIsArray = true;
		Array.Value.SetArray({FStoryFlowVariant::FromString(TEXT("sword"))}, EStoryFlowVariableType::String);
		Asset->Variables.Add(Array);
		Project->GlobalStrings.Add(TEXT("en.title.value"), TEXT("Old project"));
		Subsystem->SetProject(Project);
	}
	bool RoundTrip()
	{
		auto Json = StoryFlowSaveHelpers::SerializeSaveData(
			Subsystem->GetGlobalVariables(), Subsystem->GetRuntimeCharacters(), Subsystem->GetUsedOnceOnlyOptions(),
			Subsystem->GetDataAssetSeed(), Subsystem->GetDataAssetOverlay());
		return StoryFlowSaveHelpers::DeserializeSaveData(
			Json, Subsystem->GetGlobalVariables(), Subsystem->GetRuntimeCharacters(),
			Subsystem->GetUsedOnceOnlyOptions(), Subsystem->GetDataAssetSeed(), Subsystem->GetDataAssetOverlay());
	}
};
} // namespace DataAssetHardening

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAEnumKeyHardening, "StoryFlow.DataAssets.Hardening.EnumKeyRoundTrip",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAEnumKeyHardening::RunTest(const FString&)
{
	DataAssetHardening::FFixture F;
	bool Found = false;
	auto Before = F.Subsystem->GetDataAssetVariantVariable(F.Asset, TEXT("rewards"), Found);
	TArray<FStoryFlowVariant> Keys, Values;
	UStoryFlowVariantLibrary::GetVariantMap(Before, Keys, Values);
	TestTrue(TEXT("read-edit-write works before save"),
			 F.Subsystem->SetDataAssetMapVariable(F.Asset, TEXT("rewards"), Keys, Values));
	TestTrue(TEXT("save round trip succeeds"), F.RoundTrip());
	auto After = F.Subsystem->GetDataAssetVariantVariable(F.Asset, TEXT("rewards"), Found);
	UStoryFlowVariantLibrary::GetVariantMap(After, Keys, Values);
	TestTrue(TEXT("read-edit-write must still work after loading"),
			 F.Subsystem->SetDataAssetMapVariable(F.Asset, TEXT("rewards"), Keys, Values));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDACharacterNameHardening, "StoryFlow.DataAssets.Hardening.BridgedNameLiteral",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDACharacterNameHardening::RunTest(const FString&)
{
	DataAssetHardening::FFixture F;
	auto Character = NewObject<UStoryFlowCharacterAsset>();
	Character->Name = TEXT("hero.cf_name");
	F.Project->Characters.Add(TEXT("hero"), Character);
	F.Project->CharacterIdToPath.Add(TEXT("hero-id"), TEXT("hero"));
	F.Project->GlobalStrings.Add(TEXT("en.hero.cf_name"), TEXT("Knight"));
	F.Subsystem->SetProject(F.Project);
	auto Handle = NewObject<UStoryFlowDataAssetAsset>();
	Handle->AssetId = TEXT("hero-id");
	bool Found = false;
	TestTrue(TEXT("host name write succeeds"),
			 F.Subsystem->SetDataAssetStringVariable(Handle, TEXT("Name"), TEXT("hero.cf_name")));
	TestEqual(TEXT("host name must stay literal"), F.Subsystem->GetDataAssetStringVariable(Handle, TEXT("Name"), Found),
			  FString(TEXT("hero.cf_name")));
	TestTrue(TEXT("name provenance is recorded"), F.Subsystem->GetRuntimeCharacters()[TEXT("hero")].bNameIsLiteral);
	F.Subsystem->GetRuntimeCharacters()[TEXT("hero")].bNameIsLiteral = true;
	TestEqual(TEXT("variant getter respects a character-native literal write"),
			  F.Subsystem->GetDataAssetVariantVariable(Handle, TEXT("Name"), Found).GetString(),
			  FString(TEXT("hero.cf_name")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAProjectSwapHardening, "StoryFlow.DataAssets.Hardening.ProjectSwapRead",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAProjectSwapHardening::RunTest(const FString&)
{
	DataAssetHardening::FFixture F;
	FStoryFlowExecutionContext Context;
	Context.InitializeWithSubsystem(F.Project, nullptr, &F.Subsystem->GetGlobalVariables(),
									&F.Subsystem->GetRuntimeCharacters(), &F.Subsystem->GetUsedOnceOnlyOptions(),
									F.Subsystem->GetDataAssetStore(), &F.Subsystem->GetCharacterIdToPath(),
									&F.Subsystem->GetLanguageRef());
	auto NewProject = NewObject<UStoryFlowProjectAsset>();
	NewProject->DataAssets.Add(F.Asset->AssetId, F.Asset);
	NewProject->GlobalStrings.Add(TEXT("en.title.value"), TEXT("New project"));
	F.Subsystem->SetProject(NewProject);
	bool Found = false;
	TestEqual(TEXT("host reads incoming project"),
			  F.Subsystem->GetDataAssetStringVariable(F.Asset, TEXT("title"), Found), FString(TEXT("New project")));
	FStoryFlowVariant Result;
	TestTrue(TEXT("context can read the incoming seed"),
			 Context.TryResolveDataAsset(TEXT("asset"), TEXT("title"), Result));
	TestEqual(TEXT("context should use the same project generation as its seed"), Result.GetString(),
			  FString(TEXT("New project")));
	F.Subsystem->SetProject(nullptr);
	Context.RefreshSharedState();
	TestFalse(TEXT("clear drops the retained project"), Context.Project.IsValid());
	TestFalse(TEXT("cleared seed cannot read"), Context.TryResolveDataAsset(TEXT("asset"), TEXT("title"), Result));
	NewProject->SourceLanguage = TEXT("de");
	NewProject->GlobalStrings.Add(TEXT("de.title.value"), TEXT("Neues Projekt"));
	F.Subsystem->SetProject(NewProject);
	Context.RefreshSharedState();
	TestTrue(TEXT("project rebound after clear"), Context.Project.Get() == NewProject);
	TestTrue(TEXT("incoming source-language read"), Context.TryResolveDataAsset(TEXT("asset"), TEXT("title"), Result));
	TestEqual(TEXT("incoming source text"), Result.GetString(), FString(TEXT("Neues Projekt")));
	NewProject->bHasLocalization = true;
	FStoryFlowLanguage French;
	French.Code = TEXT("fr");
	NewProject->Languages.Add(French);
	NewProject->LanguageStrings.FindOrAdd(TEXT("fr")).Entries.Add(TEXT("title.value"), TEXT("Nouveau projet"));
	TestTrue(TEXT("incoming language available"), F.Subsystem->SetLanguage(TEXT("fr")));
	TestTrue(TEXT("incoming translated read"), Context.TryResolveDataAsset(TEXT("asset"), TEXT("title"), Result));
	TestEqual(TEXT("incoming translated text"), Result.GetString(), FString(TEXT("Nouveau projet")));
	FStoryFlowVariable Global;
	Global.Id = TEXT("same-id");
	Global.Name = TEXT("old-name");
	F.Subsystem->GetGlobalVariables().Add(Global.Id, Global);
	F.Subsystem->NotifySharedStateChanged();
	TestNotNull(TEXT("initial global name index"), Context.FindVariableByName(TEXT("old-name"), true));
	F.Subsystem->GetGlobalVariables()[Global.Id].Name = TEXT("new-name");
	F.Subsystem->NotifySharedStateChanged();
	TestNull(TEXT("changed names invalidate old index"), Context.FindVariableByName(TEXT("old-name"), true));
	TestNotNull(TEXT("changed name resolves"), Context.FindVariableByName(TEXT("new-name"), true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDANestedArrayHardening, "StoryFlow.DataAssets.Hardening.ContainerElementShape",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDANestedArrayHardening::RunTest(const FString&)
{
	DataAssetHardening::FFixture F;
	FStoryFlowVariant Nested;
	Nested.SetArray({FStoryFlowVariant::FromString(TEXT("shield"))}, EStoryFlowVariableType::String);
	TestFalse(TEXT("array-of-arrays cannot be stored in a string array declaration"),
			  F.Subsystem->SetDataAssetArrayVariable(F.Asset, TEXT("items"), {Nested}));
	Nested.SetArray({}, EStoryFlowVariableType::String);
	TestFalse(TEXT("empty nested arrays are still arrays"),
			  F.Subsystem->SetDataAssetArrayVariable(F.Asset, TEXT("items"), {Nested}));
	Nested.SetString(TEXT("scalar"));
	TestTrue(TEXT("a scalar setter replaces prior array shape"),
			 F.Subsystem->SetDataAssetArrayVariable(F.Asset, TEXT("items"), {Nested}));
	Nested.SetArray({FStoryFlowVariant::FromString(TEXT("old"))}, EStoryFlowVariableType::String);
	Nested.SetString(TEXT("scalar"));
	Nested.PackArrayForSerialization();
	Nested.UnpackArrayFromSerialization();
	TestFalse(TEXT("serialized scalar residue does not become array shape"), Nested.IsArray());
	TestTrue(TEXT("serialized scalar still valid as an element"),
			 F.Subsystem->SetDataAssetArrayVariable(F.Asset, TEXT("items"), {Nested}));
	Nested.SetArray({}, EStoryFlowVariableType::String);
	TestFalse(
		TEXT("nested empty array cannot be an enum map key"),
		F.Subsystem->SetDataAssetMapVariable(F.Asset, TEXT("rewards"), {Nested}, {FStoryFlowVariant::FromInt(1)}));
	Nested.SetArray({}, EStoryFlowVariableType::Integer);
	TestFalse(TEXT("nested empty array cannot be an integer map value"),
			  F.Subsystem->SetDataAssetMapVariable(F.Asset, TEXT("rewards"),
												   {FStoryFlowVariant::FromString(TEXT("gold"))}, {Nested}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAMigrationHardening, "StoryFlow.DataAssets.Hardening.SaveMigration",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAMigrationHardening::RunTest(const FString&)
{
	DataAssetHardening::FFixture F;
	FStoryFlowVariable Count;
	Count.Id = TEXT("count");
	Count.Name = TEXT("count");
	Count.Type = EStoryFlowVariableType::Integer;
	Count.Value.SetInt(7);
	F.Asset->Variables.Add(Count);
	F.Asset->Variables[0].KeyEnumValues = {TEXT("gold")};
	F.Subsystem->SetProject(F.Project);
	const TArray<FString> Invalid = {TEXT("\"old text\""), TEXT("1.5"),		   TEXT("true"),	   TEXT("null"),
									 TEXT("[]"),		   TEXT("2147483648"), TEXT("-2147483649")};
	for (const FString& Token : Invalid)
	{
		const FString Json =
			TEXT("{\"dataAssets\":{\"asset\":{\"count\":") + Token +
			TEXT(",\"title\":\"literal\",\"items\":[\"ok\",4],\"rewards\":[{\"key\":\"silver\",\"value\":1}]}}}");
		TestTrue(TEXT("save remains loadable"),
				 StoryFlowSaveHelpers::DeserializeSaveData(
					 Json, F.Subsystem->GetGlobalVariables(), F.Subsystem->GetRuntimeCharacters(),
					 F.Subsystem->GetUsedOnceOnlyOptions(), F.Subsystem->GetDataAssetSeed(),
					 F.Subsystem->GetDataAssetOverlay()));
		const auto& Slots = F.Subsystem->GetDataAssetOverlay()[TEXT("asset")];
		TestFalse(TEXT("incompatible scalar drops rather than coercing"), Slots.Contains(TEXT("count")));
		TestFalse(TEXT("one invalid array element drops entire slot"), Slots.Contains(TEXT("items")));
		TestFalse(TEXT("removed enum option drops entire map slot"), Slots.Contains(TEXT("rewards")));
		TestTrue(TEXT("compatible sibling survives"), Slots.Contains(TEXT("title")));
		bool Found = false;
		TestEqual(TEXT("default revealed"), F.Subsystem->GetDataAssetIntVariable(F.Asset, TEXT("count"), Found), 7);
	}
	const FString Valid = TEXT("{\"dataAssets\":{\"asset\":{\"count\":0,\"items\":[],\"rewards\":[]}}}");
	TestTrue(TEXT("zero/empty restore"), StoryFlowSaveHelpers::DeserializeSaveData(
											 Valid, F.Subsystem->GetGlobalVariables(),
											 F.Subsystem->GetRuntimeCharacters(), F.Subsystem->GetUsedOnceOnlyOptions(),
											 F.Subsystem->GetDataAssetSeed(), F.Subsystem->GetDataAssetOverlay()));
	TestEqual(TEXT("all compatible empty slots retained"), F.Subsystem->GetDataAssetOverlay()[TEXT("asset")].Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAUnresolvedSetHardening, "StoryFlow.DataAssets.Hardening.UnresolvedSetSource",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAUnresolvedSetHardening::RunTest(const FString&)
{
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("world"), W.Init()))
		return false;
	DataAssetHardening::FFixture F;
	FStoryFlowVariable Count;
	Count.Id = TEXT("count");
	Count.Name = TEXT("count");
	Count.Type = EStoryFlowVariableType::Integer;
	Count.Value.SetInt(7);
	F.Asset->Variables.Add(Count);
	auto Script = NewObject<UStoryFlowScriptAsset>();
	Script->StartNode = TEXT("0");
	auto Add = [&](const TCHAR* Id, EStoryFlowNodeType Type) -> FStoryFlowNode& {
		FStoryFlowNode N;
		N.Id = Id;
		N.Type = Type;
		return Script->Nodes.Add(Id, N);
	};
	Add(TEXT("0"), EStoryFlowNodeType::Start);
	Add(TEXT("pill"), EStoryFlowNodeType::GetDataAsset).Data.AssetId = TEXT("asset");
	auto& Set = Add(TEXT("set"), EStoryFlowNodeType::SetDataAssetVariable);
	Set.Data.VariableId = TEXT("count");
	Set.Data.VariableType = TEXT("integer");
	Add(TEXT("source"), EStoryFlowNodeType::GetInt).Data.Variable = TEXT("removed");
	auto Edge = [&](const TCHAR* Source, const TCHAR* Target, const TCHAR* SH, const TCHAR* TH) {
		FStoryFlowConnection C;
		C.Source = Source;
		C.Target = Target;
		C.SourceHandle = SH;
		C.TargetHandle = TH;
		Script->Connections.Add(C);
	};
	Edge(TEXT("0"), TEXT("set"), TEXT("source-0-"), TEXT("target-set-0"));
	Edge(TEXT("pill"), TEXT("set"), TEXT("source-pill-dataAsset-asset"), TEXT("target-set-dataAsset-asset"));
	Edge(TEXT("source"), TEXT("set"), TEXT("source-source-integer-"), TEXT("target-set-integer-2"));
	Script->BuildConnectionIndices();
	F.Project->Scripts.Add(TEXT("sourcecheck"), Script);
	W.Subsystem->SetProject(F.Project);
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("missing ordinary source must not write"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	FStoryFlowVariable Zero = Count;
	Zero.Id = TEXT("removed");
	Zero.Value.SetInt(0);
	Script->Variables.Add(Zero.Id, Zero);
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	bool Found = false;
	TestEqual(TEXT("legitimate zero writes"), W.Subsystem->GetDataAssetIntVariable(F.Asset, TEXT("count"), Found), 0);
	W.Component->StopDialogue();
	W.Subsystem->ResetDataAssetOverlay();
	Script->Variables.Empty();
	Script->Nodes[TEXT("source")].Type = EStoryFlowNodeType::Plus;
	Script->Nodes[TEXT("source")].Data.Value1.SetInt(2);
	Script->Nodes[TEXT("source")].Data.Value2.SetInt(3);
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("unwired expression fallback is legitimate"),
			  W.Subsystem->GetDataAssetIntVariable(F.Asset, TEXT("count"), Found), 5);
	W.Component->StopDialogue();
	W.Subsystem->ResetDataAssetOverlay();
	Add(TEXT("missing"), EStoryFlowNodeType::GetInt).Data.Variable = TEXT("removed");
	Edge(TEXT("missing"), TEXT("source"), TEXT("source-missing-integer-"), TEXT("target-source-integer-1"));
	Script->BuildConnectionIndices();
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("evaluated missing getter inside expression refuses"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	Script->Nodes[TEXT("source")].Type = EStoryFlowNodeType::GetDataAssetVariable;
	Script->Nodes[TEXT("source")].Data.VariableId = TEXT("deleted");
	Script->Nodes[TEXT("source")].Data.VariableType = TEXT("integer");
	Edge(TEXT("pill"), TEXT("source"), TEXT("source-pill-dataAsset-asset"), TEXT("target-source-dataAsset-asset"));
	Script->BuildConnectionIndices();
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("deleted upstream data declaration refuses"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	Script->Nodes[TEXT("source")].Type = EStoryFlowNodeType::GetCharacterVar;
	Script->Nodes[TEXT("source")].Data.CharacterPath = TEXT("hero");
	Script->Nodes[TEXT("source")].Data.VariableName = TEXT("gone");
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("deleted character source refuses"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	W.Subsystem->GetRuntimeCharacters().Add(TEXT("hero"), FStoryFlowCharacterDef());
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("deleted character variable source refuses"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	FStoryFlowVariable Map;
	Map.Id = TEXT("sourceMap");
	Map.Type = EStoryFlowVariableType::Map;
	Map.KeyType = EStoryFlowVariableType::String;
	Map.ValueType = EStoryFlowVariableType::String;
	Map.Value.SetMap({{FStoryFlowVariant::FromString(TEXT("gold")), FStoryFlowVariant::FromString(TEXT("bad"))}});
	Script->Variables.Add(Map.Id, Map);
	auto& MapSource = Script->Nodes[TEXT("source")];
	MapSource.Type = EStoryFlowNodeType::GetMap;
	MapSource.Data.Variable = Map.Id;
	MapSource.Data.KeyType = TEXT("string");
	MapSource.Data.ValueType = TEXT("string");
	auto& MapSet = Script->Nodes[TEXT("set")];
	MapSet.Data.VariableId = TEXT("rewards");
	MapSet.Data.VariableType = TEXT("map");
	MapSet.Data.KeyType = TEXT("enum");
	MapSet.Data.ValueType = TEXT("integer");
	Script->Connections[2].TargetHandle = TEXT("target-set-map-enum-integer-2");
	Script->BuildConnectionIndices();
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("map key/value mismatch refuses entire write"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	Map.KeyType = EStoryFlowVariableType::Enum;
	Map.ValueType = EStoryFlowVariableType::Integer;
	Map.Value.SetMap({{FStoryFlowVariant::FromString(TEXT("gold")), FStoryFlowVariant::FromInt(0)}});
	Script->Variables.Add(Map.Id, Map);
	MapSource.Data.KeyType = TEXT("enum");
	MapSource.Data.ValueType = TEXT("integer");
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("matching map with zero value writes"), W.Subsystem->GetDataAssetOverlay().Num(), 1);
	W.Component->StopDialogue();
	W.Subsystem->ResetDataAssetOverlay();
	FStoryFlowVariable Array;
	Array.Id = TEXT("sourceArray");
	Array.Type = EStoryFlowVariableType::String;
	Array.bIsArray = true;
	Array.Value.SetArray({}, EStoryFlowVariableType::String);
	Script->Variables.Add(Array.Id, Array);
	MapSource.Type = EStoryFlowNodeType::GetStringArray;
	MapSource.Data.Variable = Array.Id;
	MapSet.Data.VariableId = TEXT("items");
	MapSet.Data.VariableType = TEXT("string");
	MapSet.Data.bIsArray = true;
	Script->Connections[2].TargetHandle = TEXT("target-set-string-array-2");
	Script->BuildConnectionIndices();
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("matching empty array writes"), W.Subsystem->GetDataAssetOverlay().Num(), 1);
	W.Component->StopDialogue();
	W.Subsystem->ResetDataAssetOverlay();
	Script->Variables.Remove(Array.Id);
	W.Component->StartDialogueWithScript(TEXT("sourcecheck"));
	TestEqual(TEXT("missing array source refuses"), W.Subsystem->GetDataAssetOverlay().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDACacheHardening, "StoryFlow.DataAssets.Hardening.SubsystemWriteBooleanMemo",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDACacheHardening::RunTest(const FString&)
{
	DataAssetHardening::FFixture F;
	FStoryFlowVariable Alive;
	Alive.Id = TEXT("alive");
	Alive.Name = TEXT("alive");
	Alive.Type = EStoryFlowVariableType::Boolean;
	Alive.Value.SetBool(false);
	F.Asset->Variables.Add(Alive);
	F.Subsystem->SetProject(F.Project);
	auto Script = NewObject<UStoryFlowScriptAsset>();
	FStoryFlowNode Pill;
	Pill.Id = TEXT("pill");
	Pill.Type = EStoryFlowNodeType::GetDataAsset;
	Pill.Data.AssetId = TEXT("asset");
	Script->Nodes.Add(Pill.Id, Pill);
	FStoryFlowNode Getter;
	Getter.Id = TEXT("get");
	Getter.Type = EStoryFlowNodeType::GetDataAssetVariable;
	Getter.Data.VariableId = TEXT("alive");
	Getter.Data.VariableType = TEXT("boolean");
	Script->Nodes.Add(Getter.Id, Getter);
	FStoryFlowNode And;
	And.Id = TEXT("and");
	And.Type = EStoryFlowNodeType::AndBool;
	And.Data.Value2.SetBool(true);
	Script->Nodes.Add(And.Id, And);
	FStoryFlowNode Dialogue;
	Dialogue.Id = TEXT("dialogue");
	Dialogue.Type = EStoryFlowNodeType::Dialogue;
	Script->Nodes.Add(Dialogue.Id, Dialogue);
	auto Edge = [&](const TCHAR* Source, const TCHAR* Target, const TCHAR* SourceHandle, const TCHAR* TargetHandle) {
		FStoryFlowConnection C;
		C.Source = Source;
		C.Target = Target;
		C.SourceHandle = SourceHandle;
		C.TargetHandle = TargetHandle;
		Script->Connections.Add(C);
	};
	Edge(TEXT("pill"), TEXT("get"), TEXT("source-pill-dataAsset-asset"), TEXT("target-get-dataAsset-asset"));
	Edge(TEXT("get"), TEXT("and"), TEXT("source-get-boolean-"), TEXT("target-and-boolean-1"));
	Edge(TEXT("and"), TEXT("dialogue"), TEXT("source-and-boolean-"), TEXT("target-dialogue-boolean-choice"));
	Script->BuildConnectionIndices();
	FStoryFlowExecutionContext Context;
	Context.InitializeWithSubsystem(F.Project, Script, &F.Subsystem->GetGlobalVariables(),
									&F.Subsystem->GetRuntimeCharacters(), &F.Subsystem->GetUsedOnceOnlyOptions(),
									F.Subsystem->GetDataAssetStore(), &F.Subsystem->GetCharacterIdToPath(),
									&F.Subsystem->GetLanguageRef());
	FStoryFlowEvaluator Evaluator(&Context);
	FStoryFlowExecutionContext Second;
	Second.InitializeWithSubsystem(F.Project, Script, &F.Subsystem->GetGlobalVariables(),
								   &F.Subsystem->GetRuntimeCharacters(), &F.Subsystem->GetUsedOnceOnlyOptions(),
								   F.Subsystem->GetDataAssetStore(), &F.Subsystem->GetCharacterIdToPath(),
								   &F.Subsystem->GetLanguageRef());
	FStoryFlowEvaluator OtherEvaluator(&Second);
	TestFalse(TEXT("second context initial hidden"),
			  OtherEvaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	TestFalse(TEXT("initial choice is hidden"),
			  Evaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	TestTrue(TEXT("subsystem writes the live shared value"),
			 F.Subsystem->SetDataAssetBoolVariable(F.Asset, TEXT("alive"), true));
	bool Found = false;
	TestTrue(TEXT("host sees new value"), F.Subsystem->GetDataAssetBoolVariable(F.Asset, TEXT("alive"), Found));
	TestTrue(TEXT("BuildDialogueState's visibility call must see new value"),
			 Evaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	TestTrue(TEXT("another live context sees same write"),
			 OtherEvaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	F.Subsystem->ResetDataAssetOverlay();
	TestFalse(TEXT("reset invalidates first context"),
			  Evaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	TestFalse(TEXT("reset invalidates second context"),
			  OtherEvaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	F.Subsystem->SetDataAssetBoolVariable(F.Asset, TEXT("alive"), true);
	const FString Slot = TEXT("StoryFlowHardeningCacheTest");
	TestTrue(TEXT("save shared state"), F.Subsystem->SaveToSlot(Slot));
	F.Subsystem->ResetAllState();
	TestFalse(TEXT("all-state reset reaches cached context"),
			  Evaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	TestTrue(TEXT("restore shared state"), F.Subsystem->LoadFromSlot(Slot));
	TestTrue(TEXT("restore reaches first context"),
			 Evaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	TestTrue(TEXT("restore reaches second context"),
			 OtherEvaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	F.Subsystem->DeleteSave(Slot);
	Evaluator.ClearCache();
	TestTrue(TEXT("a full HandleDialogue rebuild is a workaround"),
			 Evaluator.EvaluateOptionVisibility(&Script->Nodes[TEXT("dialogue")], TEXT("choice")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDAMapOutputHardening, "StoryFlow.DataAssets.Hardening.DetachedMapOutput",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDAMapOutputHardening::RunTest(const FString&)
{
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("world"), W.Init()))
		return false;
	DataAssetHardening::FFixture F;
	auto Script = NewObject<UStoryFlowScriptAsset>();
	Script->StartNode = TEXT("0");
	auto Add = [&](const TCHAR* Id, EStoryFlowNodeType Type) -> FStoryFlowNode& {
		FStoryFlowNode N;
		N.Id = Id;
		N.Type = Type;
		return Script->Nodes.Add(Id, N);
	};
	Add(TEXT("0"), EStoryFlowNodeType::Start);
	Add(TEXT("pill"), EStoryFlowNodeType::GetDataAsset).Data.AssetId = TEXT("asset");
	auto& Source = Add(TEXT("source"), EStoryFlowNodeType::GetDataAssetVariable);
	Source.Data.VariableId = TEXT("rewards");
	Source.Data.VariableType = TEXT("map");
	Source.Data.KeyType = TEXT("enum");
	Source.Data.ValueType = TEXT("integer");
	auto& Mutation = Add(TEXT("mutate"), EStoryFlowNodeType::SetMapValue);
	Mutation.Data.KeyType = TEXT("enum");
	Mutation.Data.ValueType = TEXT("integer");
	Mutation.Data.MapKey.SetString(TEXT("gold"));
	Mutation.Data.MapInlineValue.SetInt(9);
	auto& Set = Add(TEXT("set"), EStoryFlowNodeType::SetDataAssetVariable);
	Set.Data.VariableId = TEXT("rewards");
	Set.Data.VariableType = TEXT("map");
	Set.Data.KeyType = TEXT("enum");
	Set.Data.ValueType = TEXT("integer");
	auto& Other = Add(TEXT("other"), EStoryFlowNodeType::SetDataAssetVariable);
	Other.Data.VariableId = TEXT("title");
	Other.Data.VariableType = TEXT("string");
	Add(TEXT("text"), EStoryFlowNodeType::ConcatenateString).Data.Value1.SetString(TEXT("marker"));
	auto Edge = [&](const TCHAR* From, const TCHAR* To, const TCHAR* SH, const TCHAR* TH) {
		FStoryFlowConnection C;
		C.Source = From;
		C.Target = To;
		C.SourceHandle = SH;
		C.TargetHandle = TH;
		Script->Connections.Add(C);
	};
	Edge(TEXT("0"), TEXT("mutate"), TEXT("source-0-"), TEXT("target-mutate-0"));
	Edge(TEXT("pill"), TEXT("source"), TEXT("source-pill-dataAsset-asset"), TEXT("target-source-dataAsset-asset"));
	Edge(TEXT("pill"), TEXT("set"), TEXT("source-pill-dataAsset-asset"), TEXT("target-set-dataAsset-asset"));
	Edge(TEXT("pill"), TEXT("other"), TEXT("source-pill-dataAsset-asset"), TEXT("target-other-dataAsset-asset"));
	Edge(TEXT("text"), TEXT("other"), TEXT("source-text-string-"), TEXT("target-other-string-2"));
	Edge(TEXT("source"), TEXT("mutate"), TEXT("source-source-map-enum-integer"),
		 TEXT("target-mutate-map-enum-integer-2"));
	Edge(TEXT("mutate"), TEXT("set"), TEXT("source-mutate-map-enum-integer"), TEXT("target-set-map-enum-integer-2"));
	Script->BuildConnectionIndices();
	F.Project->Scripts.Add(TEXT("map-output"), Script);
	W.Subsystem->SetProject(F.Project);
	W.Component->StartDialogueWithScript(TEXT("map-output"));
	TestEqual(TEXT("detached mutation does not implicitly write shared source"),
			  W.Subsystem->GetDataAssetOverlay().Num(), 0);
	W.Component->StopDialogue();
	Edge(TEXT("mutate"), TEXT("other"), TEXT("source-mutate-1"), TEXT("target-other-0"));
	Edge(TEXT("other"), TEXT("set"), TEXT("source-other-1"), TEXT("target-set-0"));
	Script->BuildConnectionIndices();
	for (EStoryFlowNodeType Operation :
		 {EStoryFlowNodeType::SetMapValue, EStoryFlowNodeType::RemoveMapKey, EStoryFlowNodeType::ClearMap})
	{
		W.Subsystem->ResetDataAssetOverlay();
		Script->Nodes[TEXT("mutate")].Type = Operation;
		W.Component->StartDialogueWithScript(TEXT("map-output"));
		bool Found = false;
		const auto Value = W.Subsystem->GetDataAssetVariantVariable(F.Asset, TEXT("rewards"), Found);
		if (Operation == EStoryFlowNodeType::SetMapValue)
		{
			TestEqual(TEXT("explicit writeback retains executed set result after unrelated write"),
					  Value.GetMap()[0].Value.GetInt(), 9);
		}
		else
		{
			TestEqual(TEXT("explicit writeback retains executed empty map"), Value.GetMap().Num(), 0);
		}
		W.Component->StopDialogue();
	}
	return true;
}
