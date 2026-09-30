// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Lipsync/StoryFlowVisemeTable.h"

namespace StoryFlowVisemeTable
{
	const TArray<FName>& Axis()
	{
		static const TArray<FName> Value = { TEXT("OO"), TEXT("OH"), TEXT("AA"), TEXT("EE") };
		return Value;
	}

	const TArray<FName>& PoseNames()
	{
		static const TArray<FName> Value = {
			TEXT("rest"), TEXT("AA"), TEXT("EE"), TEXT("IH"), TEXT("OH"),
			TEXT("OO"), TEXT("MM"), TEXT("FF"), TEXT("TH"), TEXT("L")
		};
		return Value;
	}

	FTable Default()
	{
		FTable Table;

		Table.Add(TEXT("rest"), FPose());

		Table.Add(TEXT("AA"), FPose{
			{ TEXT("jawOpen"), 0.85f },
			{ TEXT("mouthLowerDownLeft"), 0.32f }, { TEXT("mouthLowerDownRight"), 0.32f } });

		Table.Add(TEXT("EE"), FPose{
			{ TEXT("jawOpen"), 0.28f },
			{ TEXT("mouthStretchLeft"), 0.78f }, { TEXT("mouthStretchRight"), 0.78f },
			{ TEXT("mouthSmileLeft"), 0.32f }, { TEXT("mouthSmileRight"), 0.32f } });

		Table.Add(TEXT("IH"), FPose{
			{ TEXT("jawOpen"), 0.36f },
			{ TEXT("mouthStretchLeft"), 0.44f }, { TEXT("mouthStretchRight"), 0.44f } });

		Table.Add(TEXT("OH"), FPose{
			{ TEXT("jawOpen"), 0.62f },
			{ TEXT("mouthFunnel"), 0.72f }, { TEXT("mouthPucker"), 0.32f } });

		Table.Add(TEXT("OO"), FPose{
			{ TEXT("jawOpen"), 0.20f },
			{ TEXT("mouthPucker"), 0.72f }, { TEXT("mouthFunnel"), 0.38f } });

		Table.Add(TEXT("MM"), FPose{
			{ TEXT("mouthClose"), 0.68f },
			{ TEXT("mouthPressLeft"), 0.52f }, { TEXT("mouthPressRight"), 0.52f } });

		Table.Add(TEXT("FF"), FPose{
			{ TEXT("jawOpen"), 0.20f }, { TEXT("mouthRollLower"), 0.72f },
			{ TEXT("mouthUpperUpLeft"), 0.38f }, { TEXT("mouthUpperUpRight"), 0.38f } });

		Table.Add(TEXT("TH"), FPose{
			{ TEXT("jawOpen"), 0.44f }, { TEXT("tongueOut"), 0.66f }, { TEXT("tongueUp"), 0.28f } });

		Table.Add(TEXT("L"), FPose{
			{ TEXT("jawOpen"), 0.52f }, { TEXT("tongueUp"), 0.82f }, { TEXT("tongueRaise"), 0.58f } });

		return Table;
	}

	TSet<FName> OwnedMorphs(const FTable& Table)
	{
		TSet<FName> Owned;
		for (const TPair<FName, FPose>& Pose : Table)
		{
			for (const TPair<FName, float>& Morph : Pose.Value)
			{
				Owned.Add(Morph.Key);
			}
		}
		return Owned;
	}
}
