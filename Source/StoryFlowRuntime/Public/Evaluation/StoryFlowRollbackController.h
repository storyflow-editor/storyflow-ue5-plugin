// Copyright 2026 StoryFlow. All Rights Reserved.
#pragma once
#include "Evaluation/StoryFlowExecutionSnapshot.h"

class UStoryFlowComponent;
class UStoryFlowSubsystem;
class UStoryFlowProjectAsset;

class STORYFLOWRUNTIME_API FStoryFlowRollbackController
{
public:
	static constexpr int64 PayloadLimit = 32LL * 1024 * 1024;
	FStoryFlowRollbackController(UStoryFlowComponent* Component, UStoryFlowSubsystem* Subsystem, int32 Limit);
	FStoryFlowRollbackAvailability Availability() const;
	FStoryFlowRollbackResult GoBack();
	void Clear(const FString& Reason, bool bPermanent = false);
	void Capture();
	void BeforeLeave();
	void Publish(bool bForce = false);
	void Enter() { ++Depth; }
	void Exit();
	int32 Count() const { return History.Num(); }
	int64 EstimatedBytes() const { return Bytes; }
	uint64 Captures = 0;
	double CaptureMilliseconds = 0, PrepareMilliseconds = 0, CommitMilliseconds = 0;
#if WITH_DEV_AUTOMATION_TESTS
	static bool bFailPrepare;
	static bool bFailCommit;
	static bool bFailRecovery;
#endif
private:
	TWeakObjectPtr<UStoryFlowComponent> Component;
	TWeakObjectPtr<UStoryFlowSubsystem> Subsystem;
	TArray<FStoryFlowExecutionSnapshot> History;
	FStoryFlowRollbackAvailability Published;
	int32 Limit, Depth = 0;
	int64 Bytes = 0;
	uint64 BlockedEntry = 0;
	bool bBlocked = false, bContentChanged = false, bRestoring = false, bNotifying = false, bInvalid = false;
	FString Reason = TEXT("empty");
};

/** Hold the outgoing controller alive across synchronous restart callbacks. */
struct STORYFLOWRUNTIME_API FStoryFlowRollbackExecutionScope
{
	TSharedPtr<FStoryFlowRollbackController> Owner;
	explicit FStoryFlowRollbackExecutionScope(TSharedPtr<FStoryFlowRollbackController> InOwner) : Owner(InOwner) { if (Owner) Owner->Enter(); }
	~FStoryFlowRollbackExecutionScope() { if (Owner) Owner->Exit(); }
};

/** Public host boundaries never inherit an executing story's ownership. */
struct STORYFLOWRUNTIME_API FStoryFlowRollbackMutationScope
{
	UStoryFlowSubsystem* Subsystem;
	bool bContent;
	explicit FStoryFlowRollbackMutationScope(UStoryFlowSubsystem* Subsystem, bool bContent = false);
	~FStoryFlowRollbackMutationScope();
};

/** Import exclusion spans every subsystem and future registration for this project. */
struct STORYFLOWRUNTIME_API FStoryFlowContentUpdateScope
{
	TWeakObjectPtr<UStoryFlowProjectAsset> Project;
	explicit FStoryFlowContentUpdateScope(UStoryFlowProjectAsset* Project);
	~FStoryFlowContentUpdateScope();
	static bool IsChanging(UStoryFlowProjectAsset* Project);
};
