#include "Aimbot.h"
#include "AimbotHitscan/AimbotHitscan.h"
#include "AimbotProjectile/AimbotProjectile.h"
#include "AimbotMelee/AimbotMelee.h"
// ... includes ...

bool CAimbot::ShouldRun(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
    if (Vars::Aimbot::General::DisableOnSpectate.Value && H::Entities.IsSpectated())
        return false;

    if (!pLocal || !pWeapon || !pLocal->CanAttack())
        return false;
        
    if (!SDK::AttribHookValue(1, "mult_dmg", pWeapon))
        return false;

    return true;
}

void CAimbot::RunAimbot(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, bool bSecondary)
{
    m_bRunningSecondary = bSecondary;
    EWeaponType eWeaponType = !bSecondary ? G::PrimaryWeaponType : G::SecondaryWeaponType;

    // Save state if running secondary logic
    bool bOriginalCanPrimary = G::CanPrimaryAttack;
    if (bSecondary)
        G::CanPrimaryAttack = G::CanSecondaryAttack;

    // Dispatcher
    switch (eWeaponType)
    {
    case EWeaponType::HITSCAN: F::AimbotHitscan.Run(pLocal, pWeapon, pCmd); break;
    case EWeaponType::PROJECTILE: F::AimbotProjectile.Run(pLocal, pWeapon, pCmd); break;
    case EWeaponType::MELEE: F::AimbotMelee.Run(pLocal, pWeapon, pCmd); break;
    default: break;
    }

    // Restore state
    if (bSecondary)
        G::CanPrimaryAttack = bOriginalCanPrimary;
}

void CAimbot::RunMain(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    // Handle queued weapon switch cancellations
    if (F::AimbotProjectile.m_iLastTickCancel)
    {
        pCmd->weaponselect = F::AimbotProjectile.m_iLastTickCancel;
        F::AimbotProjectile.m_iLastTickCancel = 0;
    }

    // Reset targets if expired
    if (abs(G::AimTarget.m_iTickCount - I::GlobalVars->tickcount) > G::AimTarget.m_iDuration)
        G::AimTarget = {};
    if (abs(G::AimPoint.m_iTickCount - I::GlobalVars->tickcount) > G::AimPoint.m_iDuration)
        G::AimPoint = {};

    if (pCmd->weaponselect) return;

    F::AutoRocketJump.Run(pLocal, pWeapon, pCmd);

    if (!ShouldRun(pLocal, pWeapon)) return;

    // Auto Helper Features
    F::AutoDetonate.Run(pLocal, pWeapon, pCmd);
    F::AutoAirblast.Run(pLocal, pWeapon, pCmd);
    F::AutoHeal.Run(pLocal, pWeapon, pCmd);

    // Run Aimbot logic (Primary then Secondary capability)
    RunAimbot(pLocal, pWeapon, pCmd, false);
    RunAimbot(pLocal, pWeapon, pCmd, true);
}

// ... rest of Store/Draw functions ...
