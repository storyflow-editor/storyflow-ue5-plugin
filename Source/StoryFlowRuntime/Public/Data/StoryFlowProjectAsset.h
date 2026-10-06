// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/ObjectSaveContext.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowProjectAsset.generated.h"

class UStoryFlowScriptAsset;
class UStoryFlowCharacterAsset;
class UStoryFlowDataAssetAsset;

/**
 * DataAsset containing a StoryFlow project with all its scripts
 */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowProjectAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** StoryFlow editor version */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Version;

	/** Export format version */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString ApiVersion;

	/** Project metadata */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowProjectMetadata Metadata;

	/** Entry point script path */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString StartupScript;

	/** All scripts in the project, keyed by normalized path */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, UStoryFlowScriptAsset*> Scripts;

	/** Global variables (shared across scripts) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariable> GlobalVariables;

	/** Characters (each is a separate DataAsset) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, UStoryFlowCharacterAsset*> Characters;

	/**
	 * Character FILE id (`da_`) -> the Characters map's key, from character-index.json
	 * (P4 contract §1.4). Values are stored VERBATIM: the export guarantees they are the
	 * exact NormalizeCharacterPath shape (lowercase, backslashes), so lookups need no
	 * re-normalization. Empty on pre-P4 exports — ids then resolve nothing and the path
	 * fields stay authoritative.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FString> CharacterIdToPath;

	/**
	 * Data Assets (.sfd) the project's scripts reference, PLUS their full ancestor chains,
	 * keyed by assetId (engine contract §2.1). Keyed by id and not by path because that is
	 * what the whole contract is keyed by — pills bind ids, the resolver walks ids, saves
	 * persist ids. Each is a separate DataAsset.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, UStoryFlowDataAssetAsset*> DataAssets;

	/** data-assets.json localization contract. Legacy imports localize declarations only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	int32 DataAssetLocalizationVersion = 1;

	/** Global string table */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FString> GlobalStrings;

	/**
	 * THE FILE-PRESENCE MARKER (localization spec §9): true when this project was imported from a
	 * build that carried a `localization.json` beside its artifacts.
	 *
	 * A bool and not a "are there any tables" test ON PURPOSE. An absent sidecar and a sidecar
	 * carrying no rows are the same empty TMap in C++, and only one of them is a pre-localization
	 * export. The contract branches on the FILE EXISTING, never on a key count: an author who
	 * registered a language and translated nothing still ships full tables of source text, and
	 * that is a localized project. False here means source-only and ZERO behavior change — every
	 * lookup below falls straight through to the artifact tables it always used.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bHasLocalization = false;

	/**
	 * The language the documents are AUTHORED in, and therefore the language the artifacts' own
	 * `strings` blocks hold — the second tier of the lookup. "en" without a sidecar, which is
	 * exactly what every pre-localization export's `strings` block is keyed by.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString SourceLanguage = TEXT("en");

	/** The project's TARGET languages, in the author's registry order. Never includes the source. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowLanguage> Languages;

	/**
	 * `<language code>` -> that language's FULL, PRE-RESOLVED table, straight from the sidecar.
	 *
	 * Full and pre-resolved is the whole engine contract (§9): the export already applied every
	 * fallback rule — an outdated row ships the OLD translation, an untranslated or cleared one
	 * ships the source text, an orphan has no row at all — so this plugin computes NO status,
	 * compares NO hash, and holds no rule beyond the three-step lookup in GetGlobalString.
	 *
	 * These ids are the ids that KEYED an engine artifact. `.sfui` widget and dropdown strings
	 * have no rows here and never will: `.sfui` documents do not reach a plugin at all and their
	 * text localizes in the HTML lane. Their absence is the contract, not a missing feature.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowStringTable> LanguageStrings;

	/** Resolved Unreal asset references */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, TSoftObjectPtr<UObject>> ResolvedAssets;

#if WITH_EDITORONLY_DATA
	/** Hash of the StoryFlow source this asset was last successfully imported
	    and saved from; lets sync skip rewriting unchanged assets. Cleared when
	    a save fails so the next sync retries. */
	UPROPERTY()
	FString ImportedSourceHash;
#endif

public:
	/** Get the startup script asset */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowScriptAsset* GetStartupScriptAsset() const;

	/** Get a script by path */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowScriptAsset* GetScriptByPath(const FString& ScriptPath) const;

	/** Get a global variable by name */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FStoryFlowVariable GetGlobalVariable(const FString& VariableName) const;

	/** Check if a global variable exists by name */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool HasGlobalVariable(const FString& VariableName) const;

	/** Get a character asset by path */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowCharacterAsset* GetCharacterAsset(const FString& CharacterPath) const;

	/** Get a global string */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetGlobalString(const FString& Key, const FString& LanguageCode = TEXT("en")) const;

	/**
	 * THE OVERLAY TIER (localization spec §9), and the first step of every string lookup in this
	 * plugin: the sidecar's row for `Key` in `LanguageCode`, or null when there is none.
	 *
	 * Null covers all four ways a row can be absent — no sidecar, an unknown code, the SOURCE
	 * language (which has no table by construction), and an id this table does not carry — and
	 * every one of them means the same thing to a caller: fall through to the artifact's own
	 * source table. An EMPTY row reads as absent too: the export already turned a cleared
	 * translation back into source text, and this is the second net for a hand-edited sidecar.
	 */
	const FString* FindLocalizedString(const FString& Key, const FString& LanguageCode) const;

	/**
	 * The code this project actually carries that matches `Code`, or EMPTY when it carries none.
	 *
	 * Case-INSENSITIVE with the registered casing winning, matching the HTML runtime's
	 * resolveLanguage and the store's own rule that a code IS a file name: "ES" and "es" are one
	 * language, and the canonical form is the one the tables are keyed by. The SOURCE language
	 * matches too — running in the authored language is a legitimate choice, it simply has no
	 * table. A project with no sidecar therefore matches its source language and nothing else.
	 */
	FString ResolveLanguageCode(const FString& Code) const;

	/** Unpack array variables after loading from disk */
	virtual void PostLoad() override;

	/** Pack array variables before saving */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
};
