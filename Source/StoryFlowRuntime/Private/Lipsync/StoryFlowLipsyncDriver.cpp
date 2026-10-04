// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowLipsyncDriver.h"

namespace
{
	/** Speech never reaches the top of the band, so an unscaled centroid never leaves the OO end. */
	constexpr float CentroidScale = 2.6f;

	/**
	 * Decay of the loudness peak PER SECOND-AT-60 — `pow(Decay, dt * 60)`, not once per frame.
	 *
	 * The reference decayed once a frame at roughly 60 Hz, so the number means what it means only when it is
	 * anchored to that rate. Applied per frame it gives a 21 s time constant at 60 fps and a 9 s one at 144,
	 * which is a mouth that behaves differently on a better machine.
	 */
	constexpr float PeakDecay = 0.9992f;
	constexpr float PeakFloor = 0.04f;
	constexpr float PeakInitial = 0.12f;

	constexpr float GateStart = 0.10f;
	constexpr float GateRange = 0.22f;
	constexpr float ClosingBreath = 0.45f;

	/** Web Audio's decibel window: -100 dB reads 0, -30 dB reads 1. The domain every constant above wants. */
	constexpr float ReferenceFloorDb = 100.0f;
	constexpr float ReferenceRangeDb = 70.0f;

	/** The analyser's per-bin temporal smoothing the reference ran with (`smoothingTimeConstant = .55`). */
	constexpr float SpectralSmoothing = 0.55f;
}

FStoryFlowLipsyncDriver::FStoryFlowLipsyncDriver(const StoryFlowVisemeTable::FTable& InTable)
	: Table(InTable.Num() > 0 ? InTable : StoryFlowVisemeTable::Default())
	, Random(FRandomStream(FMath::Rand()))
{
	for (const FName& Morph : StoryFlowVisemeTable::OwnedMorphs(Table))
	{
		CurrentWeights.Add(Morph, 0.0f);
	}
	bOwnsMouthClose = CurrentWeights.Contains(TEXT("mouthClose"));

	// The idle pool is the ACTIVE table's poses, not the built-in names: a rig-specific map with four poses
	// has to idle on its four. Sorted so the pool's order is deterministic on both engine arms; the seeds
	// differ, the walk does not.
	for (const TPair<FName, StoryFlowVisemeTable::FPose>& Pose : Table)
	{
		if (Pose.Key != TEXT("rest"))
		{
			IdlePool.Add(Pose.Key);
		}
	}
	IdlePool.Sort([](const FName& A, const FName& B) { return A.Compare(B) < 0; });
}

/**
 * The per-frame rule, v2. Steps below are the spec's, numbered as it numbers them.
 *
 * Step 1 is the one that was missing and the one everything else depended on: the incoming magnitudes are
 * LINEAR, and every constant here was placed on a DECIBEL scale. Converting per bin — rather than scaling
 * the constants — is what lets both engine arms keep one set of numbers while their FFTs disagree about
 * what "1.0" means.
 */
void FStoryFlowLipsyncDriver::AdvanceFromMagnitudes(const TArray<float>& Magnitudes, float DeltaSeconds)
{
	if (Magnitudes.Num() < 2)
	{
		AdvanceSilent(DeltaSeconds);
		return;
	}

	// A bin count that changed means a different analysis: the old smoothing state describes nothing.
	if (Smoothed.Num() != Magnitudes.Num())
	{
		Smoothed.Init(0.0f, Magnitudes.Num());
	}

	const float Divisor = FMath::Max(FullScale, 1e-9f);

	// 2's coefficient, PER SECOND at the reference's ~60 Hz like the peak follower below, so the analyser's
	// lag does not double at 30 fps and halve at 120. A zero or negative delta keeps every bin as it was.
	const float Keep = DeltaSeconds > 0.0f ? FMath::Pow(SpectralSmoothing, DeltaSeconds * 60.0f) : 1.0f;

	float Sum = 0.0f;
	float Weighted = 0.0f;
	for (int32 Index = 0; Index < Magnitudes.Num(); ++Index)
	{
		// A NaN or infinite sample would sit in the smoothing state and in the peak and turn every weight
		// after it into NaN for the rest of the line. Read it as silence instead.
		const float Raw = Magnitudes[Index];
		const float Magnitude = FMath::IsFinite(Raw) ? FMath::Max(0.0f, Raw) : 0.0f;
		RawPeakValue = FMath::Max(RawPeakValue, Magnitude);

		// 1. reference domain, per bin.
		const float Decibels = 20.0f * FMath::LogX(10.0f, FMath::Max(Magnitude, 1e-9f) / Divisor);
		const float Referenced = FMath::Clamp((Decibels + ReferenceFloorDb) / ReferenceRangeDb, 0.0f, 1.0f);

		// 2. spectral smoothing, per bin.
		float& Bin = Smoothed[Index];
		Bin = Keep * Bin + (1.0f - Keep) * Referenced;

		Sum += Bin;
		Weighted += Bin * Index;
	}

	// 3. energy and centroid, over the SMOOTHED spectrum.
	const float Energy = Sum / Magnitudes.Num();
	float Centroid = 0.0f;
	if (Sum > 0.0f)
	{
		Centroid = FMath::Clamp((Weighted / Sum) / (Magnitudes.Num() - 1) * CentroidScale, 0.0f, 1.0f);
	}
	CentroidValue = Centroid;

	// 4. peak follower, per second.
	Peak = FMath::Max(Energy, Peak * FMath::Pow(PeakDecay, DeltaSeconds * 60.0f));
	const float Norm = Energy / FMath::Max(PeakFloor, Peak);
	LevelValue = FMath::Min(1.0f, Norm);

	// 5, 6. gate and amplitude.
	const float Gate = FMath::Clamp((Norm - GateStart) / GateRange, 0.0f, 1.0f);
	const float Amp = FMath::Min(1.0f, Norm * 1.15f * Sensitivity) * Gate;

	// 7, 8, 9.
	BuildAxisPose(Centroid, Amp, Gate);
	Ease(DeltaSeconds);
}

void FStoryFlowLipsyncDriver::AdvanceIdle(float DeltaSeconds)
{
	// The meters report ANALYSED audio and nothing else. An idle mouth is moving on a coin flip, not on
	// anything it heard, and a level that stayed stale while it did would be a lying instrument.
	LevelValue = 0.0f;
	CentroidValue = 0.0f;

	IdleHold -= DeltaSeconds;
	if (IdleHold <= 0.0f)
	{
		// MM is the mouth-closed gap, so a table without it gaps on rest instead of reaching for a pose that
		// does not exist.
		const bool bGap = Random.FRand() < 0.20f;
		if (bGap || IdlePool.Num() == 0)
		{
			// The coin is drawn either way, so the number of draws a gap costs does not depend on the shape
			// of the table — two tables landing on different poses from the same seed would be drift.
			const bool bWantsMM = Random.FRand() < 0.5f;
			IdlePose = (bWantsMM && Table.Contains(TEXT("MM"))) ? FName(TEXT("MM")) : FName(TEXT("rest"));
			IdleHold = 0.14f + Random.FRand() * 0.22f;
		}
		else
		{
			IdlePose = IdlePool[Random.RandRange(0, IdlePool.Num() - 1)];
			IdleHold = 0.12f + Random.FRand() * 0.13f;
		}
	}

	ClearTarget();
	if (const StoryFlowVisemeTable::FPose* Pose = Table.Find(IdlePose))
	{
		for (const TPair<FName, float>& Morph : *Pose)
		{
			TargetWeights.Add(Morph.Key, Morph.Value * Strength * (Morph.Key == TEXT("jawOpen") ? JawBias : 1.0f));
		}
	}
	Ease(DeltaSeconds);
}

void FStoryFlowLipsyncDriver::AdvanceSilent(float DeltaSeconds)
{
	LevelValue = 0.0f;
	CentroidValue = 0.0f;
	ClearTarget();
	Ease(DeltaSeconds);
}

void FStoryFlowLipsyncDriver::ResetPose()
{
	ClearTarget();
	for (auto& Weight : CurrentWeights) Weight.Value = 0.0f;
	ResetLevel(); IdlePose = TEXT("rest"); IdleHold = 0.0f;
}

void FStoryFlowLipsyncDriver::ResetLevel()
{
	Peak = PeakInitial;
	LevelValue = 0.0f;
	RawPeakValue = 0.0f;

	// The smoothing state is loudness history too: a new line that inherited the last one's spectrum would
	// start its first frames shaped by whoever spoke before.
	for (float& Bin : Smoothed)
	{
		Bin = 0.0f;
	}
}

void FStoryFlowLipsyncDriver::BuildAxisPose(float Centroid, float Amp, float Gate)
{
	ClearTarget();

	const TArray<FName>& AxisPoses = StoryFlowVisemeTable::Axis();
	const float Position = Centroid * (AxisPoses.Num() - 1);
	const int32 Low = FMath::Clamp(FMath::FloorToInt(Position), 0, AxisPoses.Num() - 1);
	const int32 High = FMath::Min(AxisPoses.Num() - 1, Low + 1);
	const float Alpha = Position - Low;

	AccumulateBlend(Table.Find(AxisPoses[Low]), 1.0f - Alpha, Amp);
	AccumulateBlend(Table.Find(AxisPoses[High]), Alpha, Amp);

	// The closing breath: as the gate shuts the lips come together, instead of hanging half-open. Only when
	// some pose in this table owns mouthClose — writing a key nothing consumes leaves a float climbing
	// forever behind a step that silently does nothing.
	if (Gate < 1.0f && bOwnsMouthClose)
	{
		float& Closed = TargetWeights.FindOrAdd(TEXT("mouthClose"));
		Closed += (1.0f - Gate) * ClosingBreath * Strength;
	}
}

void FStoryFlowLipsyncDriver::AccumulateBlend(const StoryFlowVisemeTable::FPose* Pose, float Share, float Amp)
{
	if (Pose == nullptr || Share <= 0.0f)
	{
		return;
	}

	for (const TPair<FName, float>& Morph : *Pose)
	{
		const float Scale = Amp * Strength * (Morph.Key == TEXT("jawOpen") ? JawBias : 1.0f);
		float& Have = TargetWeights.FindOrAdd(Morph.Key);
		Have += Morph.Value * Share * Scale;
	}
}

void FStoryFlowLipsyncDriver::ClearTarget()
{
	for (const TPair<FName, float>& Morph : CurrentWeights)
	{
		TargetWeights.Add(Morph.Key, 0.0f);
	}
}

void FStoryFlowLipsyncDriver::Ease(float DeltaSeconds)
{
	// Frame-rate independent. A raw per-frame lerp constant would make the mouth snappier at 144 Hz than at
	// 30, which is the kind of thing nobody notices until the tuning refuses to hold across machines.
	//
	// A zero or negative delta HOLDS. Snapping to target there is the worst possible reading of a paused
	// game: the audio clock is not paused, so the mouth would jitter at full amplitude on a still frame.
	const float K = DeltaSeconds <= 0.0f ? 0.0f : 1.0f - FMath::Exp(-Smooth * DeltaSeconds);
	for (TPair<FName, float>& Morph : CurrentWeights)
	{
		const float* Want = TargetWeights.Find(Morph.Key);
		Morph.Value += ((Want != nullptr ? *Want : 0.0f) - Morph.Value) * K;

		// JawBias 2 on a 0.85 pose asks for 1.7. A morph target is 0..1 and everything past it extrapolates
		// the shape into a face nobody sculpted.
		Morph.Value = FMath::Clamp(Morph.Value, 0.0f, 1.0f);
	}
}
