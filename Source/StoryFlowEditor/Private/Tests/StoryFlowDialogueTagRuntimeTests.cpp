// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "UObject/Package.h"

#include "StoryFlowTagAccumulator.h"

/**
 * Runtime integration test: entering a tagged dialogue node fires one
 * OnDialogueTagReached event per tag, in authored order, exactly once — and an
 * untagged line fires nothing.
 *
 * Drives the real engine end-to-end (spawned world + registered
 * UStoryFlowComponent + StartDialogueWithScript), unlike the import-only test in
 * StoryFlowDialogueTagTests.cpp. Binds an accumulator into OnDialogueTagReached
 * and walks: start -> dialogue(tags ["a","b"]) -> advance -> dialogue(untagged).
 * Asserts the accumulator holds exactly ["a","b"] and the untagged advance adds
 * nothing.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.DialogueTags", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.DialogueTags" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowDialogueTagRuntimeTestHelpers
{
	/** Standalone game instance + registered component, same shape as the map tests. */
	struct FScopedWorld
	{
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		UStoryFlowComponent* Component = nullptr;
		UStoryFlowSubsystem* Subsystem = nullptr;

		bool Init()
		{
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->InitializeStandalone();
			World = GameInstance->GetWorld();
			if (!World) { return false; }
			AActor* Owner = World->SpawnActor<AActor>();
			if (!Owner) { return false; }
			Component = NewObject<UStoryFlowComponent>(Owner);
			Component->RegisterComponent();
			Subsystem = GameInstance->GetSubsystem<UStoryFlowSubsystem>();
			return Component != nullptr && Subsystem != nullptr;
		}

		~FScopedWorld()
		{
			if (World) { World->DestroyWorld(false); }
		}
	};

	FStoryFlowNode MakeNode(const FString& Id, EStoryFlowNodeType Type, const TCHAR* TypeString)
	{
		FStoryFlowNode N;
		N.Id = Id;
		N.Type = Type;
		N.TypeString = TypeString;
		return N;
	}

	FStoryFlowConnection MakeEdge(const TCHAR* Id, const TCHAR* Source, const TCHAR* Target,
		const FString& SourceHandle, const FString& TargetHandle)
	{
		FStoryFlowConnection C;
		C.Id = Id;
		C.Source = Source;
		C.Target = Target;
		C.SourceHandle = SourceHandle;
		C.TargetHandle = TargetHandle;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDialogueTagRuntimeTest,
	"StoryFlow.DialogueTags.RuntimeFiresTagsOnEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDialogueTagRuntimeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDialogueTagRuntimeTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture initialized"), W.Init())) { return false; }

	// --- script: start -> dialogue(tags ["a","b"]) -> dialogue(untagged) -> end ---
	// Dialogues define zero options, so each is a narrative line that waits for
	// AdvanceDialogue. Advance follows the header source handle only.
	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	{
		FStoryFlowNode Tagged = MakeNode(TEXT("1"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		Tagged.Data.Title = TEXT("t");
		Tagged.Data.Text = TEXT("tagged line");
		Tagged.Data.Tags = { TEXT("a"), TEXT("b") };
		Script->Nodes.Add(Tagged.Id, Tagged);
	}
	{
		FStoryFlowNode Untagged = MakeNode(TEXT("2"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		Untagged.Data.Title = TEXT("t");
		Untagged.Data.Text = TEXT("untagged line");
		Script->Nodes.Add(Untagged.Id, Untagged);
	}
	Script->Nodes.Add(TEXT("3"), MakeNode(TEXT("3"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("e1"), TEXT("0"), TEXT("1"),
		StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
	Script->Connections.Add(MakeEdge(TEXT("e2"), TEXT("1"), TEXT("2"),
		StoryFlowHandles::Source(TEXT("1")), StoryFlowHandles::Target(TEXT("2"))));
	Script->Connections.Add(MakeEdge(TEXT("e3"), TEXT("2"), TEXT("3"),
		StoryFlowHandles::Source(TEXT("2")), StoryFlowHandles::Target(TEXT("3"))));
	Script->BuildConnectionIndices();

	UStoryFlowProjectAsset* Project = NewObject<UStoryFlowProjectAsset>(GetTransientPackage());
	Project->Scripts.Add(TEXT("tagtest"), Script);
	W.Subsystem->SetProject(Project);

	UStoryFlowTagAccumulator* Acc = NewObject<UStoryFlowTagAccumulator>(GetTransientPackage());
	W.Component->OnDialogueTagReached.AddDynamic(Acc, &UStoryFlowTagAccumulator::OnTag);

	// --- start: enters node 1 -> fires its tags ["a","b"] once, in order ---
	W.Component->StartDialogueWithScript(TEXT("tagtest"));

	if (TestEqual(TEXT("entering the tagged line fired both tags"), Acc->Tags.Num(), 2))
	{
		TestEqual(TEXT("tag[0] fired first"), Acc->Tags[0], FString(TEXT("a")));
		TestEqual(TEXT("tag[1] fired second"), Acc->Tags[1], FString(TEXT("b")));
	}

	// --- advance to the untagged line: it must add nothing ---
	W.Component->AdvanceDialogue();
	TestEqual(TEXT("untagged line fired no tags"), Acc->Tags.Num(), 2);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
