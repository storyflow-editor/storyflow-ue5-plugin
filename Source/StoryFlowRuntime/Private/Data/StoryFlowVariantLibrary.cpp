// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowVariantLibrary.h"

bool UStoryFlowVariantLibrary::IsVariantValid(const FStoryFlowVariant& Variant)
{
	return Variant.IsValid();
}

EStoryFlowVariableType UStoryFlowVariantLibrary::GetVariantType(const FStoryFlowVariant& Variant)
{
	return Variant.GetType();
}

bool UStoryFlowVariantLibrary::GetVariantAsBool(const FStoryFlowVariant& Variant, bool bDefault)
{
	return Variant.GetBool(bDefault);
}

int32 UStoryFlowVariantLibrary::GetVariantAsInt(const FStoryFlowVariant& Variant, int32 Default)
{
	return Variant.GetInt(Default);
}

float UStoryFlowVariantLibrary::GetVariantAsFloat(const FStoryFlowVariant& Variant, float Default)
{
	return Variant.GetFloat(Default);
}

FString UStoryFlowVariantLibrary::GetVariantAsString(const FStoryFlowVariant& Variant, const FString& Default)
{
	return Variant.GetString(Default);
}

FString UStoryFlowVariantLibrary::GetVariantDisplayString(const FStoryFlowVariant& Variant)
{
	return Variant.ToString();
}

TArray<FStoryFlowVariant> UStoryFlowVariantLibrary::GetVariantArray(const FStoryFlowVariant& Variant)
{
	return Variant.GetArray();
}

bool UStoryFlowVariantLibrary::IsVariantMap(const FStoryFlowVariant& Variant)
{
	return Variant.IsMap();
}

void UStoryFlowVariantLibrary::GetVariantMap(const FStoryFlowVariant& Variant, TArray<FStoryFlowVariant>& Keys, TArray<FStoryFlowVariant>& Values)
{
	Keys.Reset();
	Values.Reset();
	if (!Variant.IsMap())
	{
		return;
	}
	const TArray<FStoryFlowMapEntry>& Entries = Variant.GetMap();
	Keys.Reserve(Entries.Num());
	Values.Reserve(Entries.Num());
	for (const FStoryFlowMapEntry& Entry : Entries)
	{
		Keys.Add(Entry.Key);
		Values.Add(Entry.Value);
	}
}

FStoryFlowVariant UStoryFlowVariantLibrary::MakeVariantFromBool(bool bValue)
{
	return FStoryFlowVariant::FromBool(bValue);
}

FStoryFlowVariant UStoryFlowVariantLibrary::MakeVariantFromInt(int32 Value)
{
	return FStoryFlowVariant::FromInt(Value);
}

FStoryFlowVariant UStoryFlowVariantLibrary::MakeVariantFromFloat(float Value)
{
	return FStoryFlowVariant::FromFloat(Value);
}

FStoryFlowVariant UStoryFlowVariantLibrary::MakeVariantFromString(const FString& Value)
{
	return FStoryFlowVariant::FromString(Value);
}

FStoryFlowVariant UStoryFlowVariantLibrary::MakeVariantFromEnum(const FString& Value)
{
	FStoryFlowVariant Variant;
	Variant.SetEnum(Value);
	return Variant;
}
