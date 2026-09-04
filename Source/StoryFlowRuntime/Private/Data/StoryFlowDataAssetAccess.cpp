// Copyright 2026 StoryFlow. All Rights Reserved.

#include "Data/StoryFlowDataAssetAccess.h"
#include "Subsystems/StoryFlowSubsystem.h"
#include "Data/StoryFlowDataAssetAsset.h"
#include "Data/StoryFlowProjectAsset.h"
#include "StoryFlowRuntime.h"

namespace
{
	/**
	 * Can a declaration of `DeclaredType` be reached through the `ExpectedType` accessor?
	 *
	 * Exact, except that the STRING accessor also serves image / character / audio: those three
	 * are declared distinctly in the editor but hold a plain string at runtime (the importer
	 * stores all three through FStoryFlowVariant::SetString), so reading one as a string is not a
	 * coercion — it is the value. Enum is NOT in that set: it carries its own type tag, and
	 * folding it in here would make a Blueprint write land in the overlay typed String while the
	 * file value it shadows is typed Enum — invisible to a read, visible in the save key.
	 *
	 * NOT the same set as StoryFlowEngineContract::IsStringFamily (the test fixtures' helper),
	 * which has FIVE members because it answers a different question: it asks what STORAGE a
	 * value ended up in, where Enum does live in StringValue alongside the other four. This one
	 * asks which DECLARATION an accessor may reach, and Enum has its own accessor. The two must
	 * not be merged: doing it would either open enum writes to the string setter (above) or shut
	 * the fixture comparator out of every enum value it checks.
	 */
	/**
	 * The KEY twin of DataAssetAccessorTypeMatches, and deliberately not the same rule: the importer
	 * types a map's keys as Int or String and nothing else (StoryFlowImporter's map parse - there is
	 * no SetEnum for a key), so an ENUM-keyed map holds String-typed keys. The accessor rule keeps
	 * Enum out of the string family on purpose - an enum VALUE carries its own tag - which is
	 * exactly why it cannot be reused for keys: it would refuse every enum-keyed map write a
	 * Blueprint could make, and the first cut of SetDataAssetMapVariable did.
	 */
	bool DataAssetKeyTypeMatches(EStoryFlowVariableType DeclaredKeyType, EStoryFlowVariableType OfferedType)
	{
		if (DeclaredKeyType == EStoryFlowVariableType::String || DeclaredKeyType == EStoryFlowVariableType::Enum)
		{
			return OfferedType == EStoryFlowVariableType::String;
		}
		return DeclaredKeyType == OfferedType;
	}

	bool DataAssetAccessorTypeMatches(EStoryFlowVariableType DeclaredType, EStoryFlowVariableType ExpectedType)
	{
		if (ExpectedType == EStoryFlowVariableType::String)
		{
			return DeclaredType == EStoryFlowVariableType::String
				|| DeclaredType == EStoryFlowVariableType::Image
				|| DeclaredType == EStoryFlowVariableType::Character
				|| DeclaredType == EStoryFlowVariableType::Audio;
		}
		return DeclaredType == ExpectedType;
	}

	/** A character's display name in the caller's language - the project-level tier both surfaces share. */
	FString ResolveName(const UStoryFlowSubsystem& Subsystem, const FString& Key, const FString& LanguageCode)
	{
		if (const UStoryFlowProjectAsset* Project = Subsystem.GetProject())
		{
			return Project->GetGlobalString(Key, LanguageCode);
		}
		return Key;
	}

	// ---- the character branch (contract section 3), moved verbatim from the component ----

	FStoryFlowCharacterDef* FindBridgedCharacter(UStoryFlowSubsystem& Subsystem, const FString& AssetId)
	{
		const FString* RecordKey = Subsystem.GetCharacterIdToPath().Find(AssetId);
		if (!RecordKey)
		{
			return nullptr;
		}
		// Null when the record is not loaded (post-LoadFromSlot: the bridge is project-derived,
		// the runtime characters hold only what the save carried) - the branch then misses whole.
		return Subsystem.GetRuntimeCharacters().Find(*RecordKey);
	}

	bool TryGetCharacterScalarByName(const UStoryFlowSubsystem& Subsystem, const FStoryFlowCharacterDef& CharDef, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, const FString& LanguageCode, FStoryFlowVariant& OutValue)
	{
		// The builtin rows are plain strings (cf_ aliases per amendment A1) and the surface never
		// coerces, so they answer only the STRING accessor - same rule as the seed path's gate.
		const bool bIsName = IsCharacterNameBuiltin(VariableName);
		const bool bIsImage = IsCharacterImageBuiltin(VariableName);
		if (bIsName || bIsImage)
		{
			if (ExpectedType != EStoryFlowVariableType::String)
			{
				return false;
			}
			OutValue.SetString(bIsName ? ResolveName(Subsystem, CharDef.Name, LanguageCode) : CharDef.Image);
			return true;
		}

		const FStoryFlowVariable* Variable = CharDef.Variables.Find(VariableName);
		if (!Variable || Variable->bIsArray || !DataAssetAccessorTypeMatches(Variable->Type, ExpectedType))
		{
			return false;
		}
		OutValue = Variable->Value;
		return true;
	}

	bool TrySetCharacterScalarByName(FStoryFlowCharacterDef& CharDef, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, const FStoryFlowVariant& Value)
	{
		const bool bIsName = IsCharacterNameBuiltin(VariableName);
		const bool bIsImage = IsCharacterImageBuiltin(VariableName);
		if (bIsName || bIsImage)
		{
			if (ExpectedType != EStoryFlowVariableType::String)
			{
				return false;
			}
			if (bIsName)
			{
				CharDef.Name = Value.ToString();
			}
			else
			{
				CharDef.Image = Value.ToString();
			}
			return true;
		}

		FStoryFlowVariable* Variable = CharDef.Variables.Find(VariableName);
		if (!Variable || Variable->bIsArray || !DataAssetAccessorTypeMatches(Variable->Type, ExpectedType))
		{
			return false;
		}
		Variable->Value = Value;
		return true;
	}

	bool TryGetCharacterVariantByName(const UStoryFlowSubsystem& Subsystem, const FStoryFlowCharacterDef& CharDef, const FString& VariableName,
		const FString& LanguageCode, FStoryFlowVariant& OutValue)
	{
		if (IsCharacterNameBuiltin(VariableName))
		{
			OutValue.SetString(ResolveName(Subsystem, CharDef.Name, LanguageCode));
			return true;
		}
		if (IsCharacterImageBuiltin(VariableName))
		{
			OutValue.SetString(CharDef.Image);
			return true;
		}

		const FStoryFlowVariable* Variable = CharDef.Variables.Find(VariableName);
		if (!Variable)
		{
			return false;
		}
		if (Variable->Value.IsMap())
		{
			// DETACHED storage: SetMap copies the entries into fresh shared storage, so what the
			// caller holds can be mutated without reaching into the live character - the same
			// promise the seed path's TryResolve makes.
			OutValue.SetMap(Variable->Value.GetMap());
		}
		else
		{
			OutValue = Variable->Value;
		}
		return true;
	}

	// ---- the seed path, moved verbatim from the component ----

	const FStoryFlowVariable* FindDeclaration(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName, StoryFlowDataAssets::FStoreRef& OutStore)
	{
		OutStore = StoryFlowDataAssets::FStoreRef();

		if (!DataAsset)
		{
			// Verbose, not Warning: these accessors report failure through their return value, and a
			// caller may well invoke one every tick (contract section 6's warn-once ladder is keyed
			// on a NODE id, which a host call does not have).
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset accessor called with no asset"));
			return nullptr;
		}

		OutStore = Subsystem.GetDataAssetStore();
		if (!OutStore.IsValid())
		{
			return nullptr;
		}

		// Names are resolved to ids exactly HERE, at the host boundary - everything downstream
		// (the resolver, the overlay, the save key) is keyed by id, as the contract keys the system.
		const FStoryFlowVariable* Declaration = StoryFlowDataAssets::FindDeclarationByName(*OutStore.Seed, DataAsset->AssetId, VariableName);
		if (!Declaration)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset '%s' declares no variable named '%s' on its chain"), *DataAsset->AssetId, *VariableName);
		}
		return Declaration;
	}

	const FStoryFlowVariable* FindScalarDeclaration(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, StoryFlowDataAssets::FStoreRef& OutStore)
	{
		const FStoryFlowVariable* Declaration = FindDeclaration(Subsystem, DataAsset, VariableName, OutStore);
		if (!Declaration)
		{
			return nullptr;
		}

		// NO SILENT COERCION (contract section 6.1's rule, applied at this surface): an array read
		// through a scalar accessor, or an integer read as a float, reports not-found rather than
		// converting. Arrays and maps travel through the variant getter and the variant library.
		if (Declaration->bIsArray || !DataAssetAccessorTypeMatches(Declaration->Type, ExpectedType))
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: Data Asset variable '%s' is declared as a different type than the accessor reading it"), *VariableName);
			return nullptr;
		}
		return Declaration;
	}

	bool WriteContainer(UStoryFlowDataAssetAsset* DataAsset, const FStoryFlowVariable* Declaration,
		StoryFlowDataAssets::FStoreRef& Store, const FStoryFlowVariant& Value)
	{
		// The write lands at the referenced asset's OWN level (contract section 5). The cache drop
		// that used to sit beside this belongs to the caller now - see the header.
		return StoryFlowDataAssets::TrySet(*Store.Seed, *Store.Overlay, DataAsset->AssetId, Declaration->Id, Value);
	}
}

namespace StoryFlowDataAssetAccess
{
	bool TryGetScalar(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, const FString& LanguageCode, FStoryFlowVariant& OutValue)
	{
		// Contract section 3's character branch - see SetScalar. A read routes to the same
		// character state a char-var node reads, so a value written on either surface is visible
		// on the other by construction (one state).
		if (const FStoryFlowCharacterDef* BridgedCharacter = DataAsset ? FindBridgedCharacter(Subsystem, DataAsset->AssetId) : nullptr)
		{
			return TryGetCharacterScalarByName(Subsystem, *BridgedCharacter, VariableName, ExpectedType, LanguageCode, OutValue);
		}

		StoryFlowDataAssets::FStoreRef Store;
		const FStoryFlowVariable* Declaration = FindScalarDeclaration(Subsystem, DataAsset, VariableName, ExpectedType, Store);
		if (!Declaration)
		{
			return false;
		}

		// Through the RESOLVER, never a cached copy: chain defaults, ancestor overrides and this
		// session's writes all have to be visible here (contract section 4). TryRead is the
		// resolver plus the localization gate - the SAME door the script lane's accessor arms use,
		// so a declared string value reads translated on both surfaces and a written one stays
		// verbatim on both (localization spec section 2's amendment; every rule lives on TryRead).
		return StoryFlowDataAssets::TryRead(Store, Subsystem.GetProject(), LanguageCode, DataAsset->AssetId, Declaration->Id, OutValue);
	}

	bool SetScalar(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		EStoryFlowVariableType ExpectedType, const FStoryFlowVariant& Value)
	{
		// Contract section 3's character branch (host surfaces only): an id the seed cannot know
		// may be a character FILE id - a bridge hit routes the write to the character system's
		// state by NAME, with the surface's own bFound posture. The write never touches the .sfd
		// overlay: the character system is the one runtime-state owner.
		if (FStoryFlowCharacterDef* BridgedCharacter = DataAsset ? FindBridgedCharacter(Subsystem, DataAsset->AssetId) : nullptr)
		{
			return TrySetCharacterScalarByName(*BridgedCharacter, VariableName, ExpectedType, Value);
		}

		StoryFlowDataAssets::FStoreRef Store;
		const FStoryFlowVariable* Declaration = FindScalarDeclaration(Subsystem, DataAsset, VariableName, ExpectedType, Store);
		if (!Declaration)
		{
			return false;
		}

		// The write lands at THE REFERENCED ASSET'S OWN LEVEL, always (contract section 5) - the
		// same store call the Set node makes, so a host write cascades to descendants exactly as
		// a scripted one does and rides the next save the same way.
		return StoryFlowDataAssets::TrySet(*Store.Seed, *Store.Overlay, DataAsset->AssetId, Declaration->Id, Value);
	}

	bool TryGetVariant(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const FString& LanguageCode, FStoryFlowVariant& OutValue)
	{
		// Contract section 3's character branch - untyped like the seed path below, so this is
		// also the ARRAY and MAP route to a character's variables.
		if (const FStoryFlowCharacterDef* BridgedCharacter = DataAsset ? FindBridgedCharacter(Subsystem, DataAsset->AssetId) : nullptr)
		{
			return TryGetCharacterVariantByName(Subsystem, *BridgedCharacter, VariableName, LanguageCode, OutValue);
		}

		StoryFlowDataAssets::FStoreRef Store;
		// No type gate here: this IS the untyped accessor, and the caller picks the value apart
		// with UStoryFlowVariantLibrary. Arrays and maps have no other host path.
		const FStoryFlowVariable* Declaration = FindDeclaration(Subsystem, DataAsset, VariableName, Store);
		if (!Declaration)
		{
			return false;
		}

		// The SAME door the typed accessors use - this is the array and map route, and an array's
		// elements and a string map's values localize exactly as a scalar does (see TryRead).
		// TryRead copies out with map storage detached, so what a caller gets can be held or
		// mutated without reaching into the store (contract section 3).
		return StoryFlowDataAssets::TryRead(Store, Subsystem.GetProject(), LanguageCode, DataAsset->AssetId, Declaration->Id, OutValue);
	}

	TArray<FString> VariableNames(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset)
	{
		if (!DataAsset)
		{
			// Verbose for the same reason FindDeclaration is: a host may call this every tick and
			// it reports emptiness through its return value.
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: GetDataAssetVariableNames called with no asset"));
			return TArray<FString>();
		}
		// Straight to the shared walk - no ladder, no warn-once latch. The graph node reaches the
		// same function through the execution context, so the surfaces cannot answer differently.
		return StoryFlowDataAssets::VariableNames(Subsystem.GetDataAssetSeed(), DataAsset->AssetId);
	}

	bool SetArray(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const TArray<FStoryFlowVariant>& Elements)
	{
		StoryFlowDataAssets::FStoreRef Store;
		const FStoryFlowVariable* Declaration = FindDeclaration(Subsystem, DataAsset, VariableName, Store);
		if (!Declaration)
		{
			return false;
		}

		// THE SHAPE GATE, the whole reason this is a container setter and not a variant one. A map
		// is refused here rather than falling through: its entries are a different shape, and
		// writing an array over one leaves a value nothing can read.
		if (!Declaration->bIsArray || Declaration->Type == EStoryFlowVariableType::Map)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: '%s.%s' is not an array"), *DataAsset->AssetId, *VariableName);
			return false;
		}

		// EVERY element, checked BEFORE anything is written: a partial list is a shape no author
		// declared, so a single mismatch refuses the whole write.
		for (const FStoryFlowVariant& Element : Elements)
		{
			if (!DataAssetAccessorTypeMatches(Declaration->Type, Element.GetType()))
			{
				UE_LOG(LogStoryFlow, Verbose,
					TEXT("StoryFlow: an element offered to '%s.%s' does not match its declared type"), *DataAsset->AssetId, *VariableName);
				return false;
			}
		}

		FStoryFlowVariant Value;
		// The typed overload, so an EMPTY write still lands as an array of the declared type rather
		// than as a type-less variant - SetArray's one-argument form infers from the first element
		// and has nothing to infer from here.
		Value.SetArray(Elements, Declaration->Type);
		return WriteContainer(DataAsset, Declaration, Store, Value);
	}

	bool SetMap(UStoryFlowSubsystem& Subsystem, UStoryFlowDataAssetAsset* DataAsset, const FString& VariableName,
		const TArray<FStoryFlowVariant>& Keys, const TArray<FStoryFlowVariant>& Values)
	{
		StoryFlowDataAssets::FStoreRef Store;
		const FStoryFlowVariable* Declaration = FindDeclaration(Subsystem, DataAsset, VariableName, Store);
		if (!Declaration)
		{
			return false;
		}

		if (Declaration->Type != EStoryFlowVariableType::Map)
		{
			UE_LOG(LogStoryFlow, Verbose, TEXT("StoryFlow: '%s.%s' is not a map"), *DataAsset->AssetId, *VariableName);
			return false;
		}

		// Refused rather than truncated to the shorter: truncating silently drops entries the
		// caller listed, and a caller that mismatched these has a bug worth being told about.
		if (Keys.Num() != Values.Num())
		{
			UE_LOG(LogStoryFlow, Verbose,
				TEXT("StoryFlow: '%s.%s' was offered %d keys and %d values"), *DataAsset->AssetId, *VariableName, Keys.Num(), Values.Num());
			return false;
		}

		const EStoryFlowVariableType DeclaredKeyType = Declaration->KeyType;
		const EStoryFlowVariableType DeclaredValueType = Declaration->ValueType;
		TArray<FStoryFlowMapEntry> Entries;
		Entries.Reserve(Keys.Num());
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
		{
			if (!DataAssetKeyTypeMatches(DeclaredKeyType, Keys[Index].GetType())
				|| !DataAssetAccessorTypeMatches(DeclaredValueType, Values[Index].GetType()))
			{
				UE_LOG(LogStoryFlow, Verbose,
					TEXT("StoryFlow: an entry offered to '%s.%s' does not match its declared key/value types"), *DataAsset->AssetId, *VariableName);
				return false;
			}
			Entries.Add(FStoryFlowMapEntry{ Keys[Index], Values[Index] });
		}

		FStoryFlowVariant Value;
		Value.SetMap(Entries);
		return WriteContainer(DataAsset, Declaration, Store, Value);
	}
}
