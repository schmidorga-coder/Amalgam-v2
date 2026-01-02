#pragma once
#include "BotUtils.h"
#include <vector>

class CNavArea;

class CNavBotCore
{
public:
    // Controls the bot parameters like distance from enemy
    struct BotClassConfig_t
    {
        float m_flMinFullDanger;
        float m_flMinSlightDanger;
        float m_flMax;
        bool m_bPreferFar;
    };

    // Configurations (Static Constexpr for optimization)
    static constexpr BotClassConfig_t CONFIG_SHORT_RANGE = { 140.0f, 400.0f, 600.0f, false };
    static constexpr BotClassConfig_t CONFIG_MID_RANGE = { 200.0f, 500.0f, 3000.0f, true };
    static constexpr BotClassConfig_t CONFIG_LONG_RANGE = { 300.0f, 500.0f, 4000.0f, true };
    static constexpr BotClassConfig_t CONFIG_ENGINEER = { 200.0f, 500.0f, 3000.0f, false };
    static constexpr BotClassConfig_t CONFIG_GUNSLINGER_ENGINEER = { 50.0f, 300.0f, 2000.0f, false };

    BotClassConfig_t m_tSelectedConfig = CONFIG_MID_RANGE;

public:
    // Main Entry Points
    void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
    void Reset();
    void Draw(CTFPlayer* pLocal);

    // Recursive function to find hiding spot
    // Updated signature: Uses a pointer to a vector for visited tracking to avoid static state bugs
    bool FindClosestHidingSpot(CNavArea* pArea, Vector vVischeckPoint, int iMaxRecursion, std::pair<CNavArea*, int>& tOut, bool bVischeck = true, int iCurrentDepth = 0, std::vector<int>* pVisited = nullptr);

private:
    // Internal Logic Helpers
    void MarkAreaDangerous(const Vector& vOrigin, bool bDormant);
    void UpdateEnemyBlacklist(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iSlot);
    void UpdateSlot(CTFPlayer* pLocal, ClosestEnemy_t tClosestEnemy);
    void UpdateEngineerSlot(CTFPlayer* pLocal);
};

ADD_FEATURE(CNavBotCore, NavBotCore);
