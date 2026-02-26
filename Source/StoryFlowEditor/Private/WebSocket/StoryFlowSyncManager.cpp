// Copyright 2026 StoryFlow. All Rights Reserved.

#include "WebSocket/StoryFlowSyncManager.h"
#include "StoryFlowRuntime.h"
#include "WebSocket/StoryFlowWebSocketClient.h"
#include "Import/StoryFlowImporter.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Async/Async.h"
#include "Editor.h"

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

void FStoryFlowSyncManager::HandleProjectUpdated(const TSharedPtr<FJsonObject>& Payload)
{
	if (!Payload.IsValid())
	{
		return;
	}

	// Get project path from payload
	FString NewProjectPath;
	if (Payload->TryGetStringField(TEXT("projectPath"), NewProjectPath) && !NewProjectPath.IsEmpty())
	{
		ProjectPath = NewProjectPath;
	}

	// Get build directory
	FString BuildDir = FPaths::Combine(ProjectPath, TEXT("build"));
	if (!FPaths::DirectoryExists(BuildDir))
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Build directory not found: %s"), *BuildDir);
		return;
	}

	// Ensure import runs on the game thread (WebSocket callbacks may arrive on other threads)
	FString CapturedBuildDir = BuildDir;
	FString CapturedContentPath = ContentPath;
	TWeakPtr<FStoryFlowSyncManager> WeakSelf = AsShared();

	AsyncTask(ENamedThreads::GameThread, [WeakSelf, CapturedBuildDir, CapturedContentPath]()
	{
		TSharedPtr<FStoryFlowSyncManager> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		if (GEditor && GEditor->IsPlaySessionInProgress())
		{
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Cannot sync while Play-In-Editor is active. Stop playing to sync."));
			return;
		}

		UStoryFlowProjectAsset* ImportedAsset = UStoryFlowImporter::ImportProject(CapturedBuildDir, CapturedContentPath);
		Self->ProjectAsset.Reset(ImportedAsset);

		if (ImportedAsset)
		{
			UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Project synced successfully"));
			Self->OnSyncComplete.Broadcast(ImportedAsset);
		}
		else
		{
			UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to import project"));
		}
	});
}

