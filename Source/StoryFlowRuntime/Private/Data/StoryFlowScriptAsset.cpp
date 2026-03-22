// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowHandles.h"

void UStoryFlowScriptAsset::PostLoad()
{
	Super::PostLoad();
	BuildConnectionIndices();
}

void UStoryFlowScriptAsset::BuildConnectionIndices()
{
	SourceHandleIndex.Empty(Connections.Num());
	SourceNodeIndex.Empty();
	TargetNodeIndex.Empty();

	for (int32 i = 0; i < Connections.Num(); ++i)
	{
		const FStoryFlowConnection& Conn = Connections[i];

		// SourceHandle -> first index only (handles are unique)
		if (!SourceHandleIndex.Contains(Conn.SourceHandle))
		{
			SourceHandleIndex.Add(Conn.SourceHandle, i);
		}

		// Source node -> all indices
		SourceNodeIndex.FindOrAdd(Conn.Source).Add(i);

		// Target node -> all indices
		TargetNodeIndex.FindOrAdd(Conn.Target).Add(i);
	}
}

FStoryFlowNode UStoryFlowScriptAsset::GetNode(const FString& NodeId) const
{
	if (const FStoryFlowNode* Node = Nodes.Find(NodeId))
	{
		return *Node;
	}
	return FStoryFlowNode();
}

bool UStoryFlowScriptAsset::HasNode(const FString& NodeId) const
{
	return Nodes.Contains(NodeId);
}

FString UStoryFlowScriptAsset::GetString(const FString& Key, const FString& LanguageCode) const
{
	// Lookup with language prefix
	const FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *Key);
	if (const FString* Value = Strings.Find(FullKey))
	{
		return *Value;
	}

	// Fallback to key without language prefix (for backwards compatibility)
	if (const FString* Value = Strings.Find(Key))
	{
		return *Value;
	}

	// Return key itself as fallback
	return Key;
}

FStoryFlowVariable UStoryFlowScriptAsset::GetVariable(const FString& VariableName) const
{
	for (const auto& Pair : Variables)
	{
		if (Pair.Value.Name == VariableName)
		{
			return Pair.Value;
		}
	}
	return FStoryFlowVariable();
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindEdgeBySourceHandle(const FString& SourceHandle) const
{
	if (const int32* IndexPtr = SourceHandleIndex.Find(SourceHandle))
	{
		return &Connections[*IndexPtr];
	}
	return nullptr;
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindEdgeBySource(const FString& SourceNodeId) const
{
	if (const TArray<int32>* Indices = SourceNodeIndex.Find(SourceNodeId))
	{
		if (Indices->Num() > 0)
		{
			return &Connections[(*Indices)[0]];
		}
	}
	return nullptr;
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindInputEdge(const FString& NodeId, const FString& HandleSuffix) const
{
	const FString Pattern = StoryFlowHandles::Target(NodeId, HandleSuffix);

	if (const TArray<int32>* Indices = TargetNodeIndex.Find(NodeId))
	{
		// Try exact match first
		for (int32 Idx : *Indices)
		{
			const FStoryFlowConnection& Conn = Connections[Idx];
			if (Conn.TargetHandle == Pattern)
			{
				return &Conn;
			}
		}

		// Fallback: prefix match for handles with trailing option ID.
		// The editor appends a numbered suffix to handles (e.g., "string-2", "string-array-1")
		// while the runtime constants omit it (e.g., "string", "string-array").
		const FString Prefix = Pattern + TEXT("-");
		for (int32 Idx : *Indices)
		{
			const FStoryFlowConnection& Conn = Connections[Idx];
			if (Conn.TargetHandle.StartsWith(Prefix))
			{
				return &Conn;
			}
		}
	}
	return nullptr;
}

TArray<const FStoryFlowConnection*> UStoryFlowScriptAsset::GetEdgesFromSource(const FString& SourceNodeId) const
{
	TArray<const FStoryFlowConnection*> Result;

	if (const TArray<int32>* Indices = SourceNodeIndex.Find(SourceNodeId))
	{
		Result.Reserve(Indices->Num());
		for (int32 Idx : *Indices)
		{
			Result.Add(&Connections[Idx]);
		}
	}

	return Result;
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindEdgeByTarget(const FString& TargetNodeId) const
{
	if (const TArray<int32>* Indices = TargetNodeIndex.Find(TargetNodeId))
	{
		if (Indices->Num() > 0)
		{
			return &Connections[(*Indices)[0]];
		}
	}
	return nullptr;
}

TArray<const FStoryFlowConnection*> UStoryFlowScriptAsset::GetEdgesByTarget(const FString& TargetNodeId) const
{
	TArray<const FStoryFlowConnection*> Result;

	if (const TArray<int32>* Indices = TargetNodeIndex.Find(TargetNodeId))
	{
		Result.Reserve(Indices->Num());
		for (int32 Idx : *Indices)
		{
			Result.Add(&Connections[Idx]);
		}
	}

	return Result;
}
