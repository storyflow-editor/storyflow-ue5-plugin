// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
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
 * THE INPUT DOMAIN IS THE WHOLE STORY. Every constant here came from a three.js build that read
 * `getByteFrequencyData()/255` — Web Audio's DECIBEL mapping (-100 dB -> 0, -30 dB -> 1) after the
 * analyser's own per-bin temporal smoothing. Handed the LINEAR magnitudes an engine FFT actually produces,
 * the same constants sit twenty times above where speech lives and the mouth never opens. So the driver
 * reproduces that domain itself, per bin, before it measures anything: see AdvanceFromMagnitudes.
 *
 * The legacy defaults follow the normative v2 spec in LIPSYNC_DESIGN.md, shared with the Unity arm.
 * Spectral articulation is an optional rig calibration and does not change that default response.
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

	/** The pose being eased toward. A TEST SEAM: the spec forbids writing a key no pose owns. */
	const TMap<FName, float>& Target() const { return TargetWeights; }

	/** Loudness 0..1 after the peak follower, for a meter. Not part of the pose. */
	float Level() const { return LevelValue; }

	/**
	 * Where the last analysed frame sat on the vowel axis, 0 (OO) to 1 (EE). The second meter: a mouth that
	 * opens but looks wrong is usually a centroid pinned at one end, and this says which.
	 */
	float Centroid() const { return CentroidValue; }

	/**
	 * The largest RAW magnitude seen since the last ResetLevel, before the reference-domain transform.
	 *
	 * This is the calibration instrument for FullScale, which is algebra rather than measurement on Unreal:
	 * play a loud line, read this, and that is the number FullScale wants.
	 */
	float RawPeak() const { return RawPeakValue; }

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
	/** Close immediately, including a zero-time restore frame. */
	void ResetPose();

	// Tunables. Defaults are the three.js build's, which is the point of them.
	float Strength = 0.55f;
	float Sensitivity = 1.0f;
	float JawBias = 1.0f;
	float Smooth = 16.0f;
	/** Source-audio opt-in: continuous velocity with the same low-frequency response delay. */
	bool bContinuousMotion = false;

	/**
	 * The raw magnitude a full-scale sine produces at its own bin in whatever is feeding this driver — the
	 * divisor that puts the incoming spectrum back on the reference's decibel scale.
	 *
	 * 1.0 here because that is what a NORMALISED spectrum wants (Unity's GetSpectrumData); the Unreal
	 * component overrides it with its own AnalysisFullScale, because the mixer's magnitudes are not
	 * normalised at all.
	 */
	float FullScale = 1.0f;
	/** Optional rig calibration; defaults retain the original vowel axis. */
	float VowelScale = 2.6f;
	float VowelOffset = 0.f;
	float SpectralContrast = 1.f;
	/** Optional spectral response: retain spectral peaks and separate articulation from loudness.
	 * Broad acoustic cues only. Does not consume a transcript, reference animation or future audio. */
	bool bSpectralArticulation = false;
	float ArticulationBlend = 1.f;
	/** Stabilize acoustic shape selection before the nonlinear pose blend; zero retains legacy behavior. */
	float ArticulationTransitionSeconds = 0.f;
	/** AR20 mouthClose corrects an open jaw; it must not push a resting lip upward. */
	bool bJawRelativeClosure = false;

private:
	void BuildAxisPose(float Centroid, float Amp, float Gate);
	void BuildArticulatedPose(const TArray<float>& Magnitudes, float DeltaSeconds, float Amp);
	void AccumulateBlend(const StoryFlowVisemeTable::FPose* Pose, float Share, float Amp);
	void ClearTarget();
	void Ease(float DeltaSeconds);

	StoryFlowVisemeTable::FTable Table;
	TMap<FName, float> CurrentWeights;
	TMap<FName, float> MotionWeights;
	TMap<FName, float> TargetWeights;

	/** True when some pose in the ACTIVE table drives mouthClose — the closing breath's licence to write it. */
	bool bOwnsMouthClose = false;

	/**
	 * The ACTIVE table's pose names minus `rest`, sorted, built once. The idle mouth picks from this rather
	 * than from the built-in names, so a custom map with four poses idles on its four and not on eight names
	 * it has never heard of.
	 */
	TArray<FName> IdlePool;

	/** Per-bin temporal smoothing state, the analyser's `smoothingTimeConstant` the reference relied on. */
	TArray<float> Smoothed;
	float ArticulationOpen = 0.f;
	float ArticulationWide = 0.f;
	float ArticulationSibilant = 0.f;
	float ArticulationClosed = 0.f;
	float StableCentroid = 0.f;
	bool bHasStableCentroid = false;

	float Peak = 0.12f;
	float LevelValue = 0.0f;
	float CentroidValue = 0.0f;
	float RawPeakValue = 0.0f;

	// Idle mouth state.
	FRandomStream Random;
	FName IdlePose = TEXT("rest");
	float IdleHold = 0.0f;
};
