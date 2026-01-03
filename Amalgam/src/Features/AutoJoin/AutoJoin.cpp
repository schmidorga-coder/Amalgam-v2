#include "AutoJoin.h"
#include <format>

void CAutoJoin::Run(CTFPlayer* pLocal)
{
    // 1. Safety Checks
    if (!pLocal || !I::EngineClient->IsInGame() || !I::EngineClient->IsConnected())
        return;

    // 2. Handle Random Class Logic
    static Timer tRandomTimer{};
    static int iRandomClass = 0;

    int iDesiredClass = Vars::Misc::Automation::ForceClass.Value;

    if (Vars::Misc::Automation::RandomClass.Value)
    {
        if (!iRandomClass || tRandomTimer.Run(Vars::Misc::Automation::RandomClassInterval.Value * 60.f))
        {
            int iExclude = Vars::Misc::Automation::RandomClassExclude.Value;
            do { iRandomClass = SDK::RandomInt(1, 9); }
            while (iExclude & (1 << (iRandomClass - 1)));
        }
        iDesiredClass = iRandomClass;
    }
    else
    {
        iRandomClass = 0;
    }

    // 3. Get Current State
    // Note: Adjust property names (m_iTeamNum, m_iClass) if your SDK uses specific getters like GetTeamNum()
    int iCurrentTeam = pLocal->m_iTeamNum(); 
    int iCurrentClass = pLocal->m_iClass();

    bool bValidTeam = (iCurrentTeam == 2 || iCurrentTeam == 3); // TF_TEAM_RED, TF_TEAM_BLU
    bool bValidClass = (iDesiredClass > 0 && iDesiredClass < 10);

    // 4. Execution
    // Using a static timer to avoid spamming the console every single frame
    static Timer tJoinTimer{};
    
    if (tJoinTimer.Run(0.5f))
    {
        // If we are not on a valid team (Unassigned or Spec), join a team immediately.
        // 'autoteam' bypasses the VGUI menu, fixing custom HUD issues.
        if (!bValidTeam)
        {
            I::EngineClient->ClientCmd_Unrestricted("autoteam");
        }
        // If we are on a team, but not the class we want, switch class.
        else if (bValidClass && iCurrentClass != iDesiredClass)
        {
            // Access the static constexpr array defined in the header
            const char* szClassCmd = m_aClassNames[iDesiredClass - 1];
            
            I::EngineClient->ClientCmd_Unrestricted(std::format("joinclass {}", szClassCmd).c_str());
        }
    }
}
