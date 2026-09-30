// Copyright 2026 StoryFlow. All Rights Reserved.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "StoryFlowEngineContractFixtures.h"
#include "StoryFlowRuntime.h"
#include "Components/StoryFlowComponent.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowDataAssetAsset.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowNestedReferenceTest,
    "StoryFlow.References.NestedInterpolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowNestedReferenceTest::RunTest(const FString& Parameters)
{
    const auto DataType = ParseVariableType(TEXT("dataAsset"));
    TestTrue(TEXT("Data references have their own variable type"), DataType != EStoryFlowVariableType::None && DataType != EStoryFlowVariableType::String);
    auto Var = [](const TCHAR* Name, EStoryFlowVariableType Type, const TCHAR* Value) {
        FStoryFlowVariable V; V.Id = Name; V.Name = Name; V.Type = Type; V.Value.SetString(Value); return V;
    };
    StoryFlowDataAssets::FSeed Seed;
    StoryFlowDataAssets::FOverlay Overlay;
    FStoryFlowDataAssetDef Stats; Stats.Id = TEXT("stats");
    Stats.Variables.Add(Var(TEXT("HP"), EStoryFlowVariableType::Integer, TEXT("")));
    Stats.Variables[0].Value.SetInt(42);
    Stats.Variables.Add(Var(TEXT("Owner"), EStoryFlowVariableType::Character, TEXT("hero")));
    Stats.Variables.Add(Var(TEXT("Skill.Level"), EStoryFlowVariableType::String, TEXT("exact")));
    Stats.Variables.Add(Var(TEXT("Title"), EStoryFlowVariableType::String, TEXT("title")));
    Seed.Add(TEXT("stats"), Stats);
    TMap<FString, FStoryFlowCharacterDef> Characters;
    FStoryFlowCharacterDef Hero; Hero.Name = TEXT("Hero");
    Hero.Variables.Add(TEXT("Stats"), Var(TEXT("Stats"), DataType, TEXT("stats")));
    Characters.Add(TEXT("hero"), Hero);
    TMap<FString, FStoryFlowVariable> Globals;
    Globals.Add(TEXT("DB"), Var(TEXT("DB"), DataType, TEXT("missing")));
    auto* Project = NewObject<UStoryFlowProjectAsset>(); FString Language = TEXT("en");
    Project->GlobalStrings.Add(TEXT("en.title"), TEXT("Knight")); Project->GlobalStrings.Add(TEXT("fr.title"), TEXT("Chevalier"));
    FStoryFlowExecutionContext Context;
    Context.InitializeWithSubsystem(Project, nullptr, &Globals, &Characters, nullptr, {&Seed, &Overlay}, nullptr, &Language);
    Context.LocalVariables.Add(TEXT("DB"), Var(TEXT("DB"), DataType, TEXT("stats")));
    Context.LocalVariables.Add(TEXT("Player"), Var(TEXT("Player"), EStoryFlowVariableType::Character, TEXT("hero")));
    Context.LocalVariables.Add(TEXT("Text"), Var(TEXT("Text"), EStoryFlowVariableType::String, TEXT("{DB.HP}")));
    TestEqual(TEXT("mixed reference hops and exact dotted fields"), Context.InterpolateVariables(TEXT("{ DB.HP } {DB.Owner.Stats.HP} {Player.Stats.HP} {DB.Skill.Level}")), FString(TEXT("42 42 42 exact")));
    TestEqual(TEXT("references and unresolved placeholders survive"), Context.InterpolateVariables(TEXT("{DB} {DB.Missing} {Missing}")), FString(TEXT("{DB} {DB.Missing} {Missing}")));
    TestEqual(TEXT("replacement text is not recursively evaluated"), Context.InterpolateVariables(TEXT("{Text} {DB.HP}")), FString(TEXT("{DB.HP} 42")));
    FStoryFlowVariant HP; HP.SetInt(91); Context.TrySetDataAsset(TEXT("stats"), TEXT("HP"), HP);
    TestEqual(TEXT("writes are immediately visible through mixed references"), Context.InterpolateVariables(TEXT("{Player.Stats.HP}")), FString(TEXT("91")));
    TestEqual(TEXT("nested Data prose localizes"), Context.InterpolateVariables(TEXT("{Player.Stats.Title}")), FString(TEXT("Knight")));
    Language = TEXT("fr"); TestEqual(TEXT("language switches use the live Data read"), Context.InterpolateVariables(TEXT("{Player.Stats.Title}")), FString(TEXT("Chevalier")));
    FStoryFlowVariant Literal; Literal.SetString(TEXT("title")); Context.TrySetDataAsset(TEXT("stats"), TEXT("Title"), Literal);
    TestEqual(TEXT("Data writes remain literal"), Context.InterpolateVariables(TEXT("{Player.Stats.Title}")), FString(TEXT("title")));
    Overlay.Empty(); TestEqual(TEXT("reset restores localized declarations"), Context.InterpolateVariables(TEXT("{Player.Stats.Title}")), FString(TEXT("Chevalier")));
    Characters.Remove(TEXT("hero"));
    TestEqual(TEXT("missing live character does not use stale state"), Context.InterpolateVariables(TEXT("{Player.Stats.HP}")), FString(TEXT("{Player.Stats.HP}")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowNestedReferenceGoldenTest,
    "StoryFlow.References.SharedHtmlFixture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowNestedReferenceGoldenTest::RunTest(const FString& Parameters)
{
    const auto Fixture = StoryFlowEngineContract::LoadFixture(TEXT("nested-reference-interpolation.json"));
    if (!TestTrue(TEXT("shared fixture loads"), Fixture.IsValid())) { return false; }
    const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp/NestedReferenceFixture"));
    const FString Content = TEXT("/Game/StoryFlowNestedReferenceFixture");
    IFileManager::Get().MakeDirectory(*Dir, true);
    UEditorAssetLibrary::DeleteDirectory(Content);
    auto Save = [&](const TCHAR* File, const TSharedPtr<FJsonObject>& Object) {
        FString Json; FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
        return FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Dir, File));
    };
    auto VariablesObject = [](const TArray<TSharedPtr<FJsonValue>>& Rows) {
        auto Object = MakeShared<FJsonObject>();
        for (const auto& Row : Rows) { Object->SetObjectField(Row->AsObject()->GetStringField(TEXT("id")), Row->AsObject()); }
        return Object;
    };
    auto ProjectJson = MakeShared<FJsonObject>(); ProjectJson->SetStringField(TEXT("version"), TEXT("1.0.0"));
    Save(TEXT("project.json"), ProjectJson); Save(TEXT("data-assets.json"), Fixture);
    auto CharacterTable = MakeShared<FJsonObject>(); auto IndexTable = MakeShared<FJsonObject>();
    for (const auto& Entry : Fixture->GetArrayField(TEXT("characters"))) {
        auto Source = Entry->AsObject(); auto Character = MakeShared<FJsonObject>();
        Character->SetStringField(TEXT("name"), Source->GetStringField(TEXT("name")));
        Character->SetObjectField(TEXT("variables"), VariablesObject(Source->GetArrayField(TEXT("variables"))));
        CharacterTable->SetObjectField(Source->GetStringField(TEXT("path")), Character);
        IndexTable->SetStringField(Source->GetStringField(TEXT("id")), NormalizeCharacterPath(Source->GetStringField(TEXT("path"))));
    }
    auto CharactersJson = MakeShared<FJsonObject>(); CharactersJson->SetObjectField(TEXT("characters"), CharacterTable); Save(TEXT("characters.json"), CharactersJson);
    auto IndexJson = MakeShared<FJsonObject>(); IndexJson->SetStringField(TEXT("schemaVersion"), TEXT("1")); IndexJson->SetObjectField(TEXT("characters"), IndexTable); Save(TEXT("character-index.json"), IndexJson);
    auto* Project = UStoryFlowImporter::ImportProject(Dir, Content);
    if (!TestNotNull(TEXT("fixture imports"), Project)) { return false; }
    StoryFlowDataAssets::FSeed Seed; StoryFlowDataAssets::BuildSeed(Project->DataAssets, Seed);
    StoryFlowDataAssets::FOverlay Overlay;
    TMap<FString, FStoryFlowCharacterDef> Characters;
    for (const auto& Pair : Project->Characters) { FStoryFlowCharacterDef Def; Def.Name = Pair.Value->Name; Def.Variables = Pair.Value->Variables; Characters.Add(Pair.Key, Def); }
    for (const auto& Row : Fixture->GetArrayField(TEXT("cases"))) {
        const auto Case = Row->AsObject();
        const TArray<TSharedPtr<FJsonValue>>* Roots;
        if (!Case->TryGetArrayField(TEXT("roots"), Roots)) { Roots = &Fixture->GetArrayField(TEXT("roots")); }
        auto ScriptJson = MakeShared<FJsonObject>(); ScriptJson->SetObjectField(TEXT("variables"), VariablesObject(*Roots));
        auto* Script = UStoryFlowImporter::ImportScriptFromJson(ScriptJson, TEXT("fixture.sfe"), Content, nullptr, true);
        if (!TestNotNull(TEXT("roots import"), Script)) { break; }
        FStoryFlowExecutionContext Context;
        Context.InitializeWithSubsystem(Project, Script, nullptr, &Characters, nullptr, {&Seed, &Overlay}, &Project->CharacterIdToPath);
        Context.CurrentDialogueState.Character.CharacterPath = Fixture->GetStringField(TEXT("assignedCharacterId"));
        TestEqual(*Case->GetStringField(TEXT("name")), Context.InterpolateVariables(Case->GetStringField(TEXT("text"))), Case->GetStringField(TEXT("expected")));
    }
    UEditorAssetLibrary::DeleteDirectory(Content);
    IFileManager::Get().DeleteDirectory(*Dir, false, true);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataReferenceNodesTest,
    "StoryFlow.References.DataNodePipelines", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowDataReferenceNodesTest::RunTest(const FString& Parameters)
{
    auto* Project = NewObject<UStoryFlowProjectAsset>();
    auto* Script = NewObject<UStoryFlowScriptAsset>();
    auto AddNode = [&](const TCHAR* Id, const TCHAR* Type) -> FStoryFlowNode& {
        FStoryFlowNode N; N.Id = Id; N.TypeString = Type; N.Type = ParseNodeType(Type); Script->Nodes.Add(Id, N); return Script->Nodes[Id];
    };
    auto Edge = [&](const TCHAR* From, const TCHAR* To, const TCHAR* SourcePin, const TCHAR* TargetPin) {
        FStoryFlowConnection E; E.Id = FString(From) + To + TargetPin; E.Source = From; E.Target = To;
        E.SourceHandle = FString(TEXT("source-")) + From + TEXT("-") + SourcePin;
        E.TargetHandle = FString(TEXT("target-")) + To + TEXT("-") + TargetPin; Script->Connections.Add(E);
    };
    FStoryFlowVariant Ref; Ref.SetDataAsset(TEXT("stats"));
    FStoryFlowVariable DB; DB.Id = TEXT("db"); DB.Name = TEXT("DB"); DB.Type = EStoryFlowVariableType::DataAsset; DB.Value = Ref;
    Script->Variables.Add(DB.Id, DB);
    FStoryFlowVariable List = DB; List.Id = TEXT("list"); List.Name = TEXT("List"); List.bIsArray = true; List.Value.SetArray({Ref}, DB.Type); Script->Variables.Add(List.Id, List);
    AddNode(TEXT("pill"), TEXT("getDataAsset")).Data.AssetId = TEXT("stats");
    AddNode(TEXT("ref"), TEXT("getDataAssetRef")).Data.Variable = DB.Id;
    AddNode(TEXT("list"), TEXT("getDataAssetRefArray")).Data.Variable = List.Id;
    AddNode(TEXT("element"), TEXT("getDataAssetArrayElement")); Edge(TEXT("list"), TEXT("element"), TEXT("dataAsset-array"), TEXT("dataAsset-array-1"));
    AddNode(TEXT("random"), TEXT("getRandomDataAssetArrayElement")); Edge(TEXT("list"), TEXT("random"), TEXT("dataAsset-array"), TEXT("dataAsset-array-1"));
    AddNode(TEXT("length"), TEXT("arrayLengthDataAsset")); Edge(TEXT("list"), TEXT("length"), TEXT("dataAsset-array"), TEXT("dataAsset-array-1"));
    AddNode(TEXT("contains"), TEXT("arrayContainsDataAsset")).Data.Value = Ref; Edge(TEXT("list"), TEXT("contains"), TEXT("dataAsset-array"), TEXT("dataAsset-array-1"));
    AddNode(TEXT("find"), TEXT("findInDataAssetArray")).Data.Value = Ref; Edge(TEXT("list"), TEXT("find"), TEXT("dataAsset-array"), TEXT("dataAsset-array-1"));
    FStoryFlowVariable Map; Map.Id = TEXT("map"); Map.Name = TEXT("Map"); Map.Type = EStoryFlowVariableType::Map; Map.KeyType = EStoryFlowVariableType::String; Map.ValueType = EStoryFlowVariableType::DataAsset;
    FStoryFlowMapEntry Entry; Entry.Key.SetString(TEXT("slot")); Entry.Value = Ref; Map.Value.SetMap({Entry}); Script->Variables.Add(Map.Id, Map);
    AddNode(TEXT("map"), TEXT("getMap")).Data.Variable = Map.Id;
    auto& MapRead = AddNode(TEXT("mapRead"), TEXT("getMapValue")); MapRead.Data.KeyType = TEXT("string"); MapRead.Data.ValueType = TEXT("dataAsset"); MapRead.Data.MapKey.SetString(TEXT("slot"));
    Edge(TEXT("map"), TEXT("mapRead"), TEXT("map-string-dataAsset"), TEXT("map-string-dataAsset-1"));
    auto& MapWrite = AddNode(TEXT("mapWrite"), TEXT("setMapValue")); MapWrite.Data.ValueType = TEXT("dataAsset"); Edge(TEXT("pill"), TEXT("mapWrite"), TEXT("dataAsset"), TEXT("dataAsset-4"));
    auto& Run = AddNode(TEXT("run"), TEXT("runScript")); FStoryFlowScriptInterfaceParam Output; Output.Id = TEXT("out"); Output.Name = TEXT("Result"); Output.Type = TEXT("dataAsset"); Run.Data.ScriptOutputs.Add(Output);
    AddNode(TEXT("outArray"), TEXT("getDataAssetArrayElement")); Edge(TEXT("run"), TEXT("outArray"), TEXT("dataAsset-array-out-out"), TEXT("dataAsset-array-1"));
    auto& MapLoop = AddNode(TEXT("mapLoop"), TEXT("forEachMap")); MapLoop.Data.ValueType = TEXT("dataAsset");
    AddNode(TEXT("loop"), TEXT("forEachDataAssetLoop"));
    auto& Read = AddNode(TEXT("read"), TEXT("getDataAssetVariable")); Read.Data.VariableId = TEXT("hp"); Read.Data.VariableType = TEXT("integer");
    Edge(TEXT("ref"), TEXT("read"), TEXT("dataAsset"), TEXT("dataAsset-asset"));
    StoryFlowDataAssets::FSeed Seed; StoryFlowDataAssets::FOverlay Overlay;
    FStoryFlowDataAssetDef Stats; Stats.Id = TEXT("stats"); FStoryFlowVariable HP; HP.Id = TEXT("hp"); HP.Name = TEXT("HP"); HP.Type = EStoryFlowVariableType::Integer; HP.Value.SetInt(17); Stats.Variables.Add(HP); Seed.Add(Stats.Id, Stats);
    Script->BuildConnectionIndices(); FStoryFlowExecutionContext Context; Context.InitializeWithSubsystem(Project, Script, nullptr, nullptr, nullptr, {&Seed, &Overlay}); FStoryFlowEvaluator Evaluator(&Context);
    TestEqual(TEXT("Data variable binds Data accessor"), Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("read")), TEXT("test"), FString()), 17);
    TestEqual(TEXT("Data array element"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("element"))), FString(TEXT("stats")));
    TestEqual(TEXT("Data random array element"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("random"))), FString(TEXT("stats")));
    TestEqual(TEXT("Data array length"), Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("length")), TEXT("test"), FString()), 1);
    TestTrue(TEXT("Data array contains"), Evaluator.EvaluateBooleanFromNode(Context.GetNode(TEXT("contains")), TEXT("test"), FString()));
    TestEqual(TEXT("Data array find"), Evaluator.EvaluateIntegerFromNode(Context.GetNode(TEXT("find")), TEXT("test"), FString()), 0);
    TestEqual(TEXT("Data map read"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("mapRead"))), FString(TEXT("stats")));
    TestEqual(TEXT("Data map write input keeps type"), Evaluator.EvaluateMapOpValueInput(Context.GetNode(TEXT("mapWrite")), TEXT("4")).GetType(), EStoryFlowVariableType::DataAsset);
    auto& RunState = Context.GetNodeState(TEXT("run")); RunState.bHasOutputValues = true; RunState.OutputValues.Add(TEXT("Result"), Ref);
    TestEqual(TEXT("Data RunScript output"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("run")), TEXT("test"), TEXT("source-run-dataAsset-out-out")), FString(TEXT("stats")));
    FStoryFlowVariant StaleOutput = Ref; StaleOutput.SetArray({Ref}, EStoryFlowVariableType::DataAsset); RunState.OutputValues[TEXT("Result")] = StaleOutput;
    Context.bCaptureReadFailures = true; const uint64 FailuresBefore = Context.ReadFailures;
    TestEqual(TEXT("stale scalar RunScript metadata cannot read array output"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("run")), TEXT("test"), TEXT("source-run-dataAsset-out-out")), FString());
    TestTrue(TEXT("stale RunScript shape marks failed input for guarded Data setters"), Context.ReadFailures > FailuresBefore);
    RunState.OutputValues[TEXT("Result")].SetString(TEXT("stats"));
    TestEqual(TEXT("stale scalar RunScript metadata cannot read String output"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("run")), TEXT("test"), TEXT("source-run-dataAsset-out-out")), FString());
    Context.GetNode(TEXT("run"))->Data.ScriptOutputs[0].bIsArray = true;
    RunState.OutputValues[TEXT("Result")] = Ref;
    uint64 ArrayFailures = Context.ReadFailures;
    Evaluator.EvaluateDataAssetArrayInput(Context.GetNode(TEXT("outArray")), TEXT("dataAsset-array-1"));
    TestTrue(TEXT("Data array output refuses actual scalar output"), Context.ReadFailures > ArrayFailures);
    RunState.OutputValues[TEXT("Result")].SetArray({}, EStoryFlowVariableType::String); ArrayFailures = Context.ReadFailures;
    Evaluator.EvaluateDataAssetArrayInput(Context.GetNode(TEXT("outArray")), TEXT("dataAsset-array-1"));
    TestTrue(TEXT("Data array output refuses empty String array"), Context.ReadFailures > ArrayFailures);
    RunState.OutputValues[TEXT("Result")].SetArray({}, EStoryFlowVariableType::DataAsset); ArrayFailures = Context.ReadFailures;
    Evaluator.EvaluateDataAssetArrayInput(Context.GetNode(TEXT("outArray")), TEXT("dataAsset-array-1"));
    TestEqual(TEXT("a valid empty Data output remains valid"), Context.ReadFailures, ArrayFailures);
    Context.GetNode(TEXT("run"))->Data.ScriptOutputs[0].bIsArray = false;
    RunState.OutputValues[TEXT("Result")] = Ref;
    Context.GetNodeState(TEXT("mapLoop")).LoopValue = Ref;
    TestEqual(TEXT("Data map loop value"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("mapLoop")), TEXT("test"), TEXT("source-mapLoop-dataAsset-value")), FString(TEXT("stats")));
    auto& LoopState = Context.GetNodeState(TEXT("loop")); LoopState.bLoopInitialized = true; LoopState.LoopIndex = 0; LoopState.LoopArray.Add(Ref);
    TestEqual(TEXT("Data array loop value"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("loop"))), FString(TEXT("stats")));
    Context.LocalVariables[DB.Id].Type = EStoryFlowVariableType::String;
    TestEqual(TEXT("Data getter refuses string declaration"), Evaluator.EvaluateDataAssetFromNode(Context.GetNode(TEXT("ref"))), FString());
    FStoryFlowVariant Packed; Packed.SetArray({Ref}, EStoryFlowVariableType::DataAsset); Packed.PackArrayForSerialization(); Packed.UnpackArrayFromSerialization();
    TestEqual(TEXT("binary array serialization preserves Data tag"), Packed.GetArray()[0].GetType(), EStoryFlowVariableType::DataAsset);
    TMap<FString, FStoryFlowVariable> Saved; Saved.Add(DB.Id, DB); Saved.Add(List.Id, List); Saved.Add(Map.Id, Map);
    TMap<FString, FStoryFlowCharacterDef> Characters; TSet<FString> Used;
    const FString Json = StoryFlowSaveHelpers::SerializeSaveData(Saved, Characters, Used, Seed, Overlay);
    Saved.Empty(); TestTrue(TEXT("Data save restores"), StoryFlowSaveHelpers::DeserializeSaveData(Json, Saved, Characters, Used, Seed, Overlay));
    TestEqual(TEXT("save scalar retains Data tag"), Saved[DB.Id].Value.GetType(), EStoryFlowVariableType::DataAsset);
    TestEqual(TEXT("save array retains Data tag"), Saved[List.Id].Value.GetArray()[0].GetType(), EStoryFlowVariableType::DataAsset);
    TestEqual(TEXT("save map retains Data tag"), Saved[Map.Id].Value.GetMap()[0].Value.GetType(), EStoryFlowVariableType::DataAsset);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDataReferenceSetRefusalTest,
    "StoryFlow.References.SetRefusesWrongDeclaration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowDataReferenceSetRefusalTest::RunTest(const FString& Parameters)
{
    auto* Game = NewObject<UGameInstance>(GEngine); Game->InitializeStandalone(); auto* World = Game->GetWorld();
    if (!TestNotNull(TEXT("test world"), World)) { return false; }
    auto* Component = NewObject<UStoryFlowComponent>(World->SpawnActor<AActor>()); Component->RegisterComponent();
    auto* Subsystem = Game->GetSubsystem<UStoryFlowSubsystem>(); auto* Project = NewObject<UStoryFlowProjectAsset>(); auto* Script = NewObject<UStoryFlowScriptAsset>();
    FStoryFlowVariable Wrong; Wrong.Id = TEXT("wrong"); Wrong.Name = TEXT("Wrong"); Wrong.Type = EStoryFlowVariableType::String; Wrong.Value.SetString(TEXT("original")); Project->GlobalVariables.Add(Wrong.Id, Wrong);
    FStoryFlowNode Set; Set.Id = TEXT("0"); Set.TypeString = TEXT("setDataAssetRef"); Set.Type = EStoryFlowNodeType::SetDataAssetRef; Set.Data.Variable = Wrong.Id; Set.Data.bIsGlobal = true; Set.Data.Value.SetString(TEXT("stats"));
    Script->StartNode = Set.Id; Script->Nodes.Add(Set.Id, Set); Script->BuildConnectionIndices(); Project->Scripts.Add(TEXT("refusal"), Script);
    Subsystem->SetProject(Project); Component->StartDialogueWithScript(TEXT("refusal"));
    TestEqual(TEXT("Data setter refuses string variable"), Subsystem->GetGlobalVariables()[Wrong.Id].Value.GetString(), FString(TEXT("original")));

    // A real call/return graph passes the typed reference into a child and back into a global.
    auto* Caller = NewObject<UStoryFlowScriptAsset>(); Caller->ScriptPath = TEXT("caller");
    auto* Child = NewObject<UStoryFlowScriptAsset>(); Child->ScriptPath = TEXT("child");
    FStoryFlowVariable Echo; Echo.Id = TEXT("echo"); Echo.Name = TEXT("Echo"); Echo.Type = EStoryFlowVariableType::DataAsset; Echo.Value.SetDataAsset(TEXT("")); Project->GlobalVariables.Add(Echo.Id, Echo);
    FStoryFlowVariable Param = Echo; Param.Id = TEXT("param"); Param.Name = TEXT("Param"); Param.bIsInput = true; Param.bIsOutput = true; Child->Variables.Add(Param.Id, Param);
    FStoryFlowNode End; End.Id = TEXT("0"); End.Type = EStoryFlowNodeType::End; Child->Nodes.Add(End.Id, End);
    FStoryFlowNode Run; Run.Id = TEXT("0"); Run.Type = EStoryFlowNodeType::RunScript; Run.Data.Script = TEXT("child");
    FStoryFlowScriptInterfaceParam Interface; Interface.Id = Param.Id; Interface.Name = Param.Name; Interface.Type = TEXT("dataAsset"); Run.Data.ScriptParameters.Add(Interface); Run.Data.ScriptOutputs.Add(Interface); Caller->Nodes.Add(Run.Id, Run);
    FStoryFlowNode Pill; Pill.Id = TEXT("pill"); Pill.Type = EStoryFlowNodeType::GetDataAsset; Pill.Data.AssetId = TEXT("stats"); Caller->Nodes.Add(Pill.Id, Pill);
    Set.Id = TEXT("save"); Set.Data.Variable = Echo.Id; Caller->Nodes.Add(Set.Id, Set);
    auto Wire = [&](const TCHAR* From, const TCHAR* To, const TCHAR* Out, const TCHAR* In) {
        FStoryFlowConnection E; E.Id = FString(From) + To + In; E.Source = From; E.Target = To;
        E.SourceHandle = FString(TEXT("source-")) + From + TEXT("-") + Out; E.TargetHandle = FString(TEXT("target-")) + To + TEXT("-") + In; Caller->Connections.Add(E);
    };
    Wire(TEXT("pill"), TEXT("0"), TEXT("dataAsset"), TEXT("dataAsset-param-param"));
    Wire(TEXT("0"), TEXT("save"), TEXT("output"), TEXT("0"));
    Wire(TEXT("0"), TEXT("save"), TEXT("dataAsset-out-param"), TEXT("dataAsset-2"));
    Caller->BuildConnectionIndices(); Child->BuildConnectionIndices(); Project->Scripts.Add(TEXT("caller"), Caller); Project->Scripts.Add(TEXT("child"), Child);
    Subsystem->SetProject(Project); Component->StartDialogueWithScript(TEXT("caller"));
    TestEqual(TEXT("RunScript Data param/output executes"), Subsystem->GetGlobalVariables()[Echo.Id].Value.GetString(), FString(TEXT("stats")));
    TestEqual(TEXT("RunScript Data param/output retains type"), Subsystem->GetGlobalVariables()[Echo.Id].Value.GetType(), EStoryFlowVariableType::DataAsset);
    Child->Variables[Param.Id].Type = EStoryFlowVariableType::String;
    Component->StartDialogueWithScript(TEXT("caller"));
    TestEqual(TEXT("callee declaration wins over stale Data interface metadata"), Subsystem->GetGlobalVariables()[Echo.Id].Value.GetString(), FString());

    auto* Asset = NewObject<UStoryFlowDataAssetAsset>(); Asset->AssetId = TEXT("stats");
    FStoryFlowVariable Refs = Echo; Refs.Id = TEXT("refs"); Refs.Name = TEXT("Refs"); Refs.bIsArray = true;
    FStoryFlowVariant Original; Original.SetDataAsset(TEXT("original")); Refs.Value.SetArray({Original}, EStoryFlowVariableType::DataAsset); Asset->Variables.Add(Refs); Project->DataAssets.Add(Asset->AssetId, Asset);
    Caller->Nodes[Run.Id].Data.ScriptOutputs[0].bIsArray = true;
    auto& FieldSet = Caller->Nodes[Set.Id]; FieldSet.Type = EStoryFlowNodeType::SetDataAssetVariable; FieldSet.Data.VariableId = Refs.Id; FieldSet.Data.VariableName = Refs.Name; FieldSet.Data.VariableType = TEXT("dataAsset"); FieldSet.Data.bIsArray = true;
    Wire(TEXT("pill"), TEXT("save"), TEXT("dataAsset"), TEXT("dataAsset-asset"));
    Wire(TEXT("0"), TEXT("save"), TEXT("dataAsset-array-out-param"), TEXT("dataAsset-array-2")); Caller->BuildConnectionIndices();
    Child->Variables[Param.Id].Type = EStoryFlowVariableType::DataAsset;
    Subsystem->SetProject(Project); Component->StartDialogueWithScript(TEXT("caller"));
    FStoryFlowVariant Stored; auto Store = Subsystem->GetDataAssetStore();
    StoryFlowDataAssets::TryResolve(*Store.Seed, *Store.Overlay, TEXT("stats"), Refs.Id, Stored);
    TestEqual(TEXT("scalar output cannot wipe a Data array field"), Stored.GetArray().Num(), 1);
    Caller->Nodes[Run.Id].Data.ScriptParameters.Empty(); Child->Variables[Param.Id].Type = EStoryFlowVariableType::String; Child->Variables[Param.Id].bIsArray = true; Child->Variables[Param.Id].Value.SetArray({}, EStoryFlowVariableType::String);
    Component->StartDialogueWithScript(TEXT("caller")); StoryFlowDataAssets::TryResolve(*Store.Seed, *Store.Overlay, TEXT("stats"), Refs.Id, Stored);
    TestEqual(TEXT("empty String array output cannot wipe a Data array field"), Stored.GetArray().Num(), 1);
    Child->Variables[Param.Id].Type = EStoryFlowVariableType::DataAsset; Child->Variables[Param.Id].Value.SetArray({}, EStoryFlowVariableType::DataAsset);
    Component->StartDialogueWithScript(TEXT("caller")); StoryFlowDataAssets::TryResolve(*Store.Seed, *Store.Overlay, TEXT("stats"), Refs.Id, Stored);
    TestEqual(TEXT("valid empty Data array output writes the field"), Stored.GetArray().Num(), 0);

    FStoryFlowVariable Array = Echo; Array.Id = TEXT("array"); Array.Name = TEXT("Array"); Array.bIsArray = true; Array.Value.SetArray({}, EStoryFlowVariableType::DataAsset);
    Subsystem->GetGlobalVariables().Add(Array.Id, Array);
    auto* Mutations = NewObject<UStoryFlowScriptAsset>(); Mutations->ScriptPath = TEXT("mutations");
    FStoryFlowNode GetArray; GetArray.Id = TEXT("getArray"); GetArray.Type = EStoryFlowNodeType::GetDataAssetRefArray; GetArray.Data.Variable = Array.Id; GetArray.Data.bIsGlobal = true; Mutations->Nodes.Add(GetArray.Id, GetArray);
    FStoryFlowNode Mutation; Mutation.Id = TEXT("0"); Mutation.Data.Variable = Array.Id; Mutation.Data.bIsGlobal = true;
    FStoryFlowConnection ArrayEdge; ArrayEdge.Id = TEXT("arrayEdge"); ArrayEdge.Source = GetArray.Id; ArrayEdge.Target = Mutation.Id; ArrayEdge.SourceHandle = TEXT("source-getArray-dataAsset-array"); ArrayEdge.TargetHandle = TEXT("target-0-dataAsset-array-1"); Mutations->Connections.Add(ArrayEdge);
    Mutations->BuildConnectionIndices(); Project->Scripts.Add(TEXT("mutations"), Mutations);
    auto Execute = [&](EStoryFlowNodeType Type) { Mutation.Type = Type; Mutations->Nodes.Add(Mutation.Id, Mutation); Mutations->Connections[0].TargetHandle = Type == EStoryFlowNodeType::SetDataAssetArrayElement ? TEXT("target-0-dataAsset-array-2") : TEXT("target-0-dataAsset-array-1"); Mutations->BuildConnectionIndices(); Component->StartDialogueWithScript(TEXT("mutations")); };
    Mutation.Data.Value.SetDataAsset(TEXT("stats")); Execute(EStoryFlowNodeType::AddToDataAssetArray);
    TestEqual(TEXT("Data Add executes"), Subsystem->GetGlobalVariables()[Array.Id].Value.GetArray().Num(), 1);
    Mutation.Data.Value1.SetInt(0); Mutation.Data.Value2.SetDataAsset(TEXT("other")); Execute(EStoryFlowNodeType::SetDataAssetArrayElement);
    TestEqual(TEXT("Data element Set executes"), Subsystem->GetGlobalVariables()[Array.Id].Value.GetArray()[0].GetString(), FString(TEXT("other")));
    Mutation.Data.Value.SetInt(0); Execute(EStoryFlowNodeType::RemoveFromDataAssetArray);
    TestEqual(TEXT("Data Remove executes"), Subsystem->GetGlobalVariables()[Array.Id].Value.GetArray().Num(), 0);
    Mutation.Data.Value.SetDataAsset(TEXT("stats")); Execute(EStoryFlowNodeType::AddToDataAssetArray); Execute(EStoryFlowNodeType::ClearDataAssetArray);
    TestEqual(TEXT("Data Clear executes"), Subsystem->GetGlobalVariables()[Array.Id].Value.GetArray().Num(), 0);
    auto* Hero = NewObject<UStoryFlowCharacterAsset>(); Hero->CharacterPath = TEXT("hero"); Hero->Variables.Add(Refs.Id, Refs); Project->Characters.Add(TEXT("hero"), Hero);
    GetArray.Type = EStoryFlowNodeType::GetCharacterVar; GetArray.Data.CharacterPath = TEXT("hero"); GetArray.Data.VariableName = Refs.Name; GetArray.Data.VariableType = TEXT("dataAsset"); GetArray.Data.bIsArray = true;
    GetArray.Data.Variable.Empty(); Mutation.Data.Variable.Empty(); Mutations->Nodes[GetArray.Id] = GetArray;
    Subsystem->SetProject(Project); Execute(EStoryFlowNodeType::ClearDataAssetArray);
    TestEqual(TEXT("Clear writes the immediate Character Data array field"), Component->GetCharacterVariable(TEXT("hero"), Refs.Name).GetArray().Num(), 0);

    // Derived arrays are independent snapshots: Remove consumes Add's output but cannot
    // write through Add back into the original variable.
    GetArray.Type = EStoryFlowNodeType::GetDataAssetRefArray; GetArray.Data.Variable = Array.Id; Mutations->Nodes[GetArray.Id] = GetArray;
    Array.Value.SetArray({Original}, EStoryFlowVariableType::DataAsset); Subsystem->GetGlobalVariables().Add(Array.Id, Array);
    FStoryFlowVariable Result = Array; Result.Id = TEXT("result"); Subsystem->GetGlobalVariables().Add(Result.Id, Result);
    FStoryFlowNode Remove; Remove.Id = TEXT("remove"); Remove.Type = EStoryFlowNodeType::RemoveFromDataAssetArray; Remove.Data.Value.SetInt(0); Mutations->Nodes.Add(Remove.Id, Remove);
    FStoryFlowNode SaveArray; SaveArray.Id = TEXT("saveArray"); SaveArray.Type = EStoryFlowNodeType::SetDataAssetRefArray; SaveArray.Data.Variable = Result.Id; SaveArray.Data.bIsGlobal = true; Mutations->Nodes.Add(SaveArray.Id, SaveArray);
    auto ChainWire = [&](const TCHAR* From, const TCHAR* To, const TCHAR* Out, const TCHAR* In) {
        FStoryFlowConnection E; E.Id = FString(From) + To + In; E.Source = From; E.Target = To;
        E.SourceHandle = FString(TEXT("source-")) + From + TEXT("-") + Out; E.TargetHandle = FString(TEXT("target-")) + To + TEXT("-") + In; Mutations->Connections.Add(E);
    };
    ChainWire(TEXT("0"), TEXT("remove"), TEXT("1"), TEXT("0")); ChainWire(TEXT("0"), TEXT("remove"), TEXT("dataAsset-array"), TEXT("dataAsset-array-2"));
    ChainWire(TEXT("remove"), TEXT("saveArray"), TEXT("1"), TEXT("0")); ChainWire(TEXT("remove"), TEXT("saveArray"), TEXT("dataAsset-array"), TEXT("dataAsset-array-2"));
    Mutation.Data.Value.SetDataAsset(TEXT("stats")); Execute(EStoryFlowNodeType::AddToDataAssetArray);
    TestEqual(TEXT("derived Remove leaves original post-Add array intact"), Subsystem->GetGlobalVariables()[Array.Id].Value.GetArray().Num(), 2);
    TestEqual(TEXT("derived Remove publishes its independent result"), Subsystem->GetGlobalVariables()[Result.Id].Value.GetArray().Num(), 1);
    TestEqual(TEXT("derived Remove removes only from snapshot"), Subsystem->GetGlobalVariables()[Result.Id].Value.GetArray().IsEmpty() ? FString() : Subsystem->GetGlobalVariables()[Result.Id].Value.GetArray()[0].GetString(), FString(TEXT("stats")));
    World->DestroyWorld(false); return true;
}
#endif
