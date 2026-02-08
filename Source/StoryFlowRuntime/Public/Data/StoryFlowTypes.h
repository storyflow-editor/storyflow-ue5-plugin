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
	Character
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

public:
	// Type checking
	bool IsValid() const { return Type != EStoryFlowVariableType::None; }
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
	}

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
		if (Type == EStoryFlowVariableType::String || Type == EStoryFlowVariableType::Enum)
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
			return StringValue;
		default:
			return TEXT("");
		}
	}

	// Reset
	void Reset()
	{
		Type = EStoryFlowVariableType::None;
		bBoolValue = false;
		IntValue = 0;
		FloatValue = 0.0f;
		StringValue.Empty();
		ArrayValue.Empty();
	}

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
};

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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** String table key for display name */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Variable type */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowVariableType Type = EStoryFlowVariableType::Boolean;

	/** Current value */
	UPROPERTY(BlueprintReadWrite, Category = "StoryFlow")
	FStoryFlowVariant Value;

	/** Array flag */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bIsArray = false;

	/** Enum values (for enum type) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	TArray<FString> EnumValues;
};

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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** String table key for display text */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** String table key for display text */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Text;

	/** Hide after first selection */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bOnceOnly = false;
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Variable;

	/** Scope flag */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bIsGlobal = false;

	/** Fallback value (when no connection) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant Value;

	/** First input fallback */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant Value1;

	/** Second input fallback */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowVariant Value2;

	// === Dialogue Fields ===

	/** String table key for title */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Title;

	/** String table key for text */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Text;

	/** Asset key for image */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Image;

	/** Whether to reset/clear image when this dialogue has no image */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bImageReset = false;

	/** Asset key for audio */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Audio;

	/** Whether audio should loop continuously */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioLoop = false;

	/** Whether to stop previous audio when this dialogue has no audio */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioReset = false;

	/** Character key */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Character;

	/** Text blocks (non-interactive text displayed in dialogue) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowTextBlock> TextBlocks;

	/** Button options */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowChoice> Options;

	/** Input source flags */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bImageUseVarInput = false;

	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bAudioUseVarInput = false;

	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bCharacterUseVarInput = false;

	// === Script Execution Fields ===

	/** Script path for runScript nodes */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Script;

	/** Flow ID for flow nodes */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString FlowId;

	// === Enum Fields ===

	/** Target enum variable (for intToEnum, etc.) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString EnumVariable;

	// === Character Variable Fields (for getCharacterVar/setCharacterVar) ===

	/** Character path for character variable nodes */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString CharacterPath;

	/** Variable name for character variable nodes */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString VariableName;

	/** Variable type for character variable nodes */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString VariableType;

	// === Runtime State (not serialized) ===

	/** Cached output value (for evaluators) */
	FStoryFlowVariant CachedOutput;
	bool bHasCachedOutput = false;

	/** Loop state (for forEach nodes) */
	int32 LoopIndex = -1;
	TArray<FStoryFlowVariant> LoopArray;
	bool bLoopInitialized = false;
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowNodeType Type = EStoryFlowNodeType::Unknown;

	/** Type as string (for extensibility) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString TypeString;

	/** Unique node ID */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Type-specific data */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowNodeData Data;
};

// ============================================================================
// Connection / Edge
// ============================================================================

/**
 * Edge connecting two nodes
 */
USTRUCT(BlueprintType)
struct STORYFLOWRUNTIME_API FStoryFlowConnection
{
	GENERATED_BODY()

	/** Unique edge ID */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Source node ID */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Source;

	/** Target node ID */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Target;

	/** Source handle (for multi-output nodes) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString SourceHandle;

	/** Target handle (for multi-input nodes) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Asset type */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	EStoryFlowAssetType Type = EStoryFlowAssetType::Image;

	/** Relative path within project (normalized) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Path;

	/** Loaded Unreal asset reference */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Asset key for default image */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Image;

	/** Character-specific variables */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Name;

	/** Loaded character image */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	UTexture2D* Image = nullptr;

	/** Character variables (for interpolation) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Id;

	/** Resolved display text */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString NodeId;

	/** Resolved title text */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Title;

	/** Resolved dialogue text (with variable interpolation) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Text;

	/** Current image asset */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	UTexture2D* Image = nullptr;

	/** Current audio asset */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	USoundBase* Audio = nullptr;

	/** Current character data */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FStoryFlowCharacterData Character;

	/** Visible text blocks (non-interactive) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowDialogueOption> TextBlocks;

	/** Visible options (buttons, filtered by visibility and once-only) */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	TArray<FStoryFlowDialogueOption> Options;

	/** Is this state valid/active */
	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	bool bIsValid = false;
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

	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Title;

	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
	FDateTime Created;

	UPROPERTY(BlueprintReadOnly, Category = "StoryFlow")
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
