// AutoJoin.h
#pragma once
#include "../../SDK/SDK.h" // Verify your include path

class CAutoJoin
{
private:
    // Added here so it's available to Run()
    const std::string m_aClassNames[9] = {
        "scout", "sniper", "soldier", "demoman", "medic", 
        "heavyweapons", "pyro", "spy", "engineer"
    };

public:
    void Run(CTFPlayer* pLocal);
};
