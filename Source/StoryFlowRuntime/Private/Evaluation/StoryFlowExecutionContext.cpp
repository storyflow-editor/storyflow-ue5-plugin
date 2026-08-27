// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Evaluation/StoryFlowExecutionContext.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Data/StoryFlowHandles.h"

void FStoryFlowExecutionContext::Initialize(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript)
{
	Reset();

	Project = InProject;
	CurrentScript = InScript;
	ExternalGlobalVariables = nullptr;

	if (InScript)
	{
		// Copy local variables from script. Map storage is shared (TSharedPtr) —
		// detach it so runtime mutations never write into the asset (HTML
		// re-inflates fresh maps from script data here).
		LocalVariables = InScript->Variables;
		DeepCopyMapVariables(LocalVariables);
		ResolveStringVariableValues(LocalVariables);
		CurrentNodeId = InScript->StartNode;
	}

	RebuildLocalNameIndex();
	RebuildGlobalNameIndex();
}

void FStoryFlowExecutionContext::InitializeWithSubsystem(UStoryFlowProjectAsset* InProject, UStoryFlowScriptAsset* InScript, TMap<FString, FStoryFlowVariable>* InGlobalVariables, TMap<FString, FStoryFlowCharacterDef>* InCharacters, TSet<FString>* InUsedOnceOnlyOptions, StoryFlowDataAssets::FStoreRef InDataAssetStore, const TMap<FString, FString>* InCharacterIdToPath, const FString* InActiveLanguage)
{
	Reset();

	Project = InProject;
	CurrentScript = InScript;
	ExternalGlobalVariables = InGlobalVariables;
	ExternalCharacters = InCharacters;
	ExternalUsedOnceOnlyOptions = InUsedOnceOnlyOptions;
	DataAssetStore = InDataAssetStore;
	CharacterIdToPath = InCharacterIdToPath;
	ActiveLanguage = InActiveLanguage;

	if (InScript)
	{
		// Copy local variables from script. Map storage is shared (TSharedPtr) —
		// detach it so runtime mutations never write into the asset (HTML
		// re-inflates fresh maps from script data here).
		LocalVariables = InScript->Variables;
		DeepCopyMapVariables(LocalVariables);
		ResolveStringVariableValues(LocalVariables);
		CurrentNodeId = InScript->StartNode;
	}

	RebuildLocalNameIndex();
	RebuildGlobalNameIndex();
}

void FStoryFlowExecutionContext::Reset()
{
	CurrentScript = nullptr;
	CurrentNodeId.Empty();
	bIsWaitingForInput = false;
	bIsExecuting = false;
	bIsPaused = false;
	bEnteringDialogueViaEdge = false;
	CallStack.Empty();
	FlowCallStack.Empty();
	LoopStack.Empty();
	LocalVariables.Empty();
	LocalVariableNameIndex.Empty();
	GlobalVariableNameIndex.Empty();
	ExternalUsedOnceOnlyOptions = nullptr;
	CurrentDialogueState = FStoryFlowDialogueState();
	PersistentBackgroundImage = nullptr;
	EvaluationDepth = 0;
	ProcessingDepth = 0;
	NodeRuntimeStates.Empty();
	WarnedUnknownNodes.Empty();
	WarnedMapNodes.Empty();
	WarnedDataAssetNodes.Empty();
	DataAssetWarningsEmitted = 0;
	WarnedCharacterIds.Empty();
	CharacterIdWarningsEmitted = 0;
	ExternalGlobalVariables = nullptr;
	ExternalCharacters = nullptr;
	CharacterIdToPath = nullptr;
	DataAssetStore = StoryFlowDataAssets::FStoreRef();
	ActiveLanguage = nullptr;
}

FStoryFlowNode* FStoryFlowExecutionContext::GetCurrentNode()
{
	return GetNode(CurrentNodeId);
}

FStoryFlowNode* FStoryFlowExecutionContext::GetNode(const FString& NodeId)
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		if (FStoryFlowNode* Node = Script->Nodes.Find(NodeId))
		{
			return Node;
		}
	}
	return nullptr;
}

FStoryFlowVariable* FStoryFlowExecutionContext::FindVariable(const FString& VariableId, bool bIsGlobal)
{
	if (bIsGlobal)
	{
		// Use external global variables if available (shared across all components via subsystem)
		if (ExternalGlobalVariables)
		{
			return ExternalGlobalVariables->Find(VariableId);
		}
		// Fall back to project's global variables
		if (UStoryFlowProjectAsset* Proj = Project.Get())
		{
			return Proj->GlobalVariables.Find(VariableId);
		}
		return nullptr;
	}
	return LocalVariables.Find(VariableId);
}

void FStoryFlowExecutionContext::SetVariable(const FString& VariableId, const FStoryFlowVariant& Value, bool bIsGlobal)
{
	if (FStoryFlowVariable* Variable = FindVariable(VariableId, bIsGlobal))
	{
		Variable->Value = Value;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: SetVariable failed - variable '%s' not found (bIsGlobal=%s)"), *VariableId, bIsGlobal ? TEXT("true") : TEXT("false"));
	}
}

FStoryFlowVariant FStoryFlowExecutionContext::GetVariableValue(const FString& VariableId, bool bIsGlobal)
{
	if (const FStoryFlowVariable* Variable = FindVariable(VariableId, bIsGlobal))
	{
		return Variable->Value;
	}
	return FStoryFlowVariant();
}

bool FStoryFlowExecutionContext::TryResolveDataAsset(const FString& AssetId, const FString& VariableId, FStoryFlowVariant& OutValue) const
{
	// TryRead, not TryResolve: a `.sfd` value read by graph code is read by a PLAYER, so a
	// declared string one resolves through the string tables here exactly as it does through the
	// Blueprint accessors (localization spec §2's amendment). The store's own null-check is
	// TryRead's, so the guard the other accessors here repeat is not repeated.
	//
	// This deliberately does NOT go through GetString: that ladder probes the current SCRIPT's
	// table first, and a `.sfd` id is keyed by data-assets.json, which the importer merges into
	// the project globals. TryRead consults the project's ladder directly, so a `.sfd` value
	// reads the same inside a dialogue and outside one — which is why the tripwire on the two
	// GetString ladders does not extend to a third here.
	return StoryFlowDataAssets::TryRead(DataAssetStore, Project.Get(), ActiveLanguage ? *ActiveLanguage : FString(), AssetId, VariableId, OutValue);
}

bool FStoryFlowExecutionContext::TrySetDataAsset(const FString& AssetId, const FString& VariableId, const FStoryFlowVariant& Value)
{
	if (!DataAssetStore.IsValid())
	{
		return false;
	}
	return StoryFlowDataAssets::TrySet(*DataAssetStore.Seed, *DataAssetStore.Overlay, AssetId, VariableId, Value);
}

void FStoryFlowExecutionContext::MaybeWarnDataAsset(const FString& NodeId, const TCHAR* Reason, const FString& Message)
{
	// The key is node AND reason, so a node with two problems reports both once,
	// and a fixed-then-broken-again node stays quiet until the next game restart.
	const FString Key = NodeId + TEXT("|") + Reason;
	if (WarnedDataAssetNodes.Contains(Key))
	{
		return;
	}
	WarnedDataAssetNodes.Add(Key);
	++DataAssetWarningsEmitted;
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: %s"), *Message);
}

FString FStoryFlowExecutionContext::ResolveDataAssetId(const FStoryFlowNode& Accessor) const
{
	const FStoryFlowConnection* Edge = FindInputEdge(Accessor.Id, StoryFlowHandles::In_DataAssetRef);
	if (!Edge)
	{
		return FString();
	}

	// const_cast only because GetNode is non-const; nothing here writes through it.
	const FStoryFlowNode* Source = const_cast<FStoryFlowExecutionContext*>(this)->GetNode(Edge->Source);

	// The type check keeps a non-pill source honest instead of speculatively reading an
	// AssetId field off whatever is on the far end (contract §6 row 1). An unbound pill
	// answers with its own empty AssetId, which is row 2 — both degrade identically here,
	// and the reference ladder folds them into one 'unwired' reason too.
	if (!Source || Source->Type != EStoryFlowNodeType::GetDataAsset)
	{
		return FString();
	}
	return Source->Data.AssetId;
}

bool FStoryFlowExecutionContext::TryResolveDataAssetBinding(const FStoryFlowNode& Accessor, FString& OutAssetId)
{
	if (Accessor.Data.VariableId.IsEmpty())
	{
		MaybeWarnDataAsset(Accessor.Id, TEXT("nodata"),
			FString::Printf(TEXT("Data Asset accessor has no variable binding: node %s"), *Accessor.Id));
		return false;
	}

	const FString AssetId = ResolveDataAssetId(Accessor);
	if (AssetId.IsEmpty())
	{
		MaybeWarnDataAsset(Accessor.Id, TEXT("unwired"),
			FString::Printf(TEXT("Data Asset accessor has no Data Asset connected: node %s"), *Accessor.Id));
		return false;
	}

	if (!DataAssetStore.IsValid())
	{
		// No store at all (a context that never met a subsystem). Latched under the dead-reference
		// reason: from the node's point of view its asset is not there, and the alternative is a
		// silent false that reads exactly like a healthy miss.
		MaybeWarnDataAsset(Accessor.Id, TEXT("deadref"),
			FString::Printf(TEXT("Data Asset store unavailable: %s (node %s)"), *AssetId, *Accessor.Id));
		return false;
	}

	// Dead REFERENCE vs stale BINDING: FindDeclaration answers null for both, so ask the seed
	// which one this is and name it — the two have different fixes (rebind the pill vs rebind
	// the accessor).
	if (!StoryFlowDataAssets::HasAsset(*DataAssetStore.Seed, AssetId))
	{
		MaybeWarnDataAsset(Accessor.Id, TEXT("deadref"),
			FString::Printf(TEXT("Data Asset not found: %s (node %s)"), *AssetId, *Accessor.Id));
		return false;
	}

	const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclaration(*DataAssetStore.Seed, AssetId, Accessor.Data.VariableId);
	if (!Declaration)
	{
		MaybeWarnDataAsset(Accessor.Id, TEXT("missing"),
			FString::Printf(TEXT("Data Asset variable not found: %s.%s (node %s)"), *AssetId, *Accessor.Data.VariableId, *Accessor.Id));
		return false;
	}

	// §6.1: the declaration moved under a live node. Treated as MISSING, never coerced — within
	// the string family a value carries no evidence of its declared type, which is exactly why
	// the check is on the DECLARATION. The character-variable node lane carries the same gate
	// on its write side now (HandleSetCharacterVar, the §5 type-mismatch pin).
	if (!StoryFlowDataAssets::DeclMatchesNodeData(*Declaration, Accessor.Data))
	{
		MaybeWarnDataAsset(Accessor.Id, TEXT("changed"),
			FString::Printf(TEXT("Data Asset variable type changed since this node was made: %s.%s (node %s)"), *AssetId, *Accessor.Data.VariableId, *Accessor.Id));
		return false;
	}

	OutAssetId = AssetId;
	return true;
}

FString FStoryFlowExecutionContext::ResolveCharacterKeyIn(const TMap<FString, FString>* IdToPath, const TMap<FString, FStoryFlowCharacterDef>* Characters, const FString& IdOrPath, FStoryFlowExecutionContext& WarnLatch)
{
	if (IsCharacterIdRef(IdOrPath))
	{
		const FString* RecordKey = IdToPath ? IdToPath->Find(IdOrPath) : nullptr;
		if (RecordKey)
		{
			if (Characters && Characters->Contains(*RecordKey))
			{
				// VERBATIM, never re-normalized: the bridge value IS the record key (the
				// export contract guarantees the normalized shape), and a normalize pass
				// here could only mask an exporter that broke that guarantee.
				return *RecordKey;
			}
			// A bridge hit whose record is not loaded is a MISS of the WHOLE resolution:
			// after LoadFromSlot the character store holds only what the save carried,
			// while the bridge is project-derived — falling through to path treatment
			// (and the caller's path field) is what keeps that mismatch survivable.
			WarnLatch.MaybeWarnCharacterId(IdOrPath, TEXT("unloaded"),
				FString::Printf(TEXT("Character id %s maps to '%s', which is not among the loaded runtime characters - falling back to path resolution"), *IdOrPath, **RecordKey));
		}
		else
		{
			WarnLatch.MaybeWarnCharacterId(IdOrPath, TEXT("dangling"),
				FString::Printf(TEXT("Character id %s is not in this project's character index - falling back to path resolution"), *IdOrPath));
		}
		// Fall through: the id is treated as a path from here. That lookup will normally
		// miss too, which is exactly the contract's degraded posture — the caller's path
		// field (via ResolveCharacterRef) or the existing missing-character behavior takes
		// over, never a crash.
	}
	return NormalizeCharacterPath(IdOrPath);
}

FString FStoryFlowExecutionContext::ResolveCharacterKey(const FString& IdOrPath)
{
	return ResolveCharacterKeyIn(CharacterIdToPath, ExternalCharacters, IdOrPath, *this);
}

FString FStoryFlowExecutionContext::ResolveCharacterRef(const FString& CharacterId, const FString& CharacterPath)
{
	if (!CharacterId.IsEmpty())
	{
		const FString RecordKey = ResolveCharacterKey(CharacterId);
		if (ExternalCharacters && ExternalCharacters->Contains(RecordKey))
		{
			return RecordKey;
		}
		// Dangling or unloaded id (warned once inside ResolveCharacterKey): contract §3 —
		// the path field is the fall-back.
	}
	// VERBATIM, not normalized: pre-P4 content must flow byte-identically through the path
	// lane, warnings included (they print the authored spelling, as they always have).
	return CharacterPath;
}

void FStoryFlowExecutionContext::MaybeWarnCharacterId(const FString& CharacterId, const TCHAR* Reason, const FString& Message)
{
	// Id AND reason, like MaybeWarnDataAsset above: an id that is dangling now and unloaded
	// after a save load names both problems once each.
	const FString Key = CharacterId + TEXT("|") + Reason;
	if (WarnedCharacterIds.Contains(Key))
	{
		return;
	}
	WarnedCharacterIds.Add(Key);
	++CharacterIdWarningsEmitted;
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: %s"), *Message);
}

FStoryFlowCharacterDef* FStoryFlowExecutionContext::FindCharacter(const FString& CharacterPath)
{
	if (CharacterPath.IsEmpty())
	{
		return nullptr;
	}

	// The one resolution point (P4): a character id answers its bridged record key verbatim,
	// anything else normalizes as a path exactly as before P4.
	FString RecordKey = ResolveCharacterKey(CharacterPath);

	// Use external characters (from subsystem - mutable runtime copies)
	if (ExternalCharacters)
	{
		return ExternalCharacters->Find(RecordKey);
	}

	// No external characters available - characters are now stored as UStoryFlowCharacterAsset*
	// in the project, so we can't return a mutable FStoryFlowCharacterDef* without a subsystem.
	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: FindCharacter called without external characters (no subsystem). Character: %s"), *CharacterPath);
	return nullptr;
}

FStoryFlowVariable* FStoryFlowExecutionContext::FindCharacterVariable(const FString& CharacterPath, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		return nullptr;
	}

	// The builtin rows (Name/Image and their cf_ aliases, amendment A1) have no
	// FStoryFlowVariable storage — they live as plain fields on the def — so they can never
	// be answered here. That is correct for every caller: this function serves the map and
	// array paths, and the builtins are scalar strings. Builtin-aware access goes through
	// SetCharacterVariable / GetCharacterVariableValue below.
	return CharDef->Variables.Find(VariableName);
}

void FStoryFlowExecutionContext::SetCharacterVariable(const FString& CharacterPath, const FString& VariableName, const FStoryFlowVariant& Value)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character not found for SetCharacterVariable: %s"), *CharacterPath);
		return;
	}

	// Handle built-in "Name" field (or its reserved cf_name id — amendment A1: the cf_ ids
	// alias the builtin rows; all other character-variable access stays name-keyed)
	if (IsCharacterNameBuiltin(VariableName))
	{
		CharDef->Name = Value.ToString();
		return;
	}

	// Handle built-in "Image" field (or cf_image — amendment A1)
	if (IsCharacterImageBuiltin(VariableName))
	{
		CharDef->Image = Value.GetString();
		return;
	}

	// Find and set custom variable
	if (FStoryFlowVariable* Variable = CharDef->Variables.Find(VariableName))
	{
		Variable->Value = Value;
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *CharacterPath);
	}
}

FStoryFlowVariant FStoryFlowExecutionContext::GetCharacterVariableValue(const FString& CharacterPath, const FString& VariableName)
{
	FStoryFlowCharacterDef* CharDef = FindCharacter(CharacterPath);
	if (!CharDef)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Character not found for GetCharacterVariable: %s"), *CharacterPath);
		return FStoryFlowVariant();
	}

	// Handle built-in "Name" field (or cf_name — amendment A1, see SetCharacterVariable)
	if (IsCharacterNameBuiltin(VariableName))
	{
		FStoryFlowVariant Result;
		Result.SetString(CharDef->Name);
		return Result;
	}

	// Handle built-in "Image" field (or cf_image — amendment A1)
	if (IsCharacterImageBuiltin(VariableName))
	{
		FStoryFlowVariant Result;
		Result.SetString(CharDef->Image);
		return Result;
	}

	// Find custom variable
	if (const FStoryFlowVariable* Variable = CharDef->Variables.Find(VariableName))
	{
		return Variable->Value;
	}

	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found on character '%s'"), *VariableName, *CharacterPath);
	return FStoryFlowVariant();
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindEdgeBySourceHandle(const FString& SourceHandle) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindEdgeBySourceHandle(SourceHandle);
	}
	return nullptr;
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindEdgeBySource(const FString& SourceNodeId) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindEdgeBySource(SourceNodeId);
	}
	return nullptr;
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindInputEdge(const FString& NodeId, const FString& HandleSuffix) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindInputEdge(NodeId, HandleSuffix);
	}
	return nullptr;
}

const FStoryFlowConnection* FStoryFlowExecutionContext::FindEdgeByTarget(const FString& TargetNodeId) const
{
	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		return Script->FindEdgeByTarget(TargetNodeId);
	}
	return nullptr;
}

bool FStoryFlowExecutionContext::PushScript(const FString& ScriptPath, const FString& ReturnNodeId)
{
	if (IsAtMaxScriptDepth())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Max script nesting depth exceeded (%d)"), STORYFLOW_MAX_SCRIPT_DEPTH);
		return false;
	}

	UStoryFlowProjectAsset* Proj = Project.Get();
	if (!Proj)
	{
		return false;
	}

	UStoryFlowScriptAsset* NewScript = Proj->GetScriptByPath(ScriptPath);
	if (!NewScript)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Script not found: %s"), *ScriptPath);
		return false;
	}

	// Save current state. SavedVariables intentionally SHARES map storage with the
	// live locals (variant copy keeps the TSharedPtr): the HTML runtime's call
	// frames hold live variable references (runtime-core.js pushCallStack saves
	// gameState.variables.slice()), so aliasing established before a runScript
	// call survives the call and restore. Do NOT deep-copy here.
	FStoryFlowCallFrame Frame;
	Frame.ScriptPath = CurrentScript.IsValid() ? CurrentScript->ScriptPath : TEXT("");
	Frame.ReturnNodeId = ReturnNodeId;
	Frame.ScriptAsset = CurrentScript;
	Frame.SavedVariables = LocalVariables;

	// Save flow call stack (flows are in-script, so we preserve them when crossing script boundaries)
	for (const FStoryFlowFlowFrame& FlowFrame : FlowCallStack)
	{
		Frame.SavedFlowStack.Add(FlowFrame.FlowId);
	}

	CallStack.Push(Frame);

	// Clear flow stack for new script (each script has its own flow scope)
	FlowCallStack.Reset();

	// Switch to new script. Detach map storage from the asset — the HTML runtime
	// deep-copies + re-inflates script variables per invocation (runtime-state.js
	// SWITCH_SCRIPT), so each call gets fresh maps and never mutates the asset.
	CurrentScript = NewScript;
	LocalVariables = NewScript->Variables;
	DeepCopyMapVariables(LocalVariables);
	ResolveStringVariableValues(LocalVariables);
	CurrentNodeId = NewScript->StartNode;
	RebuildLocalNameIndex();

	return true;
}

bool FStoryFlowExecutionContext::PopScript()
{
	if (CallStack.Num() == 0)
	{
		return false;
	}

	FStoryFlowCallFrame Frame = CallStack.Pop();

	// Restore state
	if (Frame.ScriptAsset.IsValid())
	{
		CurrentScript = Frame.ScriptAsset;
		LocalVariables = Frame.SavedVariables;
		CurrentNodeId = Frame.ReturnNodeId;
		RebuildLocalNameIndex();

		// Restore flow call stack (flows are script-local)
		FlowCallStack.Reset();
		for (const FString& FlowId : Frame.SavedFlowStack)
		{
			FStoryFlowFlowFrame FlowFrame;
			FlowFrame.FlowId = FlowId;
			FlowCallStack.Push(FlowFrame);
		}
	}

	return true;
}

FString FStoryFlowExecutionContext::GetString(const FString& Key, const FString& LanguageCode) const
{
	UStoryFlowProjectAsset* Proj = Project.Get();

	// TIER 1, the localization overlay (spec §9). PROJECT-WIDE and ahead of the script table
	// because the sidecar's id namespace is project-wide: the export keys one row per shipped id
	// across every artifact it wrote, and nothing can key an id another artifact already claimed
	// for different text. The tables are FULL and PRE-RESOLVED, so this plugin computes no status
	// and no hash; a miss here simply means the source tiers answer.
	//
	// TRIPWIRE: this ladder and UStoryFlowProjectAsset::GetGlobalString's are deliberately
	// separate — this one probes the current script's table before the project globals, that one
	// has no script to probe — but the probes themselves must stay in lockstep. A probe ADDED or
	// REORDERED here must move there, and the reverse, or a string resolves one way inside
	// dialogue and another way outside it.
	//
	// THE LOOKUP RUNS ON THE AUTHORED TEMPLATE. Every caller that interpolates `{Variable}` tokens
	// calls InterpolateVariables on the RESULT of this function, never the other way round — a
	// translated line is authored with the same tokens as the source line, so interpolating first
	// would hand this lookup a string no table was ever keyed by. That failure is invisible: the
	// text still renders, in the source language, only for lines that happen to carry a token.
	if (Proj)
	{
		if (const FString* Localized = Proj->FindLocalizedString(Key, LanguageCode))
		{
			return *Localized;
		}
	}

	// TIER 2, the keying artifact's own source table — current script first (check map directly
	// to avoid key-echo fragility), then the project globals characters.json merges into. The
	// language-prefixed probe is the pre-localization behavior, unchanged; the source-language
	// probe beside it is what makes the fall-through work once the language being read is a
	// target language, since every artifact this editor exports keys its strings by the source
	// language alone. The two probes are the same key whenever the codes agree.
	const FString FullKey = FString::Printf(TEXT("%s.%s"), *LanguageCode, *Key);
	const FString SourceKey = Proj ? FString::Printf(TEXT("%s.%s"), *Proj->SourceLanguage, *Key) : FullKey;

	if (UStoryFlowScriptAsset* Script = CurrentScript.Get())
	{
		if (const FString* Value = Script->Strings.Find(FullKey))
		{
			return *Value;
		}
		if (const FString* Value = Script->Strings.Find(SourceKey))
		{
			return *Value;
		}
		if (const FString* Value = Script->Strings.Find(Key))
		{
			return *Value;
		}
	}

	// Try project global strings
	if (Proj)
	{
		if (const FString* Value = Proj->GlobalStrings.Find(FullKey))
		{
			return *Value;
		}
		if (const FString* Value = Proj->GlobalStrings.Find(SourceKey))
		{
			return *Value;
		}
		if (const FString* Value = Proj->GlobalStrings.Find(Key))
		{
			return *Value;
		}
	}

	// TIER 3: the raw stored value, never an accidental empty string (see GetGlobalString).
	return Key;
}

FString FStoryFlowExecutionContext::InterpolateVariables(const FString& Text) const
{
	FString Result = Text;

	// Early out if no interpolation needed
	if (!Result.Contains(TEXT("{")))
	{
		return Result;
	}

	// Build display-name -> variable lookup maps once (O(n) total instead of O(n) per token)
	// Each entry maps display name AND id to the variable pointer
	TMap<FString, const FStoryFlowVariable*> VarLookup;

	// Local variables (higher priority - added first, won't be overwritten)
	for (const auto& Pair : LocalVariables)
	{
		VarLookup.Add(Pair.Value.Name, &Pair.Value);
		VarLookup.Add(Pair.Value.Id, &Pair.Value);
	}

	// Global variables (lower priority - only added if key not already present)
	const TMap<FString, FStoryFlowVariable>* GlobalVars = ExternalGlobalVariables;
	if (!GlobalVars && Project.IsValid())
	{
		GlobalVars = &Project->GlobalVariables;
	}

	if (GlobalVars)
	{
		for (const auto& Pair : *GlobalVars)
		{
			if (!VarLookup.Contains(Pair.Value.Name))
			{
				VarLookup.Add(Pair.Value.Name, &Pair.Value);
			}
			if (!VarLookup.Contains(Pair.Value.Id))
			{
				VarLookup.Add(Pair.Value.Id, &Pair.Value);
			}
		}
	}

	// Pattern: {variableName}
	int32 StartIndex = 0;
	while (true)
	{
		int32 OpenBrace = Result.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIndex);
		if (OpenBrace == INDEX_NONE)
		{
			break;
		}

		int32 CloseBrace = Result.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenBrace);
		if (CloseBrace == INDEX_NONE)
		{
			break;
		}

		FString VarName = Result.Mid(OpenBrace + 1, CloseBrace - OpenBrace - 1);
		FString Replacement;

		// Check for Character.property pattern
		if (VarName.StartsWith(TEXT("Character.")))
		{
			FString PropertyName = VarName.RightChop(10);
			if (PropertyName.Equals(TEXT("name"), ESearchCase::IgnoreCase))
			{
				Replacement = CurrentDialogueState.Character.Name;
			}
			else if (const FStoryFlowVariant* CharVar = CurrentDialogueState.Character.Variables.Find(PropertyName))
			{
				Replacement = CharVar->ToString();
			}
		}
		else if (VarName.Contains(TEXT(".")))
		{
			// Handle nested variable (charVar.innerVar)
			// This is for character-type variables: {protagonist.Health}
			int32 DotIndex;
			VarName.FindChar(TEXT('.'), DotIndex);
			FString CharVarName = VarName.Left(DotIndex);
			FString InnerVarName = VarName.RightChop(DotIndex + 1);

			// Find the character-type variable via lookup map
			const FStoryFlowVariable* const* FoundVar = VarLookup.Find(CharVarName);
			if (FoundVar && *FoundVar && (*FoundVar)->Type == EStoryFlowVariableType::Character)
			{
				FString CharacterPath = (*FoundVar)->Value.GetString();
				if (!CharacterPath.IsEmpty())
				{
					// Normalize path for lookup
					FString NormalizedPath = NormalizeCharacterPath(CharacterPath);

					// Find character definition from runtime characters
					const FStoryFlowCharacterDef* CharDef = nullptr;
					if (ExternalCharacters)
					{
						CharDef = ExternalCharacters->Find(NormalizedPath);
					}

					if (CharDef)
					{
						// Handle built-in "Name" property (resolve through string table; cf_name
						// alias — amendment A2a). NAME-OR-CF SPELLINGS ONLY on this branch: the
						// custom-variable row below tolerates ids by design (an HTML-reference
						// asymmetry recorded as deliberate), and that tolerance must not creep
						// into the builtins.
						if (IsCharacterNameBuiltin(InnerVarName))
						{
							Replacement = GetString(CharDef->Name);
						}
						// Handle built-in "Image" property (or cf_image — amendment A2a)
						else if (IsCharacterImageBuiltin(InnerVarName))
						{
							Replacement = CharDef->Image;
						}
						else
						{
							// Look up custom variable by name
							for (const auto& VarPair : CharDef->Variables)
							{
								if (VarPair.Value.Name == InnerVarName || VarPair.Value.Id == InnerVarName)
								{
									Replacement = VarPair.Value.Value.ToString();
									break;
								}
							}
						}
					}
				}
			}
		}
		else
		{
			// Regular variable lookup via pre-built map (O(1) instead of O(n))
			if (const FStoryFlowVariable* const* FoundVar = VarLookup.Find(VarName))
			{
				Replacement = (*FoundVar)->Value.ToString();
			}
		}

		// Replace the pattern
		FString Pattern = FString::Printf(TEXT("{%s}"), *VarName);
		Result = Result.Replace(*Pattern, *Replacement);

		StartIndex = OpenBrace + Replacement.Len();
	}

	return Result;
}

void FStoryFlowExecutionContext::ResolveStringVariableValues(TMap<FString, FStoryFlowVariable>& Variables) const
{
	for (auto& VarPair : Variables)
	{
		if (VarPair.Value.Type == EStoryFlowVariableType::String)
		{
			if (VarPair.Value.bIsArray)
			{
				for (auto& Element : VarPair.Value.Value.GetArrayMutable())
				{
					FString Key = Element.GetString();
					if (!Key.IsEmpty())
					{
						Element.SetString(GetString(Key, SeedLanguageCode));
					}
				}
			}
			else
			{
				FString Key = VarPair.Value.Value.GetString();
				if (!Key.IsEmpty())
				{
					VarPair.Value.Value.SetString(GetString(Key, SeedLanguageCode));
				}
			}
		}
		else if (VarPair.Value.Type == EStoryFlowVariableType::Map &&
			VarPair.Value.ValueType == EStoryFlowVariableType::String)
		{
			// Map entry VALUES of string valueType store exported strings-table keys
			// verbatim (importer ParseMapEntries) — resolve them at this load boundary,
			// the identical path/timing scalar string variables use, against the same
			// table scoping (current script's strings, then project globals). KEYS are
			// raw identifiers and must NEVER resolve, regardless of keyType. In-place
			// value rewrite preserves entry order.
			if (VarPair.Value.Value.IsMap())
			{
				for (FStoryFlowMapEntry& Entry : VarPair.Value.Value.GetMapMutable())
				{
					FString Key = Entry.Value.GetString();
					if (!Key.IsEmpty())
					{
						Entry.Value.SetString(GetString(Key, SeedLanguageCode));
					}
				}
			}
		}
	}
}

void FStoryFlowExecutionContext::RebuildLocalNameIndex()
{
	LocalVariableNameIndex.Empty(LocalVariables.Num());
	for (const auto& Pair : LocalVariables)
	{
		if (!Pair.Value.Name.IsEmpty())
		{
			LocalVariableNameIndex.Add(Pair.Value.Name, Pair.Key);
		}
	}
}

void FStoryFlowExecutionContext::RebuildGlobalNameIndex()
{
	const TMap<FString, FStoryFlowVariable>* GlobalVars = ExternalGlobalVariables;
	if (!GlobalVars && Project.IsValid())
	{
		GlobalVars = &Project->GlobalVariables;
	}

	if (GlobalVars)
	{
		GlobalVariableNameIndex.Empty(GlobalVars->Num());
		for (const auto& Pair : *GlobalVars)
		{
			if (!Pair.Value.Name.IsEmpty())
			{
				GlobalVariableNameIndex.Add(Pair.Value.Name, Pair.Key);
			}
		}
	}
	else
	{
		GlobalVariableNameIndex.Empty();
	}
}

FStoryFlowVariable* FStoryFlowExecutionContext::FindVariableByName(const FString& VariableName, bool bIsGlobal)
{
	TMap<FString, FString>& Index = bIsGlobal ? GlobalVariableNameIndex : LocalVariableNameIndex;

	// Fast path: O(1) index lookup
	if (const FString* FoundId = Index.Find(VariableName))
	{
		if (FStoryFlowVariable* Var = FindVariable(*FoundId, bIsGlobal))
		{
			return Var;
		}
		// ID in index but not in map — index is stale, fall through to scan
	}

	// Slow path: linear scan + index repair (handles stale index after global reset/load)
	TMap<FString, FStoryFlowVariable>* VarMap = nullptr;
	if (bIsGlobal)
	{
		VarMap = ExternalGlobalVariables;
		if (!VarMap && Project.IsValid())
		{
			VarMap = &Project->GlobalVariables;
		}
	}
	else
	{
		VarMap = &LocalVariables;
	}

	if (VarMap)
	{
		for (auto& Pair : *VarMap)
		{
			if (Pair.Value.Name == VariableName)
			{
				// Repair index entry
				Index.Add(VariableName, Pair.Key);
				return &Pair.Value;
			}
		}
	}

	UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Variable '%s' not found by name (bIsGlobal=%s)"),
		*VariableName, bIsGlobal ? TEXT("true") : TEXT("false"));
	return nullptr;
}

void FStoryFlowExecutionContext::ClearEvaluationCache()
{
	for (auto& Pair : NodeRuntimeStates)
	{
		Pair.Value.bHasCachedOutput = false;
		Pair.Value.CachedOutput.Reset();
	}
}
