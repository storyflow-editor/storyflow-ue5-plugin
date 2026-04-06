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
