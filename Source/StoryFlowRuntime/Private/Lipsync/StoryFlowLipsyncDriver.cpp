// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowLipsyncDriver.h"

namespace
{
	/** Speech never reaches the top of the band, so an unscaled centroid never leaves the OO end. */
	constexpr float CentroidScale = 2.6f;

	/** Per-frame decay of the loudness peak, so a quiet take still reaches a full-open mouth. */
	constexpr float PeakDecay = 0.9992f;
	constexpr float PeakFloor = 0.04f;
	constexpr float PeakInitial = 0.12f;

	constexpr float GateStart = 0.10f;
	constexpr float GateRange = 0.22f;
	constexpr float ClosingBreath = 0.45f;
}

FStoryFlowLipsyncDriver::FStoryFlowLipsyncDriver(const StoryFlowVisemeTable::FTable& InTable)
	: Table(InTable.Num() > 0 ? InTable : StoryFlowVisemeTable::Default())
	, Random(FRandomStream(FMath::Rand()))
{
	for (const FName& Morph : StoryFlowVisemeTable::OwnedMorphs(Table))
	{
		CurrentWeights.Add(Morph, 0.0f);
	}
}

void FStoryFlowLipsyncDriver::AdvanceFromMagnitudes(const TArray<float>& Magnitudes, float DeltaSeconds)
{
	if (Magnitudes.Num() < 2)
	{
		AdvanceSilent(DeltaSeconds);
		return;
	}

	float Sum = 0.0f;
	float Weighted = 0.0f;
	for (int32 Index = 0; Index < Magnitudes.Num(); ++Index)
	{
		const float Amplitude = FMath::Max(0.0f, Magnitudes[Index]);
		Sum += Amplitude;
		Weighted += Amplitude * Index;
	}

	const float Energy = Sum / Magnitudes.Num();
	float Centroid = 0.0f;
	if (Sum > 0.0f)
	{
		Centroid = FMath::Clamp((Weighted / Sum) / (Magnitudes.Num() - 1) * CentroidScale, 0.0f, 1.0f);
	}

	Peak = FMath::Max(Energy, Peak * PeakDecay);
	const float Norm = Energy / FMath::Max(PeakFloor, Peak);
	LevelValue = FMath::Min(1.0f, Norm);

	const float Gate = FMath::Clamp((Norm - GateStart) / GateRange, 0.0f, 1.0f);
	const float Amp = FMath::Min(1.0f, Norm * 1.15f * Sensitivity) * Gate;

	BuildAxisPose(Centroid, Amp, Gate);
	Ease(DeltaSeconds);
}

void FStoryFlowLipsyncDriver::AdvanceIdle(float DeltaSeconds)
{
	IdleHold -= DeltaSeconds;
	if (IdleHold <= 0.0f)
	{
		const bool bGap = Random.FRand() < 0.20f;
		if (bGap)
		{
			IdlePose = Random.FRand() < 0.5f ? FName(TEXT("MM")) : FName(TEXT("rest"));
			IdleHold = 0.14f + Random.FRand() * 0.22f;
		}
		else
		{
			const TArray<FName>& Names = StoryFlowVisemeTable::PoseNames();
			IdlePose = Names[Random.RandRange(1, Names.Num() - 1)];
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
	ClearTarget();
	Ease(DeltaSeconds);
}

void FStoryFlowLipsyncDriver::ResetLevel()
{
	Peak = PeakInitial;
	LevelValue = 0.0f;
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

	// The closing breath: as the gate shuts the lips come together, instead of hanging half-open.
	if (Gate < 1.0f)
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
	const float K = DeltaSeconds <= 0.0f ? 1.0f : 1.0f - FMath::Exp(-Smooth * DeltaSeconds);
	for (TPair<FName, float>& Morph : CurrentWeights)
	{
		const float* Want = TargetWeights.Find(Morph.Key);
		Morph.Value += ((Want != nullptr ? *Want : 0.0f) - Morph.Value) * K;
	}
}
