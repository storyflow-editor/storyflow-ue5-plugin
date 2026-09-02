// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "Lipsync/StoryFlowVisemeTable.h"

/**
 * The Tier 1 lipsync driver.
 *
 * Whether a mouth reads as speech is not testable — only eyes judge a face. What IS testable is everything a
 * wrong number breaks first: that loudness opens the mouth, that pitch moves it along the vowel axis, that
 * silence closes it, that easing does not drift with the frame rate, and that the identity morphs a Sidekick
 * rig carries are never in the driver's reach.
 *
 * These mirror the Unity arm's tests case for case. The two arms implement one normative spec
 * (LIPSYNC_DESIGN.md), so a change that passes here and fails there means the arms have drifted.
 */

namespace
{
	/** Magnitudes for the component's evenly spaced band, with the energy concentrated at one position. */
	TArray<float> BandsPeakingAt(float Position, float Amplitude, int32 Count = 24)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncLoudOpensAndSilenceClosesTest,
	"StoryFlow.Lipsync.LoudAudioOpensTheJawAndSilenceClosesIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncLoudOpensAndSilenceClosesTest::RunTest(const FString& Parameters)
{
	FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
	Settle(Driver, BandsPeakingAt(0.25f, 0.9f));
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
	// Low frequencies sit at the OO end of the axis (pucker), high at the EE end (stretch). This is the
	// difference between a mouth that shapes vowels and one that only flaps open and shut.
	FStoryFlowLipsyncDriver Low(StoryFlowVisemeTable::Default());
	Settle(Low, BandsPeakingAt(0.05f, 0.9f));

	FStoryFlowLipsyncDriver High(StoryFlowVisemeTable::Default());
	Settle(High, BandsPeakingAt(0.95f, 0.9f));

	TestTrue(TEXT("low audio puckers more"), WeightOf(Low, TEXT("mouthPucker")) > WeightOf(High, TEXT("mouthPucker")));
	TestTrue(TEXT("high audio stretches more"), WeightOf(High, TEXT("mouthStretchLeft")) > WeightOf(Low, TEXT("mouthStretchLeft")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncIdentityMorphTest,
	"StoryFlow.Lipsync.NeverOwnsAnIdentityMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncIdentityMorphTest::RunTest(const FString& Parameters)
{
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
	// The same elapsed time must reach the same place whether it arrives in 30 frames or 120. A raw
	// per-frame lerp constant fails this, and the tuning then refuses to hold across machines.
	const TArray<float> Bands = BandsPeakingAt(0.25f, 0.9f);
	FStoryFlowLipsyncDriver Slow(StoryFlowVisemeTable::Default());
	FStoryFlowLipsyncDriver Fast(StoryFlowVisemeTable::Default());
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Slow.AdvanceFromMagnitudes(Bands, 1.0f / 30.0f);
	}
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		Fast.AdvanceFromMagnitudes(Bands, 1.0f / 120.0f);
	}

	const float Drift = FMath::Abs(WeightOf(Slow, TEXT("jawOpen")) - WeightOf(Fast, TEXT("jawOpen")));
	TestTrue(FString::Printf(TEXT("easing is frame-rate independent (drift %.3f)"), Drift), Drift < 0.05f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowLipsyncIdleMouthTest,
	"StoryFlow.Lipsync.IdleMouthMoves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStoryFlowLipsyncIdleMouthTest::RunTest(const FString& Parameters)
{
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
	// A rig-specific map replaces the table wholesale: it must drive its own names, and must not carry the
	// built-in ones along behind it onto a rig that has never heard of them.
	StoryFlowVisemeTable::FTable Custom;
	Custom.Add(TEXT("rest"), StoryFlowVisemeTable::FPose());
	for (const FName Pose : StoryFlowVisemeTable::Axis())
	{
		Custom.Add(Pose, StoryFlowVisemeTable::FPose{ { TEXT("customShape"), 1.0f } });
	}

	FStoryFlowLipsyncDriver Driver(Custom);
	Settle(Driver, BandsPeakingAt(0.25f, 0.9f));
	TestTrue(TEXT("a custom map drives its own morph names"), WeightOf(Driver, TEXT("customShape")) > 0.05f);
	TestFalse(TEXT("a custom map drives nothing else"), Driver.Current().Contains(TEXT("jawOpen")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
