/* PublicBrowse.h — @browse chat command: FE+SE public server list via 333networks
 *
 * Declares shell symbols BrowsePublicServers(INDEX) and BrowseCheckResult(INDEX).
 * Call PublicBrowse_Init() once from INetwork::Initialize(), next to GeoIP_Init().
 */

#ifndef CECIL_INCL_PUBLICBROWSE_H
#define CECIL_INCL_PUBLICBROWSE_H

#ifdef PRAGMA_ONCE
  #pragma once
#endif

void PlayersBrowse_Init(void);

#endif
