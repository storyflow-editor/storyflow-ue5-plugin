// Copyright 2026 StoryFlow. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSP/FFTAlgorithm.h"
#include "DSP/AudioFFT.h"
#include "DSP/BufferVectorOperations.h"
#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "Lipsync/StoryFlowVoiceSpectrum.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSteadyVoiceTest, "StoryFlow.Lipsync.Spectrum.SteadyHarmonicVoice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowSteadyVoiceTest::RunTest(const FString&)
{
	TArray<float> Frequencies, Magnitudes, Samples;
	for (int32 I = 0; I < 24; ++I) Frequencies.Add(FMath::Lerp(90.f, 4200.f, I / 23.f));
	Samples.SetNumZeroed(4096);
	for (float Gain : {.2f, .02f})
	for (float Pitch : {85.f, 103.f, 157.f, 211.f})
	{
		float Minimum = MAX_flt, Maximum = -MAX_flt;
		for (int32 Phase = 0; Phase < 32; ++Phase)
		{
			for (int32 I = 0; I < Samples.Num(); ++I)
			{
				const double Time = I / 48000. + Phase / (32. * Pitch);
				double Value = 0.;
				for (int32 Harmonic = 1; Harmonic < 35; ++Harmonic)
					Value += FMath::Sin(2. * PI * Pitch * Harmonic * Time) / Harmonic;
				Samples[I] = float(Gain * Value);
			}
			FStoryFlowSpectrumAnalyzer Analyzer(48000.f);
			Analyzer.PushAudio(Samples);
			Analyzer.Read(Frequencies, Magnitudes);
			float Sum = 0.f, Weighted = 0.f;
			for (int32 I = 0; I < Magnitudes.Num(); ++I)
			{
				const float Weight = FMath::Pow(FMath::Clamp((20.f * FMath::LogX(10.f,
					FMath::Max(Magnitudes[I], 1.e-9f) / 32.f) + 100.f) / 70.f, 0.f, 1.f), 1.31f);
				Sum += Weight; Weighted += Weight * I;
			}
			// The same sound, no articulation change. Do not clamp the calibrated
			// selector: clipping would hide instability at the ends of the axis.
			const float Centroid = Weighted / FMath::Max(Sum, 1.e-9f) / 23.f * 9.44f - 3.89f;
			Minimum = FMath::Min(Minimum, Centroid); Maximum = FMath::Max(Maximum, Centroid);
		}
		AddInfo(FString::Printf(TEXT("%.0f Hz gain %.2f fixed voice selector span = %.6f"), Pitch, Gain, Maximum - Minimum));
		TestTrue(TEXT("A fixed voiced sound moves less than 2 percent of the vowel axis"), Maximum - Minimum < .02f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowStreamSpectrumTest, "StoryFlow.Lipsync.Spectrum.StreamChunksAndRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowStreamSpectrumTest::RunTest(const FString&)
{
	TArray<float> Samples, Expected, Actual;
	const TArray<float> Frequencies = {400.f, 1200.f, 2400.f};
	Samples.SetNumUninitialized(4096);
	for (int32 I = 0; I < Samples.Num(); ++I) Samples[I] = .25f * FMath::Sin(2.f * PI * 400.f * I / 48000.f);
	FStoryFlowSpectrumAnalyzer Whole(48000.f), Chunked(48000.f);
	Whole.PushAudio(Samples); Whole.Read(Frequencies, Expected);
	for (int32 Offset = 0; Offset < Samples.Num();)
	{
		const int32 Count = FMath::Min(137, Samples.Num() - Offset);
		Chunked.PushAudio(MakeArrayView(Samples.GetData() + Offset, Count)); Offset += Count;
	}
	Chunked.Read(Frequencies, Actual);
	TestTrue(TEXT("Quiet partial audio callbacks still produce a usable spectrum"), Actual[0] > 10.f);
	for (int32 I = 0; I < Expected.Num(); ++I)
		TestEqual(TEXT("Audio packet boundaries do not change the spectrum"), Actual[I], Expected[I], .0001f);
	// A hop-aligned vowel change fully replaces the previous window in 43 ms.
	Samples.SetNumUninitialized(2048);
	for (int32 I = 0; I < Samples.Num(); ++I) Samples[I] = .25f * FMath::Sin(2.f * PI * 2400.f * I / 48000.f);
	Chunked.PushAudio(Samples); Chunked.Read(Frequencies, Actual);
	TestTrue(TEXT("New articulation is available within 43 ms"), Actual[2] > 10.f && Actual[0] < .1f);
	Samples.Init(0.f, 2048); Chunked.PushAudio(Samples); Chunked.Read(Frequencies, Actual);
	TestEqual(TEXT("Silence clears RMS within 43 ms"), Chunked.RMS, 0.f);
	for (float Value : Actual) TestEqual(TEXT("Silence clears every band"), Value, 0.f);
	Samples.Init(.25f, 4096); Chunked.PushAudio(Samples);
	Chunked.PushAudio(MakeArrayView(Samples.GetData(), 137));
	Samples.Init(0.f, 2559); Chunked.PushAudio(Samples); Chunked.Read(Frequencies, Actual);
	TestEqual(TEXT("Unaligned silence clears within one window plus one hop (54 ms)"), Chunked.RMS, 0.f);
	for (float Value : Actual) TestEqual(TEXT("Unaligned silence clears every band"), Value, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowContinuousMotionTest, "StoryFlow.Lipsync.Spectrum.ContinuousMouthMotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowContinuousMotionTest::RunTest(const FString&)
{
	const StoryFlowVisemeTable::FTable Table = {
		{TEXT("OO"), {{TEXT("jawOpen"), 1.f}}}, {TEXT("OH"), {{TEXT("jawOpen"), 1.f}}},
		{TEXT("AA"), {{TEXT("jawOpen"), 1.f}}}, {TEXT("EE"), {{TEXT("jawOpen"), 1.f}}}
	};
	TArray<float> Magnitudes; Magnitudes.Init(.01f, 24);
	for (int32 FPS : {30, 60, 120})
	{
		FStoryFlowLipsyncDriver Driver(Table);
		Driver.Strength = .4f; Driver.Smooth = 20.f; Driver.bContinuousMotion = true;
		for (int32 I = 0; I < FPS; ++I) Driver.AdvanceFromMagnitudes(Magnitudes, 1.f / FPS);
		const float Before = Driver.Current().FindRef(TEXT("jawOpen"));
		Driver.Strength = 1.f;
		Driver.AdvanceFromMagnitudes(Magnitudes, 1.f / 120.f);
		const float First = Driver.Current().FindRef(TEXT("jawOpen"));
		Driver.AdvanceFromMagnitudes(Magnitudes, 1.f / 120.f);
		const float Second = Driver.Current().FindRef(TEXT("jawOpen"));
		// An abrupt target change must build motion, rather than immediately
		// delivering the largest displacement and reversing velocity next frame.
		TestTrue(TEXT("Motion starts gradually and gains speed"), Second - First > First - Before);
		const float Gap = Driver.Target().FindRef(TEXT("jawOpen")) - Before;
		TestTrue(TEXT("First 8 ms moves less than 6 percent of the target gap"), First - Before < Gap * .06f);
		for (int32 I = 0; I < FPS / 5; ++I) Driver.AdvanceFromMagnitudes(Magnitudes, 1.f / FPS);
		TestTrue(TEXT("Sustained articulation reaches 98 percent within 220 ms"), Driver.Current().FindRef(TEXT("jawOpen")) > .98f);
		const float Hold = Driver.Current().FindRef(TEXT("jawOpen"));
		Driver.AdvanceSilent(0.f);
		TestEqual(TEXT("Paused motion holds"), Driver.Current().FindRef(TEXT("jawOpen")), Hold);
		for (int32 I = 0; I < FPS / 5; ++I) Driver.AdvanceSilent(1.f / FPS);
		TestTrue(TEXT("Stop closes below half a percent within 200 ms"), Driver.Current().FindRef(TEXT("jawOpen")) < .005f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVowelTransientTest, "StoryFlow.Lipsync.Spectrum.VowelTransientDoesNotSnapJaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowVowelTransientTest::RunTest(const FString&)
{
	const StoryFlowVisemeTable::FTable Table = {
		{TEXT("OO"), {{TEXT("jawOpen"), .20f}}}, {TEXT("OH"), {{TEXT("jawOpen"), .62f}}},
		{TEXT("AA"), {{TEXT("jawOpen"), .85f}}}, {TEXT("EE"), {{TEXT("jawOpen"), .28f}}}
	};
	for (const int32 FPS : {30,60,120})
	{
		FStoryFlowLipsyncDriver Driver(Table);
		Driver.Strength = Driver.JawBias = Driver.Sensitivity = 1.f;
		Driver.FullScale = 32.f;
		Driver.VowelScale = 9.44f;
		Driver.VowelOffset = -3.89f;
		Driver.ArticulationTransitionSeconds = .045f;
		TArray<float> Steady, Transient;
		Steady.Init(0.f, 24); Transient.Init(0.f, 24);
		Steady[11] = 1.f; Transient[12] = 1.f;
		for (int32 I = 0; I < FPS; ++I) Driver.AdvanceFromMagnitudes(Steady, 1.f / FPS);
		const float Before = Driver.Target().FindRef(TEXT("jawOpen"));
		// A short spectral spike inside a sustained vowel must not switch the jaw
		// to a nearly closed EE pose. Test the selection, before final morph easing.
		Driver.AdvanceFromMagnitudes(Transient, 1.f / 120.f);
		const float After = Driver.Target().FindRef(TEXT("jawOpen"));
		AddInfo(FString::Printf(TEXT("%d FPS vowel jaw before=%f after spike=%f"), FPS, Before, After));
		TestTrue(TEXT("Brief brightness spike retains at least 95 percent of the vowel opening"), After > Before * .95f);
		for (int32 I = 0; I < FPS / 5; ++I) Driver.AdvanceFromMagnitudes(Transient, 1.f / FPS);
		TestTrue(TEXT("Sustained new vowel is reached within 200 ms"), Driver.Centroid() > .95f);
		Driver.AdvanceSilent(1.f);
		TestTrue(TEXT("Stop still releases the jaw promptly"), Driver.Current().FindRef(TEXT("jawOpen")) < .001f);
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowMotionResetTest, "StoryFlow.Lipsync.Spectrum.RestoreClearsMotion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowMotionResetTest::RunTest(const FString&)
{
    FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
    Driver.bContinuousMotion = true;
    TArray<float> Voice; Voice.Init(.02f, 24);
    for (int32 I = 0; I < 60; ++I) Driver.AdvanceFromMagnitudes(Voice, 1.f / 60.f);
    TestTrue(TEXT("Speech produced a pose before restore"), Driver.Current().FindRef(TEXT("jawOpen")) > .01f);
    Driver.ResetPose();
    for (int32 I = 0; I < 60; ++I)
    {
        Driver.AdvanceSilent(1.f / 60.f);
        for (const auto& Weight : Driver.Current())
            TestEqual(TEXT("Restore clears both motion stages, without a later rebound"), Weight.Value, 0.f);
    }
    return true;
}
#endif
