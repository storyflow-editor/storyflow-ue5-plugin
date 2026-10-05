// Copyright 2026 StoryFlow. All Rights Reserved.

#include "MetaHuman/StoryFlowMetaHumanAnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "AnimNodes/AnimNode_CopyPoseFromMesh.h"
#include "Runtime/Launch/Resources/Version.h"

struct FStoryFlowMetaHumanAnimProxy : FAnimInstanceProxy
{
	explicit FStoryFlowMetaHumanAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance)
	{
		CopyBody.bUseAttachedParent = true;
		CopyBody.bCopyCurves = false;
	}
	FAnimNode_CopyPoseFromMesh CopyBody;
	TMap<FName, float> Controls;
	const UAnimSequence* BakedAnimation = nullptr;
	float BakedTime = 0.f;
	virtual void Initialize(UAnimInstance* Instance) override
	{
		FAnimInstanceProxy::Initialize(Instance);
		CopyBody.Initialize_AnyThread(FAnimationInitializeContext(this));
	}
	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override
	{
		FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
		CopyBody.PreUpdate(Instance);
		const auto* Face = CastChecked<UStoryFlowMetaHumanAnimInstance>(Instance);
		Controls = Face->RigControls;
		BakedAnimation = Face->BakedAnimation;
		BakedTime = Face->BakedTime;
	}
	virtual void CacheBones() override
	{
		CopyBody.CacheBones_AnyThread(FAnimationCacheBonesContext(this));
	}
	virtual void Update(float DeltaSeconds) override
	{
		CopyBody.Update_AnyThread(FAnimationUpdateContext(this, DeltaSeconds));
	}
	virtual bool Evaluate(FPoseContext& Output) override
	{
		CopyBody.Evaluate_AnyThread(Output);
		if (BakedAnimation)
		{
			// Keep body pose and let the existing post process run RigLogic from the
			// exported facial control curves. No editor solver is used at runtime.
			UStoryFlowMetaHumanAnimInstance::ApplyBakedSpeechCurves(*BakedAnimation, BakedTime, Output.Curve);
			return true;
		}
		for (const auto& Control : Controls)
		{
			Output.Curve.Set(Control.Key, Control.Value);
		}
		return true;
	}
};

void UStoryFlowMetaHumanAnimInstance::ApplyBakedSpeechCurves(const UAnimSequence& Animation, float Time, FBlendedCurve& Output)
{
	// Epic's mouth-only solver also includes these nose and neck controls. Keep
	// them for speech quality without admitting unrelated controls from a full-face bake.
	static const TSet<FName> AdditionalSpeechControls = {
		TEXT("CTRL_expressions_neckDigastricDown"),
		TEXT("CTRL_expressions_neckMastoidContractL"), TEXT("CTRL_expressions_neckMastoidContractR"),
		TEXT("CTRL_expressions_neckStretchL"), TEXT("CTRL_expressions_neckStretchR"),
		TEXT("CTRL_expressions_neckSwallowPh1"), TEXT("CTRL_expressions_neckThroatDown"), TEXT("CTRL_expressions_neckThroatExhale"),
		TEXT("CTRL_expressions_noseNasolabialDeepenL"), TEXT("CTRL_expressions_noseNasolabialDeepenR"),
		TEXT("CTRL_expressions_noseNostrilDilateL"), TEXT("CTRL_expressions_noseNostrilDilateR"),
		TEXT("CTRL_expressions_noseWrinkleL"), TEXT("CTRL_expressions_noseWrinkleR")};
	// EvaluateCurveData replaces its destination. Sampling separately preserves
	// the incoming pose's gaze, blinks, brows, head controls and other curves.
	FBlendedCurve Sampled;
	Sampled.InitFrom(Output);
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 6
	Animation.EvaluateCurveData(Sampled, FAnimExtractContext(double(Time)));
#else
	Animation.EvaluateCurveData(Sampled, Time);
#endif
	Sampled.ForEachElement([&Output](const UE::Anim::FCurveElement& Curve)
	{
		const FString Name = Curve.Name.ToString();
		if (Name.StartsWith(TEXT("CTRL_expressions_jaw")) || Name.StartsWith(TEXT("CTRL_expressions_mouth"))
			|| Name.StartsWith(TEXT("CTRL_expressions_teeth")) || Name.StartsWith(TEXT("CTRL_expressions_tongue"))
			|| AdditionalSpeechControls.Contains(Curve.Name))
			Output.Set(Curve.Name, Curve.Value);
	});
}

FAnimInstanceProxy* UStoryFlowMetaHumanAnimInstance::CreateAnimInstanceProxy()
{
	return new FStoryFlowMetaHumanAnimProxy(this);
}

void UStoryFlowMetaHumanAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete InProxy;
}
