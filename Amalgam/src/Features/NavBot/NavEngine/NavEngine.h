#pragma once
#include "Map.h"

// Bot Priority State Machine
Enum(PriorityList, None,
     Patrol = 5,
     LowPrioGetHealth,
     StayNear,
     RunReload, RunSafeReload,
     SnipeSentry,
     Capture,
     GetAmmo,
     MeleeAttack,
     Engineer,
     GetHealth,
     EscapeSpawn, EscapeDanger,
     Followbot
)

struct Crumb_t
{
    CNavArea* m_pNavArea = nullptr;
    Vector m_vPos = {};
    Vector m_vApproachDir = {}; // Normalized direction to this crumb
    float m_flDropHeight = 0.f;
    float m_flApproachDistance = 0.f;
    bool m_bRequiresDrop = false;
};

struct RespawnRoom_t
{    
    int m_iTeam = 0;
    TriggerData_t tData = {};
};

class CNavEngine
{
public:
    // --- Public State ---
    PriorityListEnum::PriorityListEnum m_eCurrentPriority = PriorityListEnum::None;
    Crumb_t m_tCurrentCrumb = {};
    Crumb_t m_tLastCrumb = {};
    Vector m_vLastDestination = {};

private:
    std::unique_ptr<CMap> m_pMap;
    std::vector<Crumb_t> m_vCrumbs;
    std::vector<RespawnRoom_t> m_vRespawnRooms;
    std::vector<CNavArea*> m_vRespawnRoomExitAreas;
    
    CNavArea* m_pLocalArea = nullptr;

    Timer m_tTimeSpentOnCrumbTimer = {};
    Timer m_tInactivityTimer = {};

    bool m_bCurrentNavToLocal = false;
    bool m_bRepathOnFail = false;
    bool m_bUpdatedRespawnRooms = false;

    // --- Internal Logic ---
    bool IsSetupTime();
    void BuildIntraAreaCrumbs(const Vector& vStart, const Vector& vDestination, CNavArea* pArea);
    void AbandonPath();
    void UpdateRespawnRooms();

public:
    // --- Main Interface ---
    void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
    void Reset(bool bForced = false);
    void Render();
    
    // --- Pathing Control ---
    // Make sure to update m_pLocalArea with GetLocalNavArea before running
    bool NavTo(const Vector& vDestination, PriorityListEnum::PriorityListEnum ePriority = PriorityListEnum::Patrol, bool bShouldRepath = true, bool bNavToLocal = true);
    void CancelPath();
    
    // --- Path Execution ---
    void FollowCrumbs(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
    void VischeckPath();
    void CheckBlacklist(CTFPlayer* pLocal);
    void UpdateStuckTime(CTFPlayer* pLocal);

    // --- Queries ---
    bool IsPathing() const { return !m_vCrumbs.empty(); }
    bool IsReady(bool bRoundCheck = false);
    bool IsBlacklistIrrelevant();
    float GetPathCost(const Vector& vLocalOrigin, const Vector& vDestination);
    
    // Visibility & Passability
    bool IsVectorVisibleNavigation(const Vector vFrom, const Vector vTo, unsigned int nMask = MASK_SHOT_HULL);
    // Checks if player can walk from one position to another without bumping into anything (optimized in .cpp)
    bool IsPlayerPassableNavigation(const Vector vFrom, Vector vTo, unsigned int nMask = MASK_PLAYERSOLID);

    // --- Accessors ---
    bool IsNavMeshLoaded() const { return m_pMap && m_pMap->m_eState == NavStateEnum::Active; }
    std::string GetNavFilePath() const { return m_pMap ? m_pMap->m_sMapName : ""; }
    
    CNavFile* GetNavFile() { return m_pMap ? &m_pMap->m_navfile : nullptr; }
    CNavArea* GetLocalNavArea() const { return m_pLocalArea; }
    CNavArea* GetLocalNavArea(const Vector& vLocalOrigin);
    
    // Finds the closest area on the mesh to a point
    CNavArea* FindClosestNavArea(const Vector vOrigin, bool bLocalOrigin = true) { 
        return m_pMap ? m_pMap->FindClosestNavArea(vOrigin, bLocalOrigin) : nullptr; 
    }
    // Alias used by NavBotCore optimizations
    CNavArea* GetNavArea(const Vector& vPos) { return FindClosestNavArea(vPos, false); }

    std::vector<Crumb_t>* GetCrumbs() { return &m_vCrumbs; }

    // --- Respawn Room Management ---
    bool HasRespawnRooms() const { return !m_vRespawnRooms.empty(); }
    void ClearRespawnRooms() { m_vRespawnRooms.clear(); }
    void AddRespawnRoom(int iTeam, TriggerData_t tTrigger) { m_vRespawnRooms.emplace_back(iTeam, tTrigger); }
    std::vector<CNavArea*>* GetRespawnRoomExitAreas() { return &m_vRespawnRoomExitAreas; }

    // --- Blacklist Management ---
    std::unordered_map<CNavArea*, BlacklistReason_t>* GetFreeBlacklist() { 
        return m_pMap ? &m_pMap->m_mFreeBlacklist : nullptr; 
    }
    
    // Get copy of blacklist matching a specific reason
    std::unordered_map<CNavArea*, BlacklistReason_t> GetFreeBlacklist(BlacklistReason_t tReason)
    {
        std::unordered_map<CNavArea*, BlacklistReason_t> mReturnMap;
        if (!m_pMap) return mReturnMap;

        for (const auto& [pNav, tBlacklist] : m_pMap->m_mFreeBlacklist)
        {
            if (tBlacklist.m_eValue == tReason.m_eValue)
                mReturnMap[pNav] = tBlacklist;
        }
        return mReturnMap;
    }

    void ClearFreeBlacklist() { if(m_pMap) m_pMap->m_mFreeBlacklist.clear(); }
    void ClearFreeBlacklist(BlacklistReason_t tReason)
    {
        if(!m_pMap) return;
        std::erase_if(m_pMap->m_mFreeBlacklist, [&](const auto& entry) {
            return entry.second.m_eValue == tReason.m_eValue;
        });
    }
};

ADD_FEATURE(CNavEngine, NavEngine);
