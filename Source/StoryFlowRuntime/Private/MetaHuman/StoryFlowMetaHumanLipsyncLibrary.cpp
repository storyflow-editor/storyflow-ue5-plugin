// Copyright 2026 StoryFlow. All Rights Reserved.

#include "MetaHuman/StoryFlowMetaHumanLipsyncLibrary.h"
#include "Animation/AnimSequence.h"
#include "Sound/SoundWave.h"
#include "Engine/SkeletalMesh.h"

UAnimSequence* UStoryFlowMetaHumanLipsyncLibrary::FindAnimation(USoundWave* Voice, USkeletalMesh* FaceMesh) const
{
	for (const auto& Entry : Entries)
		if (Entry.Voice == Voice && Entry.FaceMesh == FaceMesh && !Entry.bStale)
			return Entry.Animation;
	return nullptr;
}

FStoryFlowMetaHumanBakeEntry* UStoryFlowMetaHumanLipsyncLibrary::FindEntry(USoundWave* Voice, USkeletalMesh* FaceMesh)
{
	return Entries.FindByPredicate([=](const auto& Entry) { return Entry.Voice == Voice && Entry.FaceMesh == FaceMesh; });
}
