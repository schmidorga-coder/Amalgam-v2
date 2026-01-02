#pragma once
#include "FileReader/CNavFile.h"
#include "MicroPather/micropather.h"
#include <boost/container_hash/hash.hpp>
#include <limits>
#include <queue>
#include <unordered_set>
#include <unordered_map>

// Dimensions & Constants
#define PLAYER_WIDTH        49.0f
#define HALF_PLAYER_WIDTH   (PLAYER_WIDTH / 2.0f)
#define PLAYER_HEIGHT       83.0f
#define PLAYER_CROUCHED_JUMP_HEIGHT  72.0f
#define PLAYER_JUMP_HEIGHT  50.0f

// Helper macro for timestamp calculation
#define TICKCOUNT_TIMESTAMP(seconds) (I::GlobalVars->tickcount + static_cast<int>((seconds) / I::GlobalVars->interval_per_tick))

Enum(NavState, Unavailable, Active)
Enum(VischeckState, NotVisible = -1, NotChecked, Visible)

// Blacklist Reasons
Enum(BlacklistReason, Init = -1,
     Sentry, SentryMedium, SentryLow,
     Sticky,
     EnemyNormal, EnemyDormant, EnemyInvuln,
     BadBuildSpot
)

struct BlacklistReason_t
{
    BlacklistReasonEnum::BlacklistReasonEnum m_eValue = BlacklistReasonEnum::Init;
    int m_iTime = 0;

    BlacklistReason_t() = default;

    // Constructors for easier usage
    BlacklistReason_t(BlacklistReasonEnum::BlacklistReasonEnum eReason, int iTime = 0) 
        : m_eValue(eReason), m_iTime(iTime) {}

    // Operator for direct assignment of enum
    BlacklistReason_t& operator=(BlacklistReasonEnum::BlacklistReasonEnum eReason) {
        m_eValue = eReason;
        m_iTime = 0;
        return *this;
    }
};

struct NavPoints_t
{
    Vector m_vCurrent = {};
    Vector m_vCenter = {};
    Vector m_vCenterNext = {}; // Used for height checks
    Vector m_vNext = {};

    NavPoints_t() = default;
    NavPoints_t(const Vector& cur, const Vector& center, const Vector& centerNext, const Vector& next)
        : m_vCurrent(cur), m_vCenter(center), m_vCenterNext(centerNext), m_vNext(next) {}
};

struct DropdownHint_t
{
    Vector m_vAdjustedPos = {};
    Vector m_vApproachDir = {};
    float m_flDropHeight = 0.f;
    float m_flApproachDistance = 0.f;
    bool m_bRequiresDrop = false;
};

struct CachedConnection_t
{
    int m_iExpireTick = 0;
    VischeckStateEnum::VischeckStateEnum m_eVischeckState = VischeckStateEnum::NotChecked;
    bool m_bPassable = false;
    float m_flCachedCost = std::numeric_limits<float>::max();
    DropdownHint_t m_tDropdown = {};
    NavPoints_t m_tPoints = {};
};

struct CachedStucktime_t
{
    int m_iExpireTick = 0;
    int m_iTimeStuck = 0;
};

class CMap : public micropather::Graph
{
public:
    CNavFile m_navfile;
    std::string m_sMapName;
    NavStateEnum::NavStateEnum m_eState;
    micropather::MicroPather m_pather{ this, 3000, 6, true };

    // Caches (Using Boost hash for pairs)
    using NavPair = std::pair<CNavArea*, CNavArea*>;
    std::unordered_map<NavPair, CachedConnection_t, boost::hash<NavPair>> m_mVischeckCache;
    std::unordered_map<NavPair, CachedStucktime_t, boost::hash<NavPair>> m_mConnectionStuckTime;

    // Free Blacklist (Manually managed, cleared on level change or via logic)
    std::unordered_map<CNavArea*, BlacklistReason_t> m_mFreeBlacklist;
    bool m_bFreeBlacklistBlocked = false;

    // Constructor
    CMap(const char* sMapName) : m_navfile(sMapName), m_sMapName(sMapName)
    {
        m_eState = m_navfile.m_bOK ? NavStateEnum::Active : NavStateEnum::Unavailable;
    }

    // --- MicroPather Interface ---
    float LeastCostEstimate(void* pStartArea, void* pEndArea) override 
    { 
        return reinterpret_cast<CNavArea*>(pStartArea)->m_vCenter.DistTo(reinterpret_cast<CNavArea*>(pEndArea)->m_vCenter); 
    }
    
    void AdjacentCost(void* pArea, std::vector<micropather::StateCost>* pAdjacent) override;
    void PrintStateInfo(void*) override {} // Unused but required

    // --- Core Logic ---
    NavPoints_t DeterminePoints(CNavArea* pCurrentArea, CNavArea* pNextArea);
    DropdownHint_t HandleDropdown(const Vector& vCurrentPos, const Vector& vNextPos);

private:
    // Internal Cost Helpers
    float EvaluateConnectionCost(CNavArea* pCurrentArea, CNavArea* pNextArea, const NavPoints_t& tPoints, const DropdownHint_t& tDropdown) const;
    float GetBlacklistPenalty(const BlacklistReason_t& tReason) const;
    bool ShouldOverrideBlacklist(const BlacklistReason_t& tCurrent, const BlacklistReason_t& tIncoming) const;
    
    // Internal Area Helpers
    void ApplyBlacklistAround(const Vector& vOrigin, float flRadius, const BlacklistReason_t& tReason, unsigned int nMask, bool bRequireLOS);
    void CollectAreasAround(const Vector& vOrigin, float flRadius, std::vector<CNavArea*>& vOutAreas);

public:
    // --- Public Queries ---
    
    // Finds the closest nav area to a point (optimized in cpp)
    CNavArea* FindClosestNavArea(const Vector& vPos, bool bLocalOrigin);
    void UpdateIgnores(CTFPlayer* pLocal);

    // Path finding wrapper
    std::vector<void*> FindPath(CNavArea* pLocalArea, CNavArea* pDestArea)
    {
        if (m_eState != NavStateEnum::Active) return {};

        float flCost = 0.f;
        std::vector<void*> vPath;
        int result = m_pather.Solve(reinterpret_cast<void*>(pLocalArea), reinterpret_cast<void*>(pDestArea), &vPath, &flCost);

        if (result == micropather::MicroPather::START_END_SAME)
            return { reinterpret_cast<void*>(pLocalArea) };

        return vPath;
    }

    void Reset()
    {
        m_mVischeckCache.clear();
        m_mConnectionStuckTime.clear();
        m_mFreeBlacklist.clear();
        m_pather.Reset();
    }
};
