/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/*
   Copyright (C) 2010 by Ronnie Sahlberg <ronniesahlberg@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef AROS
#include "aros_compat.h"
#endif

#ifdef PS2_EE
#include "ps2_compat.h"
#endif

#ifdef PS3_PPU
#include "ps3_compat.h"
#endif

#ifdef WIN32
#include <win32/win32_compat.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "slist.h"
#include "libnfs-zdr.h"
#include "libnfs.h"
#include "libnfs-raw.h"
#include "libnfs-private.h"

uint64_t rpc_current_time(void)
{
#ifdef HAVE_CLOCK_GETTIME
	struct timespec tp;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &tp);
	return (uint64_t)tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
#else
	return (uint64_t)time(NULL) * 1000;
#endif
}

int rpc_set_hash_size(struct rpc_context *rpc, int hashes)
{
	uint32_t i;

#ifdef HAVE_MULTITHREADING
        if (rpc->multithreading_enabled) {
                nfs_mt_mutex_lock(&rpc->rpc_mutex);
        }
#endif /* HAVE_MULTITHREADING */
        rpc->num_hashes = hashes;
        free(rpc->waitpdu);
	rpc->waitpdu = malloc(sizeof(struct rpc_queue) * rpc->num_hashes);
        if (rpc->waitpdu == NULL) {
                return -1;
        }
	for (i = 0; i < rpc->num_hashes; i++)
		rpc_reset_queue(&rpc->waitpdu[i]);
#ifdef HAVE_MULTITHREADING
        if (rpc->multithreading_enabled) {
                nfs_mt_mutex_unlock(&rpc->rpc_mutex);
        }
#endif /* HAVE_MULTITHREADING */
        return 0;
}

int nfs_set_hash_size(struct nfs_context *nfs, int hashes)
{
        return rpc_set_hash_size(nfs->rpc, hashes);
}

struct rpc_context *rpc_init_context(void)
{
	struct rpc_context *rpc;
	static uint32_t salt = 0;

	rpc = malloc(sizeof(struct rpc_context));
	if (rpc == NULL) {
		return NULL;
	}
	memset(rpc, 0, sizeof(struct rpc_context));

	if (rpc_set_hash_size(rpc, DEFAULT_HASHES)) {
                free(rpc);
		return NULL;
	}

	rpc->magic = RPC_CONTEXT_MAGIC;
        rpc->inpos  = 0;
        rpc->record_marker = 0;
        rpc->state = READ_RM;

#ifdef HAVE_MULTITHREADING
	nfs_mt_mutex_init(&rpc->rpc_mutex);
#endif /* HAVE_MULTITHREADING */

 	rpc->auth = authunix_create_default();
	if (rpc->auth == NULL) {
		free(rpc->waitpdu);
		free(rpc);
		return NULL;
	}
	// Add PID to rpc->xid for easier debugging, making sure to cast
	// pid to 32-bit type to avoid invalid left-shifts.
	rpc->xid = salt + (uint32_t)rpc_current_time() + ((uint32_t)getpid() << 16);
	salt += 0x01000000;
	rpc->fd = -1;
	rpc->tcp_syncnt = RPC_PARAM_UNDEFINED;
	rpc->pagecache_ttl = NFS_PAGECACHE_DEFAULT_TTL;
#if defined(WIN32) || defined(ANDROID) || defined(PS3_PPU)
	rpc->uid = 65534;
	rpc->gid = 65534;
#else
	rpc->uid = getuid();
	rpc->gid = getgid();
#endif
	rpc_reset_queue(&rpc->outqueue);

	/* Default is no timeout */
	rpc->timeout = -1;
	/* Default is to timeout after 100ms of poll(2) */
	rpc->poll_timeout = 100;

	return rpc;
}

struct rpc_context *rpc_init_server_context(int s)
{
	struct rpc_context *rpc;

	rpc = malloc(sizeof(struct rpc_context));
	if (rpc == NULL) {
		return NULL;
	}
	memset(rpc, 0, sizeof(struct rpc_context));

	rpc->magic = RPC_CONTEXT_MAGIC;

	rpc->is_server_context = 1;
	rpc->fd = s;
	rpc->is_connected = 1;
        rpc->is_udp = rpc_is_udp_socket(rpc);
	rpc_reset_queue(&rpc->outqueue);

#ifdef HAVE_MULTITHREADING
        nfs_mt_mutex_init(&rpc->rpc_mutex);
#endif /* HAVE_MULTITHREADING */
	return rpc;
}

static uint32_t round_to_power_of_two(uint32_t x) {
	uint32_t power = 1;
	while (power < x) {
		power <<= 1;
	}
	return power;
}

void rpc_set_pagecache(struct rpc_context *rpc, uint32_t v)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);
	v = MAX(rpc->pagecache, round_to_power_of_two(v));
	RPC_LOG(rpc, 2, "pagecache set to %d pages of size %d", v, NFS_BLKSIZE);
	rpc->pagecache = v;
}

void rpc_set_pagecache_ttl(struct rpc_context *rpc, uint32_t v) {
	if (v) {
		RPC_LOG(rpc, 2, "set pagecache ttl to %d seconds\n", v);
	} else {
		RPC_LOG(rpc, 2, "set pagecache ttl to infinite");
	}
	rpc->pagecache_ttl = v;
}

void rpc_set_readahead(struct rpc_context *rpc, uint32_t v)
{
	uint32_t min_pagecache;

	assert(rpc->magic == RPC_CONTEXT_MAGIC);
	if (v) {
		v = MAX(NFS_BLKSIZE, round_to_power_of_two(v));
	}
	RPC_LOG(rpc, 2, "readahead set to %d byte", v);
	rpc->readahead = v;
	min_pagecache = (2 * v) / NFS_BLKSIZE;
	if (rpc->pagecache < min_pagecache) {
		/* current pagecache implementation needs a pagecache bigger
		 * than the readahead size to avoid collisions */
		rpc_set_pagecache(rpc, min_pagecache);
	}
}

#ifdef HAVE_SO_BINDTODEVICE
void rpc_set_interface(struct rpc_context *rpc, const char *ifname)
{
	/*
	 * This only copies the interface information into the RPC
	 * structure.  It doesn't stop whatever interface is being used. The
	 * connection needs to be restarted for that happen. In other words,
	 * set this before you connect.
	 */
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	if (ifname) {
		/*
		 * Allow at one-less character just-in-case IFNAMSIZ for
		 * the defined platform does not include the NUL-terminator.
		 */
		strncpy(rpc->ifname, ifname, sizeof(rpc->ifname) - 1);
	}
}
#endif

void rpc_set_debug(struct rpc_context *rpc, int level)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	rpc->debug = level;
}

struct rpc_context *rpc_init_udp_context(void)
{
	struct rpc_context *rpc;

	rpc = rpc_init_context();
	if (rpc != NULL) {
		rpc->is_udp = 1;
	}
	
	return rpc;
}

void rpc_set_auth(struct rpc_context *rpc, struct AUTH *auth)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	if (rpc->auth != NULL) {
		auth_destroy(rpc->auth);
	}
	rpc->auth = auth;
}

static void rpc_set_uid_gid(struct rpc_context *rpc, int uid, int gid) {
	if (uid != rpc->uid || gid != rpc->gid) {
		struct AUTH *auth = libnfs_authunix_create("libnfs", uid, gid, 0, NULL);
		if (auth != NULL) {
			rpc_set_auth(rpc, auth);
			rpc->uid = uid;
			rpc->gid = gid;
		}
	}
}

void rpc_set_uid(struct rpc_context *rpc, int uid) {
	rpc_set_uid_gid(rpc, uid, rpc->gid);
}

void rpc_set_gid(struct rpc_context *rpc, int gid) {
	rpc_set_uid_gid(rpc, rpc->uid, gid);
}

void rpc_set_auxiliary_gids(struct rpc_context *rpc, uint32_t len, uint32_t* gids) {
	struct AUTH *auth = libnfs_authunix_create("libnfs", rpc->uid, rpc->gid, len, gids);
	if (auth != NULL) {
		rpc_set_auth(rpc, auth);
	}
}

void rpc_set_error(struct rpc_context *rpc, const char *error_string, ...)
{
        va_list ap;
	char *old_error_string;

#ifdef HAVE_MULTITHREADING
        if (rpc->multithreading_enabled) {
                nfs_mt_mutex_lock(&rpc->rpc_mutex);
        }
#endif /* HAVE_MULTITHREADING */
        old_error_string = rpc->error_string;
        va_start(ap, error_string);
	rpc->error_string = malloc(1024);
	vsnprintf(rpc->error_string, 1024, error_string, ap);
        va_end(ap);

	RPC_LOG(rpc, 1, "error: %s", rpc->error_string);

	if (old_error_string != NULL) {
		free(old_error_string);
	}
#ifdef HAVE_MULTITHREADING
        if (rpc->multithreading_enabled) {
                nfs_mt_mutex_unlock(&rpc->rpc_mutex);
        }
#endif /* HAVE_MULTITHREADING */
}

char *rpc_get_error(struct rpc_context *rpc)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	return rpc->error_string ? rpc->error_string : "";
}

static void rpc_purge_all_pdus(struct rpc_context *rpc, int status, const char *error)
{
	struct rpc_queue outqueue;
	struct rpc_pdu *pdu;
	int i;

	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	/* Remove all entries from each queue before cancellation to prevent
	 * the callbacks manipulating entries that are about to be removed.
	 *
	 * This code assumes that the callbacks will not enqueue any new
	 * pdus when called.
	 */

#ifdef HAVE_MULTITHREADING
        if (rpc->multithreading_enabled) {
                nfs_mt_mutex_lock(&rpc->rpc_mutex);
        }
#endif /* HAVE_MULTITHREADING */
	outqueue = rpc->outqueue;

	rpc_reset_queue(&rpc->outqueue);
	while ((pdu = outqueue.head) != NULL) {
		outqueue.head = pdu->next;
                pdu->next = NULL;
		pdu->cb(rpc, status, (void *) error, pdu->private_data);
		rpc_free_pdu(rpc, pdu);
	}
#ifdef HAVE_MULTITHREADING
        if (rpc->multithreading_enabled) {
                nfs_mt_mutex_unlock(&rpc->rpc_mutex);
        }
#endif /* HAVE_MULTITHREADING */

	for (i = 0; i < rpc->num_hashes; i++) {
		struct rpc_queue waitqueue;

#ifdef HAVE_MULTITHREADING
                if (rpc->multithreading_enabled) {
                        nfs_mt_mutex_lock(&rpc->rpc_mutex);
                }
#endif /* HAVE_MULTITHREADING */
		waitqueue = rpc->waitpdu[i];
		rpc_reset_queue(&rpc->waitpdu[i]);
#ifdef HAVE_MULTITHREADING
                if (rpc->multithreading_enabled) {
                        nfs_mt_mutex_unlock(&rpc->rpc_mutex);
                }
#endif /* HAVE_MULTITHREADING */
		while((pdu = waitqueue.head) != NULL) {
			waitqueue.head = pdu->next;
			pdu->next = NULL;
			pdu->cb(rpc, status, (void *) error, pdu->private_data);
			rpc_free_pdu(rpc, pdu);
		}
	}

	assert(!rpc->outqueue.head);
}

void rpc_error_all_pdus(struct rpc_context *rpc, const char *error)
{
	rpc_purge_all_pdus(rpc, RPC_STATUS_ERROR, error);
}

static void rpc_free_fragment(struct rpc_fragment *fragment)
{
	if (fragment->data != NULL) {
		free(fragment->data);
	}
	free(fragment);
}

void rpc_free_all_fragments(struct rpc_context *rpc)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	while (rpc->fragments != NULL) {
	      struct rpc_fragment *fragment = rpc->fragments;

	      rpc->fragments = fragment->next;
	      rpc_free_fragment(fragment);
	}
}

int rpc_add_fragment(struct rpc_context *rpc, char *data, uint32_t size)
{
	struct rpc_fragment *fragment;

	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	fragment = malloc(sizeof(struct rpc_fragment));
	if (fragment == NULL) {
		return -1;
	}

	fragment->size = size;
	fragment->data = malloc(fragment->size);
	if(fragment->data == NULL) {
		free(fragment);
		return -1;
	}

	memcpy(fragment->data, data, fragment->size);
	LIBNFS_LIST_ADD_END(&rpc->fragments, fragment);
	return 0;
}

void rpc_destroy_context(struct rpc_context *rpc)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	rpc_purge_all_pdus(rpc, RPC_STATUS_CANCEL, NULL);

	rpc_free_all_fragments(rpc);

        if (rpc->auth) {
                auth_destroy(rpc->auth);
                rpc->auth =NULL;
        }

	if (rpc->fd != -1) {
 		close(rpc->fd);
	}

	if (rpc->error_string != NULL) {
		free(rpc->error_string);
		rpc->error_string = NULL;
	}

        free(rpc->waitpdu);
        rpc->waitpdu = NULL;
	free(rpc->inbuf);
	rpc->inbuf = NULL;

	rpc->magic = 0;
#ifdef HAVE_MULTITHREADING
        nfs_mt_mutex_destroy(&rpc->rpc_mutex);
#endif /* HAVE_MULTITHREADING */
	free(rpc);
}

void rpc_set_poll_timeout(struct rpc_context *rpc, int poll_timeout)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	rpc->poll_timeout = poll_timeout;
}

int rpc_get_poll_timeout(struct rpc_context *rpc)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	return rpc->poll_timeout;
}

void rpc_set_timeout(struct rpc_context *rpc, int timeout)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	rpc->timeout = timeout;
}

int rpc_get_timeout(struct rpc_context *rpc)
{
	assert(rpc->magic == RPC_CONTEXT_MAGIC);

	return rpc->timeout;
}

int rpc_register_service(struct rpc_context *rpc, int program, int version,
                         struct service_proc *procs, int num_procs)
{
        struct rpc_endpoint *endpoint;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (!rpc->is_server_context) {
		rpc_set_error(rpc, "Not a server context.");
                return -1;
        }

        endpoint = malloc(sizeof(*endpoint));
        if (endpoint == NULL) {
		rpc_set_error(rpc, "Out of memory: Failed to allocate endpoint "
                              "structure");
                return -1;
        }

        endpoint->program = program;
        endpoint->version = version;
        endpoint->procs = procs;
        endpoint->num_procs = num_procs;
        endpoint->next = rpc->endpoints;
        rpc->endpoints = endpoint;

        return 0;
}

void rpc_free_iovector(struct rpc_context *rpc, struct rpc_io_vectors *v)
{
        int i;

        for (i = 0; i < v->niov; i++) {
                if (v->iov[i].free) {
                        v->iov[i].free(v->iov[i].buf);
                }
        }
        v->niov = 0;
}

int rpc_add_iovector(struct rpc_context *rpc, struct rpc_io_vectors *v,
                      char *buf, int len, void (*free)(void *))
{
        if (v->niov >= RPC_MAX_VECTORS) {
                rpc_set_error(rpc, "Too many io vectors");
                return -1;
        }

        v->iov[v->niov].buf = buf;
        v->iov[v->niov].len = len;
        v->iov[v->niov].free = free;
        v->niov++;

        return 0;
}
