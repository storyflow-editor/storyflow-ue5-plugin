// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Evaluation/StoryFlowExecutionContext.h"
#include "Evaluation/StoryFlowEvaluator.h"
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
	SeenSharedRevision = InDataAssetStore.SharedState ? InDataAssetStore.SharedState->Revision : 0;
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
	CallerActivations.Empty();
	RollbackRandomState.Reset();
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
	MarkReadFailure();
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
		NotifyStateChanged();
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
	const UStoryFlowProjectAsset* ReadProject = DataAssetStore.SharedState ? DataAssetStore.SharedState->Project.Get() : Project.Get();
	return StoryFlowDataAssets::TryRead(DataAssetStore, ReadProject, ActiveLanguage ? *ActiveLanguage : FString(), AssetId, VariableId, OutValue);
}

bool FStoryFlowExecutionContext::TrySetDataAsset(const FString& AssetId, const FString& VariableId, const FStoryFlowVariant& Value)
{
	if (!DataAssetStore.IsValid())
	{
		return false;
	}
	return StoryFlowDataAssets::TrySet(DataAssetStore, AssetId, VariableId, Value);
}

TArray<FString> FStoryFlowExecutionContext::GetDataAssetVariableNames(const FString& AssetId) const
{
	// Null-check-and-forward, the same shape as the accessors above: a context with no store
	// answers "no names" instead of making the caller repeat the guard.
	if (!DataAssetStore.IsValid())
	{
		return TArray<FString>();
	}
	return StoryFlowDataAssets::VariableNames(*DataAssetStore.Seed, AssetId);
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

	FStoryFlowExecutionContext* Mutable = const_cast<FStoryFlowExecutionContext*>(this);
	FStoryFlowEvaluator Evaluator(Mutable);
	return Evaluator.EvaluateDataAssetFromNode(Mutable->GetNode(Edge->Source), Accessor.Id, Edge->SourceHandle);
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
		CharDef->bNameIsLiteral = true;
		NotifyStateChanged();
		return;
	}

	// Handle built-in "Image" field (or cf_image — amendment A1)
	if (IsCharacterImageBuiltin(VariableName))
	{
		CharDef->Image = Value.GetString();
		NotifyStateChanged();
		return;
	}

	// Find and set custom variable
	if (FStoryFlowVariable* Variable = CharDef->Variables.Find(VariableName))
	{
		Variable->Value = Value;
		NotifyStateChanged();
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
		Result.SetString(ResolveCharacterName(*CharDef));
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

int32 FStoryFlowExecutionContext::GetMaxScriptDepth() const
{
	const UStoryFlowProjectAsset* Proj = DataAssetStore.SharedState ? DataAssetStore.SharedState->Project.Get() : Project.Get();
	const int32 Limit = Proj ? Proj->Metadata.MaxScriptNesting : STORYFLOW_MAX_SCRIPT_DEPTH;
	return Limit >= 1 && Limit <= 100 ? Limit : STORYFLOW_MAX_SCRIPT_DEPTH;
}

bool FStoryFlowExecutionContext::PushScript(const FString& ScriptPath, const FString& ReturnNodeId)
{
	if (IsAtMaxScriptDepth())
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow Editor: Max script nesting depth exceeded (%d) when calling '%s'. Check for recursive script calls or adjust Maximum Script Nesting in project settings."), GetMaxScriptDepth(), *ScriptPath);
		return false;
	}

	UStoryFlowProjectAsset* Proj = DataAssetStore.SharedState ? DataAssetStore.SharedState->Project.Get() : Project.Get();
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
	FStoryFlowActivationState Activation;
	Activation.Loops = MoveTemp(LoopStack); Activation.Nodes = MoveTemp(NodeRuntimeStates);
	CallerActivations.Add(MoveTemp(Activation));

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
	if (CallerActivations.Num() > 0)
	{
		auto Activation = CallerActivations.Pop();
		LoopStack = MoveTemp(Activation.Loops); NodeRuntimeStates = MoveTemp(Activation.Nodes);
	}
	// Caller memo predates callee writes; execution outputs and loop fields remain intact.
	ClearEvaluationCache();

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

FString FStoryFlowExecutionContext::GetString(const FString& Key) const
{
	const UStoryFlowProjectAsset* Proj = DataAssetStore.SharedState ? DataAssetStore.SharedState->Project.Get() : Project.Get();
	return GetString(Key, Proj && Proj->bHasLocalization && ActiveLanguage ? *ActiveLanguage : SeedLanguageCode);
}

FString FStoryFlowExecutionContext::GetString(const FString& Key, const FString& LanguageCode) const
{
	UStoryFlowProjectAsset* Proj = DataAssetStore.SharedState ? DataAssetStore.SharedState->Project.Get() : Project.Get();

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
	// Older localized exports stored source text under en, regardless of their source label.
	const FString LegacySourceKey = FString::Printf(TEXT("en.%s"), *Key);

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
		if (const FString* Value = Script->Strings.Find(LegacySourceKey))
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
		if (const FString* Value = Proj->GlobalStrings.Find(LegacySourceKey))
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
	TMap<FString, const FStoryFlowVariable*> Roots;
	const auto* Globals = ExternalGlobalVariables ? ExternalGlobalVariables : (Project.IsValid() ? &Project->GlobalVariables : nullptr);
	if (Globals) { for (const auto& Pair : *Globals) { Roots.Add(Pair.Value.Name, &Pair.Value); Roots.Add(Pair.Value.Id, &Pair.Value); } }
	for (const auto& Pair : LocalVariables) { Roots.Add(Pair.Value.Name, &Pair.Value); Roots.Add(Pair.Value.Id, &Pair.Value); }
	auto Leaf = [](const FStoryFlowVariable& V, FString& Out) {
		if (V.bIsArray || V.Value.IsArray()) { return false; }
		switch (V.Type) {
		case EStoryFlowVariableType::Boolean: case EStoryFlowVariableType::Integer:
		case EStoryFlowVariableType::Float: case EStoryFlowVariableType::String: case EStoryFlowVariableType::Enum:
			Out = V.Value.ToString(); return true;
		default: return false;
		}
	};
	auto Field = [&](EStoryFlowVariableType Type, const FString& Ref, const FString& Name, FStoryFlowVariable& Out) {
		if (Type == EStoryFlowVariableType::DataAsset) {
			if (!DataAssetStore.IsValid()) { return false; }
			const auto* Decl = StoryFlowDataAssets::FindDeclarationByName(*DataAssetStore.Seed, Ref, Name);
			if (!Decl) { return false; }
			Out = *Decl; return TryResolveDataAsset(Ref, Decl->Id, Out.Value);
		}
		if (Type != EStoryFlowVariableType::Character) { return false; }
		FString Key = NormalizeCharacterPath(Ref);
		if (IsCharacterIdRef(Ref)) {
			const FString* Path = CharacterIdToPath ? CharacterIdToPath->Find(Ref) : nullptr;
			if (!Path) { return false; } Key = *Path;
		}
		const auto* Character = ExternalCharacters ? ExternalCharacters->Find(Key) : nullptr;
		if (!Character) {
			// Legacy callers can provide a presentation-only speaker without a live ref.
			if (!Ref.IsEmpty() || !CurrentDialogueState.Character.CharacterPath.IsEmpty()) { return false; }
			if (Name.Equals(TEXT("name"), ESearchCase::IgnoreCase)) {
				Out.Type = EStoryFlowVariableType::String; Out.Value.SetString(CurrentDialogueState.Character.Name); return true;
			}
			if (const auto* Value = CurrentDialogueState.Character.Variables.Find(Name)) { Out.Type = Value->GetType(); Out.Value = *Value; return true; }
			return false;
		}
		if (IsCharacterNameBuiltin(Name)) {
			Out.Type = EStoryFlowVariableType::String;
			Out.Value.SetString(ResolveCharacterName(*Character)); return true;
		}
		for (const auto& Pair : Character->Variables) {
			if (Pair.Value.Name.Equals(Name, ESearchCase::CaseSensitive) || Pair.Value.Id.Equals(Name, ESearchCase::CaseSensitive)) { Out = Pair.Value; return true; }
		}
		return false;
	};
	auto Resolve = [&](const FString& Path, FString& Out) {
		EStoryFlowVariableType Type; FString Ref, Remaining;
		if (Path.StartsWith(TEXT("Character."), ESearchCase::CaseSensitive)) {
			Type = EStoryFlowVariableType::Character; Ref = CurrentDialogueState.Character.CharacterPath; Remaining = Path.Mid(10).TrimStartAndEnd();
		} else {
			int32 Dot = INDEX_NONE;
			if (!Path.FindChar(TEXT('.'), Dot)) { const auto* V = Roots.Find(Path); return V && Leaf(**V, Out); }
			const auto* V = Roots.Find(Path.Left(Dot));
			if (!V || (*V)->bIsArray || (*V)->Value.IsArray()) { return false; }
			Type = (*V)->Type; Ref = (*V)->Value.GetString(); Remaining = Path.Mid(Dot + 1);
			if (Ref.IsEmpty()) { return false; }
		}
		while (!Remaining.IsEmpty()) {
			FStoryFlowVariable V;
			if (Field(Type, Ref, Remaining, V)) { return Leaf(V, Out); }
			int32 Dot = INDEX_NONE;
			if (!Remaining.FindChar(TEXT('.'), Dot) || !Field(Type, Ref, Remaining.Left(Dot), V) || V.bIsArray || V.Value.IsArray()) { return false; }
			Type = V.Type; Ref = V.Value.GetString(); Remaining = Remaining.Mid(Dot + 1);
			if (Ref.IsEmpty()) { return false; }
		}
		return false;
	};
	FString Result; int32 Position = 0;
	while (Position < Text.Len()) {
		const int32 Open = Text.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Position);
		const int32 Close = Open == INDEX_NONE ? INDEX_NONE : Text.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
		if (Close == INDEX_NONE) { Result += Text.Mid(Position); break; }
		Result += Text.Mid(Position, Open - Position);
		FString Replacement;
		const FString Path = Text.Mid(Open + 1, Close - Open - 1).TrimStartAndEnd();
		Result += Resolve(Path, Replacement) ? Replacement : Text.Mid(Open, Close - Open + 1);
		Position = Close + 1;
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
	RefreshSharedState();
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
		if (Pair.Value.bIsExecutionOutput) { continue; }
		Pair.Value.bHasCachedOutput = false;
		Pair.Value.CachedOutput.Reset();
	}
}

void FStoryFlowExecutionContext::RefreshSharedState()
{
	if (DataAssetStore.SharedState && SeenSharedRevision != DataAssetStore.SharedState->Revision)
	{
		SeenSharedRevision = DataAssetStore.SharedState->Revision;
		// Retain the running script/local snapshot, but rebind every project-based read together.
		Project = DataAssetStore.SharedState->Project;
		RebuildGlobalNameIndex();
		ClearEvaluationCache();
	}
}

void FStoryFlowExecutionContext::NotifyStateChanged()
{
	DataAssetStore.NotifyChanged();
	ClearEvaluationCache();
}

uint32 FStoryFlowExecutionContext::NextRollbackRandom()
{
	uint32 State = RollbackRandomState.GetValue();
	if (State == 0) State = 1;
	State ^= State << 13; State ^= State >> 17; State ^= State << 5;
	RollbackRandomState = State; return State;
}
int32 FStoryFlowExecutionContext::RandomInt(int32 Min, int32 Max)
{
	if (!RollbackRandomState.IsSet()) return FMath::RandRange(Min, Max);
	if (Min > Max) Swap(Min, Max);
	return int32(int64(Min) + int64((double(NextRollbackRandom()) / 4294967296.0) * (int64(Max)-Min+1)));
}
float FStoryFlowExecutionContext::RandomFloat(float Min, float Max)
{
	if (!RollbackRandomState.IsSet()) return FMath::FRandRange(Min, Max);
	return Min + float(double(NextRollbackRandom()) / 4294967296.0) * (Max-Min);
}
FString FStoryFlowExecutionContext::ResolveCharacterName(const FStoryFlowCharacterDef& Character) const
{
	if (Character.bNameIsLiteral) return Character.Name;
	const auto* P = DataAssetStore.SharedState ? DataAssetStore.SharedState->Project.Get() : Project.Get();
	return P ? P->GetGlobalString(Character.Name, P->bHasLocalization && ActiveLanguage ? *ActiveLanguage : SeedLanguageCode) : Character.Name;
}
