// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowSaveGame.h"
#include "Data/StoryFlowTypes.h"
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
	case EStoryFlowVariableType::Character: return TEXT("Character");
	case EStoryFlowVariableType::Map:       return TEXT("Map");
	default:                                return TEXT("None");
	}
}

EStoryFlowVariableType StringToVariableType(const FString& Str)
{
	if (Str == TEXT("Boolean"))   return EStoryFlowVariableType::Boolean;
	if (Str == TEXT("Integer"))   return EStoryFlowVariableType::Integer;
	if (Str == TEXT("Float"))     return EStoryFlowVariableType::Float;
	if (Str == TEXT("String"))    return EStoryFlowVariableType::String;
	if (Str == TEXT("Enum"))      return EStoryFlowVariableType::Enum;
	if (Str == TEXT("Image"))     return EStoryFlowVariableType::Image;
	if (Str == TEXT("Audio"))     return EStoryFlowVariableType::Audio;
	if (Str == TEXT("Character")) return EStoryFlowVariableType::Character;
	if (Str == TEXT("Map"))       return EStoryFlowVariableType::Map;
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
	case EStoryFlowVariableType::Character:
		Result.SetString(JsonValue->AsString());
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
	CharDef.Image = Obj->GetStringField(TEXT("image"));

	const TSharedPtr<FJsonObject>* VarsObj;
	if (Obj->TryGetObjectField(TEXT("variables"), VarsObj))
	{
		for (const auto& VarPair : (*VarsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* VarObj;
			if (VarPair.Value->TryGetObject(VarObj))
			{
				CharDef.Variables.Add(VarPair.Key, VariableFromJson(*VarObj));
			}
		}
	}

	return CharDef;
}

// --- Top-level serialize/deserialize ---

FString SerializeSaveData(
	const TMap<FString, FStoryFlowVariable>& GlobalVariables,
	const TMap<FString, FStoryFlowCharacterDef>& RuntimeCharacters,
	const TSet<FString>& UsedOnceOnlyOptions)
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

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	return OutputString;
}

bool DeserializeSaveData(
	const FString& JsonString,
	TMap<FString, FStoryFlowVariable>& OutGlobalVariables,
	TMap<FString, FStoryFlowCharacterDef>& OutRuntimeCharacters,
	TSet<FString>& OutUsedOnceOnlyOptions)
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
				OutGlobalVariables.Add(VarPair.Key, VariableFromJson(*VarObj));
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
				OutRuntimeCharacters.Add(CharPair.Key, CharacterDefFromJson(*CharObj));
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

	return true;
}

} // namespace StoryFlowSaveHelpers
