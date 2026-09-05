// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowVisemeMap.h"

#include "StoryFlowRuntime.h"

StoryFlowVisemeTable::FTable UStoryFlowVisemeMap::ToTable() const
{
	if (Poses.Num() == 0)
	{
		return StoryFlowVisemeTable::Default();
	}

	// A typo'd pose name and a missing axis pose both fail the same way without this: a mouth that is half
	// dead, or stuck at one end of the vowel axis, with nothing anywhere saying why. Once per asset, because
	// ToTable runs on every component that references it.
	TArray<FString> Unknown;

	StoryFlowVisemeTable::FTable Table;
	for (const FStoryFlowVisemePose& Pose : Poses)
	{
		if (Pose.Pose.IsNone())
		{
			continue;
		}

		if (!StoryFlowVisemeTable::PoseNames().Contains(Pose.Pose))
		{
			Unknown.AddUnique(Pose.Pose.ToString());
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

	if (!bValidated)
	{
		bValidated = true;

		if (Unknown.Num() > 0)
		{
			UE_LOG(LogStoryFlow, Warning,
				TEXT("StoryFlow: viseme map '%s' names %s, which no pose on the vowel axis is called. ")
				TEXT("Speech never reaches those entries; only the idle mouth does. The pose names are %s."),
				*GetName(), *FString::Join(Unknown, TEXT(", ")),
				*FString::JoinBy(StoryFlowVisemeTable::PoseNames(), TEXT(", "), [](const FName& Name) { return Name.ToString(); }));
		}

		// The axis is what the vowel blend interpolates along. A map missing one of its four leaves the
		// mouth pinned at whichever end survived, which reads as a driver bug rather than a missing entry.
		TArray<FString> MissingAxis;
		for (const FName& Pose : StoryFlowVisemeTable::Axis())
		{
			if (!Table.Contains(Pose))
			{
				MissingAxis.Add(Pose.ToString());
			}
		}
		if (MissingAxis.Num() > 0)
		{
			UE_LOG(LogStoryFlow, Warning,
				TEXT("StoryFlow: viseme map '%s' has no %s. Those are the vowel axis, so the mouth cannot ")
				TEXT("reach that end of it."),
				*GetName(), *FString::Join(MissingAxis, TEXT(", ")));
		}
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
