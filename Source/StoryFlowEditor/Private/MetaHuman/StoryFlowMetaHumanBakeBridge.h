// Copyright 2026 StoryFlow. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class USkeletalMesh;
class USoundWave;

/** Optional editor-only access to MetaHuman Animator's reflected API. No Epic module is linked. */
namespace StoryFlowMetaHumanBakeBridge
{
	FString UnavailableReason();
	/** Loads the enabled backend and validates the API used by this bridge without starting a solve. */
	bool ValidateSchema(FString& Error);
	UObject* CreatePerformance(UObject* Outer, USoundWave* Voice, USkeletalMesh* Face,
		UObject* Listener, FName FinishedFunction, FString& Error);
	bool Start(UObject* Performance, FString& Error);
	bool IsProcessing(UObject* Performance);
	bool ContainsAnimation(UObject* Performance);
	void Cancel(UObject* Performance);
	void Unbind(UObject* Performance, UObject* Listener, FName FinishedFunction);
	UAnimSequence* Export(UObject* Performance, USkeletalMesh* Face, const FString& PackagePath,
		const FString& AssetName, FString& Error);
}
