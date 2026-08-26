// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

STORYFLOWRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogStoryFlow, Log, All);

/** Normalize a script path by stripping the .json extension if present */
inline FString NormalizeScriptPath(const FString& Path)
{
	FString Result = Path;
	Result.RemoveFromEnd(TEXT(".json"));
	return Result;
}

/** Normalize a character path for consistent map lookups (lowercase, forward slashes to backslashes) */
inline FString NormalizeCharacterPath(const FString& Path)
{
	return Path.ToLower().Replace(TEXT("/"), TEXT("\\"));
}

/**
 * True when the string is shaped like a character FILE id, mirroring the editor exporter's
 * isCharacterIdRef (`startsWith('da_')`, case-sensitive — ids preserve case per the V2
 * case rules). Shape only: `.sfd` data-asset ids share the prefix, so whether a `da_`
 * string names a CHARACTER is decided by the character id bridge, never by this test.
 */
inline bool IsCharacterIdRef(const FString& Value)
{
	return Value.StartsWith(TEXT("da_"), ESearchCase::CaseSensitive);
}

/**
 * True when a character-variable access names the Name builtin: the display spelling or the
 * contract-reserved `cf_name` id (P4 contract amendment A1 — character-variable access stays
 * NAME-keyed; the cf_ ids do nothing more than alias the two builtin rows).
 */
inline bool IsCharacterNameBuiltin(const FString& VariableName)
{
	return VariableName.Equals(TEXT("Name"), ESearchCase::IgnoreCase)
		|| VariableName.Equals(TEXT("cf_name"), ESearchCase::IgnoreCase);
}

/** Image twin of IsCharacterNameBuiltin (`Image` | `cf_image`, amendment A1) */
inline bool IsCharacterImageBuiltin(const FString& VariableName)
{
	return VariableName.Equals(TEXT("Image"), ESearchCase::IgnoreCase)
		|| VariableName.Equals(TEXT("cf_image"), ESearchCase::IgnoreCase);
}

class FStoryFlowRuntimeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
