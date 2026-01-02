#include "NavEngine.h"
#include "../../Ticks/Ticks.h"
#include "../../Misc/Misc.h"
#include "../BotUtils.h"
#include "../../FollowBot/FollowBot.h"
#include <limits>
#include <algorithm>
#include <cmath>
#include <filesystem>

// Optimized Setup Time check with caching
bool CNavEngine::IsSetupTime()
{
    static Timer tCheckTimer{};
    static bool bSetupTime = false;
    static bool bIsPayloadOrControlPoint = false;
    static bool bMapChecked = false;

    if (Vars::Misc::Movement::NavEngine::PathInSetup.Value)
        return false;

    // Only run expensive checks every second
    if (!tCheckTimer.Run(1.0f))
        return bSetupTime;

    auto pLocal = H::Entities.GetLocal();
    if (!pLocal) return false;

    // Cache map type to avoid string comparisons every second
    if (!bMapChecked)
    {
        std::string sLevelName = SDK::GetLevelName();
        // Pipeline is special case
        if (sLevelName == "plr_pipeline")
            bIsPayloadOrControlPoint = false;
        else
            bIsPayloadOrControlPoint = sLevelName.starts_with("pl_") || sLevelName.starts_with("cp_");
        
        bMapChecked = true;
    }

    if (auto pGameRules = I::TFGameRules())
    {
        if (pGameRules->m_iRoundState() == GR_STATE_PREROUND)
            return bSetupTime = true;

        if (pLocal->m_iTeamNum() == TF_TEAM_BLUE)
        {
            // Logic specific to PL and CP maps where Blue waits
            if ((pGameRules->m_bInSetup() || pGameRules->m_bInWaitingForPlayers()) && bIsPayloadOrControlPoint)
                return bSetupTime = true;
        }
        return bSetupTime = false;
    }
    return bSetupTime;
}

bool CNavEngine::IsVectorVisibleNavigation(const Vector vFrom, const Vector vTo, unsigned int nMask)
{
    CGameTrace trace = {};
    CTraceFilterNavigation filter = {};
    SDK::Trace(vFrom, vTo, nMask, &filter, &trace);
    return trace.fraction == 1.0f;
}

// Optimized passability check (No Trig functions)
bool CNavEngine::IsPlayerPassableNavigation(const Vector vFrom, Vector vTo, unsigned int nMask)
{
    CGameTrace trace = {};
    CTraceFilterNavigation filter = {};

    Vector vDelta = vTo - vFrom;
    
    // Calculate direction on the horizontal plane
    Vector vDir = vDelta;
    vDir.z = 0.f;
    vDir.NormalizeInPlace();

    // Calculate Right vector: Cross product of Up(0,0,1) and Forward(x,y,0) is (-y, x, 0)
    // This avoids AngleVectors entirely.
    Vector vRight(-vDir.y, vDir.x, 0.f);
    
    // Scale to player width
    vRight *= HALF_PLAYER_WIDTH;

    // Trace Left side
    Vector vLeftStart = vFrom - vRight;
    SDK::Trace(vLeftStart, vLeftStart + vDelta, nMask, &filter, &trace);
    if (trace.DidHit()) return false;

    // Trace Right side
    Vector vRightStart = vFrom + vRight;
    SDK::Trace(vRightStart, vRightStart + vDelta, nMask, &filter, &trace);
    return !trace.DidHit();
}

void CNavEngine::BuildIntraAreaCrumbs(const Vector& vStart, const Vector& vDestination, CNavArea* pArea)
{
    if (!pArea) return;

    Vector vDelta = vDestination - vStart;
    float flDistSq = vDelta.LengthSqr();

    // Optimization: Don't build crumbs for tiny distances
    if (flDistSq <= 1.0f) return;

    float flEffectiveDist = std::max(vDelta.Length2D(), std::fabs(vDelta.z));
    constexpr float kMaxSegmentLength = 120.f;

    // Fast ceiling division
    int nIntermediate = static_cast<int>((flEffectiveDist + kMaxSegmentLength - 1) / kMaxSegmentLength);
    nIntermediate = std::clamp(nIntermediate, 1, 8);
    
    const Vector vStep = vDelta / static_cast<float>(nIntermediate + 1);
    
    Vector vApproachDir = vDelta;
    vApproachDir.z = 0.f;
    vApproachDir.NormalizeInPlace();

    // Reserve memory to prevent realloc
    m_vCrumbs.reserve(m_vCrumbs.size() + nIntermediate);
    for (int i = 1; i <= nIntermediate; ++i)
    {
        m_vCrumbs.push_back({ pArea, vStart + vStep * static_cast<float>(i), vApproachDir });
    }
}

bool CNavEngine::NavTo(const Vector& vDestination, PriorityListEnum::PriorityListEnum ePriority, bool bShouldRepath, bool bNavToLocal)
{
    // Safety checks
    if (F::Ticks.m_bWarp || F::Ticks.m_bDoubletap || !IsReady()) return false;
    if (ePriority < m_eCurrentPriority) return false;
    if (!GetLocalNavArea()) return false;

    CNavArea* pDestArea = FindClosestNavArea(vDestination, false);
    if (!pDestArea) return false;

    // Find Path using Micropather
    auto vPath = m_pMap->FindPath(m_pLocalArea, pDestArea);
    
    bool bSingleAreaPath = (vPath.empty() && m_pLocalArea == pDestArea);
    
    // Handle Remote Paths
    if (!bSingleAreaPath && !vPath.empty() && !bNavToLocal)
    {
        vPath.erase(vPath.begin()); // Remove current area from path
        if (vPath.empty() && m_pLocalArea == pDestArea) 
            bSingleAreaPath = true;
        else if (vPath.empty())
            return false;
    }

    if (vPath.empty() && !bSingleAreaPath) return false;

    m_vCrumbs.clear();

    if (bSingleAreaPath)
    {
        Vector vStart = (m_pLocalArea ? m_pLocalArea->m_vCenter : vDestination);
        if (auto pLocal = H::Entities.GetLocal()) 
            if (pLocal->IsAlive()) vStart = pLocal->GetAbsOrigin();

        BuildIntraAreaCrumbs(vStart, vDestination, m_pLocalArea);
    }
    else
    {
        // Reserve memory
        m_vCrumbs.reserve(vPath.size() * 2);

        for (size_t i = 0; i < vPath.size(); i++)
        {
            auto pArea = reinterpret_cast<CNavArea*>(vPath[i]);
            if (!pArea) continue;

            if (i < vPath.size() - 1)
            {
                auto pNextArea = reinterpret_cast<CNavArea*>(vPath[i + 1]);

                // Check Vischeck Cache
                const std::pair<CNavArea*, CNavArea*> tKey(pArea, pNextArea);
                auto itCache = m_pMap->m_mVischeckCache.find(tKey);

                NavPoints_t tPoints;
                DropdownHint_t tDropdown;

                if (itCache != m_pMap->m_mVischeckCache.end() && itCache->second.m_bPassable)
                {
                    tPoints = itCache->second.m_tPoints;
                    tDropdown = itCache->second.m_tDropdown;
                }
                else
                {
                    // Compute new points
                    tPoints = m_pMap->DeterminePoints(pArea, pNextArea);
                    tDropdown = m_pMap->HandleDropdown(tPoints.m_vCenter, tPoints.m_vNext);
                    tPoints.m_vCenter = tDropdown.m_vAdjustedPos;
                }

                // Add points
                m_vCrumbs.push_back({ pArea, tPoints.m_vCurrent });

                Crumb_t tCenterCrumb = { pArea, tPoints.m_vCenter };
                tCenterCrumb.m_bRequiresDrop = tDropdown.m_bRequiresDrop;
                tCenterCrumb.m_flDropHeight = tDropdown.m_flDropHeight;
                tCenterCrumb.m_flApproachDistance = tDropdown.m_flApproachDistance;
                tCenterCrumb.m_vApproachDir = tDropdown.m_vApproachDir;
                m_vCrumbs.push_back(tCenterCrumb);
            }
            else
            {
                m_vCrumbs.push_back({ pArea, pArea->m_vCenter });
            }
        }
    }

    m_vCrumbs.push_back({ nullptr, vDestination });
    m_tInactivityTimer.Update();
    m_eCurrentPriority = ePriority;
    m_bCurrentNavToLocal = bNavToLocal;
    m_bRepathOnFail = bShouldRepath;
    if (m_bRepathOnFail) m_vLastDestination = vDestination;

    return true;
}

float CNavEngine::GetPathCost(const Vector& vLocalOrigin, const Vector& vDestination)
{
    if (!IsNavMeshLoaded() || !GetLocalNavArea(vLocalOrigin)) return FLT_MAX;
    
    auto pDestArea = FindClosestNavArea(vDestination, false);
    if (!pDestArea) return FLT_MAX;

    float flCost;
    std::vector<void*> vPath;
    if (m_pMap->m_pather.Solve(reinterpret_cast<void*>(m_pLocalArea), reinterpret_cast<void*>(pDestArea), &vPath, &flCost) == micropather::MicroPather::START_END_SAME)
        return 0.f;

    return flCost;
}

CNavArea* CNavEngine::GetLocalNavArea(const Vector& pLocalOrigin)
{
    // Update local area only if our origin is no longer in its bounds
    if (!m_pLocalArea || (!m_pLocalArea->IsOverlapping(pLocalOrigin) || pLocalOrigin.z < m_pLocalArea->m_flMinZ))
        m_pLocalArea = FindClosestNavArea(pLocalOrigin);
    return m_pLocalArea;
}

void CNavEngine::VischeckPath()
{
    static Timer tVischeckTimer{};
    if (m_vCrumbs.size() < 2 || !tVischeckTimer.Run(Vars::Misc::Movement::NavEngine::VischeckTime.Value))
        return;

    const auto iExpireTick = TICKCOUNT_TIMESTAMP(Vars::Misc::Movement::NavEngine::VischeckCacheTime.Value);

    for (size_t i = 0; i < m_vCrumbs.size() - 1; ++i)
    {
        auto& tCrumb = m_vCrumbs[i];
        auto& tNextCrumb = m_vCrumbs[i + 1];

        // Only check valid nav transitions
        if (!tCrumb.m_pNavArea || !tNextCrumb.m_pNavArea) continue;

        auto tKey = std::pair<CNavArea*, CNavArea*>(tCrumb.m_pNavArea, tNextCrumb.m_pNavArea);
        
        // Skip if cache is valid and visible
        auto it = m_pMap->m_mVischeckCache.find(tKey);
        if (it != m_pMap->m_mVischeckCache.end() && 
            it->second.m_eVischeckState == VischeckStateEnum::Visible && 
            it->second.m_iExpireTick > I::GlobalVars->tickcount)
            continue;

        Vector vFrom = tCrumb.m_vPos; vFrom.z += PLAYER_CROUCHED_JUMP_HEIGHT;
        Vector vTo = tNextCrumb.m_vPos; vTo.z += PLAYER_CROUCHED_JUMP_HEIGHT;

        bool bPassable = IsPlayerPassableNavigation(vFrom, vTo);

        CachedConnection_t& tEntry = m_pMap->m_mVischeckCache[tKey];
        tEntry.m_iExpireTick = iExpireTick;
        tEntry.m_bPassable = bPassable;
        tEntry.m_eVischeckState = bPassable ? VischeckStateEnum::Visible : VischeckStateEnum::NotVisible;

        if (!bPassable)
        {
            tEntry.m_flCachedCost = std::numeric_limits<float>::max();
            AbandonPath();
            break;
        }
    }
}

void CNavEngine::CheckBlacklist(CTFPlayer* pLocal)
{
    static Timer tBlacklistCheckTimer{};
    if (!tBlacklistCheckTimer.Run(0.5f)) return;

    if (pLocal->IsInvulnerable())
    {
        m_pMap->m_bFreeBlacklistBlocked = true;
        m_pMap->m_pather.Reset();
        return;
    }

    auto IsBlocked = [&](CNavArea* pArea) { return m_pMap->m_mFreeBlacklist.contains(pArea); };

    // If local player is IN a blacklisted area, temporarily ignore it so we can leave
    if (IsBlocked(m_pLocalArea))
    {
        m_pMap->m_bFreeBlacklistBlocked = true;
        m_pMap->m_pather.Reset();
        return;
    }

    m_pMap->m_bFreeBlacklistBlocked = false;

    // Check path for blacklisted nodes
    for (const auto& crumb : m_vCrumbs)
    {
        if (crumb.m_pNavArea && IsBlocked(crumb.m_pNavArea))
        {
            AbandonPath();
            return;
        }
    }
}

void CNavEngine::UpdateStuckTime(CTFPlayer* pLocal)
{
    if (m_vCrumbs.empty()) return;

    const bool bDropCrumb = m_vCrumbs[0].m_bRequiresDrop;
    float flTrigger = Vars::Misc::Movement::NavEngine::StuckTime.Value;
    if (!bDropCrumb) flTrigger *= 0.5f;

    if (m_tInactivityTimer.Check(flTrigger))
    {
        auto tKey = std::pair<CNavArea*, CNavArea*>(
            m_tLastCrumb.m_pNavArea ? m_tLastCrumb.m_pNavArea : m_vCrumbs[0].m_pNavArea, 
            m_vCrumbs[0].m_pNavArea
        );

        m_pMap->m_mConnectionStuckTime[tKey].m_iExpireTick = TICKCOUNT_TIMESTAMP(Vars::Misc::Movement::NavEngine::StuckExpireTime.Value);
        m_pMap->m_mConnectionStuckTime[tKey].m_iTimeStuck++;

        int iDetectTicks = TIME_TO_TICKS(Vars::Misc::Movement::NavEngine::StuckDetectTime.Value);
        if (bDropCrumb) iDetectTicks += TIME_TO_TICKS(Vars::Misc::Movement::NavEngine::StuckDetectTime.Value * 0.5f);

        if (m_pMap->m_mConnectionStuckTime[tKey].m_iTimeStuck > iDetectTicks)
        {
            if (Vars::Debug::Logging.Value)
                SDK::Output("CNavEngine", "Stuck detected. Blacklisting node.", { 255, 131, 131 }, OUTPUT_CONSOLE | OUTPUT_DEBUG);
            
            auto& cache = m_pMap->m_mVischeckCache[tKey];
            cache.m_iExpireTick = TICKCOUNT_TIMESTAMP(Vars::Misc::Movement::NavEngine::StuckBlacklistTime.Value);
            cache.m_eVischeckState = VischeckStateEnum::NotChecked;
            cache.m_bPassable = false;
            AbandonPath();
        }
    }
}

void CNavEngine::Reset(bool bForced)
{
    CancelPath();
    m_pLocalArea = nullptr;

    static std::string sPath = std::filesystem::current_path().string();
    std::string sLevelName = I::EngineClient->GetLevelName();
    
    if (sLevelName.empty()) return;

    if (m_pMap) m_pMap->Reset();

    if (bForced || !m_pMap || m_pMap->m_sMapName != sLevelName)
    {
        // Remove extension
        size_t lastDot = sLevelName.find_last_of('.');
        if (lastDot != std::string::npos) sLevelName.erase(lastDot);

        std::string sNavPath = std::format("{}\\tf\\{}.nav", sPath, sLevelName);
        if (Vars::Debug::Logging.Value)
            SDK::Output("NavEngine", std::format("Nav File: {}", sNavPath).c_str(), { 50, 255, 50 }, OUTPUT_CONSOLE | OUTPUT_DEBUG);
        
        m_pMap = std::make_unique<CMap>(sNavPath.c_str());
        m_vRespawnRoomExitAreas.clear();
        m_bUpdatedRespawnRooms = false;
    }
}

bool CNavEngine::IsReady(bool bRoundCheck)
{
    static Timer tRestartTimer{};
    if (!Vars::Misc::Movement::NavEngine::Enabled.Value)
    {
        tRestartTimer.Update();
        return false;
    }
    if (!tRestartTimer.Check(0.5f)) return false;
    if (!I::EngineClient->IsInGame()) return false;
    if (!m_pMap || m_pMap->m_eState != NavStateEnum::Active) return false;
    if (!bRoundCheck && IsSetupTime()) return false;

    return true;
}

bool CNavEngine::IsBlacklistIrrelevant()
{
    static bool bIrrelevant = false;
    static Timer tUpdateTimer{};
    
    if (tUpdateTimer.Run(0.5f))
    {
        int iState = GR_STATE_RND_RUNNING;
        if (auto pGameRules = I::TFGameRules())
            iState = pGameRules->m_iRoundState();

        bIrrelevant = (iState == GR_STATE_TEAM_WIN || iState == GR_STATE_STALEMATE || 
                       iState == GR_STATE_PREROUND || iState == GR_STATE_GAME_OVER);
    }
    return bIrrelevant;
}

// ... Run, AbandonPath, UpdateRespawnRooms, CancelPath (Standard Logic) ...
// Included below for completeness of file

void CNavEngine::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    static bool bWasOn = false;
    if (!Vars::Misc::Movement::NavEngine::Enabled.Value)
        bWasOn = false;
    else if (I::EngineClient->IsInGame() && !bWasOn)
    {
        bWasOn = true;
        Reset(true);
    }

    if (Vars::Misc::Movement::NavEngine::DisableOnSpectate.Value && H::Entities.IsSpectated()) return;
    if (!m_bUpdatedRespawnRooms) UpdateRespawnRooms();

    if (!pLocal->IsAlive() || F::FollowBot.m_bActive)
    {
        CancelPath();
        return;
    }

    // Logic gate for Engineer/Capture modes
    if ((m_eCurrentPriority == PriorityListEnum::Engineer && 
        ((!Vars::Aimbot::AutoEngie::AutoRepair.Value && !Vars::Aimbot::AutoEngie::AutoUpgrade.Value) || pLocal->m_iClass() != TF_CLASS_ENGINEER)) ||
        (m_eCurrentPriority == PriorityListEnum::Capture && !(Vars::Misc::Movement::NavBot::Preferences.Value & Vars::Misc::Movement::NavBot::PreferencesEnum::CaptureObjectives)))
    {
        CancelPath();
        return;
    }

    if (!pCmd || (pCmd->buttons & (IN_FORWARD | IN_BACK | IN_MOVERIGHT | IN_MOVELEFT) && !F::Misc.m_bAntiAFK) || !IsReady(true))
        return;

    if (IsSetupTime())
    {
        CancelPath();
        return;
    }

    GetLocalNavArea(pLocal->GetAbsOrigin());

    if (Vars::Misc::Movement::NavEngine::VischeckEnabled.Value && !F::Ticks.m_bWarp && !F::Ticks.m_bDoubletap)
        VischeckPath();

    if (!IsBlacklistIrrelevant())
    {
        m_pMap->UpdateIgnores(pLocal);
        CheckBlacklist(pLocal);
    }
    else if (!m_pMap->m_mFreeBlacklist.empty())
    {
        m_pMap->m_mFreeBlacklist.clear();
    }

    FollowCrumbs(pLocal, pWeapon, pCmd);
    UpdateStuckTime(pLocal);
}

void CNavEngine::AbandonPath()
{
    if (!m_pMap) return;
    m_pMap->m_pather.Reset();
    m_vCrumbs.clear();
    m_tLastCrumb.m_pNavArea = nullptr;
    
    if (m_bRepathOnFail)
        NavTo(m_vLastDestination, m_eCurrentPriority, true, m_bCurrentNavToLocal);
    else
        m_eCurrentPriority = PriorityListEnum::None;
}

void CNavEngine::UpdateRespawnRooms()
{
    if (m_vRespawnRooms.empty() || !m_pMap) return;

    // Optimization: Use set to avoid duplicates
    std::unordered_set<CNavArea*> setSpawnAreas;
    static Vector vStepHeight(0.f, 0.f, 18.f);

    for (const auto& room : m_vRespawnRooms)
    {
        for (auto& area : m_pMap->m_navfile.m_vAreas)
        {
            if (setSpawnAreas.contains(&area)) continue;

            if (room.tData.PointIsWithin(area.m_vCenter + vStepHeight) ||
                room.tData.PointIsWithin(area.m_vNwCorner + vStepHeight) ||
                room.tData.PointIsWithin(area.GetNeCorner() + vStepHeight))
            {
                setSpawnAreas.insert(&area);
                uint32_t uFlags = (room.m_iTeam == TF_TEAM_BLUE) ? TF_NAV_SPAWN_ROOM_BLUE : 
                                  (room.m_iTeam == TF_TEAM_RED ? TF_NAV_SPAWN_ROOM_RED : (TF_NAV_SPAWN_ROOM_BLUE | TF_NAV_SPAWN_ROOM_RED));
                area.m_iTFAttributeFlags |= uFlags;
            }
        }
    }

    // Mark exits
    for (auto pArea : setSpawnAreas)
    {
        for (auto& conn : pArea->m_vConnections)
        {
            if (!(conn.m_pArea->m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_RED | TF_NAV_SPAWN_ROOM_BLUE | TF_NAV_SPAWN_ROOM_EXIT)))
            {
                conn.m_pArea->m_iTFAttributeFlags |= TF_NAV_SPAWN_ROOM_EXIT;
                m_vRespawnRoomExitAreas.push_back(conn.m_pArea);
            }
        }
    }
    m_bUpdatedRespawnRooms = true;
}

void CNavEngine::CancelPath()
{
    m_vCrumbs.clear();
    m_tLastCrumb.m_pNavArea = nullptr;
    m_eCurrentPriority = PriorityListEnum::None;
}

// Check if we can jump while scoped (TF2 mechanics)
static bool CanJumpIfScoped(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
    if (pLocal->m_fFlags() & FL_INWATER) return true;
    auto id = pWeapon->GetWeaponID();
    if (id == TF_WEAPON_SNIPERRIFLE_CLASSIC) return !pWeapon->As<CTFSniperRifleClassic>()->m_bCharging();
    return !pLocal->InCond(TF_COND_ZOOMED);
}

void CNavEngine::FollowCrumbs(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    static Timer tLastJump{};
    static int iTicksSinceJump = 0;
    static bool bCrouch = false;

    // --- Helper for looking ---
    auto DoLook = [&](const Vector& vTarget, bool bValid) {
        if (G::Attacking == 1) { F::BotUtils.InvalidateLLAP(); return; }
        
        auto eLook = Vars::Misc::Movement::NavEngine::LookAtPath.Value;
        bool bSilent = (eLook == Vars::Misc::Movement::NavEngine::LookAtPathEnum::Silent || eLook == Vars::Misc::Movement::NavEngine::LookAtPathEnum::LegitSilent);
        bool bLegit = (eLook == Vars::Misc::Movement::NavEngine::LookAtPathEnum::Legit || eLook == Vars::Misc::Movement::NavEngine::LookAtPathEnum::LegitSilent);

        if (eLook == Vars::Misc::Movement::NavEngine::LookAtPathEnum::Off || (bSilent && G::AntiAim)) {
            F::BotUtils.InvalidateLLAP(); return;
        }

        if (bLegit) {
            Vec3 vT = { vTarget.x, vTarget.y, vTarget.z };
            F::BotUtils.LookLegit(pLocal, pCmd, bValid ? vT : Vec3{}, bSilent);
        } else if (bValid) {
            F::BotUtils.InvalidateLLAP();
            F::BotUtils.LookAtPath(pCmd, Vec2(vTarget.x, vTarget.y), pLocal->GetEyePosition(), bSilent);
        } else {
            F::BotUtils.InvalidateLLAP();
        }
    };

    if (m_vCrumbs.empty()) {
        m_tLastCrumb.m_pNavArea = nullptr;
        m_bRepathOnFail = false;
        m_eCurrentPriority = PriorityListEnum::None;
        DoLook(Vector{}, false);
        return;
    }

    Vector vLocalOrigin = pLocal->GetAbsOrigin();
    Vector vLocalVelocity = pLocal->GetAbsVelocity();

    // --- Height Reset Check ---
    // If we are on ground, we can snap crumbs to our Z level to prevent looking up/down excessively
    bool bResetHeight = (pLocal->m_fFlags() & FL_ONGROUND) || (pLocal->GetMoveType() == MOVETYPE_NOCLIP);
    
    // --- Processing Crumbs Loop ---
    constexpr float kDefaultReachRadius = 50.f;
    constexpr float kDropReachRadius = 28.f;

    Vector vMoveTarget = {};
    Vector vMoveDir = {};
    bool bDropCrumb = false;
    bool bHasMoveDir = false;

    // Eat crumbs that are close
    while (!m_vCrumbs.empty())
    {
        auto& tActive = m_vCrumbs.front();
        if (m_tCurrentCrumb.m_pNavArea != tActive.m_pNavArea) m_tTimeSpentOnCrumbTimer.Update();
        m_tCurrentCrumb = tActive;

        bDropCrumb = tActive.m_bRequiresDrop;
        vMoveTarget = tActive.m_vPos;

        if (bResetHeight) {
            vMoveTarget.z = vLocalOrigin.z; // Snap Z
            if (!bDropCrumb) tActive.m_vPos.z = vLocalOrigin.z;
        }

        // Calculate Move Direction
        vMoveDir = tActive.m_vApproachDir; vMoveDir.z = 0.f;
        float flDirLen = vMoveDir.Length();
        if (flDirLen < 0.01f && m_vCrumbs.size() > 1) {
            vMoveDir = m_vCrumbs[1].m_vPos - tActive.m_vPos; vMoveDir.z = 0.f;
            flDirLen = vMoveDir.Length();
        }
        bHasMoveDir = flDirLen > 0.01f;
        if (bHasMoveDir) {
            vMoveDir /= flDirLen;
            if (bDropCrumb) {
                float flPush = std::clamp(tActive.m_flApproachDistance > 0 ? tActive.m_flApproachDistance : tActive.m_flDropHeight * 0.35f, PLAYER_WIDTH * 0.6f, PLAYER_WIDTH * 2.5f);
                vMoveTarget += vMoveDir * flPush;
            }
        } else {
            vMoveDir = {};
        }

        // Check distance
        float flRadiusSqr = std::pow(bDropCrumb ? kDropReachRadius : kDefaultReachRadius, 2);
        Vector vCheck = tActive.m_vPos; vCheck.z = vLocalOrigin.z; // planar check
        
        if (vCheck.DistToSqr(vLocalOrigin) < flRadiusSqr)
        {
            m_tLastCrumb = tActive;
            m_vCrumbs.erase(m_vCrumbs.begin());
            m_tTimeSpentOnCrumbTimer.Update();
            m_tInactivityTimer.Update();
            if (m_vCrumbs.empty()) { DoLook({}, false); return; }
            continue; // Check next crumb immediately
        }

        // Drop completion logic
        if (bDropCrumb)
        {
            bool bComplete = (vCheck.z - vLocalOrigin.z) >= std::max(18.f, tActive.m_flDropHeight * 0.5f);
            if (!bComplete && m_pLocalArea && m_pLocalArea != tActive.m_pNavArea && tActive.m_flDropHeight > 18.f) bComplete = true;
            
            if (bComplete) {
                m_tLastCrumb = tActive;
                m_vCrumbs.erase(m_vCrumbs.begin());
                m_tTimeSpentOnCrumbTimer.Update();
                m_tInactivityTimer.Update();
                if (m_vCrumbs.empty()) { DoLook({}, false); return; }
                continue;
            }
        }
        break; // Found current valid crumb
    }

    // --- Stuck Handling ---
    if (!m_tTimeSpentOnCrumbTimer.Check(Vars::Misc::Movement::NavEngine::StuckDetectTime.Value)) {
        if (!vLocalVelocity.Get2D().IsZero(40.f)) m_tInactivityTimer.Update();
        else if (bDropCrumb) {
            if (bHasMoveDir) vMoveTarget += vMoveDir * (PLAYER_WIDTH * 0.75f); // Nudge
            m_tInactivityTimer.Update();
        }
    }

    // --- Jump Logic ---
    if (pWeapon)
    {
        bool bCanJump = true;
        int id = pWeapon->GetWeaponID();
        if ((id == TF_WEAPON_SNIPERRIFLE || id == TF_WEAPON_SNIPERRIFLE_CLASSIC || id == TF_WEAPON_SNIPERRIFLE_DECAP) && !CanJumpIfScoped(pLocal, pWeapon)) bCanJump = false;
        if (id == TF_WEAPON_MINIGUN && (pCmd->buttons & IN_ATTACK2)) bCanJump = false;

        if (bCanJump)
        {
            bool bShouldJump = false;
            bool bBlockJump = bDropCrumb;
            
            if (m_vCrumbs.size() > 1) {
                if (m_vCrumbs[0].m_vPos.z - m_vCrumbs[1].m_vPos.z <= -PLAYER_JUMP_HEIGHT) bBlockJump = true;
            }

            // Jump if stuck or area requires it
            if (!bBlockJump && m_pLocalArea && !(m_pLocalArea->m_iAttributeFlags & (NAV_MESH_NO_JUMP | NAV_MESH_STAIRS)) &&
                m_tInactivityTimer.Check(Vars::Misc::Movement::NavEngine::StuckTime.Value / 2))
                bShouldJump = true;

            if (bShouldJump && tLastJump.Check(0.2f)) {
                pCmd->buttons |= bCrouch ? IN_DUCK : IN_JUMP;
                if (!bCrouch) { bCrouch = true; iTicksSinceJump = 0; }
                iTicksSinceJump++;
                if (bCrouch && pLocal->OnSolid() && iTicksSinceJump > 3) {
                    bCrouch = false; tLastJump.Update();
                }
            }
        }
    }

    // Execute Move and Look
    DoLook(vMoveTarget, true);
    SDK::WalkTo(pCmd, pLocal, vMoveTarget);
}

void CNavEngine::Render()
{
    if (!Vars::Misc::Movement::NavEngine::Draw.Value || !IsReady()) return;
    auto pLocal = H::Entities.GetLocal();
    if (!pLocal || !pLocal->IsAlive()) return;

    if (Vars::Misc::Movement::NavEngine::Draw.Value & Vars::Misc::Movement::NavEngine::DrawEnum::Blacklist)
    {
        if (auto pBlacklist = GetFreeBlacklist()) {
            for (auto& [area, reason] : *pBlacklist) {
                H::Draw.RenderBox(area->m_vCenter, Vector(-4,-4,-1), Vector(4,4,1), {}, Vars::Colors::NavbotBlacklist.Value, false);
                H::Draw.RenderWireframeBox(area->m_vCenter, Vector(-4,-4,-1), Vector(4,4,1), {}, Vars::Colors::NavbotBlacklist.Value, false);
            }
        }
    }

    if ((Vars::Misc::Movement::NavEngine::Draw.Value & Vars::Misc::Movement::NavEngine::DrawEnum::Area) && GetLocalNavArea(pLocal->GetAbsOrigin()))
    {
        // Draw Current Area Box
        auto vEdge = m_pLocalArea->GetNearestPoint(Vector2D(pLocal->GetAbsOrigin().x, pLocal->GetAbsOrigin().y));
        vEdge.z += PLAYER_CROUCHED_JUMP_HEIGHT;
        H::Draw.RenderBox(vEdge, Vector(-4,-4,-1), Vector(4,4,1), {}, Color_t(255, 0, 0, 255), false);
        
        // Draw Area Bounds
        H::Draw.RenderLine(m_pLocalArea->m_vNwCorner, m_pLocalArea->GetNeCorner(), Vars::Colors::NavbotArea.Value, true);
        H::Draw.RenderLine(m_pLocalArea->m_vNwCorner, m_pLocalArea->GetSwCorner(), Vars::Colors::NavbotArea.Value, true);
        H::Draw.RenderLine(m_pLocalArea->GetNeCorner(), m_pLocalArea->m_vSeCorner, Vars::Colors::NavbotArea.Value, true);
        H::Draw.RenderLine(m_pLocalArea->GetSwCorner(), m_pLocalArea->m_vSeCorner, Vars::Colors::NavbotArea.Value, true);
    }

    if ((Vars::Misc::Movement::NavEngine::Draw.Value & Vars::Misc::Movement::NavEngine::DrawEnum::Path) && !m_vCrumbs.empty())
    {
        for (size_t i = 0; i < m_vCrumbs.size() - 1; i++)
            H::Draw.RenderLine(m_vCrumbs[i].m_vPos, m_vCrumbs[i + 1].m_vPos, Vars::Colors::NavbotPath.Value, false);
    }
}
