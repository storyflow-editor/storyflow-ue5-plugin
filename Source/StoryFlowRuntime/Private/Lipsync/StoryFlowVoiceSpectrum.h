// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once
#include "AudioDeviceHandle.h"
#include "AudioBusSubsystem.h"
#include "DSP/FFTAlgorithm.h"
#include "DSP/AudioFFT.h"
#include "DSP/BufferVectorOperations.h"
#include "UObject/StrongObjectPtr.h"

class UAudioComponent;
class UAudioBus;

/** Streaming DSP, independent of the audio-device tap so signal behavior is testable. */
struct FStoryFlowSpectrumAnalyzer
{
	explicit FStoryFlowSpectrumAnalyzer(float InSampleRate);
	void PushAudio(TArrayView<const float> Audio);
	void Read(const TArray<float>& Frequencies, TArray<float>& Magnitudes) const;
	float RMS = 0.f;
private:
	void AnalyzeWindow();
	static constexpr int32 WindowSize = 2048;
	static constexpr int32 HopSize = 512;
	int32 WritePosition = 0;
	int32 SamplesUntilAnalysis = HopSize;
	float SampleRate;
	TUniquePtr<Audio::IFFTAlgorithm> FFT;
	TUniquePtr<Audio::FWindow> Window;
	Audio::FAlignedFloatBuffer Samples;
	Audio::FAlignedFloatBuffer WindowedSamples;
	Audio::FAlignedFloatBuffer Spectrum;
};

/** One live, non-audible pre-volume tap. No voice-file decode, cached poses or model. */
struct FStoryFlowVoiceSpectrum
{
	FStoryFlowVoiceSpectrum(UObject* Owner, UAudioComponent* Audio);
	~FStoryFlowVoiceSpectrum();
	void Read(const TArray<float>& Frequencies, TArray<float>& Magnitudes);
	TWeakObjectPtr<UAudioComponent> Source;
	uint32 PlayOrder = 0;
	float RMS = 0.f;
private:
	FAudioDeviceHandle Device;
	TStrongObjectPtr<UAudioBus> Bus;
	Audio::FPatchOutputStrongPtr Patch;
	TUniquePtr<FStoryFlowSpectrumAnalyzer> Analyzer;
	Audio::FAlignedFloatBuffer Samples;
};
