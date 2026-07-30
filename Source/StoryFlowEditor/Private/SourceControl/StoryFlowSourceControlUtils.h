// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Thin wrapper around the editor's revision-control provider used by the
 * importer's save path, so a sync behaves like a well-mannered editor tool:
 * files checked into source control are checked out before being
 * overwritten, and newly created files are marked for add. The changed
 * files land in the user's pending changelist to review and submit.
 *
 * Every function is a success no-op when no provider is enabled, so users
 * without source control see no behavior change.
 */
namespace StoryFlowSourceControl
{
	/** Test seam: replaces the real provider calls in automation tests. */
	struct FTestOverrides
	{
		TFunction<bool(const FString& Filename, FString& OutError)> EnsureWritable;
		TFunction<bool(const FString& Filename, FString& OutError)> MarkForAdd;
	};

	/** Install overrides for a test; pass an unset optional to restore the real behavior. */
	void SetTestOverrides(const TOptional<FTestOverrides>& Overrides);

	/**
	 * Make an existing tracked file writable (checkout). Returns false with a
	 * user-facing reason when it cannot (e.g. exclusively checked out by a
	 * teammate). True for untracked files, missing files and no provider.
	 */
	bool EnsureWritable(const FString& Filename, FString& OutError);

	/** Register a newly created file with the provider (mark for add). */
	bool MarkForAdd(const FString& Filename, FString& OutError);
}
