// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "StoryFlowRuntime.h"

UStoryFlowScriptAsset* UStoryFlowProjectAsset::GetStartupScriptAsset() const
{
	if (StartupScript.IsEmpty())
	{
		return nullptr;
	}

	return GetScriptByPath(StartupScript);
}

UStoryFlowScriptAsset* UStoryFlowProjectAsset::GetScriptByPath(const FString& ScriptPath) const
{
	FString NormalizedPath = NormalizeScriptPath(ScriptPath);
	if (UStoryFlowScriptAsset* const* Script = Scripts.Find(NormalizedPath))
	{
		return *Script;
	}

	return nullptr;
}

FStoryFlowVariable UStoryFlowProjectAsset::GetGlobalVariable(const FString& VariableName) const
{
	for (const auto& Pair : GlobalVariables)
	{
		if (Pair.Value.Name == VariableName)
		{
			return Pair.Value;
		}
	}
	return FStoryFlowVariable();
}

bool UStoryFlowProjectAsset::HasGlobalVariable(const FString& VariableName) const
{
	for (const auto& Pair : GlobalVariables)
	{
		if (Pair.Value.Name == VariableName)
		{
			return true;
		}
	}
	return false;
}

UStoryFlowCharacterAsset* UStoryFlowProjectAsset::GetCharacterAsset(const FString& CharacterPath) const
{
	if (UStoryFlowCharacterAsset* const* CharAsset = Characters.Find(CharacterPath))
	{
		return *CharAsset;
	}
	return nullptr;
}

void UStoryFlowProjectAsset::PostLoad()
{
	Super::PostLoad();

	// Restore array data from serialized blob (ArrayValue is non-UPROPERTY)
	UnpackVariablesFromSerialization(GlobalVariables);
}

void UStoryFlowProjectAsset::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	// Persist array data into serialized blob before saving
	PackVariablesForSerialization(GlobalVariables);
}

const FString* UStoryFlowProjectAsset::FindLocalizedString(const FString& Key, const FString& LanguageCode) const
{
	const FStoryFlowStringTable* Table = LanguageStrings.Find(LanguageCode);
	if (!Table)
	{
		return nullptr;
	}
	const FString* Text = Table->Entries.Find(Key);
	return (Text && !Text->IsEmpty()) ? Text : nullptr;
}

FString UStoryFlowProjectAsset::ResolveLanguageCode(const FString& Code) const
{
	if (Code.IsEmpty())
	{
		return FString();
	}
	if (!SourceLanguage.IsEmpty() && SourceLanguage.Equals(Code, ESearchCase::IgnoreCase))
	{
		return SourceLanguage;
	}
	for (const FStoryFlowLanguage& Language : Languages)
	{
		if (Language.Code.Equals(Code, ESearchCase::IgnoreCase))
		{
			return Language.Code;
		}
	}
	return FString();
}

FString UStoryFlowProjectAsset::GetGlobalString(const FString& Key, const FString& LanguageCode) const
{
	// TIER 1, the localization overlay (spec §9): the sidecar's row for this id in the language
	// being read. Absent for a pre-localization export, for the source language and for an id
	// the sidecar does not carry — all of which fall through to the artifact's own table below.
	if (const FString* Localized = FindLocalizedString(Key, LanguageCode))
	{
		return *Localized;
	}

	// TIER 2, the artifact's own table. The language-prefixed probe comes first and is the
	// PRE-LOCALIZATION behavior, kept exactly as it was: an artifact `strings` block may itself
	// carry more than one language block, and the importer flattens each to `<code>.<key>`.
	const FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *Key);
	if (const FString* Value = GlobalStrings.Find(FullKey))
	{
		return *Value;
	}

	// Still tier 2, and the step the sidecar makes necessary: the SOURCE table. Every export this
	// editor writes keys its artifact strings by the source language alone, so once the language
	// being read is a target language the probe above cannot hit and this is the fall-through the
	// contract names ("-> the keying artifact's own strings.en"). Identical to the probe above
	// whenever the two codes agree, which is every pre-localization project.
	const FString SourceKey = FString::Printf(TEXT("%s.%s"), *SourceLanguage, *Key);
	if (const FString* Value = GlobalStrings.Find(SourceKey))
	{
		return *Value;
	}

	// Fallback to key without prefix
	if (const FString* Value = GlobalStrings.Find(Key))
	{
		return *Value;
	}

	// TIER 3: the raw stored value. NEVER an empty string by accident and never a lookup failure
	// a caller has to test for — a value that never keyed a table is its own text.
	return Key;
}
