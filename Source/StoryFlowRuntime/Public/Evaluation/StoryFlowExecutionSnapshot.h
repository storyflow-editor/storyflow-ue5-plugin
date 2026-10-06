// Copyright 2026 StoryFlow. All Rights Reserved.
#pragma once
#include "Evaluation/StoryFlowExecutionContext.h"

/** Detached bounded mutable payload. Authored content/media references are weak. */
struct STORYFLOWRUNTIME_API FStoryFlowExecutionSnapshot
{
	FStoryFlowExecutionContext Context;
	TMap<FString, FStoryFlowVariable> Globals;
	TMap<FString, FStoryFlowCharacterDef> Characters;
	TSet<FString> Once;
	StoryFlowDataAssets::FOverlay Overlay;
	TWeakObjectPtr<UTexture2D> Background, Image, Portrait;
	TWeakObjectPtr<USoundBase> Audio, LoopSound;
	float LoopPosition = 0.0f, LoopVolume = 1.0f;
	TMap<FString, TWeakObjectPtr<UTexture2D>> CachedPortraits;
	StoryFlowDataAssets::FSharedState ResolverShared;
	uint64 Entry = 0;
	int64 Bytes = 0;
	static bool Capture(const FStoryFlowExecutionContext& Context, uint64 Entry, FStoryFlowExecutionSnapshot& Out, FString& Reason);
	bool Prepare(const FStoryFlowExecutionContext& Live, FStoryFlowExecutionSnapshot& Out, FString& Reason) const;
	void Apply(FStoryFlowExecutionContext& Live);
};
