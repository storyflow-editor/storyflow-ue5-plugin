// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowDataAssetAsset.h"

namespace StoryFlowDataAssets
{
	namespace
	{
		/** The declaration of `VariableId` on one level, or null. Mirrors runtime-data-assets.js findDeclared. */
		const FStoryFlowVariable* FindDeclaredOnLevel(const FStoryFlowDataAssetDef& Level, const FString& VariableId)
		{
			for (const FStoryFlowVariable& Variable : Level.Variables)
			{
				if (Variable.Id == VariableId)
				{
					return &Variable;
				}
			}
			return nullptr;
		}

		/**
		 * THE chain walk, leaf -> root, shared by every function in this file so none of them can
		 * disagree about chain order, the depth cap or the cycle guard. Calls Visit(Level) per
		 * level and stops early when Visit returns false.
		 *
		 * Mirrors runtime-data-assets.js's `depth++ <= MAX_DEPTH` boundary exactly: the counter is
		 * tested BEFORE it is incremented, so MaxChainDepth ancestors plus the starting level —
		 * 65 levels — are visited (contract §4.4). An absent parent, a cycle, or the cap ends the
		 * walk silently, and callers answer with whatever they collected so far.
		 */
		template <typename VisitorType>
		void WalkChain(const FSeed& Seed, const FString& AssetId, VisitorType&& Visit)
		{
			const FStoryFlowDataAssetDef* Level = Seed.Find(AssetId);
			TSet<FString> Visited;
			int32 Depth = 0;
			while (Level && Depth++ <= MaxChainDepth && !Visited.Contains(Level->Id))
			{
				Visited.Add(Level->Id);
				if (!Visit(*Level))
				{
					return;
				}
				Level = Level->Parent.IsEmpty() ? nullptr : Seed.Find(Level->Parent);
			}
		}

		/** Copy a resolved value out of the store with its map storage detached (contract §3). */
		FStoryFlowVariant CopyOut(const FStoryFlowVariant& Value)
		{
			FStoryFlowVariant Copy = Value;
			Copy.DeepCopyMap();
			return Copy;
		}
	}

	void BuildSeed(const TMap<FString, UStoryFlowDataAssetAsset*>& Assets, FSeed& OutSeed)
	{
		OutSeed.Empty();
		for (const auto& AssetPair : Assets)
		{
			const UStoryFlowDataAssetAsset* Asset = AssetPair.Value;
			if (!Asset)
			{
				continue;
			}

			FStoryFlowDataAssetDef Def;
			// The MAP KEY is the authoritative assetId (it is what the pills, the resolver and
			// the save key all use); the asset's own AssetId field carries the same value for
			// inspection.
			Def.Id = AssetPair.Key;
			Def.Name = Asset->Name;
			Def.Parent = Asset->Parent;
			Def.Variables = Asset->Variables;
			Def.Overrides = Asset->Overrides;

			DeepCopyMapVariables(Def.Variables);
			DeepCopyMapVariants(Def.Overrides);

			OutSeed.Add(AssetPair.Key, MoveTemp(Def));
		}
	}

	bool HasAsset(const FSeed& Seed, const FString& AssetId)
	{
		return Seed.Contains(AssetId);
	}

	const FStoryFlowVariable* FindDeclaration(const FSeed& Seed, const FString& AssetId, const FString& VariableId)
	{
		// Keep walking past a hit: the ROOT-MOST declaration owns the slot (contract §4.3), so
		// each ancestor's declaration overwrites the descendant's.
		const FStoryFlowVariable* Declared = nullptr;
		WalkChain(Seed, AssetId, [&](const FStoryFlowDataAssetDef& Level)
		{
			if (const FStoryFlowVariable* Decl = FindDeclaredOnLevel(Level, VariableId))
			{
				Declared = Decl;
			}
			return true;
		});
		return Declared;
	}

	const FStoryFlowVariable* FindDeclarationByName(const FSeed& Seed, const FString& AssetId, const FString& VariableName)
	{
		// Same walk, same root-most-wins rule as FindDeclaration — only the match differs. A
		// descendant that re-declares an inherited NAME therefore resolves to the ancestor's
		// declaration, which is the id its value actually lives under.
		const FStoryFlowVariable* Declared = nullptr;
		WalkChain(Seed, AssetId, [&](const FStoryFlowDataAssetDef& Level)
		{
			for (const FStoryFlowVariable& Variable : Level.Variables)
			{
				if (Variable.Name == VariableName)
				{
					Declared = &Variable;
					break;
				}
			}
			return true;
		});
		return Declared;
	}

	bool TryResolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue)
	{
		// resolveEntry's two accumulators, kept apart on purpose:
		//  - Nearest: the FIRST overlay-or-override hit leaf -> root (nearest wins, §4.1/§4.2).
		//  - Declared: the ROOT-MOST declaration's own value (§4.3), which is why a declaration
		//    must NOT stop the walk.
		// Returning early on an override would resurrect ORPHAN overrides: deleting a variable
		// from a base prunes overrides on the declaring asset only, leaving descendants' entries
		// behind as inert data. An override counts only when the chain still declares the id.
		const FStoryFlowVariant* Nearest = nullptr;
		const FStoryFlowVariant* Declared = nullptr;

		WalkChain(Seed, AssetId, [&](const FStoryFlowDataAssetDef& Level)
		{
			if (!Nearest)
			{
				if (const TMap<FString, FStoryFlowVariant>* LevelOverlay = Overlay.Find(Level.Id))
				{
					Nearest = LevelOverlay->Find(VariableId);
				}
				if (!Nearest)
				{
					Nearest = Level.Overrides.Find(VariableId);
				}
			}
			if (const FStoryFlowVariable* Decl = FindDeclaredOnLevel(Level, VariableId))
			{
				Declared = &Decl->Value;
			}
			return true;
		});

		if (!Declared)
		{
			return false;
		}
		OutValue = CopyOut(Nearest ? *Nearest : *Declared);
		return true;
	}

	FStoryFlowVariant Resolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId)
	{
		FStoryFlowVariant Value;
		TryResolve(Seed, Overlay, AssetId, VariableId, Value);
		return Value;
	}

	bool IsDeclaredOnChain(const FSeed& Seed, const FString& AssetId, const FString& VariableId)
	{
		bool bDeclared = false;
		WalkChain(Seed, AssetId, [&](const FStoryFlowDataAssetDef& Level)
		{
			if (FindDeclaredOnLevel(Level, VariableId))
			{
				bDeclared = true;
				return false; // any declaration answers this question — stop
			}
			return true;
		});
		return bDeclared;
	}

	bool TrySet(const FSeed& Seed, FOverlay& Overlay, const FString& AssetId, const FString& VariableId, const FStoryFlowVariant& Value)
	{
		if (!HasAsset(Seed, AssetId))
		{
			return false;
		}
		if (!IsDeclaredOnChain(Seed, AssetId, VariableId))
		{
			return false;
		}

		// Deep copy on the way in, so a caller mutating its own array/map afterwards cannot
		// reach into the overlay (contract §5). Map writes replace the whole value by
		// construction — the entry list is assigned, never merged.
		FStoryFlowVariant Copy = Value;
		Copy.DeepCopyMap();
		Overlay.FindOrAdd(AssetId).Add(VariableId, MoveTemp(Copy));
		return true;
	}

	bool DeclMatches(const FStoryFlowVariable& Declaration, EStoryFlowVariableType VariableType, bool bIsArray, EStoryFlowVariableType KeyType, EStoryFlowVariableType ValueType)
	{
		if (Declaration.Type != VariableType)
		{
			return false;
		}
		if (Declaration.bIsArray != bIsArray)
		{
			return false;
		}
		if (VariableType == EStoryFlowVariableType::Map)
		{
			return Declaration.KeyType == KeyType && Declaration.ValueType == ValueType;
		}
		return true;
	}

	bool DeclMatchesNodeData(const FStoryFlowVariable& Declaration, const FStoryFlowNodeData& Data)
	{
		return DeclMatches(
			Declaration,
			ParseVariableType(Data.VariableType),
			Data.bIsArray,
			ParseVariableType(Data.KeyType),
			ParseVariableType(Data.ValueType));
	}

	void ResetOverlay(FOverlay& Overlay)
	{
		Overlay.Empty();
	}
}
