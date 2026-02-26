// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class FStoryFlowWebSocketClient;
class UStoryFlowProjectAsset;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnStoryFlowSyncComplete, UStoryFlowProjectAsset* /* Project */);

/**
 * Manages synchronization between Unreal and StoryFlow Editor
 */
class STORYFLOWEDITOR_API FStoryFlowSyncManager : public TSharedFromThis<FStoryFlowSyncManager>
{
public:
	FStoryFlowSyncManager();
	~FStoryFlowSyncManager();

	/**
	 * Initialize with a WebSocket client
	 * @param InClient The WebSocket client to use for communication
	 */
	void Initialize(TSharedPtr<FStoryFlowWebSocketClient> InClient);

	/**
	 * Shutdown and cleanup
	 */
	void Shutdown();

	/**
	 * Set the content path where assets will be imported
	 * @param Path Unreal content path (e.g., "/Game/StoryFlow")
	 */
	void SetContentPath(const FString& Path);

	/**
	 * Get the current content path
	 */
	FString GetContentPath() const { return ContentPath; }

	/**
	 * Set the project root path (StoryFlow project directory)
	 * @param Path File system path to the project
	 */
	void SetProjectPath(const FString& Path);

	/**
	 * Get the current project path
	 */
	FString GetProjectPath() const { return ProjectPath; }

	/**
	 * Get the currently loaded project asset
	 */
	UStoryFlowProjectAsset* GetProjectAsset() const { return ProjectAsset.Get(); }

	// Events
	FOnStoryFlowSyncComplete OnSyncComplete;

private:
	/** Handle incoming WebSocket messages (signature must match FOnStoryFlowMessageReceived delegate) */
	void HandleMessage(const FString& Type, TSharedPtr<FJsonObject> Payload);

	/** Handle project-updated message */
	void HandleProjectUpdated(const TSharedPtr<FJsonObject>& Payload);

	/** WebSocket client */
	TSharedPtr<FStoryFlowWebSocketClient> Client;

	/** Delegate handle for message received */
	FDelegateHandle MessageReceivedHandle;

	/** Execute the actual import */
	void ExecuteImport(const FString& BuildDir, const FString& ImportContentPath);

	/** Called when PIE ends, to execute any pending sync */
	void OnEndPIE(bool bIsSimulating);

	/** Content path for imported assets */
	FString ContentPath = TEXT("/Game/StoryFlow");

	/** Project root path */
	FString ProjectPath;

	/** Currently loaded project asset (prevents GC collection in non-UObject class) */
	TStrongObjectPtr<UStoryFlowProjectAsset> ProjectAsset;

	/** Pending sync data deferred until PIE ends */
	TOptional<TPair<FString, FString>> PendingSync;

	/** Delegate handle for EndPIE */
	FDelegateHandle EndPIEHandle;
};
