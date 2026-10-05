// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimCurveTypes.h"
#include "StoryFlowMetaHumanAnimInstance.generated.h"

class UAnimSequence;

/** Facial input for an assembled MetaHuman's existing RigLogic postprocess. */
UCLASS(Transient)
class STORYFLOWRUNTIME_API UStoryFlowMetaHumanAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	TMap<FName, float> RigControls;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> BakedAnimation;
	float BakedTime = 0.f;
	static void ApplyBakedSpeechCurves(const UAnimSequence& Animation, float Time, FBlendedCurve& Output);
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
};
