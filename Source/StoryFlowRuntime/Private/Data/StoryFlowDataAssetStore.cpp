// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "StoryFlowRuntime.h"

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
		 * The same lookup by DISPLAY NAME — the Blueprint surface's entry point, where an author
		 * types a name rather than an id.
		 *
		 * FIRST DECLARED WINS within a level, which only matters because names, unlike ids, are
		 * not unique by construction: the editor keeps them unique per asset, but nothing in the
		 * seed format enforces it and a hand-edited export can carry two. Between LEVELS the
		 * root-most declaration still wins — that rule lives in the walk, not here.
		 */
		const FStoryFlowVariable* FindDeclaredOnLevelByName(const FStoryFlowDataAssetDef& Level, const FString& VariableName)
		{
			for (const FStoryFlowVariable& Variable : Level.Variables)
			{
				if (Variable.Name == VariableName)
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

		/**
		 * Is this string PROSE, i.e. worth looking up? Non-blank after trimming, exactly as the
		 * editor's keying pass decides it — a whitespace-only value keys nothing there, so looking
		 * one up here would probe an id no translator can ever reach.
		 */
		bool IsProse(const FString& Value)
		{
			return !Value.TrimStartAndEnd().IsEmpty();
		}

		/** One string through the project's string ladder, left alone when it is not prose. */
		void LocalizeString(const UStoryFlowProjectAsset& Project, const FString& LanguageCode, FStoryFlowVariant& Value)
		{
			const FString Key = Value.GetString();
			if (IsProse(Key))
			{
				Value.SetString(Project.GetGlobalString(Key, LanguageCode));
			}
		}

		/**
		 * TryRead's second half: a DECLARED value with its string-table keys resolved, in place.
		 *
		 * The type gate is the exporter's, transcribed (json-export-strategy.ts
		 * keyDataAssetDeclaration): a string scalar, the elements of a string ARRAY, and the values
		 * of a map whose ValueType is String. Everything else — enum, image, audio, character, and
		 * every number and boolean — passes through untouched even when its value is a string, and
		 * a map's KEYS are identifiers that never resolve whatever their KeyType is. A gate that
		 * drifted from the exporter's would look up an id nothing keyed, or hand back a key.
		 *
		 * ValueType defaults to String on FStoryFlowVariable, which is how "an absent valueType is
		 * a string map" arrives here without a special case.
		 */
		void LocalizeDeclaredValue(const FStoryFlowVariable& Declaration, const UStoryFlowProjectAsset& Project, const FString& LanguageCode, FStoryFlowVariant& Value)
		{
			if (Declaration.Type == EStoryFlowVariableType::Map)
			{
				if (Declaration.ValueType != EStoryFlowVariableType::String || !Value.IsMap())
				{
					return;
				}
				for (FStoryFlowMapEntry& Entry : Value.GetMapMutable())
				{
					LocalizeString(Project, LanguageCode, Entry.Value);
				}
				return;
			}

			if (Declaration.Type != EStoryFlowVariableType::String)
			{
				return;
			}

			if (Declaration.bIsArray)
			{
				for (FStoryFlowVariant& Element : Value.GetArrayMutable())
				{
					LocalizeString(Project, LanguageCode, Element);
				}
				return;
			}

			LocalizeString(Project, LanguageCode, Value);
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
		// Same walk, same root-most-wins rule as FindDeclaration — only the match differs. Two
		// shapes reach this, and they are not the same problem:
		//
		//  - THE RE-DECLARED ID: a descendant repeats an inherited id under the same name. The
		//    ancestor wins, which is right and invisible — both levels name one variable, and the
		//    ancestor's id is the one its value actually lives under.
		//  - THE SAME NAME ON TWO DIFFERENT IDS: two genuinely separate variables share a display
		//    name across levels. The ancestor still wins (one rule, no special case), which means
		//    the descendant's own variable is UNREACHABLE BY NAME from Blueprint. That is by
		//    design — a name lookup with two right answers has no better one — but it is worth
		//    saying out loud, because from the author's chair it looks like the setter silently
		//    wrote to the wrong variable. The editor keeps names unique per asset, so this only
		//    arrives from a hand-edited export or a rename across levels.
		const FStoryFlowVariable* Declared = nullptr;
		WalkChain(Seed, AssetId, [&](const FStoryFlowDataAssetDef& Level)
		{
			if (const FStoryFlowVariable* Decl = FindDeclaredOnLevelByName(Level, VariableName))
			{
				if (Declared && Declared->Id != Decl->Id)
				{
					UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset '%s' has the name '%s' on two different variables ('%s' and '%s') - the root-most one wins and the other cannot be reached by name"),
						*AssetId, *VariableName, *Decl->Id, *Declared->Id);
				}
				Declared = Decl;
			}
			return true;
		});
		return Declared;
	}

	TArray<FString> VariableNames(const FSeed& Seed, const FString& AssetId)
	{
		// THE shared walk hands the chain over leaf -> root; the list's order is ROOT-first, so the
		// levels are collected and then iterated BACKWARDS — the same shape the reference
		// implementation's eachDeclaration takes (chainOf + a reverse loop). The reverse iteration
		// is load-bearing twice over: first-wins on the id below is root-most-wins ONLY because the
		// root is visited first, and the name dedupe keeps the root-most position for the same
		// reason.
		TArray<const FStoryFlowDataAssetDef*> Chain;
		WalkChain(Seed, AssetId, [&Chain](const FStoryFlowDataAssetDef& Level)
		{
			Chain.Add(&Level);
			return true;
		});

		TArray<FString> Names;
		TSet<FString> ClaimedIds;
		TSet<FString> ClaimedNames;
		for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
		{
			// Declarations only — Overrides are deliberately never visited (see the header note).
			for (const FStoryFlowVariable& Declaration : Chain[Index]->Variables)
			{
				bool bIdClaimed = false;
				ClaimedIds.Add(Declaration.Id, &bIdClaimed);
				if (bIdClaimed)
				{
					// A descendant re-declaring an inherited id: the root-most declaration keeps
					// its slot, exactly as FindDeclaration resolves it.
					continue;
				}
				if (Declaration.Name.IsEmpty())
				{
					continue;
				}
				bool bNameClaimed = false;
				ClaimedNames.Add(Declaration.Name, &bNameClaimed);
				if (bNameClaimed)
				{
					// The same display name under a different id: stated once, at the root-most
					// position — the only one FindDeclarationByName can reach.
					continue;
				}
				Names.Add(Declaration.Name);
			}
		}
		return Names;
	}

	bool TryResolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue, EResolvedFrom* OutResolvedFrom)
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
		bool bDeclaredArray = false;
		// WHICH of Nearest's two sources hit. Recorded where the branch already is rather than
		// re-derived afterwards: an overlay entry and an override are indistinguishable once both
		// are just a variant pointer, and the localization gate needs them apart (EResolvedFrom).
		EResolvedFrom NearestFrom = EResolvedFrom::Override;

		WalkChain(Seed, AssetId, [&](const FStoryFlowDataAssetDef& Level)
		{
			if (!Nearest)
			{
				if (const TMap<FString, FStoryFlowVariant>* LevelOverlay = Overlay.Find(Level.Id))
				{
					Nearest = LevelOverlay->Find(VariableId);
					if (Nearest)
					{
						NearestFrom = EResolvedFrom::SessionWrite;
					}
				}
				if (!Nearest)
				{
					Nearest = Level.Overrides.Find(VariableId);
					if (Nearest)
					{
						NearestFrom = EResolvedFrom::Override;
					}
				}
			}
			if (const FStoryFlowVariable* Decl = FindDeclaredOnLevel(Level, VariableId))
			{
				Declared = &Decl->Value;
				bDeclaredArray = Decl->bIsArray;
			}
			return true;
		});

		if (!Declared)
		{
			return false;
		}
		if (OutResolvedFrom)
		{
			*OutResolvedFrom = Nearest ? NearestFrom : EResolvedFrom::Declaration;
		}
		OutValue = CopyOut(Nearest ? *Nearest : *Declared);
		// Legacy empty array overrides have no serialized element blob from which to infer shape.
		OutValue.SetArrayShape(bDeclaredArray);
		return true;
	}

	FStoryFlowVariant Resolve(const FSeed& Seed, const FOverlay& Overlay, const FString& AssetId, const FString& VariableId)
	{
		FStoryFlowVariant Value;
		TryResolve(Seed, Overlay, AssetId, VariableId, Value);
		return Value;
	}

	bool TryRead(const FStoreRef& Store, const UStoryFlowProjectAsset* Project, const FString& LanguageCode, const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue)
	{
		if (!Store.IsValid())
		{
			return false;
		}

		EResolvedFrom From = EResolvedFrom::Declaration;
		if (!TryResolve(*Store.Seed, *Store.Overlay, AssetId, VariableId, OutValue, &From))
		{
			return false;
		}

		// THE GATE, and it is the whole of it: only the seed's own declared value is content.
		// An override ships literal (the exporter keys declarations only) and a session write is
		// live data — neither has a row in any table, so a lookup over one could only find
		// SOMEBODY ELSE'S prose. Gated on provenance, never on the value's shape.
		if (From != EResolvedFrom::Declaration || !Project)
		{
			return true;
		}

		// The declaration the walk just answered from, for its DECLARED type — the gate the
		// amendment puts on prose. FindDeclaration repeats the walk rather than TryResolve
		// handing the pointer back: the chain rule stays a value-answering function, and this
		// costs one more walk on a path that is already doing a string-table lookup.
		const FStoryFlowVariable* Declaration = FindDeclaration(*Store.Seed, AssetId, VariableId);
		if (!Declaration)
		{
			return true;
		}
		LocalizeDeclaredValue(*Declaration, *Project, LanguageCode, OutValue);
		return true;
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
