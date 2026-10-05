// Copyright 2026 StoryFlow. All Rights Reserved.
#include "MetaHuman/StoryFlowMetaHumanBakeSubsystem.h"
#include "EditorReimportHandler.h"
#include "MetaHuman/StoryFlowMetaHumanLipsyncComponent.h"
#include "MetaHuman/StoryFlowMetaHumanAnimInstance.h"
#include "Data/StoryFlowProjectAsset.h"
#include "Data/StoryFlowScriptAsset.h"
#include "Subsystems/StoryFlowEditorSubsystem.h"
#include "Components/StoryFlowComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkeletalMesh.h"
#include "Sound/SoundWave.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Misc/EngineVersion.h"
#include "MetaHuman/StoryFlowMetaHumanBakeBridge.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Engine/AssetUserData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

namespace StoryFlowMetaHumanSetup
{
const UStoryFlowMetaHumanLipsyncComponent* FindSetupTemplate(
	const TArray<const UStoryFlowMetaHumanLipsyncComponent*>& Templates, FName ComponentName)
{
	auto NormalizeName = [](FName Name)
	{
		FString Value = Name.ToString();
		Value.RemoveFromEnd(UActorComponent::ComponentTemplateNameSuffix);
		return FName(*Value);
	};
	const FName Wanted = NormalizeName(ComponentName);
	for (const auto* Template : Templates)
		if (Template && NormalizeName(Template->GetFName()) == Wanted) return Template;
	return nullptr;
}
}

namespace
{
UBlueprint* BlueprintFor(UStoryFlowMetaHumanLipsyncComponent* Component)
{
	if (!Component) return nullptr;
	if (auto* Owner = Component->GetOwner()) return Cast<UBlueprint>(Owner->GetClass()->ClassGeneratedBy);
	if (auto* Class = Component->GetTypedOuter<UBlueprintGeneratedClass>()) return Cast<UBlueprint>(Class->ClassGeneratedBy);
	return Component->GetTypedOuter<UBlueprint>();
}

bool Save(UObject* Asset)
{
	if (!Asset) return false;
	Asset->MarkPackageDirty();
	return UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
}

bool SaveComponent(UStoryFlowMetaHumanLipsyncComponent* Component)
{
	if (auto* Blueprint = BlueprintFor(Component))
	{
		TArray<const UStoryFlowMetaHumanLipsyncComponent*> Templates;
		AActor::GetActorClassDefaultComponents(TSubclassOf<AActor>(Blueprint->GeneratedClass.Get()), Templates);
		const auto* Template = StoryFlowMetaHumanSetup::FindSetupTemplate(Templates, Component->GetFName());
		// A live SCS component omits its template's _GEN_VARIABLE suffix. Never report a
		// successful save when no corresponding template received the setup choices.
		if (!Template) return false;
		{
			auto* Editable = const_cast<UStoryFlowMetaHumanLipsyncComponent*>(Template);
			auto* TargetClass = CastChecked<UBlueprintGeneratedClass>(Blueprint->GeneratedClass.Get());
			if (Template->GetTypedOuter<UBlueprintGeneratedClass>() != TargetClass)
			{
				// Inherited SCS templates belong to the parent until an explicit child
				// override is created. Never write setup choices into the parent.
				Editable = nullptr;
				for (UClass* Parent = TargetClass->GetSuperClass(); Parent && !Editable; Parent = Parent->GetSuperClass())
					if (auto* ParentBlueprint = Cast<UBlueprint>(Parent->ClassGeneratedBy))
						if (ParentBlueprint->SimpleConstructionScript)
							for (auto* Node : ParentBlueprint->SimpleConstructionScript->GetAllNodes())
								if (Node->GetActualComponentTemplate(TargetClass) == Template)
								{
									auto* Handler = Blueprint->GetInheritableComponentHandler(true);
									Handler->Modify();
									Editable = Cast<UStoryFlowMetaHumanLipsyncComponent>(Handler->CreateOverridenComponentTemplate(FComponentKey(Node)));
									break;
								}
				if (!Editable) return false;
			}
			Editable->Modify();
			Editable->BakeLibrary = Component->BakeLibrary;
			Editable->BakingProject = Component->BakingProject;
			Editable->CharacterId = Component->CharacterId;
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		if (!Save(Blueprint)) return false;
	}
	Component->MarkPackageDirty();
	return true;
}

bool Matches(UStoryFlowProjectAsset* Project, const FString& Selected, const FStoryFlowNodeData& Data)
{
	if (Selected.IsEmpty() || Data.bCharacterUseVarInput) return true;
	const FString Wanted = Project->CharacterIdToPath.Contains(Selected) ? Project->CharacterIdToPath[Selected] : Selected;
	const FString Actual = Project->CharacterIdToPath.Contains(Data.CharacterRefId) ? Project->CharacterIdToPath[Data.CharacterRefId] : Data.Character;
	// An unspecified/variable speaker cannot be excluded statically.
	return Actual.IsEmpty() || UStoryFlowLipsyncComponent::SpeakerPathsMatch(Wanted, Actual);
}

FString RigFingerprint(USkeletalMesh* Mesh)
{
	if (!Mesh || !Mesh->GetSkeleton()) return FString();
	TArray<FString> Parts;
	Parts.Add(Mesh->GetPathName());
	Parts.Add(Mesh->GetSkeleton()->GetGuid().ToString());
	if (const auto* Model = Mesh->GetImportedModel()) Parts.Add(Model->GetIdString());
	// DNA can be replaced without changing the mesh path or skeleton. Hash its actual saved payload
	// through UObject's virtual serializer; this neither includes nor links the optional RigLogic module.
	if (const auto* UserData = Mesh->GetAssetUserDataArray())
		for (auto* Data : *UserData)
		{
			bool bDNA = false;
			for (UClass* Class = Data ? Data->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
				bDNA |= Class->GetPathName() == TEXT("/Script/RigLogicModule.DNAAsset");
			if (!bDNA) continue;
			FBufferArchive Bytes;
			FObjectAndNameAsStringProxyArchive Archive(Bytes, false);
			Archive.SetIsPersistent(true);
			Data->Serialize(Archive);
			FMD5 Hash;
			Hash.Update(Bytes.GetData(), Bytes.Num());
			uint8 Digest[16]; Hash.Final(Digest);
			Parts.Add(BytesToHex(Digest, UE_ARRAY_COUNT(Digest)));
		}
	Parts.Sort();
	return FMD5::HashAnsiString(*FString::Join(Parts, TEXT("|")));
}

FString VoiceFingerprint(USoundWave* Voice, USkeletalMesh* FaceMesh, const FString& Rig)
{
	if (!Voice || !FaceMesh || Rig.IsEmpty()) return FString();
	const FString Recipe = FString::Printf(TEXT("offline-mouth-neutral-v2|%s|%s|%s|%f|%d"),
		*FEngineVersion::Current().ToString(), *LexToString(Voice->RawData.GetPayloadId()),
		*Rig, Voice->Duration, Voice->NumChannels);
	return FMD5::HashAnsiString(*Recipe);
}
}

void UStoryFlowMetaHumanBakeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UStoryFlowEditorSubsystem>();
	GEditor->GetEditorSubsystem<UStoryFlowEditorSubsystem>()->OnSyncComplete.AddDynamic(this, &ThisClass::OnSyncComplete);
	FReimportManager::Instance()->OnPostReimport().AddWeakLambda(this, [this](UObject* Asset, bool bSucceeded)
	{
		if (!bSucceeded || !Asset) return;
		const auto* ReimportedFace = Cast<USkeletalMesh>(Asset) ? Cast<USkeletalMesh>(Asset) : Asset->GetTypedOuter<USkeletalMesh>();
		if (!Asset->IsA<USoundWave>() && !Asset->IsA<USkeleton>() && !ReimportedFace) return;
		InvalidateStatusCache();
		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().GetAssetsByClass(UStoryFlowMetaHumanLipsyncLibrary::StaticClass()->GetClassPathName(), Assets);
		for (const auto& Data : Assets)
			if (auto* Library = Cast<UStoryFlowMetaHumanLipsyncLibrary>(Data.GetAsset()))
				if (Library->Entries.ContainsByPredicate([Asset, ReimportedFace](const auto& Entry)
				{ return Entry.Voice == Asset || Entry.FaceMesh == ReimportedFace || (Entry.FaceMesh && Entry.FaceMesh->GetSkeleton() == Asset); }))
					PendingProjects.AddUnique(Library->Project);
	});
	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddWeakLambda(this,
		[this](UObject* Object, FPropertyChangedEvent&)
		{
			if (Object && (Object->IsA<UStoryFlowMetaHumanLipsyncComponent>() || Object->IsA<UStoryFlowMetaHumanLipsyncLibrary>()
				|| Object->IsA<UStoryFlowProjectAsset>() || Object->IsA<USoundWave>() || Object->IsA<USkeletalMeshComponent>()
				|| Object->IsA<USkeletalMesh>() || Object->IsA<USkeleton>() || Object->GetTypedOuter<USkeletalMesh>())) InvalidateStatusCache();
		});
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ThisClass::Tick));
}

void UStoryFlowMetaHumanBakeSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	FReimportManager::Instance()->OnPostReimport().RemoveAll(this);
	StopPreview();
	CancelBake();
	StoryFlowMetaHumanBakeBridge::Unbind(Performance, this, GET_FUNCTION_NAME_CHECKED(ThisClass, ProcessingFinished));
	Performance = nullptr;
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	StatusCache.Empty();
	if (GEditor)
		if (auto* Source = GEditor->GetEditorSubsystem<UStoryFlowEditorSubsystem>()) Source->OnSyncComplete.RemoveAll(this);
	Super::Deinitialize();
}

UStoryFlowProjectAsset* UStoryFlowMetaHumanBakeSubsystem::ResolveProject(UStoryFlowMetaHumanLipsyncComponent* Component) const
{
	if (Component && Component->BakingProject) return Component->BakingProject;
	if (Component && Component->BakeLibrary) return Component->BakeLibrary->Project;
	if (GEditor)
		if (auto* Source = GEditor->GetEditorSubsystem<UStoryFlowEditorSubsystem>())
			if (auto* Project = Source->GetProjectAsset()) return Project;
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().GetAssetsByClass(UStoryFlowProjectAsset::StaticClass()->GetClassPathName(), Assets);
	return Assets.Num() == 1 ? Cast<UStoryFlowProjectAsset>(Assets[0].GetAsset()) : nullptr;
}

TMap<FString, FString> UStoryFlowMetaHumanBakeSubsystem::GetCharacters(UStoryFlowProjectAsset* Project) const
{
	return Project ? Project->CharacterIdToPath : TMap<FString, FString>();
}

TArray<USoundWave*> UStoryFlowMetaHumanBakeSubsystem::DiscoverVoices(UStoryFlowProjectAsset* Project, const FString& CharacterId) const
{
	TArray<USoundWave*> Result;
	if (!Project) return Result;
	bool bDynamicAudio = false;
	for (const auto& ScriptPair : Project->Scripts)
	{
		const auto* Script = ScriptPair.Value;
		if (!Script) continue;
		for (const auto& Pair : Script->Nodes)
		{
			const auto& Node = Pair.Value;
			if (Node.Type != EStoryFlowNodeType::Dialogue || !Matches(Project, CharacterId, Node.Data)) continue;
			bDynamicAudio |= Node.Data.bAudioUseVarInput;
			auto Reference = Script->ResolvedAssets.FindRef(Node.Data.Audio);
			if (Reference.IsNull()) Reference = Project->ResolvedAssets.FindRef(Node.Data.Audio);
			if (auto* Voice = Cast<USoundWave>(Reference.LoadSynchronous())) Result.AddUnique(Voice);
		}
	}
	// Variable-driven audio may select any imported wave. Include these conservatively.
	if (bDynamicAudio)
	{
		auto AddAll = [&Result](const auto& Assets) { for (const auto& Pair : Assets) if (auto* Wave = Cast<USoundWave>(Pair.Value.LoadSynchronous())) Result.AddUnique(Wave); };
		AddAll(Project->ResolvedAssets);
		for (const auto& Pair : Project->Scripts) if (Pair.Value) AddAll(Pair.Value->ResolvedAssets);
	}
	Result.Sort([](const USoundWave& A, const USoundWave& B) { return A.GetPathName() < B.GetPathName(); });
	return Result;
}

static USkeletalMeshComponent* FindFaceComponent(UStoryFlowMetaHumanLipsyncComponent* Component, FString& Issue)
{
	Issue.Empty();
	if (!Component) { Issue = TEXT("Select a MetaHuman lip-sync component."); return nullptr; }
	TArray<const USkeletalMeshComponent*> Meshes;
	if (auto* Owner = Component->GetOwner())
	{
		TInlineComponentArray<USkeletalMeshComponent*> Owned(Owner);
		for (auto* Mesh : Owned) Meshes.Add(Mesh);
	}
	if (Meshes.IsEmpty())
		if (auto* Blueprint = BlueprintFor(Component)) AActor::GetActorClassDefaultComponents(TSubclassOf<AActor>(Blueprint->GeneratedClass.Get()), Meshes);
	const FComponentReference& Reference = Component->FaceRoot;
	const bool bExplicitScope = !Reference.ComponentProperty.IsNone() || !Reference.PathToComponent.IsEmpty()
		|| Reference.OverrideComponent.IsValid() || Reference.OtherActor.IsValid();
	const USceneComponent* Scope = Cast<USceneComponent>(Reference.GetComponent(Component->GetOwner()));
	// Blueprint component templates can be selected by SCS variable/property name even without an actor instance.
	if (!Scope && !Reference.ComponentProperty.IsNone())
	{
		for (auto* Candidate : Meshes)
			if (Candidate->GetFName() == Reference.ComponentProperty
				|| Candidate->GetName() == Reference.ComponentProperty.ToString() + TEXT("_GEN_VARIABLE")) Scope = Candidate;
		if (!Scope)
			if (auto* Blueprint = BlueprintFor(Component))
			{
				TArray<const USceneComponent*> SceneComponents;
				AActor::GetActorClassDefaultComponents(TSubclassOf<AActor>(Blueprint->GeneratedClass.Get()), SceneComponents);
				for (auto* Candidate : SceneComponents)
					if (Candidate->GetFName() == Reference.ComponentProperty
						|| Candidate->GetName() == Reference.ComponentProperty.ToString() + TEXT("_GEN_VARIABLE")) Scope = Candidate;
			}
	}
	if (bExplicitScope && (!Scope || (Component->GetOwner() && Scope->GetOwner() != Component->GetOwner())))
	{
		Issue = TEXT("The selected FaceRoot could not be resolved on this character. Select its facial mesh or parent component.");
		return nullptr;
	}
	TSet<const USceneComponent*> ScopedTemplates;
	if (Scope)
	{
		ScopedTemplates.Add(Scope);
		// SCS templates are not necessarily attached like live components. Follow their construction tree too.
		if (auto* Blueprint = BlueprintFor(Component))
		{
			auto* TargetClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass.Get());
			TArray<USCS_Node*> Nodes;
			for (UClass* Class = TargetClass; Class; Class = Class->GetSuperClass())
				if (auto* OwnerBlueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
					if (OwnerBlueprint->SimpleConstructionScript) Nodes.Append(OwnerBlueprint->SimpleConstructionScript->GetAllNodes());
			bool bAdded = true;
			while (bAdded)
			{
				bAdded = false;
				for (auto* Node : Nodes)
				{
					auto* Template = Cast<USceneComponent>(Node->GetActualComponentTemplate(TargetClass));
					bool bWithin = ScopedTemplates.Contains(Template);
					for (const auto* Parent : ScopedTemplates)
					{
						FString ParentName = Parent->GetName(); ParentName.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
						bWithin |= !Node->ParentComponentOrVariableName.IsNone() && Node->ParentComponentOrVariableName.ToString() == ParentName;
					}
					if (!bWithin) continue;
					TArray<USCS_Node*> Descendants { Node };
					while (!Descendants.IsEmpty())
					{
						auto* Descendant = Descendants.Pop();
						Descendants.Append(Descendant->GetChildNodes());
						if (auto* Child = Cast<USceneComponent>(Descendant->GetActualComponentTemplate(TargetClass)))
							if (!ScopedTemplates.Contains(Child)) { ScopedTemplates.Add(Child); bAdded = true; }
					}
				}
			}
		}
	}
	const USkeletalMeshComponent* Face = nullptr;
	for (auto* Mesh : Meshes)
	{
		if (Scope && !ScopedTemplates.Contains(Mesh) && !Mesh->IsAttachedTo(Scope)) continue;
		if (!Mesh->GetSkeletalMeshAsset() || Mesh->GetBoneIndex(TEXT("FACIAL_C_FacialRoot")) == INDEX_NONE) continue;
		if (Face) { Issue = TEXT("Multiple facial meshes found. Select FaceRoot to identify the face to bake."); return nullptr; }
		Face = Mesh;
	}
	if (!Face) { Issue = TEXT("No assembled MetaHuman face with an enabled facial post process was found."); return nullptr; }
	if (!UStoryFlowMetaHumanLipsyncComponent::CanDriveFace(const_cast<USkeletalMeshComponent*>(Face), Issue)) return nullptr;
	return const_cast<USkeletalMeshComponent*>(Face);
}

USkeletalMesh* UStoryFlowMetaHumanBakeSubsystem::FindFace(UStoryFlowMetaHumanLipsyncComponent* Component, FString& Issue)
{
	const auto* Face = FindFaceComponent(Component, Issue);
	return Face ? Face->GetSkeletalMeshAsset() : nullptr;
}

FString UStoryFlowMetaHumanBakeSubsystem::Fingerprint(USoundWave* Voice, USkeletalMesh* FaceMesh)
{
	return VoiceFingerprint(Voice, FaceMesh, RigFingerprint(FaceMesh));
}

bool UStoryFlowMetaHumanBakeSubsystem::Configure(UStoryFlowMetaHumanLipsyncComponent* Component, UStoryFlowProjectAsset* Project)
{
	if (!GEditor || GEditor->PlayWorld || !Component) return false;
	InvalidateStatusCache();
	FString Issue;
	auto* Mesh = FindFace(Component, Issue);
	if (!Project) Project = ResolveProject(Component);
	if (!Project || !Mesh) { ProgressText = Project ? Issue : TEXT("Select the imported StoryFlow Editor project."); return false; }
	const FString Folder = FPackageName::GetLongPackagePath(Project->GetOutermost()->GetName()) / TEXT("Lipsync");
	const FString Name = TEXT("LS_") + Project->GetName();
	auto* Library = UEditorAssetLibrary::DoesAssetExist(Folder / Name) ? Cast<UStoryFlowMetaHumanLipsyncLibrary>(UEditorAssetLibrary::LoadAsset(Folder / Name)) : nullptr;
	if (!Library) Library = Cast<UStoryFlowMetaHumanLipsyncLibrary>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, UStoryFlowMetaHumanLipsyncLibrary::StaticClass(), nullptr));
	if (!Library) { ProgressText = TEXT("Could not create the shared lip-sync library."); return false; }
	Library->Modify();
	Library->Project = Project;
	Component->Modify();
	Component->BakingProject = Project;
	Component->BakeLibrary = Library;
	if (Component->CharacterId.IsEmpty() && Project->CharacterIdToPath.Num() == 1)
		for (const auto& Pair : Project->CharacterIdToPath) Component->CharacterId = Pair.Key;
	if (!Library->Profiles.ContainsByPredicate([&](const auto& P) { return P.FaceMesh == Mesh && P.CharacterId == Component->CharacterId; }))
	{
		auto& Profile = Library->Profiles.AddDefaulted_GetRef();
		Profile.FaceMesh = Mesh;
		Profile.CharacterId = Component->CharacterId;
	}
	Libraries.AddUnique(Library);
	if (!Save(Library)) { ProgressText = TEXT("The shared lip-sync library could not be saved."); return false; }
	if (!SaveComponent(Component)) { ProgressText = TEXT("Could not save the character Blueprint setup. Make it writable and retry."); return false; }
	return true;
}

void UStoryFlowMetaHumanBakeSubsystem::InvalidateStatusCache()
{
	StatusCache.Empty();
}

FStoryFlowMetaHumanSetupStatus UStoryFlowMetaHumanBakeSubsystem::GetStatus(UStoryFlowMetaHumanLipsyncComponent* Component)
{
	const TWeakObjectPtr<UStoryFlowMetaHumanLipsyncComponent> Key(Component);
	FStoryFlowMetaHumanSetupStatus* Cached = StatusCache.Find(Key);
	if (!Cached)
	{
		FStoryFlowMetaHumanSetupStatus Status;
		FString Issue;
		auto* Mesh = FindFace(Component, Issue);
		auto* Project = ResolveProject(Component);
		if (!Mesh || !Project) Status.Message = Mesh ? TEXT("Choose an imported StoryFlow Editor project.") : Issue;
		else
		{
			const auto Voices = DiscoverVoices(Project, Component->CharacterId);
			const FString Rig = RigFingerprint(Mesh);
			for (auto* Voice : Voices)
			{
				auto* Entry = Component->BakeLibrary ? Component->BakeLibrary->FindEntry(Voice, Mesh) : nullptr;
				if (!Entry || !Entry->Animation) { ++Status.Missing; if (Entry && !Entry->LastError.IsEmpty()) ++Status.Failed; continue; }
				const bool bChanged = Entry->Fingerprint != VoiceFingerprint(Voice, Mesh, Rig);
				if (Entry->bStale != bChanged) { Entry->bStale = bChanged; Component->BakeLibrary->MarkPackageDirty(); }
				if (bChanged) ++Status.Changed; else ++Status.Ready;
				if (!Entry->LastError.IsEmpty()) ++Status.Failed;
			}
			Status.bCanBake = Status.Missing + Status.Changed > 0;
			Status.Message = Voices.IsEmpty() ? TEXT("Face detected. No dialogue voices were found for this character.") :
				FString::Printf(TEXT("Face detected. %d ready | %d missing | %d changed | %d failed"), Status.Ready, Status.Missing, Status.Changed, Status.Failed);
			if (Status.Missing + Status.Changed > 0) Status.Message += TEXT(". Unbaked voices use live fallback.");
		}
		const FString Unavailable = StoryFlowMetaHumanBakeBridge::UnavailableReason();
		if (!Unavailable.IsEmpty()) { Status.bCanBake = false; Status.Message += TEXT(" ") + Unavailable; }
		Cached = &StatusCache.Add(Key, MoveTemp(Status));
	}
	FStoryFlowMetaHumanSetupStatus Status = *Cached;
	Status.bCanBake &= GEditor && !GEditor->PlayWorld && !IsBaking();
	return Status;
}

void UStoryFlowMetaHumanBakeSubsystem::Queue(UStoryFlowMetaHumanLipsyncLibrary* Library, USkeletalMesh* Mesh, const FString& CharacterId)
{
	if (!Library || !Library->Project || !Mesh) return;
	InvalidateStatusCache();
	const FString Rig = RigFingerprint(Mesh);
	for (auto* Voice : DiscoverVoices(Library->Project, CharacterId))
	{
		const FString Hash = VoiceFingerprint(Voice, Mesh, Rig);
		auto* Entry = Library->FindEntry(Voice, Mesh);
		if (Entry && Entry->Animation && Entry->Fingerprint == Hash) { Entry->bStale = false; continue; }
		if (!Entry) { Entry = &Library->Entries.AddDefaulted_GetRef(); Entry->Voice = Voice; Entry->FaceMesh = Mesh; }
		Entry->bStale = true;
		if (Jobs.ContainsByPredicate([&](const auto& J) { return J.Library == Library && J.Voice == Voice && J.FaceMesh == Mesh; })) continue;
		auto& Job = Jobs.AddDefaulted_GetRef();
		Job.Library = Library; Job.Voice = Voice; Job.FaceMesh = Mesh; Job.Fingerprint = Hash;
		++Total;
	}
	Save(Library);
}

bool UStoryFlowMetaHumanBakeSubsystem::Bake(UStoryFlowMetaHumanLipsyncComponent* Component)
{
	const FString Unavailable = StoryFlowMetaHumanBakeBridge::UnavailableReason();
	if (!Unavailable.IsEmpty()) { ProgressText = Unavailable; return false; }
	if (IsBaking() || !Configure(Component, ResolveProject(Component))) return false;
	StopPreview();
	Completed = Total = Failures = 0;
	bCancelRequested = false;
	FString Issue;
	Queue(Component->BakeLibrary, FindFace(Component, Issue), Component->CharacterId);
	ProgressText = Jobs.IsEmpty() ? TEXT("All dialogue voices are up to date.") : FString::Printf(TEXT("Queued %d voices"), Total);
	return true;
}

void UStoryFlowMetaHumanBakeSubsystem::StartNext()
{
	ActiveJob = Jobs[0]; Jobs.RemoveAt(0);
	FString Error;
	Performance = StoryFlowMetaHumanBakeBridge::CreatePerformance(this, ActiveJob.Voice, ActiveJob.FaceMesh,
		this, GET_FUNCTION_NAME_CHECKED(ThisClass, ProcessingFinished), Error);
	bFinished = false;
	ProgressText = FString::Printf(TEXT("Baking %d/%d: %s"), Completed + 1, Total, *GetNameSafe(ActiveJob.Voice));
	if (!Performance || !StoryFlowMetaHumanBakeBridge::Start(Performance, Error))
	{
		if (auto* Entry = ActiveJob.Library->FindEntry(ActiveJob.Voice, ActiveJob.FaceMesh)) Entry->LastError = Error;
		bFinished = true;
		if (!Performance) CompleteActive();
	}
}

void UStoryFlowMetaHumanBakeSubsystem::ProcessingFinished() { bFinished = true; }

void UStoryFlowMetaHumanBakeSubsystem::CompleteActive()
{
	auto* Entry = ActiveJob.Library->FindEntry(ActiveJob.Voice, ActiveJob.FaceMesh);
	const FStoryFlowMetaHumanBakeEntry Previous = *Entry;
	FString Error;
	if (bCancelRequested) Error = TEXT("Cancelled. Click Bake to retry.");
	else if (!Performance || !StoryFlowMetaHumanBakeBridge::ContainsAnimation(Performance))
		Error = Entry->LastError.IsEmpty() ? TEXT("MetaHuman Animator produced no animation. Check the voice and Output Log, then retry.") : Entry->LastError;
	else if (ActiveJob.Fingerprint != Fingerprint(ActiveJob.Voice, ActiveJob.FaceMesh)) Error = TEXT("The voice changed during baking. Click Bake to retry.");
	else
	{
		const FString PackagePath = FPackageName::GetLongPackagePath(ActiveJob.Library->GetOutermost()->GetName()) / TEXT("Generated");
		FString UniquePackage, UniqueName;
		FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(PackagePath / (TEXT("LS_") + ActiveJob.Voice->GetName() + TEXT("_") + ActiveJob.Fingerprint.Left(10)), TEXT(""), UniquePackage, UniqueName);
		auto* Animation = StoryFlowMetaHumanBakeBridge::Export(Performance, ActiveJob.FaceMesh, PackagePath, UniqueName, Error);
		if (Animation && Save(Animation))
		{
			Entry->Animation = Animation;
			Entry->Fingerprint = ActiveJob.Fingerprint;
			Entry->bStale = false;
		}
		else if (Error.IsEmpty()) Error = TEXT("Could not export/save the animation. Previous output was retained.");
	}
	Entry->LastError = Error;
	if (!Save(ActiveJob.Library))
	{
		*Entry = Previous;
		Entry->bStale = true;
		Error = TEXT("Could not save the lip-sync library. Make it writable and click Bake to retry.");
		Entry->LastError = Error;
	}
	if (!Error.IsEmpty()) ++Failures;
	StoryFlowMetaHumanBakeBridge::Unbind(Performance, this, GET_FUNCTION_NAME_CHECKED(ThisClass, ProcessingFinished));
	InvalidateStatusCache();
	Performance = nullptr;
	ActiveJob = {};
	++Completed;
	bFinished = false;
	if (Jobs.IsEmpty()) ProgressText = bCancelRequested ? TEXT("Cancelled. Completed bakes were kept.") :
		Failures == 0 ? FString::Printf(TEXT("Finished %d voices. Ready to play."), Completed) :
		FString::Printf(TEXT("Finished with %d failed voice(s). Check status and Output Log, then click Bake to retry."), Failures);
}

void UStoryFlowMetaHumanBakeSubsystem::CancelBake()
{
	bCancelRequested = true;
	Jobs.Empty();
	if (Performance) { StoryFlowMetaHumanBakeBridge::Cancel(Performance); bFinished = true; }
	else ProgressText = TEXT("Cancelled. Completed bakes were kept.");
}

void UStoryFlowMetaHumanBakeSubsystem::OnSyncComplete(UStoryFlowProjectAsset* Project) { InvalidateStatusCache(); PendingProjects.AddUnique(Project); }

void UStoryFlowMetaHumanBakeSubsystem::RefreshProject(UStoryFlowProjectAsset* Project)
{
	if (!GEditor || !Project) return;
	InvalidateStatusCache();
	if (IsBaking() || GEditor->PlayWorld) { PendingProjects.AddUnique(Project); return; }
	Completed = Total = Failures = 0; bCancelRequested = false;
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().GetAssetsByClass(UStoryFlowMetaHumanLipsyncLibrary::StaticClass()->GetClassPathName(), Assets);
	for (auto& Asset : Assets) if (auto* Library = Cast<UStoryFlowMetaHumanLipsyncLibrary>(Asset.GetAsset())) Libraries.AddUnique(Library);
	TMap<USkeletalMesh*, FString> RigHashes;
	for (const auto& Library : Libraries)
	{
		if (!Library || Library->Project != Project) continue;
		for (auto& Entry : Library->Entries)
		{
			FString* Rig = RigHashes.Find(Entry.FaceMesh);
			if (!Rig) Rig = &RigHashes.Add(Entry.FaceMesh, RigFingerprint(Entry.FaceMesh));
			Entry.bStale = Entry.Fingerprint != VoiceFingerprint(Entry.Voice, Entry.FaceMesh, *Rig);
		}
		Save(Library);
		if (Library->bAutoBakeAfterSync && StoryFlowMetaHumanBakeBridge::UnavailableReason().IsEmpty())
		{
			for (auto& Profile : Library->Profiles) Queue(Library, Profile.FaceMesh, Profile.CharacterId);
		}
	}
}

void UStoryFlowMetaHumanBakeSubsystem::SetAutoBake(UStoryFlowMetaHumanLipsyncComponent* Component, bool bEnabled)
{
	if (Configure(Component, ResolveProject(Component))) { Component->BakeLibrary->bAutoBakeAfterSync = bEnabled; Save(Component->BakeLibrary); }
}

bool UStoryFlowMetaHumanBakeSubsystem::SelectCharacter(UStoryFlowMetaHumanLipsyncComponent* Component, const FString& CharacterId)
{
	auto* Project = ResolveProject(Component);
	if (!Project || !Component || (!CharacterId.IsEmpty() && !Project->CharacterIdToPath.Contains(CharacterId))) return false;
	Component->Modify(); Component->CharacterId = CharacterId;
	// Register the new speaker even before it has voices. A subsequent sync can
	// introduce that speaker's first line and must honor the auto-bake setting.
	return Configure(Component, Project);
}

bool UStoryFlowMetaHumanBakeSubsystem::Tick(float DeltaSeconds)
{
	if (!GEditor) return true;
	if (Performance && bFinished && !StoryFlowMetaHumanBakeBridge::IsProcessing(Performance)) CompleteActive();
	if (!GEditor->PlayWorld && !Performance && !Jobs.IsEmpty()) StartNext();
	if (!GEditor->PlayWorld && !IsBaking() && !PendingProjects.IsEmpty()) { auto* Project = PendingProjects[0].Get(); PendingProjects.RemoveAt(0); RefreshProject(Project); }
	if (PreviewAudio)
	{
		if (GEditor->PlayWorld || !PreviewAudio->IsPlaying()) StopPreview();
		else if (PreviewFace.IsValid())
		{
			if (auto* Anim = Cast<UStoryFlowMetaHumanAnimInstance>(PreviewFace->GetAnimInstance())) { Anim->BakedAnimation = PreviewAnimation; Anim->BakedTime = PreviewTime; }
			PreviewFace->TickAnimation(DeltaSeconds, false);
			PreviewFace->RefreshBoneTransforms();
			PreviewFace->MarkRenderDynamicDataDirty();
		}
	}
	return true;
}

bool UStoryFlowMetaHumanBakeSubsystem::Preview(UStoryFlowMetaHumanLipsyncComponent* Component, USoundWave* Voice)
{
	if (!GEditor || !Component || GEditor->PlayWorld || IsBaking()) return false;
	FString Issue;
	const auto* SelectedFace = FindFaceComponent(Component, Issue);
	auto* Mesh = SelectedFace ? SelectedFace->GetSkeletalMeshAsset() : nullptr;
	auto* Library = Component->BakeLibrary.Get();
	auto* Animation = Library && Mesh ? Library->FindAnimation(Voice, Mesh) : nullptr;
	auto* Blueprint = BlueprintFor(Component);
	if (!Animation || !Blueprint) { ProgressText = TEXT("Bake this voice before previewing it."); return false; }
	StopPreview();
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World || !Blueprint->GeneratedClass) return false;
	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.bTemporaryEditorActor = true;
	Params.bHideFromSceneOutliner = true;
	PreviewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, FTransform(FVector(180, 0, 0)), Params);
	if (!PreviewActor) return false;
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(PreviewActor);
	FString SelectedName = SelectedFace->GetName(); SelectedName.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
	for (auto* Part : Meshes)
	{
		FString PartName = Part->GetName(); PartName.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
		if (Part->GetSkeletalMeshAsset() == Mesh && PartName == SelectedName) PreviewFace = Part;
	}
	if (!PreviewFace.IsValid()) { StopPreview(); return false; }
	PreviewFace->SetAnimInstanceClass(UStoryFlowMetaHumanAnimInstance::StaticClass());
	PreviewFace->SetUpdateAnimationInEditor(true);
	PreviewFace->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	PreviewAnimation = Animation; PreviewTime = 0.f;
	PreviewAudio = NewObject<UAudioComponent>(PreviewActor);
	PreviewAudio->bAutoDestroy = false;
	PreviewAudio->bIsUISound = true;
	PreviewAudio->bAllowSpatialization = false;
	PreviewAudio->SetSound(Voice);
	PreviewAudio->RegisterComponentWithWorld(World);
	PreviewPercentHandle = PreviewAudio->OnAudioPlaybackPercentNative.AddUObject(this, &ThisClass::PreviewPercent);
	PreviewAudio->Play();
	GEditor->MoveViewportCamerasToActor(*PreviewActor, false);
	return true;
}

void UStoryFlowMetaHumanBakeSubsystem::PreviewPercent(const UAudioComponent* Audio, const USoundWave* Voice, float Percent)
{
	if (Audio == PreviewAudio && Voice && PreviewAnimation)
		PreviewTime = FMath::Clamp(Percent * Voice->Duration, 0.f, PreviewAnimation->GetPlayLength());
}

void UStoryFlowMetaHumanBakeSubsystem::StopPreview()
{
	if (PreviewAudio) { PreviewAudio->OnAudioPlaybackPercentNative.Remove(PreviewPercentHandle); PreviewAudio->Stop(); PreviewAudio->DestroyComponent(); }
	PreviewAudio = nullptr; PreviewAnimation = nullptr; PreviewFace.Reset();
	if (PreviewActor && PreviewActor->GetWorld()) PreviewActor->Destroy();
	PreviewActor = nullptr;
}
