/* The network for bug reports (bugreport_3ds.c) and updates (update_3ds.c). */
#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "net_3ds.h"
#include "github_ca.h"
#include "plat/plat.h"

#define CA_FILE_PATH "sdmc:/3ds/dkr/cacert.pem"

static LightLock sLock = 1; // Unlocked (LightLock_Init) before anything runs.
static int sUsers;
static u32 *sSocBuffer;

// The lowest priority a game thread gets is 0x3F (plat_3ds.c, which only the idle meter uses); this is just above.
#define BACKGROUND_PRIO 0x3E
static __thread s32 sSavedPrio = -1;

void net_3ds_background(int background) {
    s32 prio = 0;

    if (background && sSavedPrio < 0) {
        if (R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)) && prio < BACKGROUND_PRIO) {
            sSavedPrio = prio;
            svcSetThreadPriority(CUR_THREAD_HANDLE, BACKGROUND_PRIO);
        }
    } else if (!background && sSavedPrio >= 0) {
        svcSetThreadPriority(CUR_THREAD_HANDLE, sSavedPrio);
        sSavedPrio = -1;
    }
}

int net_3ds_begin(char *error, size_t errorSize) {
    static int sCurlReady;
    u32 wifi = 0;
    u32 socSize = 0x100000;
    int status = 0;

    if (R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi);
        acExit();
        if (wifi == 0) {
            snprintf(error, errorSize, "no Wi-Fi connection");
            return -1;
        }
    }
    LightLock_Lock(&sLock);
    if (sUsers == 0) {
        // The network buffer comes from the heap the game also uses; take less if a full one does not fit.
        while (sSocBuffer == NULL && socSize >= 0x40000) {
            sSocBuffer = memalign(0x1000, socSize);
            if (sSocBuffer == NULL) {
                socSize /= 2;
            }
        }
        if (sSocBuffer == NULL) {
            snprintf(error, errorSize, "not enough memory for the network");
            status = -1;
        } else if (R_FAILED(socInit(sSocBuffer, socSize))) {
            free(sSocBuffer);
            sSocBuffer = NULL;
            snprintf(error, errorSize, "could not start the network");
            status = -1;
        } else {
            sslcInit(0); // For the TLS library's random numbers.
            if (!sCurlReady) {
                curl_global_init(CURL_GLOBAL_DEFAULT);
                sCurlReady = 1;
            }
        }
    }
    if (status == 0 && sUsers++ == 0) {
        plat_3ds_hold_sleep(1);
    }
    LightLock_Unlock(&sLock);
    return status;
}

void net_3ds_end(void) {
    LightLock_Lock(&sLock);
    if (sUsers > 0 && --sUsers == 0) {
        sslcExit();
        socExit();
        free(sSocBuffer);
        sSocBuffer = NULL;
        plat_3ds_hold_sleep(0);
    }
    LightLock_Unlock(&sLock);
}

const char *net_3ds_ca_file(void) {
    struct stat st;
    return stat(CA_FILE_PATH, &st) == 0 ? CA_FILE_PATH : NULL;
}

const char *net_3ds_ca_pem(void) {
    return sGithubCaPem;
}
