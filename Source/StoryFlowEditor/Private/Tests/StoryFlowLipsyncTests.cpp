// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StoryFlowComponent.h"
#include "Data/StoryFlowHandles.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowTypes.h"
#include "GameFramework/Actor.h"
#include "Lipsync/StoryFlowLipsyncComponent.h"
#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "Lipsync/StoryFlowVisemeTable.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "UObject/Package.h"

#include "StoryFlowScopedWorld.h"

/**
 * The Tier 1 lipsync driver, and the component contract around it.
 *
 * Whether a mouth reads as speech is not testable — only eyes judge a face. What IS testable is everything a
 * wrong number breaks first: that loudness opens the mouth AT THE LEVEL AN ENGINE ACTUALLY PRODUCES, that
 * pitch moves it along the vowel axis, that silence closes it, that neither the easing nor the loudness
 * follower drifts with the frame rate, and that the identity morphs a Sidekick rig carries are never in the
 * driver's reach.
 *
 * The amplitudes here matter as much as the assertions. The first round of these tests fed 0.9 into three of
 * twenty-four bands, which is roughly eighty times what speech puts there, and every one of them passed
 * against a driver whose gate a real recording never opened.
 *
 * These mirror the Unity arm's tests case for case. The two arms implement one normative spec
 * (LIPSYNC_DESIGN.md, "v2"), so a change that passes here and fails there means the arms have drifted.
 */

namespace StoryFlowLipsyncTestHelpers
{
	/**
	 * Magnitudes for the component's evenly spaced band, with the energy concentrated at one position.
	 *
	 * The default amplitude is 0.02, not 0.9: with the driver reading a NORMALISED spectrum (FullScale 1)
	 * that is where a voiced band sits, and a tone test above the noise floor of a driver nothing can reach
	 * proves only that the arithmetic runs.
	 */
	TArray<float> BandsPeakingAt(float Position, float Amplitude = 0.02f, int32 Count = 24)
	{
		TArray<float> Magnitudes;
		Magnitudes.Init(0.0f, Count);
		const int32 Centre = FMath::Clamp(FMath::RoundToInt(Position * (Count - 1)), 0, Count - 1);
		for (int32 Index = FMath::Max(0, Centre - 1); Index <= FMath::Min(Count - 1, Centre + 1); ++Index)
		{
			Magnitudes[Index] = Amplitude;
		}
		return Magnitudes;
	}

	/** Run enough frames that the easing has settled — one frame proves nothing about a smoothed value. */
	void Settle(FStoryFlowLipsyncDriver& Driver, const TArray<float>& Magnitudes, int32 Frames = 120)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Driver.AdvanceFromMagnitudes(Magnitudes, 1.0f / 60.0f);
		}
	}

	float WeightOf(const FStoryFlowLipsyncDriver& Driver, FName Morph)
	{
		const float* Found = Driver.Current().Find(Morph);
		return Found != nullptr ? *Found : 0.0f;
	}

	/**
	 * What a voiced syllable actually looks like coming out of an FFT: LINEAR magnitudes, a mean around
	 * 0.005, three bins near 0.02 down where the first formant lives, and a tail that has fallen away to
	 * nothing by 4 kHz. This is the input the engines really produce.
	 *
	 * Scale it by the engine's full-scale magnitude to get the same signal on an unnormalised analyser:
	 * 5.66 puts the loud bins at 0.07-0.12, which is where Unreal's mixer reports speech.
	 */
	TArray<float> RealisticSpeechBands(float Scale = 1.0f)
	{
		static const float Speech[24] = {
			0.012f, 0.022f, 0.020f, 0.016f, 0.011f, 0.0072f, 0.0046f, 0.0029f,
			0.0018f, 0.0011f, 0.0007f, 0.00043f, 0.00027f, 0.00017f, 0.00010f, 6.4e-5f,
			4.0e-5f, 2.5e-5f, 1.5e-5f, 9.6e-6f, 6.0e-6f, 3.7e-6f, 2.3e-6f, 1.4e-6f
		};

		TArray<float> Magnitudes;
		Magnitudes.Reserve(UE_ARRAY_COUNT(Speech));
		for (const float Magnitude : Speech)
		{
			Magnitudes.Add(Magnitude * Scale);
		}
		return Magnitudes;
	}

	/** A table with only the vowel axis and rest — the shape a rig-specific mapping asset usually has. */
	StoryFlowVisemeTable::FTable AxisOnlyTable()
	{
		StoryFlowVisemeTable::FTable Custom;
		Custom.Add(TEXT("rest"), StoryFlowVisemeTable::FPose());
		for (const FName Pose : StoryFlowVisemeTable::Axis())
		{
			Custom.Add(Pose, StoryFlowVisemeTable::FPose{ { TEXT("jawOpen"), 0.6f }, { Pose, 1.0f } });
		}
		return Custom;
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncRealisticSpectrumTest,
	"StoryFlow.Lipsync.RealisticSpeechOpensTheJaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncRealisticSpectrumTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// THE headline pin. Every other test here can pass while the mouth never moves in a game, because a tone
	// test picks its own amplitude and the driver's constants were tuned on a scale no engine produces. This
	// one feeds what the engine feeds and demands a mouth.
	FStoryFlowLipsyncDriver Normalised(StoryFlowVisemeTable::Default());
	const TArray<float> Speech = RealisticSpeechBands();
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Normalised.AdvanceFromMagnitudes(Speech, 1.0f / 60.0f);
	}
	const float NormalisedJaw = WeightOf(Normalised, TEXT("jawOpen"));
	AddInfo(FString::Printf(TEXT("FullScale 1, mean 0.0042: jawOpen %.3f, Level %.3f"), NormalisedJaw, Normalised.Level()));
	TestTrue(FString::Printf(TEXT("a second of real speech opens the jaw (jawOpen %.3f)"), NormalisedJaw), NormalisedJaw > 0.3f);

	// The same signal as Unreal's mixer reports it: not normalised, so the driver has to be told what full
	// scale is. Nothing about the mouth may change — that is the entire point of FullScale existing.
	FStoryFlowLipsyncDriver Mixer(StoryFlowVisemeTable::Default());
	Mixer.FullScale = 5.66f;
	const TArray<float> MixerSpeech = RealisticSpeechBands(5.66f);
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		Mixer.AdvanceFromMagnitudes(MixerSpeech, 1.0f / 60.0f);
	}
	const float MixerJaw = WeightOf(Mixer, TEXT("jawOpen"));
	AddInfo(FString::Printf(TEXT("FullScale 5.66, raw peak %.3f: jawOpen %.3f"), Mixer.RawPeak(), MixerJaw));
	TestTrue(FString::Printf(TEXT("mixer-scale speech opens the jaw (jawOpen %.3f)"), MixerJaw), MixerJaw > 0.3f);
	TestEqual(TEXT("FullScale cancels the engine's scaling exactly"), MixerJaw, NormalisedJaw, 0.01f);

	// The calibration instrument: RawPeak reports the magnitude BEFORE the transform, so a smoke can read it
	// off a loud line and type it into AnalysisFullScale.
	TestEqual(TEXT("RawPeak reports the raw magnitude, not the transformed one"), Mixer.RawPeak(), 0.022f * 5.66f, 0.001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLoudOpensAndSilenceClosesTest,
	"StoryFlow.Lipsync.LoudAudioOpensTheJawAndSilenceClosesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncLoudOpensAndSilenceClosesTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
	Settle(Driver, BandsPeakingAt(0.25f));
	// Settles near 0.46; anything under 0.3 means a constant is off by more than the eye would forgive.
	TestTrue(TEXT("loud audio opens the jaw"), WeightOf(Driver, TEXT("jawOpen")) > 0.3f);

	// One bad sample must not poison the line: a NaN in the smoothing state or the peak would turn every
	// weight after it into NaN until the next ResetLevel.
	TArray<float> Poisoned = BandsPeakingAt(0.25f);
	Poisoned[3] = NAN;
	Driver.AdvanceFromMagnitudes(Poisoned, 1.0f / 60.0f);
	Settle(Driver, BandsPeakingAt(0.25f), 30);
	const float AfterNaN = WeightOf(Driver, TEXT("jawOpen"));
	TestTrue(FString::Printf(TEXT("a NaN sample reads as silence and the mouth recovers (jawOpen %.3f)"), AfterNaN),
		FMath::IsFinite(AfterNaN) && AfterNaN > 0.3f);

	// Closing matters as much as opening: a mouth that freezes mid-vowel when a line ends is worse than one
	// that never moved at all.
	for (int32 Frame = 0; Frame < 240; ++Frame)
	{
		Driver.AdvanceSilent(1.0f / 60.0f);
	}
	TestTrue(TEXT("silence closes the jaw"), WeightOf(Driver, TEXT("jawOpen")) < 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncCentroidAxisTest,
	"StoryFlow.Lipsync.CentroidMovesAlongTheVowelAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncCentroidAxisTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// Low frequencies sit at the OO end of the axis (pucker), high at the EE end (stretch). This is the
	// difference between a mouth that shapes vowels and one that only flaps open and shut.
	FStoryFlowLipsyncDriver Low(StoryFlowVisemeTable::Default());
	Settle(Low, BandsPeakingAt(0.05f));

	FStoryFlowLipsyncDriver High(StoryFlowVisemeTable::Default());
	Settle(High, BandsPeakingAt(0.95f));

	TestTrue(TEXT("low audio puckers more"), WeightOf(Low, TEXT("mouthPucker")) > WeightOf(High, TEXT("mouthPucker")));
	TestTrue(TEXT("high audio stretches more"), WeightOf(High, TEXT("mouthStretchLeft")) > WeightOf(Low, TEXT("mouthStretchLeft")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncIdentityMorphTest,
	"StoryFlow.Lipsync.NeverOwnsAnIdentityMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncIdentityMorphTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// A Sidekick character's body type is a morph like any other, and in UNREAL the part meshes really do
	// carry them (unlike Unity's baked mesh). A driver that reset every weight it found would silently
	// flatten the character back to the base body. The owned set is what prevents it.
	const TSet<FName> Owned = StoryFlowVisemeTable::OwnedMorphs(StoryFlowVisemeTable::Default());
	for (const FName Identity : { FName(TEXT("defaultBuff")), FName(TEXT("defaultSkinny")),
								  FName(TEXT("defaultHeavy")), FName(TEXT("masculineFeminine")) })
	{
		TestFalse(FString::Printf(TEXT("the driver never owns %s"), *Identity.ToString()), Owned.Contains(Identity));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncFrameRateTest,
	"StoryFlow.Lipsync.EasingIsFrameRateIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncFrameRateTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// The same two seconds of audio must reach the same mouth whether they arrive in 60 frames or 240, and
	// that means BOTH smoothed quantities: the ease and the loudness follower.
	//
	// A loud half second then a quiet second and a half, because a steady signal hides the follower entirely
	// — the peak simply tracks the energy and `Amp` saturates, which is why the first version of this test
	// passed against a decay that was running four times too fast at 120 fps.
	const TArray<float> Loud = RealisticSpeechBands();
	TArray<float> Quiet;
	for (const float Magnitude : Loud)
	{
		Quiet.Add(Magnitude * 0.1f);
	}

	auto RunAt = [&Loud, &Quiet](int32 Fps) -> FStoryFlowLipsyncDriver
	{
		FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
		const float Delta = 1.0f / Fps;
		for (int32 Frame = 0; Frame < Fps / 2; ++Frame)
		{
			Driver.AdvanceFromMagnitudes(Loud, Delta);
		}
		for (int32 Frame = 0; Frame < (Fps * 3) / 2; ++Frame)
		{
			Driver.AdvanceFromMagnitudes(Quiet, Delta);
		}
		return Driver;
	};

	const FStoryFlowLipsyncDriver Slow = RunAt(30);
	const FStoryFlowLipsyncDriver Fast = RunAt(120);

	const float SlowJaw = WeightOf(Slow, TEXT("jawOpen"));
	const float FastJaw = WeightOf(Fast, TEXT("jawOpen"));
	AddInfo(FString::Printf(TEXT("30 fps jawOpen %.4f, 120 fps jawOpen %.4f"), SlowJaw, FastJaw));

	// Below saturation, or the follower's error would be clamped away before it reached the mouth.
	TestTrue(TEXT("the test amplitude is below saturation"), Slow.Level() < 0.87f && Slow.Level() > 0.1f);
	TestEqual(TEXT("the same two seconds reach the same mouth at 30 and 120 fps"), SlowJaw, FastJaw, 0.02f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncIdleMouthTest,
	"StoryFlow.Lipsync.IdleMouthMoves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncIdleMouthTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// A line whose audio cannot be analysed still has to look alive, or a subtitled game gets a wall of
	// dead faces.
	FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
	bool bMoved = false;
	for (int32 Frame = 0; Frame < 240; ++Frame)
	{
		Driver.AdvanceIdle(1.0f / 60.0f);
		if (WeightOf(Driver, TEXT("jawOpen")) > 0.02f)
		{
			bMoved = true;
		}
	}
	TestTrue(TEXT("the idle mouth moves"), bMoved);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncCustomMapTest,
	"StoryFlow.Lipsync.CustomMapDrivesOnlyItsOwnMorphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncCustomMapTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// A rig-specific map replaces the table wholesale: it must drive its own names, and must not carry the
	// built-in ones along behind it onto a rig that has never heard of them.
	StoryFlowVisemeTable::FTable Custom;
	Custom.Add(TEXT("rest"), StoryFlowVisemeTable::FPose());
	for (const FName Pose : StoryFlowVisemeTable::Axis())
	{
		Custom.Add(Pose, StoryFlowVisemeTable::FPose{ { TEXT("customShape"), 1.0f } });
	}

	FStoryFlowLipsyncDriver Driver(Custom);
	Settle(Driver, BandsPeakingAt(0.25f));
	TestTrue(TEXT("a custom map drives its own morph names"), WeightOf(Driver, TEXT("customShape")) > 0.05f);
	TestFalse(TEXT("a custom map drives nothing else"), Driver.Current().Contains(TEXT("jawOpen")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncCustomIdlePoolTest,
	"StoryFlow.Lipsync.IdleUsesTheActiveTablesPoses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncCustomIdlePoolTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// The idle pool used to be the BUILT-IN pose names, so a four-pose map spent most of its picks reaching
	// for IH, FF, TH and L — names its table has never heard of — and answered an empty pose each time. The
	// visible result is a face that twitches once every second or so and looks broken rather than idle.
	FStoryFlowLipsyncDriver Driver(AxisOnlyTable());

	// Twenty seconds, not five: the pose picks are random, and a short run's variance is wide enough to
	// straddle any threshold that also tells the two behaviours apart.
	constexpr int32 Frames = 1200;
	int32 FramesWithAPose = 0;
	float HighWater = 0.0f;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		Driver.AdvanceIdle(1.0f / 60.0f);

		float TargetSum = 0.0f;
		for (const TPair<FName, float>& Morph : Driver.Target())
		{
			TargetSum += Morph.Value;
		}
		if (TargetSum > 0.0f)
		{
			++FramesWithAPose;
		}
		HighWater = FMath::Max(HighWater, WeightOf(Driver, TEXT("jawOpen")));
	}

	AddInfo(FString::Printf(TEXT("a pose was held on %d of %d idle frames, jawOpen reached %.3f"), FramesWithAPose, Frames, HighWater));

	// Four fifths of the picks are poses and one fifth are gaps, so roughly three quarters of the frames
	// hold something. The old pool reached for a name the table lacks five times in nine and answered an
	// empty pose each time, which lands near a third.
	TestTrue(FString::Printf(TEXT("the idle mouth holds one of its own poses most of the time (%d/%d)"), FramesWithAPose, Frames),
		FramesWithAPose > (Frames * 3) / 5);
	TestTrue(TEXT("the idle mouth actually moves"), HighWater > 0.05f);

	// And only its own: nothing from the built-in table leaked into the driven set.
	TestEqual(TEXT("the driver owns exactly the custom table's morphs"), Driver.Current().Num(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncClosingBreathTest,
	"StoryFlow.Lipsync.ClosingBreathOnlyWhenTheTableOwnsMouthClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncClosingBreathTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// Silence: the gate is shut, which is exactly when the closing breath fires.
	TArray<float> Silence;
	Silence.Init(0.0f, 24);

	FStoryFlowLipsyncDriver WithClose(StoryFlowVisemeTable::Default());
	WithClose.AdvanceFromMagnitudes(Silence, 1.0f / 60.0f);
	const float* Closed = WithClose.Target().Find(TEXT("mouthClose"));
	TestTrue(TEXT("the built-in table gets its closing breath"), Closed != nullptr && *Closed > 0.0f);

	// A map with no MM pose owns no mouthClose. Writing the key anyway is a step that silently does nothing
	// while a float in the target map climbs forever behind it.
	FStoryFlowLipsyncDriver WithoutClose(AxisOnlyTable());
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		WithoutClose.AdvanceFromMagnitudes(Silence, 1.0f / 60.0f);
	}
	TestFalse(TEXT("a table without mouthClose never gains a mouthClose target"), WithoutClose.Target().Contains(TEXT("mouthClose")));
	TestFalse(TEXT("and never drives one"), WithoutClose.Current().Contains(TEXT("mouthClose")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncJawBiasClampTest,
	"StoryFlow.Lipsync.JawBiasNeverExceedsOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncJawBiasClampTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// AA drives jawOpen to 0.85. At full Strength and JawBias 2 that asks for 1.7, and a morph weight past 1
	// extrapolates the shape into a face nobody sculpted.
	FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
	Driver.Strength = 1.0f;
	Driver.JawBias = 2.0f;

	const TArray<float> Speech = RealisticSpeechBands();
	float HighWater = 0.0f;
	for (int32 Frame = 0; Frame < 300; ++Frame)
	{
		Driver.AdvanceFromMagnitudes(Speech, 1.0f / 60.0f);
		HighWater = FMath::Max(HighWater, WeightOf(Driver, TEXT("jawOpen")));
	}

	AddInfo(FString::Printf(TEXT("JawBias 2 at full Strength reached jawOpen %.4f"), HighWater));
	TestTrue(TEXT("the clamp is doing work (the unclamped target is above 1)"), HighWater > 0.9f);
	TestTrue(FString::Printf(TEXT("jawOpen never exceeds 1 (%.4f)"), HighWater), HighWater <= 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncZeroDeltaTest,
	"StoryFlow.Lipsync.ZeroDeltaHoldsTheMouth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncZeroDeltaTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// A paused game still has an audio clock, and a frame with no time in it is not a reason to jump. The
	// old rule set the ease constant to 1 on dt <= 0, which SNAPPED every weight to its raw target: maximum
	// jitter, on the one frame nothing is moving.
	FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
	Settle(Driver, RealisticSpeechBands());
	const TMap<FName, float> Held = Driver.Current();

	Driver.AdvanceFromMagnitudes(BandsPeakingAt(0.95f), 0.0f);
	Driver.AdvanceIdle(0.0f);
	Driver.AdvanceSilent(0.0f);

	for (const TPair<FName, float>& Morph : Held)
	{
		TestEqual(FString::Printf(TEXT("%s held exactly through three zero-delta frames"), *Morph.Key.ToString()),
			WeightOf(Driver, Morph.Key), Morph.Value);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLevelTest,
	"StoryFlow.Lipsync.LevelReportsOnlyAnalysedLoudness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncLevelTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// Level is the smoke's instrument for the whole of the calibration question, so it has to mean one thing:
	// how loud the analysed audio is. An idle mouth is moving on a coin flip and a silent one on nothing, and
	// a meter that kept reporting the last analysed value through either is lying at the moment it matters.
	FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
	Settle(Driver, RealisticSpeechBands());
	TestTrue(TEXT("analysed audio reads a level"), Driver.Level() > 0.0f);

	Driver.AdvanceIdle(1.0f / 60.0f);
	TestEqual(TEXT("the idle mouth reports no level"), Driver.Level(), 0.0f);

	Settle(Driver, RealisticSpeechBands());
	Driver.AdvanceSilent(1.0f / 60.0f);
	TestEqual(TEXT("silence reports no level"), Driver.Level(), 0.0f);
	return true;
}

// ============================================================================
// The component contract
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncSpeakerPathTest,
	"StoryFlow.Lipsync.SpeakerPathsMatchThroughNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncSpeakerPathTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// The two sides are built by different code and disagree about shape. GetCharacterPathById answers the
	// bridge's normalized key; a node whose character id fails to resolve falls back to the AUTHORED path,
	// verbatim, forward slashes and mixed case and all. Same character, two spellings, a face that never
	// moves and nothing anywhere saying why.
	TestTrue(TEXT("slashes do not decide who is speaking"),
		UStoryFlowLipsyncComponent::SpeakerPathsMatch(TEXT("Characters/Hero.json"), TEXT("characters\\hero.json")));
	TestTrue(TEXT("case does not decide who is speaking"),
		UStoryFlowLipsyncComponent::SpeakerPathsMatch(TEXT("Characters\\Hero.json"), TEXT("characters\\hero.json")));
	TestTrue(TEXT("identical paths match"),
		UStoryFlowLipsyncComponent::SpeakerPathsMatch(TEXT("characters\\hero.json"), TEXT("characters\\hero.json")));

	TestFalse(TEXT("different characters do not match"),
		UStoryFlowLipsyncComponent::SpeakerPathsMatch(TEXT("characters\\hero.json"), TEXT("characters\\villain.json")));
	TestFalse(TEXT("a narrator line (no speaker) matches nobody"),
		UStoryFlowLipsyncComponent::SpeakerPathsMatch(TEXT("characters\\hero.json"), TEXT("")));
	TestFalse(TEXT("an unresolved id matches nobody"),
		UStoryFlowLipsyncComponent::SpeakerPathsMatch(TEXT(""), TEXT("characters\\hero.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncAudioFollowTest,
	"StoryFlow.Lipsync.DialogueAudioPlayingIsFalseWithoutAudio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncAudioFollowTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// The audio-follow rule's whole cue. A line with no sound must read as not sounding, or a face keeps
	// mouthing the music for as long as the text sits on screen.
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("fixture initialized"), W.Init())) { return false; }

	TestFalse(TEXT("nothing is playing before a line starts"), W.Component->IsDialogueAudioPlaying());
	TestNull(TEXT("and there is no audio component to follow"), W.Component->GetCurrentDialogueAudio());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncRerenderTest,
	"StoryFlow.Lipsync.ARerenderDoesNotStartTheLineAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncRerenderTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// The same node comes round again for reasons that have nothing to do with speech: a variable changed
	// and the text was re-interpolated, the dialogue resumed, a dead-end branch re-broadcast the line it is
	// still sitting on. Each one used to re-arm the loudness history, and a game writing a variable per
	// frame pinned the peak to its initial value and held the mouth wide open for the whole line.
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("fixture initialized"), W.Init())) { return false; }

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	for (const TCHAR* Id : { TEXT("1"), TEXT("2") })
	{
		FStoryFlowNode Line = MakeNode(Id, EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
		Line.Data.Text = TEXT("a line");
		Script->Nodes.Add(Line.Id, Line);
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
	Project->Scripts.Add(TEXT("lipsync"), Script);
	W.Subsystem->SetProject(Project);

	// No CharacterId, so every line is this face's, and no mesh, so nothing is written anywhere.
	AActor* Owner = W.Component->GetOwner();
	UStoryFlowLipsyncComponent* Lipsync = NewObject<UStoryFlowLipsyncComponent>(Owner);
	Lipsync->Source = W.Component;
	Lipsync->RegisterComponent();
	Owner->DispatchBeginPlay();

	W.Component->StartDialogueWithScript(TEXT("lipsync"));
	TestEqual(TEXT("entering a line starts it"), Lipsync->GetLineStarts(), 1);

	// ResumeDialogue re-broadcasts the state it is already showing — the cheapest of the re-render paths.
	W.Component->PauseDialogue();
	W.Component->ResumeDialogue();
	TestEqual(TEXT("a re-render of the same node does not start the line again"), Lipsync->GetLineStarts(), 1);

	W.Component->PauseDialogue();
	W.Component->ResumeDialogue();
	TestEqual(TEXT("nor does a second one"), Lipsync->GetLineStarts(), 1);

	// A DIFFERENT node is a different line, and must arm the mouth again.
	W.Component->AdvanceDialogue();
	TestEqual(TEXT("the next line does start"), Lipsync->GetLineStarts(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncManualSurvivesDialogueTest,
	"StoryFlow.Lipsync.GameCodeLipsyncSurvivesOtherSpeakers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncManualSurvivesDialogueTest::RunTest(const FString& Parameters)
{
	using namespace StoryFlowLipsyncTestHelpers;

	// StartLipsync promises to run until StopLipsync: a bark, a radio, a cutscene line the game plays
	// itself. Other characters talking, and the dialogue ending, are not StopLipsync — and both used to
	// cancel it mid-word, because the "not my line" and "dialogue over" paths cleared everything.
	StoryFlowTestWorld::FScopedWorld W;
	if (!TestTrue(TEXT("fixture initialized"), W.Init())) { return false; }

	UStoryFlowScriptAsset* Script = NewObject<UStoryFlowScriptAsset>(GetTransientPackage());
	Script->StartNode = TEXT("0");
	Script->Nodes.Add(TEXT("0"), MakeNode(TEXT("0"), EStoryFlowNodeType::Start, TEXT("start")));
	FStoryFlowNode Line = MakeNode(TEXT("1"), EStoryFlowNodeType::Dialogue, TEXT("dialogue"));
	Line.Data.Text = TEXT("someone else's line");
	Script->Nodes.Add(Line.Id, Line);
	Script->Nodes.Add(TEXT("2"), MakeNode(TEXT("2"), EStoryFlowNodeType::End, TEXT("end")));
	Script->Connections.Add(MakeEdge(TEXT("e1"), TEXT("0"), TEXT("1"),
		StoryFlowHandles::Source(TEXT("0")), StoryFlowHandles::Target(TEXT("1"))));
	Script->Connections.Add(MakeEdge(TEXT("e2"), TEXT("1"), TEXT("2"),
		StoryFlowHandles::Source(TEXT("1")), StoryFlowHandles::Target(TEXT("2"))));
	Script->BuildConnectionIndices();

	UStoryFlowProjectAsset* Project = NewObject<UStoryFlowProjectAsset>(GetTransientPackage());
	Project->Scripts.Add(TEXT("other"), Script);
	W.Subsystem->SetProject(Project);

	// A character id the project does not have: every line is somebody else's.
	AActor* Owner = W.Component->GetOwner();
	UStoryFlowLipsyncComponent* Lipsync = NewObject<UStoryFlowLipsyncComponent>(Owner);
	Lipsync->Source = W.Component;
	Lipsync->CharacterId = TEXT("da_nobody_in_this_project");
	Lipsync->RegisterComponent();
	Owner->DispatchBeginPlay();

	TestFalse(TEXT("nothing is driving the face yet"), Lipsync->IsLipsyncActive());
	Lipsync->StartLipsync();
	TestTrue(TEXT("game code started it"), Lipsync->IsLipsyncActive());

	AddExpectedError(TEXT("which this project has no character for"), EAutomationExpectedErrorFlags::Contains, 1);
	W.Component->StartDialogueWithScript(TEXT("other"));
	TestTrue(TEXT("another speaker's line does not cancel it"), Lipsync->IsLipsyncActive());
	TestEqual(TEXT("and it is not a line of this face's"), Lipsync->GetLineStarts(), 0);

	W.Component->StopDialogue();
	TestTrue(TEXT("nor does the dialogue ending"), Lipsync->IsLipsyncActive());

	Lipsync->StopLipsync();
	TestFalse(TEXT("StopLipsync is what ends it"), Lipsync->IsLipsyncActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncBroadbandCalibrationTest,
    "StoryFlow.Lipsync.BroadbandCalibrationSeparatesVowels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowLipsyncBroadbandCalibrationTest::RunTest(const FString& Parameters)
{
    using namespace StoryFlowLipsyncTestHelpers;
    // Two 24-band mixer snapshots from different syllables in the demo recording.
    // Both saturated the original centroid at EE, despite different spectral balance.
    const TArray<float> Rounded = {0.225459f, 0.038149f, 0.065496f, 0.004047f, 0.003854f, 0.006124f, 0.005453f, 0.005577f, 0.004852f, 0.011677f, 0.024018f, 0.004392f, 0.004730f, 0.005595f, 0.003086f, 0.002760f, 0.006922f, 0.003290f, 0.001756f, 0.001987f, 0.000508f, 0.001501f, 0.002072f, 0.002075f};
    const TArray<float> Spread = {5.937835f, 3.068410f, 2.017326f, 10.631539f, 1.097391f, 0.214983f, 0.165242f, 0.170387f, 0.159038f, 1.025327f, 2.729825f, 0.415630f, 0.325098f, 0.364233f, 4.312669f, 0.924465f, 0.259884f, 0.122447f, 0.279529f, 0.074249f, 0.218343f, 0.383125f, 0.186893f, 0.058610f};
    // Isolate spectrum calibration from a particular rig's stretch limits.
    const StoryFlowVisemeTable::FTable CalibrationTable = {
        {TEXT("OO"), {{TEXT("jawOpen"), .2f}, {TEXT("mouthPucker"), .8f}}},
        {TEXT("OH"), {{TEXT("jawOpen"), .6f}, {TEXT("mouthPucker"), .5f}}},
        {TEXT("AA"), {{TEXT("jawOpen"), .85f}}},
        {TEXT("EE"), {{TEXT("jawOpen"), .28f}, {TEXT("mouthStretchLeft"), .8f}}}
    };
    FStoryFlowLipsyncDriver Low(CalibrationTable);
    FStoryFlowLipsyncDriver High(CalibrationTable);
    for (auto* Driver : {&Low, &High})
    {
        Driver->FullScale = 32.f;
        Driver->VowelScale = 4.f;
        Driver->VowelOffset = -1.1f;
        Driver->SpectralContrast = 2.f;
    }
    Settle(Low, Rounded);
    Settle(High, Spread);
    TestTrue(TEXT("Rounded speech no longer pins the wide vowel"), Low.Centroid() < .35f);
    TestTrue(TEXT("Bright speech still reaches the open/spread end"), High.Centroid() > .65f && High.Centroid() < .95f);
    TestTrue(TEXT("The actual mouth rounds on the low syllable"), WeightOf(Low, TEXT("mouthPucker")) > WeightOf(High, TEXT("mouthPucker")) + .1f);
    TestTrue(TEXT("The actual mouth spreads on the bright syllable"), WeightOf(High, TEXT("mouthStretchLeft")) > WeightOf(Low, TEXT("mouthStretchLeft")) + .03f);
    Settle(Low, TArray<float>{}, 60);
    TestTrue(TEXT("Calibration still returns the jaw to rest"), WeightOf(Low, TEXT("jawOpen")) < .001f);
    TestTrue(TEXT("Calibration releases round lips at rest"), WeightOf(Low, TEXT("mouthPucker")) < .001f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowArticulationTest, "StoryFlow.Lipsync.SpectralArticulationSeparatesSounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStoryFlowArticulationTest::RunTest(const FString& Parameters)
{
 // Median normalized spectra from the comparison voice's estimated sound intervals.
 // No recording times or phoneme labels are supplied to the driver.
 const TArray<float> AA = {4.5638395f, 3.3948797f, 6.2837417f, 8.0000000f, 3.8874296f, 1.6700996f, 1.9254578f, 0.5760763f, 0.2302174f, 0.1066827f, 0.1166886f, 0.0938568f, 0.2558874f, 0.4739501f, 0.6472464f, 0.3766224f, 0.2991101f, 0.2205013f, 0.2532450f, 0.1732813f, 0.1459124f, 0.0846823f, 0.1716618f, 0.4784366f};
 const TArray<float> UW = {2.9408187f, 8.0000000f, 3.6435724f, 0.5051192f, 0.2034790f, 0.0858681f, 0.1604327f, 0.3337183f, 0.3196717f, 0.2147750f, 0.5994444f, 0.3105703f, 0.7424886f, 0.1371866f, 0.0533936f, 0.0839217f, 0.1155893f, 0.0643006f, 0.1023510f, 0.0338608f, 0.0797036f, 0.0736186f, 0.0238007f, 0.0224656f};
 const TArray<float> IY = {3.1999630f, 8.0000000f, 3.8742417f, 0.7173810f, 0.1962019f, 0.1253982f, 0.0693804f, 0.0584704f, 0.0861837f, 0.2057375f, 0.4408576f, 0.5205051f, 1.1689148f, 0.5477177f, 0.3212263f, 0.1219156f, 0.0938451f, 0.1095114f, 0.1745078f, 0.1540079f, 0.0825296f, 0.0532116f, 0.0613775f, 0.0704049f};
 const TArray<float> M = {5.6308946f, 8.0000000f, 4.4280608f, 1.2038556f, 0.8340944f, 1.0155546f, 0.5861614f, 0.2316687f, 0.1009630f, 0.1744277f, 0.2570279f, 0.2765488f, 0.5936318f, 0.2198207f, 0.0616570f, 0.0615312f, 0.0525952f, 0.0629775f, 0.0929129f, 0.0390245f, 0.0280979f, 0.0304940f, 0.0154473f, 0.0432758f};
 const TArray<float> S = {2.7210916f, 1.6577262f, 2.0424855f, 1.0707995f, 0.7632159f, 0.8244199f, 0.4780119f, 0.4176268f, 1.1365432f, 1.6889101f, 1.1313594f, 0.9467233f, 1.3474261f, 1.6206611f, 1.0042480f, 0.7454493f, 1.3812182f, 1.3656784f, 2.9090891f, 1.7947428f, 1.5337434f, 2.4566829f, 3.6452522f, 7.9999999f};
 StoryFlowVisemeTable::FTable Table;
 Table.Add(TEXT("rest"), {});
 Table.Add(TEXT("AA"), {{TEXT("jawOpen"), .8f}});
 Table.Add(TEXT("OH"), {{TEXT("jawOpen"), .65f}, {TEXT("mouthPucker"), .5f}});
 Table.Add(TEXT("OO"), {{TEXT("jawOpen"), .25f}, {TEXT("mouthPucker"), .9f}});
 Table.Add(TEXT("EE"), {{TEXT("jawOpen"), .25f}, {TEXT("mouthStretchLeft"), .5f}});
 Table.Add(TEXT("SS"), {{TEXT("jawOpen"), .12f}, {TEXT("mouthStretchLeft"), .3f}});
 Table.Add(TEXT("MM"), {{TEXT("jawOpen"), .1f}, {TEXT("mouthClose"), 1.f}});
 auto Pose = [&](const TArray<float>& Spectrum) {
  FStoryFlowLipsyncDriver Driver(Table);
  Driver.bSpectralArticulation = true;
  Driver.FullScale = 32.f;
  for (int32 I = 0; I < 60; ++I) Driver.AdvanceFromMagnitudes(Spectrum, 1.f/60.f);
  return Driver.Current();
 };
 const auto Open = Pose(AA), Sibilant = Pose(S), Round = Pose(UW), Wide = Pose(IY), Closed = Pose(M);
 TestTrue(TEXT("A sibilant must not open the jaw like an open vowel"), Open.FindRef(TEXT("jawOpen")) > Sibilant.FindRef(TEXT("jawOpen")) + .08f);
 TestTrue(TEXT("Rounded and wide vowels with similar low peaks need different lips"), Round.FindRef(TEXT("mouthPucker")) > Wide.FindRef(TEXT("mouthPucker")) + .04f);
 TestTrue(TEXT("Low nasal energy can bring lips together during speech"), Closed.FindRef(TEXT("mouthClose")) > Open.FindRef(TEXT("mouthClose")) + .02f);
 const auto Silence = Pose(TArray<float>({0.f,0.f,0.f,0.f}));
 for (const auto& Value : Silence) TestEqual(TEXT("Silence releases lips instead of clenching"), Value.Value, 0.f);
 FStoryFlowLipsyncDriver Legacy(Table), Blended(Table);
 Legacy.FullScale = Blended.FullScale = 32.f;
 Blended.bSpectralArticulation = true;
 Blended.ArticulationBlend = 0.f;
 for (int32 I = 0; I < 60; ++I) { Legacy.AdvanceFromMagnitudes(AA, 1.f/60.f); Blended.AdvanceFromMagnitudes(AA, 1.f/60.f); }
 TestEqual(TEXT("Zero articulation blend preserves the original speaking pose"), Blended.Current().FindRef(TEXT("jawOpen")), Legacy.Current().FindRef(TEXT("jawOpen")), .0001f);
 return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
