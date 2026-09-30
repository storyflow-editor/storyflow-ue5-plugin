// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowDataAssetAsset.h"

void UStoryFlowDataAssetAsset::PostLoad()
{
	Super::PostLoad();

	// Restore array/map data from serialized blobs (ArrayValue and MapValue are non-UPROPERTY).
	// Both halves of the asset carry variant values: the declarations AND the override blob.
	UnpackVariablesFromSerialization(Variables);
	UnpackVariantsFromSerialization(Overrides);
}

void UStoryFlowDataAssetAsset::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	// Persist array/map data into the serialized blobs before saving
	PackVariablesForSerialization(Variables);
	PackVariantsForSerialization(Overrides);
}
