// Copyright 2026 StoryFlow. All Rights Reserved.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Lipsync/StoryFlowLipsyncComponent.h"
#include "Lipsync/StoryFlowLipsyncDriver.h"
#include "Lipsync/StoryFlowVisemeTable.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowSidekickPresetTest, "StoryFlow.Lipsync.Sidekick.DefaultJawRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowSidekickPresetTest::RunTest(const FString&)
{
    auto* Preset = NewObject<UStoryFlowLipsyncComponent>();
    FStoryFlowLipsyncDriver Driver(StoryFlowVisemeTable::Default());
    Driver.Strength = Preset->Strength;
    Driver.Sensitivity = Preset->Sensitivity;
    Driver.JawBias = Preset->JawBias;
    Driver.Smooth = Preset->Smoothing;
    Driver.FullScale = Preset->AnalysisFullScale;
    Driver.VowelScale = Preset->VowelScale;
    Driver.VowelOffset = Preset->VowelOffset;
    Driver.SpectralContrast = Preset->SpectralContrast;
    Driver.bJawRelativeClosure = Preset->bJawRelativeClosure;
    Driver.bContinuousMotion = Preset->bAnalyzeVoiceBeforeVolume;
    Driver.ArticulationTransitionSeconds = .045f;
    TArray<float> Voice;
    // Sweep the spectrum so a narrow vowel cannot hide excessive opening elsewhere.
    float MaximumJaw = 0.f;
    for (int32 Band = 0; Band < 24; ++Band)
    {
        Voice.Init(0.f, 24);
        for (int32 I = 0; I <= Band; ++I) Voice[I] = .02f * Driver.FullScale;
        Driver.ResetPose();
        for (int32 Frame = 0; Frame < 120; ++Frame) Driver.AdvanceFromMagnitudes(Voice, 1.f / 60.f);
        MaximumJaw = FMath::Max(MaximumJaw, Driver.Current().FindRef(TEXT("jawOpen")));
        TestTrue(TEXT("Lip closure cannot exceed the jaw it corrects"),
            Driver.Current().FindRef(TEXT("mouthClose")) <= Driver.Current().FindRef(TEXT("jawOpen")));
    }
    TestTrue(TEXT("Default Sidekick speech still articulates an open vowel"), MaximumJaw > .2f);
    TestTrue(TEXT("Default Sidekick jaw stays within the approved reduced range"), MaximumJaw < .29f);
    for (int32 Frame = 0; Frame < 120; ++Frame) Driver.AdvanceSilent(1.f / 60.f);
    TestTrue(TEXT("The mouth returns to rest after speech"), Driver.Current().FindRef(TEXT("jawOpen")) < .0001f);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoryFlowCustomJawPresetTest, "StoryFlow.Lipsync.Sidekick.CustomJawMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStoryFlowCustomJawPresetTest::RunTest(const FString&)
{
    const StoryFlowVisemeTable::FTable Custom = {
        {TEXT("EE"), {{TEXT("customJaw"), .5f}, {TEXT("mouthClose"), .4f}}}
    };
    FStoryFlowLipsyncDriver Driver(Custom);
    Driver.bJawRelativeClosure = true;
    TArray<float> Voice; Voice.Init(.02f, 24);
    for (int32 I = 0; I < 120; ++I) Driver.AdvanceFromMagnitudes(Voice, 1.f / 60.f);
    TestTrue(TEXT("A custom jaw remains driven by its authored map"), Driver.Current().FindRef(TEXT("customJaw")) > .1f);
    TestTrue(TEXT("An absent ARKit jaw does not erase authored lip closure"), Driver.Current().FindRef(TEXT("mouthClose")) > .1f);
    return true;
}
#endif
