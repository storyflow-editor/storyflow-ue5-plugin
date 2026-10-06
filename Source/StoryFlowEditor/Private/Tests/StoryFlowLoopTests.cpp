// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "StoryFlowScopedWorld.h"
#include "StoryFlowTagAccumulator.h"
#include "StoryFlowVariableAccumulator.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Import/StoryFlowImporter.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

/**
 * For Each loops, held to what the editor's own runtime does with them.
 *
 * A loop body runs until a node has nothing connected after it, then the loop moves on to its next
 * element and finally takes Completed. That has to hold for every exec node a body can end on: a
 * handler that just stops leaves the loop open after its first element, Completed never runs and
 * the loop's next run resumes the abandoned one.
 *
 * The scenarios mirror the editor's loop-body-unconnected-tail, loop-early-return,
 * save-loop-continuation and loop-state-after-load-and-reset runtime tests. Scripts are written in
 * the JSON export's own shape and go through the real importer, and everything is read back
 * through the component's public surface.
 *
 * Run via: Session Frontend > Automation > "StoryFlow.Loops", or
 *   UnrealEditor-Cmd.exe StoryFlow.uproject -ExecCmds="Automation RunTests StoryFlow.Loops" -TestExit="Automation Test Queue Empty" -unattended -nullrhi
 */
namespace StoryFlowLoopTests
{
	const TCHAR* const IsArray = TEXT(R"("isArray":true)");
	const TCHAR* const StepOne = TEXT(R"("variable":"var_Step","value":1)");
	const TCHAR* const DoneOne = TEXT(R"("variable":"var_Done","value":1,"isGlobal":true)");

	/** One script in the shape the editor's JSON export writes, assembled from text fragments. */
	struct FScript
	{
		TArray<FString> Nodes, Edges, Variables, Flows;

		FScript() { Node(TEXT("0"), TEXT("start")); }

		FScript& Node(const FString& Id, const FString& Type, const FString& Fields = FString())
		{
			Nodes.Add(FString::Printf(TEXT(R"("%s":{"type":"%s","id":"%s"%s%s})"), *Id, *Type, *Id, Fields.IsEmpty() ? TEXT("") : TEXT(","), *Fields));
			return *this;
		}

		/** "source-{From}-{Out}" into "target-{To}-{In}". In "0" is the exec input. */
		FScript& Edge(const FString& From, const FString& Out, const FString& To, const FString& In = TEXT("0"))
		{
			Edges.Add(FString::Printf(TEXT(R"({"id":"%s-%s-%s-%s","source":"%s","target":"%s","sourceHandle":"source-%s-%s","targetHandle":"target-%s-%s"})"),
				*From, *Out, *To, *In, *From, *To, *From, *Out, *To, *In));
			return *this;
		}

		/** A dialogue line. With no options it advances through its header output. */
		FScript& Line(const FString& Id, const TArray<FString>& Options = TArray<FString>())
		{
			TArray<FString> Choices;
			for (const FString& Option : Options)
			{
				Choices.Add(FString::Printf(TEXT(R"({"id":"%s","text":"%s"})"), *Option, *Option));
			}
			return Node(Id, TEXT("dialogue"), FString::Printf(TEXT(R"("text":"%s","choices":[%s])"), *Id, *FString::Join(Choices, TEXT(","))));
		}

		FScript& Variable(const FString& Name, const FString& Type, const FString& Value, const FString& Fields = FString())
		{
			Variables.Add(FString::Printf(TEXT(R"("var_%s":{"id":"var_%s","name":"%s","type":"%s","value":%s%s%s})"),
				*Name, *Name, *Name, *Type, *Value, Fields.IsEmpty() ? TEXT("") : TEXT(","), *Fields));
			return *this;
		}

		/** For Each over the local integer array Variable. */
		FScript& ForEach(const FString& Id, const FString& Variable)
		{
			return Node(Id + TEXT("Array"), TEXT("getIntArray"), FString::Printf(TEXT(R"("variable":"var_%s")"), *Variable))
				.Node(Id, TEXT("forEachIntLoop"))
				.Edge(Id + TEXT("Array"), TEXT("integer-array-"), Id, TEXT("integer-array-array"));
		}

		/** For Each over the local string to integer map Variable. */
		FScript& ForEachMap(const FString& Id, const FString& Variable)
		{
			return Node(Id + TEXT("Map"), TEXT("getMap"), FString::Printf(TEXT(R"("variable":"var_%s","keyType":"string","valueType":"integer")"), *Variable))
				.Node(Id, TEXT("forEachMap"), TEXT(R"("keyType":"string","valueType":"integer")"))
				.Edge(Id + TEXT("Map"), TEXT("map-string-integer"), Id, TEXT("map-string-integer-map"));
		}

		/** Add To Array(Log, the integer on From's Output pin): the global Log records what ran. */
		FScript& Log(const FString& Id, const FString& From, const FString& Output)
		{
			return Node(Id + TEXT("Array"), TEXT("getIntArray"), TEXT(R"("variable":"var_Log","isGlobal":true)"))
				.Node(Id, TEXT("addToIntArray"))
				.Edge(Id + TEXT("Array"), TEXT("integer-array-"), Id, TEXT("integer-array-2"))
				.Edge(From, Output, Id, TEXT("integer-3"));
		}

		FString Json() const
		{
			return FString::Printf(TEXT(R"({"startNode":"0","nodes":{%s},"connections":[%s],"variables":{%s},"flows":[%s],"strings":{"en":{}},"assets":{}})"),
				*FString::Join(Nodes, TEXT(",")), *FString::Join(Edges, TEXT(",")), *FString::Join(Variables, TEXT(",")), *FString::Join(Flows, TEXT(",")));
		}
	};

	/** The node a loop body ends on, with nothing connected after it, and what it needs beside it. */
	struct FTail
	{
		const TCHAR* Name;
		const TCHAR* Type;
		const TCHAR* Fields;
		void (*Wire)(FScript& Script, const FString& Id);
	};

	const FTail Tails[] = {
		{ TEXT("Set Int"), TEXT("setInt"), StepOne, nullptr },
		{ TEXT("Set Background Image"), TEXT("setBackgroundImage"), TEXT(""), nullptr },
		{ TEXT("Play Audio"), TEXT("playAudio"), TEXT(""), nullptr },
		{ TEXT("Set Bool Array"), TEXT("setBoolArray"), TEXT(R"("variable":"var_Bools")"), nullptr },
		{ TEXT("Set Int Array"), TEXT("setIntArray"), TEXT(R"("variable":"var_Ints")"), nullptr },
		{ TEXT("Set Float Array"), TEXT("setFloatArray"), TEXT(R"("variable":"var_Floats")"), nullptr },
		{ TEXT("Set String Array"), TEXT("setStringArray"), TEXT(R"("variable":"var_Strings")"), nullptr },
		{ TEXT("Set Image Array"), TEXT("setImageArray"), TEXT(R"("variable":"var_Images")"), nullptr },
		{ TEXT("Set Character Array"), TEXT("setCharacterArray"), TEXT(R"("variable":"var_Characters")"), nullptr },
		{ TEXT("Set Data Asset Array"), TEXT("setDataAssetRefArray"), TEXT(R"("variable":"var_DataAssets")"), nullptr },
		{ TEXT("Set Audio Array"), TEXT("setAudioArray"), TEXT(R"("variable":"var_Audios")"), nullptr },
		{ TEXT("Random Branch whose selected output is unconnected"), TEXT("randomBranch"), TEXT(R"("options":[{"id":"only","weight":1}])"), nullptr },
		{ TEXT("Random Branch with every weight at zero"), TEXT("randomBranch"), TEXT(R"("options":[{"id":"only","weight":1}])"),
			[](FScript& Script, const FString& Id) { Script.Edge(TEXT("zero"), TEXT("integer-"), Id, TEXT("integer-only")); } },
		{ TEXT("Switch On Enum with no output for the value"), TEXT("switchOnEnum"), TEXT(R"("variable":"var_Mode","enumValues":["x"])"), nullptr },
		{ TEXT("Run Script whose output is unconnected"), TEXT("runScript"), TEXT(R"("script":"empty.json")"), nullptr },
		{ TEXT("Block Rollback"), TEXT("blockRollback"), TEXT(""), nullptr },
		{ TEXT("Run Flow into an Entry Flow with nothing after it"), TEXT("runFlow"), TEXT(R"("flowId":"side")"), nullptr },
		{ TEXT("Branch whose taken output is unconnected"), TEXT("branch"), TEXT(R"("value":true)"), nullptr },
		{ TEXT("For Each whose Completed is unconnected"), TEXT("forEachIntLoop"), TEXT(""),
			[](FScript& Script, const FString& Id)
			{
				Script.Node(Id + TEXT("Array"), TEXT("getIntArray"), TEXT(R"("variable":"var_Numbers")"))
					.Edge(Id + TEXT("Array"), TEXT("integer-array-"), Id, TEXT("integer-array-array"))
					.Node(Id + TEXT("Step"), TEXT("setInt"), StepOne)
					.Edge(Id, TEXT("loopBody"), Id + TEXT("Step"));
			} },
		{ TEXT("For Each Map with no map connected"), TEXT("forEachMap"), TEXT(""), nullptr },
	};
	const int32 TailCount = UE_ARRAY_COUNT(Tails);

	FString Go(const int32 Tail) { return FString::Printf(TEXT("go%d"), Tail); }
	FString Bare(const int32 Tail) { return FString::Printf(TEXT("bare%d"), Tail); }

	/** What the tails lean on: the zero weight, the empty Entry Flow and every variable they name. */
	void TailSupport(FScript& Script)
	{
		Script.Node(TEXT("zero"), TEXT("getInt"), TEXT(R"("variable":"var_Zero")"))
			.Node(TEXT("entry"), TEXT("entryFlow"), TEXT(R"("flowId":"side")"));
		Script.Flows.Add(TEXT(R"({"id":"side","name":"Side"})"));
		// Every Set Array tail writes its own scratch array, so none of them can touch the looped one.
		Script.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray)
			.Variable(TEXT("Pairs"), TEXT("map"), TEXT(R"([{"key":"a","value":1},{"key":"b","value":2}])"), TEXT(R"("keyType":"string","valueType":"integer")"))
			.Variable(TEXT("Step"), TEXT("integer"), TEXT("0"))
			.Variable(TEXT("Zero"), TEXT("integer"), TEXT("0"))
			.Variable(TEXT("Mode"), TEXT("enum"), TEXT(R"("x")"), TEXT(R"("enumValues":["x"])"))
			.Variable(TEXT("Bools"), TEXT("boolean"), TEXT("[]"), IsArray)
			.Variable(TEXT("Ints"), TEXT("integer"), TEXT("[]"), IsArray)
			.Variable(TEXT("Floats"), TEXT("float"), TEXT("[]"), IsArray)
			.Variable(TEXT("Strings"), TEXT("string"), TEXT("[]"), IsArray)
			.Variable(TEXT("Images"), TEXT("image"), TEXT("[]"), IsArray)
			.Variable(TEXT("Characters"), TEXT("character"), TEXT("[]"), IsArray)
			.Variable(TEXT("DataAssets"), TEXT("dataAsset"), TEXT("[]"), IsArray)
			.Variable(TEXT("Audios"), TEXT("audio"), TEXT("[]"), IsArray);
	}

	/** Line A with no options, advancing straight into the tail: nothing is connected after it. */
	FScript DeadEnd(const FTail& Tail)
	{
		FScript Script;
		Script.Line(TEXT("A")).Node(TEXT("bare"), Tail.Type, Tail.Fields).Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT(""), TEXT("bare"));
		if (Tail.Wire) { Tail.Wire(Script, TEXT("bare")); }
		TailSupport(Script);
		return Script;
	}

	/**
	 * Line A with, for every tail, the option go{N} that runs a For Each over [1, 2] ending on it:
	 *   For Each -> Loop Body -> Add To Array(Log, element) -> tail, Completed -> Set Int(Done = 1)
	 * and the option bare{N} that reaches a copy of the tail outside any loop. The option empty runs
	 * a For Each with nothing on Loop Body. The option next leaves the line: line B, or End when
	 * the line sits in a called script.
	 */
	FScript LineWithLoops(const bool bMapLoop, const bool bCalled)
	{
		FScript Script;
		TArray<FString> Options;
		for (int32 Tail = 0; Tail < TailCount; ++Tail)
		{
			Options.Add(Go(Tail));
			Options.Add(Bare(Tail));
		}
		Options.Add(TEXT("empty"));
		Options.Add(TEXT("next"));
		Script.Line(TEXT("A"), Options).Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT("next"), TEXT("B"));
		if (bCalled)
		{
			Script.Node(TEXT("B"), TEXT("end"));
		}
		else
		{
			Script.Line(TEXT("B"));
		}

		const auto Loop = [&Script, bMapLoop](const FString& Id)
		{
			if (bMapLoop) { Script.ForEachMap(Id, TEXT("Pairs")); } else { Script.ForEach(Id, TEXT("Numbers")); }
		};
		for (int32 Tail = 0; Tail < TailCount; ++Tail)
		{
			const FString N = FString::FromInt(Tail);
			Loop(TEXT("loop") + N);
			Script.Log(TEXT("log") + N, TEXT("loop") + N, bMapLoop ? TEXT("integer-value") : TEXT("integer-element"))
				.Node(TEXT("tail") + N, Tails[Tail].Type, Tails[Tail].Fields)
				.Node(TEXT("done") + N, TEXT("setInt"), DoneOne)
				.Node(TEXT("bare") + N, Tails[Tail].Type, Tails[Tail].Fields)
				.Edge(TEXT("A"), Go(Tail), TEXT("loop") + N)
				.Edge(TEXT("loop") + N, TEXT("loopBody"), TEXT("log") + N)
				.Edge(TEXT("log") + N, TEXT("1"), TEXT("tail") + N)
				.Edge(TEXT("loop") + N, TEXT("completed"), TEXT("done") + N)
				.Edge(TEXT("A"), Bare(Tail), TEXT("bare") + N);
			if (Tails[Tail].Wire)
			{
				Tails[Tail].Wire(Script, TEXT("tail") + N);
				Tails[Tail].Wire(Script, TEXT("bare") + N);
			}
		}
		Loop(TEXT("hollow"));
		Script.Node(TEXT("hollowDone"), TEXT("setInt"), DoneOne)
			.Edge(TEXT("A"), TEXT("empty"), TEXT("hollow"))
			.Edge(TEXT("hollow"), TEXT("completed"), TEXT("hollowDone"));

		TailSupport(Script);
		return Script;
	}

	/** empty: Start -> End. */
	FScript Empty()
	{
		FScript Script;
		Script.Node(TEXT("end"), TEXT("end")).Edge(TEXT("0"), TEXT(""), TEXT("end"));
		return Script;
	}

	/** visit: Visit -> End. One Visit line per call. */
	FScript Visit()
	{
		FScript Script;
		Script.Line(TEXT("Visit")).Node(TEXT("end"), TEXT("end")).Edge(TEXT("0"), TEXT(""), TEXT("Visit")).Edge(TEXT("Visit"), TEXT(""), TEXT("end"));
		return Script;
	}

	/**
	 * For Each [1, 2, 3] -> Run Script(Callee) -> Set Int(Last = element), Completed -> line Finished,
	 * or End when this script is itself a called one.
	 */
	FScript PerElement(const FString& Callee, const bool bCalled)
	{
		FScript Script;
		Script.ForEach(TEXT("loop"), TEXT("Numbers"))
			.Node(TEXT("call"), TEXT("runScript"), FString::Printf(TEXT(R"("script":"%s.json")"), *Callee))
			.Node(TEXT("after"), TEXT("setInt"), TEXT(R"("variable":"var_Last","isGlobal":true)"))
			.Edge(TEXT("0"), TEXT(""), TEXT("loop"))
			.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("call"))
			.Edge(TEXT("call"), TEXT("output"), TEXT("after"))
			.Edge(TEXT("loop"), TEXT("integer-element"), TEXT("after"), TEXT("integer-2"))
			.Edge(TEXT("loop"), TEXT("completed"), TEXT("Finished"))
			.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2,3]"), IsArray);
		if (bCalled)
		{
			Script.Node(TEXT("Finished"), TEXT("end"));
		}
		else
		{
			Script.Line(TEXT("Finished"));
		}
		return Script;
	}

	/** A -> Run Script(Callee) -> B, and with a second call -> Run Script(Callee) -> C. */
	FScript Story(const FString& Callee, const bool bSecondCall)
	{
		const FString Call = FString::Printf(TEXT(R"("script":"%s.json")"), *Callee);
		FScript Script;
		Script.Line(TEXT("A")).Node(TEXT("first"), TEXT("runScript"), Call).Line(TEXT("B"))
			.Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT(""), TEXT("first")).Edge(TEXT("first"), TEXT("output"), TEXT("B"));
		if (bSecondCall)
		{
			Script.Node(TEXT("second"), TEXT("runScript"), Call).Line(TEXT("C"))
				.Edge(TEXT("B"), TEXT(""), TEXT("second")).Edge(TEXT("second"), TEXT("output"), TEXT("C"));
		}
		return Script;
	}

	/**
	 * scan: For Each [1, 2, 3] -> log the element -> Branch(Stop): false -> Set Bool(Stop = true), true -> End.
	 * The first element arms Stop, the second returns from inside the loop body.
	 */
	FScript Scan()
	{
		FScript Script;
		Script.ForEach(TEXT("loop"), TEXT("Numbers")).Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
			.Node(TEXT("stop"), TEXT("getBool"), TEXT(R"("variable":"var_Stop")"))
			.Node(TEXT("gate"), TEXT("branch"))
			.Node(TEXT("arm"), TEXT("setBool"), TEXT(R"("variable":"var_Stop","value":true)"))
			.Node(TEXT("end"), TEXT("end"))
			.Edge(TEXT("0"), TEXT(""), TEXT("loop"))
			.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("log"))
			.Edge(TEXT("log"), TEXT("1"), TEXT("gate"))
			.Edge(TEXT("gate"), TEXT("false"), TEXT("arm"))
			.Edge(TEXT("gate"), TEXT("true"), TEXT("end"))
			.Edge(TEXT("stop"), TEXT("boolean-"), TEXT("gate"), TEXT("boolean-condition"))
			.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2,3]"), IsArray)
			.Variable(TEXT("Stop"), TEXT("boolean"), TEXT("false"));
		return Script;
	}

	/** chapter: For Each [10, 20, 30] -> log the element -> Run Script(scan) -> Set Int, Completed -> End. */
	FScript Chapter()
	{
		FScript Script;
		Script.ForEach(TEXT("outer"), TEXT("Tens")).Log(TEXT("note"), TEXT("outer"), TEXT("integer-element"))
			.Node(TEXT("call"), TEXT("runScript"), TEXT(R"("script":"scan.json")"))
			.Node(TEXT("after"), TEXT("setInt"), StepOne)
			.Node(TEXT("end"), TEXT("end"))
			.Edge(TEXT("0"), TEXT(""), TEXT("outer"))
			.Edge(TEXT("outer"), TEXT("loopBody"), TEXT("note"))
			.Edge(TEXT("note"), TEXT("1"), TEXT("call"))
			.Edge(TEXT("call"), TEXT("output"), TEXT("after"))
			.Edge(TEXT("outer"), TEXT("completed"), TEXT("end"))
			.Variable(TEXT("Tens"), TEXT("integer"), TEXT("[10,20,30]"), IsArray)
			.Variable(TEXT("Step"), TEXT("integer"), TEXT("0"));
		return Script;
	}

	/**
	 * walk: For Each [1, 2] -> log the element -> Branch(Inner): false -> Set Bool(Inner = true) -> Run Script(walk) -> Set Int,
	 * true -> End. The inner call returns on its first element, then the outer call on its second.
	 */
	FScript Walk()
	{
		FScript Script;
		Script.ForEach(TEXT("loop"), TEXT("Numbers")).Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
			.Node(TEXT("inner"), TEXT("getBool"), TEXT(R"("variable":"var_Inner","isGlobal":true)"))
			.Node(TEXT("gate"), TEXT("branch"))
			.Node(TEXT("mark"), TEXT("setBool"), TEXT(R"("variable":"var_Inner","value":true,"isGlobal":true)"))
			.Node(TEXT("again"), TEXT("runScript"), TEXT(R"("script":"walk.json")"))
			.Node(TEXT("after"), TEXT("setInt"), StepOne)
			.Node(TEXT("end"), TEXT("end"))
			.Edge(TEXT("0"), TEXT(""), TEXT("loop"))
			.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("log"))
			.Edge(TEXT("log"), TEXT("1"), TEXT("gate"))
			.Edge(TEXT("gate"), TEXT("false"), TEXT("mark"))
			.Edge(TEXT("mark"), TEXT("1"), TEXT("again"))
			.Edge(TEXT("again"), TEXT("output"), TEXT("after"))
			.Edge(TEXT("gate"), TEXT("true"), TEXT("end"))
			.Edge(TEXT("inner"), TEXT("boolean-"), TEXT("gate"), TEXT("boolean-condition"))
			.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray)
			.Variable(TEXT("Step"), TEXT("integer"), TEXT("0"));
		return Script;
	}

	/**
	 * halting: For Each [1, 2, 3] -> Branch(Stop): false -> Set Bool(Stop = true),
	 * true -> Set Int(Seen = element) -> Run Script with no script selected, where the walk stops.
	 * The second element leaves the loop mid-way.
	 */
	FScript Halting()
	{
		FScript Script;
		Script.ForEach(TEXT("loop"), TEXT("Numbers"))
			.Node(TEXT("stop"), TEXT("getBool"), TEXT(R"("variable":"var_Stop")"))
			.Node(TEXT("gate"), TEXT("branch"))
			.Node(TEXT("arm"), TEXT("setBool"), TEXT(R"("variable":"var_Stop","value":true)"))
			.Node(TEXT("seen"), TEXT("setInt"), TEXT(R"("variable":"var_Seen","isGlobal":true)"))
			.Node(TEXT("halt"), TEXT("runScript"))
			.Edge(TEXT("0"), TEXT(""), TEXT("loop"))
			.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("gate"))
			.Edge(TEXT("gate"), TEXT("false"), TEXT("arm"))
			.Edge(TEXT("gate"), TEXT("true"), TEXT("seen"))
			.Edge(TEXT("seen"), TEXT("1"), TEXT("halt"))
			.Edge(TEXT("stop"), TEXT("boolean-"), TEXT("gate"), TEXT("boolean-condition"))
			.Edge(TEXT("loop"), TEXT("integer-element"), TEXT("seen"), TEXT("integer-2"))
			.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2,3]"), IsArray)
			.Variable(TEXT("Stop"), TEXT("boolean"), TEXT("false"));
		return Script;
	}

	/** An imported project of test scripts, a component to play them and what it reported. */
	struct FFixture
	{
		StoryFlowTestWorld::FScopedWorld W;
		UStoryFlowProjectAsset* Project = nullptr;
		UStoryFlowTagAccumulator* Errors = nullptr;
		UStoryFlowTagAccumulator* Started = nullptr;
		const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LoopTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString Content = TEXT("/Game/StoryFlowLoopTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Slot = TEXT("StoryFlowLoopTests") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TMap<FString, FString> Files;

		void Script(const FString& Name, const FScript& Json) { Files.Add(Name + TEXT(".json"), Json.Json()); }

		bool Import()
		{
			FScript Globals;
			Globals.Variable(TEXT("Log"), TEXT("integer"), TEXT("[]"), IsArray)
				.Variable(TEXT("Done"), TEXT("integer"), TEXT("0"))
				.Variable(TEXT("Last"), TEXT("integer"), TEXT("0"))
				.Variable(TEXT("Seen"), TEXT("integer"), TEXT("0"))
				.Variable(TEXT("Inner"), TEXT("boolean"), TEXT("false"));
			Files.Add(TEXT("global-variables.json"), FString::Printf(TEXT(R"({"variables":{%s},"strings":{"en":{}},"assets":{}})"), *FString::Join(Globals.Variables, TEXT(","))));
			Files.Add(TEXT("project.json"), TEXT(R"({"version":"1.8.2","apiVersion":"1.0","metadata":{},"startupScript":"main.json"})"));

			const FString Build = FPaths::Combine(Root, TEXT("build"));
			IFileManager::Get().MakeDirectory(*Build, true);
			for (const TPair<FString, FString>& File : Files)
			{
				if (!FFileHelper::SaveStringToFile(File.Value, *FPaths::Combine(Build, File.Key))) { return false; }
			}
			if (!W.Init()) { return false; }
			Project = UStoryFlowImporter::ImportProject(Build, Content);
			if (!Project) { return false; }
			W.Subsystem->SetProject(Project);
			W.Component->bTraceEnabled = false;
			Errors = NewObject<UStoryFlowTagAccumulator>(W.Component);
			Started = NewObject<UStoryFlowTagAccumulator>(W.Component);
			W.Component->OnError.AddDynamic(Errors, &UStoryFlowTagAccumulator::OnTag);
			W.Component->OnScriptStarted.AddDynamic(Started, &UStoryFlowTagAccumulator::OnTag);
			return true;
		}

		~FFixture()
		{
			if (W.Component) { W.Component->StopDialogue(); }
			if (W.Subsystem) { W.Subsystem->SetProject(nullptr); }
			UStoryFlowSubsystem::DeleteSave(Slot);
			if (Project) { UEditorAssetLibrary::DeleteDirectory(Content); }
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		/** A fresh run of one script: every variable back at its exported value, nothing reported yet. */
		void Start(const FString& ScriptPath, const bool bRollback)
		{
			W.Component->StopDialogue();
			W.Subsystem->ResetAllState();
			Errors->Tags.Reset();
			Started->Tags.Reset();
			Project->Metadata.DialogueRollback.bEnabled = bRollback;
			W.Component->StartDialogueWithScript(ScriptPath);
		}

		FString Line() const { return W.Component->IsWaitingForInput() ? W.Component->GetCurrentDialogue().NodeId : FString(TEXT("no line")); }
		int32 Global(const FString& Name) const { return W.Component->GetIntVariable(Name, true); }
		int32 Calls(const FString& ScriptPath) const { return Started->Tags.FilterByPredicate([&ScriptPath](const FString& Path) { return Path == ScriptPath; }).Num(); }

		FString Array(const FString& Name, const bool bGlobal) const
		{
			TArray<FString> Values;
			for (const int32 Value : W.Component->GetIntArrayVariable(Name, bGlobal))
			{
				Values.Add(FString::FromInt(Value));
			}
			return FString::Join(Values, TEXT(","));
		}

		/** The line on screen, the global Log and Done, and how many errors the component reported. */
		FString State() const
		{
			return FString::Printf(TEXT("%s log=[%s] done=%d errors=%d"), *Line(), *Array(TEXT("Log"), true), Global(TEXT("Done")), Errors->Tags.Num());
		}

		/** Advances through the Visit lines and returns how many were shown before the story left them. */
		int32 Visits() const
		{
			int32 Shown = 0;
			while (Line() == TEXT("Visit") && Shown < 10)
			{
				++Shown;
				W.Component->AdvanceDialogue();
			}
			return Shown;
		}
	};

	FString RunLabel(const bool bRollback) { return bRollback ? TEXT("rollback on, ") : TEXT("rollback off, "); }

	/** The "VAR SET" lines of the execution trace, as they were logged. */
	struct FVarSetTrace : FOutputDevice
	{
		TArray<FString> Lines;

		FVarSetTrace() { GLog->AddOutputDevice(this); }
		virtual ~FVarSetTrace() override { GLog->RemoveOutputDevice(this); }

		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			const FString Line(Text);
			const int32 At = Line.Find(TEXT("VAR SET"));
			if (At != INDEX_NONE) { Lines.Add(Line.Mid(At)); }
		}
	};
}

// ============================================================================
// A body that ends on a node with nothing connected after it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopBodyEndTest,
	"StoryFlow.Loops.BodyEndingOnUnconnectedOutputFinishesIteration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopBodyEndTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	FFixture F;
	F.Script(TEXT("array"), LineWithLoops(false, false));
	F.Script(TEXT("map"), LineWithLoops(true, false));
	F.Script(TEXT("empty"), Empty());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		for (const TCHAR* Loop : { TEXT("array"), TEXT("map") })
		{
			const FString Run = RunLabel(bRollback) + Loop + TEXT(" loop, ");
			for (int32 Tail = 0; Tail < TailCount; ++Tail)
			{
				const FString Label = Run + Tails[Tail].Name;
				F.Start(Loop, bRollback);
				F.W.Component->SelectOption(Go(Tail));
				TestEqual(Label + TEXT(": the loop runs every element and reaches Completed"), F.State(), FString(TEXT("A log=[1,2] done=1 errors=0")));
				F.W.Component->SelectOption(Go(Tail));
				TestEqual(Label + TEXT(": its next run starts at the first element"), F.State(), FString(TEXT("A log=[1,2,1,2] done=1 errors=0")));
				F.W.Component->SelectOption(TEXT("next"));
				TestEqual(Label + TEXT(": the story carries on"), F.State(), FString(TEXT("B log=[1,2,1,2] done=1 errors=0")));
			}
			F.Start(Loop, bRollback);
			F.W.Component->SelectOption(TEXT("empty"));
			TestEqual(Run + TEXT("nothing on Loop Body: the loop reaches Completed"), F.State(), FString(TEXT("A log=[] done=1 errors=0")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopCalledBodyEndTest,
	"StoryFlow.Loops.BodyEndInCalledScriptFinishesIteration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopCalledBodyEndTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// caller: For Each [1, 2, 3] -> Run Script(callee), Completed -> Finished. The line with the loops
	// sits in the called script, so the caller's own loop is parked while they run.
	FFixture F;
	F.Script(TEXT("caller"), PerElement(TEXT("callee"), false));
	F.Script(TEXT("callee"), LineWithLoops(false, true));
	F.Script(TEXT("empty"), Empty());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		for (int32 Tail = 0; Tail < TailCount; ++Tail)
		{
			const FString Label = RunLabel(bRollback) + Tails[Tail].Name;
			F.Start(TEXT("caller"), bRollback);
			F.W.Component->SelectOption(Go(Tail));
			TestEqual(Label + TEXT(": the called script's loop runs every element and reaches Completed"), F.State(), FString(TEXT("A log=[1,2] done=1 errors=0")));
			F.W.Component->SelectOption(Go(Tail));
			TestEqual(Label + TEXT(": its next run starts at the first element"), F.State(), FString(TEXT("A log=[1,2,1,2] done=1 errors=0")));
			// Next ends the called script, and the caller's loop calls it again for its second element.
			F.W.Component->SelectOption(TEXT("next"));
			TestEqual(Label + TEXT(": the caller's loop goes on to its second element"), F.State(), FString(TEXT("A log=[1,2,1,2] done=1 errors=0")));
			TestEqual(Label + TEXT(": the second element calls the script again"), F.Calls(TEXT("callee")), 2);
			F.W.Component->SelectOption(TEXT("next"));
			F.W.Component->SelectOption(TEXT("next"));
			TestEqual(Label + TEXT(": the caller's loop reaches Completed"), F.State(), FString(TEXT("Finished log=[1,2,1,2] done=1 errors=0")));
			TestEqual(Label + TEXT(": one call per element"), F.Calls(TEXT("callee")), 3);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopOutsideTest,
	"StoryFlow.Loops.UnconnectedOutputOutsideLoopEndsWalk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopOutsideTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	FFixture F;
	F.Script(TEXT("array"), LineWithLoops(false, false));
	F.Script(TEXT("empty"), Empty());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		for (int32 Tail = 0; Tail < TailCount; ++Tail)
		{
			const FString Label = RunLabel(bRollback) + Tails[Tail].Name;
			F.Start(TEXT("array"), bRollback);
			F.W.Component->SelectOption(Bare(Tail));
			TestEqual(Label + TEXT(": the walk ends there and the line stays"), F.State(), FString(TEXT("A log=[] done=0 errors=0")));
			F.W.Component->SelectOption(TEXT("next"));
			TestEqual(Label + TEXT(": the story carries on"), F.State(), FString(TEXT("B log=[] done=0 errors=0")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopLineInsideBodyTest,
	"StoryFlow.Loops.UnconnectedOptionKeepsLineInsideBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopLineInsideBodyTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// inside: For Each [1, 2] -> Loop Body -> line Item, Completed -> line After. Item.take -> Add To Array(Log, element).
	// The editor refuses a line inside a For Each body. This plugin has always played one, so an option
	// with nothing connected has to keep doing what SelectOption promises: stay on the line. Only a
	// node's own output finishes the iteration.
	FFixture F;
	FScript Inside;
	Inside.ForEach(TEXT("loop"), TEXT("Numbers")).Line(TEXT("Item"), { TEXT("stay"), TEXT("take") }).Line(TEXT("After"))
		.Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
		.Edge(TEXT("0"), TEXT(""), TEXT("loop"))
		.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("Item"))
		.Edge(TEXT("Item"), TEXT("take"), TEXT("log"))
		.Edge(TEXT("loop"), TEXT("completed"), TEXT("After"))
		.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray);
	F.Script(TEXT("inside"), Inside);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		const FString Run = RunLabel(bRollback);
		F.Start(TEXT("inside"), bRollback);
		const uint64 Entry = F.W.Component->GetDialogueEntrySerial();
		F.W.Component->SelectOption(TEXT("stay"));
		TestEqual(Run + TEXT("an option with nothing connected stays on the line"), F.State(), FString(TEXT("Item log=[] done=0 errors=0")));
		TestEqual(Run + TEXT("staying on the line is a redraw, not the next element"), F.W.Component->GetDialogueEntrySerial() == Entry, true);
		F.W.Component->SelectOption(TEXT("take"));
		TestEqual(Run + TEXT("a body that ends on a node moves on to the second element"), F.State(), FString(TEXT("Item log=[1] done=0 errors=0")));
		F.W.Component->SelectOption(TEXT("stay"));
		F.W.Component->SelectOption(TEXT("take"));
		TestEqual(Run + TEXT("the loop reaches Completed after its last element"), F.State(), FString(TEXT("After log=[1,2] done=0 errors=0")));
	}
	return true;
}

// ============================================================================
// End inside a For Each body of a called script
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopEarlyReturnTest,
	"StoryFlow.Loops.EndInsideBodyAbandonsCalledScriptLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopEarlyReturnTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	FFixture F;
	F.Script(TEXT("twice"), Story(TEXT("scan"), true));
	F.Script(TEXT("nested"), Story(TEXT("chapter"), false));
	F.Script(TEXT("recursive"), Story(TEXT("walk"), false));
	F.Script(TEXT("scan"), Scan());
	F.Script(TEXT("chapter"), Chapter());
	F.Script(TEXT("walk"), Walk());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		const FString Run = RunLabel(bRollback);

		// twice: A -> Run Script(scan) -> B -> Run Script(scan) -> C
		F.Start(TEXT("twice"), bRollback);
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("the first call returns from its second element"), F.State(), FString(TEXT("B log=[1,2] done=0 errors=0")));
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("the next call runs the loop from its first element"), F.State(), FString(TEXT("C log=[1,2,1,2] done=0 errors=0")));

		// nested: A -> Run Script(chapter) -> B, and chapter calls scan from its own loop body
		F.Start(TEXT("nested"), bRollback);
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("a loop parked in the caller keeps its cursor while each call starts the left loop afresh"), F.State(), FString(TEXT("B log=[10,1,2,20,1,2,30,1,2] done=0 errors=0")));

		// recursive: A -> Run Script(walk) -> B, and walk calls itself from inside its loop
		F.Start(TEXT("recursive"), bRollback);
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("a script that calls itself from inside its loop: the inner End leaves the caller's cursor alone"), F.State(), FString(TEXT("B log=[1,1,2] done=0 errors=0")));
	}
	return true;
}

// ============================================================================
// Save and Load around a parked loop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopSaveScopeTest,
	"StoryFlow.Loops.SaveInsideCalledScriptLeavesParkedLoopAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopSaveScopeTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// counting: For Each [1, 2, 3] -> Run Script(visit) -> Set Int(Last = element), Completed -> Finished.
	// A slot holds the variables and nothing of where the story stands, and Load is refused while a
	// dialogue runs, so what can be held here is that neither disturbs the loop the caller parked.
	FFixture F;
	F.Script(TEXT("counting"), PerElement(TEXT("visit"), false));
	F.Script(TEXT("visit"), Visit());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }
	const auto Position = [&F]() { return FString::Printf(TEXT("%s last=%d errors=%d"), *F.Line(), F.Global(TEXT("Last")), F.Errors->Tags.Num()); };

	F.Start(TEXT("counting"), false);
	TestEqual(TEXT("no Save or Load: the first iteration"), Position(), FString(TEXT("Visit last=0 errors=0")));
	F.W.Component->AdvanceDialogue();
	TestEqual(TEXT("no Save or Load: the second iteration"), Position(), FString(TEXT("Visit last=1 errors=0")));
	F.W.Component->AdvanceDialogue();
	TestEqual(TEXT("no Save or Load: the third iteration"), Position(), FString(TEXT("Visit last=2 errors=0")));
	F.W.Component->AdvanceDialogue();
	TestEqual(TEXT("no Save or Load: Completed"), Position(), FString(TEXT("Finished last=3 errors=0")));

	F.Start(TEXT("counting"), false);
	F.W.Component->AdvanceDialogue();
	TestEqual(TEXT("Save succeeds while the called script waits on its line"), F.W.Subsystem->SaveToSlot(F.Slot), true);
	AddExpectedError(TEXT("Cannot load while a dialogue is active"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("Load is refused while the story runs"), F.W.Subsystem->LoadFromSlot(F.Slot), false);
	TestEqual(TEXT("Save and the refused Load leave the line"), Position(), FString(TEXT("Visit last=1 errors=0")));
	F.W.Component->AdvanceDialogue();
	TestEqual(TEXT("after Save: the remaining iteration"), Position(), FString(TEXT("Visit last=2 errors=0")));
	F.W.Component->AdvanceDialogue();
	TestEqual(TEXT("after Save: Completed"), Position(), FString(TEXT("Finished last=3 errors=0")));

	F.W.Component->StopDialogue();
	TestEqual(TEXT("Load succeeds between dialogues"), F.W.Subsystem->LoadFromSlot(F.Slot), true);
	TestEqual(TEXT("Load brings the saved variables back"), Position(), FString(TEXT("no line last=1 errors=0")));
	F.W.Component->StartDialogueWithScript(TEXT("counting"));
	TestEqual(TEXT("the slot holds no script position: the story starts over and the loop runs every element"), F.Visits(), 3);
	TestEqual(TEXT("the loop started after Load reaches Completed"), Position(), FString(TEXT("Finished last=3 errors=0")));
	return true;
}

// ============================================================================
// A loop in progress when the session position is replaced
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopReplacedPositionTest,
	"StoryFlow.Loops.LoadResetAndRestartLeaveNoLoopPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopReplacedPositionTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// A -> Run Script(chapter) -> B -> Run Script(chapter) -> C, the chapter being the script under test.
	// visiting: For Each [1, 2, 3] -> Run Script(visit) -> Set Int, Completed -> End. One Visit line per
	// iteration, so the lines shown count the iterations that ran.
	FFixture F;
	F.Script(TEXT("visits"), Story(TEXT("visiting"), true));
	F.Script(TEXT("visiting"), PerElement(TEXT("visit"), true));
	F.Script(TEXT("visit"), Visit());
	F.Script(TEXT("halts"), Story(TEXT("halting"), true));
	F.Script(TEXT("halting"), Halting());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }
	// Every run of halting stops on its Run Script with no script selected.

	for (const bool bRollback : { false, true })
	{
		const FString Run = RunLabel(bRollback);

		F.Start(TEXT("visits"), bRollback);
		TestEqual(Run + TEXT("Save succeeds on the first line"), F.W.Subsystem->SaveToSlot(F.Slot), true);
		F.W.Component->AdvanceDialogue();
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("the caller's loop is parked on its second element"), F.State(), FString(TEXT("Visit log=[] done=0 errors=0")));
		F.W.Component->StopDialogue();
		TestEqual(Run + TEXT("Load succeeds once the story is stopped"), F.W.Subsystem->LoadFromSlot(F.Slot), true);
		F.W.Component->StartDialogueWithScript(TEXT("visits"));
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("Load taken while the loop is parked: its next run plays every iteration"), F.Visits(), 3);
		TestEqual(Run + TEXT("Load taken while the loop is parked: the story carries on"), F.State(), FString(TEXT("B log=[] done=0 errors=0")));

		F.Start(TEXT("halts"), bRollback);
		F.W.Subsystem->SaveToSlot(F.Slot);
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("the walk stops inside the loop body on the second element"), F.Global(TEXT("Seen")), 2);
		F.W.Component->StopDialogue();
		F.W.Subsystem->LoadFromSlot(F.Slot);
		TestEqual(Run + TEXT("Load brings the saved variables back"), F.Global(TEXT("Seen")), 0);
		F.W.Component->StartDialogueWithScript(TEXT("halts"));
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("Load taken after a walk stopped inside the loop body: its next run starts at the first element"), F.Global(TEXT("Seen")), 2);
		F.W.Component->SetIntVariable(TEXT("Seen"), 0, true);
		F.W.Component->StartDialogueWithScript(TEXT("halts"));
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("Restart taken after a walk stopped inside the loop body: its next run starts at the first element"), F.Global(TEXT("Seen")), 2);

		F.Start(TEXT("visits"), bRollback);
		F.W.Component->AdvanceDialogue();
		F.W.Component->AdvanceDialogue();
		F.W.Subsystem->ResetAllState();
		F.W.Component->ResetVariables();
		TestEqual(Run + TEXT("Reset from the called script: the caller's loop runs on to its end"), F.Visits(), 2);
		TestEqual(Run + TEXT("Reset from the called script: the story carries on"), F.State(), FString(TEXT("B log=[] done=0 errors=0")));
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("Reset from the called script: the loop's next run plays every iteration"), F.Visits(), 3);
		TestEqual(Run + TEXT("Reset from the called script: the story reaches its last line"), F.State(), FString(TEXT("C log=[] done=0 errors=0")));

		F.Start(TEXT("visits"), bRollback);
		F.W.Component->AdvanceDialogue();
		F.W.Component->AdvanceDialogue();
		F.W.Component->StartDialogueWithScript(TEXT("visits"));
		TestEqual(Run + TEXT("Restart from the called script: the story is back on its first line"), F.State(), FString(TEXT("A log=[] done=0 errors=0")));
		F.W.Component->AdvanceDialogue();
		TestEqual(Run + TEXT("Restart from the called script: the restarted story plays every iteration"), F.Visits(), 3);
		TestEqual(Run + TEXT("Restart from the called script: the story carries on"), F.State(), FString(TEXT("B log=[] done=0 errors=0")));
	}
	return true;
}

// ============================================================================
// Rollback: restoring the line after an option ran a loop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopRollbackRestoreTest,
	"StoryFlow.Loops.RollbackRestoresLineAfterLoopOption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopRollbackRestoreTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	FFixture F;
	F.Script(TEXT("array"), LineWithLoops(false, false));
	F.Script(TEXT("empty"), Empty());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (int32 Tail = 0; Tail < TailCount; ++Tail)
	{
		const FString Label = Tails[Tail].Name;

		// Save right after the option, then Load the way the plugin allows it: between dialogues.
		F.Start(TEXT("array"), true);
		F.W.Component->SelectOption(Go(Tail));
		TestEqual(Label + TEXT(": Save succeeds right after the option"), F.W.Subsystem->SaveToSlot(F.Slot), true);
		F.W.Component->StopDialogue();
		F.W.Subsystem->ResetAllState();
		TestEqual(Label + TEXT(": Load succeeds"), F.W.Subsystem->LoadFromSlot(F.Slot), true);
		F.W.Component->StartDialogueWithScript(TEXT("array"));
		TestEqual(Label + TEXT(": Save then Load restores the line without an error"), F.State(), FString(TEXT("A log=[1,2] done=1 errors=0")));
		F.W.Component->SelectOption(Go(Tail));
		TestEqual(Label + TEXT(": after Load the loop runs every element again"), F.State(), FString(TEXT("A log=[1,2,1,2] done=1 errors=0")));
		F.W.Component->SelectOption(TEXT("next"));
		TestEqual(Label + TEXT(": after Load the story carries on"), F.State(), FString(TEXT("B log=[1,2,1,2] done=1 errors=0")));

		// A Block Rollback in the body is a barrier by design: it leaves nothing to go back to.
		if (FCString::Strcmp(Tails[Tail].Type, TEXT("blockRollback")) == 0) { continue; }

		// Back from the next line restores the line as it was entered, before the option ran.
		F.Start(TEXT("array"), true);
		F.W.Component->SelectOption(Go(Tail));
		F.W.Component->SelectOption(TEXT("next"));
		TestEqual(Label + TEXT(": Back succeeds from the next line"), F.W.Component->GoBack().bOk, true);
		TestEqual(Label + TEXT(": Back restores the line without an error"), F.State(), FString(TEXT("A log=[] done=0 errors=0")));
		F.W.Component->SelectOption(Go(Tail));
		TestEqual(Label + TEXT(": the restored line runs the whole loop"), F.State(), FString(TEXT("A log=[1,2] done=1 errors=0")));
		F.W.Component->SelectOption(TEXT("next"));
		TestEqual(Label + TEXT(": after Back the story carries on"), F.State(), FString(TEXT("B log=[1,2] done=1 errors=0")));
	}
	return true;
}

// ============================================================================
// Loops: the snapshot they iterate and how far they can go
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopSnapshotTest,
	"StoryFlow.Loops.IteratesSnapshotTakenAtLoopStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// array: For Each Numbers [1, 2] -> Add To Array(Numbers, 9) -> log the element, Completed -> Set Int(Done = 1).
	// map: For Each Pairs {a: 1, b: 2} -> Set Map Value(Pairs, z = 5) -> log the value, Completed -> Set Int(Done = 1).
	// The body grows what it iterates, and the loop still ends after the elements it started with.
	FFixture F;
	FScript Array, Map;
	Array.Line(TEXT("A"), { TEXT("go") }).ForEach(TEXT("loop"), TEXT("Numbers"))
		.Node(TEXT("growArray"), TEXT("getIntArray"), TEXT(R"("variable":"var_Numbers")"))
		.Node(TEXT("grow"), TEXT("addToIntArray"), TEXT(R"("value":9)"))
		.Edge(TEXT("growArray"), TEXT("integer-array-"), TEXT("grow"), TEXT("integer-array-2"))
		.Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
		.Node(TEXT("done"), TEXT("setInt"), DoneOne)
		.Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT("go"), TEXT("loop"))
		.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("grow")).Edge(TEXT("grow"), TEXT("1"), TEXT("log"))
		.Edge(TEXT("loop"), TEXT("completed"), TEXT("done"))
		.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray);
	Map.Line(TEXT("A"), { TEXT("go") }).ForEachMap(TEXT("loop"), TEXT("Pairs"))
		.Node(TEXT("growMap"), TEXT("getMap"), TEXT(R"("variable":"var_Pairs","keyType":"string","valueType":"integer")"))
		.Node(TEXT("grow"), TEXT("setMapValue"), TEXT(R"("keyType":"string","valueType":"integer","key":"z","value":5)"))
		.Edge(TEXT("growMap"), TEXT("map-string-integer"), TEXT("grow"), TEXT("map-string-integer-2"))
		.Log(TEXT("log"), TEXT("loop"), TEXT("integer-value"))
		.Node(TEXT("done"), TEXT("setInt"), DoneOne)
		.Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT("go"), TEXT("loop"))
		.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("grow")).Edge(TEXT("grow"), TEXT("1"), TEXT("log"))
		.Edge(TEXT("loop"), TEXT("completed"), TEXT("done"))
		.Variable(TEXT("Pairs"), TEXT("map"), TEXT(R"([{"key":"a","value":1},{"key":"b","value":2}])"), TEXT(R"("keyType":"string","valueType":"integer")"));
	F.Script(TEXT("array"), Array);
	F.Script(TEXT("map"), Map);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	F.Start(TEXT("array"), false);
	F.W.Component->SelectOption(TEXT("go"));
	TestEqual(TEXT("an array the body appends to: the loop ends after the elements it started with"), F.State(), FString(TEXT("A log=[1,2] done=1 errors=0")));
	TestEqual(TEXT("an array the body appends to: the appends land on the variable"), F.Array(TEXT("Numbers"), false), FString(TEXT("1,2,9,9")));
	F.Start(TEXT("map"), false);
	F.W.Component->SelectOption(TEXT("go"));
	TestEqual(TEXT("a map the body adds a key to: the loop ends after the entries it started with"), F.State(), FString(TEXT("A log=[1,2] done=1 errors=0")));
	TestEqual(TEXT("a map the body adds a key to: the key lands on the variable"), F.W.Component->GetMapKeysInOrder(TEXT("Pairs"), false).Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLoopLongTest,
	"StoryFlow.Loops.LongLoopsRunToCompleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLoopLongTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// 700 elements, each iteration logging its element: an iteration that nested inside the one before
	// it would run out of processing depth long before the end.
	// calls: the same array, each iteration calling the empty script and then logging.
	const int32 Count = 700;
	TArray<FString> Elements, Entries;
	for (int32 Index = 1; Index <= Count; ++Index)
	{
		Elements.Add(FString::FromInt(Index));
		Entries.Add(FString::Printf(TEXT(R"({"key":"k%d","value":%d})"), Index, Index));
	}
	const FString Numbers = TEXT("[") + FString::Join(Elements, TEXT(",")) + TEXT("]");
	const FString Pairs = TEXT("[") + FString::Join(Entries, TEXT(",")) + TEXT("]");

	FFixture F;
	FScript Array, Map, Calls;
	Array.Line(TEXT("A"), { TEXT("go") }).ForEach(TEXT("loop"), TEXT("Numbers")).Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
		.Node(TEXT("done"), TEXT("setInt"), DoneOne)
		.Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT("go"), TEXT("loop"))
		.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("log")).Edge(TEXT("loop"), TEXT("completed"), TEXT("done"))
		.Variable(TEXT("Numbers"), TEXT("integer"), Numbers, IsArray);
	Map.Line(TEXT("A"), { TEXT("go") }).ForEachMap(TEXT("loop"), TEXT("Pairs")).Log(TEXT("log"), TEXT("loop"), TEXT("integer-value"))
		.Node(TEXT("done"), TEXT("setInt"), DoneOne)
		.Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT("go"), TEXT("loop"))
		.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("log")).Edge(TEXT("loop"), TEXT("completed"), TEXT("done"))
		.Variable(TEXT("Pairs"), TEXT("map"), Pairs, TEXT(R"("keyType":"string","valueType":"integer")"));
	Calls.Line(TEXT("A"), { TEXT("go") }).ForEach(TEXT("loop"), TEXT("Numbers")).Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
		.Node(TEXT("call"), TEXT("runScript"), TEXT(R"("script":"empty.json")"))
		.Node(TEXT("done"), TEXT("setInt"), DoneOne)
		.Edge(TEXT("0"), TEXT(""), TEXT("A")).Edge(TEXT("A"), TEXT("go"), TEXT("loop"))
		.Edge(TEXT("loop"), TEXT("loopBody"), TEXT("call")).Edge(TEXT("call"), TEXT("output"), TEXT("log"))
		.Edge(TEXT("loop"), TEXT("completed"), TEXT("done"))
		.Variable(TEXT("Numbers"), TEXT("integer"), Numbers, IsArray);
	F.Script(TEXT("array"), Array);
	F.Script(TEXT("map"), Map);
	F.Script(TEXT("calls"), Calls);
	F.Script(TEXT("empty"), Empty());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		for (const TCHAR* Loop : { TEXT("array"), TEXT("map"), TEXT("calls") })
		{
			F.Start(Loop, bRollback);
			F.W.Component->SelectOption(TEXT("go"));
			const TArray<int32> Log = F.W.Component->GetIntArrayVariable(TEXT("Log"), true);
			TestEqual(RunLabel(bRollback) + Loop + TEXT(": 700 iterations run and Completed is taken"),
				FString::Printf(TEXT("%s logged=%d last=%d done=%d errors=%d"), *F.Line(), Log.Num(), Log.Num() ? Log.Last() : 0, F.Global(TEXT("Done")), F.Errors->Tags.Num()),
				FString(TEXT("A logged=700 last=700 done=1 errors=0")));
		}
	}
	return true;
}

// ============================================================================
// Array nodes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowWalkArrayNodesTest,
	"StoryFlow.Walk.ArrayNodesWriteWhatTheEditorWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowWalkArrayNodesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// Line A, one option per case:
	//   unwired: Set Int Array(Numbers) with nothing on its array input
	//   element: Set Array Element(Numbers, index 1, value 9)
	//   copy:    Set Int Array(Copy <- Numbers) -> Add To Array(Copy, 7)
	//   chain:   Add To Array(Numbers, 7) -> Add To Array(the first one's output, 8) -> Set Int Array(Result <- the second one's output)
	FFixture F;
	FScript Script;
	Script.Line(TEXT("A"), { TEXT("unwired"), TEXT("element"), TEXT("copy"), TEXT("chain") }).Edge(TEXT("0"), TEXT(""), TEXT("A"))
		.Node(TEXT("numbers"), TEXT("getIntArray"), TEXT(R"("variable":"var_Numbers")"))
		.Node(TEXT("copied"), TEXT("getIntArray"), TEXT(R"("variable":"var_Copy")"))
		.Node(TEXT("unwired"), TEXT("setIntArray"), TEXT(R"("variable":"var_Numbers")"))
		.Edge(TEXT("A"), TEXT("unwired"), TEXT("unwired"))
		.Node(TEXT("element"), TEXT("setIntArrayElement"), TEXT(R"("value1":1,"value2":9)"))
		.Edge(TEXT("A"), TEXT("element"), TEXT("element"))
		.Edge(TEXT("numbers"), TEXT("integer-array-"), TEXT("element"), TEXT("integer-array-2"))
		.Node(TEXT("copy"), TEXT("setIntArray"), TEXT(R"("variable":"var_Copy")"))
		.Node(TEXT("extend"), TEXT("addToIntArray"), TEXT(R"("value":7)"))
		.Edge(TEXT("A"), TEXT("copy"), TEXT("copy")).Edge(TEXT("copy"), TEXT("1"), TEXT("extend"))
		.Edge(TEXT("numbers"), TEXT("integer-array-"), TEXT("copy"), TEXT("integer-array-2"))
		.Edge(TEXT("copied"), TEXT("integer-array-"), TEXT("extend"), TEXT("integer-array-2"))
		.Node(TEXT("first"), TEXT("addToIntArray"), TEXT(R"("value":7)"))
		.Node(TEXT("second"), TEXT("addToIntArray"), TEXT(R"("value":8)"))
		.Node(TEXT("result"), TEXT("setIntArray"), TEXT(R"("variable":"var_Result")"))
		.Edge(TEXT("A"), TEXT("chain"), TEXT("first")).Edge(TEXT("first"), TEXT("1"), TEXT("second")).Edge(TEXT("second"), TEXT("1"), TEXT("result"))
		.Edge(TEXT("numbers"), TEXT("integer-array-"), TEXT("first"), TEXT("integer-array-2"))
		.Edge(TEXT("first"), TEXT("integer-array-4"), TEXT("second"), TEXT("integer-array-2"))
		.Edge(TEXT("second"), TEXT("integer-array-4"), TEXT("result"), TEXT("integer-array-2"))
		.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray)
		.Variable(TEXT("Copy"), TEXT("integer"), TEXT("[]"), IsArray)
		.Variable(TEXT("Result"), TEXT("integer"), TEXT("[]"), IsArray);
	F.Script(TEXT("arrays"), Script);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }
	const auto Arrays = [&F]() { return FString::Printf(TEXT("%s numbers=[%s] copy=[%s] result=[%s] errors=%d"), *F.Line(), *F.Array(TEXT("Numbers"), false), *F.Array(TEXT("Copy"), false), *F.Array(TEXT("Result"), false), F.Errors->Tags.Num()); };

	F.Start(TEXT("arrays"), false);
	F.W.Component->SelectOption(TEXT("unwired"));
	TestEqual(TEXT("Set Array with nothing on its array input keeps the variable"), Arrays(), FString(TEXT("A numbers=[1,2] copy=[] result=[] errors=0")));
	F.Start(TEXT("arrays"), false);
	F.W.Component->SelectOption(TEXT("element"));
	TestEqual(TEXT("Set Array Element writes the element back to the variable behind its array input"), Arrays(), FString(TEXT("A numbers=[1,9] copy=[] result=[] errors=0")));
	F.Start(TEXT("arrays"), false);
	F.W.Component->SelectOption(TEXT("copy"));
	TestEqual(TEXT("a wired Set Array copies its source: appending to the copy leaves the source alone"), Arrays(), FString(TEXT("A numbers=[1,2] copy=[1,2,7] result=[] errors=0")));
	F.Start(TEXT("arrays"), false);
	F.W.Component->SelectOption(TEXT("chain"));
	TestEqual(TEXT("an array op fed by another op's output works on that output and writes no variable"), Arrays(), FString(TEXT("A numbers=[1,2,7] copy=[] result=[1,2,7,8] errors=0")));
	return true;
}

// ============================================================================
// Walks that end outside a loop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowWalkDeadEndTest,
	"StoryFlow.Walk.AdvanceIntoUnconnectedOutputKeepsLineUsable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowWalkDeadEndTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	FFixture F;
	for (int32 Tail = 0; Tail < TailCount; ++Tail)
	{
		F.Script(FString::Printf(TEXT("dead%d"), Tail), DeadEnd(Tails[Tail]));
	}
	F.Script(TEXT("empty"), Empty());
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		for (int32 Tail = 0; Tail < TailCount; ++Tail)
		{
			const FString Label = RunLabel(bRollback) + Tails[Tail].Name;
			F.Start(FString::Printf(TEXT("dead%d"), Tail), bRollback);
			F.W.Component->AdvanceDialogue();
			TestEqual(Label + TEXT(": the line stays and still takes input"), F.State(), FString(TEXT("A log=[] done=0 errors=0")));
			F.W.Component->AdvanceDialogue();
			TestEqual(Label + TEXT(": and again after a second advance"), F.State(), FString(TEXT("A log=[] done=0 errors=0")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowWalkExitFlowTest,
	"StoryFlow.Walk.ExitFlowWithUnconnectedRouteStaysInScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowWalkExitFlowTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// exits: A -> Run Script(room) -> B, with nothing on the Run Script's Out route.
	// room: line X. leave -> Run Flow(Out, an exit flow). scan -> For Each [1, 2] -> log the element -> Run Flow(Out).
	//       bump -> Set Int(Done = 1). done -> End.
	// The exit has no route in the caller, so the story stays in room, and the loop it left stays open:
	// the next walk that runs out of edges moves it on, as it does in the editor.
	FFixture F;
	FScript Room;
	Room.Line(TEXT("X"), { TEXT("leave"), TEXT("scan"), TEXT("bump"), TEXT("done") }).Edge(TEXT("0"), TEXT(""), TEXT("X"))
		.Node(TEXT("leave"), TEXT("runFlow"), TEXT(R"("flowId":"out")")).Edge(TEXT("X"), TEXT("leave"), TEXT("leave"))
		.ForEach(TEXT("loop"), TEXT("Numbers")).Log(TEXT("log"), TEXT("loop"), TEXT("integer-element"))
		.Node(TEXT("leaveLoop"), TEXT("runFlow"), TEXT(R"("flowId":"out")"))
		.Edge(TEXT("X"), TEXT("scan"), TEXT("loop")).Edge(TEXT("loop"), TEXT("loopBody"), TEXT("log")).Edge(TEXT("log"), TEXT("1"), TEXT("leaveLoop"))
		.Node(TEXT("bump"), TEXT("setInt"), DoneOne).Edge(TEXT("X"), TEXT("bump"), TEXT("bump"))
		.Node(TEXT("end"), TEXT("end")).Edge(TEXT("X"), TEXT("done"), TEXT("end"))
		.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray);
	Room.Flows.Add(TEXT(R"({"id":"out","name":"Out","isExit":true})"));
	F.Script(TEXT("exits"), Story(TEXT("room"), false));
	F.Script(TEXT("room"), Room);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	for (const bool bRollback : { false, true })
	{
		const FString Run = RunLabel(bRollback);
		F.Start(TEXT("exits"), bRollback);
		F.W.Component->AdvanceDialogue();
		F.W.Component->SelectOption(TEXT("leave"));
		TestEqual(Run + TEXT("an exit with no route stays in the called script"), F.State(), FString(TEXT("X log=[] done=0 errors=0")));
		F.W.Component->SelectOption(TEXT("scan"));
		TestEqual(Run + TEXT("an exit with no route inside a loop body stops the walk there"), F.State(), FString(TEXT("X log=[1] done=0 errors=0")));
		F.W.Component->SelectOption(TEXT("bump"));
		TestEqual(Run + TEXT("the loop the exit left open moves on with the next walk that runs out of edges"), F.State(), FString(TEXT("X log=[1,2] done=1 errors=0")));
		F.W.Component->SelectOption(TEXT("done"));
		TestEqual(Run + TEXT("End still returns through the default output"), F.State(), FString(TEXT("B log=[1,2] done=1 errors=0")));
	}
	return true;
}

// ============================================================================
// A local and a global that share an id
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowWalkSharedIdTest,
	"StoryFlow.Walk.NodeScopeFlagPicksBetweenLocalAndGlobalWithOneId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowWalkSharedIdTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// The exporter derives a variable id from the name alone, so the local Done and Log here carry the
	// ids of the globals with those names. Only the node's isGlobal flag says which one it means.
	// Line A, one option per node: Set Int(Done = 5) and Add To Array(Log, 9), each flagged global and unflagged,
	// and an unflagged Set Int(Last = 5) where only a global has that id: with no flag it means a local, and there is none.
	FFixture F;
	FScript Script;
	Script.Line(TEXT("A"), { TEXT("setGlobal"), TEXT("setLocal"), TEXT("addGlobal"), TEXT("addLocal"), TEXT("older") }).Edge(TEXT("0"), TEXT(""), TEXT("A"))
		.Node(TEXT("older"), TEXT("setInt"), TEXT(R"("variable":"var_Last","value":5)")).Edge(TEXT("A"), TEXT("older"), TEXT("older"))
		.Node(TEXT("setGlobal"), TEXT("setInt"), TEXT(R"("variable":"var_Done","value":5,"isGlobal":true)")).Edge(TEXT("A"), TEXT("setGlobal"), TEXT("setGlobal"))
		.Node(TEXT("setLocal"), TEXT("setInt"), TEXT(R"("variable":"var_Done","value":5)")).Edge(TEXT("A"), TEXT("setLocal"), TEXT("setLocal"))
		.Node(TEXT("globalLog"), TEXT("getIntArray"), TEXT(R"("variable":"var_Log","isGlobal":true)"))
		.Node(TEXT("localLog"), TEXT("getIntArray"), TEXT(R"("variable":"var_Log")"))
		.Node(TEXT("addGlobal"), TEXT("addToIntArray"), TEXT(R"("value":9)")).Edge(TEXT("A"), TEXT("addGlobal"), TEXT("addGlobal"))
		.Edge(TEXT("globalLog"), TEXT("integer-array-"), TEXT("addGlobal"), TEXT("integer-array-2"))
		.Node(TEXT("addLocal"), TEXT("addToIntArray"), TEXT(R"("value":9)")).Edge(TEXT("A"), TEXT("addLocal"), TEXT("addLocal"))
		.Edge(TEXT("localLog"), TEXT("integer-array-"), TEXT("addLocal"), TEXT("integer-array-2"))
		.Variable(TEXT("Done"), TEXT("integer"), TEXT("0"))
		.Variable(TEXT("Log"), TEXT("integer"), TEXT("[]"), IsArray);
	F.Script(TEXT("shared"), Script);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }
	UStoryFlowVariableAccumulator* Changed = NewObject<UStoryFlowVariableAccumulator>(F.W.Component);
	F.W.Component->OnVariableChanged.AddDynamic(Changed, &UStoryFlowVariableAccumulator::OnChanged);
	F.W.Component->bTraceEnabled = true;
	const auto Scopes = [&F, Changed](const TCHAR* Option)
	{
		F.Start(TEXT("shared"), false);
		Changed->Changes.Reset();
		FVarSetTrace Trace;
		F.W.Component->SelectOption(Option);
		GLog->Flush();
		Changed->Changes.Append(Trace.Lines);
		return FString::Printf(TEXT("global Done=%d Log=[%s], local Done=%d Log=[%s], changed %s"),
			F.Global(TEXT("Done")), *F.Array(TEXT("Log"), true), F.W.Component->GetIntVariable(TEXT("Done"), false), *F.Array(TEXT("Log"), false), *FString::Join(Changed->Changes, TEXT(" ")));
	};

	TestEqual(TEXT("Set Int flagged global writes the global and reports it"), Scopes(TEXT("setGlobal")), FString(TEXT("global Done=5 Log=[], local Done=0 Log=[], changed Done:global VAR SET \"Done\" global=true value=5")));
	TestEqual(TEXT("Set Int with no flag writes the local and reports it"), Scopes(TEXT("setLocal")), FString(TEXT("global Done=0 Log=[], local Done=5 Log=[], changed Done:local VAR SET \"Done\" global=false value=5")));
	TestEqual(TEXT("Add To Array on a global flagged array writes the global and reports it"), Scopes(TEXT("addGlobal")), FString(TEXT("global Done=0 Log=[9], local Done=0 Log=[], changed Log:global VAR SET \"Log\" global=true value=[array]")));
	TestEqual(TEXT("Add To Array on an unflagged array writes the local and reports it"), Scopes(TEXT("addLocal")), FString(TEXT("global Done=0 Log=[], local Done=0 Log=[9], changed Log:local VAR SET \"Log\" global=false value=[array]")));
	const FString Older = Scopes(TEXT("older"));
	TestEqual(TEXT("Set Int with no flag and no local of that id leaves the global alone"), Older + FString::Printf(TEXT(", Last=%d errors=%d"), F.Global(TEXT("Last")), F.Errors->Tags.Num()),
		FString(TEXT("global Done=0 Log=[], local Done=0 Log=[], changed , Last=0 errors=0")));
	return true;
}

// ============================================================================
// Run Script parameters with nothing wired into them
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowWalkUnwiredParametersTest,
	"StoryFlow.Walk.UnwiredRunScriptParametersArriveAsTheEditorPassesThem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowWalkUnwiredParametersTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// caller: Run Script(callee) with one parameter of every type and nothing wired into any of them.
	// callee: line In. Each parameter's variable is declared with a value of its own.
	FFixture F;
	FScript Caller;
	Caller.Node(TEXT("call"), TEXT("runScript"), TEXT(R"("script":"callee.json","scriptInterface":{"parameters":[)"
			R"({"id":"p1","name":"Flag","type":"boolean","isArray":false},{"id":"p2","name":"Count","type":"integer","isArray":false},)"
			R"({"id":"p3","name":"Ratio","type":"float","isArray":false},{"id":"p4","name":"Title","type":"string","isArray":false},)"
			R"({"id":"p5","name":"Mood","type":"enum","isArray":false},{"id":"p6","name":"Picture","type":"image","isArray":false},)"
			R"({"id":"p7","name":"Voice","type":"audio","isArray":false},{"id":"p8","name":"Who","type":"character","isArray":false},)"
			R"({"id":"p9","name":"Item","type":"dataAsset","isArray":false},{"id":"p10","name":"Numbers","type":"integer","isArray":true},)"
			R"({"id":"p11","name":"Names","type":"string","isArray":true},{"id":"p12","name":"Scores","type":"map","isArray":false,"keyType":"string","valueType":"integer"}]})"))
		.Edge(TEXT("0"), TEXT(""), TEXT("call"));
	FScript Callee;
	Callee.Line(TEXT("In")).Edge(TEXT("0"), TEXT(""), TEXT("In"))
		.Variable(TEXT("Flag"), TEXT("boolean"), TEXT("true"))
		.Variable(TEXT("Count"), TEXT("integer"), TEXT("7"))
		.Variable(TEXT("Ratio"), TEXT("float"), TEXT("1.5"))
		.Variable(TEXT("Title"), TEXT("string"), TEXT(R"("hello")"))
		.Variable(TEXT("Mood"), TEXT("enum"), TEXT(R"("happy")"), TEXT(R"("enumValues":["happy","sad"])"))
		.Variable(TEXT("Picture"), TEXT("image"), TEXT(R"("pic.png")"))
		.Variable(TEXT("Voice"), TEXT("audio"), TEXT(R"("hi.wav")"))
		.Variable(TEXT("Who"), TEXT("character"), TEXT(R"("hero")"))
		.Variable(TEXT("Item"), TEXT("dataAsset"), TEXT(R"("asset1")"))
		.Variable(TEXT("Numbers"), TEXT("integer"), TEXT("[1,2]"), IsArray)
		.Variable(TEXT("Names"), TEXT("string"), TEXT(R"(["a"])"), IsArray)
		.Variable(TEXT("Scores"), TEXT("map"), TEXT(R"([{"key":"a","value":1}])"), TEXT(R"("keyType":"string","valueType":"integer")"));
	F.Script(TEXT("caller"), Caller);
	F.Script(TEXT("callee"), Callee);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }

	F.Start(TEXT("caller"), false);
	UStoryFlowComponent* C = F.W.Component;
	TArray<FStoryFlowVariant> Keys, Values;
	C->GetMapVariable(TEXT("Scores"), Keys, Values, false);
	const FString Received = FString::Printf(TEXT("%s flag=%d count=%d ratio=%.1f title=%s mood=%s picture=%s voice=%s who=%s item=%s numbers=[%s] names=%d scores=%d errors=%d"),
		*F.Line(), C->GetBoolVariable(TEXT("Flag"), false) ? 1 : 0, C->GetIntVariable(TEXT("Count"), false), C->GetFloatVariable(TEXT("Ratio"), false),
		*C->GetStringVariable(TEXT("Title"), false), *C->GetStringVariable(TEXT("Mood"), false), *C->GetStringVariable(TEXT("Picture"), false),
		*C->GetStringVariable(TEXT("Voice"), false), *C->GetStringVariable(TEXT("Who"), false), *C->GetStringVariable(TEXT("Item"), false),
		*F.Array(TEXT("Numbers"), false), C->GetStringArrayVariable(TEXT("Names"), false).Num(), Keys.Num(), F.Errors->Tags.Num());
	// Every scalar arrives as its type's zero and every array empty. A map is the one type left as the called script declared it.
	TestEqual(TEXT("unwired parameters reach the called script as the editor passes them"), Received,
		FString(TEXT("In flag=0 count=0 ratio=0.0 title= mood= picture= voice= who= item= numbers=[] names=0 scores=1 errors=0")));
	return true;
}

// ============================================================================
// What a Run Script, a Run Flow and a Start with nothing after it report
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowWalkCallDiagnosticsTest,
	"StoryFlow.Walk.CallsReportWhatTheEditorReports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowWalkCallDiagnosticsTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLoopTests;
	// calls: line A, one option per case.
	//   nopath:  Run Script with no script selected
	//   noflow:  Run Flow with no flow selected
	//   missing: Run Script(nowhere), a script the project does not have
	//   adrift:  Run Script(adrift), a script whose Start has nothing connected
	//   astray:  Run Script(astray), a script whose Start is connected to a node it does not have
	//   noentry: Run Flow(Lost Road), a flow the script declares with no Entry Flow node
	//   unknown: Run Flow(ghost), a flow the script does not declare
	// adrift and astray are also started on their own.
	FFixture F;
	FScript Calls;
	Calls.Line(TEXT("A"), { TEXT("nopath"), TEXT("noflow"), TEXT("missing"), TEXT("adrift"), TEXT("astray"), TEXT("noentry"), TEXT("unknown") }).Edge(TEXT("0"), TEXT(""), TEXT("A"))
		.Node(TEXT("astray"), TEXT("runScript"), TEXT(R"("script":"astray.json")")).Edge(TEXT("A"), TEXT("astray"), TEXT("astray"))
		.Node(TEXT("noentry"), TEXT("runFlow"), TEXT(R"("flowId":"lost")")).Edge(TEXT("A"), TEXT("noentry"), TEXT("noentry"))
		.Node(TEXT("unknown"), TEXT("runFlow"), TEXT(R"("flowId":"ghost")")).Edge(TEXT("A"), TEXT("unknown"), TEXT("unknown"))
		.Node(TEXT("nopath"), TEXT("runScript")).Edge(TEXT("A"), TEXT("nopath"), TEXT("nopath"))
		.Node(TEXT("noflow"), TEXT("runFlow")).Edge(TEXT("A"), TEXT("noflow"), TEXT("noflow"))
		.Node(TEXT("missing"), TEXT("runScript"), TEXT(R"("script":"nowhere.json")")).Edge(TEXT("A"), TEXT("missing"), TEXT("missing"))
		.Node(TEXT("adrift"), TEXT("runScript"), TEXT(R"("script":"adrift.json")")).Edge(TEXT("A"), TEXT("adrift"), TEXT("adrift"));
	Calls.Flows.Add(TEXT(R"({"id":"lost","name":"Lost Road"})"));
	FScript Adrift;
	Adrift.Line(TEXT("Unreached"));
	FScript Astray;
	Astray.Edge(TEXT("0"), TEXT(""), TEXT("ghost"));
	F.Script(TEXT("astray"), Astray);
	F.Script(TEXT("calls"), Calls);
	F.Script(TEXT("adrift"), Adrift);
	if (!TestTrue(TEXT("fixture imported"), F.Import())) { return false; }
	// The log lines behind the three errors asserted below, spelled so that no assertion's own message matches
	AddExpectedError(TEXT("StoryFlow: Script not found"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("StoryFlow Error: Script not found"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("StoryFlow Error: Script's Start node is not connected"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("StoryFlow Error: Start node is not connected"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("StoryFlow Error: Script is missing a Start node"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("StoryFlow Error: Start node connects to missing node"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("StoryFlow Error: Flow "), EAutomationExpectedErrorFlags::Contains, 0);
	const auto Reported = [&F](const TCHAR* Option)
	{
		F.Start(TEXT("calls"), false);
		F.W.Component->SelectOption(Option);
		return FString::Printf(TEXT("[%s]"), *FString::Join(F.Errors->Tags, TEXT("|")));
	};

	TestEqual(TEXT("Run Script with no script selected reports no error"), Reported(TEXT("nopath")), FString(TEXT("[]")));
	TestEqual(TEXT("Run Flow with no flow selected reports no error"), Reported(TEXT("noflow")), FString(TEXT("[]")));
	TestEqual(TEXT("Run Script naming a script the project lacks reports it"), Reported(TEXT("missing")), FString(TEXT("[Script not found: nowhere]")));
	TestEqual(TEXT("Run Script into a script whose Start is unconnected reports it"), Reported(TEXT("adrift")), FString(TEXT("[Script's Start node is not connected]")));
	TestEqual(TEXT("Run Script into a script whose Start leads to a missing node reports it"), Reported(TEXT("astray")), FString(TEXT("[Script is missing a Start node]")));
	TestEqual(TEXT("Run Flow into a flow with no Entry Flow node names the flow"), Reported(TEXT("noentry")), FString(TEXT("[Flow \"Lost Road\" not found]")));
	TestEqual(TEXT("Run Flow into a flow the script does not declare names its id"), Reported(TEXT("unknown")), FString(TEXT("[Flow \"ghost\" not found]")));
	F.Start(TEXT("astray"), false);
	TestEqual(TEXT("starting a script whose Start leads to a missing node reports it"), FString::Printf(TEXT("[%s]"), *FString::Join(F.Errors->Tags, TEXT("|"))), FString(TEXT("[Start node connects to missing node]")));
	F.Start(TEXT("adrift"), false);
	TestEqual(TEXT("starting a script whose Start is unconnected reports it"), FString::Printf(TEXT("[%s]"), *FString::Join(F.Errors->Tags, TEXT("|"))), FString(TEXT("[Start node is not connected]")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
