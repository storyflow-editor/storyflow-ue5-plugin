// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowDataAssetStore.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowRuntime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// File-local JSON serialization helpers
//
// These convert between StoryFlow runtime types and JSON for save/load.
// JSON is required because FStoryFlowVariant::ArrayValue is not a UPROPERTY,
// so Unreal's built-in serialization drops array data.
// ============================================================================

namespace StoryFlowSaveHelpers
{

// --- Variable Type string conversion ---

FString VariableTypeToString(EStoryFlowVariableType Type)
{
	switch (Type)
	{
	case EStoryFlowVariableType::Boolean:   return TEXT("Boolean");
	case EStoryFlowVariableType::Integer:   return TEXT("Integer");
	case EStoryFlowVariableType::Float:     return TEXT("Float");
	case EStoryFlowVariableType::String:    return TEXT("String");
	case EStoryFlowVariableType::Enum:      return TEXT("Enum");
	case EStoryFlowVariableType::Image:     return TEXT("Image");
	case EStoryFlowVariableType::Audio:     return TEXT("Audio");
	case EStoryFlowVariableType::DataAsset: return TEXT("DataAsset");
	case EStoryFlowVariableType::Character: return TEXT("Character");
	case EStoryFlowVariableType::Map:       return TEXT("Map");
	default:                                return TEXT("None");
	}
}

/**
 * VariableTypeToString's inverse, and deliberately its own table rather than a reuse of
 * ParseVariableType (StoryFlowTypes.h). TWO VOCABULARIES: that one parses the EXPORTER's
 * lowercase wire tokens ("boolean"), these are the SAVE format's capitalized names ("Boolean").
 * Folding them together would make each silently accept the other's spelling.
 *
 * BOTH ARE EXACT. Matching here is case-sensitive, like the wire table and like the siblings:
 * Unity parses save type names with Enum.TryParse(ignoreCase: false), Godot with an exact
 * dictionary match. `==` on FString is case-INSENSITIVE, so this used to accept "BOOLEAN" and
 * even the wire's own "boolean" — spellings no engine writes and two of the four reject.
 *
 * None for anything else, "None" itself included: that is what VariableTypeToString writes for a
 * type it cannot name, and a record no engine can use is skipped by the reader rather than
 * guessed at (see VariableFromJson).
 */
EStoryFlowVariableType StringToVariableType(const FString& Str)
{
	struct FTypeName
	{
		FString Name;
		EStoryFlowVariableType Type;
	};

	static const FTypeName TypeNames[] = {
		{ TEXT("Boolean"),   EStoryFlowVariableType::Boolean },
		{ TEXT("Integer"),   EStoryFlowVariableType::Integer },
		{ TEXT("Float"),     EStoryFlowVariableType::Float },
		{ TEXT("String"),    EStoryFlowVariableType::String },
		{ TEXT("Enum"),      EStoryFlowVariableType::Enum },
		{ TEXT("Image"),     EStoryFlowVariableType::Image },
		{ TEXT("Audio"),     EStoryFlowVariableType::Audio },
		{ TEXT("Character"), EStoryFlowVariableType::Character },
		{ TEXT("DataAsset"), EStoryFlowVariableType::DataAsset },
		{ TEXT("Map"),       EStoryFlowVariableType::Map },
	};

	for (const FTypeName& TypeName : TypeNames)
	{
		if (Str.Equals(TypeName.Name, ESearchCase::CaseSensitive))
		{
			return TypeName.Type;
		}
	}
	return EStoryFlowVariableType::None;
}

// --- FStoryFlowVariant <-> JSON ---

TSharedPtr<FJsonValue> VariantToJson(const FStoryFlowVariant& Variant)
{
	switch (Variant.GetType())
	{
	case EStoryFlowVariableType::Boolean:
		return MakeShared<FJsonValueBoolean>(Variant.GetBool());

	case EStoryFlowVariableType::Integer:
		return MakeShared<FJsonValueNumber>(static_cast<double>(Variant.GetInt()));

	case EStoryFlowVariableType::Float:
		return MakeShared<FJsonValueNumber>(static_cast<double>(Variant.GetFloat()));

	case EStoryFlowVariableType::String:
	case EStoryFlowVariableType::Enum:
	case EStoryFlowVariableType::Image:
	case EStoryFlowVariableType::Audio:
	case EStoryFlowVariableType::DataAsset:
	case EStoryFlowVariableType::Character:
		return MakeShared<FJsonValueString>(Variant.GetString());

	default:
		return MakeShared<FJsonValueNull>();
	}
}

FStoryFlowVariant VariantFromJson(const TSharedPtr<FJsonValue>& JsonValue, EStoryFlowVariableType Type)
{
	FStoryFlowVariant Result;

	if (!JsonValue.IsValid())
	{
		return Result;
	}

	switch (Type)
	{
	case EStoryFlowVariableType::Boolean:
		Result.SetBool(JsonValue->AsBool());
		break;

	case EStoryFlowVariableType::Integer:
		Result.SetInt(static_cast<int32>(JsonValue->AsNumber()));
		break;

	case EStoryFlowVariableType::Float:
		Result.SetFloat(static_cast<float>(JsonValue->AsNumber()));
		break;

	case EStoryFlowVariableType::String:
	case EStoryFlowVariableType::Image:
	case EStoryFlowVariableType::Audio:
	case EStoryFlowVariableType::DataAsset:
	case EStoryFlowVariableType::Character:
		if (Type == EStoryFlowVariableType::DataAsset) { Result.SetDataAsset(JsonValue->AsString()); } else { Result.SetString(JsonValue->AsString()); }
		break;

	case EStoryFlowVariableType::Enum:
		Result.SetEnum(JsonValue->AsString());
		break;

	default:
		break;
	}

	return Result;
}

// --- FStoryFlowVariable <-> JSON ---

TSharedPtr<FJsonObject> VariableToJson(const FStoryFlowVariable& Variable)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	Obj->SetStringField(TEXT("id"), Variable.Id);
	Obj->SetStringField(TEXT("name"), Variable.Name);
	Obj->SetStringField(TEXT("type"), VariableTypeToString(Variable.Type));
	Obj->SetBoolField(TEXT("isArray"), Variable.bIsArray);

	if (Variable.Type == EStoryFlowVariableType::Map)
	{
		// Map shape mirrors the importer's: keyType/valueType plus an ordered
		// [{key, value}, ...] entry array, each variant typed per K/V. Entry
		// VALUES are whatever is in memory at save time — i.e. already RESOLVED
		// runtime strings (resolution happens once at the asset->runtime load
		// boundary), matching what scalar string variables persist. KEYS are raw
		// identifiers and are never strings-table-resolved.
		Obj->SetStringField(TEXT("keyType"), VariableTypeToString(Variable.KeyType));
		Obj->SetStringField(TEXT("valueType"), VariableTypeToString(Variable.ValueType));

		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const FStoryFlowMapEntry& Entry : Variable.Value.GetMap())
		{
			TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetField(TEXT("key"), VariantToJson(Entry.Key));
			EntryObj->SetField(TEXT("value"), VariantToJson(Entry.Value));
			Entries.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		Obj->SetArrayField(TEXT("value"), Entries);
	}
	else if (Variable.bIsArray)
	{
		TArray<TSharedPtr<FJsonValue>> ArrayValues;
		for (const FStoryFlowVariant& Element : Variable.Value.GetArray())
		{
			ArrayValues.Add(VariantToJson(Element));
		}
		Obj->SetArrayField(TEXT("value"), ArrayValues);
	}
	else
	{
		Obj->SetField(TEXT("value"), VariantToJson(Variable.Value));
	}

	if (Variable.EnumValues.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> EnumVals;
		for (const FString& EnumVal : Variable.EnumValues)
		{
			EnumVals.Add(MakeShared<FJsonValueString>(EnumVal));
		}
		Obj->SetArrayField(TEXT("enumValues"), EnumVals);
	}

	// Map K/V enum value lists (mirrors the scalar enumValues handling above)
	if (Variable.KeyEnumValues.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeyEnumVals;
		for (const FString& EnumVal : Variable.KeyEnumValues)
		{
			KeyEnumVals.Add(MakeShared<FJsonValueString>(EnumVal));
		}
		Obj->SetArrayField(TEXT("keyEnumValues"), KeyEnumVals);
	}
	if (Variable.ValueEnumValues.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ValueEnumVals;
		for (const FString& EnumVal : Variable.ValueEnumValues)
		{
			ValueEnumVals.Add(MakeShared<FJsonValueString>(EnumVal));
		}
		Obj->SetArrayField(TEXT("valueEnumValues"), ValueEnumVals);
	}

	return Obj;
}

/**
 * One saved variable record. A None Type means the record named a type this build does not know
 * — a case variant, a future type, the literal "None" — and CALLERS MUST SKIP IT: an unusable
 * record is dropped on the way in, never guessed at, and never allowed to overwrite the
 * declaration the project already carries. Unity's TryVariableValueFromJson returns false and
 * Godot's _variable_from_json returns {} for the same reason; a document carrying one still
 * loads, minus that record.
 *
 * Only the record's own type gates this. A map's key/value types stay tolerant — absent or
 * unknown keeps the struct default (String), matching both siblings.
 */
FStoryFlowVariable VariableFromJson(const TSharedPtr<FJsonObject>& Obj)
{
	FStoryFlowVariable Variable;

	if (!Obj.IsValid())
	{
		return Variable;
	}

	Variable.Id = Obj->GetStringField(TEXT("id"));
	Variable.Name = Obj->GetStringField(TEXT("name"));
	Variable.Type = StringToVariableType(Obj->GetStringField(TEXT("type")));
	Variable.bIsArray = Obj->GetBoolField(TEXT("isArray"));

	if (Variable.Type == EStoryFlowVariableType::Map)
	{
		// Tolerant K/V type parse — absent/unknown strings keep the struct defaults (String)
		FString KVTypeStr;
		if (Obj->TryGetStringField(TEXT("keyType"), KVTypeStr))
		{
			const EStoryFlowVariableType Parsed = StringToVariableType(KVTypeStr);
			if (Parsed != EStoryFlowVariableType::None)
			{
				Variable.KeyType = Parsed;
			}
		}
		if (Obj->TryGetStringField(TEXT("valueType"), KVTypeStr))
		{
			const EStoryFlowVariableType Parsed = StringToVariableType(KVTypeStr);
			if (Parsed != EStoryFlowVariableType::None)
			{
				Variable.ValueType = Parsed;
			}
		}

		// Entry array: absent or malformed degrades to an empty map with Type
		// preserved. Saved values were resolved at load time, so they round-trip
		// as-is — no strings-table resolution on the load path.
		TArray<FStoryFlowMapEntry> Entries;
		const TArray<TSharedPtr<FJsonValue>>* EntryValues;
		if (Obj->TryGetArrayField(TEXT("value"), EntryValues))
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *EntryValues)
			{
				const TSharedPtr<FJsonObject>* EntryObj;
				if (!EntryValue->TryGetObject(EntryObj))
				{
					continue;
				}
				// An entry without a key is unaddressable — skip it (matches the importer)
				const TSharedPtr<FJsonValue> KeyField = (*EntryObj)->TryGetField(TEXT("key"));
				if (!KeyField.IsValid())
				{
					continue;
				}
				FStoryFlowMapEntry Entry;
				Entry.Key = VariantFromJson(KeyField, Variable.KeyType);
				Entry.Value = VariantFromJson((*EntryObj)->TryGetField(TEXT("value")), Variable.ValueType);
				Entries.Add(Entry);
			}
		}
		Variable.Value.SetMap(Entries);
	}
	else if (Variable.bIsArray)
	{
		TArray<FStoryFlowVariant> ArrayElements;
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues;
		if (Obj->TryGetArrayField(TEXT("value"), ArrayValues))
		{
			for (const TSharedPtr<FJsonValue>& Element : *ArrayValues)
			{
				ArrayElements.Add(VariantFromJson(Element, Variable.Type));
			}
		}
		Variable.Value.SetArray(ArrayElements);
	}
	else
	{
		Variable.Value = VariantFromJson(Obj->TryGetField(TEXT("value")), Variable.Type);
	}

	const TArray<TSharedPtr<FJsonValue>>* EnumVals;
	if (Obj->TryGetArrayField(TEXT("enumValues"), EnumVals))
	{
		for (const TSharedPtr<FJsonValue>& Val : *EnumVals)
		{
			Variable.EnumValues.Add(Val->AsString());
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* KeyEnumVals;
	if (Obj->TryGetArrayField(TEXT("keyEnumValues"), KeyEnumVals))
	{
		for (const TSharedPtr<FJsonValue>& Val : *KeyEnumVals)
		{
			Variable.KeyEnumValues.Add(Val->AsString());
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ValueEnumVals;
	if (Obj->TryGetArrayField(TEXT("valueEnumValues"), ValueEnumVals))
	{
		for (const TSharedPtr<FJsonValue>& Val : *ValueEnumVals)
		{
			Variable.ValueEnumValues.Add(Val->AsString());
		}
	}

	return Variable;
}

// --- FStoryFlowCharacterDef <-> JSON ---

TSharedPtr<FJsonObject> CharacterDefToJson(const FStoryFlowCharacterDef& CharDef)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	Obj->SetStringField(TEXT("name"), CharDef.Name);
	Obj->SetBoolField(TEXT("nameIsLiteral"), CharDef.bNameIsLiteral);
	Obj->SetStringField(TEXT("image"), CharDef.Image);

	if (CharDef.Variables.Num() > 0)
	{
		TSharedPtr<FJsonObject> VarsObj = MakeShared<FJsonObject>();
		for (const auto& VarPair : CharDef.Variables)
		{
			VarsObj->SetObjectField(VarPair.Key, VariableToJson(VarPair.Value));
		}
		Obj->SetObjectField(TEXT("variables"), VarsObj);
	}

	return Obj;
}

FStoryFlowCharacterDef CharacterDefFromJson(const TSharedPtr<FJsonObject>& Obj)
{
	FStoryFlowCharacterDef CharDef;

	if (!Obj.IsValid())
	{
		return CharDef;
	}

	CharDef.Name = Obj->GetStringField(TEXT("name"));
	// Legacy saves cannot distinguish an authored key from an identical player-written name.
	CharDef.bNameIsLiteral = true;
	Obj->TryGetBoolField(TEXT("nameIsLiteral"), CharDef.bNameIsLiteral);
	CharDef.Image = Obj->GetStringField(TEXT("image"));

	const TSharedPtr<FJsonObject>* VarsObj;
	if (Obj->TryGetObjectField(TEXT("variables"), VarsObj))
	{
		for (const auto& VarPair : (*VarsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* VarObj;
			if (VarPair.Value->TryGetObject(VarObj))
			{
				// A record whose type this build cannot name is dropped, not stored untyped
				const FStoryFlowVariable Parsed = VariableFromJson(*VarObj);
				if (Parsed.Type != EStoryFlowVariableType::None)
				{
					CharDef.Variables.Add(FString(*VarPair.Key), Parsed);
				}
			}
		}
	}

	return CharDef;
}

// --- The Data Asset overlay: the sparse `dataAssets` key (contract §7) ---
//
// NORMATIVE SOURCE: the HTML runtime's runtime-data-assets.js snapshot()/restore(), whose table
// this key is byte-shape-identical to. BARE values, not the typed records VariableToJson writes
// for globals and characters: the seed is schema-authoritative and always ships with the game, so
// a save that pinned types would freeze content the author later edited.

/**
 * One overlay value as a BARE JSON value.
 *
 * A map is told by the variant (only SetMap produces one). Array vs scalar is told by the
 * DECLARATION, which is the only thing that can: FStoryFlowVariant stores the ELEMENT type for
 * arrays and has no "is an array" flag, so an EMPTY array and a scalar are the same variant, and
 * a cleared array would otherwise persist as `""` and reload as a scalar.
 *
 * Declaration may be null — a save written after the variable was deleted from the .sfd. Such an
 * entry is dropped on the way back IN (it can never resolve), so the fallback here only has to
 * be harmless.
 */
TSharedPtr<FJsonValue> BareValueToJson(const FStoryFlowVariant& Value, const FStoryFlowVariable* Declaration)
{
	if (Value.IsMap())
	{
		// Ordered entry list (contract §2.1) — the same writer the typed map path uses, minus
		// the keyType/valueType record around it. Entry ORDER is authored and observable.
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const FStoryFlowMapEntry& Entry : Value.GetMap())
		{
			TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetField(TEXT("key"), VariantToJson(Entry.Key));
			EntryObj->SetField(TEXT("value"), VariantToJson(Entry.Value));
			Entries.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		return MakeShared<FJsonValueArray>(Entries);
	}

	// THE DECLARATION DECIDES, and it can say NO as well as yes. FStoryFlowVariant's scalar
	// setters do not clear ArrayValue (only SetArray / SetMap do), so a variant that once held an
	// array and was re-set as a scalar still carries the old elements — trusting "there are
	// elements" over the declaration would persist that residue as a JSON array under a scalar
	// declaration, and it would reload as a scalar, silently losing the value. No writer produces
	// that state today; the rule costs nothing and does not depend on that staying true.
	//
	// The element count only answers for a value with NO declaration at all (deleted from the
	// .sfd since the write), which is dropped on the way back in anyway.
	const bool bIsArray = Declaration ? Declaration->bIsArray : Value.GetArray().Num() > 0;
	if (bIsArray)
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (const FStoryFlowVariant& Element : Value.GetArray())
		{
			Elements.Add(VariantToJson(Element));
		}
		return MakeShared<FJsonValueArray>(Elements);
	}

	return VariantToJson(Value);
}

/**
 * One saved bare value back into a variant, TYPED FROM THE DECLARATION.
 *
 * The save carries no types, so the declaration is the only authority — the same rule the
 * importer applies to overrides in its second pass. Getting this wrong is invisible to a read
 * (an enum and a string both answer GetString) and visible in the NEXT save, so a save -> load
 * -> save cycle would not be stable.
 */
FStoryFlowVariant BareValueFromJson(const TSharedPtr<FJsonValue>& JsonValue, const FStoryFlowVariable& Declaration)
{
	if (Declaration.Type == EStoryFlowVariableType::Map)
	{
		TArray<FStoryFlowMapEntry> Entries;
		const TArray<TSharedPtr<FJsonValue>>* EntryValues;
		if (JsonValue.IsValid() && JsonValue->TryGetArray(EntryValues))
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *EntryValues)
			{
				const TSharedPtr<FJsonObject>* EntryObj;
				if (!EntryValue->TryGetObject(EntryObj))
				{
					continue;
				}
				// An entry without a key is unaddressable — skip it (matches the importer)
				const TSharedPtr<FJsonValue> KeyField = (*EntryObj)->TryGetField(TEXT("key"));
				if (!KeyField.IsValid())
				{
					continue;
				}
				FStoryFlowMapEntry Entry;
				Entry.Key = VariantFromJson(KeyField, Declaration.KeyType == EStoryFlowVariableType::Enum ? EStoryFlowVariableType::String : Declaration.KeyType);
				Entry.Value = VariantFromJson((*EntryObj)->TryGetField(TEXT("value")), Declaration.ValueType);
				Entries.Add(Entry);
			}
		}
		FStoryFlowVariant Result;
		Result.SetMap(Entries);
		return Result;
	}

	if (Declaration.bIsArray)
	{
		TArray<FStoryFlowVariant> Elements;
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues;
		if (JsonValue.IsValid() && JsonValue->TryGetArray(ArrayValues))
		{
			for (const TSharedPtr<FJsonValue>& Element : *ArrayValues)
			{
				Elements.Add(VariantFromJson(Element, Declaration.Type));
			}
		}
		FStoryFlowVariant Result;
		// The ELEMENT-TYPE overload, always: an emptied array carries nothing to infer from, and
		// a variant that reads back typed or untyped depending on the last writer is a variant
		// whose next save has a different shape.
		Result.SetArray(Elements, Declaration.Type);
		return Result;
	}

	return VariantFromJson(JsonValue, Declaration.Type);
}

/** The sparse overlay table: `{ assetId: { variableId: bare value } }`, `{}` when untouched. */
TSharedPtr<FJsonObject> DataAssetOverlayToJson(
	const StoryFlowDataAssets::FSeed& Seed,
	const StoryFlowDataAssets::FOverlay& Overlay)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	for (const auto& AssetPair : Overlay)
	{
		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		for (const auto& ValuePair : AssetPair.Value)
		{
			const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclaration(Seed, AssetPair.Key, ValuePair.Key);
			AssetObj->SetField(ValuePair.Key, BareValueToJson(ValuePair.Value, Declaration));
		}
		Root->SetObjectField(AssetPair.Key, AssetObj);
	}
	return Root;
}

namespace
{
bool SavedScalarMatches(const TSharedPtr<FJsonValue>& Value, EStoryFlowVariableType Type, const TArray<FString>& EnumValues)
{
	if (!Value.IsValid()) { return false; }
	switch (Type)
	{
	case EStoryFlowVariableType::Boolean:
		return Value->Type == EJson::Boolean;
	case EStoryFlowVariableType::Integer:
	case EStoryFlowVariableType::Float:
	{
		double Number;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number)) { return false; }
		return Type == EStoryFlowVariableType::Integer
			? Number >= MIN_int32 && Number <= MAX_int32 && FMath::FloorToDouble(Number) == Number
			: FMath::IsFinite(static_cast<float>(Number));
	}
	case EStoryFlowVariableType::String:
	case EStoryFlowVariableType::Image:
	case EStoryFlowVariableType::Audio:
	case EStoryFlowVariableType::DataAsset:
	case EStoryFlowVariableType::Character:
	case EStoryFlowVariableType::Enum:
		return Value->Type == EJson::String && (Type != EStoryFlowVariableType::Enum || EnumValues.IsEmpty() || EnumValues.ContainsByPredicate([&Value](const FString& Option) { return Option.Equals(Value->AsString(), ESearchCase::CaseSensitive); }));
	default:
		return false;
	}
}

bool SavedSlotMatches(const TSharedPtr<FJsonValue>& Value, const FStoryFlowVariable& Declaration)
{
	if (Declaration.Type == EStoryFlowVariableType::Map || Declaration.bIsArray)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries;
		if (!Value.IsValid() || !Value->TryGetArray(Entries)) { return false; }
		for (const auto& Entry : *Entries)
		{
			if (Declaration.Type == EStoryFlowVariableType::Map)
			{
				const TSharedPtr<FJsonObject>* Object;
				if (!Entry.IsValid() || !Entry->TryGetObject(Object)
					|| !SavedScalarMatches((*Object)->TryGetField(TEXT("key")), Declaration.KeyType, Declaration.KeyEnumValues)
					|| !SavedScalarMatches((*Object)->TryGetField(TEXT("value")), Declaration.ValueType, Declaration.ValueEnumValues)) { return false; }
			}
			else if (!SavedScalarMatches(Entry, Declaration.Type, Declaration.EnumValues)) { return false; }
		}
		return true;
	}
	return SavedScalarMatches(Value, Declaration.Type, Declaration.EnumValues);
}
}

/**
 * REPLACE the overlay with the saved table (contract §7). Clears FIRST and unconditionally: an
 * absent or malformed key clears, which is seed state, which is exactly the state such a save was
 * made in. Merging instead would let the pre-load session's writes survive into the loaded game.
 *
 * Two kinds of entry are DROPPED rather than restored:
 *  - an asset the current seed does not carry (deleted since the save). Mirrors the reference's
 *    restore(): resolution starts its walk at seed[assetId], so the entry can never be read, and
 *    keeping it would make it ride every subsequent save forever.
 *  - a variable no level of that asset's chain declares any more. The reference keeps such an
 *    entry because JS values need no declaration; a variant does — with no declaration there is
 *    no type to restore it AS, and the same read rule (a value is honored only where the chain
 *    declares the id) already makes it dead data. Dropping it is the typed-language shape of the
 *    same "it can never be read" argument.
 *
 * Validate raw JSON before typed conversion. An incompatible slot is dropped as a whole,
 * revealing its current inherited/default value; compatible sibling slots survive.
 */
void DataAssetOverlayFromJson(
	const TSharedPtr<FJsonObject>& Root,
	const StoryFlowDataAssets::FSeed& Seed,
	StoryFlowDataAssets::FOverlay& OutOverlay)
{
	OutOverlay.Empty();

	const TSharedPtr<FJsonObject>* TableObj = nullptr;
	if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("dataAssets"), TableObj))
	{
		return;
	}

	for (const auto& AssetPair : (*TableObj)->Values)
	{
		const FString AssetId(AssetPair.Key);
		if (!StoryFlowDataAssets::HasAsset(Seed, AssetId))
		{
			// Deliberately not the write path's wording: a load-time drop (the save outlived the
			// asset) and a script write to a dead reference are different problems with
			// different fixes, and they would otherwise read as the same line.
			//
			// WARNING, where the per-variable drop below is Verbose: a whole asset gone means the
			// save outlived the .sfd, which is a project-shape change worth surfacing once, and it
			// can fire at most once per saved asset. The variable-level drop is one line per
			// stale entry and is the expected residue of any variable rename, so it stays quiet.
			UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Save load dropped Data Asset '%s' - no such asset in this project"), *AssetId);
			continue;
		}

		const TSharedPtr<FJsonObject>* AssetObj;
		if (!AssetPair.Value->TryGetObject(AssetObj))
		{
			continue;
		}

		TMap<FString, FStoryFlowVariant> Values;
		for (const auto& ValuePair : (*AssetObj)->Values)
		{
			const FString VariableId(ValuePair.Key);
			const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclaration(Seed, AssetId, VariableId);
			if (!Declaration)
			{
				UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Save load dropped Data Asset value '%s.%s' - the chain no longer declares it"), *AssetId, *VariableId);
				continue;
			}
			if (!SavedSlotMatches(ValuePair.Value, *Declaration))
			{
				UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Save load dropped incompatible Data Asset value '%s.%s'"), *AssetId, *VariableId);
				continue;
			}
			Values.Add(VariableId, BareValueFromJson(ValuePair.Value, *Declaration));
		}

		// An asset whose every entry was dropped leaves NO entry behind — an empty inner table
		// would ride every subsequent save carrying nothing.
		if (Values.Num() > 0)
		{
			OutOverlay.Add(AssetId, MoveTemp(Values));
		}
	}
}

// --- Top-level serialize/deserialize ---

FString SerializeSaveData(
	const TMap<FString, FStoryFlowVariable>& GlobalVariables,
	const TMap<FString, FStoryFlowCharacterDef>& RuntimeCharacters,
	const TSet<FString>& UsedOnceOnlyOptions,
	const StoryFlowDataAssets::FSeed& DataAssetSeed,
	const StoryFlowDataAssets::FOverlay& DataAssetOverlay)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("version"), TEXT("1"));

	// Global variables
	TSharedPtr<FJsonObject> GlobalsObj = MakeShared<FJsonObject>();
	for (const auto& VarPair : GlobalVariables)
	{
		GlobalsObj->SetObjectField(VarPair.Key, VariableToJson(VarPair.Value));
	}
	Root->SetObjectField(TEXT("globalVariables"), GlobalsObj);

	// Runtime characters
	TSharedPtr<FJsonObject> CharsObj = MakeShared<FJsonObject>();
	for (const auto& CharPair : RuntimeCharacters)
	{
		CharsObj->SetObjectField(CharPair.Key, CharacterDefToJson(CharPair.Value));
	}
	Root->SetObjectField(TEXT("characters"), CharsObj);

	// Once-only options
	TArray<TSharedPtr<FJsonValue>> OnceOnlyArray;
	for (const FString& Key : UsedOnceOnlyOptions)
	{
		OnceOnlyArray.Add(MakeShared<FJsonValueString>(Key));
	}
	Root->SetArrayField(TEXT("usedOnceOnlyOptions"), OnceOnlyArray);

	// Data Asset overlay (contract §7). ALWAYS written, `{}` when nothing was written this
	// session — the `characters` convention, and the shape runtime-save.js persists. Additive:
	// SaveVersion stays "1", older plugin builds ignore the key, and this build reads its
	// absence as "clear the overlay".
	Root->SetObjectField(TEXT("dataAssets"), DataAssetOverlayToJson(DataAssetSeed, DataAssetOverlay));

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	return OutputString;
}

bool DeserializeSaveData(
	const FString& JsonString,
	TMap<FString, FStoryFlowVariable>& OutGlobalVariables,
	TMap<FString, FStoryFlowCharacterDef>& OutRuntimeCharacters,
	TSet<FString>& OutUsedOnceOnlyOptions,
	const StoryFlowDataAssets::FSeed& DataAssetSeed,
	StoryFlowDataAssets::FOverlay& OutDataAssetOverlay)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	// Global variables
	OutGlobalVariables.Empty();
	const TSharedPtr<FJsonObject>* GlobalsObj;
	if (Root->TryGetObjectField(TEXT("globalVariables"), GlobalsObj))
	{
		for (const auto& VarPair : (*GlobalsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* VarObj;
			if (VarPair.Value->TryGetObject(VarObj))
			{
				// A record whose type this build cannot name is dropped, not stored untyped
				const FStoryFlowVariable Parsed = VariableFromJson(*VarObj);
				if (Parsed.Type != EStoryFlowVariableType::None)
				{
					OutGlobalVariables.Add(FString(*VarPair.Key), Parsed);
				}
			}
		}
	}

	// Runtime characters
	OutRuntimeCharacters.Empty();
	const TSharedPtr<FJsonObject>* CharsObj;
	if (Root->TryGetObjectField(TEXT("characters"), CharsObj))
	{
		for (const auto& CharPair : (*CharsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* CharObj;
			if (CharPair.Value->TryGetObject(CharObj))
			{
				OutRuntimeCharacters.Add(FString(*CharPair.Key), CharacterDefFromJson(*CharObj));
			}
		}
	}

	// Once-only options
	OutUsedOnceOnlyOptions.Empty();
	const TArray<TSharedPtr<FJsonValue>>* OnceOnlyArray;
	if (Root->TryGetArrayField(TEXT("usedOnceOnlyOptions"), OnceOnlyArray))
	{
		for (const TSharedPtr<FJsonValue>& Val : *OnceOnlyArray)
		{
			OutUsedOnceOnlyOptions.Add(Val->AsString());
		}
	}

	// Data Asset overlay: REPLACE, and an absent key clears (contract §7)
	DataAssetOverlayFromJson(Root, DataAssetSeed, OutDataAssetOverlay);

	return true;
}

} // namespace StoryFlowSaveHelpers
