#pragma once
#include "../../SDK/SDK.h"
#include <array>

class CAutoJoin
{
private:
    // Map TF2 Class IDs (1-9) to console command strings.
    // 1:Scout, 2:Sniper, 3:Soldier, 4:Demoman, 5:Medic, 6:Heavy, 7:Pyro, 8:Spy, 9:Engineer
    static constexpr std::array<const char*, 9> m_aClassNames = {
        "scout",
        "sniper",
        "soldier",
        "demoman",
        "medic",
        "heavyweapons",
        "pyro",
        "spy",
        "engineer"
    };

public:
    void Run(CTFPlayer* pLocal);
};

ADD_FEATURE(CAutoJoin, AutoJoin)
