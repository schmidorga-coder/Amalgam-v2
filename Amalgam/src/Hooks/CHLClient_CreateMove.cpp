#include "../SDK/SDK.h"
// ... includes ...
#include <array>

// ... Macros ...

// Ring Buffer implementation for History to avoid heap allocations in CreateMove
template<typename T, size_t Size>
class RingBuffer {
    std::array<T, Size> data;
    size_t head = 0;
    size_t count = 0;
public:
    void push_front(const T& item) {
        head = (head + Size - 1) % Size;
        data[head] = item;
        if (count < Size) count++;
    }
    T& operator[](size_t index) { return data[(head + index) % Size]; }
    const T& operator[](size_t index) const { return data[(head + index) % Size]; }
    size_t size() const { return count; }
};

struct CmdHistory_t
{
    Vec3 m_vAngle;
    bool m_bAttack1;
    bool m_bAttack2;
    bool m_bSendingPacket;
};

// Split big UpdateInfo function
static void UpdateWeaponInfo(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    G::CanPrimaryAttack = G::CanSecondaryAttack = G::Reloading = false;

    // ... (Keep existing weapon slot loop logic here, it is game specific) ...
    // Note: This logic is complex and specific to TF2 mechanics, kept mostly intact but formatted.
    
    // Check specific weapon logic
    bool bCanAttack = pLocal->CanAttack();
    if (bCanAttack)
    {
        G::CanPrimaryAttack = pWeapon->CanPrimaryAttack();
        G::CanSecondaryAttack = pWeapon->CanSecondaryAttack();
        
        // ... (Switch statement for specific weapon IDs) ...
    }
    
    // Update global attack state
    G::Attacking = SDK::IsAttacking(pLocal, pWeapon, pCmd);
    G::PrimaryWeaponType = SDK::GetWeaponType(pWeapon, &G::SecondaryWeaponType);
    G::CanHeadshot = pWeapon->CanHeadshot() || pWeapon->AmbassadorCanHeadshot(TICKS_TO_TIME(pLocal->m_nTickBase()));
}

__declspec(noinline) static void UpdateInfo(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    G::PSilentAngles = G::SilentAngles = G::Attacking = G::Throwing = false;
    G::LastUserCmd = G::CurrentUserCmd ? G::CurrentUserCmd : pCmd;
    G::CurrentUserCmd = pCmd;
    G::OriginalCmd = *pCmd;

    if (pWeapon)
        UpdateWeaponInfo(pLocal, pWeapon, pCmd);
}

__declspec(noinline) static void AntiCheatCompatibility(CUserCmd* pCmd, bool* pSendPacket)
{
    if (!Vars::Misc::Game::AntiCheatCompatibility.Value)
        return;

    Math::ClampAngles(pCmd->viewangles);

    // Optimized: Use RingBuffer instead of std::deque
    static RingBuffer<CmdHistory_t, 5> vHistory;
    
    vHistory.push_front({pCmd->viewangles, (bool)(pCmd->buttons & IN_ATTACK), (bool)(pCmd->buttons & IN_ATTACK2), *pSendPacket});

    if (vHistory.size() < 3)
        return;

    // logic to prevent trigger checks
    if (!vHistory[0].m_bAttack1 && vHistory[1].m_bAttack1 && !vHistory[2].m_bAttack1)
        pCmd->buttons |= IN_ATTACK;
    
    // ... rest of AC logic using vHistory ...
    // Note: The logic access pattern (vHistory[0], vHistory[1]) works identically with the RingBuffer
}

MAKE_HOOK(CHLClient_CreateMove, U::Memory.GetVirtual(I::Client, 21), void,
    void* rcx, int sequence_number, float input_sample_frametime, bool active)
{
#ifdef DEBUG_HOOKS
    if (!Vars::Hooks::CHLClient_CreateMove[DEFAULT_BIND])
        return CALL_ORIGINAL(rcx, sequence_number, input_sample_frametime, active);
#endif

    CALL_ORIGINAL(rcx, sequence_number, input_sample_frametime, active);
    
    static auto uSendPackedAddr = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + 0x20;
    auto pSendPacket = reinterpret_cast<bool*>(uSendPackedAddr);

    auto pLocal = H::Entities.GetLocal();
    if (!pLocal || G::Unload)
        return;
        
    auto pWeapon = H::Entities.GetWeapon(); // Move inside check

    CUserCmd* pCmd = &I::Input->m_pCommands[sequence_number % MULTIPLAYER_BACKUP];

    // Prediction Update
    if (I::Prediction && I::ClientState) {
        I::Prediction->Update(I::ClientState->m_nDeltaTick, 
                              I::ClientState->m_nDeltaTick > 0, 
                              I::ClientState->last_command_ack, 
                              I::ClientState->lastoutgoingcommand + I::ClientState->chokedcommands);
    }

    UpdateInfo(pLocal, pWeapon, pCmd);

    // Feature execution ...
    // ...
    // This part is mostly fine, just ensuring modular calls
    
    AntiCheatCompatibility(pCmd, pSendPacket);

#ifndef TEXTMODE
    LocalAnimations(pLocal, pCmd, *pSendPacket);
#endif

    G::Choking = !*pSendPacket;
    G::LastUserCmd = pCmd;
}
