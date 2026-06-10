// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Sound/SoundBase.h"
#include "StoryFlowTypes.generated.h"

// Forward declarations
class UStoryFlowScriptAsset;

// ============================================================================
// Enums
// ============================================================================

/**
 * Node type enumeration matching StoryFlow Editor node types
 */
UENUM(BlueprintType)
enum class EStoryFlowNodeType : uint8
{
	// Control Flow
	Start,
	End,
	Branch,
	RunScript,
	RunFlow,
	EntryFlow,

	// Dialogue
	Dialogue,

	// Boolean
	GetBool,
	SetBool,
	AndBool,
	OrBool,
	NotBool,
	EqualBool,

	// Integer
	GetInt,
	SetInt,
	Plus,
	Minus,
	Multiply,
	Divide,
	Random,

	// Integer Comparison
	GreaterThan,
	GreaterThanOrEqual,
	LessThan,
	LessThanOrEqual,
	EqualInt,

	// Float
	GetFloat,
	SetFloat,
	PlusFloat,
	MinusFloat,
	MultiplyFloat,
	DivideFloat,
	RandomFloat,

	// Float Comparison
	GreaterThanFloat,
	GreaterThanOrEqualFloat,
	LessThanFloat,
	LessThanOrEqualFloat,
	EqualFloat,

	// String
	GetString,
	SetString,
	ConcatenateString,
	EqualString,
	ContainsString,
	ToUpperCase,
	ToLowerCase,

	// Enum
	GetEnum,
	SetEnum,
	EqualEnum,
	SwitchOnEnum,
	RandomBranch,

	// Type Conversion
	IntToBoolean,
	FloatToBoolean,
	BooleanToInt,
	BooleanToFloat,
	IntToString,
	FloatToString,
	StringToInt,
	StringToFloat,
	IntToEnum,
	StringToEnum,
	IntToFloat,
	FloatToInt,
	EnumToString,
	LengthString,

	// Boolean Arrays
	GetBoolArray,
	SetBoolArray,
	GetBoolArrayElement,
	SetBoolArrayElement,
	GetRandomBoolArrayElement,
	AddToBoolArray,
	RemoveFromBoolArray,
	ClearBoolArray,
	ArrayLengthBool,
	ArrayContainsBool,
	FindInBoolArray,

	// Integer Arrays
	GetIntArray,
	SetIntArray,
	GetIntArrayElement,
	SetIntArrayElement,
	GetRandomIntArrayElement,
	AddToIntArray,
	RemoveFromIntArray,
	ClearIntArray,
	ArrayLengthInt,
	ArrayContainsInt,
	FindInIntArray,

	// Float Arrays
	GetFloatArray,
	SetFloatArray,
	GetFloatArrayElement,
	SetFloatArrayElement,
	GetRandomFloatArrayElement,
	AddToFloatArray,
	RemoveFromFloatArray,
	ClearFloatArray,
	ArrayLengthFloat,
	ArrayContainsFloat,
	FindInFloatArray,

	// String Arrays
	GetStringArray,
	SetStringArray,
	GetStringArrayElement,
	SetStringArrayElement,
	GetRandomStringArrayElement,
	AddToStringArray,
	RemoveFromStringArray,
	ClearStringArray,
	ArrayLengthString,
	ArrayContainsString,
	FindInStringArray,

	// Image Arrays
	GetImageArray,
	SetImageArray,
	GetImageArrayElement,
	SetImageArrayElement,
	GetRandomImageArrayElement,
	AddToImageArray,
	RemoveFromImageArray,
	ClearImageArray,
	ArrayLengthImage,
	ArrayContainsImage,
	FindInImageArray,

	// Character Arrays
	GetCharacterArray,
	SetCharacterArray,
	GetCharacterArrayElement,
	SetCharacterArrayElement,
	GetRandomCharacterArrayElement,
	AddToCharacterArray,
	RemoveFromCharacterArray,
	ClearCharacterArray,
	ArrayLengthCharacter,
	ArrayContainsCharacter,
	FindInCharacterArray,

	// Audio Arrays
	GetAudioArray,
	SetAudioArray,
	GetAudioArrayElement,
	SetAudioArrayElement,
	GetRandomAudioArrayElement,
	AddToAudioArray,
	RemoveFromAudioArray,
	ClearAudioArray,
	ArrayLengthAudio,
	ArrayContainsAudio,
	FindInAudioArray,

	// Loops
	ForEachBoolLoop,
	ForEachIntLoop,
	ForEachFloatLoop,
	ForEachStringLoop,
	ForEachImageLoop,
	ForEachCharacterLoop,
	ForEachAudioLoop,

	// Media
	GetImage,
	SetImage,
	SetBackgroundImage,
	GetAudio,
	SetAudio,
	PlayAudio,
	GetCharacter,
	SetCharacter,

	// Character Variables (get/set properties on a character)
	GetCharacterVar,
	SetCharacterVar,

	// Map Variables
	GetMap,
	SetMap,
	GetMapValue,
	SetMapValue,
	HasMapKey,
	MapSize,
	MapKeys,
	MapValues,
	RemoveMapKey,
	ClearMap,
	ForEachMap,

	// Unknown/Custom
	Unknown UMETA(Hidden)
};

/**
 * Variable type enumeration
 */
UENUM(BlueprintType)
enum class EStoryFlowVariableType : uint8
{
	None,
	Boolean,
	Integer,
	Float,
	String,
	Enum,
	Image,
	Audio,
	Character,
	Map
};

/**
 * Asset type enumeration
 */
UENUM(BlueprintType)
enum class EStoryFlowAssetType : uint8
{
	Image,
	Audio,
	Video
};

/**
 * Loop type enumeration
 */
UENUM(BlueprintType)
enum class EStoryFlowLoopType : uint8
{
	ForEach
};

// ============================================================================
// Variant Type (Type-safe value container)
// ============================================================================

// Forward declaration — defined below FStoryFlowVariant (an entry holds variants by value,
// so the entry struct needs the complete variant type)
struct FStoryFlowMapEntry;

/**
 * Type-safe value container for StoryFlow variables
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowVariant
{
	GENERATED_BODY()

private:
	UPROPERTY()
	EStoryFlowVariableType Type = EStoryFlowVariableType::None;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	float FloatValue = 0.0f;

	UPROPERTY()
	FString StringValue;

	// Note: Not a UPROPERTY because UHT doesn't support recursive struct arrays
	// Still usable in C++ code, just not reflected to Blueprint
	TArray<FStoryFlowVariant> ArrayValue;

	// Note: Not a UPROPERTY for the same reason as ArrayValue (entries hold variants
	// recursively). Ordered entry array, NOT a TMap — entry order is observable through
	// mapKeys/mapValues/forEachMap and must match the editor's serialized order.
	// Persisted through SerializedArrayData alongside ArrayValue.
	TArray<FStoryFlowMapEntry> MapValue;

public:
	// Type checking
	bool IsValid() const { return Type != EStoryFlowVariableType::None; }
	// Note: In Blueprint, use FStoryFlowVariable.Type instead (BlueprintReadOnly).
	// GetType() is C++ only — not a UFUNCTION, and FStoryFlowVariant's members are private.
	EStoryFlowVariableType GetType() const { return Type; }

	// Setters
	void SetBool(bool Value)
	{
		Type = EStoryFlowVariableType::Boolean;
		bBoolValue = Value;
	}

	void SetInt(int32 Value)
	{
		Type = EStoryFlowVariableType::Integer;
		IntValue = Value;
	}

	void SetFloat(float Value)
	{
		Type = EStoryFlowVariableType::Float;
		FloatValue = Value;
	}

	void SetString(const FString& Value)
	{
		Type = EStoryFlowVariableType::String;
		StringValue = Value;
	}

	void SetEnum(const FString& Value)
	{
		Type = EStoryFlowVariableType::Enum;
		StringValue = Value;
	}

	void SetArray(const TArray<FStoryFlowVariant>& Value)
	{
		ArrayValue = Value;
		MapValue.Empty(); // a variant holds either array or map data, never both
		// Infer type from first element, or keep current type
		if (Value.Num() > 0)
		{
			Type = Value[0].GetType();
		}
	}

	// Defined below FStoryFlowMapEntry (assigning the entry array needs the complete type)
	void SetMap(const TArray<FStoryFlowMapEntry>& Value);

	// Getters with default fallbacks
	bool GetBool(bool Default = false) const
	{
		return Type == EStoryFlowVariableType::Boolean ? bBoolValue : Default;
	}

	int32 GetInt(int32 Default = 0) const
	{
		return Type == EStoryFlowVariableType::Integer ? IntValue : Default;
	}

	float GetFloat(float Default = 0.0f) const
	{
		return Type == EStoryFlowVariableType::Float ? FloatValue : Default;
	}

	FString GetString(const FString& Default = TEXT("")) const
	{
		// Image, Audio, and Character types also store their values in StringValue
		if (Type == EStoryFlowVariableType::String || Type == EStoryFlowVariableType::Enum ||
			Type == EStoryFlowVariableType::Image || Type == EStoryFlowVariableType::Audio ||
			Type == EStoryFlowVariableType::Character)
		{
			return StringValue;
		}
		return Default;
	}

	const TArray<FStoryFlowVariant>& GetArray() const
	{
		return ArrayValue;
	}

	TArray<FStoryFlowVariant>& GetArrayMutable()
	{
		return ArrayValue;
	}

	bool IsMap() const
	{
		return Type == EStoryFlowVariableType::Map;
	}

	const TArray<FStoryFlowMapEntry>& GetMap() const
	{
		return MapValue;
	}

	// Caller must have established Type via SetMap first (mirrors GetArrayMutable's contract)
	TArray<FStoryFlowMapEntry>& GetMapMutable()
	{
		return MapValue;
	}

	// Conversion to string for display
	FString ToString() const
	{
		switch (Type)
		{
		case EStoryFlowVariableType::Boolean:
			return bBoolValue ? TEXT("true") : TEXT("false");
		case EStoryFlowVariableType::Integer:
			return FString::FromInt(IntValue);
		case EStoryFlowVariableType::Float:
			return FString::SanitizeFloat(FloatValue);
		case EStoryFlowVariableType::String:
		case EStoryFlowVariableType::Enum:
		case EStoryFlowVariableType::Image:
		case EStoryFlowVariableType::Audio:
		case EStoryFlowVariableType::Character:
			return StringValue;
		case EStoryFlowVariableType::Map:
			// Map display formatting is deliberately deferred — interpolation of maps
			// is suppressed contract-wide, so maps render as empty string
			return TEXT("");
		default:
			return TEXT("");
		}
	}

	// Reset — defined below FStoryFlowMapEntry (clearing the entry array needs the complete type)
	void Reset();

	// Static factory methods
	static FStoryFlowVariant FromBool(bool Value)
	{
		FStoryFlowVariant Variant;
		Variant.SetBool(Value);
		return Variant;
	}

	static FStoryFlowVariant FromInt(int32 Value)
	{
		FStoryFlowVariant Variant;
		Variant.SetInt(Value);
		return Variant;
	}

	static FStoryFlowVariant FromFloat(float Value)
	{
		FStoryFlowVariant Variant;
		Variant.SetFloat(Value);
		return Variant;
	}

	static FStoryFlowVariant FromString(const FString& Value)
	{
		FStoryFlowVariant Variant;
		Variant.SetString(Value);
		return Variant;
	}

	/**
	 * Serialize the non-UPROPERTY ArrayValue and MapValue into/from SerializedArrayData.
	 * Must be called before saving (PreSave) and after loading (PostLoad).
	 */
	void PackArrayForSerialization();
	void UnpackArrayFromSerialization();

private:
	/**
	 * Flat binary blob that persists the recursive ArrayValue and MapValue through Unreal's
	 * serialization. Neither can be a UPROPERTY (UHT doesn't support recursive struct arrays),
	 * so we serialize them into this byte array before saving and restore them after loading.
	 */
	UPROPERTY()
	TArray<uint8> SerializedArrayData;
};

/** One map entry. Entry ORDER is observable through mapKeys/mapValues/forEachMap
 *  and must match the editor's serialized order — which is why map storage is an
 *  ordered array of entries, not a TMap (unspecified iteration order). */
struct FStoryFlowMapEntry
{
	FStoryFlowVariant Key;
	FStoryFlowVariant Value;
};

// FStoryFlowVariant members that need the complete FStoryFlowMapEntry type
// (declaring TArray<FStoryFlowMapEntry> only needs the forward declaration,
// but assigning/clearing the entry array does not)

inline void FStoryFlowVariant::SetMap(const TArray<FStoryFlowMapEntry>& Value)
{
	Type = EStoryFlowVariableType::Map;
	MapValue = Value;
	ArrayValue.Empty(); // a variant holds either array or map data, never both
}

inline void FStoryFlowVariant::Reset()
{
	Type = EStoryFlowVariableType::None;
	bBoolValue = false;
	IntValue = 0;
	FloatValue = 0.0f;
	StringValue.Empty();
	ArrayValue.Empty();
	MapValue.Empty();
	SerializedArrayData.Empty();
}

// ============================================================================
// Variable Definition
// ============================================================================

/**
 * Variable definition and value
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowVariable
{
	GENERATED_BODY()

	/** Generated hash ID (var_XXXXXXXX) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Human-readable display name (used for lookup in Get/Set functions) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Variable type */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowVariableType Type = EStoryFlowVariableType::Boolean;

	/** Current value */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "StoryFlow")
	FStoryFlowVariant Value;

	/** Array flag */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsArray = false;

	/** Enum values (for enum type) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FString> EnumValues;

	/** Key type (for map type: String, Integer, or Enum) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowVariableType KeyType = EStoryFlowVariableType::String;

	/** Value type (for map type) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowVariableType ValueType = EStoryFlowVariableType::String;

	/** Key enum values (for map type with enum keys) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FString> KeyEnumValues;

	/** Value enum values (for map type with enum values) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FString> ValueEnumValues;

	/** When true, this variable is exposed as an input on Run Script nodes calling this script */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsInput = false;

	/** When true, this variable is an output value returned from the script */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsOutput = false;
};

/**
 * Pack/unpack all FStoryFlowVariant array values or map entries in a variable map.
 * Use these helpers in asset PreSave/PostLoad to persist array/map data that lives
 * in the non-UPROPERTY FStoryFlowVariant::ArrayValue and MapValue fields.
 */
STORYFLOWRUNTIME_API void PackVariablesForSerialization(TMap<FString, FStoryFlowVariable>& Variables);
STORYFLOWRUNTIME_API void UnpackVariablesFromSerialization(TMap<FString, FStoryFlowVariable>& Variables);

// ============================================================================
// Text Block
// ============================================================================

/**
 * Non-interactive text block displayed in dialogue
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowTextBlock
{
	GENERATED_BODY()

	/** Unique text block ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** String table key for display text */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Text;
};

// ============================================================================
// Choice / Dialogue Option
// ============================================================================

/**
 * Button option definition
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowChoice
{
	GENERATED_BODY()

	/** Unique option ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** String table key for display text */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Text;

	/** Hide after first selection */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bOnceOnly = false;
};

// ============================================================================
// Weighted Option (for Random Branch nodes)
// ============================================================================

/**
 * A weighted output option for random branch nodes
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowWeightedOption
{
	GENERATED_BODY()

	/** Unique option ID (matches source handle suffix) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Relative weight (higher = more likely) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	int32 Weight = 1;
};

// ============================================================================
// Flow Definition
// ============================================================================

/**
 * Flow definition (in-script subflow metadata)
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowFlowDef
{
	GENERATED_BODY()

	/** Flow ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Flow name */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** When true, this flow is an exit route (script termination signal) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsExit = false;
};

// ============================================================================
// Script Interface (for RunScript nodes)
// ============================================================================

/**
 * Parameter/output definition in a script interface
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowScriptInterfaceParam
{
	GENERATED_BODY()

	/** Variable ID in the target script */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Display name */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Variable type (boolean, integer, float, string, enum, image, character, audio) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Type;

	/** Whether this parameter is an array type */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsArray = false;
};

/**
 * Exit route definition in a script interface
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowScriptInterfaceExit
{
	GENERATED_BODY()

	/** Flow ID in the target script */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Display name */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;
};

// ============================================================================
// Node Data
// ============================================================================

/**
 * Type-specific node data container
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowNodeData
{
	GENERATED_BODY()

	// === Common Fields ===

	/** Variable reference (for get/set nodes) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Variable;

	/** Scope flag */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsGlobal = false;

	/** Fallback value (when no connection) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant Value;

	/** First input fallback */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant Value1;

	/** Second input fallback */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant Value2;

	// === Dialogue Fields ===

	/** String table key for title */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Title;

	/** String table key for text */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Text;

	/** Asset key for image */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Image;

	/** Whether to reset/clear image when this dialogue has no image */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bImageReset = false;

	/** Asset key for audio */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Audio;

	/** Whether audio should loop continuously */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioLoop = false;

	/** Whether to stop previous audio when this dialogue has no audio */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioReset = false;

	/** Whether to auto-advance when audio finishes playing (non-looped, no interactive options) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioAdvanceOnEnd = false;

	/** Whether to allow the player to skip audio and advance early (only when bAudioAdvanceOnEnd is true) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioAllowSkip = false;

	/** Character key */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Character;

	/** Text blocks (non-interactive text displayed in dialogue) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowTextBlock> TextBlocks;

	/** Button options */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowChoice> Options;

	/** Input source flags */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bImageUseVarInput = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioUseVarInput = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bCharacterUseVarInput = false;

	// === Script Execution Fields ===

	/** Script path for runScript nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Script;

	/** Flow ID for flow nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString FlowId;

	// === Enum Fields ===

	/** Target enum variable (for intToEnum, etc.) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString EnumVariable;

	/** Enum value list (for intToEnum/stringToEnum conversions and enum variable nodes) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FString> EnumValues;

	// === Character Variable Fields (for getCharacterVar/setCharacterVar) ===

	/** Character path for character variable nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString CharacterPath;

	/** Variable name for character variable nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString VariableName;

	/** Variable type for character variable nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString VariableType;

	/** Array flag for character variable nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsArray = false;

	// === Map Fields (for map variable nodes) ===

	/** Key type for map nodes (string, integer, enum) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString KeyType;

	/** Value type for map nodes */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString ValueType;

	/** Inline key fallback (when the key input has no connection) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant MapKey;

	/** Inline value fallback (when the value input has no connection).
	 *  For map op nodes read this, NOT Data.Value — Data.Value is parsed without
	 *  type context and mis-types integral floats and enums. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant MapInlineValue;

	// === Random Branch Fields ===

	/** Weighted output options (for randomBranch nodes) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowWeightedOption> RandomBranchOptions;

	// === Script Interface Fields (for runScript nodes) ===

	/** Input parameters from the called script's interface */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowScriptInterfaceParam> ScriptParameters;

	/** Output variables from the called script's interface */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowScriptInterfaceParam> ScriptOutputs;

	/** Exit routes from the called script's interface */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowScriptInterfaceExit> ScriptExits;
};

// ============================================================================
// Node Definition
// ============================================================================

/**
 * Represents a single node in the graph
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowNode
{
	GENERATED_BODY()

	/** Node type */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowNodeType Type = EStoryFlowNodeType::Unknown;

	/** Type as string (for extensibility) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString TypeString;

	/** Unique node ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Type-specific data */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowNodeData Data;
};

// ============================================================================
// Connection / Edge
// ============================================================================

/**
 * Edge connecting two nodes.
 *
 * Handle format (matches StoryFlow Editor node graph conventions):
 *   Source: "source-{nodeId}-{suffix}"
 *     - Flow outputs:    "source-{nodeId}-"  (empty suffix for default output)
 *     - Branch:          "source-{nodeId}-true", "source-{nodeId}-false"
 *     - Typed outputs:   "source-{nodeId}-{type}-"  (boolean, integer, float, string, enum, image, character, audio)
 *     - Dialogue option: "source-{nodeId}-{optionId}"
 *   Target: "target-{nodeId}-{suffix}"
 *
 * Data edges (typed outputs) are distinguished from flow edges by containing a type segment
 * such as "-boolean-", "-integer-", etc. in the SourceHandle.
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowConnection
{
	GENERATED_BODY()

	/** Unique edge ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Source node ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Source;

	/** Target node ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Target;

	/** Source handle (for multi-output nodes). See handle format above. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString SourceHandle;

	/** Target handle (for multi-input nodes). See handle format above. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString TargetHandle;
};

// ============================================================================
// Asset Reference
// ============================================================================

/**
 * Asset reference in registry
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowAsset
{
	GENERATED_BODY()

	/** Generated asset ID (asset_image_1, etc.) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Asset type */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowAssetType Type = EStoryFlowAssetType::Image;

	/** Relative path within project (normalized) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Path;

	/** Loaded Unreal asset reference */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TSoftObjectPtr<UObject> LoadedAsset;
};

// ============================================================================
// String Table
// ============================================================================

/**
 * Localized string lookup table
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowStringTable
{
	GENERATED_BODY()

	/** String key -> localized value */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FString> Entries;

	/** Get string with fallback */
	FString Get(const FString& Key, const FString& Fallback = TEXT("")) const
	{
		if (const FString* Value = Entries.Find(Key))
		{
			return *Value;
		}
		return Fallback.IsEmpty() ? Key : Fallback;
	}
};

// ============================================================================
// Character System
// ============================================================================

/**
 * Character definition
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowCharacterDef
{
	GENERATED_BODY()

	/** String table key for name */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Asset key for default image */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Image;

	/** Cached resolved image texture (set when SetCharacterVar Image runs, used as cross-script fallback) */
	UPROPERTY()
	TObjectPtr<UTexture2D> CachedImage = nullptr;

	/** Character-specific variables */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariable> Variables;
};

/**
 * Resolved character data for runtime display
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowCharacterData
{
	GENERATED_BODY()

	/** Resolved character name */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Loaded character image */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TObjectPtr<UTexture2D> Image = nullptr;

	/** Character variables (for interpolation) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TMap<FString, FStoryFlowVariant> Variables;
};

// ============================================================================
// Runtime State Structures
// ============================================================================

/**
 * Call stack frame for script nesting (runScript nodes)
 * Scripts RETURN to the runScript node's output when they end
 */
USTRUCT()
struct STORYFLOWRUNTIME_API FStoryFlowCallFrame
{
	GENERATED_BODY()

	/** Path of the calling script */
	UPROPERTY()
	FString ScriptPath;

	/** Node ID to return to after END */
	UPROPERTY()
	FString ReturnNodeId;

	/** Reference to the calling script asset */
	UPROPERTY()
	TWeakObjectPtr<UStoryFlowScriptAsset> ScriptAsset;

	/** Saved local variables */
	UPROPERTY()
	TMap<FString, FStoryFlowVariable> SavedVariables;

	/** Saved flow call stack (flows are in-script, so we save/restore when crossing script boundaries) */
	UPROPERTY()
	TArray<FString> SavedFlowStack;
};

/**
 * Flow stack entry for in-script flow nesting (runFlow nodes)
 * Flows do NOT return - they simply end when reaching an End node
 * This stack is only for tracking depth to prevent infinite recursion
 */
USTRUCT()
struct STORYFLOWRUNTIME_API FStoryFlowFlowFrame
{
	GENERATED_BODY()

	/** Flow ID being executed */
	UPROPERTY()
	FString FlowId;
};

/**
 * Loop context for forEach nodes
 */
USTRUCT()
struct STORYFLOWRUNTIME_API FStoryFlowLoopContext
{
	GENERATED_BODY()

	/** Loop node ID */
	UPROPERTY()
	FString NodeId;

	/** Loop type */
	UPROPERTY()
	EStoryFlowLoopType Type = EStoryFlowLoopType::ForEach;

	/** Current iteration index */
	UPROPERTY()
	int32 CurrentIndex = 0;
};

// ============================================================================
// Dialogue State (for UI)
// ============================================================================

/**
 * Resolved dialogue option for display
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowDialogueOption
{
	GENERATED_BODY()

	/** Option ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Resolved display text */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Text;
};

/**
 * Current dialogue display state
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowDialogueState
{
	GENERATED_BODY()

	/** Current dialogue node ID */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString NodeId;

	/** Resolved title text */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Title;

	/** Resolved dialogue text (with variable interpolation) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Text;

	/** Current image asset */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TObjectPtr<UTexture2D> Image = nullptr;

	/** Current audio asset */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TObjectPtr<USoundBase> Audio = nullptr;

	/** Current character data */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowCharacterData Character;

	/** Visible text blocks (non-interactive) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowDialogueOption> TextBlocks;

	/** Visible options (buttons, filtered by visibility and once-only) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowDialogueOption> Options;

	/** Is this state valid/active */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bIsValid = false;

	/** True when dialogue can be advanced without options (narrative-only with connected output) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bCanAdvance = false;

	/** True when dialogue will auto-advance after audio finishes playing */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioAdvanceOnEnd = false;

	/** True when player can click to skip audio and advance early (only when bAudioAdvanceOnEnd is true) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioAllowSkip = false;
};

// ============================================================================
// Project Metadata
// ============================================================================

/**
 * Project metadata
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowProjectMetadata
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Title;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FString Description;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FDateTime Created;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "StoryFlow")
	FDateTime Modified;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Parse node type string to enum
 */
STORYFLOWRUNTIME_API EStoryFlowNodeType ParseNodeType(const FString& TypeString);

/**
 * Parse handle string to components
 */
USTRUCT()
struct STORYFLOWRUNTIME_API FStoryFlowHandle
{
	GENERATED_BODY()

	/** Handle direction */
	bool bIsSource = false;

	/** Node ID */
	FString NodeId;

	/** Data type (boolean, integer, float, string, etc.) */
	FString Type;

	/** Additional suffix (index, option ID, etc.) */
	FString Suffix;

	/** Parse handle string */
	static bool Parse(const FString& HandleString, FStoryFlowHandle& OutHandle);
};
