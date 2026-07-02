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

#include "QueryManager.h"

#include "Query/PlayerDB.h" //  1111
#include "Query/DemoManager.h" //  1111

#pragma comment(lib, "wsock32.lib")

#if _PATCHCONFIG_NEW_QUERY

static WSADATA *_wsaData = NULL;
// static sockaddr_in *_sin = NULL;
static sockaddr_in* _sin = NULL;   // 333networks.com   1111
static sockaddr_in* _sin2 = NULL;  // 42amsterdam.net   1111
static sockaddr_in *_sinLocal = NULL;

static SOCKET _socket = INVALID_SOCKET;

// Current master server protocol
INDEX ms_iProtocol = E_MS_LEGACY;

// Available protocols
IAbstractProtocol *_aProtocols[E_MS_MAX];

// Debug output for query
INDEX ms_bDebugOutput = FALSE;

// Hook old master server address instead of replacing entire query manager
INDEX ms_bVanillaQuery = FALSE;

// Commonly used symbols
CSymbolPtr _piNetPort;
CSymbolPtr _pstrLocalHost;

// Replace internal GameSpy master server addresses with Legacy ones
extern void UpdateInternalGameSpyMS(INDEX)
{
  static char *astrAddresses[3] = {
    (char *)ADDR_GAMESPY_MS_1,
    (char *)ADDR_GAMESPY_MS_2,
    (char *)ADDR_GAMESPY_MS_3,
  };

  // Maximum address length
  static const size_t ctAddress = strlen("master.gamespy.com");

  for (INDEX i = 0; i < 3; i++) {
    const CTString &strMS = _aProtocols[E_MS_LEGACY]->GetMS();
    strncpy(astrAddresses[i], strMS.str_String, ctAddress);
  }
};

ILegacy::ILegacy() {
  // Register master server address
  m_strMS = "333networks.com";
  _pShell->DeclareSymbol("persistent user CTString ms_strLegacyMS post:UpdateInternalGameSpyMS;", &m_strMS);
};

IDarkPlaces::IDarkPlaces() {
  // Register master server address
  m_strMS = "192.168.1.4";
  _pShell->DeclareSymbol("persistent user CTString ms_strDarkPlacesMS;", &m_strMS);
};

IGameAgent::IGameAgent() {
  // Register master server address
  m_strMS = "333networks.com";
  _pShell->DeclareSymbol("persistent user CTString ms_strGameAgentMS;", &m_strMS);
};

// Initialize query manager
extern void InitQuery(void) {
  // Custom symbols
  _pShell->DeclareSymbol("void UpdateInternalGameSpyMS(INDEX);", &UpdateInternalGameSpyMS);
  _pShell->DeclareSymbol("persistent user INDEX ms_iProtocol;",          &ms_iProtocol);
  _pShell->DeclareSymbol("persistent user INDEX ms_bDebugOutput;",       &ms_bDebugOutput);

  _pShell->DeclareSymbol("persistent user INDEX ms_bVanillaQuery pre:UpdateServerSymbolValue;", &ms_bVanillaQuery);

  // Master server protocol types
  static const INDEX iLegacyMS     = E_MS_LEGACY;
  static const INDEX iDarkPlacesMS = E_MS_DARKPLACES;
  static const INDEX iGameAgentMS  = E_MS_GAMEAGENT;
  _pShell->DeclareSymbol("const INDEX MS_LEGACY;",     (void *)&iLegacyMS);
  _pShell->DeclareSymbol("const INDEX MS_DARKPLACES;", (void *)&iDarkPlacesMS);
  _pShell->DeclareSymbol("const INDEX MS_GAMEAGENT;",  (void *)&iGameAgentMS);

  // Create protocol interfaces
  _aProtocols[E_MS_LEGACY]     = new ILegacy;
  _aProtocols[E_MS_DARKPLACES] = new IDarkPlaces;
  _aProtocols[E_MS_GAMEAGENT]  = new IGameAgent;

  // Retrieve commonly used symbols
  _piNetPort.Find("net_iPort");
  _pstrLocalHost.Find("net_strLocalHost");

  PlayerDB_Init();      // 1111
  DemoManager_Init();   // 1111
};

namespace IQuery {

sockaddr_in sinFrom;
char *pBuffer = NULL;

BOOL bServer = FALSE;
BOOL bInitialized = FALSE;

CDynamicStackArray<SServerRequest> aRequests;

// Add new server request from a received address
void Address::AddServerRequest(const char **ppBuffer, INDEX &iLength, const UWORD uwSetPort, const char *strPacket, SOCKET iSocketUDP) {
  const INDEX iAddrLength = 6; // IQuery::Address struct size

  // If valid port and at least one valid address byte
  if (uwPort != 0 && ulIP != 0xFFFFFFFF) {
    // Make an address string
    CTString strIP;
    strIP.PrintF("%u.%u.%u.%u", aIP[0], aIP[1], aIP[2], aIP[3]);

    // Socket address for the server
    sockaddr_in sinServer;
    sinServer.sin_family = AF_INET;
    sinServer.sin_addr.s_addr = inet_addr(strIP);
    sinServer.sin_port = uwSetPort;

    // Add a new server status request
    SServerRequest::AddRequest(sinServer);

    // Send packet to the server
    SendPacketTo(&sinServer, strPacket, (int)strlen(strPacket), iSocketUDP);
  }

  // Get next address
  *ppBuffer += iAddrLength;
  iLength -= iAddrLength;
};

// Initialize the socket    1111
void InitWinsock(void) {
    if (_wsaData != NULL && _socket != INVALID_SOCKET) {
        return;
    }

    _wsaData = new WSADATA;
    _socket = INVALID_SOCKET;

    if (WSAStartup(MAKEWORD(2, 2), _wsaData) != 0) {
        CPutString("Error initializing winsock!\n");
        CloseWinsock();
        return;
    }

    if (pBuffer != NULL) {
        delete[] pBuffer;
    }
    pBuffer = new char[2050];

    // Get the port from the active protocol
    const UWORD uwPort = _aProtocols[IMasterServer::GetProtocol()]->GetPort();

    // Resolve primary master server (333networks.com)
    {
        hostent* phe = gethostbyname("333networks.com");
        if (phe == NULL) {
            CPutString("Couldn't resolve primary master server '333networks.com'\n");
        }
        else {
            _sin = new sockaddr_in;
            _sin->sin_family = AF_INET;
            _sin->sin_addr.s_addr = *(ULONG*)phe->h_addr_list[0];
            _sin->sin_port = htons(uwPort);
        }
    }

    // Resolve secondary master server (42amsterdam.net)
    {
        hostent* phe = gethostbyname("42amsterdam.net");
        if (phe == NULL) {
            CPutString("Couldn't resolve secondary master server '42amsterdam.net'\n");
        }
        else {
            _sin2 = new sockaddr_in;
            _sin2->sin_family = AF_INET;
            _sin2->sin_addr.s_addr = *(ULONG*)phe->h_addr_list[0];
            _sin2->sin_port = htons(uwPort);
        }
    }

    // Abort if neither resolved
    if (_sin == NULL && _sin2 == NULL) {
        CPutString("Couldn't resolve any master server — aborting socket init\n");
        CloseWinsock();
        return;
    }

    _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_socket == INVALID_SOCKET) {
        CloseWinsock();
        return;
    }

    if (bServer) {
        _sinLocal = new sockaddr_in;
        _sinLocal->sin_family = AF_INET;
        _sinLocal->sin_addr.s_addr = inet_addr("0.0.0.0");
        _sinLocal->sin_port = htons(_piNetPort.GetIndex() + 1);

        int iOpt = 1;
        if (setsockopt(_socket, SOL_SOCKET, SO_BROADCAST, (char*)&iOpt, sizeof(iOpt)) != 0) {
            CPutString("Couldn't allow receiving UDP broadcast for the socket!\n");
            CloseWinsock();
            return;
        }

        bind(_socket, (sockaddr*)_sinLocal, sizeof(*_sinLocal));
    }

    DWORD dwNonBlocking = 1;
    if (ioctlsocket(_socket, FIONBIO, &dwNonBlocking) != 0) {
        CPutString("Couldn't make socket non-blocking!\n");
        CloseWinsock();
    }
};

// Close the socket 1111
void CloseWinsock(void) {
    if (_socket != INVALID_SOCKET) {
        closesocket(_socket);
        _socket = INVALID_SOCKET;
    }

    if (_sin != NULL) {
        delete _sin;
        _sin = NULL;
    }

    if (_sin2 != NULL) {
        delete _sin2;
        _sin2 = NULL;
    }

    if (_wsaData != NULL) {
        delete _wsaData;
        _wsaData = NULL;
    }
};

// Check if the socket is usable
BOOL IsSocketUsable(void) {
  return (_socket != INVALID_SOCKET && bInitialized);
};

// Send packet with data from a buffer  1111
void SendPacket(const char* pBuffer, int iLength) {
    InitWinsock();

    if (iLength == -1) {
        iLength = (int)strlen(pBuffer);
    }

    if (_sin != NULL) {
        SendPacketTo(_sin, pBuffer, iLength);
    }

    if (_sin2 != NULL) {
        SendPacketTo(_sin2, pBuffer, iLength);
    }
};

// Send data packet to a specific socket address
void SendPacketTo(sockaddr_in *psin, const char *pBuffer, int iLength, SOCKET iSocket) {
  // Default to static one
  if (iSocket == INVALID_SOCKET) iSocket = _socket;

  sendto(iSocket, pBuffer, iLength, 0, (sockaddr *)psin, sizeof(sockaddr_in));
};

// Send reply packet with a message
void SendReply(const CTString &strMessage) {
  SendPacketTo(&sinFrom, strMessage.str_String, strMessage.Length());
};

// Receive some packet
int ReceivePacket(void) {
  socklen_t ctFrom = sizeof(sinFrom);
  return recvfrom(_socket, pBuffer, 2048, 0, (sockaddr *)&sinFrom, &ctFrom);
};

// Set enumeration status
void SetStatus(const CTString &strStatus) {
  _pNetwork->ga_bEnumerationChange = TRUE;
  _pNetwork->ga_strEnumerationStatus = strStatus;
};

}; // namespace

#endif // _PATCHCONFIG_NEW_QUERY
