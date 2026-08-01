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
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "UObject/Package.h"

#include "StoryFlowWidgetSpy.h"

/**
 * Runtime integration tests for the auto-created dialogue widget: who may
 * create it, who is told about it, and who is allowed to tear it down.
 *
 * bAutoAddWidgetToViewport = true (default, today's behavior) means the
 * component owns placement and teardown. false means the game owns both, so
 * the component must never remove the widget from its parent — neither at
 * dialogue end nor when a later dialogue replaces it.
 *
 * Headless coverage limit: AddToViewport routes through UGameViewportSubsystem,
 * which bails without a game viewport (-nullrhi automation has none), so whether
 * the widget really reaches the screen cannot be observed here and stays
 * code-review-only. What is observable is that the call was made, via the
 * complaint that bail-out logs (see the expected message below). The REMOVE half
 * needs no such indirection: UWidget::RemoveFromParent is virtual, so
 * UStoryFlowWidgetSpy counts the component's teardown calls directly, and the
 * same spy counts dialogue updates to show a released widget has gone quiet.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.DialogueWidget", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.DialogueWidget" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */

namespace StoryFlowDialogueWidgetTestHelpers
{
	/**
	 * Standalone game instance + registered component, plus the local player
	 * controller the widget path needs.
	 *
	 * CreateWidget refuses any non-local player controller, and the component
	 * reaches the controller through World::GetFirstPlayerController, which reads
	 * the world's controller list. A world spawned this way has not begun play,
	 * so the spawned controller never registers itself: AddController does it by
	 * hand. UGameInstance::CreateLocalPlayer cannot be used because it ensures on
	 * a missing game viewport, so the local player is constructed directly.
	 */
	struct FScopedWorld
	{
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		UStoryFlowComponent* Component = nullptr;
		UStoryFlowSubsystem* Subsystem = nullptr;
		APlayerController* PlayerController = nullptr;

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

			UClass* LocalPlayerClass = GEngine->LocalPlayerClass ? GEngine->LocalPlayerClass.Get() : ULocalPlayer::StaticClass();
			ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine, LocalPlayerClass);
			PlayerController = World->SpawnActor<APlayerController>();
			if (!PlayerController || !LocalPlayer) { return false; }
			PlayerController->Player = LocalPlayer;
			LocalPlayer->PlayerController = PlayerController;
			World->AddController(PlayerController);

			return Component != nullptr && Subsystem != nullptr && World->GetFirstPlayerController() != nullptr;
		}

		~FScopedWorld()
		{
			if (World) { World->DestroyWorld(false); }
		}
	};

	/** start -> dialogue (narrative line, waits for input so the dialogue stays open) */
	void InstallScript(UStoryFlowSubsystem* Subsystem)
	{
		UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
		Script->StartNode = TEXT("0");

		FStoryFlowNode Start;
		Start.Id = TEXT("0");
		Start.Type = EStoryFlowNodeType::Start;
		Start.TypeString = TEXT("start");
		Script->Nodes.Add(Start.Id, Start);

		FStoryFlowNode Line;
		Line.Id = TEXT("1");
		Line.Type = EStoryFlowNodeType::Dialogue;
		Line.TypeString = TEXT("dialogue");
		Line.Data.Title = TEXT("t");
		Line.Data.Text = TEXT("a line");
		Script->Nodes.Add(Line.Id, Line);

		FStoryFlowConnection Edge;
		Edge.Id = TEXT("e1");
		Edge.Source = TEXT("0");
		Edge.Target = TEXT("1");
		Edge.SourceHandle = StoryFlowHandles::Source(TEXT("0"));
		Edge.TargetHandle = StoryFlowHandles::Target(TEXT("1"));
		Script->Connections.Add(Edge);
		Script->BuildConnectionIndices();

		UStoryFlowProjectAsset* Project = NewObject<UStoryFlowProjectAsset>(GetTransientPackage());
		Project->Scripts.Add(TEXT("widgettest"), Script);
		Subsystem->SetProject(Project);
	}

	UStoryFlowWidgetSpy* SpyFrom(UStoryFlowComponent* Component)
	{
		return Cast<UStoryFlowWidgetSpy>(Component->GetDialogueWidget());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDialogueWidgetAutoAddTest,
	"StoryFlow.DialogueWidget.ComponentOwnsLifecycleByDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDialogueWidgetAutoAddTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDialogueWidgetTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture initialized"), W.Init())) { return false; }
	InstallScript(W.Subsystem);

	// The two dialogues below each try to add their widget to a viewport this
	// fixture's world never has, and UGameViewportSubsystem says so. Expecting it
	// exactly twice both silences the noise and is the closest thing to a sensor
	// for the add half of the flag: the message is only reachable through
	// AddToViewport, so its absence would mean the component stopped calling it.
	// The mirror-image assertion is not available in the user-owned test, where
	// the message must never appear: an expected message that does not occur fails
	// a test, but an unexpected warning does not.
	AddExpectedMessage(TEXT("No game viewport was found"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	TestTrue(TEXT("auto-add defaults on, preserving the pre-flag behavior"), W.Component->bAutoAddWidgetToViewport);
	TestNull(TEXT("no widget before a dialogue starts"), W.Component->GetDialogueWidget());

	W.Component->DialogueWidgetClass = UStoryFlowWidgetSpy::StaticClass();
	UStoryFlowWidgetCreationRecorder* Recorder = NewObject<UStoryFlowWidgetCreationRecorder>(GetTransientPackage());
	W.Component->OnDialogueWidgetCreated.AddDynamic(Recorder, &UStoryFlowWidgetCreationRecorder::OnWidgetCreated);

	// --- first dialogue: the component creates, announces and hands back the widget ---
	W.Component->StartDialogueWithScript(TEXT("widgettest"));

	UStoryFlowWidgetSpy* First = SpyFrom(W.Component);
	if (!TestNotNull(TEXT("starting a dialogue creates the widget"), First)) { return false; }
	if (TestEqual(TEXT("creation is announced exactly once"), Recorder->Created.Num(), 1))
	{
		TestTrue(TEXT("the announced widget is the one the getter returns"), Recorder->Created[0] == First);
	}

	// --- a second dialogue replaces it: the component put the old one up, so it takes it down ---
	W.Component->StartDialogueWithScript(TEXT("widgettest"));

	UStoryFlowWidgetSpy* Second = SpyFrom(W.Component);
	if (!TestNotNull(TEXT("restarting creates a fresh widget"), Second)) { return false; }
	TestTrue(TEXT("the fresh widget is not the previous one"), Second != First);
	TestEqual(TEXT("the replaced widget is removed by the component"), First->RemoveFromParentCount, 1);
	TestEqual(TEXT("the fresh widget is announced too"), Recorder->Created.Num(), 2);

	// --- dialogue end: the component removes the widget and forgets it ---
	W.Component->StopDialogue();

	TestEqual(TEXT("ending the dialogue removes the widget"), Second->RemoveFromParentCount, 1);
	TestNull(TEXT("the component forgets the widget at dialogue end"), W.Component->GetDialogueWidget());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowDialogueWidgetUserOwnedTest,
	"StoryFlow.DialogueWidget.GameOwnsLifecycleWhenAutoAddIsOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowDialogueWidgetUserOwnedTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowDialogueWidgetTestHelpers;

	FScopedWorld W;
	if (!TestTrue(TEXT("fixture initialized"), W.Init())) { return false; }
	InstallScript(W.Subsystem);

	W.Component->bAutoAddWidgetToViewport = false;
	W.Component->DialogueWidgetClass = UStoryFlowWidgetSpy::StaticClass();
	UStoryFlowWidgetCreationRecorder* Recorder = NewObject<UStoryFlowWidgetCreationRecorder>(GetTransientPackage());
	W.Component->OnDialogueWidgetCreated.AddDynamic(Recorder, &UStoryFlowWidgetCreationRecorder::OnWidgetCreated);

	// --- creation and the announcement are unchanged: only placement moves to the game ---
	W.Component->StartDialogueWithScript(TEXT("widgettest"));

	UStoryFlowWidgetSpy* First = SpyFrom(W.Component);
	if (!TestNotNull(TEXT("the widget is still created for the game to place"), First)) { return false; }
	if (TestEqual(TEXT("creation is announced exactly once"), Recorder->Created.Num(), 1))
	{
		TestTrue(TEXT("the announced widget is the one the getter returns"), Recorder->Created[0] == First);
	}

	// --- a second dialogue must not touch the old widget: it may still be animating out ---
	const int32 FirstUpdatesBeforeReplacement = First->DialogueUpdatedCount;
	W.Component->StartDialogueWithScript(TEXT("widgettest"));

	UStoryFlowWidgetSpy* Second = SpyFrom(W.Component);
	if (!TestNotNull(TEXT("restarting creates a fresh widget"), Second)) { return false; }
	TestTrue(TEXT("the fresh widget is not the previous one"), Second != First);
	TestEqual(TEXT("the replaced widget is left for the game to finish"), First->RemoveFromParentCount, 0);

	// Letting go must also mean going quiet. A still-subscribed widget would be
	// driven by the new dialogue: it would show itself again on OnDialogueStarted
	// and render the new lines, on top of the fade-out it was in the middle of.
	TestEqual(TEXT("the replaced widget stops receiving dialogue updates"), First->DialogueUpdatedCount, FirstUpdatesBeforeReplacement);
	TestNull(TEXT("the replaced widget no longer points at the component"), First->GetStoryFlowComponent());

	// --- dialogue end: the reference is dropped, the widget itself is untouched ---
	W.Component->StopDialogue();

	TestEqual(TEXT("ending the dialogue leaves the widget parented"), Second->RemoveFromParentCount, 0);
	TestTrue(TEXT("ending the dialogue does not destroy the widget"), IsValid(Second));
	TestNull(TEXT("the component still forgets the widget at dialogue end"), W.Component->GetDialogueWidget());

	// --- and a widget released at dialogue end is just as detached ---
	const int32 SecondUpdatesAtDialogueEnd = Second->DialogueUpdatedCount;
	W.Component->StartDialogueWithScript(TEXT("widgettest"));

	TestEqual(TEXT("a widget released at dialogue end stops receiving updates too"), Second->DialogueUpdatedCount, SecondUpdatesAtDialogueEnd);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
