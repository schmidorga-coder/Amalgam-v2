#pragma once
#include "../../../SDK/SDK.h"
#include "../../../SDK/Definitions/Steam/ISteamMatchmaking.h"
#include "../../../SDK/Definitions/Steam/MatchmakingTypes.h"
#include <vector>
#include <string>

// Forward declarations
class gameserveritem_t;

class CAutoQueue : public ISteamMatchmakingServerListResponse
{
private:
    // ===================================================================
    // STATE FLAGS
    // ===================================================================
    bool m_bSearchingServers = false;
    bool m_bConnectedToCommunityServer = false;
    bool m_bNavmeshAbandonTriggered = false;
    bool m_bAutoDumpedThisMatch = false;
    
    // Queue status tracking (Moved from static locals for better state management)
    bool m_bQueuedCasual = false;
    bool m_bQueuedMannUp = false;

    // ===================================================================
    // TIMERS
    // ===================================================================
    float m_flLastServerSearch = 0.0f;
    float m_flServerJoinTime = 0.0f;
    float m_flAutoDumpStartTime = 0.0f;
    float m_flLastAbandonTime = 0.0f;      // To prevent spamming abandon
    float m_flLastMannUpQueueTime = 0.0f;  // Specific timer for MannUp delays

    // ===================================================================
    // DATA STORAGE
    // ===================================================================
    std::string m_sCurrentServerIP;
    std::string m_sLastLevelName;

    // NOTE: gameserveritem_t pointers are owned by the Steam Matchmaking interface.
    // They are only valid until ReleaseRequest (CleanupServerList) is called.
    HServerListRequest m_hServerListRequest = nullptr;
    std::vector<gameserveritem_t*> m_vCommunityServers;

    // ===================================================================
    // HELPER FUNCTIONS
    // ===================================================================
    void RunCommunityQueue();
    void SearchCommunityServers();
    void ConnectToServer(const gameserveritem_t* pServer);
    
    bool IsServerValid(const gameserveritem_t* pServer);
    bool HasNavmeshForMap(const std::string& sMapName);
    bool IsServerNameMatch(const std::string& sServerName);
    
    void CleanupServerList();
    void HandleDisconnect();
    void CheckServerTimeout();

public:
    void Run();

    // Virtual Destructor: Important for 64-bit inheritance safety
    virtual ~CAutoQueue() {}

    // ===================================================================
    // ISteamMatchmakingServerListResponse INTERFACE
    // ===================================================================
    virtual void ServerResponded(HServerListRequest hRequest, int iServer) override;
    virtual void ServerFailedToRespond(HServerListRequest hRequest, int iServer) override;
    virtual void RefreshComplete(HServerListRequest hRequest, EMatchMakingServerResponse response) override;
};

ADD_FEATURE(CAutoQueue, AutoQueue);
