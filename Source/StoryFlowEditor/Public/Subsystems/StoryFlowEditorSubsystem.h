// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "StoryFlowEditorSubsystem.generated.h"

class UStoryFlowProjectAsset;
class UStoryFlowScriptAsset;
class FStoryFlowWebSocketClient;
class FStoryFlowSyncManager;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStoryFlowConnected);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStoryFlowDisconnected);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStoryFlowSyncCompleteDelegate, UStoryFlowProjectAsset*, Project);

/**
 * Editor subsystem for StoryFlow integration
 *
 * Provides live sync with StoryFlow Editor via WebSocket
 */
UCLASS()
class STORYFLOWEDITOR_API UStoryFlowEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ========================================================================
	// Connection Management
	// ========================================================================

	/**
	 * Connect to StoryFlow Editor
	 * @param Host Hostname (default: localhost)
	 * @param Port Port number (default: 9000)
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void ConnectToStoryFlow(const FString& Host = TEXT("localhost"), int32 Port = 9000);

	/**
	 * Disconnect from StoryFlow Editor
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void Disconnect();

	/**
	 * Check if connected to StoryFlow Editor
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool IsConnected() const;

	/**
	 * Request a full project sync
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void RequestSync();

	// ========================================================================
	// Manual Import
	// ========================================================================

	/**
	 * Import a StoryFlow project from a build directory
	 * @param BuildDirectory Path to the build directory (containing project.json)
	 * @param ContentPath Unreal content path for imported assets (default: /Game/StoryFlow)
	 * @return The imported project asset
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	UStoryFlowProjectAsset* ImportProject(const FString& BuildDirectory, const FString& InContentPath = TEXT("/Game/StoryFlow"));

	/**
	 * Import a single script from JSON
	 * @param JsonPath Path to the script JSON file
	 * @param ContentPath Unreal content path for imported asset
	 * @return The imported script asset
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	UStoryFlowScriptAsset* ImportScript(const FString& JsonPath, const FString& InContentPath = TEXT("/Game/StoryFlow/Data"));

	// ========================================================================
	// Configuration
	// ========================================================================

	/**
	 * Set the content path where assets will be imported
	 * @param Path Unreal content path (e.g., "/Game/StoryFlow")
	 */
	UFUNCTION(BlueprintCallable, Category = "StoryFlow")
	void SetContentPath(const FString& Path);

	/**
	 * Get the current content path
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetContentPath() const;

	/**
	 * Get the currently loaded project asset
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	UStoryFlowProjectAsset* GetProjectAsset() const;

	// ========================================================================
	// Events
	// ========================================================================

	/** Called when connection to StoryFlow Editor is established */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnStoryFlowConnected OnConnected;

	/** Called when disconnected from StoryFlow Editor */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnStoryFlowDisconnected OnDisconnected;

	/** Called when a full project sync completes */
	UPROPERTY(BlueprintAssignable, Category = "StoryFlow|Events")
	FOnStoryFlowSyncCompleteDelegate OnSyncComplete;

private:
	/** Handle connection state change */
	void HandleConnectionStateChanged(bool bConnected);

	/** Handle sync complete */
	void HandleSyncComplete(UStoryFlowProjectAsset* Project);

	/** WebSocket client */
	TSharedPtr<FStoryFlowWebSocketClient> WebSocketClient;

	/** Sync manager */
	TSharedPtr<FStoryFlowSyncManager> SyncManager;

	/** Content path for imports */
	FString ContentPath = TEXT("/Game/StoryFlow");

	/** Delegate handles */
	FDelegateHandle ConnectionStateHandle;
	FDelegateHandle SyncCompleteHandle;
};
