#include "NavEngine.h"
#include "../BotUtils.h"
#include <cmath>
#include <algorithm>
#include <limits>

// Helper to check if a cache entry is valid
static bool IsCacheValid(const CachedConnection_t& entry, int iTick)
{
    // If expire tick is 0 (permanent) or in future
    return (entry.m_iExpireTick == 0 || entry.m_iExpireTick > iTick);
}

void CMap::AdjacentCost(void* pArea, std::vector<micropather::StateCost>* pAdjacent)
{
    if (!pArea) return;

    CNavArea* pCurrentArea = reinterpret_cast<CNavArea*>(pArea);
    const int iNow = I::GlobalVars->tickcount;
    const int iCacheExpiry = TICKCOUNT_TIMESTAMP(Vars::Misc::Movement::NavEngine::VischeckCacheTime.Value);

    // Reserve space to prevent reallocation
    pAdjacent->reserve(pCurrentArea->m_vConnections.size());

    for (const NavConnect_t& tConnection : pCurrentArea->m_vConnections)
    {
        CNavArea* pNextArea = tConnection.m_pArea;
        if (!pNextArea || pNextArea == pCurrentArea) continue;

        // 1. Check Node Blockage (Cached Vischeck)
        // This checks if the DESTINATION area itself is marked as "bad" generally
        const auto tAreaBlockKey = std::pair<CNavArea*, CNavArea*>(pNextArea, pNextArea);
        if (auto itBlocked = m_mVischeckCache.find(tAreaBlockKey); itBlocked != m_mVischeckCache.end())
        {
            if (itBlocked->second.m_eVischeckState == VischeckStateEnum::NotVisible && IsCacheValid(itBlocked->second, iNow))
                continue;
        }

        // 2. Blacklist Check (Soft Block)
        float flBlacklistPenalty = 0.f;
        if (!m_bFreeBlacklistBlocked)
        {
            if (auto itBlacklist = m_mFreeBlacklist.find(pNextArea); itBlacklist != m_mFreeBlacklist.end())
            {
                flBlacklistPenalty = GetBlacklistPenalty(itBlacklist->second);
                // If infinite penalty (e.g. Sentry), skip immediately
                if (flBlacklistPenalty >= std::numeric_limits<float>::max())
                    continue;
            }
        }

        // 3. Check Connection Cache (Edge Check)
        const auto tKey = std::pair<CNavArea*, CNavArea*>(pCurrentArea, pNextArea);
        auto itCache = m_mVischeckCache.find(tKey);
        
        CachedConnection_t* pCachedEntry = nullptr;
        if (itCache != m_mVischeckCache.end() && IsCacheValid(itCache->second, iNow))
            pCachedEntry = &itCache->second;

        // If explicitly marked as impassable recently, skip
        if (pCachedEntry && !pCachedEntry->m_bPassable)
            continue;

        NavPoints_t tPoints{};
        DropdownHint_t tDropdown{};
        float flBaseCost = std::numeric_limits<float>::max();
        bool bPassable = false;

        // 4. Determine Points & Passability
        if (pCachedEntry && pCachedEntry->m_eVischeckState == VischeckStateEnum::Visible)
        {
            // Cache Hit
            tPoints = pCachedEntry->m_tPoints;
            tDropdown = pCachedEntry->m_tDropdown;
            flBaseCost = pCachedEntry->m_flCachedCost;
            bPassable = true;
        }
        else
        {
            // Cache Miss: Perform Expensive Calculation
            tPoints = DeterminePoints(pCurrentArea, pNextArea);
            tDropdown = HandleDropdown(tPoints.m_vCenter, tPoints.m_vNext);
            tPoints.m_vCenter = tDropdown.m_vAdjustedPos;

            // Height Check (Did we calculate a path that requires jumping too high?)
            if ((tPoints.m_vCenterNext.z - tPoints.m_vCenter.z) > PLAYER_CROUCHED_JUMP_HEIGHT)
            {
                // Mark bad
                auto& tEntry = m_mVischeckCache[tKey];
                tEntry.m_iExpireTick = iCacheExpiry;
                tEntry.m_eVischeckState = VischeckStateEnum::NotVisible;
                tEntry.m_bPassable = false;
                tEntry.m_flCachedCost = std::numeric_limits<float>::max();
                continue;
            }

            // Visibility / Trace Check
            Vector vStart = tPoints.m_vCurrent; vStart.z += PLAYER_CROUCHED_JUMP_HEIGHT;
            Vector vMid = tPoints.m_vCenter;    vMid.z += PLAYER_CROUCHED_JUMP_HEIGHT;
            Vector vEnd = tPoints.m_vNext;      vEnd.z += PLAYER_CROUCHED_JUMP_HEIGHT;

            if (F::NavEngine.IsPlayerPassableNavigation(vStart, vMid) && F::NavEngine.IsPlayerPassableNavigation(vMid, vEnd))
            {
                bPassable = true;
                flBaseCost = EvaluateConnectionCost(pCurrentArea, pNextArea, tPoints, tDropdown);

                // Update Cache (Good)
                auto& tEntry = m_mVischeckCache[tKey];
                tEntry.m_iExpireTick = iCacheExpiry;
                tEntry.m_eVischeckState = VischeckStateEnum::Visible;
                tEntry.m_bPassable = true;
                tEntry.m_tPoints = tPoints;
                tEntry.m_tDropdown = tDropdown;
                tEntry.m_flCachedCost = flBaseCost;
            }
            else
            {
                // Update Cache (Bad)
                auto& tEntry = m_mVischeckCache[tKey];
                tEntry.m_iExpireTick = iCacheExpiry;
                tEntry.m_eVischeckState = VischeckStateEnum::NotVisible;
                tEntry.m_bPassable = false;
                tEntry.m_flCachedCost = std::numeric_limits<float>::max();
                continue;
            }
        }

        if (!bPassable) continue;

        // Recalculate dynamic cost if needed (usually cached cost is static geometry cost)
        if (flBaseCost >= std::numeric_limits<float>::max() || flBaseCost <= 0.f)
        {
             flBaseCost = EvaluateConnectionCost(pCurrentArea, pNextArea, tPoints, tDropdown);
             if (pCachedEntry) pCachedEntry->m_flCachedCost = flBaseCost;
        }

        float flFinalCost = flBaseCost;

        // Apply Blacklist Penalty
        if (flBlacklistPenalty > 0.f)
            flFinalCost += flBlacklistPenalty;

        // Apply Stuck Penalty
        if (auto itStuck = m_mConnectionStuckTime.find(tKey); itStuck != m_mConnectionStuckTime.end())
        {
            if (IsCacheValid(itStuck->second, iNow))
            {
                // Scale penalty by stuck time
                flFinalCost += std::clamp(static_cast<float>(itStuck->second.m_iTimeStuck) * 35.f, 25.f, 400.f);
            }
        }

        // Final validity check
        if (flFinalCost > 0.f && flFinalCost < std::numeric_limits<float>::max())
        {
            pAdjacent->push_back({ reinterpret_cast<void*>(pNextArea), flFinalCost });
        }
    }
}

DropdownHint_t CMap::HandleDropdown(const Vector& vCurrentPos, const Vector& vNextPos)
{
    DropdownHint_t tHint{};
    tHint.m_vAdjustedPos = vCurrentPos;

    Vector vToTarget = vNextPos - vCurrentPos;
    const float flHeightDiff = vToTarget.z;
    
    // 2D distance
    const float flHorizontalLength = vToTarget.Length2D();

    constexpr float kSmallDropGrace = 18.f;
    constexpr float kEdgePadding = 8.f;

    // Is it a drop?
    if (flHeightDiff < 0.f)
    {
        const float flDropDistance = -flHeightDiff;
        if (flDropDistance > kSmallDropGrace && flHorizontalLength > 1.f)
        {
            // Normalize 2D direction
            Vector vDirection = vToTarget; vDirection.z = 0.f; vDirection.NormalizeInPlace();

            const float desiredAdvance = std::clamp(flDropDistance * 0.4f, PLAYER_WIDTH * 0.75f, PLAYER_WIDTH * 2.5f);
            const float flMaxAdvance = std::max(flHorizontalLength - kEdgePadding, 0.f);
            
            float flApproach = (flMaxAdvance > 0.f) ? std::min(desiredAdvance, flMaxAdvance) : (flHorizontalLength * 0.8f);
            
            // Safety clamp
            const float minAdvance = std::min(flHorizontalLength * 0.95f, std::max(PLAYER_WIDTH * 0.6f, flHorizontalLength * 0.5f));
            flApproach = std::clamp(flApproach, minAdvance, flHorizontalLength * 0.95f);

            tHint.m_flApproachDistance = std::max(flApproach, 0.f);
            tHint.m_vAdjustedPos = vCurrentPos + vDirection * tHint.m_flApproachDistance;
            tHint.m_vAdjustedPos.z = vCurrentPos.z; // Keep Z flat
            tHint.m_bRequiresDrop = true;
            tHint.m_flDropHeight = flDropDistance;
            tHint.m_vApproachDir = vDirection;
        }
    }
    // Is it a climb?
    else if (flHeightDiff > 0.f && flHorizontalLength > 1.f)
    {
        Vector vDirection = vToTarget; vDirection.z = 0.f; vDirection.NormalizeInPlace();
        
        // Retreat slightly to get a running start/clearance
        const float retreat = std::clamp(flHeightDiff * 0.35f, PLAYER_WIDTH * 0.3f, PLAYER_WIDTH);
        
        tHint.m_vAdjustedPos = vCurrentPos - vDirection * retreat;
        tHint.m_vAdjustedPos.z = vCurrentPos.z;
        tHint.m_vApproachDir = -vDirection;
        tHint.m_flApproachDistance = retreat;
    }

    return tHint;
}

NavPoints_t CMap::DeterminePoints(CNavArea* pCurrentArea, CNavArea* pNextArea)
{
    const Vector vCurrentCenter = pCurrentArea->m_vCenter;
    const Vector vNextCenter = pNextArea->m_vCenter;

    // Get closest point on edges
    Vector vCurrentClosest = pCurrentArea->GetNearestPoint(Vector2D(vNextCenter.x, vNextCenter.y));
    Vector vNextClosest = pNextArea->GetNearestPoint(Vector2D(vCurrentCenter.x, vCurrentCenter.y));

    // Determine connection point
    Vector vClosest = vCurrentClosest;

    // If points are misaligned (corners/complex shapes), check inverse
    if (vClosest.x != vCurrentCenter.x && vClosest.y != vCurrentCenter.y &&
        vClosest.x != vNextCenter.x && vClosest.y != vNextCenter.y)
    {
        vClosest = vNextClosest;
        // Fix Z to current area
        vClosest.z = pCurrentArea->GetNearestPoint(Vector2D(vNextClosest.x, vNextClosest.y)).z;
    }

    // Safe Pathing Logic: Nudge points away from corners
    if (Vars::Misc::Movement::NavEngine::SafePathing.Value)
    {
        // Weighted average favoring current area
        Vector vSafeTarget = vCurrentCenter + (vNextCenter - vCurrentCenter) * 0.4f;
        vClosest = vSafeTarget;

        constexpr float flCornerMargin = PLAYER_WIDTH * 0.75f;
        
        // Clamp logic to ensure we are inside current area but away from edges
        // (Simplified logic compared to original for performance/robustness)
        Vector vMins = pCurrentArea->m_vNwCorner;
        Vector vMaxs = pCurrentArea->m_vSeCorner;

        // Ensure we respect area bounds
        vClosest.x = std::clamp(vClosest.x, vMins.x + flCornerMargin, vMaxs.x - flCornerMargin);
        vClosest.y = std::clamp(vClosest.y, vMins.y + flCornerMargin, vMaxs.y - flCornerMargin);
        
        // Re-snap to nearest valid point if our math pushed us out
        vClosest = pCurrentArea->GetNearestPoint(Vector2D(vClosest.x, vClosest.y));
    }

    // Determine target on next area
    Vector vCenterNext = pNextArea->GetNearestPoint(Vector2D(vClosest.x, vClosest.y));

    return NavPoints_t(vCurrentCenter, vClosest, vCenterNext, vNextCenter);
}

float CMap::EvaluateConnectionCost(CNavArea* pCurrentArea, CNavArea* pNextArea, const NavPoints_t& tPoints, const DropdownHint_t& tDropdown) const
{
    // Fast 2D distance
    auto Dist2D = [](const Vector& a, const Vector& b) { return (a - b).Length2D(); };

    float flForwardDistance = std::max(Dist2D(tPoints.m_vCurrent, tPoints.m_vNext), 1.f);
    float flCost = flForwardDistance;

    // Add penalty for deviating from straight line (zigzagging)
    flCost += Dist2D(tPoints.m_vCurrent, tPoints.m_vCenter) * 0.3f;
    flCost += Dist2D(tPoints.m_vCenter, tPoints.m_vNext) * 0.2f;

    // Height Penalties
    float flHeightDiff = tPoints.m_vNext.z - tPoints.m_vCurrent.z;
    if (flHeightDiff > 0.f)
        flCost += flHeightDiff * 1.8f; // Climbing is hard
    else if (flHeightDiff < -8.f)
        flCost += std::abs(flHeightDiff) * 0.9f; // Falling is risky

    // Drop Penalties
    if (tDropdown.m_bRequiresDrop) {
        flCost += tDropdown.m_flDropHeight * 2.2f;
        flCost += tDropdown.m_flApproachDistance * 0.45f;
    } else if (tDropdown.m_flApproachDistance > 0.f) {
        flCost += tDropdown.m_flApproachDistance * 0.25f;
    }

    // Turn Angle Penalty
    Vector vDir1 = tPoints.m_vCenter - tPoints.m_vCurrent; vDir1.z = 0;
    Vector vDir2 = tPoints.m_vNext - tPoints.m_vCenter;    vDir2.z = 0;
    
    // Normalize safely
    float l1 = vDir1.Length(), l2 = vDir2.Length();
    if (l1 > 1.f && l2 > 1.f) {
        float flDot = (vDir1 / l1).Dot(vDir2 / l2);
        flCost += (1.f - flDot) * 30.f; // Penalty for sharp turns
    }

    // Favor larger areas (less cramped)
    Vector vSize = pNextArea->m_vSeCorner - pNextArea->m_vNwCorner;
    float flAreaMetric = vSize.Length2D();
    if (flAreaMetric > 0.f) flCost -= std::clamp(flAreaMetric * 0.01f, 0.f, 12.f);

    // Avoid Spawn Rooms
    if (pNextArea->m_iTFAttributeFlags & (TF_NAV_SPAWN_ROOM_BLUE | TF_NAV_SPAWN_ROOM_RED))
        flCost += 900.f;

    return std::max(flCost, 1.f);
}

float CMap::GetBlacklistPenalty(const BlacklistReason_t& tReason) const
{
    switch (tReason.m_eValue)
    {
    case BlacklistReasonEnum::Sentry:       return std::numeric_limits<float>::max();
    case BlacklistReasonEnum::EnemyInvuln:  return 600.f;
    case BlacklistReasonEnum::Sticky:       return 350.f;
    case BlacklistReasonEnum::SentryMedium: return 220.f;
    case BlacklistReasonEnum::SentryLow:    return 120.f;
    case BlacklistReasonEnum::EnemyDormant: return 90.f;
    case BlacklistReasonEnum::EnemyNormal:  return 70.f;
    case BlacklistReasonEnum::BadBuildSpot: return 60.f;
    default: return 0.f;
    }
}

bool CMap::ShouldOverrideBlacklist(const BlacklistReason_t& tCurrent, const BlacklistReason_t& tIncoming) const
{
    if (tIncoming.m_eValue == tCurrent.m_eValue) return true;
    return GetBlacklistPenalty(tIncoming) >= GetBlacklistPenalty(tCurrent);
}

void CMap::CollectAreasAround(const Vector& vOrigin, float flRadius, std::vector<CNavArea*>& vOutAreas)
{
    vOutAreas.clear();
    CNavArea* pSeed = FindClosestNavArea(vOrigin, false);
    if (!pSeed) return;

    const float flRadSq = flRadius * flRadius;
    const float flLimitSq = flRadSq * 4.f;

    std::queue<CNavArea*> q;
    std::unordered_set<int> visited; // Use ID for faster hashing than ptr

    q.push(pSeed);
    visited.insert(pSeed->m_nID);

    while(!q.empty())
    {
        CNavArea* cur = q.front(); q.pop();

        float distSq = (cur->m_vCenter - vOrigin).LengthSqr();
        if (distSq <= flRadSq) vOutAreas.push_back(cur);
        if (distSq > flLimitSq) continue;

        for (auto& conn : cur->m_vConnections) {
            if (visited.find(conn.m_pArea->m_nID) == visited.end()) {
                visited.insert(conn.m_pArea->m_nID);
                q.push(conn.m_pArea);
            }
        }
    }
    if (vOutAreas.empty()) vOutAreas.push_back(pSeed);
}

void CMap::ApplyBlacklistAround(const Vector& vOrigin, float flRadius, const BlacklistReason_t& tReason, unsigned int nMask, bool bRequireLOS)
{
    std::vector<CNavArea*> vCandidates;
    CollectAreasAround(vOrigin, flRadius + HALF_PLAYER_WIDTH, vCandidates);
    if (vCandidates.empty()) return;

    float flRadSq = flRadius * flRadius;

    for (auto* pArea : vCandidates)
    {
        Vector vCheck = pArea->m_vCenter; vCheck.z += PLAYER_CROUCHED_JUMP_HEIGHT;
        
        if (vOrigin.DistToSqr(vCheck) > flRadSq) continue;
        if (bRequireLOS && !F::NavEngine.IsVectorVisibleNavigation(vOrigin, vCheck, nMask)) continue;

        // Check if existing blacklist is stronger
        if (m_mFreeBlacklist.count(pArea)) {
            if (!ShouldOverrideBlacklist(m_mFreeBlacklist[pArea], tReason)) continue;
        }
        
        m_mFreeBlacklist[pArea] = tReason;
    }
}

// Optimized Search
CNavArea* CMap::FindClosestNavArea(const Vector& vPos, bool bLocalOrigin)
{
    float flBestDist = FLT_MAX;
    CNavArea* pBestArea = nullptr;
    
    Vector vCorrected = vPos; vCorrected.z += PLAYER_CROUCHED_JUMP_HEIGHT;

    // First Pass: Check for overlapping areas (fastest)
    for (auto& area : m_navfile.m_vAreas)
    {
        // Cheap AABB Check
        if (area.IsOverlapping(vPos))
        {
            // Verify Z Height (make sure we aren't on the floor below)
            if (vPos.z >= area.m_flMinZ - 10.f && vPos.z <= area.m_flMaxZ + 50.f)
            {
                 // Verify Visibility if it's the local origin we are seeking
                 if (!bLocalOrigin || F::NavEngine.IsVectorVisibleNavigation(vCorrected, area.m_vCenter + Vector(0,0,PLAYER_CROUCHED_JUMP_HEIGHT)))
                 {
                     return &area; // Return immediately if inside
                 }
            }
        }

        // Fallback logic: Track closest center if we aren't overlapping any
        float dist = area.m_vCenter.DistToSqr(vPos);
        if (dist < flBestDist) {
            flBestDist = dist;
            pBestArea = &area;
        }
    }

    return pBestArea;
}

void CMap::UpdateIgnores(CTFPlayer* pLocal)
{
    static Timer tUpdate;
    if (!tUpdate.Run(1.f)) return;

    // Clear Sentries/Invulns
    F::NavEngine.ClearFreeBlacklist(BlacklistReason_t(BlacklistReasonEnum::Sentry));
    F::NavEngine.ClearFreeBlacklist(BlacklistReason_t(BlacklistReasonEnum::SentryMedium));
    F::NavEngine.ClearFreeBlacklist(BlacklistReason_t(BlacklistReasonEnum::SentryLow));
    F::NavEngine.ClearFreeBlacklist(BlacklistReason_t(BlacklistReasonEnum::EnemyInvuln));

    // Player Invuln Blacklist
    if (Vars::Misc::Movement::NavBot::Blacklist.Value & Vars::Misc::Movement::NavBot::BlacklistEnum::Players)
    {
        for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
        {
            auto pPlayer = pEntity->As<CTFPlayer>();
            if (!pPlayer->IsAlive()) continue;

            // Only care about Uber/Bonk etc
            if (!pPlayer->IsInvulnerable() && !(pLocal->m_iClass() == TF_CLASS_HEAVY && G::SavedDefIndexes[SLOT_MELEE] == Heavy_t_TheHolidayPunch))
                continue;

            Vector vOrigin;
            if (F::BotUtils.GetDormantOrigin(pPlayer->entindex(), vOrigin)) {
                vOrigin.z += PLAYER_CROUCHED_JUMP_HEIGHT;
                ApplyBlacklistAround(vOrigin, 1000.f, BlacklistReason_t(BlacklistReasonEnum::EnemyInvuln), MASK_SHOT, true);
            }
        }
    }

    // Sentry Blacklist
    if (Vars::Misc::Movement::NavBot::Blacklist.Value & Vars::Misc::Movement::NavBot::BlacklistEnum::Sentries)
    {
        for (auto pEntity : H::Entities.GetGroup(EntityEnum::BuildingEnemy))
        {
            auto pBuilding = pEntity->As<CBaseObject>();
            if (!pBuilding->IsSentrygun() || pBuilding->IsDormant()) continue;
            
            auto pSentry = pBuilding->As<CObjectSentrygun>();
            if (pSentry->m_iState() == SENTRY_STATE_INACTIVE || pSentry->m_bPlacing() || pSentry->m_bHasSapper()) continue;

            // Ignore weak sentries if we are tanky
            bool bTank = (pLocal->m_iClass() == TF_CLASS_HEAVY || pLocal->m_iClass() == TF_CLASS_SOLDIER);
            if (bTank && (pSentry->m_bMiniBuilding() || pSentry->m_iUpgradeLevel() == 1)) continue;

            // Ignore empty sentries
            if (pSentry->m_iAmmoShells() == 0 && (pSentry->m_iUpgradeLevel() != 3 || pSentry->m_iAmmoRockets() == 0)) continue;

            Vector vOrigin;
            if (F::BotUtils.GetDormantOrigin(pSentry->entindex(), vOrigin)) {
                vOrigin.z += PLAYER_CROUCHED_JUMP_HEIGHT;
                ApplyBlacklistAround(vOrigin, 900.f, BlacklistReason_t(BlacklistReasonEnum::Sentry), MASK_SHOT, true);
                ApplyBlacklistAround(vOrigin, 1050.f, BlacklistReason_t(BlacklistReasonEnum::SentryMedium), MASK_SHOT, true);
                if(!bTank) ApplyBlacklistAround(vOrigin, 1200.f, BlacklistReason_t(BlacklistReasonEnum::SentryLow), MASK_SHOT, true);
            }
        }
    }

    // Sticky Blacklist
    if (Vars::Misc::Movement::NavBot::Blacklist.Value & Vars::Misc::Movement::NavBot::BlacklistEnum::Stickies)
    {
        const int iExpiry = TICKCOUNT_TIMESTAMP(Vars::Misc::Movement::NavEngine::StickyIgnoreTime.Value);
        const float flStickyRad = 130.f + HALF_PLAYER_WIDTH;

        for (auto pEntity : H::Entities.GetGroup(EntityEnum::WorldProjectile))
        {
            if (pEntity->GetClassID() != ETFClassID::CTFGrenadePipebombProjectile) continue;
            auto pPipe = pEntity->As<CTFGrenadePipebombProjectile>();
            
            if (pPipe->m_iTeamNum() == pLocal->m_iTeamNum() || pPipe->m_iType() != TF_GL_MODE_REMOTE_DETONATE) continue;
            if (pPipe->IsDormant() || !pPipe->m_vecVelocity().IsZero(1.f)) continue;

            Vector vOrigin = pPipe->GetAbsOrigin(); vOrigin.z += PLAYER_JUMP_HEIGHT/2.f;
            ApplyBlacklistAround(vOrigin, flStickyRad, BlacklistReason_t(BlacklistReasonEnum::Sticky, iExpiry), MASK_SHOT, true);
        }
    }

    // Cleanup expired entries
    const int iNow = I::GlobalVars->tickcount;
    std::erase_if(m_mFreeBlacklist, [iNow](const auto& e) { return e.second.m_iTime && e.second.m_iTime < iNow; });
    std::erase_if(m_mVischeckCache, [iNow](const auto& e) { return e.second.m_iExpireTick && e.second.m_iExpireTick < iNow; });
    std::erase_if(m_mConnectionStuckTime, [iNow](const auto& e) { return e.second.m_iExpireTick && e.second.m_iExpireTick < iNow; });
}
