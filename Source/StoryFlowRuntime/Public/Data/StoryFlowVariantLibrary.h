// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Data/StoryFlowTypes.h"
#include "StoryFlowVariantLibrary.generated.h"

/**
 * Blueprint accessors for FStoryFlowVariant.
 *
 * FStoryFlowVariant keeps its fields private so the type tag can't be corrupted,
 * which also means Blueprint gets no Break node for it. Values that surface as
 * variants (the dialogue state's Character Variables map, GetArrayVariable /
 * GetMapVariable elements, the legacy GetCharacterVariable) are unwrapped
 * through these pure functions instead.
 */
UCLASS()
class STORYFLOWRUNTIME_API UStoryFlowVariantLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** True when the variant holds a value (its type is not None). */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static bool IsVariantValid(const FStoryFlowVariant& Variant);

	/** The variant's value type. None for an unset variant (e.g. a failed map Find). */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static EStoryFlowVariableType GetVariantType(const FStoryFlowVariant& Variant);

	/** Read a boolean variant. Returns Default when the variant is not a boolean. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static bool GetVariantAsBool(const FStoryFlowVariant& Variant, bool bDefault = false);

	/** Read an integer variant. Returns Default when the variant is not an integer. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static int32 GetVariantAsInt(const FStoryFlowVariant& Variant, int32 Default = 0);

	/**
	 * Read a float variant. Integer variants are coerced (whole-number JSON values
	 * import as integers). Returns Default for any other type.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static float GetVariantAsFloat(const FStoryFlowVariant& Variant, float Default = 0.0f);

	/**
	 * Read a string-family variant. Covers string, enum, image, audio and character
	 * variants (the latter three store asset keys / paths). Returns Default for any
	 * other type.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FString GetVariantAsString(const FStoryFlowVariant& Variant, const FString& Default = TEXT(""));

	/**
	 * Format any variant for display: booleans as "true"/"false", numbers as text,
	 * string-family values verbatim. Maps and unset variants format as "".
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FString GetVariantDisplayString(const FStoryFlowVariant& Variant);

	/**
	 * Read an array variant's elements. Empty for scalar variants. Unwrap each
	 * element with the scalar getters above.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static TArray<FStoryFlowVariant> GetVariantArray(const FStoryFlowVariant& Variant);

	/** True when the variant holds a map. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static bool IsVariantMap(const FStoryFlowVariant& Variant);

	/**
	 * Read a map variant as parallel key/value arrays in entry order (Keys[i] pairs
	 * with Values[i], matching GetMapVariable on the StoryFlow component). Both
	 * arrays are empty when the variant is not a map.
	 */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static void GetVariantMap(const FStoryFlowVariant& Variant, TArray<FStoryFlowVariant>& Keys, TArray<FStoryFlowVariant>& Values);

	/** Build a boolean variant (for APIs taking a raw variant, e.g. the legacy SetCharacterVariable). */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FStoryFlowVariant MakeVariantFromBool(bool bValue);

	/** Build an integer variant. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FStoryFlowVariant MakeVariantFromInt(int32 Value);

	/** Build a float variant. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FStoryFlowVariant MakeVariantFromFloat(float Value);

	/** Build a string variant. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FStoryFlowVariant MakeVariantFromString(const FString& Value);

	/** Build an enum variant from an enum option string. */
	UFUNCTION(BlueprintPure, Category = "StoryFlow|Variant")
	static FStoryFlowVariant MakeVariantFromEnum(const FString& Value);
};
