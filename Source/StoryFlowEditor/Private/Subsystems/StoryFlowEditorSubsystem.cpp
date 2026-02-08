// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Subsystems/StoryFlowEditorSubsystem.h"
#include "StoryFlowRuntime.h"
#include "WebSocket/StoryFlowWebSocketClient.h"
#include "WebSocket/StoryFlowSyncManager.h"
#include "Import/StoryFlowImporter.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"

void UStoryFlowEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Create WebSocket client
	WebSocketClient = MakeShared<FStoryFlowWebSocketClient>();

	// Create sync manager
	SyncManager = MakeShared<FStoryFlowSyncManager>();
	SyncManager->Initialize(WebSocketClient);
	SyncManager->SetContentPath(ContentPath);

	// Bind events
	ConnectionStateHandle = WebSocketClient->OnConnectionStateChanged.AddUObject(
		this, &UStoryFlowEditorSubsystem::HandleConnectionStateChanged);

	SyncCompleteHandle = SyncManager->OnSyncComplete.AddUObject(
		this, &UStoryFlowEditorSubsystem::HandleSyncComplete);

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Editor subsystem initialized"));
}

void UStoryFlowEditorSubsystem::Deinitialize()
{
	// Unbind events
	if (WebSocketClient.IsValid())
	{
		WebSocketClient->OnConnectionStateChanged.Remove(ConnectionStateHandle);
	}

	if (SyncManager.IsValid())
	{
		SyncManager->OnSyncComplete.Remove(SyncCompleteHandle);
		SyncManager->Shutdown();
	}

	// Cleanup
	if (WebSocketClient.IsValid())
	{
		WebSocketClient->Disconnect();
	}

	WebSocketClient.Reset();
	SyncManager.Reset();

	Super::Deinitialize();
}

// ============================================================================
// Connection Management
// ============================================================================

void UStoryFlowEditorSubsystem::ConnectToStoryFlow(const FString& Host, int32 Port)
{
	if (!WebSocketClient.IsValid())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: WebSocket client not initialized"));
		return;
	}

	FString URL = FString::Printf(TEXT("ws://%s:%d"), *Host, Port);
	WebSocketClient->Connect(URL);
}

void UStoryFlowEditorSubsystem::Disconnect()
{
	if (WebSocketClient.IsValid())
	{
		WebSocketClient->Disconnect();
	}
}

bool UStoryFlowEditorSubsystem::IsConnected() const
{
	return WebSocketClient.IsValid() && WebSocketClient->IsConnected();
}

void UStoryFlowEditorSubsystem::RequestSync()
{
	if (WebSocketClient.IsValid())
	{
		WebSocketClient->RequestSync();
	}
}

// ============================================================================
// Manual Import
// ============================================================================

UStoryFlowProjectAsset* UStoryFlowEditorSubsystem::ImportProject(const FString& BuildDirectory, const FString& InContentPath)
{
	return UStoryFlowImporter::ImportProject(BuildDirectory, InContentPath);
}

UStoryFlowScriptAsset* UStoryFlowEditorSubsystem::ImportScript(const FString& JsonPath, const FString& InContentPath)
{
	return UStoryFlowImporter::ImportScript(JsonPath, InContentPath);
}

// ============================================================================
// Configuration
// ============================================================================

void UStoryFlowEditorSubsystem::SetContentPath(const FString& Path)
{
	ContentPath = Path;

	if (SyncManager.IsValid())
	{
		SyncManager->SetContentPath(Path);
	}
}

FString UStoryFlowEditorSubsystem::GetContentPath() const
{
	return ContentPath;
}

UStoryFlowProjectAsset* UStoryFlowEditorSubsystem::GetProjectAsset() const
{
	if (SyncManager.IsValid())
	{
		return SyncManager->GetProjectAsset();
	}
	return nullptr;
}

// ============================================================================
// Event Handlers
// ============================================================================

void UStoryFlowEditorSubsystem::HandleConnectionStateChanged(bool bConnected)
{
	if (bConnected)
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Connected to editor"));
		OnConnected.Broadcast();
	}
	else
	{
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Disconnected from editor"));
		OnDisconnected.Broadcast();
	}
}

void UStoryFlowEditorSubsystem::HandleSyncComplete(UStoryFlowProjectAsset* Project)
{
	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Sync complete"));
	OnSyncComplete.Broadcast(Project);
}

