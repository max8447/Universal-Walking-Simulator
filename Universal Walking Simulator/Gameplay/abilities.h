#pragma once

#include <UE/structs.h>
#include <Gameplay/helper.h>
#include "Net/funcs.h"
#include "../anticheat.h"

__int64* GetActivatableAbilities(UObject* ASC)
{
    static auto ActivatableAbilitiesOffset = GetOffset(ASC, "ActivatableAbilities");

    return (__int64*)(__int64(ASC) + ActivatableAbilitiesOffset);
}

int16_t* GetCurrent(FPredictionKey* Key)
{
    if (!Key)
        return nullptr;

    static auto CurrentOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.PredictionKey", "Current");
    return (int16_t*)(__int64(Key) + CurrentOffset);
}

UObject** GetAbilityFromSpec(void* Spec)
{
    if (!Spec)
        return nullptr;

    static auto AbilityOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", "Ability");

    return (UObject**)(__int64(Spec) + AbilityOffset);
}

int GetHandleFromHandle(FGameplayAbilitySpecHandle& handle)
{
    return handle.Handle;
}

// template return and base the func and actual ret of fof that maybe?
void LoopSpecs(UObject* ASC, std::function<void(__int64*)> func)
{
    auto ActivatableAbilities = GetActivatableAbilities(ASC);

    static auto ItemsOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpecContainer", "Items");
    auto Items = (TArray<__int64>*)(__int64(ActivatableAbilities) + ItemsOffset);

    static auto SpecStruct = FindObjectOld("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", true);
    static auto SpecSize = GetSizeOfStruct(SpecStruct);

    if (ActivatableAbilities && Items)
    {
        for (int i = 0; i < Items->Num(); i++)
        {
            auto CurrentSpec = (__int64*)(__int64(Items->GetData()) + (static_cast<long long>(SpecSize) * i));
            func(CurrentSpec);
        }
    }
}

__int64* FindAbilitySpecFromHandle2(UObject* ASC, FGameplayAbilitySpecHandle Handle)
{
    __int64* SpecToReturn = nullptr;

    auto compareHandles = [&Handle, &SpecToReturn](__int64* Spec) {
        static auto HandleOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", "Handle");

        auto CurrentHandle = (FGameplayAbilitySpecHandle*)(__int64(Spec) + HandleOffset);

        if (GetHandleFromHandle(*CurrentHandle) == Handle.Handle)
        {
            SpecToReturn = Spec;
            return;
        }
    };

    LoopSpecs(ASC, compareHandles);

    return SpecToReturn;
}

void InternalServerTryActivateAbility(UObject* ASC, FGameplayAbilitySpecHandle Handle, bool InputPressed, FPredictionKey* PredictionKey, __int64* TriggerEventData, bool bConsumeData = false)
{
    if (!PredictionKey)
    {
        std::cout << ("InternalServerTryActivateAbility. Rejecting ClientActivation of ability with invalid PredictionKey!\n"); // me but this will probably never happen
        return;
    }

    void* Spec = FindAbilitySpecFromHandle2(ASC, Handle);

    if (!Spec)
    {
        // Can potentially happen in race conditions where client tries to activate ability that is removed server side before it is received.
        std::cout << ("InternalServerTryActivateAbility. Rejecting ClientActivation of ability with invalid SpecHandle!\n");
        Helper::Abilities::ClientActivateAbilityFailed(ASC, Handle, *GetCurrent(PredictionKey));
        return;
    }

    const UObject* AbilityToActivate = *GetAbilityFromSpec(Spec);

    if (!AbilityToActivate) //!ensure(AbilityToActivate))
    {
        std::cout << ("InternalServerTryActiveAbility. Rejecting ClientActivation of unconfigured spec ability!\n");
        Helper::Abilities::ClientActivateAbilityFailed(ASC, Handle, *GetCurrent(PredictionKey));
        return;
    }

    // Consume any pending target info, to clear out cancels from old executions
    // ConsumeAllReplicatedData(ASC, Handle, PredictionKey); // Try this?

    UObject* InstancedAbility = nullptr;

    if (Engine_Version < 426)
        ((FGameplayAbilitySpec<FGameplayAbilityActivationInfo, 0x50>*)Spec)->InputPressed = true;
    else if (Engine_Version == 426)
        ((FGameplayAbilitySpec<FGameplayAbilityActivationInfoFTS, 0x50>*)Spec)->InputPressed = true;
    else
        ((FGameplayAbilitySpecNewer*)Spec)->InputPressed = true;

    // Attempt to activate the ability (server side) and tell the client if it succeeded or failed.

    bool res = false;

    if (Engine_Version < 426)
        res = InternalTryActivateAbility(ASC, Handle, *PredictionKey, &InstancedAbility, nullptr, TriggerEventData);
    else if (FnVerDouble < 19.00)
        res = InternalTryActivateAbilityFTS(ASC, Handle, *(FPredictionKeyFTS*)PredictionKey, &InstancedAbility, nullptr, TriggerEventData);
    else
        res = InternalTryActivateAbilityNewer(ASC, Handle, *(FPredictionKeyNewer*)PredictionKey, &InstancedAbility, nullptr, TriggerEventData);

    if (!res)
    {
        // auto InternalTryActivateAbilityFailureTags = ASC->Member<FGameplayTagContainer>(("ClientDebugStrings"), sizeof(FGameplayTagContainer));
        // std::cout << std::format("InternalServerTryActivateAbility. Rejecting ClientActivation of {}. InternalTryActivateAbility failed: {}\n", (*GetAbilityFromSpec(Spec))->GetName(), InternalTryActivateAbilityFailureTags->ToStringSimple(true));
        std::cout << std::format("InternalServerTryActivateAbility. Rejecting ClientActivation of {}. InternalTryActivateAbility failed\n", (*GetAbilityFromSpec(Spec))->GetName());
        Helper::Abilities::ClientActivateAbilityFailed(ASC, Handle, *GetCurrent(PredictionKey));

        if (Engine_Version < 426)
            ((FGameplayAbilitySpec<FGameplayAbilityActivationInfo, 0x50>*)Spec)->InputPressed = false;
        else if (Engine_Version == 426)
            ((FGameplayAbilitySpec<FGameplayAbilityActivationInfoFTS, 0x50>*)Spec)->InputPressed = false;
        else
            ((FGameplayAbilitySpecNewer*)Spec)->InputPressed = false;

        MarkItemDirty(GetActivatableAbilities(ASC), (FFastArraySerializerItem*)Spec); // TODO: Start using the proper function again
    }
}

UObject* DoesASCHaveAbility(UObject* ASC, UObject* Ability)
{
    if (!ASC || !Ability)
        return nullptr;

    UObject* AbilityToReturn = nullptr;

    auto compareAbilities = [&AbilityToReturn, &Ability](__int64* Spec) {
        auto CurrentAbility = GetAbilityFromSpec(Spec);

        if (*CurrentAbility == Ability)
        {
            AbilityToReturn = *CurrentAbility;
            return;
        }
    };

    LoopSpecs(ASC, compareAbilities);

    return AbilityToReturn;
}

static inline UObject* GrantGameplayAbility(UObject* TargetPawn, UObject* GameplayAbilityClass, void** OutSpec = nullptr) // CREDITS: kem0x, raider3.5
{
    if (!GameplayAbilityClass || !TargetPawn)
        return nullptr;

    static auto AbilitySystemComponentOffset = GetOffset(TargetPawn, "AbilitySystemComponent");
    auto AbilitySystemComponent = *(UObject**)(__int64(TargetPawn) + AbilitySystemComponentOffset);

    if (!AbilitySystemComponent)
        return nullptr;

    UObject* DefaultObject = nullptr;

    if (!GameplayAbilityClass->GetFullName().contains("Class "))
        DefaultObject = GameplayAbilityClass; //->CreateDefaultObject(); // Easy::SpawnObject(GameplayAbilityClass, GameplayAbilityClass->OuterPrivate);
    else
    {
        // im dumb
        static std::unordered_map<std::string, UObject*> defaultAbilities; // normal class name, default ability.

        auto name = GameplayAbilityClass->GetFullName();

        auto defaultafqaf = defaultAbilities.find(name);

        if (defaultafqaf != defaultAbilities.end())
        {
            DefaultObject = defaultafqaf->second;
        }
        else
        {
            // skunked class to default
            auto ending = name.substr(name.find_last_of(".") + 1);
            auto path = name.substr(0, name.find_last_of(".") + 1);

            path = path.substr(path.find_first_of(" ") + 1);

            auto DefaultAbilityName = std::format("{1} {0}Default__{1}", path, ending);

            // std::cout << "DefaultAbilityName: " << DefaultAbilityName << '\n';

            DefaultObject = FindObject(DefaultAbilityName);
            defaultAbilities.emplace(name, DefaultObject);
        }
    }

    if (!DefaultObject)
    {
        std::cout << "Failed to create defaultobject for GameplayAbilityClass: " << GameplayAbilityClass->GetFullName() << '\n';
        return nullptr;
    }

    static auto HandleOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", "Handle");

    auto GenerateNewSpec = [&]() -> void*
    {
        static auto GameplayAbilitySpecStruct = FindObjectOld("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", true);
        static auto GameplayAbilitySpecSize = GetSizeOfStruct(GameplayAbilitySpecStruct);

        if (Engine_Version < 426)
            std::cout << "Size of GameplayAbilitySpec: " << GameplayAbilitySpecSize << '\n';

        auto ptr = malloc(GameplayAbilitySpecSize);

        if (!ptr)
            return nullptr;

        RtlSecureZeroMemory(ptr, GameplayAbilitySpecSize);

        FGameplayAbilitySpecHandle Handle{};
        Handle.GenerateNewHandle();

        ((FFastArraySerializerItem*)ptr)->MostRecentArrayReplicationKey = -1;
        ((FFastArraySerializerItem*)ptr)->ReplicationID = -1;
        ((FFastArraySerializerItem*)ptr)->ReplicationKey = -1;

        static auto AbilityOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", "Ability");
        static auto LevelOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", "Level");
        static auto InputIDOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", "InputID");

        *(FGameplayAbilitySpecHandle*)(__int64(ptr) + HandleOffset) = Handle;
        *(UObject**)(__int64(ptr) + AbilityOffset) = DefaultObject;
        *(int*)(__int64(ptr) + LevelOffset) = 1;
        *(int*)(__int64(ptr) + InputIDOffset) = -1;

        return ptr;
    };

    void* NewSpec = GenerateNewSpec();

    if (!NewSpec)
        return nullptr;

    auto Handle = (FGameplayAbilitySpecHandle*)(__int64(NewSpec) + HandleOffset);

    if (!NewSpec || DoesASCHaveAbility(AbilitySystemComponent, *GetAbilityFromSpec(NewSpec)))
        return nullptr;

    // https://github.com/EpicGames/UnrealEngine/blob/4.22/Engine/Plugins/Runtime/GameplayAbilities/Source/GameplayAbilities/Private/AbilitySystemComponent_Abilities.cpp#L232

    // std::cout << "giving ability: " << DefaultObject->GetFullName() << '\n';

    if (Engine_Version < 426 && Engine_Version >= 420)
        GiveAbility(AbilitySystemComponent, Handle, *(FGameplayAbilitySpec<FGameplayAbilityActivationInfo, 0x50>*)NewSpec);
    else if (Engine_Version < 420)
        GiveAbilityOLDDD(AbilitySystemComponent, Handle, *(FGameplayAbilitySpec<FGameplayAbilityActivationInfo, 0>*)NewSpec);
    else if (std::floor(FnVerDouble) == 15)
        GiveAbilityAHH(AbilitySystemComponent, Handle, *(FGameplayAbilitySpecAHH*)NewSpec);
    else if (Engine_Version == 426)
        GiveAbilityFTS(AbilitySystemComponent, Handle, *(FGameplayAbilitySpec<FGameplayAbilityActivationInfoFTS, 0x50>*)NewSpec);
    else
        GiveAbilityNewer(AbilitySystemComponent, Handle, *(FGameplayAbilitySpecNewer*)NewSpec);

    if (OutSpec)
        *OutSpec = NewSpec;

    return *GetAbilityFromSpec(NewSpec);
}

inline bool ServerTryActivateAbilityHook(UObject* AbilitySystemComponent, UFunction* Function, void* Parameters)
{
    struct UAbilitySystemComponent_ServerTryActivateAbility_Params {
        FGameplayAbilitySpecHandle                  AbilityToActivate;                                        // (Parm)
        bool                                               InputPressed;                                             // (Parm, ZeroConstructor, IsPlainOldData)
        FPredictionKey                              PredictionKey;                                            // (Parm)
    };

    auto Params = (UAbilitySystemComponent_ServerTryActivateAbility_Params*)Parameters;

    InternalServerTryActivateAbility(AbilitySystemComponent, Params->AbilityToActivate, Params->InputPressed, &Params->PredictionKey, nullptr);

    return false;
}

inline bool ServerAbilityRPCBatchHook(UObject* AbilitySystemComponent, UFunction* Function, void* Parameters)
{
    struct UAbilitySystemComponent_ServerAbilityRPCBatch_Params {
        __int64                      BatchInfo;                                                // (Parm)
    };

    static auto AbilitySpecHandleOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.ServerAbilityRPCBatch", "AbilitySpecHandle"); // Function->GetParam<FGameplayAbilitySpecHandle>("AbilitySpecHandle", Parameters);
    static auto InputPressedOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.ServerAbilityRPCBatch", "InputPressed");
    static auto PredictionKeyOffset = FindOffsetStruct("ScriptStruct /Script/GameplayAbilities.ServerAbilityRPCBatch", "PredictionKey");

    auto Params = (UAbilitySystemComponent_ServerAbilityRPCBatch_Params*)Parameters;

    auto AbilitySpecHandle = (FGameplayAbilitySpecHandle*)(__int64(&Params->BatchInfo) + AbilitySpecHandleOffset);
    auto InputPressed = (bool*)(__int64(&Params->BatchInfo) + InputPressedOffset);
    auto PredictionKey = (FPredictionKey*)(__int64(&Params->BatchInfo) + PredictionKeyOffset);

    if (AbilitySpecHandle && InputPressed && PredictionKey)
    {
        auto Spec = FindAbilitySpecFromHandle2(AbilitySystemComponent, *AbilitySpecHandle);

        auto AbilityD = GetAbilityFromSpec(Spec);

        if (false && AbilityD) // Ability->GetFullName().contains("Ranged"))
        {
            auto Ability = *AbilityD;

            auto PlayerState = Helper::GetOwner(AbilitySystemComponent);

            if (PlayerState)
            {
                UObject** PawnD = PlayerState->Member<UObject*>("PawnPrivate");

                if (PawnD)
                {
                    auto Pawn = *PawnD;
                    // std::cout << "pawn: " << Pawn->GetFullName() << '\n';
                    auto currentWeapon = Helper::GetCurrentWeapon(Pawn);

                    if (currentWeapon)
                    {
                        float TimeToNextFire = 0;

                        currentWeapon->ProcessEvent("GetTimeToNextFire", &TimeToNextFire);

                        auto LastFireTime = *currentWeapon->Member<float>("LastFireTime"); // in seconds since game has started
                        auto TimeSeconds = Helper::GetTimeSeconds();

                        /* std::cout << "LastFireTime: " << LastFireTime << '\n';
                        std::cout << "TimeToNextFire: " << TimeToNextFire << '\n';
                        std::cout << "FIirng Rate: " << GetFireRate(currentWeapon) << '\n';
                        std::cout << "Time Seconds: " << TimeSeconds << '\n';
                        std::cout << "Time Seconds - LastFireTime: " << TimeSeconds - LastFireTime << '\n'; */

                        if (TimeToNextFire > (GetFireRate(currentWeapon) / 60)) // (TimeToNextFire > 0.02)
                        {
                            std::cout << "Player is demospeeding!\n";
                            return true;
                        }
                    }
                    else
                        std::cout << "No CurrentWeapon!\n";
                }
                else
                    std::cout << "No Pawn!\n";
            }
        }
        else
            std::cout << "No Ability!\n";

        auto AbilitySpec = FindAbilitySpecFromHandle2(AbilitySystemComponent, *AbilitySpecHandle);

        if (!AbilitySpec)
            return false;

        InternalServerTryActivateAbility(AbilitySystemComponent, *AbilitySpecHandle, *InputPressed, PredictionKey, nullptr);
    }

    return false;
}

inline bool ServerTryActivateAbilityWithEventDataHook(UObject* AbilitySystemComponent, UFunction* Function, void* Parameters)
{
    // std::cout << ("ServerTryActivateAbilityWithEventData!\n");

    struct UAbilitySystemComponent_ServerTryActivateAbilityWithEventData_Params {
        FGameplayAbilitySpecHandle                  AbilityToActivate;                                        // (Parm)
        bool                                               InputPressed;                                             // (Parm, ZeroConstructor, IsPlainOldData)
        FPredictionKey                              PredictionKey;                                            // (Parm)
        __int64                          TriggerEventData;                                         // (Parm)
    };

    auto Params = (UAbilitySystemComponent_ServerTryActivateAbilityWithEventData_Params*)Parameters;

    InternalServerTryActivateAbility(AbilitySystemComponent, Params->AbilityToActivate, Params->InputPressed, &Params->PredictionKey, &Params->TriggerEventData);

    return false;
}

// BY ANDROID THANKS FIXES ABILITIES

bool (*o_CanActivateAbility)(UObject* GameplayAbility, const FGameplayAbilitySpecHandle Handle, const void* ActorInfo, const void* SourceTags, const void* TargetTags, OUT FGameplayTagContainer* OptionalRelevantTags);
bool hk_CanActivateAbility(UObject* GameplayAbility, const FGameplayAbilitySpecHandle Handle, const void* ActorInfo, const void* SourceTags, const void* TargetTags, OUT FGameplayTagContainer* OptionalRelevantTags)
{
    // bool ret = o_CanActivateAbility(GameplayAbility, Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);

    return true;
}

void InitializeAbilityHooks()
{
    if (FnVerDouble < 19.00)
    {
        AddHook(("Function /Script/GameplayAbilities.AbilitySystemComponent.ServerTryActivateAbilityWithEventData"), ServerTryActivateAbilityWithEventDataHook);
        AddHook(("Function /Script/GameplayAbilities.AbilitySystemComponent.ServerAbilityRPCBatch"), ServerAbilityRPCBatchHook);
        AddHook(("Function /Script/GameplayAbilities.AbilitySystemComponent.ServerTryActivateAbility"), ServerTryActivateAbilityHook);

        if (o_CanActivateAbility)
        {
            MH_CreateHook((PVOID)CanActivateAbilityAddr, hk_CanActivateAbility, (void**)&o_CanActivateAbility);
            MH_EnableHook((PVOID)CanActivateAbilityAddr);
        }
        else
            std::cout << "CanActivateAbility not found!\n";
    }
}

template <typename actualClass>
void AHH(const std::string& ClassName)
{
    auto Class = FindObjectOld(ClassName, true);

    if (Class)
    {
        auto SizeOfClass = GetSizeOfStruct(Class);
        auto OurSizeOfClass = sizeof(actualClass);

        std::cout << std::format("{}: {} {}\n", ClassName, OurSizeOfClass, SizeOfClass);
    }
}

void TestAbilitySizeDifference()
{
    /* auto PredictionKeyDiff = SizeOfPredictionKey >= sizeof(FPredictionKey) ? SizeOfPredictionKey - sizeof(FPredictionKey) : sizeof(FPredictionKey) - SizeOfPredictionKey; */

    if (Engine_Version < 426)
        AHH<FPredictionKey>(("ScriptStruct /Script/GameplayAbilities.PredictionKey"));
    else if (FnVerDouble < 19.00)
        AHH<FPredictionKeyFTS>(("ScriptStruct /Script/GameplayAbilities.PredictionKey"));
    else
        AHH<FPredictionKeyNewer>(("ScriptStruct /Script/GameplayAbilities.PredictionKey"));

    AHH<FGameplayAbilitySpecHandle>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpecHandle"));

    if (Engine_Version < 426 && Engine_Version >= 420)
        AHH<FGameplayAbilitySpec<FGameplayAbilityActivationInfo, 0x50>>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec"));
    else if (Engine_Version < 420)
        AHH<FGameplayAbilitySpec<FGameplayAbilityActivationInfo, 0>>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec"));
    else if (std::floor(FnVerDouble) == 15)
        AHH<FGameplayAbilitySpecAHH>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec"));
    else if (Engine_Version == 426)
        AHH<FGameplayAbilitySpec<FGameplayAbilityActivationInfoFTS, 0x50>>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec"));
    else
        AHH<FGameplayAbilitySpecNewer>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec"));

    if (Engine_Version <= 422)
    {
        AHH<FGameplayAbilitySpecContainerOL>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpecContainer"));
        AHH<FGameplayAbilityTargetDataHandleOL>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilityTargetDataHandle"));
        AHH<FGameplayEventDataOL>(("ScriptStruct /Script/GameplayAbilities.GameplayEventData"));
        AHH<FFastArraySerializerOL>(("ScriptStruct /Script/Engine.FastArraySerializer"));
    }
    else
    {
        AHH<FGameplayAbilitySpecContainerSE>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpecContainer"));
        AHH<FGameplayAbilityTargetDataHandleSE>(("ScriptStruct /Script/GameplayAbilities.GameplayAbilityTargetDataHandle"));

        AHH<FGameplayEventDataSE>(("ScriptStruct /Script/GameplayAbilities.GameplayEventData"));
        AHH<FFastArraySerializerSE>(("ScriptStruct /Script/Engine.FastArraySerializer"));
    }
}

std::vector<UObject*> GiveAbilitySet(UObject* Pawn, UObject* AbilitySet)
{
    if (!Pawn || !AbilitySet)
        return std::vector<UObject*>();

    std::vector<UObject*> AbilitiesToRet;

    {
        std::cout << ("Granting abiityst!\n");

        auto Abilities = AbilitySet->CachedMember<TArray<UObject*>>(("GameplayAbilities"));

        if (Abilities)
        {
            for (int i = 0; i < Abilities->Num(); i++)
            {
                auto Ability = Abilities->At(i);

                if (!Ability)
                    continue;

                auto ability = GrantGameplayAbility(Pawn, Ability);

                AbilitiesToRet.push_back(ability);

                // std::cout << "Granting ability " << Ability->GetFullName() << '\n';
            }
        }
    }

    return AbilitiesToRet;
}

void GiveAllBRAbilities(UObject* Pawn)
{
    if (GiveAbility || GiveAbilityFTS || GiveAbilityNewer || GiveAbilityAHH || GiveAbilityOLDDD)
    {
        auto AbilitySystemComponent = *Pawn->CachedMember<UObject*>(("AbilitySystemComponent"));

        if (AbilitySystemComponent)
        {
            std::cout << ("Granting abilities!\n");

            if (FnVerDouble < 8)
            {
                static auto AbilitySet = FindObject(("FortAbilitySet /Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_DefaultPlayer.GAS_DefaultPlayer"));

                if (AbilitySet)
                {
                    auto Abilities = AbilitySet->Member<TArray<UObject*>>(("GameplayAbilities"));

                    if (Abilities)
                    {
                        for (int i = 0; i < Abilities->Num(); i++)
                        {
                            auto Ability = Abilities->At(i);

                            if (!Ability)
                                continue;

                            GrantGameplayAbility(Pawn, Ability);
                            // std::cout << "Granting ability " << Ability->GetFullName() << '\n';
                        }
                    }
                }

                static auto RangedAbility = FindObject(("BlueprintGeneratedClass /Game/Abilities/Weapons/Ranged/GA_Ranged_GenericDamage.GA_Ranged_GenericDamage_C"));

                if (RangedAbility)
                    GrantGameplayAbility(Pawn, RangedAbility);
            }
            else
            {
                {
                    static auto AbilitySet = FindObject(("FortAbilitySet /Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer"));

                    if (AbilitySet)
                    {
                        auto Abilities = AbilitySet->Member<TArray<UObject*>>(("GameplayAbilities"));

                        if (Abilities)
                        {
                            for (int i = 0; i < Abilities->Num(); i++)
                            {
                                auto Ability = Abilities->At(i);

                                if (!Ability)
                                    continue;

                                GrantGameplayAbility(Pawn, Ability);
                                // std::cout << "Granting ability " << Ability->GetFullName() << '\n';
                            }
                        }
                    }
                }
            }

            if (Engine_Version < 424) // i dont think needed
            {
                static auto EmoteAbility = FindObject(("BlueprintGeneratedClass /Game/Abilities/Emotes/GAB_Emote_Generic.GAB_Emote_Generic_C"));

                if (EmoteAbility)
                {
                    GrantGameplayAbility(Pawn, EmoteAbility);
                }
            }

            std::cout << ("Granted Abilities!\n");
        }
        else
            std::cout << ("Unable to find AbilitySystemComponent!\n");
    }
    else
        std::cout << ("Unable to grant abilities due to no GiveAbility!\n");
}