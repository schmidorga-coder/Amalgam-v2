#include "AutoJoin.h"
#include <cstdio> // Required for snprintf
#include <string>

void CAutoJoin::Run(CTFPlayer* pLocal)
{
    // 1. Basic Safety Checks
    if (!pLocal || !I::EngineClient->IsInGame() || !I::EngineClient->IsConnected())
        return;

    // 2. Handle Random Class Logic
    static Timer tRandomTimer{};
    static int iRandomClass = 0;

    int iDesiredClass = Vars::Misc::Automation::ForceClass.Value;

    if (Vars::Misc::Automation::RandomClass.Value)
    {
        // If random class is enabled, pick a new one periodically
        if (!iRandomClass || tRandomTimer.Run(Vars::Misc::Automation::RandomClassInterval.Value * 60.f))
        {
            int iExclude = Vars::Misc::Automation::RandomClassExclude.Value;
            
            // Simple retry loop to find a class not in the exclude list
            do { iRandomClass = SDK::RandomInt(1, 9); } 
            while (iExclude & (1 << (iRandomClass - 1)));
        }
        iDesiredClass = iRandomClass;
    }
    else
    {
        // Reset random class tracker if random is disabled
        iRandomClass = 0; 
    }

    // 3. Check if we actually need to do anything
    // FIX: Removed parentheses. m_iTeamNum and m_iClass are NetVars (integers), not functions.
    int iCurrentTeam = pLocal->m_iTeamNum; 
    int iCurrentClass = pLocal->m_iClass;

    bool bValidTeam = (iCurrentTeam == 2 || iCurrentTeam == 3); // 2 = Red, 3 = Blu
    bool bValidClass = (iDesiredClass > 0 && iDesiredClass < 10);

    // 4. Execute Logic
    // We use a static timer to prevent spamming the console every single frame
    static Timer tJoinTimer{};
    
    if (tJoinTimer.Run(0.5f))
    {
        if (!bValidTeam)
        {
            // We are not on a valid team (Unassigned or Spectator).
            // Use 'autoteam' command directly. This bypasses the VGUI menu (fixes custom HUD issues).
            I::EngineClient->ClientCmd_Unrestricted("autoteam");
        }
        else if (bValidClass && iCurrentClass != iDesiredClass)
        {
            // We are on a team, but not the desired class.
            // Get the class string (Scout, Soldier, etc.) from the header array
            const char* szClassCmd = m_aClassNames[iDesiredClass - 1];
            
            // FIX: Used snprintf instead of std::format to prevent MSBuild linker errors.
            char szCommand[64];
            snprintf(szCommand, sizeof(szCommand), "joinclass %s", szClassCmd);
            
            I::EngineClient->ClientCmd_Unrestricted(szCommand);
        }
    }
}
