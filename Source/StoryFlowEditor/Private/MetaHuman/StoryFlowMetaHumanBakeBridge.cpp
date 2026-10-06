// Copyright 2026 StoryFlow. All Rights Reserved.
#include "MetaHuman/StoryFlowMetaHumanBakeBridge.h"

#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Sound/SoundWave.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace StoryFlowMetaHumanBakeBridge
{
namespace
{
bool SetBool(const UStruct* Type, void* Data, FName Name, bool Value)
{
	if (const auto* Property = FindFProperty<FBoolProperty>(Type, Name))
	{
		Property->SetPropertyValue_InContainer(Data, Value);
		return true;
	}
	return false;
}

bool SetObject(const UStruct* Type, void* Data, FName Name, UObject* Value)
{
	if (const auto* Property = FindFProperty<FObjectPropertyBase>(Type, Name))
	{
		if (Value && !Value->IsA(Property->PropertyClass)) return false;
		Property->SetObjectPropertyValue_InContainer(Data, Value);
		return true;
	}
	return false;
}

bool SetEnum(const UStruct* Type, void* Data, FName Name, const TCHAR* Value)
{
	if (const auto* Property = FindFProperty<FEnumProperty>(Type, Name))
	{
		const int64 Number = Property->GetEnum()->GetValueByNameString(Value);
		if (Number == INDEX_NONE) return false;
		Property->GetUnderlyingProperty()->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Data), Number);
		return true;
	}
	return false;
}

bool SetString(UObject* Object, FName Name, const FString& Value)
{
	if (const auto* Property = FindFProperty<FStrProperty>(Object->GetClass(), Name))
	{
		Property->SetPropertyValue_InContainer(Object, Value);
		return true;
	}
	return false;
}

bool BoolResult(UObject* Object, FName Name)
{
	UFunction* Function = Object ? Object->FindFunction(Name) : nullptr;
	const auto* Return = Function ? FindFProperty<FBoolProperty>(Function, TEXT("ReturnValue")) : nullptr;
	if (!Return || Function->NumParms != 1) return false;
	FStructOnScope Parameters(Function);
	Object->ProcessEvent(Function, Parameters.GetStructMemory());
	return Return->GetPropertyValue_InContainer(Parameters.GetStructMemory());
}

bool SetBlocking(UObject* Object)
{
	UFunction* Function = Object->FindFunction(TEXT("SetBlockingProcessing"));
	if (!Function || Function->NumParms != 1) return false;
	FStructOnScope Parameters(Function);
	if (!SetBool(Function, Parameters.GetStructMemory(), TEXT("bInBlockingProcessing"), false)) return false;
	Object->ProcessEvent(Function, Parameters.GetStructMemory());
	return true;
}

UClass* EpicClass(const TCHAR* Name)
{
	return FindObject<UClass>(nullptr, *(FString(TEXT("/Script/MetaHumanPerformance.")) + Name));
}
}

FString UnavailableReason()
{
#if !PLATFORM_WINDOWS || UE_VERSION_OLDER_THAN(5, 6, 0)
	return TEXT("Offline baking requires Windows and Unreal Engine 5.6 or newer. Live and previously baked playback remain available.");
#else
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MetaHuman"));
	// Enabled dependencies count too (for example MetaHuman Character enables MetaHuman).
	// StoryFlow never enables this optional plugin itself.
	if (!Plugin.IsValid() || !Plugin->IsEnabled())
		return TEXT("Enable the MetaHuman Animator plugin in this project to bake dialogue voices. Live and previously baked playback remain available.");
	return FString();
#endif
}

bool ValidateSchema(FString& Error)
{
	Error = UnavailableReason();
	if (!Error.IsEmpty()) return false;
	if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("MetaHumanPerformance")))
	{
		Error = TEXT("The enabled MetaHuman Animator module could not be loaded. Check its installation and restart the editor.");
		return false;
	}
	UClass* Class = EpicClass(TEXT("MetaHumanPerformance"));
	UClass* Settings = EpicClass(TEXT("MetaHumanPerformanceExportAnimationSettings"));
	UClass* Utils = EpicClass(TEXT("MetaHumanPerformanceExportUtils"));
	if (!Class || !Settings || !Utils) { Error = TEXT("MetaHuman Animator did not expose its performance/export classes."); return false; }
	auto Fail = [&Error](const TCHAR* Member) { Error = FString::Printf(TEXT("MetaHuman Animator's baking API is missing or incompatible: %s."), Member); return false; };
	auto HasEnum = [](const UStruct* Type, FName Member, const TCHAR* Value)
	{
		const auto* Property = FindFProperty<FEnumProperty>(Type, Member);
		return Property && Property->GetEnum()->GetValueByNameString(Value) != INDEX_NONE;
	};
	for (const TCHAR* Name : { TEXT("bRealtimeAudio"), TEXT("bGenerateBlinks"), TEXT("bDownmixChannels") })
		if (!FindFProperty<FBoolProperty>(Class, Name)) return Fail(Name);
	for (const TCHAR* Name : { TEXT("Audio"), TEXT("VisualizationMesh") })
		if (!FindFProperty<FObjectPropertyBase>(Class, Name)) return Fail(Name);
	if (!HasEnum(Class, TEXT("InputType"), TEXT("Audio"))) return Fail(TEXT("InputType.Audio"));
	if (!HasEnum(Class, TEXT("HeadMovementMode"), TEXT("Disabled"))) return Fail(TEXT("HeadMovementMode.Disabled"));
	if (!HasEnum(Class, TEXT("AudioDrivenAnimationOutputControls"), TEXT("MouthOnly"))) return Fail(TEXT("AudioDrivenAnimationOutputControls.MouthOnly"));
	const auto* Overrides = FindFProperty<FStructProperty>(Class, TEXT("AudioDrivenAnimationSolveOverrides"));
	if (!Overrides || !HasEnum(Overrides->Struct, TEXT("Mood"), TEXT("Neutral"))) return Fail(TEXT("SolveOverrides.Mood.Neutral"));
	const auto* Delegate = FindFProperty<FMulticastDelegateProperty>(Class, TEXT("OnProcessingFinishedDynamic"));
	if (!Delegate || !Delegate->SignatureFunction || Delegate->SignatureFunction->NumParms != 0) return Fail(TEXT("OnProcessingFinishedDynamic"));
	for (const TCHAR* Name : { TEXT("IsProcessing"), TEXT("ContainsAnimationData") })
	{
		const auto* Function = Class->FindFunctionByName(Name);
		if (!Function || Function->NumParms != 1 || !FindFProperty<FBoolProperty>(Function, TEXT("ReturnValue"))) return Fail(Name);
	}
	const auto* CancelFunction = Class->FindFunctionByName(TEXT("CancelPipeline"));
	if (!CancelFunction || CancelFunction->NumParms != 0) return Fail(TEXT("CancelPipeline"));
	const auto* BlockingFunction = Class->FindFunctionByName(TEXT("SetBlockingProcessing"));
	if (!BlockingFunction || BlockingFunction->NumParms != 1 || !FindFProperty<FBoolProperty>(BlockingFunction, TEXT("bInBlockingProcessing"))) return Fail(TEXT("SetBlockingProcessing"));
	const auto* StartFunction = Class->FindFunctionByName(TEXT("StartPipeline"));
	if (!StartFunction || StartFunction->NumParms != 2 || !FindFProperty<FBoolProperty>(StartFunction, TEXT("bInIsScriptedProcessing"))
		|| !HasEnum(StartFunction, TEXT("ReturnValue"), TEXT("None"))) return Fail(TEXT("StartPipeline"));
	for (const TCHAR* Name : { TEXT("bShowExportDialog"), TEXT("bAutoSaveAnimSequence"), TEXT("bEnableHeadMovement") })
		if (!FindFProperty<FBoolProperty>(Settings, Name)) return Fail(Name);
	for (const TCHAR* Name : { TEXT("PackagePath"), TEXT("AssetName") })
		if (!FindFProperty<FStrProperty>(Settings, Name)) return Fail(Name);
	if (!FindFProperty<FObjectPropertyBase>(Settings, TEXT("TargetSkeletonOrSkeletalMesh"))
		|| !HasEnum(Settings, TEXT("ExportRange"), TEXT("ProcessingRange"))) return Fail(TEXT("ExportSettings"));
	const auto* ExportFunction = Utils->FindFunctionByName(TEXT("ExportAnimationSequence"));
	if (!ExportFunction || ExportFunction->NumParms != 3) return Fail(TEXT("ExportAnimationSequence"));
	for (const TCHAR* Name : { TEXT("InPerformance"), TEXT("InExportSettings"), TEXT("ReturnValue") })
		if (!FindFProperty<FObjectPropertyBase>(ExportFunction, Name)) return Fail(Name);
	return true;
}

UObject* CreatePerformance(UObject* Outer, USoundWave* Voice, USkeletalMesh* Face,
	UObject* Listener, FName FinishedFunction, FString& Error)
{
	if (!ValidateSchema(Error)) return nullptr;
	if (!Voice || !Face || !Listener || !Listener->FindFunction(FinishedFunction))
	{
		Error = TEXT("A voice, facial mesh and completion listener are required for baking.");
		return nullptr;
	}
	UClass* Class = EpicClass(TEXT("MetaHumanPerformance"));
	UObject* Performance = NewObject<UObject>(Outer, Class, NAME_None, RF_Transient);
	const auto* Overrides = FindFProperty<FStructProperty>(Class, TEXT("AudioDrivenAnimationSolveOverrides"));
	const auto* Finished = FindFProperty<FMulticastDelegateProperty>(Class, TEXT("OnProcessingFinishedDynamic"));
	const bool bConfigured = SetEnum(Class, Performance, TEXT("InputType"), TEXT("Audio"))
		&& SetObject(Class, Performance, TEXT("Audio"), Voice)
		&& SetObject(Class, Performance, TEXT("VisualizationMesh"), Face)
		&& SetBool(Class, Performance, TEXT("bRealtimeAudio"), false)
		&& SetBool(Class, Performance, TEXT("bGenerateBlinks"), false)
		&& SetBool(Class, Performance, TEXT("bDownmixChannels"), true)
		&& SetEnum(Class, Performance, TEXT("HeadMovementMode"), TEXT("Disabled"))
		&& SetEnum(Class, Performance, TEXT("AudioDrivenAnimationOutputControls"), TEXT("MouthOnly"))
		&& Overrides && SetEnum(Overrides->Struct, Overrides->ContainerPtrToValuePtr<void>(Performance), TEXT("Mood"), TEXT("Neutral"));
	if (!bConfigured || !Finished || !Performance->FindFunction(TEXT("StartPipeline"))
		|| !Performance->FindFunction(TEXT("CancelPipeline")) || !Performance->FindFunction(TEXT("IsProcessing"))
		|| !Performance->FindFunction(TEXT("ContainsAnimationData")) || !SetBlocking(Performance))
	{
		Error = TEXT("This MetaHuman Animator version does not expose the expected audio baking API.");
		return nullptr;
	}
	FPropertyChangedEvent Changed(FindFProperty<FProperty>(Class, TEXT("Audio")));
	Performance->PostEditChangeProperty(Changed);
	FScriptDelegate Callback;
	Callback.BindUFunction(Listener, FinishedFunction);
	Finished->AddDelegate(Callback, Performance, Finished->ContainerPtrToValuePtr<void>(Performance));
	return Performance;
}

bool Start(UObject* Performance, FString& Error)
{
	UFunction* Function = Performance ? Performance->FindFunction(TEXT("StartPipeline")) : nullptr;
	const auto* Return = Function ? FindFProperty<FEnumProperty>(Function, TEXT("ReturnValue")) : nullptr;
	if (!Function || !Return || Function->NumParms != 2)
	{
		Error = TEXT("MetaHuman Animator's StartPipeline signature is unsupported.");
		return false;
	}
	FStructOnScope Parameters(Function);
	if (!SetBool(Function, Parameters.GetStructMemory(), TEXT("bInIsScriptedProcessing"), true))
	{
		Error = TEXT("MetaHuman Animator's scripted processing parameter is unavailable.");
		return false;
	}
	Performance->ProcessEvent(Function, Parameters.GetStructMemory());
	const int64 Result = Return->GetUnderlyingProperty()->GetSignedIntPropertyValue(Return->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()));
	if (Result != Return->GetEnum()->GetValueByNameString(TEXT("None")))
	{
		Error = FString::Printf(TEXT("MetaHuman Animator could not start (%s)."), *Return->GetEnum()->GetNameStringByValue(Result));
		return false;
	}
	return true;
}

bool IsProcessing(UObject* Performance) { return BoolResult(Performance, TEXT("IsProcessing")); }
bool ContainsAnimation(UObject* Performance) { return BoolResult(Performance, TEXT("ContainsAnimationData")); }

void Cancel(UObject* Performance)
{
	UFunction* Function = Performance ? Performance->FindFunction(TEXT("CancelPipeline")) : nullptr;
	if (Function && Function->NumParms == 0) Performance->ProcessEvent(Function, nullptr);
}

void Unbind(UObject* Performance, UObject* Listener, FName FinishedFunction)
{
	if (!Performance) return;
	if (const auto* Finished = FindFProperty<FMulticastDelegateProperty>(Performance->GetClass(), TEXT("OnProcessingFinishedDynamic")))
	{
		FScriptDelegate Callback;
		Callback.BindUFunction(Listener, FinishedFunction);
		Finished->RemoveDelegate(Callback, Performance, Finished->ContainerPtrToValuePtr<void>(Performance));
	}
}

UAnimSequence* Export(UObject* Performance, USkeletalMesh* Face, const FString& PackagePath,
	const FString& AssetName, FString& Error)
{
	UClass* SettingsClass = EpicClass(TEXT("MetaHumanPerformanceExportAnimationSettings"));
	UClass* UtilsClass = EpicClass(TEXT("MetaHumanPerformanceExportUtils"));
	UFunction* Function = UtilsClass ? UtilsClass->FindFunctionByName(TEXT("ExportAnimationSequence")) : nullptr;
	const auto* Return = Function ? FindFProperty<FObjectPropertyBase>(Function, TEXT("ReturnValue")) : nullptr;
	if (!SettingsClass || !Function || !Return || Function->NumParms != 3)
	{
		Error = TEXT("MetaHuman Animator's animation export API is unavailable.");
		return nullptr;
	}
	TStrongObjectPtr<UObject> Settings(NewObject<UObject>(GetTransientPackage(), SettingsClass));
	const bool bConfigured = SetBool(SettingsClass, Settings.Get(), TEXT("bShowExportDialog"), false)
		&& SetBool(SettingsClass, Settings.Get(), TEXT("bAutoSaveAnimSequence"), false)
		&& SetBool(SettingsClass, Settings.Get(), TEXT("bEnableHeadMovement"), false)
		&& SetObject(SettingsClass, Settings.Get(), TEXT("TargetSkeletonOrSkeletalMesh"), Face)
		&& SetEnum(SettingsClass, Settings.Get(), TEXT("ExportRange"), TEXT("ProcessingRange"))
		&& SetString(Settings.Get(), TEXT("PackagePath"), PackagePath)
		&& SetString(Settings.Get(), TEXT("AssetName"), AssetName);
	FStructOnScope Parameters(Function);
	if (!bConfigured || !SetObject(Function, Parameters.GetStructMemory(), TEXT("InPerformance"), Performance)
		|| !SetObject(Function, Parameters.GetStructMemory(), TEXT("InExportSettings"), Settings.Get()))
	{
		Error = TEXT("MetaHuman Animator's animation export settings are unsupported.");
		return nullptr;
	}
	UtilsClass->GetDefaultObject()->ProcessEvent(Function, Parameters.GetStructMemory());
	return Cast<UAnimSequence>(Return->GetObjectPropertyValue_InContainer(Parameters.GetStructMemory()));
}
}
