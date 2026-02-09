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

FStoryFlowVariable UStoryFlowProjectAsset::GetGlobalVariable(const FString& VariableId) const
{
	if (const FStoryFlowVariable* Variable = GlobalVariables.Find(VariableId))
	{
		return *Variable;
	}
	return FStoryFlowVariable();
}

bool UStoryFlowProjectAsset::HasGlobalVariable(const FString& VariableId) const
{
	return GlobalVariables.Contains(VariableId);
}

UStoryFlowCharacterAsset* UStoryFlowProjectAsset::GetCharacterAsset(const FString& CharacterPath) const
{
	if (UStoryFlowCharacterAsset* const* CharAsset = Characters.Find(CharacterPath))
	{
		return *CharAsset;
	}
	return nullptr;
}

FString UStoryFlowProjectAsset::GetGlobalString(const FString& Key, const FString& LanguageCode) const
{
	// Lookup with language prefix
	const FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *Key);
	if (const FString* Value = GlobalStrings.Find(FullKey))
	{
		return *Value;
	}

	// Fallback to key without prefix
	if (const FString* Value = GlobalStrings.Find(Key))
	{
		return *Value;
	}

	return Key;
}
