// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The ten mouth poses, as ARKit morph weights in 0..1.
 *
 * ARKit's face blendshape names are the de facto interchange format for facial animation, so this table
 * drives MetaHumans (through the Live Link Face curve path), VRM and Ready Player Me avatars as readily as
 * the Synty Sidekick rigs it was tuned on. Nothing here is Synty-specific.
 *
 * These weights are NOT guesses. They are the tuned table from the three.js build that already runs against
 * these rigs, and they are pinned by the normative driver spec in LIPSYNC_DESIGN.md. The Unity arm ships the
 * same numbers. Changing one here without changing it there is exactly the engine-arm drift the sequencing
 * is meant to prevent: change the spec first, then both arms.
 */
namespace StoryFlowVisemeTable
{
	/** A single pose: morph name to weight in 0..1. */
	using FPose = TMap<FName, float>;

	/** The whole table: pose name to pose. */
	using FTable = TMap<FName, FPose>;

	/** The poses the vowel axis interpolates between, low centroid to high. Order is load-bearing. */
	STORYFLOWRUNTIME_API const TArray<FName>& Axis();

	/** Every pose name, `rest` included. `rest` is empty by design: it is the absence of a pose. */
	STORYFLOWRUNTIME_API const TArray<FName>& PoseNames();

	/**
	 * The default table.
	 *
	 * TH and L reach for `tongueUp` and `tongueRaise`, which are Synty EXTENSIONS — ARKit itself has only
	 * `tongueOut` — so a plain ARKit rig resolves neither, drops them with one warning, and keeps the jaw and
	 * lip half of the pose. That is the usual tongue-less approximation and is invisible at game camera
	 * distance. A missing tongue morph must never take the jaw down with it.
	 *
	 * A rig can override all of this with a mapping asset, but this is what ships, and it is what the
	 * component falls back to when no asset is assigned: a component with nothing configured still moves a
	 * mouth rather than doing nothing and looking broken.
	 */
	STORYFLOWRUNTIME_API FTable Default();

	/**
	 * Every morph name any pose in `Table` can touch. This is the "owned set": the driver writes these and
	 * NOTHING else, so a Sidekick character's identity morphs (defaultBuff, defaultSkinny, defaultHeavy,
	 * masculineFeminine) survive. Those are body type, set once at character creation and held, and a driver
	 * that zeroed every weight it found would silently flatten the character back to the base body. Unreal is
	 * where this bites: unlike Unity's baked character, the part meshes DO carry them.
	 */
	STORYFLOWRUNTIME_API TSet<FName> OwnedMorphs(const FTable& Table);
}
