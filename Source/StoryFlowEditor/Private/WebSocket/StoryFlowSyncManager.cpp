// Copyright 2026 StoryFlow. All Rights Reserved.

#include "WebSocket/StoryFlowSyncManager.h"
#include "WebSocket/StoryFlowWebSocketClient.h"
#include "Import/StoryFlowImporter.h"
#include "Data/StoryFlowProjectAsset.h"

FStoryFlowSyncManager::FStoryFlowSyncManager()
{
}

FStoryFlowSyncManager::~FStoryFlowSyncManager()
{
	Shutdown();
}

void FStoryFlowSyncManager::Initialize(TSharedPtr<FStoryFlowWebSocketClient> InClient)
{
	Client = InClient;

	if (Client.IsValid())
	{
		MessageReceivedHandle = Client->OnMessageReceived.AddRaw(this, &FStoryFlowSyncManager::HandleMessage);
	}
}

void FStoryFlowSyncManager::Shutdown()
{
	if (Client.IsValid() && MessageReceivedHandle.IsValid())
	{
		Client->OnMessageReceived.Remove(MessageReceivedHandle);
		MessageReceivedHandle.Reset();
	}

	Client.Reset();
}

void FStoryFlowSyncManager::SetContentPath(const FString& Path)
{
	ContentPath = Path;
}

void FStoryFlowSyncManager::SetProjectPath(const FString& Path)
{
	ProjectPath = Path;
}

void FStoryFlowSyncManager::HandleMessage(const FString& Type, TSharedPtr<FJsonObject> Payload)
{
	if (Type == TEXT("project-updated"))
	{
		HandleProjectUpdated(Payload);
	}
}

void FStoryFlowSyncManager::HandleProjectUpdated(TSharedPtr<FJsonObject> Payload)
{
	if (!Payload.IsValid())
	{
		return;
	}

	// Get project path from payload
	FString NewProjectPath = Payload->GetStringField(TEXT("projectPath"));
	if (!NewProjectPath.IsEmpty())
	{
		ProjectPath = NewProjectPath;
	}

	// Get build directory
	FString BuildDir = FPaths::Combine(ProjectPath, TEXT("build"));
	if (!FPaths::DirectoryExists(BuildDir))
	{
		UE_LOG(LogTemp, Warning, TEXT("StoryFlow: Build directory not found: %s"), *BuildDir);
		return;
	}

	// Import the project
	ProjectAsset = UStoryFlowImporter::ImportProject(BuildDir, ContentPath);

	if (ProjectAsset)
	{
		UE_LOG(LogTemp, Log, TEXT("StoryFlow: Project synced successfully"));
		OnSyncComplete.Broadcast(ProjectAsset);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("StoryFlow: Failed to import project"));
	}
}

