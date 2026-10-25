#include "StdInc.h"
#include "Input/SteamInputEmulator.h"
#include "Logger.h"
#include "SteamVersionLogger.h"

#include <Xinput.h>

// Keep this implementation independent from Valve's public SDK headers.
// The rest of this emulator intentionally uses lightweight Steam ABI typedefs
// in StdInc.h (for example CSteamID as uint64_t). Pulling isteaminput.h here
// also pulls steamclientpublic.h / steam_api_internal.h, which define their own
// CSteamID class and CallbackMsg_t structure and therefore conflict with the
// emulator's ABI shim types. The declarations below mirror the ISteamInput006
// ABI that the game calls without importing those conflicting SDK headers.
namespace
{
    using InputHandle_t = uint64_t;
    using InputActionSetHandle_t = uint64_t;
    using InputDigitalActionHandle_t = uint64_t;
    using InputAnalogActionHandle_t = uint64_t;

    constexpr int STEAM_INPUT_MAX_COUNT = 16;

    enum EInputSourceMode : int
    {
        k_EInputSourceMode_None = 0
    };

    enum EInputActionOrigin : int
    {
        k_EInputActionOrigin_None = 0
    };

    enum EXboxOrigin : int
    {
        k_EXboxOrigin_A = 0
    };

    enum ESteamControllerPad : int
    {
        k_ESteamControllerPad_Left = 0,
        k_ESteamControllerPad_Right = 1
    };

    enum EControllerHapticLocation : int
    {
        k_EControllerHapticLocation_Left = 1,
        k_EControllerHapticLocation_Right = 2,
        k_EControllerHapticLocation_Both = 3
    };

    enum ESteamInputType : int
    {
        k_ESteamInputType_Unknown = 0,
        k_ESteamInputType_XBox360Controller = 2,
        k_ESteamInputType_XBoxOneController = 3
    };

    enum ESteamInputGlyphSize : int
    {
        k_ESteamInputGlyphSize_Small = 0,
        k_ESteamInputGlyphSize_Medium = 1,
        k_ESteamInputGlyphSize_Large = 2,
        k_ESteamInputGlyphSize_Count = 3
    };

    enum ESteamInputConfigurationEnableType : uint16_t
    {
        k_ESteamInputConfigurationEnableType_None = 0x0000,
        k_ESteamInputConfigurationEnableType_Playstation = 0x0001,
        k_ESteamInputConfigurationEnableType_Xbox = 0x0002,
        k_ESteamInputConfigurationEnableType_Generic = 0x0004,
        k_ESteamInputConfigurationEnableType_Switch = 0x0008
    };

#pragma pack(push, 1)
    struct InputAnalogActionData_t
    {
        EInputSourceMode eMode;
        float x;
        float y;
        bool bActive;
    };

    struct InputDigitalActionData_t
    {
        bool bState;
        bool bActive;
    };

    struct InputMotionData_t
    {
        float rotQuatX;
        float rotQuatY;
        float rotQuatZ;
        float rotQuatW;
        float posAccelX;
        float posAccelY;
        float posAccelZ;
        float rotVelX;
        float rotVelY;
        float rotVelZ;
    };
#pragma pack(pop)

    struct ScePadTriggerEffectParam;
    using SteamInputActionEventCallbackPointer = void (*)(void*);

    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

    XInputGetStateFn ResolveXInputGetState()
    {
        static XInputGetStateFn fn = nullptr;
        static bool resolved = false;
        if (resolved)
            return fn;

        resolved = true;
        const char* modules[] =
        {
            "xinput1_4.dll",
            "xinput1_3.dll",
            "xinput9_1_0.dll",
            "xinput1_2.dll",
            "xinput1_1.dll"
        };

        for (const char* name : modules)
        {
            HMODULE module = GetModuleHandleA(name);
            if (!module)
                module = LoadLibraryA(name);
            if (!module)
                continue;

            auto proc = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (proc)
            {
                fn = proc;
                break;
            }
        }

        return fn;
    }

    bool IsXInputConnected(int index)
    {
        auto fn = ResolveXInputGetState();
        if (!fn || index < 0 || index >= 4)
            return false;

        XINPUT_STATE state{};
        return fn(static_cast<DWORD>(index), &state) == ERROR_SUCCESS;
    }

    uint64_t HashHandle(const char* text)
    {
        if (!text || !text[0])
            return 1;

        uint64_t hash = 14695981039346656037ull;
        while (*text)
        {
            hash ^= static_cast<unsigned char>(*text++);
            hash *= 1099511628211ull;
        }

        return hash ? hash : 1;
    }

    // ISteamInput006-compatible vtable. Do not reorder these virtual methods.
    class OfflineSteamInput final
    {
    public:
        virtual bool Init(bool bExplicitlyCallRunFrame)
        {
            NSR_UNUSED(bExplicitlyCallRunFrame);
            initialized_ = true;
            SteamVersionLogger::LogCall("SteamInput", "Init");
            Logger::Info("SteamInput::Init -> true");
            return true;
        }

        virtual bool Shutdown()
        {
            initialized_ = false;
            actionEventCallback_ = nullptr;
            Logger::Info("SteamInput::Shutdown -> true");
            return true;
        }

        virtual bool SetInputActionManifestFilePath(const char* path)
        {
            manifestPath_ = path ? path : "";
            Logger::Info("SteamInput::SetInputActionManifestFilePath -> true");
            return true;
        }

        virtual void RunFrame(bool bReservedValue = true)
        {
            NSR_UNUSED(bReservedValue);
        }

        virtual bool BWaitForData(bool bWaitForever, uint32_t unTimeout)
        {
            NSR_UNUSED(bWaitForever);
            NSR_UNUSED(unTimeout);
            return initialized_;
        }

        virtual bool BNewDataAvailable()
        {
            return initialized_;
        }

        virtual int GetConnectedControllers(InputHandle_t* handlesOut)
        {
            int count = 0;
            for (int index = 0; index < 4; ++index)
            {
                if (!IsXInputConnected(index))
                    continue;

                if (handlesOut && count < STEAM_INPUT_MAX_COUNT)
                    handlesOut[count] = static_cast<InputHandle_t>(index + 1);
                ++count;
            }
            return count;
        }

        virtual void EnableDeviceCallbacks() {}

        virtual void EnableActionEventCallbacks(SteamInputActionEventCallbackPointer pCallback)
        {
            actionEventCallback_ = pCallback;
        }

        virtual InputActionSetHandle_t GetActionSetHandle(const char* name)
        {
            return static_cast<InputActionSetHandle_t>(HashHandle(name));
        }

        virtual void ActivateActionSet(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle)
        {
            NSR_UNUSED(inputHandle);
            currentActionSet_ = actionSetHandle;
        }

        virtual InputActionSetHandle_t GetCurrentActionSet(InputHandle_t inputHandle)
        {
            NSR_UNUSED(inputHandle);
            return currentActionSet_;
        }

        virtual void ActivateActionSetLayer(InputHandle_t inputHandle, InputActionSetHandle_t actionSetLayerHandle)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(actionSetLayerHandle);
        }

        virtual void DeactivateActionSetLayer(InputHandle_t inputHandle, InputActionSetHandle_t actionSetLayerHandle)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(actionSetLayerHandle);
        }

        virtual void DeactivateAllActionSetLayers(InputHandle_t inputHandle)
        {
            NSR_UNUSED(inputHandle);
        }

        virtual int GetActiveActionSetLayers(InputHandle_t inputHandle, InputActionSetHandle_t* handlesOut)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(handlesOut);
            return 0;
        }

        virtual InputDigitalActionHandle_t GetDigitalActionHandle(const char* name)
        {
            return static_cast<InputDigitalActionHandle_t>(HashHandle(name));
        }

        virtual InputDigitalActionData_t GetDigitalActionData(InputHandle_t inputHandle, InputDigitalActionHandle_t digitalActionHandle)
        {
            NSR_UNUSED(digitalActionHandle);
            InputDigitalActionData_t result{};
            result.bActive = inputHandle != 0;
            result.bState = false;
            return result;
        }

        virtual int GetDigitalActionOrigins(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle, InputDigitalActionHandle_t digitalActionHandle, EInputActionOrigin* originsOut)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(actionSetHandle);
            NSR_UNUSED(digitalActionHandle);
            NSR_UNUSED(originsOut);
            return 0;
        }

        virtual const char* GetStringForDigitalActionName(InputDigitalActionHandle_t actionHandle)
        {
            NSR_UNUSED(actionHandle);
            return "";
        }

        virtual InputAnalogActionHandle_t GetAnalogActionHandle(const char* name)
        {
            return static_cast<InputAnalogActionHandle_t>(HashHandle(name));
        }

        virtual InputAnalogActionData_t GetAnalogActionData(InputHandle_t inputHandle, InputAnalogActionHandle_t analogActionHandle)
        {
            NSR_UNUSED(analogActionHandle);
            InputAnalogActionData_t result{};
            result.eMode = k_EInputSourceMode_None;
            result.x = 0.0f;
            result.y = 0.0f;
            result.bActive = inputHandle != 0;
            return result;
        }

        virtual int GetAnalogActionOrigins(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle, InputAnalogActionHandle_t analogActionHandle, EInputActionOrigin* originsOut)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(actionSetHandle);
            NSR_UNUSED(analogActionHandle);
            NSR_UNUSED(originsOut);
            return 0;
        }

        virtual const char* GetGlyphPNGForActionOrigin(EInputActionOrigin origin, ESteamInputGlyphSize size, uint32_t flags)
        {
            NSR_UNUSED(origin);
            NSR_UNUSED(size);
            NSR_UNUSED(flags);
            return "";
        }

        virtual const char* GetGlyphSVGForActionOrigin(EInputActionOrigin origin, uint32_t flags)
        {
            NSR_UNUSED(origin);
            NSR_UNUSED(flags);
            return "";
        }

        virtual const char* GetGlyphForActionOrigin_Legacy(EInputActionOrigin origin)
        {
            NSR_UNUSED(origin);
            return "";
        }

        virtual const char* GetStringForActionOrigin(EInputActionOrigin origin)
        {
            NSR_UNUSED(origin);
            return "";
        }

        virtual const char* GetStringForAnalogActionName(InputAnalogActionHandle_t actionHandle)
        {
            NSR_UNUSED(actionHandle);
            return "";
        }

        virtual void StopAnalogActionMomentum(InputHandle_t inputHandle, InputAnalogActionHandle_t actionHandle)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(actionHandle);
        }

        virtual InputMotionData_t GetMotionData(InputHandle_t inputHandle)
        {
            NSR_UNUSED(inputHandle);
            return InputMotionData_t{};
        }

        virtual void TriggerVibration(InputHandle_t inputHandle, unsigned short leftSpeed, unsigned short rightSpeed)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(leftSpeed);
            NSR_UNUSED(rightSpeed);
        }

        virtual void TriggerVibrationExtended(InputHandle_t inputHandle, unsigned short leftSpeed, unsigned short rightSpeed, unsigned short leftTriggerSpeed, unsigned short rightTriggerSpeed)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(leftSpeed);
            NSR_UNUSED(rightSpeed);
            NSR_UNUSED(leftTriggerSpeed);
            NSR_UNUSED(rightTriggerSpeed);
        }

        virtual void TriggerSimpleHapticEvent(InputHandle_t inputHandle, EControllerHapticLocation location, uint8_t intensity, char gainDB, uint8_t otherIntensity, char otherGainDB)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(location);
            NSR_UNUSED(intensity);
            NSR_UNUSED(gainDB);
            NSR_UNUSED(otherIntensity);
            NSR_UNUSED(otherGainDB);
        }

        virtual void SetLEDColor(InputHandle_t inputHandle, uint8_t red, uint8_t green, uint8_t blue, unsigned int flags)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(red);
            NSR_UNUSED(green);
            NSR_UNUSED(blue);
            NSR_UNUSED(flags);
        }

        virtual void Legacy_TriggerHapticPulse(InputHandle_t inputHandle, ESteamControllerPad targetPad, unsigned short durationMicroSec)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(targetPad);
            NSR_UNUSED(durationMicroSec);
        }

        virtual void Legacy_TriggerRepeatedHapticPulse(InputHandle_t inputHandle, ESteamControllerPad targetPad, unsigned short durationMicroSec, unsigned short offMicroSec, unsigned short repeat, unsigned int flags)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(targetPad);
            NSR_UNUSED(durationMicroSec);
            NSR_UNUSED(offMicroSec);
            NSR_UNUSED(repeat);
            NSR_UNUSED(flags);
        }

        virtual bool ShowBindingPanel(InputHandle_t inputHandle)
        {
            NSR_UNUSED(inputHandle);
            return false;
        }

        virtual ESteamInputType GetInputTypeForHandle(InputHandle_t inputHandle)
        {
            return inputHandle >= 1 && inputHandle <= 4 ? k_ESteamInputType_XBoxOneController : k_ESteamInputType_Unknown;
        }

        virtual InputHandle_t GetControllerForGamepadIndex(int index)
        {
            return IsXInputConnected(index) ? static_cast<InputHandle_t>(index + 1) : 0;
        }

        virtual int GetGamepadIndexForController(InputHandle_t inputHandle)
        {
            if (inputHandle < 1 || inputHandle > 4)
                return -1;

            const int index = static_cast<int>(inputHandle - 1);
            return IsXInputConnected(index) ? index : -1;
        }

        virtual const char* GetStringForXboxOrigin(EXboxOrigin origin)
        {
            NSR_UNUSED(origin);
            return "";
        }

        virtual const char* GetGlyphForXboxOrigin(EXboxOrigin origin)
        {
            NSR_UNUSED(origin);
            return "";
        }

        virtual EInputActionOrigin GetActionOriginFromXboxOrigin(InputHandle_t inputHandle, EXboxOrigin origin)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(origin);
            return k_EInputActionOrigin_None;
        }

        virtual EInputActionOrigin TranslateActionOrigin(ESteamInputType destinationInputType, EInputActionOrigin sourceOrigin)
        {
            NSR_UNUSED(destinationInputType);
            return sourceOrigin;
        }

        virtual bool GetDeviceBindingRevision(InputHandle_t inputHandle, int* major, int* minor)
        {
            if (major)
                *major = 1;
            if (minor)
                *minor = 0;
            return inputHandle != 0;
        }

        virtual uint32_t GetRemotePlaySessionID(InputHandle_t inputHandle)
        {
            NSR_UNUSED(inputHandle);
            return 0;
        }

        virtual uint16_t GetSessionInputConfigurationSettings()
        {
            return k_ESteamInputConfigurationEnableType_Xbox;
        }

        virtual void SetDualSenseTriggerEffect(InputHandle_t inputHandle, const ScePadTriggerEffectParam* param)
        {
            NSR_UNUSED(inputHandle);
            NSR_UNUSED(param);
        }

    private:
        bool initialized_ = false;
        std::string manifestPath_;
        SteamInputActionEventCallbackPointer actionEventCallback_ = nullptr;
        InputActionSetHandle_t currentActionSet_ = 0;
    };

    OfflineSteamInput g_Input;
}

void* SteamInputEmulator::GetInterface()
{
    return &g_Input;
}
