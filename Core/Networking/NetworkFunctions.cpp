/* Copyright (c) 2022-2026 Dreamy Cecil
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation


This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "StdH.h"

#include "NetworkFunctions.h"
#include "MessageProcessing.h"
#include "Modules.h"
#include "ExtPackets.h"

#include "ScriptScheduler.h"   // 1111
#include "Query/GeoIP.h"            // 1111
#include "Query/PlayerDB.h"         // 1111
#include <string.h>                 // 1111: strcmp

CTString ser_strChatName = "Server";    //  1111
CTString ser_strDefaultNickColor = "^cffffff";  //  1111

CTString cmd_strIdleWatcherTick = "";  // 1111

void SayToClient(SHELL_FUNC_ARGS);    //  1111
void SayToAllExcept(SHELL_FUNC_ARGS);    //  1111
void ScheduleScriptFunc(SHELL_FUNC_ARGS);    //  1111
void CancelScheduledFunc(SHELL_FUNC_ARGS);    //  1111

void SetLanguage(SHELL_FUNC_ARGS);          // 1111
void SayToClientLang(SHELL_FUNC_ARGS);      // 1111
void SayToAllExceptLang(SHELL_FUNC_ARGS);   // 1111
void KickClientLang(SHELL_FUNC_ARGS);       // 1111

void VoteForMap(SHELL_FUNC_ARGS);          // 1111
void CastMapVote(SHELL_FUNC_ARGS);         // 1111
void MapVoteTimeoutCheck(SHELL_FUNC_ARGS); // 1111
void ExecuteMapChange(SHELL_FUNC_ARGS);    // 1111  fires after the countdown
 
void ListClients(SHELL_FUNC_ARGS);              // 1111
void VoteForKick(SHELL_FUNC_ARGS);              // 1111
void VoteForBan(SHELL_FUNC_ARGS);               // 1111
void BanClientTemp(SHELL_FUNC_ARGS);            // 1111
void CastPlayerVote(SHELL_FUNC_ARGS);           // 1111
void PlayerVoteTimeoutCheck(SHELL_FUNC_ARGS);   // 1111

CTString ColorizeNick_Impl(const CTString& strIn);  // 1111
static CTString ColorizeNickFunc(SHELL_FUNC_ARGS);  // 1111
 
// Initialize networking
void INetwork::Initialize(void) {
  // Modeler applications don't need networking
  if (ClassicsCore_IsModelerApp()) return;

  // Load client log
  IClientLogging::LoadLog();

  // Server commands
  _pShell->DeclareSymbol("persistent user INDEX ser_bEnableAntiFlood;",      &ser_bEnableAntiFlood);
  _pShell->DeclareSymbol("persistent user INDEX ser_iPacketFloodThreshold;", &ser_iPacketFloodThreshold);
  _pShell->DeclareSymbol("persistent user INDEX ser_iMaxMessagesPerSecond;", &ser_iMaxMessagesPerSecond);
  _pShell->DeclareSymbol("persistent user INDEX ser_iMaxPlayersPerClient;",  &ser_iMaxPlayersPerClient);

  _pShell->DeclareSymbol("persistent user CTString ser_strChatName;", &ser_strChatName);    //  1111

  _pShell->DeclareSymbol("user void SayToClient(INDEX, CTString, CTString);", &SayToClient);    //  1111
  _pShell->DeclareSymbol("user void SayToAllExcept(INDEX, CTString, CTString);", &SayToAllExcept);    //  1111

  // Multilingual chat (1111): selects EN or RU per-recipient via PlayerDB language preference
  _pShell->DeclareSymbol("user void SetLanguage(INDEX, CTString);", &SetLanguage);    // 1111
  _pShell->DeclareSymbol("user void SayToClientLang(INDEX, CTString, CTString, CTString);", &SayToClientLang);    // 1111
  _pShell->DeclareSymbol("user void SayToAllExceptLang(INDEX, CTString, CTString, CTString);", &SayToAllExceptLang);    // 1111
  _pShell->DeclareSymbol("user void KickClientLang(INDEX, CTString, CTString);", &KickClientLang);    // 1111

  // Inside INetwork::Initialize():     1111
  _pShell->DeclareSymbol("user void ScheduleScript(FLOAT, CTString);", &ScheduleScriptFunc);
  _pShell->DeclareSymbol("user void CancelScheduled(void);", &CancelScheduledFunc);

  _pShell->DeclareSymbol("user CTString cmd_strIdleWatcherTick;", &cmd_strIdleWatcherTick);    // 1111

  _pShell->DeclareSymbol("user void VoteForMap(INDEX, CTString, CTString, CTString, INDEX);", &VoteForMap);                    // 1111
  _pShell->DeclareSymbol("user void CastMapVote(INDEX, INDEX);", &CastMapVote);                                         // 1111
  _pShell->DeclareSymbol("user void MapVoteTimeoutCheck(INDEX);", &MapVoteTimeoutCheck);                                // 1111
  _pShell->DeclareSymbol("user void ExecuteMapChange(void);", &ExecuteMapChange);                                       // 1111

  _pShell->DeclareSymbol("user void ListClients(INDEX);",             &ListClients);             // 1111
  _pShell->DeclareSymbol("user void VoteForKick(INDEX, INDEX);",      &VoteForKick);             // 1111
  _pShell->DeclareSymbol("user void VoteForBan(INDEX, INDEX);",       &VoteForBan);              // 1111
  _pShell->DeclareSymbol("user void BanClientTemp(INDEX, INDEX);",    &BanClientTemp);           // 1111
  _pShell->DeclareSymbol("user void CastPlayerVote(INDEX, INDEX);",   &CastPlayerVote);          // 1111
  _pShell->DeclareSymbol("user void PlayerVoteTimeoutCheck(INDEX);",  &PlayerVoteTimeoutCheck);  // 1111

  _pShell->DeclareSymbol("user CTString ColorizeNick(CTString);", &ColorizeNickFunc);  // 1111
  _pShell->DeclareSymbol("persistent user CTString ser_strDefaultNickColor;", &ser_strDefaultNickColor);  // 1111
  
  // Register commands for packet processing
  IProcessPacket::RegisterCommands();

#if _PATCHCONFIG_NEW_QUERY
  // Initialize query manager
  extern void InitQuery(void);
  InitQuery();
#endif

#if _PATCHCONFIG_EXT_PACKETS
  // Register extension packets
  CExtPacket::RegisterExtPackets();
#endif

  // Initialize voting system
  IVotingSystem::Initialize();

  _aActiveClients.New(ICore::MAX_SERVER_CLIENTS);

#if _PATCHCONFIG_GUID_MASKING
  IProcessPacket::_aClientChecks.New(ICore::MAX_GAME_PLAYERS);
#endif

  // extern void InitHttp(void);
  // InitHttp();
  GeoIP_Init();     // Replacement      1111

  // Make sure there is enough space for local players
  _pNetwork->ga_aplsPlayers.Clear();
  _pNetwork->ga_aplsPlayers.New(ICore::MAX_LOCAL_PLAYERS);
};

// Handle packets coming from a client
// If output is TRUE, it will pass packets into engine's CServer::Handle()
BOOL INetwork::ServerHandle(CMessageDispatcher *pmd, INDEX iClient, CNetworkMessage &nmMessage) {
  CSessionSocket &sso = _pNetwork->ga_srvServer.srv_assoSessions[iClient];
  sso.sso_tvMessageReceived = _pTimer->GetHighPrecisionTimer();

  MESSAGETYPE ePacket = nmMessage.GetType();

  // Process some default packets
  switch (ePacket) {
    // Client confirming the disconnection
    case PCK_REP_DISCONNECTED:
      IProcessPacket::OnClientDisconnect(iClient, nmMessage);
      return FALSE;

    // Client requesting the session state
    case MSG_REQ_CONNECTREMOTESESSIONSTATE:
      IProcessPacket::OnConnectRemoteSessionStateRequest(iClient, nmMessage);
      return FALSE;

    // Client requesting the connection to the server
    case MSG_REQ_CONNECTPLAYER:
      IProcessPacket::OnPlayerConnectRequest(iClient, nmMessage);
      return FALSE;

    // Client changing the character
    case MSG_REQ_CHARACTERCHANGE:
      IProcessPacket::OnCharacterChangeRequest(iClient, nmMessage);
      return FALSE;

    // Client sending player actions
    case MSG_ACTION:
      IProcessPacket::OnPlayerAction(iClient, nmMessage);
      return FALSE;

    // Client sending a CRC check
    case MSG_SYNCCHECK:
      IProcessPacket::OnSyncCheck(iClient, nmMessage);
      return FALSE;

    // Client sending a chat message
    case MSG_CHAT_IN:
      return IProcessPacket::OnChatInRequest(iClient, nmMessage);
  }

#if _PATCHCONFIG_EXT_PACKETS

  // Let CServer::Handle process packets of other types
  if (ePacket != PCK_EXTENSION) return TRUE;

  // Handle specific packet types
  ULONG ulType;
  INetDecompress::Integer(nmMessage, ulType);

  // Let plugins handle packets
  FOREACHPLUGIN(itPlugin) {
    if (itPlugin->pm_events.m_network->OnServerPacket == NULL) continue;

    // Handle packet through this plugin handler
    if (itPlugin->pm_events.m_network->OnServerPacket(nmMessage, ulType)) {
      // Quit if packet has been handled
      return FALSE;
    }
  }

  CExtPacket *pPacket = NULL;

  // Only create packets that can come from clients
  if (ulType >= IClassicsExtPacket::k_EPacketType_FirstC2S) {
    pPacket = CExtPacket::CreatePacket((IClassicsExtPacket::EPacketType)ulType);
  }

  // No built-in packet under this index
  if (pPacket == NULL) {
    CPrintF(TRANS("Server received PCK_EXTENSION of an invalid (%u) type!\n"), ulType);
    ASSERT(FALSE);

    return FALSE;
  }

  // Read and process the packet
  pPacket->Read(nmMessage);
  pPacket->Process();

  // No extra processing needed
  delete pPacket;
  return FALSE;

#else

  // Let CServer::Handle process packets of other types
  return TRUE;

#endif // _PATCHCONFIG_EXT_PACKETS
};

// Handle packets coming from a server
// If output is TRUE, it will pass packets into engine's CSessionState::ProcessGameStreamBlock()
BOOL INetwork::ClientHandle(CSessionState *pses, CNetworkMessage &nmMessage) {
#if _PATCHCONFIG_EXT_PACKETS

  // Let default methods handle packets of other types
  if (nmMessage.GetType() != PCK_EXTENSION) return TRUE;

  // Handle specific packet types
  ULONG ulType;
  INetDecompress::Integer(nmMessage, ulType);

  // Let plugins handle packets
  FOREACHPLUGIN(itPlugin) {
    if (itPlugin->pm_events.m_network->OnClientPacket == NULL) continue;

    // Handle packet through this plugin handler
    if (itPlugin->pm_events.m_network->OnClientPacket(nmMessage, ulType)) {
      // Quit if packet has been handled
      return FALSE;
    }
  }

  CExtPacket *pPacket = NULL;

  // Only create packets that can come from a server
  if (ulType <= IClassicsExtPacket::k_EPacketType_LastS2C) {
    pPacket = CExtPacket::CreatePacket((IClassicsExtPacket::EPacketType)ulType);
  }

  // No built-in packet under this index
  if (pPacket == NULL) {
    CPrintF(TRANS("Client received PCK_EXTENSION of an invalid (%u) type!\n"), ulType);
    ASSERT(FALSE);

    return FALSE;
  }

  // Read and process the packet
  pPacket->Read(nmMessage);
  pPacket->Process();

  // No extra processing needed
  delete pPacket;
  return FALSE;

#else

  // Let CSessionState::ProcessGameStreamBlock process packets of other types
  return TRUE;

#endif // _PATCHCONFIG_EXT_PACKETS
};

// Send disconnect message to a client (CServer::SendDisconnectMessage reimplementation)
void INetwork::SendDisconnectMessage(INDEX iClient, const char *strExplanation, BOOL bStream) {
  // Not a server
  if (!_pNetwork->IsServer()) return;

  CSessionSocket &sso = _pNetwork->ga_srvServer.srv_assoSessions[iClient];

  if (!bStream) {
    // Send message
    CNetworkMessage nmDisconnect(MSG_INF_DISCONNECTED);
    nmDisconnect << CTString(strExplanation);

    _pNetwork->SendToClientReliable(iClient, nmDisconnect);

  } else {
    // Send stream
    CTMemoryStream strmDisconnect;
    strmDisconnect << INDEX(MSG_INF_DISCONNECTED);
    strmDisconnect << CTString(strExplanation);

    _pNetwork->SendToClientReliable(iClient, strmDisconnect);
  }

  // Report that the packet has been sent
  CPrintF(LOCALIZE("Client '%s' ordered to disconnect: %s\n"), GetComm().Server_GetClientName(iClient), strExplanation);

  // If not disconnected before
  if (sso.sso_iDisconnectedState == 0) {
    // Mark for disconnection
    sso.sso_iDisconnectedState = 1;

  // If the client is still hanging here
  } else {
    // Force disconnection
    CPrintF(LOCALIZE("Forcing client '%s' to disconnect\n"), GetComm().Server_GetClientName(iClient));

    sso.sso_iDisconnectedState = 2;
  }

  // Make client inactive
  CActiveClient::DeactivateClient(iClient);
};

// Send chat message to a client with a custom sender name
void INetwork::SendChatToClient(INDEX iClient, const CTString &strFromName, const CTString &strMessage) {
  // Not a server
  if (!_pNetwork->IsServer()) return;

  CNetworkMessage nm(MSG_CHAT_OUT);
  nm << (INDEX)0;
  nm << strFromName;
  nm << strMessage;

  _pNetwork->SendToClient(iClient, nm);
};

// Send chat message from the server to all clients
void INetwork::SendChatFromServer(const CTString &strMessage) {
  // Not a server
  if (!_pNetwork->IsServer()) return;

  // Relay the message to the server, which then sends it to everyone
  CNetworkMessage nm(MSG_CHAT_IN);
  nm << ULONG(0) << ULONG(-1); // From server (0) to all clients (-1)
  nm << strMessage;
  _pNetwork->SendToServer(nm);
};

// Send chat with a custom sender name to one client, or all if iClient == -1   //   1111
void SayToClient(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX                iClient = NEXT_ARG(INDEX);
    const CTString& strSender = *NEXT_ARG(CTString*);
    const CTString& strMessage = *NEXT_ARG(CTString*);

    if (!_pNetwork->IsServer()) return;

    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();

    if (iClient == -1) {
        for (INDEX i = 0; i < ctSessions; i++) {
            if (i > 0 && !_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
            INetwork::SendChatToClient(i, strSender, strMessage);
        }
    }
    else {
        INetwork::SendChatToClient(iClient, strSender, strMessage);
    }
};

// Send chat with a custom sender name to every active client except one    1111
void SayToAllExcept(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX                iExclude = NEXT_ARG(INDEX);
    const CTString& strSender = *NEXT_ARG(CTString*);
    const CTString& strMessage = *NEXT_ARG(CTString*);

    if (!_pNetwork->IsServer()) return;

    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();

    for (INDEX i = 0; i < ctSessions; i++) {
        if (i > 0 && !_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
        if (i == iExclude) continue;
        INetwork::SendChatToClient(i, strSender, strMessage);
    }
};

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ 1111 â”€â”€â”€â”€
// Multilingual chat â€” selects EN or RU per recipient based on their stored
// PlayerDB language preference. SayToClient/SayToAllExcept above are left
// completely untouched; these are separate functions for dual-language call sites.
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Pick the EN or RU string for a given client slot (defaults to EN)    1111
//
// PlayerDB's session cache (_aSessions[]) is keyed by PLAYER index (iNewPlayer
// from OnPlayerConnectRequest), not by CLIENT/session index like SayToClient.
// Those two indices are NOT the same number in general - iNewPlayer is just
// the first free player-buffer slot, unrelated to which client slot connected.
// Translate before touching PlayerDB.
static INDEX ClientToPlayerIndex(INDEX iClient) {    // 1111
    ULONG ulMask = INetwork::MaskOfClientPlayers(iClient);
    if (ulMask == 0) return -1;

    // First (lowest) player index belonging to this client
    INDEX iPlayer = 0;
    while (!(ulMask & 1)) {
        ulMask >>= 1;
        iPlayer++;
    }
    return iPlayer;
}
const CTString& SelectLangMessage(INDEX iClient, const CTString& strEN, const CTString& strRU) {
    const char* szLang = PlayerDB_GetLanguage(ClientToPlayerIndex(iClient));
    return (strcmp(szLang, "ru") == 0) ? strRU : strEN;
}

// Set a player's language preference: SetLanguage(iClient, "en"|"ru")    1111
void SetLanguage(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX            iClient = NEXT_ARG(INDEX);
    const CTString& strLang = *NEXT_ARG(CTString*);

    PlayerDB_SetLanguage(ClientToPlayerIndex(iClient), strLang.str_String);
};

// Like SayToClient, but picks the EN or RU string per recipient    1111
void SayToClientLang(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX                iClient = NEXT_ARG(INDEX);
    const CTString& strSender = *NEXT_ARG(CTString*);
    const CTString& strMessageEN = *NEXT_ARG(CTString*);
    const CTString& strMessageRU = *NEXT_ARG(CTString*);

    if (!_pNetwork->IsServer()) return;

    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();

    if (iClient == -1) {
        for (INDEX i = 0; i < ctSessions; i++) {
            if (i > 0 && !_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
            INetwork::SendChatToClient(i, strSender, SelectLangMessage(i, strMessageEN, strMessageRU));
        }
    }
    else {
        INetwork::SendChatToClient(iClient, strSender, SelectLangMessage(iClient, strMessageEN, strMessageRU));
    }
};

// Like SayToAllExcept, but picks the EN or RU string per recipient    1111
void SayToAllExceptLang(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX                iExclude = NEXT_ARG(INDEX);
    const CTString& strSender = *NEXT_ARG(CTString*);
    const CTString& strMessageEN = *NEXT_ARG(CTString*);
    const CTString& strMessageRU = *NEXT_ARG(CTString*);

    if (!_pNetwork->IsServer()) return;

    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();

    for (INDEX i = 0; i < ctSessions; i++) {
        if (i > 0 && !_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
        if (i == iExclude) continue;
        INetwork::SendChatToClient(i, strSender, SelectLangMessage(i, strMessageEN, strMessageRU));
    }
};

// Disconnects a client with an EN or RU reason depending on their language
// preference. Built directly on SendDisconnectMessage rather than on whatever
// KickClient(INDEX, CTString) wrapper your init.ini calls today â€” point me at
// that wrapper if it does more than a plain disconnect.    1111
void KickClientLang(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX                iClient = NEXT_ARG(INDEX);
    const CTString& strReasonEN = *NEXT_ARG(CTString*);
    const CTString& strReasonRU = *NEXT_ARG(CTString*);

    if (!_pNetwork->IsServer()) return;

    if (iClient == -1) {
        const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();
        for (INDEX i = 1; i < ctSessions; i++) {
            if (!_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
            INetwork::SendDisconnectMessage(i, SelectLangMessage(i, strReasonEN, strReasonRU), FALSE);
        }
    }
    else {
        INetwork::SendDisconnectMessage(iClient, SelectLangMessage(iClient, strReasonEN, strReasonRU), FALSE);
    }
};

#define MAPVOTE_TIMEOUT_SECONDS 60.0f

static BOOL      _bMapVoteActive = FALSE;
static CTString  _strMapVoteNameEN = "";
static CTString  _strMapVoteNameRU = "";
static CTString  _strMapVoteLevel = "";
static BOOL      _abMapVotedYes[PLAYERDB_MAX_PLAYERS + 1];  // by client slot; 0 unused
static BOOL      _abMapVotedNo[PLAYERDB_MAX_PLAYERS + 1];
static INDEX      _iMapVoteGeneration = 0;  // invalidates stale timeout callbacks
static CTString  _strPendingMapChangeLevel = "";  // read back by ExecuteMapChange
static INDEX _iPendingMapChangeRound = -1;
static INDEX _iMapVoteTargetRound = -1;

static INDEX CountActiveClients() {
    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();
    INDEX ctActive = 0;
    for (INDEX i = 1; i < ctSessions; i++) {
        if (_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) ctActive++;
    }
    return ctActive;
}

static void ResetMapVote() {
    _bMapVoteActive = FALSE;
    memset(_abMapVotedYes, 0, sizeof(_abMapVotedYes));
    memset(_abMapVotedNo, 0, sizeof(_abMapVotedNo));
    _iMapVoteGeneration++;
}

static void BroadcastVoteMessage(const CTString& strSender, const CTString& strMsgEN, const CTString& strMsgRU) {
    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();
    for (INDEX i = 1; i < ctSessions; i++) {
        if (!_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
        INetwork::SendChatToClient(i, strSender, SelectLangMessage(i, strMsgEN, strMsgRU));
    }
}

// Also refactor your existing KickClientLang's iClient==-1 branch to call
// KickAllClientsLang above, instead of its own duplicate loop:
//
// void KickClientLang(SHELL_FUNC_ARGS) {
//     BEGIN_SHELL_FUNC;
//     INDEX iClient = NEXT_ARG(INDEX);
//     const CTString& strReasonEN = *NEXT_ARG(CTString*);
//     const CTString& strReasonRU = *NEXT_ARG(CTString*);
//     if (!_pNetwork->IsServer()) return;
//     if (iClient == -1) { KickAllClientsLang(strReasonEN, strReasonRU); return; }
//     INetwork::SendDisconnectMessage(iClient, SelectLangMessage(iClient, strReasonEN, strReasonRU), FALSE);
// };

// Plain C++ kick-everyone helper, shared by KickClientLang(-1, ...) and the
// native map-change sequence below â€” avoids going through shell-arg marshaling.
static void KickAllClientsLang(const CTString& strReasonEN, const CTString& strReasonRU) {
    if (!_pNetwork->IsServer()) return;
    const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();
    for (INDEX i = 1; i < ctSessions; i++) {
        if (!_pNetwork->ga_srvServer.srv_assoSessions[i].sso_bActive) continue;
        INetwork::SendDisconnectMessage(i, SelectLangMessage(i, strReasonEN, strReasonRU), FALSE);
    }
}

// Doubles every backslash so a real path with single backslashes survives
// exactly one shell-parse pass intact (this is the ONLY place that still goes
// through _pShell->Execute for the final ded_strLevel/nextmap line).
static CTString EscapeBackslashesForShell(const CTString& strPath) {
    char szBuf[512];
    INDEX iOut = 0;
    const char* p = strPath.str_String;
    while (*p && iOut < (INDEX)sizeof(szBuf) - 2) {
        if (*p == '\\') { szBuf[iOut++] = '\\'; szBuf[iOut++] = '\\'; }
        else { szBuf[iOut++] = *p; }
        p++;
    }
    szBuf[iOut] = '\0';
    return CTString(szBuf);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Vote display formatting (1111) â€” replaces TallyMapVote's progress branch,
// PerformMapChange, and ExecuteMapChange's kick reason from MapVote_v2.cpp.
// Everything else (VoteForMap, CastMapVote, MapVoteTimeoutCheck, helpers) is
// unchanged â€” only these three pieces.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// "^cffff" / "^cff0000" used as bare sender strings â€” invisible name, colon
// stays visible. Matches the convention you already confirmed works.

// 5 separate messages so the chat window's natural 5-line display gets fully
// replaced each cast, instead of scrolling new lines under old ones.
static void BroadcastVoteTally(INDEX ctYes, INDEX ctNo) {
    const CTString strSender = "^cffff";

    BroadcastVoteMessage("^CServer", " ", " ");

    CTString strTitleEN, strTitleRU;
    strTitleEN.PrintF("^c00ff80Vote for ^c80ffff%s^c00ff80:^r", _strMapVoteNameEN.str_String);
    strTitleRU.PrintF("^c00ff80Ãîëîñîâàíèå çà ^c80ffff%s^c00ff80:^r", _strMapVoteNameRU.str_String);
    BroadcastVoteMessage(strSender, strTitleEN, strTitleRU);
	
	BroadcastVoteMessage(strSender,
        "^c00ff80To vote for map change^r",
        "^c00ff80×òîáû ïðîãîëîñîâàòü çà ñìåíó êàðòû^r");

    BroadcastVoteMessage(strSender,
        "^c00ff80Type ^cefefefyes ^c00ff80or ^cefefefno^r",
        "^c00ff80Ââåäèòå ^cefefefäà ^c00ff80èëè ^cefefefíåò^r");

    CTString strTallyEN, strTallyRU;
    strTallyEN.PrintF("^cffff80Yes:^c80ffff %d ^cffff80No:^c80ffff %d^r", (int)ctYes, (int)ctNo);
    strTallyRU.PrintF("^cffff80Äà:^c80ffff %d ^cffff80Íåò:^c80ffff %d^r", (int)ctYes, (int)ctNo);
    BroadcastVoteMessage(strSender, strTallyEN, strTallyRU);
}

static void PerformMapChange(const CTString& strLevelPath, const CTString& strNameEN, const CTString& strNameRU, INDEX iTargetRound) {
    const CTString strSender = "^cff0000";

    CTString strChangingEN, strChangingRU;
    strChangingEN.PrintF("^c00ff80The map is changing to ^c80ffff%s^r", strNameEN.str_String);
    strChangingRU.PrintF("^c00ff80Êàðòà ìåíÿåòñÿ íà ^c80ffff%s^r", strNameRU.str_String);
    BroadcastVoteMessage(strSender, strChangingEN, strChangingRU);

    BroadcastVoteMessage(strSender,
        "^c3dff9dServer will restart in^r",
        "^c3dff9dÑåðâåð ïåðåçàãðóçèòñÿ ÷åðåç^r");

    IScriptScheduler::Schedule(1.0f, "SayToAllExcept(0, \"^cff0000\", \"^c66ffb3^a55^f6* ^A^F3 ^a55^f6*^r\");");
    IScriptScheduler::Schedule(2.0f, "SayToAllExcept(0, \"^cff0000\", \"^c66ffb3^a55^f6* ^A^F2 ^a55^f6*^r\");");
    IScriptScheduler::Schedule(3.0f, "SayToAllExcept(0, \"^cff0000\", \"^c66ffb3^a55^f6* ^A^F1 ^a55^f6*^r\");");
    IScriptScheduler::Schedule(3.5f, "SayToAllExceptLang(0, \"^cff0000\", \"^c66ffb3Tip: Wait 5 seconds from now before pressing F9^r\", \"^c66ffb3Ñîâåò: Ïîäîæäèòå 5 ñåêóíä ñ äàííîãî ìîìåíòà è íàæìèòå F9^r\");");

    _strPendingMapChangeLevel = strLevelPath;
    _iPendingMapChangeRound = iTargetRound;       // 1111 store round alongside level

    IScriptScheduler::Schedule(4.0f, "ExecuteMapChange();");
}

void ExecuteMapChange(SHELL_FUNC_ARGS) {
    if (!_pNetwork->IsServer()) return;

    KickAllClientsLang(
        "\n^c45ffb8RESTARTING SERVER^r",
        "\n^c45ffb8ÏÅÐÅÇÀÃÐÓÇÊÀ ÑÅÐÂÅÐÀ^r");

    CTString strFinal;
    strFinal.PrintF("ded_iTargetRound=%d;ded_strLevel=\"%s\";nextmap();",
        _iPendingMapChangeRound,
        EscapeBackslashesForShell(_strPendingMapChangeLevel).str_String);
    _pShell->Execute(strFinal);
    _iPendingMapChangeRound = -1;
};

// Shared by VoteForMap (after starting + auto-yes) and CastMapVote.
static void TallyMapVote() {
    INDEX ctYes = 0, ctNo = 0;
    for (INDEX i = 1; i <= PLAYERDB_MAX_PLAYERS; i++) {
        if (_abMapVotedYes[i]) ctYes++;
        if (_abMapVotedNo[i])  ctNo++;
    }
    const INDEX ctActive = CountActiveClients();
    const INDEX ctNeeded = (ctActive / 2) + 1;  // strict majority of yes votes

    if (ctYes >= ctNeeded) {
        const CTString strLevel = _strMapVoteLevel;  // copy before reset clears it
        const CTString strNameEN = _strMapVoteNameEN;
        const CTString strNameRU = _strMapVoteNameRU;
        ResetMapVote();
        PerformMapChange(strLevel, strNameEN, strNameRU, _iMapVoteTargetRound);
        return;
    }

    // Everyone who could vote has voted, yes still short â€” conclude now instead
    // of waiting out the rest of the timeout for an outcome that can't change.
    if (ctYes + ctNo >= ctActive) {
        CTString strMsgEN, strMsgRU;
        strMsgEN.PrintF("^c00ff80Vote to change map to %s failed", _strMapVoteNameEN.str_String);
        strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå çà êàðòó %s íå ïðîøëî", _strMapVoteNameRU.str_String);
        ResetMapVote();
        BroadcastVoteMessage("^cffff", strMsgEN, strMsgRU);
        return;
    }

    CTString strMsgEN, strMsgRU;
    BroadcastVoteTally(ctYes, ctNo);
}

// VoteForMap(iClient, strMapNameEN, strMapNameRU, strLevelPath) â€” starts a vote
// only. If one's already running, tells the caller to use yes/no instead.
void VoteForMap(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX            iClient = NEXT_ARG(INDEX);
    const CTString& strMapNameEN = *NEXT_ARG(CTString*);
    const CTString& strMapNameRU = *NEXT_ARG(CTString*);
    const CTString& strLevelPath = *NEXT_ARG(CTString*);
    INDEX           iTargetRound = NEXT_ARG(INDEX);    // 1111 new param

    if (!_pNetwork->IsServer()) return;
    if (iClient < 1 || iClient > PLAYERDB_MAX_PLAYERS) return;

    if (_bMapVoteActive) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80A map vote is already in progress - type yes or no to vote",
            "^c00ff80Ãîëîñîâàíèå óæå èä¸ò - íàïèøèòå äà èëè íåò, ÷òîáû ïðîãîëîñîâàòü"));
        return;
    }

    _bMapVoteActive = TRUE;
    _strMapVoteNameEN = strMapNameEN;
    _strMapVoteNameRU = strMapNameRU;
    _strMapVoteLevel = strLevelPath;
    _iMapVoteTargetRound = iTargetRound;
    memset(_abMapVotedYes, 0, sizeof(_abMapVotedYes));
    memset(_abMapVotedNo, 0, sizeof(_abMapVotedNo));
    _iMapVoteGeneration++;
    _abMapVotedYes[iClient] = TRUE;  // starter auto-votes yes

    CTString strSchedule;
    strSchedule.PrintF("MapVoteTimeoutCheck(%d);", (int)_iMapVoteGeneration);
    IScriptScheduler::Schedule(MAPVOTE_TIMEOUT_SECONDS, strSchedule);

    TallyMapVote();
};

// CastMapVote(iClient, bYes) â€” called by bare "yes"/"äà"/"no"/"íåò" chat handlers.
void CastMapVote(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);
    INDEX bYes = NEXT_ARG(INDEX);

    if (!_pNetwork->IsServer()) return;
    if (!_bMapVoteActive) return;
    if (iClient < 1 || iClient > PLAYERDB_MAX_PLAYERS) return;

    if (bYes) _abMapVotedYes[iClient] = TRUE;
    else      _abMapVotedNo[iClient] = TRUE;

    TallyMapVote();
};

void MapVoteTimeoutCheck(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iGeneration = NEXT_ARG(INDEX);

    if (!_pNetwork->IsServer()) return;
    if (!_bMapVoteActive) return;
    if (iGeneration != _iMapVoteGeneration) return;  // a newer vote already replaced this one

    CTString strMsgEN, strMsgRU;
    strMsgEN.PrintF("^c00ff80Vote to change map to ^c80ffff%s ^c00ff80timed out", _strMapVoteNameEN.str_String);
    strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå çà êàðòó ^c80ffff%s ^c00ff80èñòåêëî ïî âðåìåíè", _strMapVoteNameRU.str_String);
    ResetMapVote();
    BroadcastVoteMessage("^cffff", strMsgEN, strMsgRU);
};

#define PLAYERVOTE_TIMEOUT_SECONDS  60.0f
#define PLAYERVOTE_BAN_MINUTES      20
 
static BOOL     _bPlayerVoteActive     = FALSE;  // 1111
static INDEX    _iPlayerVoteTarget     = -1;     // 1111: session slot of the target
static BOOL     _bPlayerVoteIsBan      = FALSE;  // 1111: TRUE = ban, FALSE = kick
static CTString _strPlayerVoteName     = "";     // 1111: display name stored at vote start
static BOOL     _abPlayerVotedYes[PLAYERDB_MAX_PLAYERS + 1]; // 1111: indexed by client slot
static BOOL     _abPlayerVotedNo [PLAYERDB_MAX_PLAYERS + 1]; // 1111
static INDEX    _iPlayerVoteGeneration = 0;      // 1111: invalidates stale timeout callbacks
 
// Returns the in-game character name for a connected client slot.    1111
static CTString GetNameForClient(INDEX iClient) {
    if (!_pNetwork->IsServer()) return CTString("");
 
    CServer &srv = _pNetwork->ga_srvServer;
    for (INDEX i = 0; i < srv.srv_aplbPlayers.Count(); i++) {
        CPlayerBuffer &plb = srv.srv_aplbPlayers[i];
        if (!plb.IsActive() || plb.plb_iClient != iClient) continue;
        return plb.plb_pcCharacter.GetNameForPrinting();
    }
    return CTString("");
}
 
static void ResetPlayerVote() {  // 1111
    _bPlayerVoteActive = FALSE;
    _iPlayerVoteTarget = -1;
    _bPlayerVoteIsBan  = FALSE;
    memset(_abPlayerVotedYes, 0, sizeof(_abPlayerVotedYes));
    memset(_abPlayerVotedNo,  0, sizeof(_abPlayerVotedNo));
    _iPlayerVoteGeneration++;
}
 
static void BroadcastPlayerVoteTally(INDEX ctYes, INDEX ctNo) {  // 1111
    const CTString strSender = "^cffff";
 
    BroadcastVoteMessage("^CServer", " ", " ");
 
    CTString strTitleEN, strTitleRU;
    if (_bPlayerVoteIsBan) {
        strTitleEN.PrintF("^c00ff80Vote to ban ^cffda59%s^c00ff80 for 20 minutes:^r",            _strPlayerVoteName.Undecorated().str_String);
        strTitleRU.PrintF("^c00ff80Ãîëîñîâàíèå: çàáàíèòü ^cffda59%s^c00ff80 íà 20 ìèíóò:^r",  _strPlayerVoteName.Undecorated().str_String);
    } else {
        strTitleEN.PrintF("^c00ff80Vote to kick ^cffda59%s^c00ff80:^r",            _strPlayerVoteName.Undecorated().str_String);
        strTitleRU.PrintF("^c00ff80Ãîëîñîâàíèå: êèêíóòü ^cffda59%s^c00ff80:^r",   _strPlayerVoteName.Undecorated().str_String);
    }
    BroadcastVoteMessage(strSender, strTitleEN, strTitleRU);
 
    if (_bPlayerVoteIsBan) {
		BroadcastVoteMessage(strSender,
        "^c00ff80To vote for player ban^r",
        "^c00ff80×òîáû ïðîãîëîñîâàòü çà áàí èãðîêà^r");
	} else {
		BroadcastVoteMessage(strSender,
        "^c00ff80To vote for player kick^r",
        "^c00ff80×òîáû ïðîãîëîñîâàòü çà êèê èãðîêà^r");
	}
		
    BroadcastVoteMessage(strSender,
        "^c00ff80Type ^cefefefyes ^c00ff80or ^cefefefno^r",
        "^c00ff80Ââåäèòå ^cefefefäà ^c00ff80èëè ^cefefefíåò^r");

    CTString strTallyEN, strTallyRU;
    strTallyEN.PrintF("^cffff80Yes:^c80ffff %d ^cffff80No:^c80ffff %d^r", (int)ctYes, (int)ctNo);
    strTallyRU.PrintF("^cffff80Äà:^c80ffff %d ^cffff80Íåò:^c80ffff %d^r", (int)ctYes, (int)ctNo);
    BroadcastVoteMessage(strSender, strTallyEN, strTallyRU);
}
 
static void TallyPlayerVote() {  // 1111
    INDEX ctYes = 0, ctNo = 0;
    for (INDEX i = 1; i <= PLAYERDB_MAX_PLAYERS; i++) {
        if (_abPlayerVotedYes[i]) ctYes++;
        if (_abPlayerVotedNo[i])  ctNo++;
    }
    const INDEX ctActive = CountActiveClients();
    const INDEX ctNeeded = (ctActive / 2) + 1;
 
    // â”€â”€ Passed â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (ctYes >= ctNeeded) {
        const INDEX    iTarget = _iPlayerVoteTarget;
        const BOOL     bBan    = _bPlayerVoteIsBan;
        const CTString strName = _strPlayerVoteName;
        ResetPlayerVote();
 
        CTString strMsgEN, strMsgRU;
        if (bBan) {
            strMsgEN.PrintF("^c00ff80Vote passed: ^cffda59%s^c00ff80 is banned for %d minutes", strName.Undecorated().str_String, PLAYERVOTE_BAN_MINUTES);
            strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå: ^cffda59%s^c00ff80 çàáàíåí íà %d ìèíóò",      strName.Undecorated().str_String, PLAYERVOTE_BAN_MINUTES);
        } else {
            strMsgEN.PrintF("^c00ff80Vote passed: ^cffda59%s^c00ff80 was kicked", strName.Undecorated().str_String);
            strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå: ^cffda59%s^c00ff80 êèêíóò",     strName.Undecorated().str_String);
        }
        BroadcastVoteMessage("^cff0000", strMsgEN, strMsgRU);
 
        if (bBan) {
            CTString strExec;
            strExec.PrintF("BanClientTemp(%d, %d);", (int)iTarget, (int)PLAYERVOTE_BAN_MINUTES);
            _pShell->Execute(strExec);
        } else {
            INetwork::SendDisconnectMessage(
                iTarget,
                SelectLangMessage(iTarget,
                    "\n^cf8f644KICKED BY VOTE^r",
                    "\n^cf8f644ÊÈÊÍÓÒ ÃÎËÎÑÎÂÀÍÈÅÌ^r"),
                FALSE);
        }
        return;
    }
 
    // â”€â”€ All voted, majority was no â€” conclude early â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (ctYes + ctNo >= ctActive) {
        CTString strMsgEN, strMsgRU;
        if (_bPlayerVoteIsBan) {
            strMsgEN.PrintF("^c00ff80Vote to ban ^cffda59%s^c00ff80 failed",              _strPlayerVoteName.Undecorated().str_String);
            strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå (çàáàíèòü ^cffda59%s^c00ff80) íå ïðîøëî", _strPlayerVoteName.Undecorated().str_String);
        } else {
            strMsgEN.PrintF("^c00ff80Vote to kick ^cffda59%s^c00ff80 failed",             _strPlayerVoteName.Undecorated().str_String);
            strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå (êèêíóòü ^cffda59%s^c00ff80) íå ïðîøëî",  _strPlayerVoteName.Undecorated().str_String);
        }
        ResetPlayerVote();
        BroadcastVoteMessage("^cffff", strMsgEN, strMsgRU);
        return;
    }
 
    // â”€â”€ Still pending â€” refresh the tally display â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    BroadcastPlayerVoteTally(ctYes, ctNo);
}
 
static void StartPlayerVote(INDEX iClient, INDEX iTarget, BOOL bBan) {  // 1111
    if (!_pNetwork->IsServer()) return;
    if (iClient < 1 || iClient > PLAYERDB_MAX_PLAYERS) return;
 
    // Slot 0 is the server â€” always protected.
    if (iTarget < 1 || iTarget > PLAYERDB_MAX_PLAYERS) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80Invalid index - use ^cefefef@list^c00ff80 to see active players",
            "^c00ff80Íåâåðíûé èíäåêñ - èñïîëüçóéòå ^cefefef@list^c00ff80 äëÿ ïðîñìîòðà ñïèñêà"));
        return;
    }
 
    CServer &srv = _pNetwork->ga_srvServer;
    if (iTarget >= (INDEX)srv.srv_assoSessions.Count() || !srv.srv_assoSessions[iTarget].sso_bActive) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80That slot is not active - use ^cefefef@list^c00ff80 to refresh",
            "^c00ff80Ýòîò ñëîò íå àêòèâåí - èñïîëüçóéòå ^cefefef@list^c00ff80 äëÿ îáíîâëåíèÿ ñïèñêà"));
        return;
    }
 
    if (iTarget == iClient) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80You cannot vote to kick/ban yourself",
            "^c00ff80Íåëüçÿ ãîëîñîâàòü çà êèê/áàí ñàìîãî ñåáÿ"));
        return;
    }
 
    if (_bMapVoteActive || _bPlayerVoteActive) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80A vote is already in progress - type yes or no",
            "^c00ff80Ãîëîñîâàíèå óæå èä¸ò - íàïèøèòå äà èëè íåò"));
        return;
    }
 
    _bPlayerVoteActive = TRUE;
    _iPlayerVoteTarget = iTarget;
    _bPlayerVoteIsBan  = bBan;
    _strPlayerVoteName = GetNameForClient(iTarget);
    memset(_abPlayerVotedYes, 0, sizeof(_abPlayerVotedYes));
    memset(_abPlayerVotedNo,  0, sizeof(_abPlayerVotedNo));
    _iPlayerVoteGeneration++;
    _abPlayerVotedYes[iClient] = TRUE;  // initiator auto-votes yes
 
    CTString strSchedule;
    strSchedule.PrintF("PlayerVoteTimeoutCheck(%d);", (int)_iPlayerVoteGeneration);
    IScriptScheduler::Schedule(PLAYERVOTE_TIMEOUT_SECONDS, strSchedule);
 
    TallyPlayerVote();
}
 
// ListClients(iTo) â€” sends the numbered active-player roster to client iTo.    1111
// Triggered by @list in chat. Output only goes to the requester.
void ListClients(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iTo = NEXT_ARG(INDEX);
 
    if (!_pNetwork->IsServer()) return;
 
    CServer &srv = _pNetwork->ga_srvServer;
    const INDEX ctSessions = (INDEX)srv.srv_assoSessions.Count();
    BOOL bAny = FALSE;
 
    for (INDEX i = 1; i < ctSessions; i++) {
        if (!srv.srv_assoSessions[i].sso_bActive) continue;
        CTString strLine;
        strLine.PrintF("^cffff90%d ^c888888- ^cffda59%s^r", (int)i, GetNameForClient(i).Undecorated().str_String);
        INetwork::SendChatToClient(iTo, "^ced2675", strLine);
        bAny = TRUE;
    }
 
    if (!bAny) {
        INetwork::SendChatToClient(iTo, "^ced2675", SelectLangMessage(iTo,
            "^cffda59(no other active players)^r",
            "^cffda59(íåò äðóãèõ àêòèâíûõ èãðîêîâ)^r"));
        return;
    }
}
 
void VoteForKick(SHELL_FUNC_ARGS) {  // 1111
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);
    INDEX iTarget = NEXT_ARG(INDEX);
    StartPlayerVote(iClient, iTarget, FALSE);
}
 
void VoteForBan(SHELL_FUNC_ARGS) {  // 1111
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);
    INDEX iTarget = NEXT_ARG(INDEX);
    StartPlayerVote(iClient, iTarget, TRUE);
}

// BanClientTemp(iClient, iBanMinutes) â€” 1111
// Bans a *currently connected* client by live slot. Resolves the slot to its
// persistent identity (same IP/GUID matching used by the client log) via
// _aClientIdentities, then reuses CClientRestriction::BanClient so the ban
// lands in the same list as !ban and ClientLogBan, and every live session
// under that identity gets disconnected (admins excluded automatically).
void BanClientTemp(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iClient     = NEXT_ARG(INDEX);
  INDEX iBanMinutes = NEXT_ARG(INDEX);

  if (!_pNetwork->IsServer()) return;

  const INDEX ctSessions = _pNetwork->ga_srvServer.srv_assoSessions.Count();

  if (iClient < 1 || iClient >= ctSessions || !_pNetwork->ga_srvServer.srv_assoSessions[iClient].sso_bActive) {
    CPutString(TRANS("BanClientTemp: Invalid or inactive client!\n"));
    return;
  }

  if (iBanMinutes <= 0) {
    CPutString(TRANS("BanClientTemp: Ban time must be positive (use ClientLogBan with -1 for a permanent ban).\n"));
    return;
  }

  CClientIdentity *pci = _aActiveClients[iClient].pClient;
  INDEX iIdentity = (pci != NULL) ? _aClientIdentities.GetIndex(pci) : -1;

  if (iIdentity == -1) {
    CPutString(TRANS("BanClientTemp: Couldn't find client identity in the log!\n"));
    return;
  }

  CTString strResult = CClientRestriction::BanClient(iIdentity, (DOUBLE)iBanMinutes * 60.0);
  CPutString(strResult + "\n");
};
 
// CastPlayerVote(iClient, bYes) â€” safe to call alongside CastMapVote in the    1111
// ini; silently no-ops when no player vote is active.
void CastPlayerVote(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);
    INDEX bYes    = NEXT_ARG(INDEX);
 
    if (!_pNetwork->IsServer()) return;
    if (!_bPlayerVoteActive) return;
    if (iClient < 1 || iClient > PLAYERDB_MAX_PLAYERS) return;
 
    if (bYes) _abPlayerVotedYes[iClient] = TRUE;
    else      _abPlayerVotedNo [iClient] = TRUE;
 
    TallyPlayerVote();
}
 
void PlayerVoteTimeoutCheck(SHELL_FUNC_ARGS) {  // 1111
    BEGIN_SHELL_FUNC;
    INDEX iGeneration = NEXT_ARG(INDEX);
 
    if (!_pNetwork->IsServer()) return;
    if (!_bPlayerVoteActive) return;
    if (iGeneration != _iPlayerVoteGeneration) return;
 
    CTString strMsgEN, strMsgRU;
    if (_bPlayerVoteIsBan) {
        strMsgEN.PrintF("^c00ff80Vote to ban ^cffda59%s^c00ff80 timed out",           _strPlayerVoteName.Undecorated().str_String);
        strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå (çàáàíèòü ^cffda59%s^c00ff80) èñòåêëî ïî âðåìåíè",          _strPlayerVoteName.Undecorated().str_String);
    } else {
        strMsgEN.PrintF("^c00ff80Vote to kick ^cffda59%s^c00ff80 timed out",           _strPlayerVoteName.Undecorated().str_String);
        strMsgRU.PrintF("^c00ff80Ãîëîñîâàíèå (êèêíóòü ^cffda59%s^c00ff80) èñòåêëî ïî âðåìåíè",           _strPlayerVoteName.Undecorated().str_String);
    }
    ResetPlayerVote();
    BroadcastVoteMessage("^cffff", strMsgEN, strMsgRU);
}

/* ---- ColorizeNick_Impl / ColorizeNick ----------------------------------- *
 * Walks all ^cRRGGBB codes in a decorated name, averages their R/G/B,      *
 * strips all decoration tags, and returns "^cRRGGBB<plain name>".           *
 * Falls back to "^cffffff<plain>" when the name carries no colour codes,    *
 * so callers always get a colour-prefixed string and can concatenate safely. *
 * Shell:  CTString ColorizeNick(CTString)                                   *
 * ------------------------------------------------------------------------- */
CTString ColorizeNick_Impl(const CTString& strIn) {                /* 1111 */
    const char* p = strIn.str_String;
    ULONG ulSumR = 0, ulSumG = 0, ulSumB = 0;
    INDEX ctColors = 0;

    while (*p != '\0') {
        if (p[0] != '^') { p++; continue; }

        switch (p[1]) {
        case 'c': {
            INDEX ctHex = FindZero((UBYTE*)p + 2, 6);
            if (ctHex == 6) {
                char szHex[7];
                memcpy(szHex, p + 2, 6); szHex[6] = '\0';
                char* pEnd;
                ULONG ulRGB = strtoul(szHex, &pEnd, 16);
                if (pEnd == szHex + 6) {
                    ulSumR += (ulRGB >> 16) & 0xFF;
                    ulSumG += (ulRGB >> 8) & 0xFF;
                    ulSumB += (ulRGB) & 0xFF;
                    ctColors++;
                }
            }
            p += 2 + ctHex;
        } break;
        case 'a': p += 2 + FindZero((UBYTE*)p + 2, 2); break;
        case 'f': p += 2 + FindZero((UBYTE*)p + 2, 1); break;
        case 'b': case 'i': case 'r': case 'o':
        case 'C': case 'A': case 'F': case 'B': case 'I': p += 2; break;
        case '^': p += 2; break;
        default:  p++;    break;   /* unrecognized: skip only the '^' */
        }
    }

    CTString strBare = strIn.Undecorated();

    /* No colour codes -> use white so concatenation is colour-safe */
    
    if (ctColors == 0) {
        return ser_strDefaultNickColor + strBare;
    }

    CTString strResult;
    strResult.PrintF("^c%02x%02x%02x%s",
        (unsigned)(ulSumR / ctColors),
        (unsigned)(ulSumG / ctColors),
        (unsigned)(ulSumB / ctColors),
        strBare.str_String);
    return strResult;
}

static CTString ColorizeNickFunc(SHELL_FUNC_ARGS) {                /* 1111 */
    BEGIN_SHELL_FUNC;
    const CTString& strIn = *NEXT_ARG(CTString*);
    return ColorizeNick_Impl(strIn);
}