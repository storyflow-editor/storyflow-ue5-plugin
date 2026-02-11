// Copyright 2026 StoryFlow. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace StoryFlowAudio
{
	/**
	 * Convert an MP3 file to a temporary WAV file for Unreal import.
	 * Uses minimp3 (CC0 public domain) for decoding.
	 *
	 * @param Mp3Path Full path to source MP3 file
	 * @param OutWavPath Receives the path to the generated temporary WAV file (caller must delete after use)
	 * @return true if conversion succeeded
	 */
	bool ConvertMp3ToWav(const FString& Mp3Path, FString& OutWavPath);
}
