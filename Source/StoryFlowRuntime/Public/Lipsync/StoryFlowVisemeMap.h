// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Lipsync/StoryFlowVisemeTable.h"
#include "StoryFlowVisemeMap.generated.h"

/** One morph a pose drives, and how far. */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowVisemeMorph
{
	GENERATED_BODY()

	/** The morph target's name on the rig, e.g. `jawOpen`. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	FName Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Weight = 1.0f;
};

/** One mouth pose: the morphs it drives, and how far each goes at full strength. */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowVisemePose
{
	GENERATED_BODY()

	/** One of the pose names in StoryFlowVisemeTable::PoseNames(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	FName Pose;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TArray<FStoryFlowVisemeMorph> Morphs;
};

/**
 * A rig's viseme mapping. OPTIONAL: a component with none uses the built-in table, which is ARKit-named and
 * tuned on Synty Sidekick, and is what most projects want.
 *
 * ARKit's 52 face blendshape names are the de facto interchange format — MetaHumans (via the Live Link Face
 * curve path), VRM, Ready Player Me and Character Creator all use them — so the built-in table already fits
 * far more rigs than Synty's. Make an asset only to retarget onto a face that names its shapes differently.
 *
 * The schema is deliberately flat: one morph and one weight per entry, no per-shape attack/decay curves. The
 * driver's global smoothing is what sells the motion, and per-shape curves would be four more numbers per
 * pose for a user to get wrong. See the mapping-asset schema in LIPSYNC_DESIGN.md.
 */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowVisemeMap : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StoryFlow|Lipsync")
	TArray<FStoryFlowVisemePose> Poses;

	/** The table in the driver's shape. An empty asset answers the built-in table, not an empty mouth. */
	StoryFlowVisemeTable::FTable ToTable() const;

	/** Fill the asset with the built-in table, so editing starts from the tuned numbers. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "StoryFlow|Lipsync")
	void ResetToDefault();
};
