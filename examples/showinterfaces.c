/*
  showinterfaces: a simple test program to show the network interfaces
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_net.h"

#ifdef __DREAMCAST__
#include <kos/init.h>
#include <kos/dbgio.h>
#include <kos/dbglog.h>
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
#endif

#define MAX_ADDRESSES   10
#ifdef __DREAMCAST__
#define ADDRESS_OCTET(address, shift) (int)(((address) >> (shift)) & 0xFF)
#else
#define ADDRESS_OCTET(address, shift) (((address) >> (shift)) & 0xFF)
#endif

int main(int argc, char *argv[])
{
    IPaddress addresses[MAX_ADDRESSES];
    int i, count;

    (void) argc;
    (void) argv;
#ifdef __DREAMCAST__
    dbgio_dev_select("fb"); 
#endif
    if (SDLNet_Init() < 0) {
#ifdef __DREAMCAST__
        dbglog(DBG_INFO, 
                     "Couldn't initialize net: %s\n",
                     SDLNet_GetError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't initialize net: %s\n",
                     SDLNet_GetError());
#endif
        return 1;
    }

    count = SDLNet_GetLocalAddresses(addresses, MAX_ADDRESSES);
#ifdef __DREAMCAST__
    dbglog(DBG_INFO, "Found %d local addresses", count);
#else
    SDL_Log("Found %d local addresses", count);
#endif
    for (i = 0; i < count; ++i) {
#ifdef __DREAMCAST__
        dbglog(DBG_INFO, "%d: %d.%d.%d.%d - %s", i+1,
#else
        SDL_Log("%d: %d.%d.%d.%d - %s", i+1,
#endif
            ADDRESS_OCTET(addresses[i].host, 0),
            ADDRESS_OCTET(addresses[i].host, 8),
            ADDRESS_OCTET(addresses[i].host, 16),
            ADDRESS_OCTET(addresses[i].host, 24),
            SDLNet_ResolveIP(&addresses[i]));
    }
#ifdef __DREAMCAST__
    SDL_Event event;
    int running = 1;
    while (running) { 
        while (SDL_PollEvent(&event)) { 
            if (event.type == SDL_QUIT) { 
                running = 0;
            } 
        }
    }
#endif
    SDLNet_Quit();

    return 0;
}
