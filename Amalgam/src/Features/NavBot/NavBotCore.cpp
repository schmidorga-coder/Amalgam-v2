#include "NavBotCore.h"
#include "NavBotJobs/Roam.h"
#include "NavBotJobs/EscapeDanger.h"
#include "NavBotJobs/Melee.h"
#include "NavBotJobs/Capture.h"
#include "NavBotJobs/GetSupplies.h"
#include "NavBotJobs/Engineer.h"
#include "NavBotJobs/SnipeSentry.h"
#include "NavBotJobs/GroupWithOthers.h"
#include "NavBotJobs/Reload.h"
#include "NavBotJobs/StayNear.h"
#include "NavEngine/NavEngine.h"
#include "../FollowBot/FollowBot.h"
#include "../PacketManip/FakeLag/FakeLag.h"
#include "../CritHack/CritHack.h"
#include "../Ticks/Ticks.h"
#include "../Misc/Misc.h"

// Constants
constexpr float BLACKLIST_VISCHECK_HEIGHT = 45.0f; // Roughly crouched height

// Helper to mark areas using BFS instead of iterating the entire map (Performance boost)
void CNavBotCore::MarkAreaDangerous(const Vector& vOrigin, bool bDormant)
{
    auto pStartArea = F::NavEngine.GetNavArea(vOrigin); // Assuming GetNavArea finds the area at pos
    if (!pStartArea) return;

    // Use a queue for BFS and a set to keep track of visited areas to avoid cycles
    std::deque<CNavArea*> dQueue;
    std::unordered_set<int> sVisited; // Using ID for faster lookup

    dQueue.push_back(pStartArea);
    sVisited.insert(pStartArea->m_nID);

    // Squared distances for optimization
    const float flSlightDangerSqr = std::pow(m_tSelectedConfig.m_flMinSlightDanger, 2);
    const float flFullDangerSqr = std::pow(m_tSelectedConfig.m_flMinFullDanger, 2);

    // Determines if we should actually mark this area based on game rules (capping etc)
    const bool bIgnoreSlightDanger = (Vars::Misc::Movement::NavBot::Preferences.Value & Vars::Misc::Movement::NavBot::PreferencesEnum::SafeCapping || F::NavEngine.m_eCurrentPriority != PriorityListEnum::Capture);

    while (!dQueue.empty())
    {
        CNavArea* pCurrent = dQueue.front();
        dQueue.pop_front();

        Vector vAreaCenter = pCurrent->m_vCenter;
        float flDistSqr = vOrigin.DistToSqr(vAreaCenter);

        // If we went too far, stop branching this path
        if (flDistSqr > flSlightDangerSqr)
            continue;

        // Perform Visibility Check (Expensive, so we do it after distance check)
        Vector vAreaVis = vAreaCenter; 
        vAreaVis.z += BLACKLIST_VISCHECK_HEIGHT;
        Vector vOriginVis = vOrigin;
        vOriginVis.z += BLACKLIST_VISCHECK_HEIGHT;

        if (F::NavEngine.IsVectorVisibleNavigation(vOriginVis, vAreaVis, MASK_SHOT))
        {
            auto& blacklist = *F::NavEngine.GetFreeBlacklist();
            
            // Logic: Is this area fully dangerous, or just slightly?
            if (flDistSqr < flFullDangerSqr)
            {
                // Full Danger: Always blacklist
                blacklist[pCurrent] = bDormant ? BlacklistReasonEnum::EnemyDormant : BlacklistReasonEnum::EnemyNormal;
            }
            else if (bIgnoreSlightDanger)
            {
                // Slight Danger: Only blacklist if we care about slight danger
                // Note: Original code used a counter logic here, simplified for BFS. 
                // If strict counting is needed, a map<CNavArea*, int> should be passed to this function.
                blacklist[pCurrent] = bDormant ? BlacklistReasonEnum::EnemyDormant : BlacklistReasonEnum::EnemyNormal;
            }
        }

        // Add neighbors to queue
        for (const auto& connection : pCurrent->m_vConnections)
        {
            if (sVisited.find(connection.m_pArea->m_nID) == sVisited.end())
            {
                sVisited.insert(connection.m_pArea->m_nID);
                dQueue.push_back(connection.m_pArea);
            }
        }
    }
}

void CNavBotCore::UpdateEnemyBlacklist(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iSlot)
{
    // 1. Feature & Safety Checks
    if (!(Vars::Misc::Movement::NavBot::Blacklist.Value & Vars::Misc::Movement::NavBot::BlacklistEnum::Players) ||
        F::NavEngine.IsBlacklistIrrelevant())
        return;

    static Timer tBlacklistUpdateTimer{};
    static Timer tDormantUpdateTimer{};
    static int iLastSlotBlacklist = SLOT_PRIMARY;

    bool bCheckNormal = Vars::Misc::Movement::NavBot::Blacklist.Value & Vars::Misc::Movement::NavBot::BlacklistEnum::NormalThreats;
    bool bCheckDormant = Vars::Misc::Movement::NavBot::Blacklist.Value & Vars::Misc::Movement::NavBot::BlacklistEnum::DormantThreats;

    // Timer Logic
    bool bRunNormal = bCheckNormal && (tBlacklistUpdateTimer.Run(Vars::Misc::Movement::NavBot::BlacklistDelay.Value) || iLastSlotBlacklist != iSlot);
    bool bRunDormant = bCheckDormant && (tDormantUpdateTimer.Run(Vars::Misc::Movement::NavBot::BlacklistDormantDelay.Value) || iLastSlotBlacklist != iSlot);

    if (!bRunNormal && !bRunDormant)
        return;

    iLastSlotBlacklist = iSlot;

    // Clear existing blacklists
    if (bRunNormal) F::NavEngine.ClearFreeBlacklist(BlacklistReason_t(BlacklistReasonEnum::EnemyNormal));
    if (bRunDormant) F::NavEngine.ClearFreeBlacklist(BlacklistReason_t(BlacklistReasonEnum::EnemyDormant));

    // Don't run if round is over
    if (const auto& pGameRules = I::TFGameRules())
        if (pGameRules->m_iRoundState() == GR_STATE_TEAM_WIN) return;

    // #NoFear: Don't blacklist if holding melee (likely trying to melee someone)
    if (iSlot == SLOT_MELEE) return;

    // Cache to skip processing enemies that are close to each other
    static std::vector<Vector> vProcessedOrigins;
    vProcessedOrigins.clear();

    const float flMinDistSqr = std::pow(std::max(100.0f, m_tSelectedConfig.m_flMinSlightDanger * 0.25f), 2);

    for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
    {
        auto pPlayer = pEntity->As<CTFPlayer>();
        if (!pPlayer->IsAlive()) continue;

        bool bIsDormant = pPlayer->IsDormant();
        
        // Filter: Only process if we are running the update for this type
        if (bIsDormant && !bRunDormant) continue;
        if (!bIsDormant && !bRunNormal) continue;

        Vector vOrigin;
        if (!F::BotUtils.GetDormantOrigin(pPlayer->entindex(), vOrigin)) continue;

        // Optimization: Skip if we just processed a very similar position
        bool bSkip = false;
        for (const auto& vChecked : vProcessedOrigins) {
            if (vOrigin.DistToSqr(vChecked) < flMinDistSqr) {
                bSkip = true; 
                break;
            }
        }
        if (bSkip) continue;

        vProcessedOrigins.push_back(vOrigin);

        // Run the BFS Danger Marker
        MarkAreaDangerous(vOrigin, bIsDormant);
    }
}

void CNavBotCore::UpdateEngineerSlot(CTFPlayer* pLocal)
{
    if (!F::NavBotEngineer.IsEngieMode(pLocal)) return;

    int iSwitchTo = -1;

    switch (F::NavBotEngineer.m_eTaskStage)
    {
    case EngineerTaskStageEnum::BuildSentry:
    case EngineerTaskStageEnum::BuildDispenser:
        // Switch if we are close to the build spot (prepare to wrench)
        if (F::NavBotEngineer.m_tCurrentBuildingSpot.m_flDistanceToTarget != FLT_MAX &&
            F::NavBotEngineer.m_tCurrentBuildingSpot.m_vPos.DistToSqr(pLocal->GetAbsOrigin()) <= (500.f * 500.f))
        {
            iSwitchTo = SLOT_MELEE;
        }
        break;
    case EngineerTaskStageEnum::SmackSentry:
        if (F::NavBotEngineer.m_flDistToSentry <= 300.f) iSwitchTo = SLOT_MELEE;
        break;
    case EngineerTaskStageEnum::SmackDispenser:
        if (F::NavBotEngineer.m_flDistToDispenser <= 500.f) iSwitchTo = SLOT_MELEE;
        break;
    default:
        break;
    }

    if (iSwitchTo != -1)
    {
        // Only force melee if we aren't already holding it
        if (F::BotUtils.m_iCurrentSlot < SLOT_MELEE)
            F::BotUtils.SetSlot(pLocal, SLOT_MELEE);
    }
}

void CNavBotCore::UpdateSlot(CTFPlayer* pLocal, ClosestEnemy_t tClosestEnemy)
{
    static Timer tSlotTimer{};
    if (!tSlotTimer.Run(0.2f)) return;

    // 1. Reload Check
    int iReloadSlot = F::NavBotReload.GetReloadWeaponSlot(pLocal, tClosestEnemy);
    F::NavBotReload.m_iLastReloadSlot = iReloadSlot;

    // 2. Engineer specific logic override
    if (F::NavBotEngineer.IsEngieMode(pLocal))
    {
        UpdateEngineerSlot(pLocal);
        // If engineer logic forced a switch, we might want to return early or let the reload logic override?
        // Usually building takes priority over reloading if we are actively building.
        if (F::BotUtils.m_iCurrentSlot == SLOT_MELEE) return; 
    }

    // 3. Normal Weapon Switch Logic
    int iDesiredSlot = -1;

    if (iReloadSlot != -1)
        iDesiredSlot = iReloadSlot;
    else if (Vars::Misc::Movement::BotUtils::WeaponSlot.Value)
        iDesiredSlot = F::BotUtils.m_iBestSlot;

    if (iDesiredSlot != -1 && F::BotUtils.m_iCurrentSlot != iDesiredSlot)
        F::BotUtils.SetSlot(pLocal, iDesiredSlot);
}

// Fixed recursion safety
bool CNavBotCore::FindClosestHidingSpot(CNavArea* pArea, Vector vVischeckPoint, int iMaxRecursion, std::pair<CNavArea*, int>& tOut, bool bVischeck, int iCurrentDepth, std::vector<int>* pVisited)
{
    // Use a local visited vector if this is the root call
    std::vector<int> localVisited;
    if (!pVisited) pVisited = &localVisited;

    if (iCurrentDepth >= iMaxRecursion) return false;

    Vector vAreaOrigin = pArea->m_vCenter;
    vAreaOrigin.z += PLAYER_CROUCHED_JUMP_HEIGHT;

    // Success condition: Area is hidden
    if (bVischeck && !F::NavEngine.IsVectorVisibleNavigation(vAreaOrigin, vVischeckPoint))
    {
        tOut = { pArea, iCurrentDepth };
        return true;
    }

    // Mark visited
    pVisited->push_back(pArea->m_nID);

    std::pair<CNavArea*, int> tBestSpot = { nullptr, INT_MAX };
    bool bFound = false;

    for (const auto& connection : pArea->m_vConnections)
    {
        // Skip if already visited
        if (std::find(pVisited->begin(), pVisited->end(), connection.m_pArea->m_nID) != pVisited->end())
            continue;

        std::pair<CNavArea*, int> tCandidate;
        if (FindClosestHidingSpot(connection.m_pArea, vVischeckPoint, iMaxRecursion, tCandidate, bVischeck, iCurrentDepth + 1, pVisited))
        {
            if (tCandidate.second < tBestSpot.second)
            {
                tBestSpot = tCandidate;
                bFound = true;
                // Optimization: If we found a spot at current+1, it's the best possible we can find in this branch
                if (tBestSpot.second == iCurrentDepth + 1) break; 
            }
        }
    }

    if (bFound)
    {
        tOut = tBestSpot;
        return true;
    }

    return false;
}

static bool IsWeaponValidForDT(CTFWeaponBase* pWeapon)
{
    if (!pWeapon || F::BotUtils.m_iCurrentSlot == SLOT_MELEE) return false;
    
    // DT not useful on Sniper Rifles
    switch(pWeapon->GetWeaponID()) {
        case TF_WEAPON_SNIPERRIFLE:
        case TF_WEAPON_SNIPERRIFLE_CLASSIC:
        case TF_WEAPON_SNIPERRIFLE_DECAP:
            return false;
    }

    return SDK::WeaponDoesNotUseAmmo(pWeapon, false);
}

void CNavBotCore::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
    // 1. Global Valid Checks
    if (!Vars::Misc::Movement::NavBot::Enabled.Value || !Vars::Misc::Movement::NavEngine::Enabled.Value ||
        !pLocal->IsAlive() || !pWeapon || !F::NavEngine.IsReady())
    {
        Reset();
        return;
    }

    // 2. State Checks
    if (Vars::Misc::Movement::NavBot::DisableOnSpectate.Value && H::Entities.IsSpectated()) { Reset(); return; }
    if (F::NavEngine.m_eCurrentPriority == PriorityListEnum::Followbot || F::FollowBot.m_bActive) { Reset(); return; }
    if (F::NavEngine.m_eCurrentPriority != PriorityListEnum::StayNear) F::NavBotStayNear.m_iStayNearTargetIdx = -1;
    if (F::Ticks.m_bWarp || F::Ticks.m_bDoubletap) return;
    
    // Manual movement override check
    if ((pCmd->buttons & (IN_FORWARD | IN_BACK | IN_MOVERIGHT | IN_MOVELEFT)) && !F::Misc.m_bAntiAFK) return;

    F::NavBotGroup.UpdateLocalBotPositions(pLocal);

    // Verify Nav Area
    if (!F::NavEngine.GetLocalNavArea(pLocal->GetAbsOrigin()))
    {
        Reset();
        return;
    }

    // 3. Double Tap Logic
    static Timer tDoubletapRecharge{};
    if (Vars::Misc::Movement::NavBot::RechargeDT.Value && IsWeaponValidForDT(pWeapon))
    {
        bool bCanRecharge = !F::Ticks.m_bRechargeQueue && G::Attacking != 1 && (F::Ticks.m_iShiftedTicks < F::Ticks.m_iShiftedGoal);
        bool bModeWait = Vars::Misc::Movement::NavBot::RechargeDT.Value == Vars::Misc::Movement::NavBot::RechargeDTEnum::WaitForFL;
        bool bFlReady = Vars::Fakelag::Fakelag.Value && F::FakeLag.m_iGoal;

        if (bCanRecharge && (!bModeWait || !bFlReady))
        {
            if (tDoubletapRecharge.Check(Vars::Misc::Movement::NavBot::RechargeDTDelay.Value))
                F::Ticks.m_bRechargeQueue = true;
        }
        else if (F::Ticks.m_iShiftedTicks >= F::Ticks.m_iShiftedGoal)
        {
            tDoubletapRecharge.Update();
        }
    }

    // 4. Update Sub-Systems
    F::NavBotEngineer.RefreshLocalBuildings(pLocal);
    F::NavBotEngineer.RefreshBuildingSpots(pLocal, F::BotUtils.m_tClosestEnemy);

    // 5. Config Selection
    switch (pLocal->m_iClass())
    {
    case TF_CLASS_SCOUT:
    case TF_CLASS_HEAVY:
        m_tSelectedConfig = CONFIG_SHORT_RANGE;
        break;
    case TF_CLASS_ENGINEER:
        m_tSelectedConfig = F::NavBotEngineer.IsEngieMode(pLocal) ? 
            (pWeapon->m_iItemDefinitionIndex() == Engi_t_TheGunslinger ? CONFIG_GUNSLINGER_ENGINEER : CONFIG_ENGINEER) : 
            CONFIG_SHORT_RANGE;
        break;
    case TF_CLASS_SNIPER:
        m_tSelectedConfig = pWeapon->GetWeaponID() == TF_WEAPON_COMPOUND_BOW ? CONFIG_MID_RANGE : CONFIG_LONG_RANGE;
        break;
    default:
        m_tSelectedConfig = CONFIG_MID_RANGE;
    }

    UpdateSlot(pLocal, F::BotUtils.m_tClosestEnemy);
    UpdateEnemyBlacklist(pLocal, pWeapon, F::BotUtils.m_iCurrentSlot);

    // 6. Priority Task Execution Chain
    // The order determines priority: If a function returns true, it handled the tick.
    bool bTaskHandled = 
           F::NavBotDanger.EscapeSpawn(pLocal)
        || F::NavBotDanger.EscapeProjectiles(pLocal)
        || F::NavBotMelee.Run(pCmd, pLocal, F::BotUtils.m_iCurrentSlot, F::BotUtils.m_tClosestEnemy)
        || F::NavBotDanger.EscapeDanger(pLocal)
        || F::NavBotSupplies.Run(pCmd, pLocal, GetSupplyEnum::Health)
        || F::NavBotSupplies.Run(pCmd, pLocal, GetSupplyEnum::Ammo)
        || F::NavBotReload.RunSafe(pLocal, pWeapon)
        || F::NavBotGroup.Run(pLocal, pWeapon)
        || F::NavBotCapture.Run(pLocal, pWeapon)
        || F::NavBotEngineer.Run(pCmd, pLocal, F::BotUtils.m_tClosestEnemy)
        || F::NavBotSnipe.Run(pLocal)
        || F::NavBotStayNear.Run(pLocal, pWeapon)
        || F::NavBotSupplies.Run(pCmd, pLocal, GetSupplyEnum::Health | GetSupplyEnum::LowPrio)
        || F::NavBotRoam.Run(pLocal, pWeapon);

    // 7. Conditional CritHack Force
    if (bTaskHandled)
    {
        CTFPlayer* pTarget = nullptr;
        // Identify target based on current job priority
        switch (F::NavEngine.m_eCurrentPriority)
        {
        case PriorityListEnum::StayNear:
            pTarget = I::ClientEntityList->GetClientEntity(F::NavBotStayNear.m_iStayNearTargetIdx)->As<CTFPlayer>();
            break;
        case PriorityListEnum::MeleeAttack:
        case PriorityListEnum::GetHealth:
        case PriorityListEnum::EscapeDanger:
            pTarget = F::BotUtils.m_tClosestEnemy.m_pPlayer; // Use closest known enemy
            break;
        default:
            break;
        }

        if (pTarget && !pTarget->IsDormant() && pTarget->IsAlive())
        {
            // Only force crit if we can actually kill them or deal significant damage
            F::CritHack.m_bForce = (pTarget->m_iHealth() >= pWeapon->GetDamage());
        }
        else
        {
            F::CritHack.m_bForce = false;
        }
    }
}

void CNavBotCore::Reset()
{
    F::NavBotStayNear.m_iStayNearTargetIdx = -1;
    F::NavBotReload.m_iLastReloadSlot = -1;
    F::NavBotSnipe.m_iTargetIdx = -1;
    F::NavBotSupplies.ResetTemp();
    F::NavBotEngineer.Reset();
    F::NavBotCapture.Reset();
}

void CNavBotCore::Draw(CTFPlayer* pLocal)
{
    if (!(Vars::Menu::Indicators.Value & Vars::Menu::IndicatorsEnum::NavBot) || !pLocal || !pLocal->IsAlive())
        return;

    auto bIsReady = F::NavEngine.IsReady();
    if (!Vars::Debug::Info.Value && !bIsReady) return;

    int x = Vars::Menu::NavBotDisplay.Value.x;
    int y = Vars::Menu::NavBotDisplay.Value.y + 8;
    const auto& fFont = H::Fonts.GetFont(FONT_INDICATORS);
    const int nTall = fFont.m_nTall + H::Draw.Scale(1);

    EAlign align = ALIGN_TOP;
    // Simple screen boundary check
    if (x <= 100) align = ALIGN_TOPLEFT;
    else if (x >= H::Draw.m_nScreenW - 100) align = ALIGN_TOPRIGHT;

    const auto& cColor = F::NavEngine.IsPathing() ? Vars::Menu::Theme::Active.Value : Vars::Menu::Theme::Inactive.Value;
    
    // Status String Builder
    std::wstring sJob = L"Idle";
    switch (F::NavEngine.m_eCurrentPriority)
    {
    case PriorityListEnum::Patrol:          sJob = F::NavBotRoam.m_bDefending ? L"Defend" : L"Patrol"; break;
    case PriorityListEnum::LowPrioGetHealth:sJob = L"Health (Low)"; break;
    case PriorityListEnum::StayNear:        sJob = std::format(L"Stalk {}", F::NavBotStayNear.m_sFollowTargetName); break;
    case PriorityListEnum::RunReload:
    case PriorityListEnum::RunSafeReload:   sJob = L"Reloading"; break;
    case PriorityListEnum::SnipeSentry:     sJob = L"Snipe Sentry"; break;
    case PriorityListEnum::GetAmmo:         sJob = L"Ammo"; break;
    case PriorityListEnum::Capture:         sJob = L"Capture"; break;
    case PriorityListEnum::MeleeAttack:     sJob = L"Melee"; break;
    case PriorityListEnum::GetHealth:       sJob = L"Health"; break;
    case PriorityListEnum::EscapeSpawn:     sJob = L"Escape Spawn"; break;
    case PriorityListEnum::EscapeDanger:    sJob = L"Fleeing"; break;
    case PriorityListEnum::Followbot:       sJob = L"FollowBot"; break;
    case PriorityListEnum::Engineer:
        sJob = L"Engineer";
        if (F::NavBotEngineer.m_eTaskStage == EngineerTaskStageEnum::BuildSentry) sJob = L"Building Sentry";
        else if (F::NavBotEngineer.m_eTaskStage == EngineerTaskStageEnum::SmackSentry) sJob = L"Repairing Sentry";
        break;
    }

    std::wstring sStatus = std::format(L"Job: {} {}", sJob, F::CritHack.m_bForce ? L"(Crit)" : L"");

    H::Draw.StringOutlined(fFont, x, y, cColor, Vars::Menu::Theme::Background.Value, align, sStatus.c_str());
    
    // Debug info
    if (Vars::Debug::Info.Value)
    {
        H::Draw.StringOutlined(fFont, x, y += nTall, cColor, Vars::Menu::Theme::Background.Value, align, std::format("Ready: {}", bIsReady).c_str());
        if (auto pArea = F::NavEngine.GetLocalNavArea())
            H::Draw.StringOutlined(fFont, x, y += nTall, cColor, Vars::Menu::Theme::Background.Value, align, std::format("Area ID: {}", pArea->m_nID).c_str());
    }
}
