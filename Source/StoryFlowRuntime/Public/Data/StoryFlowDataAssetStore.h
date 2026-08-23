// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"

class UStoryFlowDataAssetAsset;

/**
 * The .sfd Data Asset STORE (engine contract §3) and its chain RESOLVER (§4).
 *
 * NORMATIVE SOURCE: the HTML runtime's src/renderer/runtime/runtime-data-assets.js — its
 * resolveEntry() is the chain walk every function here mirrors, isDeclaredOnChain() the
 * write guard, declaration() the root-most declaration lookup. Where this file and that one
 * disagree, that one is right. The shared golden fixtures in
 * TestContent/engine-contract/data-assets-*.json are generated from it and pin the agreement
 * (StoryFlow.DataAssets.Resolution).
 *
 * The store is two halves:
 *  - the SEED: the exported table, keyed by assetId. Read-only, forever — contract §3 says
 *    "the seed is never mutated by anything, ever". Owned by UStoryFlowSubsystem.
 *  - the OVERLAY: this session's script writes, keyed (assetId -> variableId -> value).
 *    Cleared by a game reset, persisted sparsely in saves (§7). Also subsystem-owned.
 *
 * Free functions over both, rather than a class holding them, so the execution context can
 * point at the subsystem-owned maps (the established ExternalGlobalVariables idiom) and so
 * tests can drive the resolver against a seed built straight from the fixture JSON.
 */
namespace StoryFlowDataAssets
{
	/** assetId -> seed level. */
	using FSeed = TMap<FString, FStoryFlowDataAssetDef>;

	/**
	 * assetId -> (variableId -> value). Deliberately NOT a UPROPERTY anywhere it is stored:
	 * UHT rejects a nested TMap, and there is nothing to keep alive here — FStoryFlowVariant
	 * holds no UObject references.
	 */
	using FOverlay = TMap<FString, TMap<FString, FStoryFlowVariant>>;

	/**
	 * The two halves of the store as ONE non-owning reference, so callers that hold the pair
	 * (the execution context, and every signature that threads it) cannot end up with a seed
	 * from one owner and an overlay from another. Both point at UStoryFlowSubsystem-owned maps
	 * and share its GameInstance lifetime; a default-constructed ref is the "no store" state
	 * every accessor null-checks.
	 *
	 * The seed is const because contract §3 says it is never mutated by anything, ever — the
	 * const is that sentence, enforced.
	 */
	struct FStoreRef
	{
		const FSeed* Seed = nullptr;
		FOverlay* Overlay = nullptr;

		bool IsValid() const { return Seed != nullptr && Overlay != nullptr; }
	};

	/**
	 * Chain depth cap, matching the reference implementation's MAX_DEPTH (contract §4.4):
	 * the walk admits MAX_DEPTH ancestors PLUS the starting level, so 65 levels are visited
	 * before a malformed chain is abandoned. A cycle is caught earlier by the visited set.
	 */
	constexpr int32 MaxChainDepth = 64;

	/**
	 * Build the runtime seed from a project's imported Data Assets (contract §3 init).
	 * Replaces OutSeed wholesale; the caller clears the overlay, because a fresh seed is a
	 * fresh session and the two belong together.
	 *
	 * Map storage is DETACHED from the .uasset on the way out (the boundary
	 * ResetRuntimeCharacters draws): the seed is never written to, but a read hands map
	 * entries out and nothing downstream should reach the asset's storage through them.
	 *
	 * Deliberately does NOT resolve string-table keys the way character and global variables
	 * do: data-assets.json carries no strings table (json-export-strategy.ts exportDataAssets
	 * writes values verbatim), so a .sfd string value is a literal, and running the lookup
	 * over it would replace every literal with a failed lookup.
	 */
	STORYFLOWRUNTIME_API void BuildSeed(const TMap<FString, UStoryFlowDataAssetAsset*>& Assets, FSeed& OutSeed);

	/** Is `AssetId` carried by the seed at all? Tells a dead reference apart from a stale binding. */
	STORYFLOWRUNTIME_API bool HasAsset(const FSeed& Seed, const FString& AssetId);

	/**
	 * The declaration `VariableId` resolves to on `AssetId`'s chain, or null when the asset is
	 * unknown or no level declares the id. ROOT-MOST declaration wins (contract §4.3): a
	 * descendant re-declaring an inherited id does not shadow the ancestor's definition.
	 * Mirrors runtime-data-assets.js `declaration`.
	 *
	 * Returned by pointer INTO the seed — read it, never write through it, and do not hold it
	 * across a re-seed.
	 */
	STORYFLOWRUNTIME_API const FStoryFlowVariable* FindDeclaration(const FSeed& Seed, const FString& AssetId, const FString& VariableId);

	/**
	 * Effective value of `VariableId` as seen by `AssetId` (contract §4), mirroring
	 * runtime-data-assets.js resolveEntry. Walks leaf -> root taking, per level and in order,
	 * the overlay entry, else that level's override; first hit wins, and an ancestor's entry
	 * therefore cascades down to every descendant that does not shadow it. With no such hit
	 * the ROOT-MOST declaration's own declared value answers.
	 *
	 * A value is honored only when SOME level of the chain DECLARES the id — an orphan
	 * override left behind by a deleted base variable resolves to nothing, exactly as the
	 * editor's resolver sees it.
	 *
	 * Returns false (leaving OutValue untouched) for an unknown asset or an id nothing
	 * declares. The value is COPIED OUT with detached map storage (contract §3: graph code
	 * must not be able to mutate the seed or the overlay through a read).
	 */
	STORYFLOWRUNTIME_API bool TryResolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue);

	/**
	 * TryResolve's convenience twin: the resolved value, or an UNSET variant when nothing
	 * resolves (contract §4.5 / §9.1 — the fixtures' `"resolved": false`). Callers that must
	 * tell "unset" apart from a legitimately empty value want TryResolve instead.
	 */
	STORYFLOWRUNTIME_API FStoryFlowVariant Resolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId);

	/** True when any level of `AssetId`'s chain declares `VariableId` (what Set validates against). */
	STORYFLOWRUNTIME_API bool IsDeclaredOnChain(const FSeed& Seed, const FString& AssetId, const FString& VariableId);

	/**
	 * Record a session write in the overlay (contract §5), reporting whether it landed. Named
	 * for the house Find/Try rule rather than after the reference implementation, whose
	 * `set()` returns nothing and warns inline. Writes land at THE REFERENCED
	 * ASSET'S OWN LEVEL, always — never at the declaring ancestor: setting via a child
	 * overrides for that child's subtree, setting via the base cascades to every descendant
	 * that does not shadow it. There is no "write to base" switch.
	 *
	 * Refuses (returning false, no write) an unknown asset or an id no chain level declares.
	 * Map writes REPLACE the whole value, and the value is deep-copied on the way in so a
	 * caller mutating its container afterwards cannot reach into the store.
	 *
	 * The caller owns the warning: the node arms have a per-node warn latch (contract §6) and
	 * the Blueprint surface does not, so this reports the refusal rather than logging it.
	 */
	STORYFLOWRUNTIME_API bool TrySet(const FSeed& Seed, FOverlay& Overlay, const FString& AssetId, const FString& VariableId, const FStoryFlowVariant& Value);

	/**
	 * Does the seed's declaration still match the spawn-time snapshot an accessor node's pins
	 * were built from (contract §6.1, mirroring runtime-data-assets.js declMatches)? Stale is
	 * treated as MISSING — no silent coercion, ever, because within the string family a value
	 * carries no evidence of its declared type.
	 *
	 * KeyType/ValueType are compared for maps only; bIsArray always, since an array pin and a
	 * scalar pin of the same type are different pins.
	 */
	STORYFLOWRUNTIME_API bool DeclMatches(const FStoryFlowVariable& Declaration, EStoryFlowVariableType VariableType, bool bIsArray, EStoryFlowVariableType KeyType, EStoryFlowVariableType ValueType);

	/** Drop every session write (game restart / new game). The seed is untouched. */
	STORYFLOWRUNTIME_API void ResetOverlay(FOverlay& Overlay);
}
