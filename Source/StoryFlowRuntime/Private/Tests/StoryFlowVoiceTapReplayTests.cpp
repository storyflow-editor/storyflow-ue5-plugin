// Copyright 2026 StoryFlow. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Lipsync/StoryFlowVoiceSpectrum.h"
#include "ActiveSound.h"
#include "AudioDevice.h"
#include "AudioThread.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Sound/AudioBus.h"
#include "Sound/SoundWaveProcedural.h"

namespace StoryFlowVoiceTapReplayTests
{
struct FAudioFixture
{
	TStrongObjectPtr<UGameInstance> GameInstance{nullptr};
	TStrongObjectPtr<UAudioComponent> Audio{nullptr};
	TStrongObjectPtr<USoundWaveProcedural> Wave{nullptr};
	UWorld* World = nullptr;

	bool Init()
	{
		if (!GEngine) return false;
		GameInstance.Reset(NewObject<UGameInstance>(GEngine));
		GameInstance->InitializeStandalone();
		World = GameInstance->GetWorld();
		if (!World || !World->GetAudioDevice().IsValid()) return false;
		AActor* Owner = World->SpawnActor<AActor>();
		if (!Owner) return false;
		Audio.Reset(NewObject<UAudioComponent>(Owner));
		Audio->bAutoActivate = false;
		Audio->bAutoDestroy = false;
		Audio->bIsUISound = true;
		Wave.Reset(NewObject<USoundWaveProcedural>(Audio.Get()));
		Wave->SetSampleRate(48000);
		Wave->NumChannels = 1;
		Wave->Duration = INDEFINITELY_LOOPING_DURATION;
		// Silent PCM creates a real ActiveSound without making the test audible.
		// This test checks engine send ownership, not an implementation-shaped mock.
		TArray<int16> Silence;
		Silence.Init(0, 48000 * 4);
		Wave->QueueAudio(reinterpret_cast<const uint8*>(Silence.GetData()), Silence.Num() * sizeof(int16));
		Audio->SetSound(Wave.Get());
		Audio->RegisterComponentWithWorld(World);
		Audio->Play();
		return Audio->IsPlaying();
	}

	~FAudioFixture()
	{
		if (Audio.IsValid())
		{
			Audio->Stop();
			Audio->DestroyComponent();
		}
		FAudioCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
		if (World) World->DestroyWorld(false);
	}
};

struct FSendSnapshot
{
	bool bFoundCurrentPlayback = false;
	TArray<float> AudioBusSendLevels;
};

FSendSnapshot ReadCurrentSends(UAudioComponent& Audio)
{
	FSendSnapshot Snapshot;
	const uint32 ExpectedPlayOrder = Audio.GetLastPlayOrder();
	if (FAudioDevice* Device = Audio.GetAudioDevice())
	{
		Device->SendCommandToActiveSounds(Audio.GetAudioComponentID(),
			[&Snapshot, ExpectedPlayOrder](FActiveSound& Sound)
			{
				if (Sound.GetPlayOrder() != ExpectedPlayOrder) return;
				Snapshot.bFoundCurrentPlayback = true;
				TArray<FSoundSourceBusSendInfo> Sends;
				Sound.GetBusSends(EBusSendType::PreEffect, Sends);
				for (const auto& Send : Sends)
					if (Send.AudioBus) Snapshot.AudioBusSendLevels.Add(Send.SendLevel);
			});
		// The callback writes stack-owned results only before this fence completes.
		FAudioCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}
	return Snapshot;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowVoiceTapReplayTest,
	"StoryFlow.Lipsync.Spectrum.SourceTapSurvivesSameComponentReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowVoiceTapReplayTest::RunTest(const FString&)
{
	using namespace StoryFlowVoiceTapReplayTests;
	FAudioFixture Fixture;
	if (!TestTrue(TEXT("Replay regression requires an enabled audio device and real playback"), Fixture.Init())) return false;
	UAudioComponent& Audio = *Fixture.Audio.Get();
	auto Tap = MakeUnique<FStoryFlowVoiceSpectrum>(&Audio, &Audio);
	const uint32 FirstPlayOrder = Audio.GetLastPlayOrder();
	const FSendSnapshot First = ReadCurrentSends(Audio);
	TestTrue(TEXT("First playback has an ActiveSound"), First.bFoundCurrentPlayback);
	if (!TestEqual(TEXT("First tap adds one private analysis send"), First.AudioBusSendLevels.Num(), 1)) return false;
	TestEqual(TEXT("First analysis send receives audio"), First.AudioBusSendLevels[0], 1.f);

	// Match the production replacement ordering: construct the replacement,
	// then destroy the old tap while the SAME component owns a newer ActiveSound.
	Audio.Play();
	TestTrue(TEXT("Replay changes the playback identity without changing the component"), Audio.GetLastPlayOrder() != FirstPlayOrder);
	auto Replacement = MakeUnique<FStoryFlowVoiceSpectrum>(&Audio, &Audio);
	TestEqual(TEXT("Replacement records the new playback identity"), Replacement->PlayOrder, Audio.GetLastPlayOrder());
	Tap = MoveTemp(Replacement);
	const FSendSnapshot Replayed = ReadCurrentSends(Audio);
	TestTrue(TEXT("Replayed audio has an ActiveSound"), Replayed.bFoundCurrentPlayback);
	if (!TestEqual(TEXT("Retiring the previous tap adds no old send to the new playback"), Replayed.AudioBusSendLevels.Num(), 1)) return false;
	TestEqual(TEXT("Retiring the previous tap must not mute the replacement send"), Replayed.AudioBusSendLevels[0], 1.f);

	// A stopped component queues SetAudioBusSendPreEffect for its NEXT Play().
	// Tap cleanup must not enqueue a reference to the retired bus there.
	Audio.Stop();
	Tap.Reset();
	Audio.Play();
	const FSendSnapshot UntappedReplay = ReadCurrentSends(Audio);
	TestTrue(TEXT("Untapped replay has an ActiveSound"), UntappedReplay.bFoundCurrentPlayback);
	TestEqual(TEXT("Cleanup while stopped leaves no pending analysis send for a later replay"), UntappedReplay.AudioBusSendLevels.Num(), 0);
	return true;
}
#endif
