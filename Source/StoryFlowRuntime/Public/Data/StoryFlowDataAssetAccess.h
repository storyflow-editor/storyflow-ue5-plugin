// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"
#include "Data/StoryFlowDataAssetStore.h"

class UStoryFlowSubsystem;
class UStoryFlowDataAssetAsset;

/**
 * THE ONE `.sfd` HOST LADDER, shared by UStoryFlowComponent and UStoryFlowSubsystem.
 *
 * Both public surfaces read and write Data Assets, and before this existed only the component
 * could: the whole ladder - the declaration walk, the scalar type gate, the P4 character branch,
 * the store write - lived on it, so a Blueprint with no component object (a pause menu, an
 * inventory screen, a save-slot list) had no way in. Copying it onto the subsystem would have been
 * two ladders answering one question, which the engine contract names as the thing to avoid for
 * mirrored surfaces ("both public mirrors inherit it and cannot diverge"). It moved here instead,
 * and both surfaces are thin.
 *
 * Every operation takes the SUBSYSTEM, which owns everything the ladder consults - the seed, the
 * overlay, the runtime characters and the id bridge, the project - and reads take the LANGUAGE
 * the caller is reading in, because the two surfaces can differ there: the component falls back
 * to its own pre-localization `LanguageCode` export on a project with no sidecar, the subsystem
 * to its current language. A localized project ignores the difference.
 *
 * NO CACHE CLEAR HERE. A write's evaluator-cache drop belongs to whoever owns an evaluator, and
 * the subsystem owns none; each setter returns whether the write landed and the component drops
 * its cache on success. That is the whole of what the component still does itself.
 *
 * A character's display NAME resolves through the project's global strings in the caller's
 * language. The component used to route that through its execution context during a dialogue,
 * which adds the running script's own table - but a name is keyed in characters.json and merged
 * into the globals, so the script tier could only ever shadow it and never answered differently.
 * Resolving at project level on both surfaces is the same answer with one fewer place to diverge.
 */
namespace StoryFlowDataAssetAccess
{
	/** The typed getters' shared tail: gate on the declaration, then resolve through the chain. */
	STORYFLOWRUNTIME_API bool TryGetScalar(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, const FString& LanguageCode, FStoryFlowVariant& OutValue);

	/** The typed setters' shared tail: gate, then write at the asset's own level. True when it landed. */
	STORYFLOWRUNTIME_API bool SetScalar(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, const FStoryFlowVariant& Value);

	/** The untyped door: any declared type, map storage detached. */
	STORYFLOWRUNTIME_API bool TryGetVariant(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const FString& LanguageCode, FStoryFlowVariant& OutValue);

	/** Every name the asset's chain declares, root-most first - the Get Variable Names node's walk. */
	STORYFLOWRUNTIME_API TArray<FString> VariableNames(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset);

	/** Replace an ARRAY declaration's elements, shape-gated. True when the write landed. */
	STORYFLOWRUNTIME_API bool SetArray(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const TArray<FStoryFlowVariant>& Elements);

	/** Replace a MAP declaration's entries from parallel key/value lists, shape-gated. True when it landed. */
	STORYFLOWRUNTIME_API bool SetMap(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const TArray<FStoryFlowVariant>& Keys, const TArray<FStoryFlowVariant>& Values);
}
