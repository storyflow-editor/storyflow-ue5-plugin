// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowCharacterAsset.h"

void UStoryFlowCharacterAsset::PostLoad()
{
	Super::PostLoad();

	// Restore array data from serialized blob (ArrayValue is non-UPROPERTY)
	UnpackVariablesFromSerialization(Variables);
}

void UStoryFlowCharacterAsset::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	// Persist array data into serialized blob before saving
	PackVariablesForSerialization(Variables);
}
