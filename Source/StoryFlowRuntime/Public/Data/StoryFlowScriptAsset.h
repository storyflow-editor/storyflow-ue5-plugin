// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/ObjectSaveContext.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowScriptAsset.generated.h"

/**
 * DataAsset containing a single StoryFlow script
 */
UCLASS(BlueprintType)
class STORYFLOWRUNTIME_API UStoryFlowScriptAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Entry point node ID (always "0") */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString StartNode = TEXT("0");

	/** Nodes keyed by ID for O(1) lookup */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowNode> Nodes;

	/** Edge connections */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowConnection> Connections;

	/** Variables keyed by generated hash ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariable> Variables;

	/** Flattened string table: "en.key" -> "value" */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FString> Strings;

	/** Asset registry keyed by asset ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowAsset> Assets;

	/** Resolved Unreal asset references */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, TSoftObjectPtr<UObject>> ResolvedAssets;

	/** Flow definitions (for exit route detection) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowFlowDef> Flows;

	/** Original script path (relative to project) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString ScriptPath;

	// --- Non-serialized connection indices (built after import / load) ---

	/** SourceHandle -> Connection index (first match) */
	TMap<FString, int32> SourceHandleIndex;

	/** Source node ID -> Connection indices */
	TMap<FString, TArray<int32>> SourceNodeIndex;

	/** Target node ID -> Connection indices */
	TMap<FString, TArray<int32>> TargetNodeIndex;

	/** Build connection index maps. Call after Connections are populated. */
	void BuildConnectionIndices();

	/** Rebuild indices and unpack array variables after loading from disk */
	virtual void PostLoad() override;

	/** Pack array variables before saving */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;

public:
	/** Get a node by ID */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FStoryFlowNode GetNode(const FString& NodeId) const;

	/** Check if node exists */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	bool HasNode(const FString& NodeId) const;

	/** Get a localized string by key */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FString GetString(const FString& Key, const FString& LanguageCode = TEXT("en")) const;

	/** Get a variable by name */
	UFUNCTION(BlueprintPure, Category = "StoryFlow")
	FStoryFlowVariable GetVariable(const FString& VariableName) const;

	/** Find edge by source handle */
	const FStoryFlowConnection* FindEdgeBySourceHandle(const FString& SourceHandle) const;

	/** Find edge by source node ID */
	const FStoryFlowConnection* FindEdgeBySource(const FString& SourceNodeId) const;

	/** Find input edge to a node */
	const FStoryFlowConnection* FindInputEdge(const FString& NodeId, const FString& HandleSuffix) const;

	/** Get all edges from a source node */
	TArray<const FStoryFlowConnection*> GetEdgesFromSource(const FString& SourceNodeId) const;

	/** Find first edge by target node ID */
	const FStoryFlowConnection* FindEdgeByTarget(const FString& TargetNodeId) const;

	/** Get all edges targeting a node */
	TArray<const FStoryFlowConnection*> GetEdgesByTarget(const FString& TargetNodeId) const;
};
