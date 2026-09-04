// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "Lipsync/StoryFlowVisemeTable.h"

/**
 * The Tier 1 lipsync driver.
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
	TestTrue(TEXT("loud audio opens the jaw"), WeightOf(Driver, TEXT("jawOpen")) > 0.05f);

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

#endif // WITH_DEV_AUTOMATION_TESTS
