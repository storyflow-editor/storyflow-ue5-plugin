// Copyright 2026 StoryFlow. All Rights Reserved.

#include "SourceControl/StoryFlowSourceControlUtils.h"
#include "SourceControlHelpers.h"
#include "Misc/Paths.h"

namespace
{
	TOptional<StoryFlowSourceControl::FTestOverrides> GTestOverrides;
}

namespace StoryFlowSourceControl
{

void SetTestOverrides(const TOptional<FTestOverrides>& Overrides)
{
	GTestOverrides = Overrides;
}

bool EnsureWritable(const FString& Filename, FString& OutError)
{
	if (GTestOverrides.IsSet() && GTestOverrides->EnsureWritable)
	{
		return GTestOverrides->EnsureWritable(Filename, OutError);
	}
	if (!USourceControlHelpers::IsEnabled())
	{
		return true;
	}
	if (!FPaths::FileExists(Filename))
	{
		return true; // brand-new file: nothing to check out
	}
	FSourceControlState State = USourceControlHelpers::QueryFileState(Filename, /*bSilent*/ true);
	if (!State.bIsValid || !State.bIsSourceControlled)
	{
		return true; // untracked: plain save
	}
	if (State.bIsCheckedOut || State.bIsAdded)
	{
		return true; // already ours
	}
	if (State.bIsCheckedOutOther)
	{
		OutError = FString::Printf(TEXT("checked out by %s"), *State.CheckedOutOther);
		return false;
	}
	if (!USourceControlHelpers::CheckOutFile(Filename, /*bSilent*/ true))
	{
		OutError = USourceControlHelpers::LastErrorMsg().ToString();
		return false;
	}
	return true;
}

bool MarkForAdd(const FString& Filename, FString& OutError)
{
	if (GTestOverrides.IsSet() && GTestOverrides->MarkForAdd)
	{
		return GTestOverrides->MarkForAdd(Filename, OutError);
	}
	if (!USourceControlHelpers::IsEnabled())
	{
		return true;
	}
	FSourceControlState State = USourceControlHelpers::QueryFileState(Filename, /*bSilent*/ true);
	if (State.bIsValid && (State.bIsSourceControlled || State.bIsAdded))
	{
		return true; // already tracked
	}
	if (!USourceControlHelpers::MarkFileForAdd(Filename, /*bSilent*/ true))
	{
		OutError = USourceControlHelpers::LastErrorMsg().ToString();
		return false;
	}
	return true;
}

}
