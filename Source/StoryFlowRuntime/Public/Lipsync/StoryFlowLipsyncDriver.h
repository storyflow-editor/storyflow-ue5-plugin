// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Lipsync/StoryFlowVisemeTable.h"

/**
 * Audio in, mouth pose out. The whole of Tier 1's judgement lives here, and nothing in this class touches a
 * UObject, a component or the audio engine — it takes band magnitudes and a delta time and answers morph
 * weights, so the numbers that decide whether a mouth reads as speech are testable without a face.
 *
 * It is deliberately NOT phoneme recognition. Energy says how open the mouth is; the spectral centroid
 * slides it along OO -> OH -> AA -> EE. That reads convincingly against real speech and will not hit a
 * specific consonant, which is the honest trade for zero baking and zero dependencies. The baked-track tier
 * replaces the analysis and reuses everything below it.
 *
 * Every constant here is pinned by the normative spec in LIPSYNC_DESIGN.md, and the Unity arm implements the
 * same numbers. Read that before changing one.
 */
class STORYFLOWRUNTIME_API FStoryFlowLipsyncDriver
{
public:
	/** The analysed band. Below is room rumble; above is sibilance that would open a jaw on an S. */
	static constexpr float MinHz = 90.0f;
	static constexpr float MaxHz = 4200.0f;

	explicit FStoryFlowLipsyncDriver(const StoryFlowVisemeTable::FTable& InTable);

	/** The weights to write this frame. */
	const TMap<FName, float>& Current() const { return CurrentWeights; }

	/** Loudness 0..1 after the peak follower, for a meter. Not part of the pose. */
	float Level() const { return LevelValue; }

	/**
	 * Advance from band magnitudes: one entry per frequency in the set the component asked the mixer for,
	 * evenly spaced across MinHz..MaxHz.
	 *
	 * The band is chosen by the CALLER here, unlike the Unity arm which receives a full linear spectrum and
	 * slices it. The maths is the same quantity either way: with N evenly spaced frequencies spanning exactly
	 * the band, the magnitude-weighted mean index over N-1 IS the normalised centroid the spec describes.
	 */
	void AdvanceFromMagnitudes(const TArray<float>& Magnitudes, float DeltaSeconds);

	/** No analysable audio: random poses held briefly, so a subtitled line does not sit there with a dead face. */
	void AdvanceIdle(float DeltaSeconds);

	/** Close the mouth — what a component does when its line ends, rather than freezing mid-vowel. */
	void AdvanceSilent(float DeltaSeconds);

	/** Forget the loudness history. Call at the START of a line so takes do not scale each other. */
	void ResetLevel();

	// Tunables. Defaults are the three.js build's, which is the point of them.
	float Strength = 0.55f;
	float Sensitivity = 1.0f;
	float JawBias = 1.0f;
	float Smooth = 16.0f;

private:
	void BuildAxisPose(float Centroid, float Amp, float Gate);
	void AccumulateBlend(const StoryFlowVisemeTable::FPose* Pose, float Share, float Amp);
	void ClearTarget();
	void Ease(float DeltaSeconds);

	StoryFlowVisemeTable::FTable Table;
	TMap<FName, float> CurrentWeights;
	TMap<FName, float> TargetWeights;

	float Peak = 0.12f;
	float LevelValue = 0.0f;

	// Idle mouth state.
	FRandomStream Random;
	FName IdlePose = TEXT("rest");
	float IdleHold = 0.0f;
};
