#include "Core.h"

#include "../SDK/SDK.h"
#include "../BytePatches/BytePatches.h"
#include "../Features/Configs/Configs.h"
#include "../Features/ImGui/Menu/Menu.h"
#include "../Features/EnginePrediction/EnginePrediction.h"
#include "../Features/Visuals/Materials/Materials.h"
#include "../Features/Visuals/Visuals.h"
#include "../SDK/Events/Events.h"
#ifdef TEXTMODE
#include "../Features/Misc/NamedPipe/NamedPipe.h"
#endif
#include "../Utils/Hash/FNV1A.h"

#include <Psapi.h>
#include <TlHelp32.h>
#include <thread>
#include <vector>
#include <memory>

// RAII Handle wrapper
struct HandleDeleter {
    void operator()(HANDLE h) const { if (h) CloseHandle(h); }
};
using ScopedHandle = std::unique_ptr<void, HandleDeleter>;

static std::string GetProcessName(DWORD dwProcessID)
{
    ScopedHandle hProcess(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwProcessID));
    if (!hProcess)
        return "";

    char buffer[MAX_PATH];
    if (GetModuleBaseNameA(hProcess.get(), nullptr, buffer, std::size(buffer)))
    {
        return buffer;
    }

    return "";
}

static bool CheckDXLevel()
{
    auto mat_dxlevel = H::ConVars.FindVar("mat_dxlevel");
    if (mat_dxlevel && mat_dxlevel->GetInt() < 90)
    {
        const char* sMessage = "You are running with graphics options that Amalgam does not support. It is recommended for -dxlevel to be at least 90.";
        F::Menu.ShowDeferredNotification("Graphics Warning", sMessage);
        SDK::Output("Amalgam", sMessage, DEFAULT_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG);
        // return false; // Optionally enforce this
    }
    return true;
}

void CCore::AppendFailText(const char* sMessage)
{
    if (m_ssFailStream.str().empty())
    {
        m_ssFailStream << std::format("Built @ {}, {}, {}\n", __DATE__, __TIME__, __CONFIGURATION__);
        m_ssFailStream << std::format("Time @ {}, {}\n\n", SDK::GetDate(), SDK::GetTime());
    }

    auto formattedMsg = std::format("{}\n", sMessage);
    m_ssFailStream << formattedMsg;
    OutputDebugStringA(formattedMsg.c_str());
}

void CCore::LogFailText()
{
    try
    {
        std::ofstream file(F::Configs.m_sConfigPath + "fail_log.txt", std::ios_base::app);
        if (file.is_open()) {
            file << m_ssFailStream.str() << "\n\n\n";
        }
        
        m_ssFailStream << "\nCtrl + C to copy.\nLogged to Amalgam\\fail_log.txt.";
    }
    catch (...) {}

#ifndef TEXTMODE
    SDK::Output("Failed to load", m_ssFailStream.str().c_str(), {}, OUTPUT_DEBUG, MB_OK | MB_ICONERROR);
#endif
}

static bool ModulesLoaded()
{
#ifndef TEXTMODE
    if (!SDK::GetTeamFortressWindow())
        return false;
#endif

    // Check client.dll signatures early to ensure full initialization
    if (GetModuleHandleA("client.dll"))
    {
        // Use a lambda or helper to check critical signatures immediately
        auto checkSig = [](const char* sig) -> bool {
            auto dwDest = U::Memory.FindSignature("client.dll", sig);
            return dwDest && *reinterpret_cast<void**>(U::Memory.RelToAbs(dwDest));
        };

        if (!checkSig("48 8B 0D ? ? ? ? 48 8B 10 48 8B 19 48 8B C8 FF 92") || 
            !checkSig("48 8B 0D ? ? ? ? F3 0F 59 CA 44 8D 42"))
            return false;
    }
    else 
        return false;

    // List of required modules
    static const std::vector<const char*> requiredModules = {
#ifdef TEXTMODE
        "TextmodeTF2x64Release.dll",
#endif
        "engine.dll",
        "server.dll",
        "tier0.dll",
        "vstdlib.dll",
        "vgui2.dll",
        "vguimatsurface.dll",
        "materialsystem.dll",
        "inputsystem.dll",
        "vphysics.dll",
        "steamclient64.dll"
    };

    for (const auto& mod : requiredModules) {
        if (!GetModuleHandleA(mod)) return false;
    }

    // Check for either DX9 or Vulkan
    return GetModuleHandleA("shaderapidx9.dll") || GetModuleHandleA("shaderapivk.dll");
}

void CCore::Load()
{
    // Verify Process
    if (FNV1A::Hash32(GetProcessName(GetCurrentProcessId()).c_str()) != FNV1A::Hash32Const("tf_win64.exe"))
    {
        m_bUnload = m_bFailed = true;
        AppendFailText("Invalid process - Amalgam only supports tf_win64.exe");
        return;
    }

#ifdef TEXTMODE
    F::NamedPipe.Initialize();
#endif

    // Wait for modules
    float flTime = 0.f;
    while (!ModulesLoaded())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        flTime += 0.5f;

        if (flTime >= 60.f) {
            m_bUnload = m_bFailed = true;
            AppendFailText("Failed to load - Timeout waiting for modules");
            return;
        }

        if (U::KeyHandler.Down(VK_F11, true)) {
            m_bUnload = m_bFailed = true;
            AppendFailText("Cancelled load via F11");
            return;
        }
    }

    // Initialization Sequence
    if (!U::Signatures.Initialize() || !U::Interfaces.Initialize() || !CheckDXLevel()) {
        m_bUnload = m_bFailed = true;
        return;
    }

    if (!U::Hooks.Initialize() || !U::BytePatches.Initialize() || !H::Events.Initialize()) {
        m_bUnload = m_bFailed2 = true;
        return;
    }

#ifndef TEXTMODE
    F::Materials.LoadMaterials();
#endif
    H::ConVars.Unlock();

    F::Configs.LoadConfig(F::Configs.m_sCurrentConfig, false);
    I::EngineClient->ClientCmd_Unrestricted("exec catexec");
    SDK::Output("Amalgam", "Loaded", DEFAULT_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG);
}

void CCore::Loop()
{
    while (!m_bUnload)
    {
#ifndef TEXTMODE
        // Unload hotkey check
        if (U::KeyHandler.Down(VK_F11) && SDK::IsGameWindowInFocus()) {
            m_bUnload = true;
            break;
        }
#endif 
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void CCore::Unload()
{
#ifdef TEXTMODE
    F::NamedPipe.Shutdown();
#endif

    if (m_bFailed)
    {
        LogFailText();
        return;
    }

    G::Unload = true;

    // cleanup logic
    m_bFailed2 |= !U::Hooks.Unload();
    U::BytePatches.Unload();
    H::Events.Unload();

    if (F::Menu.m_bIsOpen && I::MatSystemSurface)
        I::MatSystemSurface->SetCursorAlwaysVisible(false);
    
    F::Visuals.RestoreWorldModulation();

    // Reset camera if needed
    if (I::Input && I::Input->CAM_IsThirdPerson())
    {
        if (auto pLocal = H::Entities.GetLocal())
        {
            I::Input->CAM_ToFirstPerson();
            pLocal->ThirdPersonSwitch();
        }
    }

    if (auto var = H::ConVars.FindVar("cl_wpn_sway_interp")) var->SetValue(0.f);
    if (auto var = H::ConVars.FindVar("cl_wpn_sway_scale")) var->SetValue(0.f);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    F::EnginePrediction.Unload();
    H::ConVars.Restore();
    F::Materials.UnloadMaterials();

    if (m_bFailed2)
    {
        LogFailText();
        return;
    }

    SDK::Output("Amalgam", "Unloaded", DEFAULT_COLOR, OUTPUT_CONSOLE | OUTPUT_DEBUG);
}
