#include "BotUtils.h"
#include "NavEngine/NavEngine.h"
#include "../Simulation/MovementSimulation/MovementSimulation.h"
#include "../Players/PlayerUtils.h"
#include "../Misc/Misc.h"
#include "../Aimbot/AimbotGlobal/AimbotGlobal.h"
#include "../Misc/NamedPipe/NamedPipe.h"
#include "../Ticks/Ticks.h"

// Constants for clearer logic
constexpr float MAX_TRACKING_DIST = 1500.0f;
constexpr float MELEE_RANGE = 200.0f;
constexpr float FLAMETHROWER_RANGE = 550.0f;
constexpr float MEDIGUN_RANGE = 450.0f;

static bool SmoothAimHasPriority()
{
    const auto iAimType = Vars::Aimbot::General::AimType.Value;
    // If aimbot is off or set to specific modes that don't require steering override
    if (iAimType != Vars::Aimbot::General::AimTypeEnum::Smooth &&
        iAimType != Vars::Aimbot::General::AimTypeEnum::Assistive)
        return false;

    return G::AimbotSteering;
}

bool CBotUtils::HasMedigunTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
    if (!Vars::Aimbot::Healing::AutoHeal.Value || !pWeapon)
        return false;

    const Vec3 vShootPos = F::Ticks.GetShootPos();
    const float flRange = MEDIGUN_RANGE; // Use constant or pWeapon->GetRange()

    // Optimization: Don't iterate all entities, iterate teammates only
    for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerTeam))
    {
        if (pEntity->entindex() == pLocal->entindex()) continue;
        if (vShootPos.DistTo(pEntity->GetCenter()) > flRange) continue;

        auto pPlayer = pEntity->As<CTFPlayer>();
        if (pPlayer->InCond(TF_COND_STEALTHED)) continue; // Don't heal stealthed spies

        // Handle Priority Logic
        bool bIsFriend = H::Entities.IsFriend(pEntity->entindex()) || H::Entities.InParty(pEntity->entindex());
        if (Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly && !bIsFriend)
            continue;

        return true;
    }
    return false;
}

ShouldTargetEnum::ShouldTargetEnum CBotUtils::ShouldTarget(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iEntIdx)
{
    auto pEntity = I::ClientEntityList->GetClientEntity(iEntIdx)->As<CBaseEntity>();
    if (!pEntity || !pEntity->IsPlayer()) return ShouldTargetEnum::Invalid;

    auto pPlayer = pEntity->As<CTFPlayer>();
    if (!pPlayer->IsAlive() || pPlayer == pLocal) return ShouldTargetEnum::Invalid;

    // Team check
    if (pPlayer->m_iTeamNum() == pLocal->m_iTeamNum()) return ShouldTargetEnum::DontTarget;

#ifdef TEXTMODE
    // Skip local bots in textmode to prevent farming loops
    if (auto pResource = H::Entities.GetResource()) {
        if (F::NamedPipe.IsLocalBot(pResource->m_iAccountID(iEntIdx)) && 
            !(Vars::Aimbot::General::BypassIgnore.Value & Vars::Aimbot::General::BypassIgnoreEnum::LocalBots))
            return ShouldTargetEnum::DontTarget;
    }
#endif

    // --- Ignore Checks ---
    // If we are bypassing ignores, skip this block
    if (!(Vars::Aimbot::General::BypassIgnore.Value & Vars::Aimbot::General::BypassIgnoreEnum::Ignored))
    {
        if (F::PlayerUtils.IsIgnored(iEntIdx)) return ShouldTargetEnum::DontTarget;
    }

    // Consolidated Ignore Flags Logic
    int ignoreFlags = Vars::Aimbot::General::Ignore.Value;
    int bypassFlags = Vars::Aimbot::General::BypassIgnore.Value;

    bool bIsFriend = H::Entities.IsFriend(iEntIdx);
    bool bInParty = H::Entities.InParty(iEntIdx);

    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Friends) && bIsFriend && !(bypassFlags & Vars::Aimbot::General::BypassIgnoreEnum::Friends)) return ShouldTargetEnum::DontTarget;
    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Party) && bInParty && !(bypassFlags & Vars::Aimbot::General::BypassIgnoreEnum::Friends)) return ShouldTargetEnum::DontTarget;
    
    // Condition checks (Invuln, Cloak, Dead Ringer, Taunt)
    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Invulnerable) && pPlayer->IsInvulnerable() && G::SavedDefIndexes[SLOT_MELEE] != Heavy_t_TheHolidayPunch) return ShouldTargetEnum::DontTarget;
    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Taunting) && pPlayer->IsTaunting()) return ShouldTargetEnum::DontTarget;
    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Disguised) && pPlayer->InCond(TF_COND_DISGUISED)) return ShouldTargetEnum::DontTarget;
    
    // Cloak handling with threshold
    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Invisible) && pPlayer->m_flInvisibility() >= (Vars::Aimbot::General::IgnoreInvisible.Value / 100.f)) return ShouldTargetEnum::DontTarget;
    if ((ignoreFlags & Vars::Aimbot::General::IgnoreEnum::DeadRinger) && pPlayer->m_bFeignDeathReady()) return ShouldTargetEnum::DontTarget;

    // Vaccinator Logic (Refactored for readability)
    if (ignoreFlags & Vars::Aimbot::General::IgnoreEnum::Vaccinator)
    {
        bool bResistBullet = pPlayer->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST);
        bool bResistFire = pPlayer->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST);
        bool bResistExplo = pPlayer->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST);

        switch (SDK::GetWeaponType(pWeapon))
        {
        case EWeaponType::HITSCAN:
            // Check if weapon can pierce resist (e.g. Enforcer)
            if (bResistBullet && SDK::AttribHookValue(0, "mod_pierce_resists_absorbs", pWeapon) == 0) 
                return ShouldTargetEnum::DontTarget;
            break;
        case EWeaponType::PROJECTILE:
            if (bResistFire && (G::SavedWepIds[SLOT_PRIMARY] == TF_WEAPON_FLAMETHROWER)) return ShouldTargetEnum::DontTarget;
            if (bResistExplo) return ShouldTargetEnum::DontTarget; // Generalize blast resist ignoring
            if (bResistBullet && G::SavedWepIds[SLOT_PRIMARY] == TF_WEAPON_COMPOUND_BOW) return ShouldTargetEnum::DontTarget; // Bows are projectiles but deal bullet dmg type
            break;
        }
    }

    return ShouldTargetEnum::Target;
}

ShouldTargetEnum::ShouldTargetEnum CBotUtils::ShouldTargetBuilding(CTFPlayer* pLocal, int iEntIdx)
{
    if (iEntIdx <= 0) return ShouldTargetEnum::DontTarget;

    auto pEntity = I::ClientEntityList->GetClientEntity(iEntIdx)->As<CBaseEntity>();
    if (!pEntity || !pEntity->IsBuilding()) return ShouldTargetEnum::DontTarget;

    auto pBuilding = pEntity->As<CBaseObject>();
    if (pBuilding->m_iHealth() <= 0 || pBuilding->IsDormant()) return ShouldTargetEnum::DontTarget;
    if (pBuilding->m_iTeamNum() == pLocal->m_iTeamNum()) return ShouldTargetEnum::DontTarget; // Friendly building

    // Type Filters
    int targetFlags = Vars::Aimbot::General::Target.Value;
    if (!(targetFlags & Vars::Aimbot::General::TargetEnum::Sentry) && pBuilding->IsSentrygun()) return ShouldTargetEnum::DontTarget;
    if (!(targetFlags & Vars::Aimbot::General::TargetEnum::Dispenser) && pBuilding->IsDispenser()) return ShouldTargetEnum::DontTarget;
    if (!(targetFlags & Vars::Aimbot::General::TargetEnum::Teleporter) && pBuilding->IsTeleporter()) return ShouldTargetEnum::DontTarget;

    // Owner Checks (Ignore friends' buildings)
    auto pOwner = pBuilding->m_hBuilder().Get();
    if (pOwner)
    {
        if (F::PlayerUtils.IsIgnored(pOwner->entindex()) && !(Vars::Aimbot::General::BypassIgnore.Value & Vars::Aimbot::General::BypassIgnoreEnum::Ignored))
            return ShouldTargetEnum::DontTarget;

        if (H::Entities.IsFriend(pOwner->entindex()) || H::Entities.InParty(pOwner->entindex()))
             if (!(Vars::Aimbot::General::BypassIgnore.Value & Vars::Aimbot::General::BypassIgnoreEnum::Friends))
                return ShouldTargetEnum::DontTarget;
    }

    return ShouldTargetEnum::Target;
}

ClosestEnemy_t CBotUtils::UpdateCloseEnemies(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
    m_vCloseEnemies.clear();
    const Vector vLocalOrigin = pLocal->GetAbsOrigin();

    // Loop Players
    for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
    {
        if (ShouldTarget(pLocal, pWeapon, pEntity->entindex()) == ShouldTargetEnum::DontTarget) continue;

        Vector vOrigin = pEntity->GetAbsOrigin();
        // Handle dormancy positions
        if (pEntity->IsDormant())
        {
            if (!H::Entities.GetDormancy(pEntity->entindex())) continue; // Not seen recently
            // vOrigin is already updated by GetDormancy logic internally usually, or use cache
        }

        float dist = vLocalOrigin.DistTo(vOrigin);
        m_vCloseEnemies.emplace_back(pEntity->entindex(), pEntity->As<CTFPlayer>(), dist);
    }

    // Sort by distance (ASC)
    std::sort(m_vCloseEnemies.begin(), m_vCloseEnemies.end(), [](const ClosestEnemy_t& a, const ClosestEnemy_t& b) {
        return a.m_flDist < b.m_flDist;
    });

    if (m_vCloseEnemies.empty()) return {};
    return m_vCloseEnemies.front();
}

void CBotUtils::UpdateBestSlot(CTFPlayer* pLocal)
{
    // If disabled or manual override, exit
    if (!Vars::Misc::Movement::BotUtils::WeaponSlot.Value) { m_iBestSlot = -1; return; }
    
    // If set to specific slot (Primary/Secondary/Melee)
    if (Vars::Misc::Movement::BotUtils::WeaponSlot.Value != Vars::Misc::Movement::BotUtils::WeaponSlotEnum::Best) {
        m_iBestSlot = Vars::Misc::Movement::BotUtils::WeaponSlot.Value - 2;
        return;
    }

    // "Best" Logic
    int iClass = pLocal->m_iClass();
    float flDist = m_tClosestEnemy.m_flDist;
    bool bHasEnemy = m_tClosestEnemy.m_pPlayer != nullptr;

    // Helper to check ammo
    auto HasAmmo = [&](int slot) -> bool {
        if (!G::AmmoInSlot[slot].m_bUsesAmmo) return true; // Melee or boots
        return G::AmmoInSlot[slot].m_iClip > 0 || G::AmmoInSlot[slot].m_iReserve > 0;
    };
    
    auto HasClip = [&](int slot) -> bool {
        return !G::AmmoInSlot[slot].m_bUsesAmmo || G::AmmoInSlot[slot].m_iClip > 0;
    };

    m_iBestSlot = SLOT_PRIMARY; // Default

    switch (iClass)
    {
    case TF_CLASS_SCOUT:
        if (!HasClip(SLOT_PRIMARY) && flDist < MELEE_RANGE) m_iBestSlot = SLOT_MELEE;
        else if (!HasClip(SLOT_PRIMARY) && HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY;
        else if (flDist > 800.f && HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY; // Pistol for range
        break;

    case TF_CLASS_HEAVY:
        if (!HasClip(SLOT_PRIMARY))
        {
            if (HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY;
            else m_iBestSlot = SLOT_MELEE;
        }
        else if (bHasEnemy && flDist < 150.f) m_iBestSlot = SLOT_MELEE; // Fists often better point blank if gun not revved
        break;

    case TF_CLASS_PYRO:
        if (!HasClip(SLOT_PRIMARY) && HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY;
        else if (bHasEnemy && flDist > FLAMETHROWER_RANGE && HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY; // Shotgun/Flare for range
        break;

    case TF_CLASS_MEDIC:
    {
        // Prioritize healing if teammates need it
        auto pSec = pLocal->GetWeaponFromSlot(SLOT_SECONDARY);
        if (pSec && (pSec->As<CWeaponMedigun>()->m_hHealingTarget() || HasMedigunTargets(pLocal, pSec)))
            m_iBestSlot = SLOT_SECONDARY;
        else if (bHasEnemy && flDist < MELEE_RANGE)
            m_iBestSlot = SLOT_MELEE; // Ubersaw farming
        else
            m_iBestSlot = SLOT_PRIMARY; // Crossbow/Syringe
        break;
    }

    case TF_CLASS_SNIPER:
    {
        // SMG Logic: If enemy is close or primary empty
        bool bLowHP = pLocal->m_iHealth() < 50;
        if (flDist < 300.f && HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY;
        else if (!HasClip(SLOT_PRIMARY) && HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_SECONDARY;
        else if (flDist < MELEE_RANGE && !HasClip(SLOT_SECONDARY)) m_iBestSlot = SLOT_MELEE;
        break;
    }

    case TF_CLASS_SOLDIER:
    {
        // Anti-Pyro logic
        bool bEnemyIsPyro = bHasEnemy && m_tClosestEnemy.m_pPlayer->m_iClass() == TF_CLASS_PYRO;
        if (bEnemyIsPyro && flDist < 500.f && HasClip(SLOT_SECONDARY)) 
            m_iBestSlot = SLOT_SECONDARY; // Use Shotgun vs Pyro to avoid reflect
        else if (!HasClip(SLOT_PRIMARY) && HasClip(SLOT_SECONDARY))
            m_iBestSlot = SLOT_SECONDARY;
        else if (!HasClip(SLOT_PRIMARY) && !HasClip(SLOT_SECONDARY))
            m_iBestSlot = SLOT_MELEE;
        break;
    }
    
    // ... Implement other classes similarly ...
    default:
        // Generic fallback
        if (!HasClip(SLOT_PRIMARY)) m_iBestSlot = SLOT_SECONDARY;
        if (!HasClip(SLOT_SECONDARY) && !HasClip(SLOT_PRIMARY)) m_iBestSlot = SLOT_MELEE;
        break;
    }
}

void CBotUtils::SetSlot(CTFPlayer* pLocal, int iSlot)
{
    if (iSlot > -1 && m_iCurrentSlot != iSlot)
    {
        std::string sCommand = std::format("slot{}", iSlot + 1);
        I::EngineClient->ClientCmd_Unrestricted(sCommand.c_str());
        m_iCurrentSlot = iSlot; // Optimistic update
    }
}

// --- Movement & Aim Utils ---

void CBotUtils::DoSlowAim(Vec3& vWishAngles, float flSpeed, Vec3 vPreviousAngles)
{
    if (vPreviousAngles == vWishAngles) return;

    // Calculate delta and normalize
    Vec3 vDelta = vWishAngles - vPreviousAngles;
    Math::ClampAngles(vDelta);

    // Apply smoothing factor
    // Higher flSpeed = slower movement (conceptually 1/Speed)
    // If flSpeed is 0, instant snap (avoid div by 0)
    if (flSpeed <= 0.1f) return; 

    vDelta /= flSpeed;
    vWishAngles = vPreviousAngles + vDelta;
    Math::ClampAngles(vWishAngles);
}

// 
// This function creates a "human-like" idle movement using sine wave superposition
void CBotUtils::LookLegit(CTFPlayer* pLocal, CUserCmd* pCmd, const Vec3& vDest, bool bSilent)
{
    if (!pLocal || SmoothAimHasPriority()) {
        m_vLastAngles = I::EngineClient->GetViewAngles();
        // Reset legit state anchor to current to prevent snapping when toggling
        m_tLLAP.m_vAnchor = m_vLastAngles; 
        return;
    }

    Vec3 vEye = pLocal->GetEyePosition();
    Vec3 vLookTarget = vDest;
    bool bFocusingEnemy = false;

    // 1. Identify best thing to look at (Enemy > Building > Path)
    // ... (Refined logic from original, but simplified) ...
    // NOTE: In a real implementation, you might want to separate "GetBestViewTarget" into its own helper.
    
    // (Preserving original logic flow for brevity but cleaning it up)
    CBaseEntity* pBestEntity = nullptr;
    float flBestDist = FLT_MAX;
    
    // Optimized entity search (reuse m_vCloseEnemies if available?)
    // For now, fast iteration:
    for (const auto& enemy : m_vCloseEnemies) {
        if (!enemy.m_pPlayer) continue;
        if (SDK::VisPos(pLocal, enemy.m_pPlayer, vEye, enemy.m_pPlayer->GetEyePosition())) {
            pBestEntity = enemy.m_pPlayer;
            flBestDist = enemy.m_flDist;
            bFocusingEnemy = true;
            break; // Found closest visible
        }
    }

    if (pBestEntity) {
        vLookTarget = pBestEntity->As<CTFPlayer>()->GetEyePosition() - Vec3(0,0,10.f); // Look at chest
    } else {
        // Look at movement direction if no enemy
        if (pLocal->m_vecVelocity().Length2D() > 10.f) {
             Vec3 vVel = pLocal->m_vecVelocity();
             vVel.Normalize();
             vLookTarget = vEye + (vVel * 500.f); // Look ahead
             vLookTarget.z = vEye.z; // Keep eye level mostly
        }
    }

    // 2. Calculate Desired Angle
    Vec3 vDesiredAngles = Math::CalcAngle(vEye, vLookTarget);
    Math::ClampAngles(vDesiredAngles);

    // 3. Apply "Human" Jitter/Sway (The LLAP Logic)
    auto& state = m_tLLAP;
    if (!state.m_bInitialized) {
        state = {}; // Reset
        state.m_bInitialized = true;
        state.m_vAnchor = I::EngineClient->GetViewAngles();
    }

    // Smoothly drag the "Anchor" towards the desired target
    // This simulates the eyes/head tracking the main subject
    float flTrackingSpeed = bFocusingEnemy ? 0.25f : 0.1f;
    state.m_vAnchor = state.m_vAnchor.LerpAngle(vDesiredAngles, flTrackingSpeed);

    // Apply Procedural Noise (Breathing + Micro-adjustments)
    // Using simple sin waves for breathing, Perlin-like noise for hands
    float time = I::GlobalVars->curtime;
    
    // Breathing (Pitch only mostly)
    float flBreath = std::sin(time * 2.5f) * 0.5f;
    
    // Micro-jitter (Hand unsteadiness)
    float flJitterX = std::sin(time * 11.0f) * 0.2f;
    float flJitterY = std::cos(time * 13.0f) * 0.2f;

    Vec3 vFinalAngles = state.m_vAnchor;
    vFinalAngles.x += flBreath + flJitterX;
    vFinalAngles.y += flJitterY;

    // Apply SlowAim smoothing to the final result to remove harsh ticks
    float flSmoothFactor = std::max(1.f, (float)Vars::Misc::Movement::BotUtils::LookAtPathSpeed.Value);
    DoSlowAim(vFinalAngles, flSmoothFactor, m_vLastAngles);

    // 4. Set Angles
    if (bSilent) pCmd->viewangles = vFinalAngles;
    else I::EngineClient->SetViewAngles(vFinalAngles);
    
    m_vLastAngles = vFinalAngles;
}


// 
// This function predicts where an enemy will be and checks visibility
void CBotUtils::AutoScope(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    // Feature gate
    if (!Vars::Misc::Movement::BotUtils::AutoScope.Value) { m_mAutoScopeCache.clear(); return; }
    
    bool bIsSniperRifle = pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE || pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_DECAP || pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_CLASSIC;
    if (!bIsSniperRifle) return;

    // Clean cache periodically
    static int iTickCount = 0;
    if (iTickCount++ > 66) { m_mAutoScopeCache.clear(); iTickCount = 0; }

    Vector vLocalEye = pLocal->GetEyePosition();
    bool bSimplePred = Vars::Misc::Movement::BotUtils::AutoScope.Value == Vars::Misc::Movement::BotUtils::AutoScopeEnum::Simple;

    // Helper: Perform the trace
    auto IsVisible = [&](const Vector& vTarget, int iEntIndex) -> bool {
        // Check cache first
        if (Vars::Misc::Movement::BotUtils::AutoScopeUseCachedResults.Value && m_mAutoScopeCache.contains(iEntIndex))
            return m_mAutoScopeCache[iEntIndex];

        CGameTrace trace;
        CTraceFilterWorldAndPropsOnly filter; // Optimization: Don't trace against other players, just world
        SDK::Trace(vLocalEye, vTarget, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
        
        bool visible = (trace.fraction > 0.99f || trace.m_pEnt == I::ClientEntityList->GetClientEntity(iEntIndex));
        m_mAutoScopeCache[iEntIndex] = visible;
        return visible;
    };

    // Iterate sorted enemies (closest first)
    for (const auto& enemy : m_vCloseEnemies)
    {
        if (!enemy.m_pPlayer) continue;
        if (enemy.m_flDist > 3000.f) continue; // Don't try to scope across the map unnecessarily

        Vector vEnemyPos = enemy.m_pPlayer->GetAbsOrigin();
        Vector vPredictedPos = vEnemyPos;

        // Prediction Logic
        if (bSimplePred) {
            vPredictedPos += enemy.m_pPlayer->m_vecVelocity() * 0.2f; // Simple linear extrapolation (200ms)
        } else {
            // Full MoveSim (Expensive, use sparingly)
            MoveStorage storage;
            if (F::MoveSim.Initialize(enemy.m_pPlayer, storage, false)) {
                F::MoveSim.RunTick(storage); // Run 1 tick? Or more? 
                // Running too many ticks is heavy. Usually ~10 ticks (150ms) is enough for reaction time
                for(int i=0; i<10; i++) F::MoveSim.RunTick(storage);
                vPredictedPos = storage.m_vPredictedOrigin;
                F::MoveSim.Restore(storage);
            }
        }

        // Check Head Height (roughly 75 units up)
        vPredictedPos.z += 70.0f; 

        if (IsVisible(vPredictedPos, enemy.m_iEntIndex))
        {
            // Scope in!
            if (!pLocal->InCond(TF_COND_ZOOMED))
                pCmd->buttons |= IN_ATTACK2;
            
            // Wait logic can be added here (wait for full charge etc.)
            return; // Found a target, stop processing
        }
    }
}

void CBotUtils::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    // Master Switch
    if ((!Vars::Misc::Movement::NavBot::Enabled.Value && 
        !(Vars::Misc::Movement::FollowBot::Enabled.Value && Vars::Misc::Movement::FollowBot::Targets.Value)) ||
        !pLocal->IsAlive() || !pWeapon)
    {
        Reset();
        return;
    }

    // 1. Update World State (Enemies)
    // We update this every tick, but inside it uses lightweight distance checks
    m_tClosestEnemy = UpdateCloseEnemies(pLocal, pWeapon);
    
    // 2. Weapon Slot Logic
    // Run this less frequently to prevent "jittery" weapon switching? 
    // Currently runs every tick, which is fine if logic is stable.
    m_iCurrentSlot = pWeapon->GetSlot();
    UpdateBestSlot(pLocal);

    // 3. Auto Scope
    AutoScope(pLocal, pWeapon, pCmd);

    // 4. Minigun Auto-Rev
    if (pWeapon->GetWeaponID() == TF_WEAPON_MINIGUN)
    {
        bool bShouldRev = false;
        if (m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_flDist < 900.f) {
             // Only rev if we have line of sight?
             if (SDK::VisPos(pLocal, m_tClosestEnemy.m_pPlayer, pLocal->GetEyePosition(), m_tClosestEnemy.m_pPlayer->GetEyePosition()))
                 bShouldRev = true;
        }

        if (bShouldRev && pWeapon->HasAmmo())
            pCmd->buttons |= IN_ATTACK2;
    }
}

void CBotUtils::Reset()
{
    m_mAutoScopeCache.clear();
    m_vCloseEnemies.clear();
    m_tClosestEnemy = {};
    m_iBestSlot = -1;
    m_iCurrentSlot = -1;
    InvalidateLLAP();
}
