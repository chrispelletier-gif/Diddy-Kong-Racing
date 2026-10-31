#ifndef PORT_NET_3DS_H
#define PORT_NET_3DS_H

#include <stddef.h>

/*
 * The network for bug reports and updates: Wi-Fi check, the socket service and the TLS random numbers, libcurl.
 * Each net_3ds_begin that returns 0 needs a net_3ds_end; the two users may overlap.
 */
int net_3ds_begin(char *error, size_t errorSize);
void net_3ds_end(void);

/*
 * Lowers the calling thread below every game thread (background=1), or puts its priority back (0). The updater and
 * the bug report run at a high priority so their buttons answer, but the TLS handshake, gzip, SHA-256 and the SD
 * writes then keep the game's core from running the game. Background work yields to the game instead. Calls do not nest.
 */
void net_3ds_background(int background);

/* Root certificates for libcurl: sdmc:/3ds/dkr/cacert.pem if it exists (net_3ds_ca_file), else the built in ones. */
const char *net_3ds_ca_file(void);
const char *net_3ds_ca_pem(void);

#endif
