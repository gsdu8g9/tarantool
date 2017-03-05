/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "box/lua/info.h"

#include <ctype.h> /* tolower() */

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "box/applier.h"
#include "box/relay.h"
#include "box/recovery.h"
#include "box/wal.h"
#include "box/replication.h"
#include "main.h"
#include "box/box.h"
#include "lua/utils.h"
#include "fiber.h"

#include "box/vinyl.h"

static void
lbox_pushvclock(struct lua_State *L, struct vclock *vclock)
{
	lua_createtable(L, 0, vclock_size(vclock));
	struct vclock_iterator it;
	vclock_iterator_init(&it, vclock);
	vclock_foreach(&it, replica) {
		lua_pushinteger(L, replica.id);
		luaL_pushuint64(L, replica.lsn);
		lua_settable(L, -3);
	}
	luaL_setmaphint(L, -1); /* compact flow */
}

static void
lbox_pushvclock_max(struct lua_State *L, struct vclock *vclock1,
		    struct vclock *vclock2)
{
	if (vclock2 == NULL)
		return lbox_pushvclock(L, vclock1);
	lua_createtable(L, 0, vclock_size(vclock1));
	struct vclock_iterator it;
	vclock_iterator_init(&it, vclock1);
	vclock_foreach(&it, replica) {
		lua_pushinteger(L, replica.id);
		int64_t lsn2 = vclock_get(vclock2, replica.id);
		luaL_pushuint64(L, replica.lsn > lsn2? replica.lsn: lsn2);
		lua_settable(L, -3);
	}
	luaL_setmaphint(L, -1); /* compact flow */

}

static void
lbox_pushapplier(lua_State *L, struct applier *applier)
{
	lua_newtable(L);
	/* Get applier state in lower case */
	static char status[16];
	char *d = status;
	const char *s = applier_state_strs[applier->state] + strlen("APPLIER_");
	assert(strlen(s) < sizeof(status));
	while ((*(d++) = tolower(*(s++))));

	lua_pushstring(L, "status");
	lua_pushstring(L, status);
	lua_settable(L, -3);

	if (applier->reader) {
		lua_pushstring(L, "lag");
		lua_pushnumber(L, applier->lag);
		lua_settable(L, -3);

		lua_pushstring(L, "idle");
		lua_pushnumber(L, ev_now(loop()) - applier->last_row_time);
		lua_settable(L, -3);

		struct error *e = diag_last_error(&applier->reader->diag);
		if (e != NULL) {
			lua_pushstring(L, "message");
			lua_pushstring(L, e->errmsg);
			lua_settable(L, -3);
		}
	}
}

static void
lbox_pushreplica(lua_State *L, struct replica *replica)
{
	struct applier *applier = replica->applier;
	struct relay *relay = replica->relay;

	lua_newtable(L);

	lua_pushstring(L, "uuid");
	lua_pushstring(L, tt_uuid_str(&replica->uuid));
	lua_settable(L, -3);

	struct vclock *vclock1 = NULL, *vclock2 = NULL;

	lua_pushstring(L, "status");
	if (applier != NULL && relay != NULL) {
		lua_pushstring(L, "bidirectional");
		if (vclock_size(&applier->vclock) >=
		    vclock_size(&relay->vclock)) {
			vclock1 = &applier->vclock;
			vclock2 = &relay->vclock;
		} else {
			vclock1 = &relay->vclock;
			vclock2 = &applier->vclock;
		}
	}
	else if (applier != NULL) {
		lua_pushstring(L, "follow");
		vclock1 = &applier->vclock;
	}
	else {
		lua_pushstring(L, "relay");
		vclock1 = &relay->vclock;
	}
	lua_settable(L, -3);

	lua_pushstring(L, "vclock");
	lbox_pushvclock_max(L, vclock1, vclock2);
	lua_settable(L, -3);

	if (applier != NULL) {
		lua_pushstring(L, "applier");
		lbox_pushapplier(L, applier);
		lua_settable(L, -3);
	}
}

static int
lbox_info_replication(struct lua_State *L)
{
	lua_newtable(L); /* box.info.replication */

	/* Nice formatting */
	lua_newtable(L); /* metatable */
	lua_pushliteral(L, "mapping");
	lua_setfield(L, -2, "__serialize");
	lua_setmetatable(L, -2);

	replicaset_foreach(replica) {
		/* Applier hasn't received replica id yet */
		if (replica->id == REPLICA_ID_NIL ||
		    (replica->applier == NULL && replica->relay == NULL))
			continue;

		lbox_pushreplica(L, replica);

		lua_rawseti(L, -2, replica->id);
	}

	return 1;
}

static int
lbox_info_server(struct lua_State *L)
{
	lua_createtable(L, 0, 2);
	lua_pushliteral(L, "id");
	lua_pushinteger(L, instance_id);
	lua_settable(L, -3);
	lua_pushliteral(L, "uuid");
	lua_pushlstring(L, tt_uuid_str(&INSTANCE_UUID), UUID_STR_LEN);
	lua_settable(L, -3);
	lua_pushliteral(L, "lsn");
	if (instance_id != REPLICA_ID_NIL && wal != NULL) {
		struct vclock vclock;
		wal_checkpoint(&vclock, false);
		luaL_pushint64(L, vclock_get(&vclock, instance_id));
	} else {
		luaL_pushint64(L, -1);
	}
	lua_settable(L, -3);
	lua_pushliteral(L, "ro");
	lua_pushboolean(L, box_is_ro());
	lua_settable(L, -3);

	return 1;
}

static int
lbox_info_vclock(struct lua_State *L)
{
	struct vclock vclock;
	if (wal != NULL) {
		wal_checkpoint(&vclock, false);
	} else {
		vclock_create(&vclock);
	}
	lbox_pushvclock(L, &vclock);
	return 1;
}

static int
lbox_info_status(struct lua_State *L)
{
	lua_pushstring(L, box_status());
	return 1;
}

static int
lbox_info_uptime(struct lua_State *L)
{
	lua_pushnumber(L, (unsigned)tarantool_uptime() + 1);
	return 1;
}

static int
lbox_info_pid(struct lua_State *L)
{
	lua_pushnumber(L, getpid());
	return 1;
}

static int
lbox_info_cluster(struct lua_State *L)
{
	lua_createtable(L, 0, 2);
	lua_pushliteral(L, "uuid");
	lua_pushlstring(L, tt_uuid_str(&REPLICASET_UUID), UUID_STR_LEN);
	lua_settable(L, -3);
	lua_pushliteral(L, "signature");
	luaL_pushint64(L, vclock_sum(&recovery->vclock));
	lua_settable(L, -3);

	return 1;
}

static void
lbox_vinyl_info_handler(struct vy_info_node *node, void *ctx)
{
	struct lua_State *L = ctx;

	if (node->type != VY_INFO_TABLE_END)
		lua_pushstring(L, node->key);

	switch (node->type) {
	case VY_INFO_TABLE_BEGIN:
		lua_newtable(L);
		break;
	case VY_INFO_TABLE_END:
		break;
	case VY_INFO_U32:
		lua_pushnumber(L, node->value.u32);
		break;
	case VY_INFO_U64:
		luaL_pushuint64(L, node->value.u64);
		break;
	case VY_INFO_STRING:
		lua_pushstring(L, node->value.str);
		break;
	default:
		unreachable();
	}

	if (node->type != VY_INFO_TABLE_BEGIN)
		lua_settable(L, -3);
}

/* Declared in vinyl_engine.cc */
extern struct vy_env *
vinyl_engine_get_env();

static int
lbox_info_vinyl_call(struct lua_State *L)
{
	struct vy_info_handler h = {
		.fn = lbox_vinyl_info_handler,
		.ctx = L,
	};
	vy_info_gather(vinyl_engine_get_env(), &h);
	return 1;
}

static int
lbox_info_vinyl(struct lua_State *L)
{
	lua_newtable(L);

	lua_newtable(L); /* metatable */

	lua_pushstring(L, "__call");
	lua_pushcfunction(L, lbox_info_vinyl_call);
	lua_settable(L, -3);

	lua_setmetatable(L, -2);

	return 1;
}

static const struct luaL_reg
lbox_info_dynamic_meta [] =
{
	{"vclock", lbox_info_vclock},
	{"server", lbox_info_server},
	{"replication", lbox_info_replication},
	{"status", lbox_info_status},
	{"uptime", lbox_info_uptime},
	{"pid", lbox_info_pid},
	{"cluster", lbox_info_cluster},
	{"vinyl", lbox_info_vinyl},
	{NULL, NULL}
};

/** Evaluate box.info.* function value and push it on the stack. */
static int
lbox_info_index(struct lua_State *L)
{
	lua_pushvalue(L, -1);			/* dup key */
	lua_gettable(L, lua_upvalueindex(1));   /* table[key] */

	if (!lua_isfunction(L, -1)) {
		/* No such key. Leave nil is on the stack. */
		return 1;
	}

	lua_call(L, 0, 1);
	lua_remove(L, -2);
	return 1;
}

/** Push a bunch of compile-time or start-time constants into a Lua table. */
static void
lbox_info_init_static_values(struct lua_State *L)
{
	/* tarantool version */
	lua_pushstring(L, "version");
	lua_pushstring(L, tarantool_version());
	lua_settable(L, -3);
}

/**
 * When user invokes box.info(), return a table of key/value
 * pairs containing the current info.
 */
static int
lbox_info_call(struct lua_State *L)
{
	lua_newtable(L);
	lbox_info_init_static_values(L);
	for (int i = 0; lbox_info_dynamic_meta[i].name; i++) {
		lua_pushstring(L, lbox_info_dynamic_meta[i].name);
		lbox_info_dynamic_meta[i].func(L);
		lua_settable(L, -3);
	}
	return 1;
}

/** Initialize box.info package. */
void
box_lua_info_init(struct lua_State *L)
{
	static const struct luaL_reg infolib [] = {
		{NULL, NULL}
	};

	luaL_register_module(L, "box.info", infolib);

	lua_newtable(L);		/* metatable for info */

	lua_pushstring(L, "__index");

	lua_newtable(L);
	luaL_register(L, NULL, lbox_info_dynamic_meta); /* table for __index */
	lua_pushcclosure(L, lbox_info_index, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__call");
	lua_pushcfunction(L, lbox_info_call);
	lua_settable(L, -3);

	lua_pushstring(L, "__serialize");
	lua_pushcfunction(L, lbox_info_call);
	lua_settable(L, -3);

	lua_setmetatable(L, -2);

	lbox_info_init_static_values(L);

	lua_pop(L, 1); /* info module */
}
