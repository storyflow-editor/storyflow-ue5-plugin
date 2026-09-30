// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/ObjectSaveContext.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowDataAssetAsset.generated.h"

/**
 * DataAsset containing a single StoryFlow Data Asset (.sfd) level of the exported SEED.
 *
 * One asset per entry of the export's data-assets.json (engine contract §2.1). The seed is
 * LINEAGE-LIVE: every level keeps its own declarations and its own overrides plus a link to
 * its parent, and values are NEVER flattened onto the leaf — flattening kills the
 * set-on-base cascade the resolver exists for (contract §4, normative implementation:
 * runtime-data-assets.js resolveEntry).
 *
 * The seed is TRUSTED (contract §2.1): the editor's export collector already stripped orphan
 * and stale overrides and collapsed duplicate map keys. Nothing here re-validates it.
 *
 * The asset name is the assetId, not the display name: ids are unique and stable across
 * re-imports, display names are neither (two .sfd files in different folders can share a
 * filename base, and the seed carries no path). Name below is what tooling shows.
 */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowDataAssetAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Stable da_<32 hex> id — the key the whole contract is keyed by */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString AssetId;

	/** Display name (the .sfd filename base). May be empty in an export made before it existed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Parent asset id. Empty means this is a root asset. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Parent;

	/** Declarations in the seed's authored order, each with its own declared default */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowVariable> Variables;

	/** This level's file overrides, keyed by variable id */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariant> Overrides;

#if WITH_EDITORONLY_DATA
	/** Hash of the StoryFlow source this asset was last successfully imported
	    and saved from; lets sync skip rewriting unchanged assets. Cleared when
	    a save fails so the next sync retries. */
	UPROPERTY()
	FString ImportedSourceHash;
#endif

	/** Unpack array/map variable data after loading from disk */
	virtual void PostLoad() override;

	/** Pack array/map variable data before saving */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
};
