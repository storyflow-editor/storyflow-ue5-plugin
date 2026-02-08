// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowScriptAsset.h"

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

FStoryFlowVariable UStoryFlowScriptAsset::GetVariable(const FString& VariableId) const
{
	if (const FStoryFlowVariable* Variable = Variables.Find(VariableId))
	{
		return *Variable;
	}
	return FStoryFlowVariable();
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindEdgeBySourceHandle(const FString& SourceHandle) const
{
	for (const FStoryFlowConnection& Connection : Connections)
	{
		if (Connection.SourceHandle.Contains(SourceHandle))
		{
			return &Connection;
		}
	}
	return nullptr;
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindEdgeBySource(const FString& SourceNodeId) const
{
	for (const FStoryFlowConnection& Connection : Connections)
	{
		if (Connection.Source == SourceNodeId)
		{
			return &Connection;
		}
	}
	return nullptr;
}

const FStoryFlowConnection* UStoryFlowScriptAsset::FindInputEdge(const FString& NodeId, const FString& HandleSuffix) const
{
	const FString Pattern = FString::Printf(TEXT("target-%s-%s"), *NodeId, *HandleSuffix);
	for (const FStoryFlowConnection& Connection : Connections)
	{
		if (Connection.Target == NodeId && Connection.TargetHandle.Contains(Pattern))
		{
			return &Connection;
		}
	}
	return nullptr;
}

TArray<const FStoryFlowConnection*> UStoryFlowScriptAsset::GetEdgesFromSource(const FString& SourceNodeId) const
{
	TArray<const FStoryFlowConnection*> Result;
	for (const FStoryFlowConnection& Connection : Connections)
	{
		if (Connection.Source == SourceNodeId)
		{
			Result.Add(&Connection);
		}
	}
	return Result;
}
