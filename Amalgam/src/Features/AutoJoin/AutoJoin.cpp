#include "AutoJoin.h"

void CAutoJoin::Run(CTFPlayer* pLocal)
{
    // 1. Safety Check: Ensure local player exists to prevent crashes
    if (!pLocal)
        return;

    static Timer tJoinTimer{}, tRandomTimer{};
    static int iRandomClass = 0;

    int iDesiredClass = Vars::Misc::Automation::ForceClass.Value;

    // --- Logic: Handle Random Class Selection ---
    if (Vars::Misc::Automation::RandomClass.Value)
    {
        // Calculate the exclusion mask (9 classes)
        // If all classes are excluded (mask 511 = 111111111 binary), do nothing to prevent infinite loop.
        int iExclude = Vars::Misc::Automation::RandomClassExclude.Value;
        if ((iExclude & 511) == 511) 
            return; 

        if (!iRandomClass || tRandomTimer.Run(Vars::Misc::Automation::RandomClassInterval.Value * 60.f))
        {
            do {
                iRandomClass = SDK::RandomInt(1, 9);
            } while (iExclude & (1 << (iRandomClass - 1)));
        }
        iDesiredClass = iRandomClass;
    }
    else
    {
        iRandomClass = 0;
    }

    // --- Logic: Join Team and Class ---
    // Only run every 0.5s to prevent command spam, but fast enough to be responsive
    if (iDesiredClass && tJoinTimer.Run(0.5f))
    {
        // Step A: Handle Team Joining
        // If we are Unassigned (0) or Spectator (1), join a team first.
        if (pLocal->m_iTeamNum() <= 1) 
        {
            // Direct engine command. Bypasses "team_ui_setup" and works on ALL HUDs.
            I::EngineClient->ClientCmd_Unrestricted("jointeam auto"); 
            return; // Return here so we don't try to join class in the same tick (waits for team switch)
        }

        // Step B: Handle Class Joining
        // Only run the command if we are NOT already the desired class.
        // This prevents console spam and stuttering.
        if (pLocal->m_iClass() != iDesiredClass)
        {
            // Direct command. No need for "menuclosed".
            // Ensure m_aClassNames corresponds to TF2 internal strings (scout, sniper, etc.)
            std::string cmd = std::format("join_class {}", m_aClassNames[iDesiredClass - 1]);
            I::EngineClient->ClientCmd_Unrestricted(cmd.c_str());
        }
    }
}
