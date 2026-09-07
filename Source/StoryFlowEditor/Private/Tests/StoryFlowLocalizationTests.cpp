// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Engine/GameInstance.h"
#include "Evaluation/StoryFlowEvaluator.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "StoryFlowScopedWorld.h"

namespace StoryFlowLocalizationTests
{
	struct FFixture
	{
		UStoryFlowProjectAsset* Project = NewObject<UStoryFlowProjectAsset>();
		UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>();
		FString Language = TEXT("fr");
		TMap<FString, FStoryFlowVariable> Globals;
		TMap<FString, FStoryFlowCharacterDef> Characters;
		FStoryFlowExecutionContext Context;
		FStoryFlowEvaluator Evaluator{&Context};

		FFixture()
		{
			Project->bHasLocalization = true;
			Project->SourceLanguage = TEXT("en");
			FStoryFlowStringTable French;
			French.Entries.Add(TEXT("literal.value1"), TEXT("Bonjour"));
			French.Entries.Add(TEXT("map.value"), TEXT("Epee"));
			French.Entries.Add(TEXT("hero.cf_name"), TEXT("Chevalier"));
			Project->LanguageStrings.Add(TEXT("fr"), French);
			Script->Strings.Add(TEXT("en.literal.value1"), TEXT("Hello"));
			Script->Strings.Add(TEXT("en.map.value"), TEXT("Sword"));
			Project->GlobalStrings.Add(TEXT("en.hero.cf_name"), TEXT("Knight"));
			FStoryFlowCharacterDef Hero;
			Hero.Name = TEXT("hero.cf_name");
			Characters.Add(TEXT("hero"), Hero);
			Context.SeedLanguageCode = Language;
			Context.InitializeWithSubsystem(Project, Script, &Globals, &Characters, nullptr, {}, nullptr, &Language);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowGraphLanguageTest,
	"StoryFlow.Localization.GraphUsesActiveLanguage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowGraphLanguageTest::RunTest(const FString& Parameters)
{
	StoryFlowLocalizationTests::FFixture F;
	FStoryFlowNode Node;
	Node.Id = TEXT("literal");
	Node.Type = EStoryFlowNodeType::ConcatenateString;
	Node.Data.Value1.SetString(TEXT("literal.value1"));
	Node.Data.Value2.SetString(TEXT("!"));
	TestEqual(TEXT("graph literals use the active language"), F.Evaluator.EvaluateStringFromNode(&Node, TEXT(""), TEXT("")), FString(TEXT("Bonjour!")));
	TestEqual(TEXT("an explicit language still overrides the active language"), F.Context.GetString(TEXT("literal.value1"), TEXT("en")), FString(TEXT("Hello")));
	F.Language = TEXT("en");
	TestEqual(TEXT("the next graph read sees a mid-dialogue switch"), F.Evaluator.EvaluateStringFromNode(&Node, TEXT(""), TEXT("")), FString(TEXT("Hello!")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMapLiteralLanguageTest,
	"StoryFlow.Localization.MapLiteralIsText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowMapLiteralLanguageTest::RunTest(const FString& Parameters)
{
	StoryFlowLocalizationTests::FFixture F;
	FStoryFlowNode Node;
	Node.Id = TEXT("map");
	Node.Type = EStoryFlowNodeType::SetMapValue;
	Node.Data.ValueType = TEXT("string");
	Node.Data.MapInlineValue.SetString(TEXT("map.value"));
	TestEqual(TEXT("map string literals resolve before entering game state"), F.Evaluator.EvaluateMapOpValueInput(&Node, TEXT("4")).GetString(), FString(TEXT("Epee")));
	Node.Data.ValueType = TEXT("image");
	TestEqual(TEXT("asset identifiers are not localized"), F.Evaluator.EvaluateMapOpValueInput(&Node, TEXT("4")).GetString(), FString(TEXT("map.value")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCharacterNameLanguageTest,
	"StoryFlow.Localization.CharacterNameGraphIsText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowCharacterNameLanguageTest::RunTest(const FString& Parameters)
{
	StoryFlowLocalizationTests::FFixture F;
	FStoryFlowNode Node;
	Node.Id = TEXT("getchar");
	Node.Type = EStoryFlowNodeType::GetCharacterVar;
	Node.Data.CharacterPath = TEXT("hero");
	Node.Data.VariableName = TEXT("Name");
	TestEqual(TEXT("Name reaches graph consumers as display text"), F.Evaluator.EvaluateStringFromNode(&Node, TEXT(""), TEXT("")), FString(TEXT("Chevalier")));
	F.Language = TEXT("en");
	TestEqual(TEXT("Name follows subsequent language changes"), F.Evaluator.EvaluateStringFromNode(&Node, TEXT(""), TEXT("")), FString(TEXT("Knight")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSourceTableLanguageTest,
	"StoryFlow.Localization.SourceTableCompatibility", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowSourceTableLanguageTest::RunTest(const FString& Parameters)
{
	StoryFlowLocalizationTests::FFixture F;
	F.Project->SourceLanguage = TEXT("fr");
	F.Project->GlobalStrings.Add(TEXT("en.source.value"), TEXT("Legacy source"));
	F.Script->Strings.Add(TEXT("en.script.value"), TEXT("Legacy script"));
	TestEqual(TEXT("legacy global source bucket remains readable"), F.Project->GetGlobalString(TEXT("source.value"), TEXT("fr")), FString(TEXT("Legacy source")));
	TestEqual(TEXT("legacy script source bucket remains readable"), F.Context.GetString(TEXT("script.value"), TEXT("fr")), FString(TEXT("Legacy script")));
	F.Project->GlobalStrings.Add(TEXT("fr.source.value"), TEXT("French source"));
	F.Script->Strings.Add(TEXT("fr.script.value"), TEXT("French script"));
	TestEqual(TEXT("actual source bucket precedes legacy globals"), F.Project->GetGlobalString(TEXT("source.value"), TEXT("de")), FString(TEXT("French source")));
	TestEqual(TEXT("actual source bucket precedes legacy script strings"), F.Context.GetString(TEXT("script.value"), TEXT("de")), FString(TEXT("French script")));
	F.Project->GlobalStrings.Add(TEXT("de.source.value"), TEXT("German source"));
	F.Script->Strings.Add(TEXT("de.script.value"), TEXT("German script"));
	TestEqual(TEXT("requested global language precedes source"), F.Project->GetGlobalString(TEXT("source.value"), TEXT("de")), FString(TEXT("German source")));
	TestEqual(TEXT("requested script language precedes source"), F.Context.GetString(TEXT("script.value"), TEXT("de")), FString(TEXT("German script")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowInitialLanguageTest,
	"StoryFlow.Localization.FirstInstallUsesSource", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowInitialLanguageTest::RunTest(const FString& Parameters)
{
	UStoryFlowProjectAsset* Project = NewObject<UStoryFlowProjectAsset>();
	Project->bHasLocalization = true;
	Project->SourceLanguage = TEXT("fr");
	FStoryFlowLanguage English;
	English.Code = TEXT("en");
	English.Name = TEXT("English");
	Project->Languages.Add(English);
	UStoryFlowSubsystem* Subsystem = NewObject<UStoryFlowSubsystem>(NewObject<UGameInstance>());
	Subsystem->SetProject(Project);
	TestEqual(TEXT("the boot default is not a player choice"), Subsystem->GetLanguage(), FString(TEXT("fr")));
	TestTrue(TEXT("the host can choose a target language"), Subsystem->SetLanguage(TEXT("en")));
	Subsystem->SetProject(Project);
	TestEqual(TEXT("reinstall preserves the host choice"), Subsystem->GetLanguage(), FString(TEXT("en")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowNameSaveProvenanceTest,
	"StoryFlow.Localization.NameSaveProvenance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowNameSaveProvenanceTest::RunTest(const FString& Parameters)
{
	StoryFlowLocalizationTests::FFixture F;
	TSet<FString> OnceOnly;
	StoryFlowDataAssets::FSeed Seed;
	StoryFlowDataAssets::FOverlay Overlay;
	auto RoundTrip = [&]()
	{
		const FString Saved = StoryFlowSaveHelpers::SerializeSaveData(F.Globals, F.Characters, OnceOnly, Seed, Overlay);
		return StoryFlowSaveHelpers::DeserializeSaveData(Saved, F.Globals, F.Characters, OnceOnly, Seed, Overlay);
	};
	TestTrue(TEXT("authored names round-trip"), RoundTrip());
	TestEqual(TEXT("authored saved names still localize"), F.Context.GetCharacterVariableValue(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("Chevalier")));
	FStoryFlowVariant Written;
	Written.SetString(TEXT("hero.cf_name"));
	F.Context.SetCharacterVariable(TEXT("hero"), TEXT("Name"), Written);
	TestEqual(TEXT("a runtime write equal to an authored key remains literal"), F.Context.GetCharacterVariableValue(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("hero.cf_name")));
	F.Language = TEXT("en");
	TestTrue(TEXT("runtime names round-trip"), RoundTrip());
	TestEqual(TEXT("language changes and save loading preserve a runtime name"), F.Context.GetCharacterVariableValue(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("hero.cf_name")));
	TestTrue(TEXT("repeated saves preserve the provenance"), RoundTrip());
	TestEqual(TEXT("a second load still leaves player text literal"), F.Context.GetCharacterVariableValue(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("hero.cf_name")));
	TestTrue(TEXT("legacy saves load"), StoryFlowSaveHelpers::DeserializeSaveData(TEXT(R"JSON({"characters":{"hero":{"name":"hero.cf_name","image":""}}})JSON"), F.Globals, F.Characters, OnceOnly, Seed, Overlay));
	TestEqual(TEXT("ambiguous legacy names remain literal"), F.Context.GetCharacterVariableValue(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("hero.cf_name")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowHostNameProvenanceTest,
	"StoryFlow.Localization.HostNameProvenance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowHostNameProvenanceTest::RunTest(const FString& Parameters)
{
	StoryFlowLocalizationTests::FFixture F;
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("world initializes"), W.Init())) return false;
	UStoryFlowCharacterAsset* Character = NewObject<UStoryFlowCharacterAsset>();
	Character->CharacterPath = TEXT("hero");
	Character->Name = TEXT("hero.cf_name");
	F.Project->Characters.Add(TEXT("hero"), Character);
	W.Subsystem->SetProject(F.Project);
	TestEqual(TEXT("authored host name is resolved"), W.Component->GetCharacterVariable(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("Knight")));
	FStoryFlowVariant Written;
	Written.SetString(TEXT("hero.cf_name"));
	W.Component->SetCharacterVariable(TEXT("hero"), TEXT("Name"), Written);
	TestEqual(TEXT("host variant setter preserves literal player text"), W.Component->GetCharacterVariable(TEXT("hero"), TEXT("Name")).GetString(), FString(TEXT("hero.cf_name")));
	W.Subsystem->ResetRuntimeCharacters();
	TestEqual(TEXT("reset restores authored name behavior"), W.Component->GetCharacterStringVariable(Character, TEXT("Name")), FString(TEXT("Knight")));
	W.Component->SetCharacterStringVariable(Character, TEXT("cf_name"), TEXT("hero.cf_name"));
	TestEqual(TEXT("host typed setter preserves literal player text"), W.Component->GetCharacterStringVariable(Character, TEXT("Name")), FString(TEXT("hero.cf_name")));
	return true;
}

#endif
