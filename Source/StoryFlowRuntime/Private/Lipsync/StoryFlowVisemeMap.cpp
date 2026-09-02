// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowVisemeMap.h"

StoryFlowVisemeTable::FTable UStoryFlowVisemeMap::ToTable() const
{
	if (Poses.Num() == 0)
	{
		return StoryFlowVisemeTable::Default();
	}

	StoryFlowVisemeTable::FTable Table;
	for (const FStoryFlowVisemePose& Pose : Poses)
	{
		if (Pose.Pose.IsNone())
		{
			continue;
		}

		StoryFlowVisemeTable::FPose Morphs;
		for (const FStoryFlowVisemeMorph& Morph : Pose.Morphs)
		{
			if (!Morph.Name.IsNone())
			{
				Morphs.Add(Morph.Name, Morph.Weight);
			}
		}
		Table.Add(Pose.Pose, MoveTemp(Morphs));
	}

	// `rest` is the pose the mouth returns to. An asset that forgets it would leave the driver with nothing
	// to ease toward on a gap.
	if (!Table.Contains(TEXT("rest")))
	{
		Table.Add(TEXT("rest"), StoryFlowVisemeTable::FPose());
	}
	return Table;
}

void UStoryFlowVisemeMap::ResetToDefault()
{
	Poses.Reset();
	for (const TPair<FName, StoryFlowVisemeTable::FPose>& Entry : StoryFlowVisemeTable::Default())
	{
		FStoryFlowVisemePose Pose;
		Pose.Pose = Entry.Key;
		for (const TPair<FName, float>& Morph : Entry.Value)
		{
			FStoryFlowVisemeMorph Mapped;
			Mapped.Name = Morph.Key;
			Mapped.Weight = Morph.Value;
			Pose.Morphs.Add(Mapped);
		}
		Poses.Add(MoveTemp(Pose));
	}
}
