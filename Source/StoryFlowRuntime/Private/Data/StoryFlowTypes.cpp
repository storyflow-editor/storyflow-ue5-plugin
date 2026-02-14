// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowTypes.h"

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
