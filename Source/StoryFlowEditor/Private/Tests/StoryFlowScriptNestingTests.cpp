// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "StoryFlowScopedWorld.h"
#include "StoryFlowTagAccumulator.h"
#include "Async/TaskGraphInterfaces.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Evaluation/StoryFlowExecutionContext.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "WebSocket/StoryFlowSyncManager.h"
#include "WebSocket/StoryFlowWebSocketClient.h"
#include <limits>

namespace StoryFlowScriptNestingTests
{
	struct FImportFixture
	{
		FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ScriptNestingTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString BuildDir = FPaths::Combine(Root, TEXT("build"));
		FString ContentPath = TEXT("/Game/StoryFlowScriptNestingTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);

		FImportFixture() { IFileManager::Get().MakeDirectory(*BuildDir, true); }
		~FImportFixture()
		{
			UEditorAssetLibrary::DeleteDirectory(ContentPath);
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		TSharedPtr<FJsonObject> Document(const TSharedPtr<FJsonValue>& Value) const
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			if (Value.IsValid())
			{
				TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
				Metadata->SetField(TEXT("maxScriptNesting"), Value);
				Json->SetObjectField(TEXT("metadata"), Metadata);
			}
			return Json;
		}

		UStoryFlowProjectAsset* Import(const TSharedPtr<FJsonValue>& Value) const
		{
			return UStoryFlowImporter::ImportProjectFromJson(Document(Value), BuildDir, ContentPath);
		}

		bool Write(const TSharedPtr<FJsonValue>& Value) const
		{
			FString Json;
			FJsonSerializer::Serialize(Document(Value).ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
			return FFileHelper::SaveStringToFile(Json, *FPaths::Combine(BuildDir, TEXT("project.json")));
		}
	};

	void CheckBoundary(FAutomationTestBase& Test, UStoryFlowProjectAsset* Project, int32 Limit, const FString& Label)
	{
		if (!Test.TestNotNull(Label + TEXT(" imported"), Project)) { return; }
		FStoryFlowExecutionContext Context;
		Context.Initialize(Project, nullptr);
		Context.CallStack.SetNum(Limit - 1);
		Test.TestFalse(Label + TEXT(" permits one more call below limit"), Context.IsAtMaxScriptDepth());
		Context.CallStack.SetNum(Limit);
		Test.TestTrue(Label + TEXT(" blocks at limit"), Context.IsAtMaxScriptDepth());
		Context.FlowCallStack.SetNum(49);
		Test.TestFalse(Label + TEXT(" flow guard permits depth 49"), Context.IsAtMaxFlowDepth());
		Context.FlowCallStack.SetNum(50);
		Test.TestTrue(Label + TEXT(" flow guard still stops at 50"), Context.IsAtMaxFlowDepth());
	}

	UStoryFlowScriptAsset* RecursiveScript(UStoryFlowProjectAsset* Project)
	{
		UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(Project);
		Script->ScriptPath = TEXT("recursive");
		Script->StartNode = TEXT("0");
		FStoryFlowNode Start;
		Start.Id = TEXT("0");
		Start.Type = EStoryFlowNodeType::Start;
		Script->Nodes.Add(Start.Id, Start);
		FStoryFlowNode Line;
		Line.Id = TEXT("line");
		Line.Type = EStoryFlowNodeType::Dialogue;
		Line.Data.Text = TEXT("Nested dialogue");
		Line.Data.Tags.Add(TEXT("entered"));
		Script->Nodes.Add(Line.Id, Line);
		FStoryFlowNode Run;
		Run.Id = TEXT("run");
		Run.Type = EStoryFlowNodeType::RunScript;
		Run.Data.Script = Script->ScriptPath;
		Script->Nodes.Add(Run.Id, Run);
		for (const TPair<FString, FString>& Ends : { TPair<FString, FString>(TEXT("0"), TEXT("line")), TPair<FString, FString>(TEXT("line"), TEXT("run")) })
		{
			FStoryFlowConnection Edge;
			Edge.Id = Ends.Key + Ends.Value;
			Edge.Source = Ends.Key;
			Edge.Target = Ends.Value;
			Edge.SourceHandle = StoryFlowHandles::Source(Ends.Key);
			Edge.TargetHandle = StoryFlowHandles::Target(Ends.Value);
			Script->Connections.Add(Edge);
		}
		Script->BuildConnectionIndices();
		Project->Scripts.Add(Script->ScriptPath, Script);
		return Script;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowScriptNestingImportTest,
	"StoryFlow.ScriptNesting.ImportValidationAndReimport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowScriptNestingImportTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowScriptNestingTests;
	FImportFixture Fixture;
	for (int32 Limit : { 1, 20, 37, 100 })
	{
		UStoryFlowProjectAsset* Project = Fixture.Import(MakeShared<FJsonValueNumber>(Limit));
		if (!TestNotNull(TEXT("project imported"), Project)) { return false; }
		CheckBoundary(*this, Project, Limit, FString::FromInt(Limit));
		TArray<uint8> Bytes;
		FObjectWriter(Project, Bytes, false, false, false);
		UStoryFlowProjectAsset* Reloaded = NewObject<UStoryFlowProjectAsset>();
		FObjectReader(Reloaded, Bytes);
		CheckBoundary(*this, Reloaded, Limit, TEXT("serialized project retains configured limit"));
	}
	const TArray<TSharedPtr<FJsonValue>> Invalid = {
		nullptr, MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("37")),
		MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(101),
		MakeShared<FJsonValueNumber>(-1), MakeShared<FJsonValueNumber>(2.5),
		MakeShared<FJsonValueNumber>(std::numeric_limits<double>::quiet_NaN()),
		MakeShared<FJsonValueNumber>(std::numeric_limits<double>::infinity()),
		MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>())
	};
	for (int32 Index = 0; Index < Invalid.Num(); ++Index)
	{
		// Every fallback is a reimport over a non-default value, exposing stale metadata.
		UStoryFlowProjectAsset* Prior = Fixture.Import(MakeShared<FJsonValueNumber>(37));
		UStoryFlowProjectAsset* Imported = Fixture.Import(Invalid[Index]);
		TestTrue(TEXT("reimport reuses the project asset"), Prior == Imported);
		CheckBoundary(*this, Imported, 20, FString::Printf(TEXT("invalid case %d"), Index));
	}
	for (const TSharedPtr<FJsonValue>& Metadata : {
		TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())),
		TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
		TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("invalid"))) })
	{
		Fixture.Import(MakeShared<FJsonValueNumber>(37));
		auto Json = MakeShared<FJsonObject>();
		Json->SetField(TEXT("metadata"), Metadata);
		CheckBoundary(*this, UStoryFlowImporter::ImportProjectFromJson(Json, Fixture.BuildDir, Fixture.ContentPath), 20, TEXT("missing setting or invalid metadata"));
	}
	CheckBoundary(*this, NewObject<UStoryFlowProjectAsset>(), 20, TEXT("legacy asset default"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowScriptNestingPushTest,
	"StoryFlow.ScriptNesting.PushScriptBoundaryAndRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowScriptNestingPushTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowScriptNestingTests;
	FImportFixture Fixture;
	for (int32 Limit : { 1, 20, 100 })
	{
		UStoryFlowProjectAsset* Project = Fixture.Import(MakeShared<FJsonValueNumber>(Limit));
		if (!TestNotNull(TEXT("project imported"), Project)) { return false; }
		UStoryFlowScriptAsset* Script = RecursiveScript(Project);
		FStoryFlowExecutionContext Context;
		Context.Initialize(Project, Script);
		bool bReachedLimit = true;
		for (int32 Depth = 0; Depth < Limit; ++Depth)
		{
			if (!TestTrue(TEXT("push succeeds through configured limit"), Context.PushScript(TEXT("recursive"), TEXT("run"))))
			{
				bReachedLimit = false;
				break;
			}
		}
		if (!bReachedLimit) { continue; }
		TestEqual(TEXT("each nested call owns one frame"), Context.CallStack.Num(), Limit);
		Context.CurrentNodeId = TEXT("sentinel");
		AddExpectedError(FString::Printf(TEXT("Max script nesting depth exceeded \\(%d\\)"), Limit), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("push rejects the next call"), Context.PushScript(TEXT("recursive"), TEXT("run")));
		TestEqual(TEXT("rejection preserves frames"), Context.CallStack.Num(), Limit);
		TestEqual(TEXT("rejection preserves caller"), Context.CurrentNodeId, FString(TEXT("sentinel")));
		TestTrue(TEXT("return frees one frame"), Context.PopScript());
		TestTrue(TEXT("call allowed after return"), Context.PushScript(TEXT("recursive"), TEXT("run")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowScriptNestingComponentTest,
	"StoryFlow.ScriptNesting.ComponentUsesConfiguredLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowScriptNestingComponentTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowScriptNestingTests;
	FImportFixture Fixture;
	StoryFlowTestWorld::FScopedWorld World;
	if (!TestTrue(TEXT("runtime fixture initialized"), World.Init())) { return false; }
	for (int32 Limit : { 1, 20, 100 })
	{
		UStoryFlowProjectAsset* Project = Fixture.Import(MakeShared<FJsonValueNumber>(Limit));
		if (!TestNotNull(TEXT("project imported"), Project)) { return false; }
		RecursiveScript(Project);
		World.Subsystem->SetProject(Project);
		UStoryFlowTagAccumulator* Entered = NewObject<UStoryFlowTagAccumulator>();
		UStoryFlowTagAccumulator* Errors = NewObject<UStoryFlowTagAccumulator>();
		World.Component->OnDialogueTagReached.AddDynamic(Entered, &UStoryFlowTagAccumulator::OnTag);
		World.Component->OnError.AddDynamic(Errors, &UStoryFlowTagAccumulator::OnTag);
		World.Component->StartDialogueWithScript(TEXT("recursive"));
		for (int32 Depth = 0; Depth < Limit && Errors->Tags.IsEmpty(); ++Depth)
		{
			World.Component->AdvanceDialogue();
		}
		TestEqual(TEXT("root plus all configured nested scripts reached dialogue"), Entered->Tags.Num(), Limit + 1);
		if (Errors->Tags.IsEmpty())
		{
			AddExpectedError(FString::Printf(TEXT("Max script nesting depth exceeded \\(%d\\)"), Limit), EAutomationExpectedErrorFlags::Contains, 1);
			World.Component->AdvanceDialogue();
			if (TestEqual(TEXT("overflow reports one error"), Errors->Tags.Num(), 1))
			{
				TestTrue(TEXT("error identifies configured limit"), Errors->Tags[0].Contains(FString::Printf(TEXT("(%d)"), Limit)));
			}
			TestEqual(TEXT("overflow never enters another script"), Entered->Tags.Num(), Limit + 1);
		}
		World.Component->StopDialogue();
		World.Component->OnDialogueTagReached.RemoveAll(Entered);
		World.Component->OnError.RemoveAll(Errors);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowScriptNestingSyncTest,
	"StoryFlow.ScriptNesting.WebSocketProjectUpdated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowScriptNestingSyncTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowScriptNestingTests;
	FImportFixture Fixture;
	auto Client = MakeShared<FStoryFlowWebSocketClient>();
	auto Sync = MakeShared<FStoryFlowSyncManager>();
	Sync->Initialize(Client);
	Sync->SetContentPath(Fixture.ContentPath);
	auto Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("projectPath"), Fixture.Root);
	for (int32 Limit : { 37, 20 })
	{
		TSharedPtr<FJsonValue> Value;
		if (Limit != 20) { Value = MakeShared<FJsonValueNumber>(Limit); }
		TestTrue(TEXT("export fixture written"), Fixture.Write(Value));
		Client->OnMessageReceived.Broadcast(TEXT("project-updated"), Payload);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		CheckBoundary(*this, Sync->GetProjectAsset(), Limit, TEXT("WebSocket reimport"));
	}
	Sync->Shutdown();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
