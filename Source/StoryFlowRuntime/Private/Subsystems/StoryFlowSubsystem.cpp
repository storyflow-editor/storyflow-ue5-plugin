// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Subsystems/StoryFlowSubsystem.h"
#include "StoryFlowRuntime.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Data/StoryFlowCharacterAsset.h"
#include "Engine/AssetManager.h"

const FString UStoryFlowSubsystem::DefaultProjectPath = TEXT("/Game/StoryFlow/SF_Project");

void UStoryFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Subsystem initializing..."));

	// Try to auto-load project from default location
	TryAutoLoadProject();
}

void UStoryFlowSubsystem::Deinitialize()
{
	ProjectAsset = nullptr;
	GlobalVariables.Empty();
	RuntimeCharacters.Empty();

	Super::Deinitialize();
}

void UStoryFlowSubsystem::SetProject(UStoryFlowProjectAsset* NewProject)
{
	ProjectAsset = NewProject;

	if (ProjectAsset)
	{
		// Initialize global variables from project
		GlobalVariables = ProjectAsset->GlobalVariables;
		ResolveStringVariableValues(GlobalVariables);

		// Initialize runtime characters from character assets (mutable copies)
		ResetRuntimeCharacters();

		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Project set: %s (%d scripts, %d global variables, %d characters)"),
			*ProjectAsset->GetName(),
			ProjectAsset->Scripts.Num(),
			GlobalVariables.Num(),
			RuntimeCharacters.Num());

		// Log available scripts
		UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Available scripts:"));
		for (const auto& ScriptPair : ProjectAsset->Scripts)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow:   '%s' -> %s"),
				*ScriptPair.Key,
				ScriptPair.Value ? *ScriptPair.Value->GetName() : TEXT("NULL"));
		}
	}
	else
	{
		GlobalVariables.Empty();
		RuntimeCharacters.Empty();
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Project cleared"));
	}
}

UStoryFlowScriptAsset* UStoryFlowSubsystem::GetScript(const FString& ScriptPath) const
{
	if (!ProjectAsset)
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: Cannot get script - no project loaded"));
		return nullptr;
	}

	return ProjectAsset->GetScriptByPath(ScriptPath);
}

TArray<FString> UStoryFlowSubsystem::GetAllScriptPaths() const
{
	TArray<FString> Paths;

	if (ProjectAsset)
	{
		ProjectAsset->Scripts.GetKeys(Paths);
	}

	return Paths;
}

void UStoryFlowSubsystem::ResetGlobalVariables()
{
	if (ProjectAsset)
	{
		GlobalVariables = ProjectAsset->GlobalVariables;
		ResolveStringVariableValues(GlobalVariables);
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Global variables reset to defaults"));
	}
}

void UStoryFlowSubsystem::ResetRuntimeCharacters()
{
	if (ProjectAsset)
	{
		RuntimeCharacters.Empty();
		for (const auto& CharPair : ProjectAsset->Characters)
		{
			if (CharPair.Value)
			{
				FStoryFlowCharacterDef CharDef;
				CharDef.Name = CharPair.Value->Name;
				CharDef.Image = CharPair.Value->Image;
				CharDef.Variables = CharPair.Value->Variables;
				ResolveStringVariableValues(CharDef.Variables);
				RuntimeCharacters.Add(CharPair.Key, CharDef);
			}
		}
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Runtime characters reset to defaults"));
	}
}

void UStoryFlowSubsystem::ResolveStringVariableValues(TMap<FString, FStoryFlowVariable>& Variables)
{
	if (!ProjectAsset)
	{
		return;
	}

	for (auto& VarPair : Variables)
	{
		if (VarPair.Value.Type == EStoryFlowVariableType::String)
		{
			if (VarPair.Value.bIsArray)
			{
				for (auto& Element : VarPair.Value.Value.GetArrayMutable())
				{
					FString Key = Element.GetString();
					if (!Key.IsEmpty())
					{
						Element.SetString(ProjectAsset->GetGlobalString(Key));
					}
				}
			}
			else
			{
				FString Key = VarPair.Value.Value.GetString();
				if (!Key.IsEmpty())
				{
					VarPair.Value.Value.SetString(ProjectAsset->GetGlobalString(Key));
				}
			}
		}
	}
}

void UStoryFlowSubsystem::TryAutoLoadProject()
{
	// Try to load the asset
	UStoryFlowProjectAsset* LoadedProject = Cast<UStoryFlowProjectAsset>(
		StaticLoadObject(UStoryFlowProjectAsset::StaticClass(), nullptr, *DefaultProjectPath)
	);

	if (LoadedProject)
	{
		SetProject(LoadedProject);
		UE_LOG(LogStoryFlow, Log, TEXT("StoryFlow: Auto-loaded project from %s"), *DefaultProjectPath);
	}
	else
	{
		UE_LOG(LogStoryFlow, Warning, TEXT("StoryFlow: No project found at %s. Use SetProject() or import a project to /Game/StoryFlow/"), *DefaultProjectPath);
	}
}
