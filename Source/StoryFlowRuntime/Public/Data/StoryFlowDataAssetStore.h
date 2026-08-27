// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/StoryFlowTypes.h"

class UStoryFlowDataAssetAsset;
class UStoryFlowProjectAsset;

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
	 * Deliberately does NOT resolve string-table keys the way character and global variables do,
	 * and the reason CHANGED with localization spec §2's amendment of 2026-08-27 (which supersedes
	 * engine-contract 2.1's literal-value posture): data-assets.json now DOES carry a strings
	 * table, and a declared .sfd string value is a table key like any other artifact's. It is
	 * still not resolved here, because the seed is the store's read-only half and a bake would
	 * (a) freeze the authored text in whatever language happened to be current at SetProject, and
	 * (b) destroy the one thing the localization gate needs — the difference between a value that
	 * came from the seed and one a script wrote. Resolution happens at the READ DOOR instead: see
	 * TryRead below.
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
	 * FindDeclaration's twin for the BLUEPRINT surface, matching on the display NAME instead of
	 * the id, with the same root-most-wins rule.
	 *
	 * Two lookups exist because two audiences do: everything the exporter emits is keyed by id
	 * (the contract keys the whole system that way, and ids survive a rename), while a Blueprint
	 * author holds an asset reference and the name they typed in the editor. The declaration the
	 * caller gets back carries the id, so a name is resolved to an id exactly once, at the
	 * boundary — nothing downstream of here knows names exist.
	 *
	 * Returned by pointer INTO the seed; see FindDeclaration.
	 */
	STORYFLOWRUNTIME_API const FStoryFlowVariable* FindDeclarationByName(const FSeed& Seed, const FString& AssetId, const FString& VariableName);

	/**
	 * WHAT ANSWERED a resolve — the PROVENANCE of the value, which the localization gate reads and
	 * nothing else does.
	 *
	 * Three values and not two, because "not the declaration" has two different reasons and a
	 * reader who cannot tell them apart re-derives the gate wrongly. The reference implementation
	 * learned this the hard way: its origin enum folded an ancestor's DECLARATION and an
	 * ancestor's OVERRIDE into one `inherited` token, and the accessor door that gated on it
	 * served the ancestor's translation for a text the descendant had deliberately replaced (fixed
	 * editor-side at b18c4de0). The failure is not a missing translation — it is a WRONG VALUE,
	 * invisible in the source language.
	 */
	enum class EResolvedFrom : uint8
	{
		/** The root-most declaration's own authored value (contract §4.3). The only tier that LOCALIZES. */
		Declaration,
		/** An `overrides` entry at some chain level. Authored, but NOT keyed — see TryRead. */
		Override,
		/** An overlay entry: a write this session made. Live data, never content — see TryRead. */
		SessionWrite,
	};

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
	 *
	 * THIS IS THE STORE'S CHAIN RULE AND NOTHING MORE: it answers the value the contract says the
	 * chain holds, with no string-table lookup anywhere in it. Game-facing reads go through
	 * TryRead, which layers the localization gate on top. Saves and the fixture harnesses want
	 * this one — a persisted overlay entry must be the bytes the game wrote.
	 *
	 * `OutResolvedFrom` is optional and reports which tier answered.
	 */
	STORYFLOWRUNTIME_API bool TryResolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue, EResolvedFrom* OutResolvedFrom = nullptr);

	/**
	 * TryResolve's convenience twin: the resolved value, or an UNSET variant when nothing
	 * resolves (contract §4.5 / §9.1 — the fixtures' `"resolved": false`). Callers that must
	 * tell "unset" apart from a legitimately empty value want TryResolve instead.
	 */
	STORYFLOWRUNTIME_API FStoryFlowVariant Resolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId);

	/**
	 * THE READ DOOR: TryResolve plus the localization gate, and the ONE function every surface that
	 * hands a `.sfd` value to game code calls (the script lane's accessor arms, the typed Blueprint
	 * getters, the untyped variant getter). Localization spec §2's amendment of 2026-08-27 —
	 * which SUPERSEDES engine-contract 2.1's "a .sfd value is a literal, never look it up" — makes
	 * a Data Asset's declared string values player-facing prose that ships as stable table keys in
	 * data-assets.json's own `strings.en`, resolving through the very ladder every other artifact's
	 * strings already use (UStoryFlowProjectAsset::GetGlobalString, which the importer merges that
	 * table into beside characters.json's).
	 *
	 * WHAT LOCALIZES, and the three rules that are re-derivable wrongly (manifest
	 * localization.dataAssets in the vendored golden package spells all of them out):
	 *
	 *  - ONLY A DECLARATION. `EResolvedFrom::Override` and `EResolvedFrom::SessionWrite` are handed
	 *    back verbatim. An override is authored but UNKEYED: a `.sfd` id carries no per-asset
	 *    segment, so a declaration and a descendant's override of it would collide on one
	 *    `<variableId>.value`, and the exporter therefore keys declarations only. Localizing an
	 *    override does not MISS — it serves the ancestor's translation for a text the descendant
	 *    replaced.
	 *  - A WRITTEN VALUE NEVER LOCALIZES, including after a save/load, because the save carries the
	 *    overlay and a restored write was never content. The gate is WHERE THE VALUE CAME FROM and
	 *    never whether it LOOKS like a key: a write that happened to equal a key would otherwise be
	 *    translated into a string the game has since redefined, and that failure is invisible in
	 *    the source language.
	 *  - STRING-TYPED PROSE ONLY, decided by the DECLARED type: string scalars, string array
	 *    elements, and the values of a map whose ValueType is String (which defaults to String, so
	 *    an absent valueType is a string map). Enum, image, audio, character, integer, float and
	 *    boolean pass through even when their values are strings. And prose is non-blank: a value
	 *    that is empty or whitespace after trimming is left alone, because no translator can reach
	 *    an id keyed by whitespace.
	 *
	 * THE ID IS BUILT FROM THE VARIABLE ALONE — `<variableId>.value`, `.value.<index>`,
	 * `.value.<mapKey>` — and it is the exporter that built it; nothing here re-derives one. That
	 * is the deliberate CONTRAST with a character value's `<characterId>.<variableId>.value`, and
	 * the reason a chain localizes at every level that declares something: it is the VARIABLE that
	 * is unique, not the asset.
	 *
	 * RESOLUTION IS AT THIS DOOR, not baked into the seed, so a mid-session SetLanguage lands on the
	 * very next `.sfd` read. That is a difference from global and character string variables, whose
	 * initial values this plugin resolves once at seeding (see UStoryFlowSubsystem::SetLanguage's
	 * "what moves, and when") — and it is forced, not chosen: the seed is read-only forever and a
	 * baked value could no longer be told apart from a write.
	 *
	 * `Project` may be null (a store with no project localizes nothing and every value passes
	 * through), and an empty `LanguageCode` reads as the source language, since no language table
	 * is keyed by it and the artifact's own source table answers.
	 */
	STORYFLOWRUNTIME_API bool TryRead(const FStoreRef& Store, const UStoryFlowProjectAsset* Project, const FString& LanguageCode, const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue);

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

	/**
	 * DeclMatches against an accessor node's spawn-time snapshot (contract §6.1). THE gate every
	 * data-asset read and write goes through.
	 *
	 * Node data keeps its types as the wire strings the exporter wrote (the whole evaluator
	 * compares them that way) while the seed's declarations are enums, so this converts through
	 * ParseVariableType (StoryFlowTypes.h) on the way in — the ONE shared table, also the
	 * importer's. An unknown wire type converts to None and therefore never matches a real
	 * declaration, so a garbled snapshot degrades instead of resolving.
	 */
	STORYFLOWRUNTIME_API bool DeclMatchesNodeData(const FStoryFlowVariable& Declaration, const FStoryFlowNodeData& Data);

	/** Drop every session write (game restart / new game). The seed is untouched. */
	STORYFLOWRUNTIME_API void ResetOverlay(FOverlay& Overlay);
}
