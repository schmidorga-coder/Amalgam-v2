#include "AutoQueue.h"
#include "../../Players/PlayerUtils.h"
#include "../../NavBot/NavEngine/NavEngine.h"
#include "../Misc.h"
#ifdef TEXTMODE
#include "../NamedPipe/NamedPipe.h"
#endif

#include <algorithm>
#include <fstream>
#include <cstdio>   // Required for snprintf
#include <cstring>  // Required for strncpy_s

void CAutoQueue::Run()
{
    // Timers and State
    static float flLastQueueTime = 0.0f;
    static float flLastAbandonTime = 0.0f;
    static float flLastMannUpQueueTime = 0.0f;
    
    static bool bQueuedCasual = false;
    static bool bQueuedMannUp = false;

    // Engine State
    const bool bInGame = I::EngineClient->IsInGame();
    const bool bIsLoading = I::EngineClient->IsDrawingLoadingImage();
    const bool bIsConnected = I::EngineClient->IsConnected();
    const bool bHasNetChannel = I::ClientState && I::ClientState->m_NetChannel;
    
    const char* pszLevelName = I::EngineClient->GetLevelName();
    const std::string sLevelName = pszLevelName ? pszLevelName : "";
    const float flCurrentTime = I::EngineClient->Time();

    // 1. Map Change Handling
    if (sLevelName != m_sLastLevelName)
    {
        m_sLastLevelName = sLevelName;
        m_bNavmeshAbandonTriggered = false;
        m_bAutoDumpedThisMatch = false;
        m_flAutoDumpStartTime = 0.0f;
        
        // Reset queue states on map load to be safe
        if (bIsLoading && !sLevelName.empty()) 
        {
            bQueuedCasual = false;
            bQueuedMannUp = false;
        }
    }

    // 2. MapBar Boost (Abandon bad maps immediately)
    if (Vars::Misc::Queueing::MapBarBoost.Value && bIsLoading && I::TFGCClientSystem)
    {
        static bool bWasFullyInGame = false;
        if (bWasFullyInGame)
        {
            I::TFGCClientSystem->AbandonCurrentMatch();
            flLastAbandonTime = flCurrentTime;
            SDK::Output("AutoQueue", "MapBar Boost: Abandoning match due to level change.", { 255, 100, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
        }
        
        if (bInGame && !bIsLoading) bWasFullyInGame = true;
        else if (!bInGame) bWasFullyInGame = false;
        
        return;
    }

    // 3. Auto Abandon (No Navmesh)
    if (Vars::Misc::Queueing::AutoAbandonIfNoNavmesh.Value && bInGame && !bIsLoading)
    {
        if (!m_bNavmeshAbandonTriggered && !F::NavEngine.IsNavMeshLoaded())
        {
            m_bNavmeshAbandonTriggered = true;
            SDK::Output("AutoQueue", "No navmesh available, abandoning match.", { 255, 100, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
            if (I::TFGCClientSystem) I::TFGCClientSystem->AbandonCurrentMatch();
            flLastAbandonTime = flCurrentTime;
            bQueuedCasual = false; // Force requeue check
            return;
        }
    }
    else if (!Vars::Misc::Queueing::AutoAbandonIfNoNavmesh.Value)
    {
        m_bNavmeshAbandonTriggered = false;
    }

    // 4. Auto Dump Profiles
    if (Vars::Misc::Queueing::AutoDumpProfiles.Value && Vars::Misc::Queueing::AutoCasualQueue.Value && !Vars::Misc::Queueing::AutoCommunityQueue.Value)
    {
        if (bInGame && !bIsLoading && !m_bAutoDumpedThisMatch)
        {
            const float flDelay = std::max(0.0f, Vars::Misc::Queueing::AutoDumpDelay.Value);
            if (m_flAutoDumpStartTime <= 0.0f) m_flAutoDumpStartTime = flCurrentTime;

            if ((flCurrentTime - m_flAutoDumpStartTime) >= flDelay)
            {
                const auto tResult = F::Misc.DumpProfiles(false);
                if (!tResult.m_bResourceAvailable || tResult.m_uCandidateCount == 0)
                {
                    m_flAutoDumpStartTime = flCurrentTime; // Retry later
                }
                else
                {
                    m_bAutoDumpedThisMatch = true;
                    m_flAutoDumpStartTime = 0.0f;

                    if (I::TFGCClientSystem)
                    {
                        const size_t uDuplicateCount = tResult.m_uSkippedSessionDuplicate + tResult.m_uSkippedFileDuplicate;
                        
                        // Use snprintf instead of std::format
                        char szMsg[256];
                        snprintf(szMsg, sizeof(szMsg), "Auto dump complete: %zu new profiles. Abandoning.", tResult.m_uAppendedCount);
                        SDK::Output("AutoQueue", szMsg, { 255, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
                        
                        I::TFGCClientSystem->AbandonCurrentMatch();
                        flLastAbandonTime = flCurrentTime;
                        bQueuedCasual = false;
                    }
                }
            }
        }
        else if (!bInGame)
        {
            m_bAutoDumpedThisMatch = false;
            m_flAutoDumpStartTime = 0.0f;
        }
    }

    // Cancel queues if loading (Safety check)
    if (bIsLoading)
    {
        if (bQueuedCasual && I::TFPartyClient->BInQueueForMatchGroup(k_eTFMatchGroup_Casual_Default))
        {
            I::TFPartyClient->CancelMatchQueueRequest(k_eTFMatchGroup_Casual_Default);
            bQueuedCasual = false;
            SDK::Output("AutoQueue", "Loading: Cancelled casual queue.", { 255, 255, 100 }, OUTPUT_CONSOLE);
        }
        if (bQueuedMannUp && I::TFPartyClient->BInQueueForMatchGroup(k_eTFMatchGroup_MvM_MannUp))
        {
            I::TFPartyClient->CancelMatchQueueRequest(k_eTFMatchGroup_MvM_MannUp);
            bQueuedMannUp = false;
        }
        return; // Do nothing else while loading
    }

    // 5. Mann Up Queue
    if (Vars::Misc::Queueing::AutoMannUpQueue.Value)
    {
        bool bIsQueued = I::TFPartyClient->BInQueueForMatchGroup(k_eTFMatchGroup_MvM_MannUp);
        if (!bIsQueued && !bInGame && !bIsConnected)
        {
            // Calculate Delay
            float flQueueDelay = (Vars::Misc::Queueing::QueueDelay.Value == 0) ? 20.0f : Vars::Misc::Queueing::QueueDelay.Value * 60.0f;
            
            // Check Timer
            if (!bQueuedMannUp || (flCurrentTime - flLastMannUpQueueTime >= flQueueDelay))
            {
                if (I::TFPartyClient)
                {
                    I::TFPartyClient->RequestQueueForMatch(k_eTFMatchGroup_MvM_MannUp);
                    flLastMannUpQueueTime = flCurrentTime;
                    bQueuedMannUp = true;
                    SDK::Output("AutoQueue", "Queued for Mann Up.", { 100, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
                }
            }
        }
        else if (bIsQueued)
        {
            bQueuedMannUp = true;
        }
    }
    else
    {
        if (bQueuedMannUp && I::TFPartyClient)
        {
            I::TFPartyClient->CancelMatchQueueRequest(k_eTFMatchGroup_MvM_MannUp);
            bQueuedMannUp = false;
        }
    }

    // 6. Casual Queue Logic
    if (Vars::Misc::Queueing::AutoCasualQueue.Value)
    {
        bool bIsQueued = I::TFPartyClient->BInQueueForMatchGroup(k_eTFMatchGroup_Casual_Default);
        
        // RQif (Requeue If player count conditions met)
        bool bRQifActive = Vars::Misc::Queueing::RQif.Value;
        bool bRQConditionMet = false;
        int nPlayerCount = 0;

        if (bInGame && bRQifActive)
        {
            if (auto pResource = H::Entities.GetResource())
            {
                for (int i = 1; i <= I::EngineClient->GetMaxClients(); i++)
                {
                    if (!pResource->m_bValid(i) || !pResource->m_bConnected(i) || pResource->m_iUserID(i) == -1)
                        continue;
                    if (pResource->IsFakePlayer(i))
                        continue;

                    bool bShouldCount = true;
                    const uint32_t uFriendsID = pResource->m_iAccountID(i);

                    if (Vars::Misc::Queueing::RQIgnoreFriends.Value)
                    {
#ifdef TEXTMODE
                        if (uFriendsID && F::NamedPipe.IsLocalBot(uFriendsID))
                            bShouldCount = false;
#endif
                        if (bShouldCount && (H::Entities.IsFriend(uFriendsID) || H::Entities.InParty(uFriendsID) ||
                            F::PlayerUtils.HasTag(uFriendsID, F::PlayerUtils.TagToIndex(FRIEND_TAG)) ||
                            F::PlayerUtils.HasTag(uFriendsID, F::PlayerUtils.TagToIndex(FRIEND_IGNORE_TAG)) ||
                            F::PlayerUtils.HasTag(uFriendsID, F::PlayerUtils.TagToIndex(IGNORED_TAG)) ||
                            F::PlayerUtils.HasTag(uFriendsID, F::PlayerUtils.TagToIndex(BOT_IGNORE_TAG)) ||
                            F::PlayerUtils.HasTag(uFriendsID, F::PlayerUtils.TagToIndex(PARTY_TAG))))
                        {
                            bShouldCount = false;
                        }
                    }

                    if (bShouldCount) nPlayerCount++;
                }
            }

            int nPlayersLT = Vars::Misc::Queueing::RQplt.Value;
            int nPlayersGT = Vars::Misc::Queueing::RQpgt.Value;
            if ((nPlayersLT > 0 && nPlayerCount < nPlayersLT) || (nPlayersGT > 0 && nPlayerCount > nPlayersGT))
                bRQConditionMet = true;
        }

        // Handling Requeue (RQif)
        if (bInGame && bRQConditionMet)
        {
            if (Vars::Misc::Queueing::RQnoAbandon.Value)
            {
                // Queue while in game (Double Queue)
                if (!bIsQueued)
                {
                    I::TFPartyClient->RequestQueueForMatch(k_eTFMatchGroup_Casual_Default);
                    bQueuedCasual = true;
                    
                    char szMsg[128];
                    snprintf(szMsg, sizeof(szMsg), "RQif: Double queueing (Players: %d).", nPlayerCount);
                    SDK::Output("AutoQueue", szMsg, { 255, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
                }
            }
            else
            {
                // Abandon and requeue
                if ((flCurrentTime - flLastAbandonTime) > 5.0f) // Spam protection
                {
                    if (I::TFGCClientSystem) I::TFGCClientSystem->AbandonCurrentMatch();
                    flLastAbandonTime = flCurrentTime;
                    bQueuedCasual = false; // We will queue once disconnected
                    
                    char szMsg[128];
                    snprintf(szMsg, sizeof(szMsg), "RQif: Abandoning (Players: %d).", nPlayerCount);
                    SDK::Output("AutoQueue", szMsg, { 255, 100, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
                }
            }
        }

        // Standard Queue Logic
        // Only proceed if we are NOT in a game and NOT connected
        if (!bInGame && !bIsConnected && !bHasNetChannel)
        {
            float flQueueDelay = (Vars::Misc::Queueing::QueueDelay.Value == 0) ? 20.0f : Vars::Misc::Queueing::QueueDelay.Value * 60.0f;
            
            // If we just abandoned or disconnected, we might want to reset the timer if RQkick is enabled (instant queue)
            if (Vars::Misc::Queueing::RQif.Value && Vars::Misc::Queueing::RQkick.Value)
            {
                // Logic handled by bRQConditionMet setting bQueuedCasual = false or abandoning
            }

            if (!bIsQueued)
            {
                bool bTimeToQueue = !bQueuedCasual || (flCurrentTime - flLastQueueTime >= flQueueDelay);
                
                if (bTimeToQueue)
                {
                    static bool bLoadedCriteria = false;
                    if (!bLoadedCriteria && I::TFPartyClient)
                    {
                        I::TFPartyClient->LoadSavedCasualCriteria();
                        bLoadedCriteria = true;
                    }

                    if (I::TFPartyClient)
                    {
                        I::TFPartyClient->RequestQueueForMatch(k_eTFMatchGroup_Casual_Default);
                        flLastQueueTime = flCurrentTime;
                        bQueuedCasual = true;
                        SDK::Output("AutoQueue", "Queued for Casual.", { 100, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
                    }
                }
            }
        }
    }
    else
    {
        if (bQueuedCasual && I::TFPartyClient)
        {
            I::TFPartyClient->CancelMatchQueueRequest(k_eTFMatchGroup_Casual_Default);
            bQueuedCasual = false;
        }
    }

    // 7. Community Queue
    if (Vars::Misc::Queueing::AutoCommunityQueue.Value)
    {
        RunCommunityQueue();
    }
    else
    {
        CleanupServerList();
        m_bConnectedToCommunityServer = false;
        m_sCurrentServerIP.clear();
    }
}

void CAutoQueue::RunCommunityQueue()
{
    if (!I::SteamMatchmakingServers)
        return;

    const bool bInGame = I::EngineClient->IsInGame();
    const bool bIsLoading = I::EngineClient->IsDrawingLoadingImage();
    const float flCurrentTime = I::EngineClient->Time();

    if (bIsLoading) return;

    // Disconnect Handling
    static bool bWasInGameCommunity = false;
    if (bWasInGameCommunity && !bInGame && m_bConnectedToCommunityServer)
    {
        HandleDisconnect();
    }
    bWasInGameCommunity = bInGame;

    if (bInGame && m_bConnectedToCommunityServer)
    {
        CheckServerTimeout();
        return;
    }

    if (!bInGame && !m_bSearchingServers)
    {
        float flSearchDelay = Vars::Misc::Queueing::ServerSearchDelay.Value;
        if ((flCurrentTime - m_flLastServerSearch) >= flSearchDelay)
            SearchCommunityServers();
    }
}

void CAutoQueue::SearchCommunityServers()
{
    if (!I::SteamMatchmakingServers || m_bSearchingServers)
        return;

    SDK::Output("AutoQueue", "Searching for community servers...", { 100, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
    CleanupServerList();

    std::vector<MatchMakingKeyValuePair_t> vFilters;
    
    // Helper lambda to add filters using MSVC safe string copy
    auto addFilter = [&vFilters](const char* key, const char* val) {
        MatchMakingKeyValuePair_t filter;
        strncpy_s(filter.m_szKey, key, sizeof(filter.m_szKey));
        strncpy_s(filter.m_szValue, val, sizeof(filter.m_szValue));
        vFilters.push_back(filter);
    };

    addFilter("appid", "440");
    addFilter("hasplayers", "1");
    addFilter("notfull", "1");

    if (Vars::Misc::Queueing::AvoidPasswordServers.Value)
    {
        addFilter("nand", "1");
        addFilter("password", "1");
    }
    if (Vars::Misc::Queueing::OnlyNonDedicatedServers.Value)
    {
        addFilter("nand", "1");
        addFilter("dedicated", "1");
    }

    m_hServerListRequest = I::SteamMatchmakingServers->RequestInternetServerList(
        440,
        vFilters.data(),
        static_cast<int>(vFilters.size()),
        this
    );

    if (m_hServerListRequest)
    {
        m_bSearchingServers = true;
        m_flLastServerSearch = I::EngineClient->Time();
    }
}

void CAutoQueue::ConnectToServer(const gameserveritem_t* pServer)
{
    if (!pServer) return;

    std::string sServerAddress = pServer->m_NetAdr.GetConnectionAddressString();
    
    // Avoid connecting to the same server twice in a row immediately
    if (sServerAddress == m_sCurrentServerIP && (I::EngineClient->Time() - m_flServerJoinTime) < 60.0f)
        return;

    // Use snprintf for output
    char szMsg[256];
    snprintf(szMsg, sizeof(szMsg), "Connecting to: %s [%s]", pServer->GetName(), sServerAddress.c_str());
    SDK::Output("AutoQueue", szMsg, { 100, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);

    std::string sConnectCmd = "connect " + sServerAddress;
    I::EngineClient->ClientCmd_Unrestricted(sConnectCmd.c_str());

    m_sCurrentServerIP = sServerAddress;
    m_flServerJoinTime = I::EngineClient->Time();
    m_bConnectedToCommunityServer = true;
}

bool CAutoQueue::IsServerValid(const gameserveritem_t* pServer)
{
    if (!pServer || !pServer->m_bHadSuccessfulResponse)
        return false;

    if (Vars::Misc::Queueing::AvoidPasswordServers.Value && pServer->m_bPassword)
        return false;

    if (pServer->m_nPlayers >= pServer->m_nMaxPlayers)
        return false;

    int nHumanPlayers = pServer->m_nPlayers - pServer->m_nBotPlayers;
    if (nHumanPlayers < Vars::Misc::Queueing::MinPlayersOnServer.Value ||
        nHumanPlayers > Vars::Misc::Queueing::MaxPlayersOnServer.Value)
        return false;

    if (Vars::Misc::Queueing::RequireNavmesh.Value && !HasNavmeshForMap(pServer->m_szMap))
        return false;

    if (Vars::Misc::Queueing::OnlySteamNetworkingIPs.Value)
    {
        std::string sServerIP = pServer->m_NetAdr.GetConnectionAddressString();
        if (sServerIP.rfind("169.254", 0) != 0) return false; 
    }

    return true;
}

bool CAutoQueue::HasNavmeshForMap(const std::string& sMapName)
{
    std::string sNavPath = F::NavEngine.GetNavFilePath();
    if (sNavPath.empty()) return false;

    // Check if current loaded navmesh matches
    if (F::NavEngine.IsNavMeshLoaded())
    {
        const size_t uFirstAfterLastSlash = sNavPath.find_last_of("/\\") + 1;
        if (sNavPath.find(sMapName, uFirstAfterLastSlash) == uFirstAfterLastSlash)
            return true;
    }

    // Check file on disk
    std::ifstream navFile(sNavPath, std::ios::binary);
    if (!navFile.is_open()) return false;

    uint32_t uMagic = 0;
    navFile.read(reinterpret_cast<char*>(&uMagic), sizeof(uint32_t));
    return uMagic == 0xFEEDFACE;
}

bool CAutoQueue::IsServerNameMatch(const std::string& sServerName)
{
    // Check for "'s Server" pattern
    size_t sServerPos = sServerName.rfind("'s Server");
    if (sServerPos == std::string::npos) return false;
    
    // Basic validation
    if (sServerPos < 2) return false;
    if (sServerName.length() > sServerPos + 9)
    {
        char cNextChar = sServerName[sServerPos + 9];
        if (cNextChar != ' ' && cNextChar != '\t' && cNextChar != '(' && cNextChar != '\0')
            return false;
    }

    return true;
}

void CAutoQueue::CleanupServerList()
{
    if (m_hServerListRequest && I::SteamMatchmakingServers)
    {
        I::SteamMatchmakingServers->ReleaseRequest(m_hServerListRequest);
        m_hServerListRequest = nullptr;
    }
    m_vCommunityServers.clear();
    m_bSearchingServers = false;
}

void CAutoQueue::HandleDisconnect()
{
    SDK::Output("AutoQueue", "Disconnected from community server.", { 255, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
    m_bConnectedToCommunityServer = false;
    m_sCurrentServerIP.clear();
    m_flServerJoinTime = 0.0f;
    m_flLastServerSearch = I::EngineClient->Time() - Vars::Misc::Queueing::ServerSearchDelay.Value + 5.0f; // Search soon
}

void CAutoQueue::CheckServerTimeout()
{
    if (Vars::Misc::Queueing::MaxTimeOnServer.Value <= 0.0f) return;

    float flTimeOnServer = I::EngineClient->Time() - m_flServerJoinTime;
    if (flTimeOnServer >= Vars::Misc::Queueing::MaxTimeOnServer.Value)
    {
        SDK::Output("AutoQueue", "Max time on server reached, disconnecting...", { 255, 255, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
        I::EngineClient->ClientCmd_Unrestricted("disconnect");
    }
}

void CAutoQueue::ServerResponded(HServerListRequest hRequest, int iServer)
{
    if (hRequest != m_hServerListRequest || !I::SteamMatchmakingServers) return;

    gameserveritem_t* pServer = I::SteamMatchmakingServers->GetServerDetails(hRequest, iServer);
    if (pServer && IsServerValid(pServer))
        m_vCommunityServers.push_back(pServer);
}

void CAutoQueue::ServerFailedToRespond(HServerListRequest hRequest, int iServer) { }

void CAutoQueue::RefreshComplete(HServerListRequest hRequest, EMatchMakingServerResponse response)
{
    if (hRequest != m_hServerListRequest) return;

    m_bSearchingServers = false;
    
    // Use snprintf for output
    char szMsg[128];
    snprintf(szMsg, sizeof(szMsg), "Found %zu valid servers.", m_vCommunityServers.size());
    SDK::Output("AutoQueue", szMsg, { 100, 255, 100 }, OUTPUT_CONSOLE);

    if (!m_vCommunityServers.empty())
    {
        // Sort: Prefer Steam Nick Servers, then by player count
        std::sort(m_vCommunityServers.begin(), m_vCommunityServers.end(),
            [this](const gameserveritem_t* a, const gameserveritem_t* b) -> bool
            {
                bool aIsNick = IsServerNameMatch(a->GetName());
                bool bIsNick = IsServerNameMatch(b->GetName());

                if (aIsNick != bIsNick) return aIsNick; // Nick servers first

                int aPlayers = a->m_nPlayers - a->m_nBotPlayers;
                int bPlayers = b->m_nPlayers - b->m_nBotPlayers;
                return aPlayers > bPlayers; // Most players first
            });

        ConnectToServer(m_vCommunityServers[0]);
    }
    else
    {
        SDK::Output("AutoQueue", "No valid community servers found. Retrying...", { 255, 100, 100 }, OUTPUT_CONSOLE | OUTPUT_TOAST, -1);
    }

    CleanupServerList();
}
