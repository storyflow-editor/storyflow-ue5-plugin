// Copyright 2026 StoryFlow. All Rights Reserved.

#include "SourceControl/StoryFlowSourceControlUtils.h"
#include "StoryFlowRuntime.h"
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
	if (!State.bIsValid)
	{
		// State unreadable (provider unreachable, query failed). Attempting the
		// save is better than refusing the sync over a state we cannot see: it
		// succeeds whenever the file is writable anyway, and fails with the
		// existing read-only diagnosis when it is not.
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Could not determine revision control state for '%s'; saving directly"), *Filename);
		return true;
	}
	if (!State.bIsSourceControlled)
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
	// Deliberately not a rung above: being out of date at head does not block a
	// checkout, and this payload is regenerated wholesale from the exported
	// JSON, so a stale local revision is resolved at submit time (where the user
	// can see both sides) rather than by refusing the sync here.
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
	if (!State.bIsValid)
	{
		// State unreadable: the file is already written, so there is nothing to
		// undo — report the gap and let the user add it by hand if needed.
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Could not determine revision control state for '%s'; leaving it for you to add"), *Filename);
		return true;
	}
	if (State.bIsAdded)
	{
		return true; // already ours, pending add
	}
	if (State.bIsSourceControlled)
	{
		// This function only runs for files that did not exist locally before
		// the save, so a file the provider already tracks is the out-of-date
		// workspace signature: a teammate added this asset, and the local write
		// landed without a checkout because there was nothing here to check out.
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: '%s' did not exist locally but revision control already tracks it, so your workspace is probably out of date. Sync and resolve this file in revision control before submitting."), *Filename);
		return true;
	}
	if (!USourceControlHelpers::MarkFileForAdd(Filename, /*bSilent*/ true))
	{
		OutError = USourceControlHelpers::LastErrorMsg().ToString();
		return false;
	}
	return true;
}

}
