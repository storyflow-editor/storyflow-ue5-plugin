// Copyright 2026 StoryFlow. All Rights Reserved.

// minimp3 implementation - compiled exactly once in this translation unit.
// minimp3 is CC0 (public domain): https://github.com/lieff/minimp3

#include "Import/StoryFlowMp3Decoder.h"
#include "StoryFlowRuntime.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

// Suppress all third-party warnings including C4706 (assignment within conditional)
THIRD_PARTY_INCLUDES_START
#ifdef _MSC_VER
#pragma warning(disable: 4706)
#endif

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3/minimp3.h"
#include "minimp3/minimp3_ex.h"

THIRD_PARTY_INCLUDES_END

bool StoryFlowAudio::ConvertMp3ToWav(const FString& Mp3Path, FString& OutWavPath)
{
	// Read the MP3 file into memory
	TArray<uint8> Mp3Data;
	if (!FFileHelper::LoadFileToArray(Mp3Data, *Mp3Path))
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Could not read MP3 file: %s"), *Mp3Path);
		return false;
	}

	// Decode MP3 to PCM using minimp3
	mp3dec_t Decoder;
	mp3dec_init(&Decoder);

	mp3dec_file_info_t FileInfo;
	FMemory::Memzero(&FileInfo, sizeof(FileInfo));

	int Result = mp3dec_load_buf(&Decoder, Mp3Data.GetData(), Mp3Data.Num(), &FileInfo, nullptr, nullptr);
	if (Result != 0 || FileInfo.samples == 0 || !FileInfo.buffer)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to decode MP3 (error %d): %s"), Result, *Mp3Path);
		if (FileInfo.buffer)
		{
			free(FileInfo.buffer);
		}
		return false;
	}

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Decoded MP3: %llu samples, %d channels, %d Hz"),
		static_cast<uint64>(FileInfo.samples), FileInfo.channels, FileInfo.hz);

	// Build WAV file in memory
	const int32 NumChannels = FileInfo.channels;
	const int32 SampleRate = FileInfo.hz;
	const int32 BitsPerSample = 16;
	const int32 BytesPerSample = BitsPerSample / 8;

	// Reject files where sample count * bytes-per-sample would overflow int32
	const size_t MaxSamples = static_cast<size_t>(MAX_int32) / BytesPerSample;
	if (FileInfo.samples > MaxSamples)
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: MP3 too large to convert (%llu samples): %s"),
			static_cast<uint64>(FileInfo.samples), *Mp3Path);
		free(FileInfo.buffer);
		return false;
	}

	const int32 NumSamples = static_cast<int32>(FileInfo.samples);
	const int32 DataSize = NumSamples * BytesPerSample;
	const int32 WavFileSize = 44 + DataSize;

	TArray<uint8> WavData;
	WavData.SetNumUninitialized(WavFileSize);
	uint8* Out = WavData.GetData();

	// RIFF header
	FMemory::Memcpy(Out, "RIFF", 4); Out += 4;
	const int32 ChunkSize = WavFileSize - 8;
	FMemory::Memcpy(Out, &ChunkSize, 4); Out += 4;
	FMemory::Memcpy(Out, "WAVE", 4); Out += 4;

	// fmt sub-chunk
	FMemory::Memcpy(Out, "fmt ", 4); Out += 4;
	const int32 SubChunk1Size = 16;
	FMemory::Memcpy(Out, &SubChunk1Size, 4); Out += 4;
	const int16 AudioFormat = 1; // PCM
	FMemory::Memcpy(Out, &AudioFormat, 2); Out += 2;
	const int16 Channels = static_cast<int16>(NumChannels);
	FMemory::Memcpy(Out, &Channels, 2); Out += 2;
	FMemory::Memcpy(Out, &SampleRate, 4); Out += 4;
	const int32 ByteRate = SampleRate * NumChannels * BytesPerSample;
	FMemory::Memcpy(Out, &ByteRate, 4); Out += 4;
	const int16 BlockAlign = static_cast<int16>(NumChannels * BytesPerSample);
	FMemory::Memcpy(Out, &BlockAlign, 2); Out += 2;
	const int16 BPS = static_cast<int16>(BitsPerSample);
	FMemory::Memcpy(Out, &BPS, 2); Out += 2;

	// data sub-chunk
	FMemory::Memcpy(Out, "data", 4); Out += 4;
	FMemory::Memcpy(Out, &DataSize, 4); Out += 4;

	// PCM sample data — minimp3 outputs int16 (mp3d_sample_t) by default
	FMemory::Memcpy(Out, FileInfo.buffer, DataSize);

	// Free minimp3 buffer
	free(FileInfo.buffer);

	// Write to temp file next to the source
	OutWavPath = FPaths::CreateTempFilename(*FPaths::GetPath(Mp3Path), TEXT("sf_mp3_"), TEXT(".wav"));
	if (!FFileHelper::SaveArrayToFile(WavData, *OutWavPath))
	{
		UE_LOG(LogStoryFlow, Error, TEXT("StoryFlow: Failed to write temporary WAV file: %s"), *OutWavPath);
		return false;
	}

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: MP3 converted to temporary WAV: %s"), *OutWavPath);
	return true;
}
