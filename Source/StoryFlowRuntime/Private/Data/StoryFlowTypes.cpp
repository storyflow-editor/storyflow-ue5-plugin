// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowTypes.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"

// ============================================================================
// FStoryFlowVariant Serialization
// ============================================================================

namespace
{
	/** Recursively write a single FStoryFlowVariant element into a memory writer. */
	void SerializeVariantElement(FMemoryWriter& Writer, const FStoryFlowVariant& Variant)
	{
		int32 TypeInt = static_cast<int32>(Variant.GetType());
		Writer << TypeInt;

		// Write the scalar value according to type
		switch (Variant.GetType())
		{
		case EStoryFlowVariableType::Boolean:
		{
			bool b = Variant.GetBool();
			Writer << b;
			break;
		}
		case EStoryFlowVariableType::Integer:
		{
			int32 i = Variant.GetInt();
			Writer << i;
			break;
		}
		case EStoryFlowVariableType::Float:
		{
			float f = Variant.GetFloat();
			Writer << f;
			break;
		}
		case EStoryFlowVariableType::String:
		case EStoryFlowVariableType::Enum:
		case EStoryFlowVariableType::Image:
		case EStoryFlowVariableType::Audio:
		case EStoryFlowVariableType::Character:
		{
			FString s = Variant.GetString();
			Writer << s;
			break;
		}
		default:
			break;
		}

		// Recursively write nested array
		const TArray<FStoryFlowVariant>& Arr = Variant.GetArray();
		int32 ArrNum = Arr.Num();
		Writer << ArrNum;
		for (int32 idx = 0; idx < ArrNum; ++idx)
		{
			SerializeVariantElement(Writer, Arr[idx]);
		}
	}

	/** Recursively read a single FStoryFlowVariant element from a memory reader. */
	void DeserializeVariantElement(FMemoryReader& Reader, FStoryFlowVariant& OutVariant)
	{
		OutVariant.Reset();

		int32 TypeInt = 0;
		Reader << TypeInt;
		EStoryFlowVariableType VarType = static_cast<EStoryFlowVariableType>(TypeInt);

		switch (VarType)
		{
		case EStoryFlowVariableType::Boolean:
		{
			bool b = false;
			Reader << b;
			OutVariant.SetBool(b);
			break;
		}
		case EStoryFlowVariableType::Integer:
		{
			int32 i = 0;
			Reader << i;
			OutVariant.SetInt(i);
			break;
		}
		case EStoryFlowVariableType::Float:
		{
			float f = 0.0f;
			Reader << f;
			OutVariant.SetFloat(f);
			break;
		}
		case EStoryFlowVariableType::String:
		{
			FString s;
			Reader << s;
			OutVariant.SetString(s);
			break;
		}
		case EStoryFlowVariableType::Enum:
		{
			FString s;
			Reader << s;
			OutVariant.SetEnum(s);
			break;
		}
		case EStoryFlowVariableType::Image:
		case EStoryFlowVariableType::Audio:
		case EStoryFlowVariableType::Character:
		{
			FString s;
			Reader << s;
			OutVariant.SetString(s);
			break;
		}
		default:
			break;
		}

		// Recursively read nested array
		int32 ArrNum = 0;
		Reader << ArrNum;
		if (ArrNum > 0)
		{
			TArray<FStoryFlowVariant> Arr;
			Arr.SetNum(ArrNum);
			for (int32 idx = 0; idx < ArrNum; ++idx)
			{
				DeserializeVariantElement(Reader, Arr[idx]);
			}
			OutVariant.SetArray(Arr);
		}
	}
}

void FStoryFlowVariant::PackArrayForSerialization()
{
	SerializedArrayData.Empty();
	if (ArrayValue.Num() == 0)
	{
		return;
	}

	FMemoryWriter Writer(SerializedArrayData);
	int32 Num = ArrayValue.Num();
	Writer << Num;
	for (int32 i = 0; i < Num; ++i)
	{
		SerializeVariantElement(Writer, ArrayValue[i]);
	}
}

void FStoryFlowVariant::UnpackArrayFromSerialization()
{
	ArrayValue.Empty();
	if (SerializedArrayData.Num() == 0)
	{
		return;
	}

	FMemoryReader Reader(SerializedArrayData);
	int32 Num = 0;
	Reader << Num;
	ArrayValue.SetNum(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		DeserializeVariantElement(Reader, ArrayValue[i]);
	}
}

void PackVariablesForSerialization(TMap<FString, FStoryFlowVariable>& Variables)
{
	for (auto& Pair : Variables)
	{
		Pair.Value.Value.PackArrayForSerialization();
	}
}

void UnpackVariablesFromSerialization(TMap<FString, FStoryFlowVariable>& Variables)
{
	for (auto& Pair : Variables)
	{
		Pair.Value.Value.UnpackArrayFromSerialization();
	}
}

EStoryFlowNodeType ParseNodeType(const FString& TypeString)
{
	static TMap<FString, EStoryFlowNodeType> TypeMap = {
		// Control Flow
		{ TEXT("start"), EStoryFlowNodeType::Start },
		{ TEXT("end"), EStoryFlowNodeType::End },
		{ TEXT("branch"), EStoryFlowNodeType::Branch },
		{ TEXT("runScript"), EStoryFlowNodeType::RunScript },
		{ TEXT("runFlow"), EStoryFlowNodeType::RunFlow },
		{ TEXT("entryFlow"), EStoryFlowNodeType::EntryFlow },

		// Dialogue
		{ TEXT("dialogue"), EStoryFlowNodeType::Dialogue },

		// Boolean
		{ TEXT("getBool"), EStoryFlowNodeType::GetBool },
		{ TEXT("setBool"), EStoryFlowNodeType::SetBool },
		{ TEXT("andBool"), EStoryFlowNodeType::AndBool },
		{ TEXT("orBool"), EStoryFlowNodeType::OrBool },
		{ TEXT("notBool"), EStoryFlowNodeType::NotBool },
		{ TEXT("equalBool"), EStoryFlowNodeType::EqualBool },

		// Integer
		{ TEXT("getInt"), EStoryFlowNodeType::GetInt },
		{ TEXT("setInt"), EStoryFlowNodeType::SetInt },
		{ TEXT("plus"), EStoryFlowNodeType::Plus },
		{ TEXT("minus"), EStoryFlowNodeType::Minus },
		{ TEXT("multiply"), EStoryFlowNodeType::Multiply },
		{ TEXT("divide"), EStoryFlowNodeType::Divide },
		{ TEXT("random"), EStoryFlowNodeType::Random },

		// Integer Comparison
		{ TEXT("greaterThan"), EStoryFlowNodeType::GreaterThan },
		{ TEXT("greaterThanOrEqual"), EStoryFlowNodeType::GreaterThanOrEqual },
		{ TEXT("lessThan"), EStoryFlowNodeType::LessThan },
		{ TEXT("lessThanOrEqual"), EStoryFlowNodeType::LessThanOrEqual },
		{ TEXT("equalInt"), EStoryFlowNodeType::EqualInt },

		// Float
		{ TEXT("getFloat"), EStoryFlowNodeType::GetFloat },
		{ TEXT("setFloat"), EStoryFlowNodeType::SetFloat },
		{ TEXT("plusFloat"), EStoryFlowNodeType::PlusFloat },
		{ TEXT("minusFloat"), EStoryFlowNodeType::MinusFloat },
		{ TEXT("multiplyFloat"), EStoryFlowNodeType::MultiplyFloat },
		{ TEXT("divideFloat"), EStoryFlowNodeType::DivideFloat },
		{ TEXT("randomFloat"), EStoryFlowNodeType::RandomFloat },

		// Float Comparison
		{ TEXT("greaterThanFloat"), EStoryFlowNodeType::GreaterThanFloat },
		{ TEXT("greaterThanOrEqualFloat"), EStoryFlowNodeType::GreaterThanOrEqualFloat },
		{ TEXT("lessThanFloat"), EStoryFlowNodeType::LessThanFloat },
		{ TEXT("lessThanOrEqualFloat"), EStoryFlowNodeType::LessThanOrEqualFloat },
		{ TEXT("equalFloat"), EStoryFlowNodeType::EqualFloat },

		// String
		{ TEXT("getString"), EStoryFlowNodeType::GetString },
		{ TEXT("setString"), EStoryFlowNodeType::SetString },
		{ TEXT("concatenateString"), EStoryFlowNodeType::ConcatenateString },
		{ TEXT("equalString"), EStoryFlowNodeType::EqualString },
		{ TEXT("containsString"), EStoryFlowNodeType::ContainsString },
		{ TEXT("toUpperCase"), EStoryFlowNodeType::ToUpperCase },
		{ TEXT("toLowerCase"), EStoryFlowNodeType::ToLowerCase },

		// Enum
		{ TEXT("getEnum"), EStoryFlowNodeType::GetEnum },
		{ TEXT("setEnum"), EStoryFlowNodeType::SetEnum },
		{ TEXT("equalEnum"), EStoryFlowNodeType::EqualEnum },
		{ TEXT("switchOnEnum"), EStoryFlowNodeType::SwitchOnEnum },
		{ TEXT("randomBranch"), EStoryFlowNodeType::RandomBranch },

		// Type Conversion
		{ TEXT("intToBoolean"), EStoryFlowNodeType::IntToBoolean },
		{ TEXT("floatToBoolean"), EStoryFlowNodeType::FloatToBoolean },
		{ TEXT("booleanToInt"), EStoryFlowNodeType::BooleanToInt },
		{ TEXT("booleanToFloat"), EStoryFlowNodeType::BooleanToFloat },
		{ TEXT("intToString"), EStoryFlowNodeType::IntToString },
		{ TEXT("floatToString"), EStoryFlowNodeType::FloatToString },
		{ TEXT("stringToInt"), EStoryFlowNodeType::StringToInt },
		{ TEXT("stringToFloat"), EStoryFlowNodeType::StringToFloat },
		{ TEXT("intToEnum"), EStoryFlowNodeType::IntToEnum },
		{ TEXT("stringToEnum"), EStoryFlowNodeType::StringToEnum },
		{ TEXT("intToFloat"), EStoryFlowNodeType::IntToFloat },
		{ TEXT("floatToInt"), EStoryFlowNodeType::FloatToInt },
		{ TEXT("enumToString"), EStoryFlowNodeType::EnumToString },
		{ TEXT("lengthString"), EStoryFlowNodeType::LengthString },

		// Boolean Arrays
		{ TEXT("getBoolArray"), EStoryFlowNodeType::GetBoolArray },
		{ TEXT("setBoolArray"), EStoryFlowNodeType::SetBoolArray },
		{ TEXT("getBoolArrayElement"), EStoryFlowNodeType::GetBoolArrayElement },
		{ TEXT("setBoolArrayElement"), EStoryFlowNodeType::SetBoolArrayElement },
		{ TEXT("getRandomBoolArrayElement"), EStoryFlowNodeType::GetRandomBoolArrayElement },
		{ TEXT("addToBoolArray"), EStoryFlowNodeType::AddToBoolArray },
		{ TEXT("removeFromBoolArray"), EStoryFlowNodeType::RemoveFromBoolArray },
		{ TEXT("clearBoolArray"), EStoryFlowNodeType::ClearBoolArray },
		{ TEXT("arrayLengthBool"), EStoryFlowNodeType::ArrayLengthBool },
		{ TEXT("arrayContainsBool"), EStoryFlowNodeType::ArrayContainsBool },
		{ TEXT("findInBoolArray"), EStoryFlowNodeType::FindInBoolArray },

		// Integer Arrays
		{ TEXT("getIntArray"), EStoryFlowNodeType::GetIntArray },
		{ TEXT("setIntArray"), EStoryFlowNodeType::SetIntArray },
		{ TEXT("getIntArrayElement"), EStoryFlowNodeType::GetIntArrayElement },
		{ TEXT("setIntArrayElement"), EStoryFlowNodeType::SetIntArrayElement },
		{ TEXT("getRandomIntArrayElement"), EStoryFlowNodeType::GetRandomIntArrayElement },
		{ TEXT("addToIntArray"), EStoryFlowNodeType::AddToIntArray },
		{ TEXT("removeFromIntArray"), EStoryFlowNodeType::RemoveFromIntArray },
		{ TEXT("clearIntArray"), EStoryFlowNodeType::ClearIntArray },
		{ TEXT("arrayLengthInt"), EStoryFlowNodeType::ArrayLengthInt },
		{ TEXT("arrayContainsInt"), EStoryFlowNodeType::ArrayContainsInt },
		{ TEXT("findInIntArray"), EStoryFlowNodeType::FindInIntArray },

		// Float Arrays
		{ TEXT("getFloatArray"), EStoryFlowNodeType::GetFloatArray },
		{ TEXT("setFloatArray"), EStoryFlowNodeType::SetFloatArray },
		{ TEXT("getFloatArrayElement"), EStoryFlowNodeType::GetFloatArrayElement },
		{ TEXT("setFloatArrayElement"), EStoryFlowNodeType::SetFloatArrayElement },
		{ TEXT("getRandomFloatArrayElement"), EStoryFlowNodeType::GetRandomFloatArrayElement },
		{ TEXT("addToFloatArray"), EStoryFlowNodeType::AddToFloatArray },
		{ TEXT("removeFromFloatArray"), EStoryFlowNodeType::RemoveFromFloatArray },
		{ TEXT("clearFloatArray"), EStoryFlowNodeType::ClearFloatArray },
		{ TEXT("arrayLengthFloat"), EStoryFlowNodeType::ArrayLengthFloat },
		{ TEXT("arrayContainsFloat"), EStoryFlowNodeType::ArrayContainsFloat },
		{ TEXT("findInFloatArray"), EStoryFlowNodeType::FindInFloatArray },

		// String Arrays
		{ TEXT("getStringArray"), EStoryFlowNodeType::GetStringArray },
		{ TEXT("setStringArray"), EStoryFlowNodeType::SetStringArray },
		{ TEXT("getStringArrayElement"), EStoryFlowNodeType::GetStringArrayElement },
		{ TEXT("setStringArrayElement"), EStoryFlowNodeType::SetStringArrayElement },
		{ TEXT("getRandomStringArrayElement"), EStoryFlowNodeType::GetRandomStringArrayElement },
		{ TEXT("addToStringArray"), EStoryFlowNodeType::AddToStringArray },
		{ TEXT("removeFromStringArray"), EStoryFlowNodeType::RemoveFromStringArray },
		{ TEXT("clearStringArray"), EStoryFlowNodeType::ClearStringArray },
		{ TEXT("arrayLengthString"), EStoryFlowNodeType::ArrayLengthString },
		{ TEXT("arrayContainsString"), EStoryFlowNodeType::ArrayContainsString },
		{ TEXT("findInStringArray"), EStoryFlowNodeType::FindInStringArray },

		// Image Arrays
		{ TEXT("getImageArray"), EStoryFlowNodeType::GetImageArray },
		{ TEXT("setImageArray"), EStoryFlowNodeType::SetImageArray },
		{ TEXT("getImageArrayElement"), EStoryFlowNodeType::GetImageArrayElement },
		{ TEXT("setImageArrayElement"), EStoryFlowNodeType::SetImageArrayElement },
		{ TEXT("getRandomImageArrayElement"), EStoryFlowNodeType::GetRandomImageArrayElement },
		{ TEXT("addToImageArray"), EStoryFlowNodeType::AddToImageArray },
		{ TEXT("removeFromImageArray"), EStoryFlowNodeType::RemoveFromImageArray },
		{ TEXT("clearImageArray"), EStoryFlowNodeType::ClearImageArray },
		{ TEXT("arrayLengthImage"), EStoryFlowNodeType::ArrayLengthImage },
		{ TEXT("arrayContainsImage"), EStoryFlowNodeType::ArrayContainsImage },
		{ TEXT("findInImageArray"), EStoryFlowNodeType::FindInImageArray },

		// Character Arrays
		{ TEXT("getCharacterArray"), EStoryFlowNodeType::GetCharacterArray },
		{ TEXT("setCharacterArray"), EStoryFlowNodeType::SetCharacterArray },
		{ TEXT("getCharacterArrayElement"), EStoryFlowNodeType::GetCharacterArrayElement },
		{ TEXT("setCharacterArrayElement"), EStoryFlowNodeType::SetCharacterArrayElement },
		{ TEXT("getRandomCharacterArrayElement"), EStoryFlowNodeType::GetRandomCharacterArrayElement },
		{ TEXT("addToCharacterArray"), EStoryFlowNodeType::AddToCharacterArray },
		{ TEXT("removeFromCharacterArray"), EStoryFlowNodeType::RemoveFromCharacterArray },
		{ TEXT("clearCharacterArray"), EStoryFlowNodeType::ClearCharacterArray },
		{ TEXT("arrayLengthCharacter"), EStoryFlowNodeType::ArrayLengthCharacter },
		{ TEXT("arrayContainsCharacter"), EStoryFlowNodeType::ArrayContainsCharacter },
		{ TEXT("findInCharacterArray"), EStoryFlowNodeType::FindInCharacterArray },

		// Audio Arrays
		{ TEXT("getAudioArray"), EStoryFlowNodeType::GetAudioArray },
		{ TEXT("setAudioArray"), EStoryFlowNodeType::SetAudioArray },
		{ TEXT("getAudioArrayElement"), EStoryFlowNodeType::GetAudioArrayElement },
		{ TEXT("setAudioArrayElement"), EStoryFlowNodeType::SetAudioArrayElement },
		{ TEXT("getRandomAudioArrayElement"), EStoryFlowNodeType::GetRandomAudioArrayElement },
		{ TEXT("addToAudioArray"), EStoryFlowNodeType::AddToAudioArray },
		{ TEXT("removeFromAudioArray"), EStoryFlowNodeType::RemoveFromAudioArray },
		{ TEXT("clearAudioArray"), EStoryFlowNodeType::ClearAudioArray },
		{ TEXT("arrayLengthAudio"), EStoryFlowNodeType::ArrayLengthAudio },
		{ TEXT("arrayContainsAudio"), EStoryFlowNodeType::ArrayContainsAudio },
		{ TEXT("findInAudioArray"), EStoryFlowNodeType::FindInAudioArray },

		// Loops
		{ TEXT("forEachBoolLoop"), EStoryFlowNodeType::ForEachBoolLoop },
		{ TEXT("forEachIntLoop"), EStoryFlowNodeType::ForEachIntLoop },
		{ TEXT("forEachFloatLoop"), EStoryFlowNodeType::ForEachFloatLoop },
		{ TEXT("forEachStringLoop"), EStoryFlowNodeType::ForEachStringLoop },
		{ TEXT("forEachImageLoop"), EStoryFlowNodeType::ForEachImageLoop },
		{ TEXT("forEachCharacterLoop"), EStoryFlowNodeType::ForEachCharacterLoop },
		{ TEXT("forEachAudioLoop"), EStoryFlowNodeType::ForEachAudioLoop },

		// Media
		{ TEXT("getImage"), EStoryFlowNodeType::GetImage },
		{ TEXT("setImage"), EStoryFlowNodeType::SetImage },
		{ TEXT("setBackgroundImage"), EStoryFlowNodeType::SetBackgroundImage },
		{ TEXT("getAudio"), EStoryFlowNodeType::GetAudio },
		{ TEXT("setAudio"), EStoryFlowNodeType::SetAudio },
		{ TEXT("playAudio"), EStoryFlowNodeType::PlayAudio },
		{ TEXT("getCharacter"), EStoryFlowNodeType::GetCharacter },
		{ TEXT("setCharacter"), EStoryFlowNodeType::SetCharacter },

		// Character Variables
		{ TEXT("getCharacterVar"), EStoryFlowNodeType::GetCharacterVar },
		{ TEXT("setCharacterVar"), EStoryFlowNodeType::SetCharacterVar },
	};

	if (const EStoryFlowNodeType* Found = TypeMap.Find(TypeString))
	{
		return *Found;
	}
	return EStoryFlowNodeType::Unknown;
}

bool FStoryFlowHandle::Parse(const FString& HandleString, FStoryFlowHandle& OutHandle)
{
	// Format: "source-{nodeId}-{type}[-{suffix}]"
	// or:     "target-{nodeId}-{type}[-{suffix}]"

	TArray<FString> Parts;
	HandleString.ParseIntoArray(Parts, TEXT("-"), true);

	if (Parts.Num() < 2)
	{
		return false;
	}

	OutHandle.bIsSource = Parts[0] == TEXT("source");
	OutHandle.NodeId = Parts[1];

	if (Parts.Num() >= 3)
	{
		OutHandle.Type = Parts[2];
	}

	if (Parts.Num() >= 4)
	{
		// Join remaining parts as suffix
		TArray<FString> SuffixParts;
		for (int32 i = 3; i < Parts.Num(); ++i)
		{
			SuffixParts.Add(Parts[i]);
		}
		OutHandle.Suffix = FString::Join(SuffixParts, TEXT("-"));
	}

	return true;
}
