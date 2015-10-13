#ifndef TARANTOOL_APPLIER_H_INCLUDED
#define TARANTOOL_APPLIER_H_INCLUDED
/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <netinet/in.h>
#include <sys/socket.h>

#include "trivia/util.h"
#include "uri.h"
#include "tt_uuid.h"
#include "third_party/tarantool_ev.h"
#define RB_COMPACT 1
#include <third_party/rb.h>
#include "vclock.h"

struct recovery;

enum { APPLIER_SOURCE_MAXLEN = 1024 }; /* enough to fit URI with passwords */

#define applier_STATE(_)                                             \
	_(APPLIER_OFF, 0)                                            \
	_(APPLIER_CONNECT, 1)                                        \
	_(APPLIER_AUTH, 2)                                           \
	_(APPLIER_CONNECTED, 3)                                      \
	_(APPLIER_BOOTSTRAP, 4)                                      \
	_(APPLIER_FOLLOW, 5)                                         \
	_(APPLIER_STOPPED, 6)                                        \
	_(APPLIER_DISCONNECTED, 7)                                   \

/** States for the applier */
ENUM(applier_state, applier_STATE);
extern const char *applier_state_strs[];

/**
 * State of a replication connection to the master
 */
struct applier {
	struct fiber *reader;
	struct fiber *writer;
	struct fiber *connecter;
	enum applier_state state;
	ev_tstamp lag, last_row_time;
	bool warning_said;
	bool cfg_merge_flag; /* used by box_set_replication_source */
	uint32_t id;
	struct tt_uuid uuid;
	char source[APPLIER_SOURCE_MAXLEN];
	rb_node(struct applier) link; /* a set by source in cluster.cc */
	struct uri uri;
	uint32_t version_id; /* remote version */
	bool connected;
	bool switched;
	bool localhost;
	struct vclock vclock;
	union {
		struct sockaddr addr;
		struct sockaddr_storage addrstorage;
	};
	socklen_t addr_len;
	/** Save master fd to re-use a connection between JOIN and SUBSCRIBE */
	struct ev_io io;
	struct ev_io in;
	/** Input/output buffer for buffered IO */
	struct iobuf *iobuf;
};

/**
 * Start a client to a remote server using a background fiber.
 *
 * If recovery is finalized (i.e. r->writer != NULL) then the client
 * connect to a master and follow remote updates using SUBSCRIBE command.
 *
 * If recovery is not finalized (i.e. r->writer == NULL) then the client
 * connect to a master, download and process snapshot using JOIN command
 * and then exits. The background fiber can be joined to get exit status
 * using applier_wait().
 *
 * \pre A connection from io->fd is re-used.
 * \sa fiber_start()
 */
void
applier_start(struct applier *applier, struct recovery *r);

/**
 * Stop a client.
 */
void
applier_stop(struct applier *applier);

/**
 * Wait replication client to finish and rethrow exception (if any).
 * Use this function to wait until bootstrap.
 *
 * \post This function keeps a open connection in io->fd.
 * \sa applier_start()
 * \sa fiber_join()
 */
void
applier_wait(struct applier *applier);

/**
 * Allocate an instance of applier object, create applier and initialize
 * remote uri (copied to struct applier).
 *
 * @pre     the uri is a valid and checked one
 * @error   throws OutOfMemory exception if out of memory.
 */
struct applier *
applier_new(const char *uri);

/**
 * Destroy and delete a applier.
 */
void
applier_delete(struct applier *applier);

#endif /* TARANTOOL_APPLIER_H_INCLUDED */
