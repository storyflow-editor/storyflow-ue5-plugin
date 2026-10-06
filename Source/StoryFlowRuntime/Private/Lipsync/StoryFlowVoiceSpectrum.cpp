// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowVoiceSpectrum.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Sound/AudioBus.h"
#include "Runtime/Launch/Resources/Version.h"

FStoryFlowVoiceSpectrum::FStoryFlowVoiceSpectrum(UObject* Owner, UAudioComponent* Audio) : Source(Audio)
{
	PlayOrder = Audio ? Audio->GetLastPlayOrder() : 0;
	Device = Audio && Audio->GetWorld() ? Audio->GetWorld()->GetAudioDevice() : FAudioDeviceHandle();
	auto* Subsystem = Device.IsValid() ? Device->GetSubsystem<UAudioBusSubsystem>() : nullptr;
	if (!Subsystem) return;
	Bus.Reset(NewObject<UAudioBus>(Owner, NAME_None, RF_Transient));
	Bus->AudioBusChannels = EAudioBusChannels::Mono;
	const Audio::FAudioBusKey Key(Bus->GetUniqueID());
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 6
	Subsystem->StartAudioBus(Key, TEXT("StoryFlow voice analysis"), 1, false);
#else
	Subsystem->StartAudioBus(Key, 1, false);
#endif
	Patch = Subsystem->AddPatchOutputForAudioBus(Key, 4096, 1);
	Analyzer = MakeUnique<FStoryFlowSpectrumAnalyzer>(Device->GetSampleRate());
	Samples.SetNumZeroed(4096);
	// UE stores this bus buffer before multiplying source/output/focus volume.
	// The bus has no audible output and leaves the original playback route untouched.
	Audio->SetAudioBusSendPreEffect(Bus.Get(), 1.f);
}

FStoryFlowSpectrumAnalyzer::FStoryFlowSpectrumAnalyzer(float InSampleRate) : SampleRate(InSampleRate)
{
	Audio::FFFTSettings Settings;
	Settings.Log2Size = 11;
	Settings.bArrays128BitAligned = true;
	Settings.bEnableHardwareAcceleration = true;
	FFT = Audio::FFFTFactory::NewFFTAlgorithm(Settings);
	Window = MakeUnique<Audio::FWindow>(Audio::EWindowType::Hann, WindowSize, 1, false);
	Samples.SetNumZeroed(WindowSize);
	WindowedSamples.SetNumZeroed(WindowSize);
	if (FFT) Spectrum.SetNumZeroed(FFT->NumOutputFloats());
}

FStoryFlowVoiceSpectrum::~FStoryFlowVoiceSpectrum()
{
	if (Bus.IsValid())
	{
		// A replay owns a new ActiveSound. Do not enqueue a zero send on that sound,
		// or leave a pending send for a later replay after this tap is destroyed.
		if (Source.IsValid() && Source->IsPlaying() && Source->GetLastPlayOrder() == PlayOrder)
			Source->SetAudioBusSendPreEffect(Bus.Get(), 0.f);
		if (auto* Subsystem = Device.IsValid() ? Device->GetSubsystem<UAudioBusSubsystem>() : nullptr)
			Subsystem->StopAudioBus(Audio::FAudioBusKey(Bus->GetUniqueID()));
	}
	Patch.Reset();
}

void FStoryFlowVoiceSpectrum::Read(const TArray<float>& Frequencies, TArray<float>& Magnitudes)
{
	Magnitudes.Init(0.f, Frequencies.Num());
	if (!Patch.IsValid() || !Analyzer) return;
	const int32 Available = Patch->GetNumSamplesAvailable();
	if (Available > 0)
	{
		// Drain every available sample in normal playback, preserving overlap at
		// high frame rates. A hitch discards old backlog and bounds work to 8 FFTs.
		// Do not discard samples arriving between the availability check and pop.
		const int32 Read = Patch->PopAudio(Samples.GetData(), FMath::Min(Available, Samples.Num()), Available >= Samples.Num());
		if (Read > 0) Analyzer->PushAudio(MakeArrayView(Samples.GetData(), Read));
	}
	Analyzer->Read(Frequencies, Magnitudes);
	RMS = Analyzer->RMS;
}

void FStoryFlowSpectrumAnalyzer::PushAudio(TArrayView<const float> Audio)
{
	if (!FFT) return;
	for (const float Value : Audio)
	{
		Samples[WritePosition] = FMath::IsFinite(Value) ? Value : 0.f;
		WritePosition = (WritePosition + 1) % WindowSize;
		if (--SamplesUntilAnalysis == 0)
		{
			AnalyzeWindow();
			SamplesUntilAnalysis = HopSize;
		}
	}
}

void FStoryFlowSpectrumAnalyzer::AnalyzeWindow()
{
	// 43 ms of causal history at 48 kHz, refreshed every 11 ms. Startup is
	// zero-padded, so it need not wait for a full window before responding.
	double Energy = 0.;
	for (int32 I = 0; I < WindowSize; ++I)
	{
		const float Sample = Samples[(WritePosition + I) % WindowSize];
		WindowedSamples[I] = Sample;
		Energy += double(Sample) * Sample;
	}
	RMS = float(FMath::Sqrt(Energy / Samples.Num()));
	Window->ApplyToBuffer(WindowedSamples.GetData());
	FFT->ForwardRealToComplex(WindowedSamples.GetData(), Spectrum.GetData());
}

void FStoryFlowSpectrumAnalyzer::Read(const TArray<float>& Frequencies, TArray<float>& Magnitudes) const
{
	Magnitudes.Init(0.f, Frequencies.Num());
	if (!FFT) return;
	for (int32 I = 0; I < Frequencies.Num(); ++I)
	{
		// Smooth the spectral envelope, not isolated harmonic peaks. Triangular
		// 180 Hz half-width bands overlap the component's 24 frequency centers.
		// This rejects pitch/phase flutter while retaining broad vowel resonances.
		constexpr float HalfWidthHz = 180.f;
		const float BinHz = SampleRate / WindowSize;
		const int32 First = FMath::Clamp(FMath::CeilToInt((Frequencies[I] - HalfWidthHz) / BinHz), 0, WindowSize / 2);
		const int32 Last = FMath::Clamp(FMath::FloorToInt((Frequencies[I] + HalfWidthHz) / BinHz), 0, WindowSize / 2);
		float Power = 0.f;
		for (int32 Bin = First; Bin <= Last; ++Bin)
		{
			const float Weight = FMath::Max(0.f, 1.f - FMath::Abs(Bin * BinHz - Frequencies[I]) / HalfWidthHz);
			Power += Weight * (FMath::Square(Spectrum[2 * Bin]) + FMath::Square(Spectrum[2 * Bin + 1]));
		}
		// Keep the old 512-point Hann amplitude scale. Hann energy is 1.5 bins
		// for a bin-centered sinusoid; FFT length must not change calibration.
		Magnitudes[I] = FMath::Sqrt(Power / 1.5f) * (512.f / WindowSize);
	}
}
