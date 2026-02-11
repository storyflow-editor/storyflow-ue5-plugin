// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Handle format constants and helpers for StoryFlow node graph edges.
 *
 * These match the handle IDs assigned by the StoryFlow Editor's React Flow node graph.
 * When the editor exports a project to JSON, each edge preserves its sourceHandle and
 * targetHandle strings. The runtime uses these to resolve which port an edge connects to.
 *
 * Format:
 *   Source handles: "source-{nodeId}-{suffix}"  (output ports)
 *   Target handles: "target-{nodeId}-{suffix}"  (input ports)
 */
namespace StoryFlowHandles
{
	// ========================================================================
	// Handle Builders
	// ========================================================================

	/** Build a source handle: "source-{NodeId}-{Suffix}" */
	inline FString Source(const FString& NodeId, const FString& Suffix = TEXT(""))
	{
		return FString::Printf(TEXT("source-%s-%s"), *NodeId, *Suffix);
	}

	/** Build a target handle: "target-{NodeId}-{Suffix}" */
	inline FString Target(const FString& NodeId, const FString& Suffix = TEXT(""))
	{
		return FString::Printf(TEXT("target-%s-%s"), *NodeId, *Suffix);
	}

	// ========================================================================
	// Source Handle Output Suffixes
	// (appended after "source-{nodeId}-")
	// ========================================================================

	/** Default/header output — empty suffix (Start, End, EntryFlow, Get* data nodes) */
	inline constexpr const TCHAR* Out_Default = TEXT("");

	/** Branch true output */
	inline constexpr const TCHAR* Out_True = TEXT("true");

	/** Branch false output */
	inline constexpr const TCHAR* Out_False = TEXT("false");

	/** Set* node flow-through output (the "1" port below the node in the editor) */
	inline constexpr const TCHAR* Out_Flow = TEXT("1");

	/** RunScript / media node output */
	inline constexpr const TCHAR* Out_Output = TEXT("output");

	/** ForEach loop body output */
	inline constexpr const TCHAR* Out_LoopBody = TEXT("loopBody");

	/** ForEach loop completed output */
	inline constexpr const TCHAR* Out_LoopCompleted = TEXT("completed");

	// Typed data output suffixes (trailing dash is part of the editor's format)
	inline constexpr const TCHAR* Out_Boolean = TEXT("boolean-");
	inline constexpr const TCHAR* Out_Integer = TEXT("integer-");
	inline constexpr const TCHAR* Out_Float = TEXT("float-");
	inline constexpr const TCHAR* Out_String = TEXT("string-");
	inline constexpr const TCHAR* Out_Enum = TEXT("enum-");

	// ========================================================================
	// Target Handle Input Suffixes
	// (used with FindInputEdge to locate incoming data edges)
	// ========================================================================

	// Single typed inputs (used by conversion nodes and single-input operations)
	inline constexpr const TCHAR* In_Boolean = TEXT("boolean");
	inline constexpr const TCHAR* In_Integer = TEXT("integer");
	inline constexpr const TCHAR* In_Float = TEXT("float");
	inline constexpr const TCHAR* In_String = TEXT("string");
	inline constexpr const TCHAR* In_Enum = TEXT("enum");
	inline constexpr const TCHAR* In_Image = TEXT("image");
	inline constexpr const TCHAR* In_Character = TEXT("character");
	inline constexpr const TCHAR* In_Audio = TEXT("audio");

	// Numbered inputs (used by binary operations: and, or, equal, arithmetic, etc.)
	inline constexpr const TCHAR* In_Boolean1 = TEXT("boolean-1");
	inline constexpr const TCHAR* In_Boolean2 = TEXT("boolean-2");
	inline constexpr const TCHAR* In_BooleanCondition = TEXT("boolean-condition");
	inline constexpr const TCHAR* In_Integer1 = TEXT("integer-1");
	inline constexpr const TCHAR* In_Integer2 = TEXT("integer-2");
	inline constexpr const TCHAR* In_IntegerIndex = TEXT("integer-index");
	inline constexpr const TCHAR* In_IntegerValue = TEXT("integer-value");
	inline constexpr const TCHAR* In_Float1 = TEXT("float-1");
	inline constexpr const TCHAR* In_Float2 = TEXT("float-2");
	inline constexpr const TCHAR* In_String1 = TEXT("string-1");
	inline constexpr const TCHAR* In_String2 = TEXT("string-2");
	inline constexpr const TCHAR* In_Enum1 = TEXT("enum-1");
	inline constexpr const TCHAR* In_Enum2 = TEXT("enum-2");

	// Array inputs
	inline constexpr const TCHAR* In_BoolArray = TEXT("boolean-array");
	inline constexpr const TCHAR* In_IntArray = TEXT("integer-array");
	inline constexpr const TCHAR* In_FloatArray = TEXT("float-array");
	inline constexpr const TCHAR* In_StringArray = TEXT("string-array");
	inline constexpr const TCHAR* In_ImageArray = TEXT("image-array");
	inline constexpr const TCHAR* In_CharacterArray = TEXT("character-array");
	inline constexpr const TCHAR* In_AudioArray = TEXT("audio-array");

	// Media node inputs
	inline constexpr const TCHAR* In_ImageInput = TEXT("image-image-input");
	inline constexpr const TCHAR* In_AudioInput = TEXT("audio-audio-input");
	inline constexpr const TCHAR* In_CharacterInput = TEXT("character-character-input");
}
