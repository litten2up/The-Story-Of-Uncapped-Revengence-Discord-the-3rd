// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2012-2016 by John "JTE" Muniz.
// Copyright (C) 2012-2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  lua_hooklib.c
/// \brief hooks for Lua scripting

#include "doomdef.h"
#include "doomstat.h"
#include "p_mobj.h"
#include "g_game.h"
#include "r_skins.h"
#include "b_bot.h"
#include "z_zone.h"

#include "lua_script.h"
#include "lua_libs.h"
#include "lua_hook.h"
#include "lua_hud.h" // hud_running errors

#include "m_perfstats.h"
#include "d_netcmd.h" // for cv_perfstats
#include "i_system.h" // I_GetTimeMicros

#undef LUA_Hook

/* =========================================================================
                                  ABSTRACTION
   ========================================================================= */

static const char * const mobjHookNames[] = { Mobj_Hook_List (TOSTR)  NULL };
static const char * const     hookNames[] = {      Hook_List (TOSTR)  NULL };

static const char * const stringHookNames[] = {
	String_Hook_List (TOSTR)  NULL
};

/* TODO: remove when doomtype version is merged */

#define BIT_ARRAY_LENGTH(n) (((n) + 7) >> 3)

static inline void set_bit_array (UINT8 *array, const int n) {
	array[n >> 3] |= 1 << (n & 7);
}

static inline int in_bit_array (const UINT8 *array, const int n) {
	return array[n >> 3] & (1 << (n & 7));
}

typedef struct {
	int numGeneric;
	int ref;
} stringhook_t;

static int hookRefs[Hook(MAX)];
static int mobjHookRefs[NUMMOBJTYPES][Mobj_Hook(MAX)];

static stringhook_t stringHooks[String_Hook(MAX)];

static int hookReg;

// After a hook errors once, don't print the error again.
static UINT8 * hooksErrored;

static boolean mobj_hook_available(int hook_type, mobjtype_t mobj_type)
{
	return
		(
				mobjHookRefs [MT_NULL] [hook_type] > 0 ||
				mobjHookRefs[mobj_type][hook_type] > 0
		);
}

static int hook_in_list
(
		const char * const         name,
		const char * const * const list
){
	int type;

	for (type = 0; list[type] != NULL; ++type)
	{
		if (strcmp(name, list[type]) == 0)
			break;
	}

	return type;
}

static void get_table(lua_State *L)
{
	lua_pushvalue(L, -1);
	lua_rawget(L, -3);

	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		lua_createtable(L, 1, 0);
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_rawset(L, -5);
	}

	lua_remove(L, -2);
}

static void new_hook_table(lua_State *L, int *ref)
{
	if (*ref > 0)
		lua_getref(L, *ref);
	else
	{
		lua_newtable(L);
		lua_pushvalue(L, -1);
		*ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
}

static void add_hook(lua_State *L, int id)
{
	lua_pushnumber(L, 1 + id);
	lua_rawseti(L, -2, 1 + lua_objlen(L, -2));
}

static void add_mobj_hook(lua_State *L, int hook_type, int id)
{
	mobjtype_t   mobj_type = luaL_optnumber(L, 3, MT_NULL);

	luaL_argcheck(L, mobj_type < NUMMOBJTYPES, 3, "invalid mobjtype_t");

	new_hook_table(L, &mobjHookRefs[mobj_type][hook_type]);
	add_hook(L, id);
}

static void add_string_hook(lua_State *L, int type, int id)
{
	stringhook_t * hook = &stringHooks[type];

	char * string = NULL;

	switch (type)
	{
		case String_Hook(BotAI):
		case String_Hook(ShouldJingleContinue):
			if (lua_isstring(L, 3))
			{ // lowercase copy
				string = Z_StrDup(lua_tostring(L, 3));
				strlwr(string);
			}
			break;

		case String_Hook(LinedefExecute):
			string = Z_StrDup(luaL_checkstring(L, 3));
			strupr(string);
			break;
	}

	new_hook_table(L, &hook->ref);

	if (string)
	{
		lua_pushstring(L, string);
		get_table(L);
		add_hook(L, id);
	}
	else
	{
		lua_pushnumber(L, 1 + id);
		lua_rawseti(L, -2, ++hook->numGeneric);
	}
}

// Takes hook, function, and additional arguments (mobj type to act on, etc.)
static int lib_addHook(lua_State *L)
{
	static int nextid;

	const char * name;
	int type;

	if (!lua_lumploading)
		return luaL_error(L, "This function cannot be called from within a hook or coroutine!");

	name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	/* this is a very special case */
	if (( type = hook_in_list(name, stringHookNames) ) < String_Hook(MAX))
	{
		add_string_hook(L, type, nextid);
	}
	else if (( type = hook_in_list(name, mobjHookNames) ) < Mobj_Hook(MAX))
	{
		add_mobj_hook(L, type, nextid);
	}
	else if (( type = hook_in_list(name, hookNames) ) < Hook(MAX))
	{
		new_hook_table(L, &hookRefs[type]);
		add_hook(L, nextid);
	}
	else
	{
		return luaL_argerror(L, 1, lua_pushfstring(L, "invalid hook " LUA_QS, name));
	}

	if (!(nextid & 7))
	{
		Z_Realloc(hooksErrored,
				BIT_ARRAY_LENGTH (nextid + 1) * sizeof *hooksErrored,
				PU_STATIC, &hooksErrored);
		hooksErrored[nextid >> 3] = 0;
	}

	// set the hook function in the registry.
	lua_getref(L, hookReg);
	lua_pushvalue(L, 2);/* the function */
	lua_rawseti(L, -2, ++nextid);

	return 0;
}

int LUA_HookLib(lua_State *L)
{
	new_hook_table(L, &hookReg);
	lua_register(L, "addHook", lib_addHook);
	return 0;
}

typedef struct Hook_State Hook_State;
typedef void (*Hook_Callback)(Hook_State *);

struct Hook_State {
	int          status;/* return status to calling function */
	void       * userdata;
	int          ref;/* ref for primary hook table */
	int          hook_type;/* Hook or Hook(MAX) + Mobj_Hook */
	mobjtype_t   mobj_type;
	const char * string;/* used to fetch table, ran first if set */
	int          top;/* index of last argument passed to hook */
	int          id;/* id to fetch function from registry */
	int          values;/* num arguments passed to hook */
	int          results;/* num values returned by hook */
	Hook_Callback results_handler;/* callback when hook successfully returns */
};

enum {
	HINDEX = 1,/* hook registry */
	EINDEX = 2,/* error handler */
	SINDEX = 3,/* string itself is pushed in case of string hook */
};

static void push_error_handler(void)
{
	lua_pushcfunction(gL, LUA_GetErrorMessage);
}

/* repush hook string */
static void push_string(void)
{
	lua_pushvalue(gL, SINDEX);
}

static boolean start_hook_stack(void)
{
	lua_settop(gL, 0);
	lua_getref(gL, hookReg);
	push_error_handler();
	return true;
}

static boolean init_hook_type
(
		Hook_State * hook,
		int          status,
		int          hook_type,
		mobjtype_t   mobj_type,
		const char * string,
		int          ref
){
	boolean ready;

	hook->status = status;

	if (mobj_type > 0)
		ready = mobj_hook_available(hook_type, mobj_type);
	else
		ready = ref > 0;

	if (ready)
	{
		hook->ref = ref;
		hook->hook_type = hook_type;
		hook->mobj_type = mobj_type;
		hook->string = string;
		return start_hook_stack();
	}
	else
		return false;
}

static boolean prepare_hook
(
		Hook_State * hook,
		int default_status,
		int hook_type
){
	return init_hook_type(hook, default_status,
			hook_type, 0, NULL,
			hookRefs[hook_type]);
}

static boolean prepare_mobj_hook
(
		Hook_State * hook,
		int          default_status,
		int          hook_type,
		mobjtype_t   mobj_type
){
	return init_hook_type(hook, default_status,
			hook_type, mobj_type, NULL,
			mobjHookRefs[mobj_type][hook_type]);
}

static boolean prepare_string_hook
(
		Hook_State * hook,
		int          default_status,
		int          hook_type,
		const char * string
){
	if (init_hook_type(hook, default_status,
				hook_type, 0, string,
				stringHooks[hook_type].ref))
	{
		lua_pushstring(gL, string);
		return true;
	}
	else
		return false;
}

static void init_hook_call
(
		Hook_State * hook,
		int    values,
		int    results,
		Hook_Callback results_handler
){
	hook->top = lua_gettop(gL);
	hook->values = values;
	hook->results = results;
	hook->results_handler = results_handler;
}

static void get_hook_table(Hook_State *hook)
{
	lua_getref(gL, hook->ref);
}

static void get_hook(Hook_State *hook, int n)
{
	lua_rawgeti(gL, -1, n);
	hook->id = lua_tonumber(gL, -1) - 1;
	lua_rawget(gL, HINDEX);
}

static int call_single_hook_no_copy(Hook_State *hook)
{
	if (lua_pcall(gL, hook->values, hook->results, EINDEX) == 0)
	{
		if (hook->results > 0)
		{
			(*hook->results_handler)(hook);
			lua_pop(gL, hook->results);
		}
	}
	else
	{
		/* print the error message once */
		if (cv_debug & DBG_LUA || !in_bit_array(hooksErrored, hook->id))
		{
			CONS_Alert(CONS_WARNING, "%s\n", lua_tostring(gL, -1));
			set_bit_array(hooksErrored, hook->id);
		}
		lua_pop(gL, 1);
	}

	return 1;
}

static int call_single_hook(Hook_State *hook)
{
	int i;

	for (i = -(hook->values) + 1; i <= 0; ++i)
		lua_pushvalue(gL, hook->top + i);

	return call_single_hook_no_copy(hook);
}

static int call_hook_table_for(Hook_State *hook, int n)
{
	int k;

	for (k = 1; k <= n; ++k)
	{
		get_hook(hook, k);
		call_single_hook(hook);
	}

	return n;
}

static int call_hook_table(Hook_State *hook)
{
	return call_hook_table_for(hook, lua_objlen(gL, -1));
}

static int call_ref(Hook_State *hook, int ref)
{
	int calls;

	if (ref > 0)
	{
		lua_getref(gL, ref);
		calls = call_hook_table(hook);

		return calls;
	}
	else
		return 0;
}

static int call_string_hooks(Hook_State *hook)
{
	const int numGeneric = stringHooks[hook->hook_type].numGeneric;

	int calls = 0;

	get_hook_table(hook);

	/* call generic string hooks first */
	calls += call_hook_table_for(hook, numGeneric);

	push_string();
	lua_rawget(gL, -2);
	calls += call_hook_table(hook);

	return calls;
}

static int call_generic_mobj_hooks(Hook_State *hook)
{
	const int ref = mobjHookRefs[MT_NULL][hook->hook_type];
	return call_ref(hook, ref);
}

static int call_hooks
(
		Hook_State * hook,
		int        values,
		int        results,
		Hook_Callback results_handler
){
	int calls = 0;

	init_hook_call(hook, values, results, results_handler);

	if (hook->string)
	{
		calls += call_string_hooks(hook);
	}
	else
	{
		if (hook->mobj_type > 0)
			calls += call_generic_mobj_hooks(hook);

		calls += call_ref(hook, hook->ref);

		if (hook->mobj_type > 0)
			ps_lua_mobjhooks += calls;
	}

	lua_settop(gL, 0);

	return calls;
}

/* =========================================================================
                            COMMON RESULT HANDLERS
   ========================================================================= */

#define res_none NULL

static void res_true(Hook_State *hook)
{
	if (lua_toboolean(gL, -1))
		hook->status = true;
}

static void res_false(Hook_State *hook)
{
	if (!lua_isnil(gL, -1) && !lua_toboolean(gL, -1))
		hook->status = false;
}

static void res_force(Hook_State *hook)
{
	if (!lua_isnil(gL, -1))
	{
		if (lua_toboolean(gL, -1))
			hook->status = 1; // Force yes
		else
			hook->status = 2; // Force no
	}
}

/* =========================================================================
                               GENERALISED HOOKS
   ========================================================================= */

int LUA_HookMobj(mobj_t *mobj, int hook_type)
{
	Hook_State hook;
	if (prepare_mobj_hook(&hook, false, hook_type, mobj->type))
	{
		LUA_PushUserdata(gL, mobj, META_MOBJ);
		call_hooks(&hook, 1, 1, res_true);
	}
	return hook.status;
}

int LUA_Hook2Mobj(mobj_t *t1, mobj_t *t2, int hook_type)
{
	Hook_State hook;
	if (prepare_mobj_hook(&hook, 0, hook_type, t1->type))
	{
		LUA_PushUserdata(gL, t1, META_MOBJ);
		LUA_PushUserdata(gL, t2, META_MOBJ);
		call_hooks(&hook, 2, 1, res_force);
	}
	return hook.status;
}

void LUA_Hook(int type)
{
	Hook_State hook;
	if (prepare_hook(&hook, 0, type))
		call_hooks(&hook, 0, 0, res_none);
}

void LUA_HookInt(INT32 number, int hook_type)
{
	Hook_State hook;
	if (prepare_hook(&hook, 0, hook_type))
	{
		lua_pushinteger(gL, number);
		call_hooks(&hook, 1, 0, res_none);
	}
}

int LUA_HookPlayer(player_t *player, int hook_type)
{
	Hook_State hook;
	if (prepare_hook(&hook, false, hook_type))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		call_hooks(&hook, 1, 1, res_true);
	}
	return hook.status;
}

int LUA_HookTiccmd(player_t *player, ticcmd_t *cmd, int hook_type)
{
	Hook_State hook;
	if (prepare_hook(&hook, false, hook_type))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		LUA_PushUserdata(gL, cmd, META_TICCMD);

		if (hook_type == Hook(PlayerCmd))
			hook_cmd_running = true;

		call_hooks(&hook, 2, 1, res_true);

		if (hook_type == Hook(PlayerCmd))
			hook_cmd_running = false;
	}
	return hook.status;
}

/* =========================================================================
                               SPECIALIZED HOOKS
   ========================================================================= */

void LUA_HookThinkFrame(void)
{
	// variables used by perf stats
	int hook_index = 0;
	int time_taken = 0;

	Hook_State hook;

	int n;
	int k;

	if (prepare_hook(&hook, 0, Hook(ThinkFrame)))
	{
		init_hook_call(&hook, 0, 0, res_none);

		get_hook_table(&hook);
		n = lua_objlen(gL, -1);

		for (k = 1; k <= n; ++k)
		{
			get_hook(&hook, k);

			if (cv_perfstats.value == 3)
			{
				lua_pushvalue(gL, -1);/* need the function again */
				time_taken = I_GetTimeMicros();
			}

			call_single_hook(&hook);

			if (cv_perfstats.value == 3)
			{
				lua_Debug ar;
				time_taken = I_GetTimeMicros() - time_taken;
				lua_getinfo(gL, ">S", &ar);
				PS_SetThinkFrameHookInfo(hook_index, time_taken, ar.short_src);
				hook_index++;
			}
		}

		lua_settop(gL, 0);
	}
}

int LUA_HookMobjLineCollide(mobj_t *mobj, line_t *line)
{
	Hook_State hook;
	if (prepare_mobj_hook(&hook, 0, Mobj_Hook(MobjLineCollide), mobj->type))
	{
		LUA_PushUserdata(gL, mobj, META_MOBJ);
		LUA_PushUserdata(gL, line, META_LINE);
		call_hooks(&hook, 2, 1, res_force);
	}
	return hook.status;
}

int LUA_HookTouchSpecial(mobj_t *special, mobj_t *toucher)
{
	Hook_State hook;
	if (prepare_mobj_hook(&hook, false, Mobj_Hook(TouchSpecial), special->type))
	{
		LUA_PushUserdata(gL, special, META_MOBJ);
		LUA_PushUserdata(gL, toucher, META_MOBJ);
		call_hooks(&hook, 2, 1, res_true);
	}
	return hook.status;
}

static int damage_hook
(
		mobj_t *target,
		mobj_t *inflictor,
		mobj_t *source,
		INT32   damage,
		UINT8   damagetype,
		int     hook_type,
		int     values,
		Hook_Callback results_handler
){
	Hook_State hook;
	if (prepare_mobj_hook(&hook, 0, hook_type, target->type))
	{
		LUA_PushUserdata(gL, target, META_MOBJ);
		LUA_PushUserdata(gL, inflictor, META_MOBJ);
		LUA_PushUserdata(gL, source, META_MOBJ);
		if (values == 5)
			lua_pushinteger(gL, damage);
		lua_pushinteger(gL, damagetype);
		call_hooks(&hook, values, 1, results_handler);
	}
	return hook.status;
}

int LUA_HookShouldDamage(mobj_t *target, mobj_t *inflictor, mobj_t *source, INT32 damage, UINT8 damagetype)
{
	return damage_hook(target, inflictor, source, damage, damagetype,
			Mobj_Hook(ShouldDamage), 5, res_force);
}

int LUA_HookMobjDamage(mobj_t *target, mobj_t *inflictor, mobj_t *source, INT32 damage, UINT8 damagetype)
{
	return damage_hook(target, inflictor, source, damage, damagetype,
			Mobj_Hook(MobjDamage), 5, res_true);
}

int LUA_HookMobjDeath(mobj_t *target, mobj_t *inflictor, mobj_t *source, UINT8 damagetype)
{
	return damage_hook(target, inflictor, source, 0, damagetype,
			Mobj_Hook(MobjDeath), 4, res_true);
}

typedef struct {
	mobj_t   * tails;
	ticcmd_t * cmd;
} BotAI_State;

static boolean checkbotkey(const char *field)
{
	return lua_toboolean(gL, -1) && strcmp(lua_tostring(gL, -2), field) == 0;
}

static void res_botai(Hook_State *hook)
{
	BotAI_State *botai = hook->userdata;

	int k[8];

	int fields = 0;

	// This turns forward, backward, left, right, jump, and spin into a proper ticcmd for tails.
	if (lua_istable(gL, -8)) {
		lua_pushnil(gL); // key
		while (lua_next(gL, -9)) {
#define check(n, f) (checkbotkey(f) ? (k[(n)-1] = 1) : 0)
			if (
					check(1,    "forward") || check(2,    "backward") ||
					check(3,       "left") || check(4,       "right") ||
					check(5, "strafeleft") || check(6, "straferight") ||
					check(7,       "jump") || check(8,        "spin")
			){
				if (8 <= ++fields)
				{
					lua_pop(gL, 2); // pop key and value
					break;
				}
			}

			lua_pop(gL, 1); // pop value
#undef check
		}
	} else {
		while (fields < 8)
		{
			k[fields] = lua_toboolean(gL, -8 + fields);
			fields++;
		}
	}

	B_KeysToTiccmd(botai->tails, botai->cmd,
			k[0],k[1],k[2],k[3],k[4],k[5],k[6],k[7]);

	hook->status = true;
}

int LUA_HookBotAI(mobj_t *sonic, mobj_t *tails, ticcmd_t *cmd)
{
	const char *skin = ((skin_t *)tails->skin)->name;

	Hook_State hook;
	BotAI_State botai;

	if (prepare_string_hook(&hook, false, String_Hook(BotAI), skin))
	{
		LUA_PushUserdata(gL, sonic, META_MOBJ);
		LUA_PushUserdata(gL, tails, META_MOBJ);

		botai.tails = tails;
		botai.cmd   = cmd;

		hook.userdata = &botai;

		call_hooks(&hook, 2, 8, res_botai);
	}

	return hook.status;
}

void LUA_HookLinedefExecute(line_t *line, mobj_t *mo, sector_t *sector)
{
	Hook_State hook;
	if (prepare_string_hook
			(&hook, 0, String_Hook(LinedefExecute), line->stringargs[0]))
	{
		LUA_PushUserdata(gL, line, META_LINE);
		LUA_PushUserdata(gL, mo, META_MOBJ);
		LUA_PushUserdata(gL, sector, META_SECTOR);
		ps_lua_mobjhooks += call_hooks(&hook, 3, 0, res_none);
	}
}

int LUA_HookPlayerMsg(int source, int target, int flags, char *msg)
{
	Hook_State hook;
	if (prepare_hook(&hook, false, Hook(PlayerMsg)))
	{
		LUA_PushUserdata(gL, &players[source], META_PLAYER); // Source player
		if (flags & 2 /*HU_CSAY*/) { // csay TODO: make HU_CSAY accessible outside hu_stuff.c
			lua_pushinteger(gL, 3); // type
			lua_pushnil(gL); // target
		} else if (target == -1) { // sayteam
			lua_pushinteger(gL, 1); // type
			lua_pushnil(gL); // target
		} else if (target == 0) { // say
			lua_pushinteger(gL, 0); // type
			lua_pushnil(gL); // target
		} else { // sayto
			lua_pushinteger(gL, 2); // type
			LUA_PushUserdata(gL, &players[target-1], META_PLAYER); // target
		}
		lua_pushstring(gL, msg); // msg
		call_hooks(&hook, 4, 1, res_true);
	}
	return hook.status;
}

int LUA_HookHurtMsg(player_t *player, mobj_t *inflictor, mobj_t *source, UINT8 damagetype)
{
	Hook_State hook;
	if (prepare_hook(&hook, false, Hook(HurtMsg)))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		LUA_PushUserdata(gL, inflictor, META_MOBJ);
		LUA_PushUserdata(gL, source, META_MOBJ);
		lua_pushinteger(gL, damagetype);
		call_hooks(&hook, 4, 1, res_true);
	}
	return hook.status;
}

void LUA_HookNetArchive(lua_CFunction archFunc)
{
	const int ref = hookRefs[Hook(NetVars)];
	Hook_State hook;
	/* this is a remarkable case where the stack isn't reset */
	if (ref > 0)
	{
		// stack: tables
		I_Assert(lua_gettop(gL) > 0);
		I_Assert(lua_istable(gL, -1));

		push_error_handler();
		lua_getref(gL, hookReg);

		lua_insert(gL, HINDEX);
		lua_insert(gL, EINDEX);

		// tables becomes an upvalue of archFunc
		lua_pushvalue(gL, -1);
		lua_pushcclosure(gL, archFunc, 1);
		// stack: tables, archFunc

		init_hook_call(&hook, 1, 0, res_none);
		call_ref(&hook, ref);

		lua_pop(gL, 2); // pop hook table and archFunc
		lua_remove(gL, EINDEX); // pop error handler
		lua_remove(gL, HINDEX); // pop hook registry
		// stack: tables
	}
}

int LUA_HookMapThingSpawn(mobj_t *mobj, mapthing_t *mthing)
{
	Hook_State hook;
	if (prepare_mobj_hook(&hook, false, Mobj_Hook(MapThingSpawn), mobj->type))
	{
		LUA_PushUserdata(gL, mobj, META_MOBJ);
		LUA_PushUserdata(gL, mthing, META_MAPTHING);
		call_hooks(&hook, 2, 1, res_true);
	}
	return hook.status;
}

int LUA_HookFollowMobj(player_t *player, mobj_t *mobj)
{
	Hook_State hook;
	if (prepare_mobj_hook(&hook, false, Mobj_Hook(FollowMobj), mobj->type))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		LUA_PushUserdata(gL, mobj, META_MOBJ);
		call_hooks(&hook, 2, 1, res_true);
	}
	return hook.status;
}

int LUA_HookPlayerCanDamage(player_t *player, mobj_t *mobj)
{
	Hook_State hook;
	if (prepare_hook(&hook, 0, Hook(PlayerCanDamage)))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		LUA_PushUserdata(gL, mobj, META_MOBJ);
		call_hooks(&hook, 2, 1, res_force);
	}
	return hook.status;
}

void LUA_HookPlayerQuit(player_t *plr, kickreason_t reason)
{
	Hook_State hook;
	if (prepare_hook(&hook, 0, Hook(PlayerQuit)))
	{
		LUA_PushUserdata(gL, plr, META_PLAYER); // Player that quit
		lua_pushinteger(gL, reason); // Reason for quitting
		call_hooks(&hook, 2, 0, res_none);
	}
}

int LUA_HookTeamSwitch(player_t *player, int newteam, boolean fromspectators, boolean tryingautobalance, boolean tryingscramble)
{
	Hook_State hook;
	if (prepare_hook(&hook, true, Hook(TeamSwitch)))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		lua_pushinteger(gL, newteam);
		lua_pushboolean(gL, fromspectators);
		lua_pushboolean(gL, tryingautobalance);
		lua_pushboolean(gL, tryingscramble);
		call_hooks(&hook, 5, 1, res_false);
	}
	return hook.status;
}

int LUA_HookViewpointSwitch(player_t *player, player_t *newdisplayplayer, boolean forced)
{
	Hook_State hook;
	if (prepare_hook(&hook, 0, Hook(ViewpointSwitch)))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		LUA_PushUserdata(gL, newdisplayplayer, META_PLAYER);
		lua_pushboolean(gL, forced);

		hud_running = true; // local hook
		call_hooks(&hook, 3, 1, res_force);
		hud_running = false;
	}
	return hook.status;
}

#ifdef SEENAMES
int LUA_HookSeenPlayer(player_t *player, player_t *seenfriend)
{
	Hook_State hook;
	if (prepare_hook(&hook, true, Hook(SeenPlayer)))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		LUA_PushUserdata(gL, seenfriend, META_PLAYER);

		hud_running = true; // local hook
		call_hooks(&hook, 2, 1, res_false);
		hud_running = false;
	}
	return hook.status;
}
#endif // SEENAMES

int LUA_HookShouldJingleContinue(player_t *player, const char *musname)
{
	Hook_State hook;
	if (prepare_string_hook
			(&hook, false, String_Hook(ShouldJingleContinue), musname))
	{
		LUA_PushUserdata(gL, player, META_PLAYER);
		push_string();

		hud_running = true; // local hook
		call_hooks(&hook, 2, 1, res_true);
		hud_running = false;
	}
	return hook.status;
}

boolean hook_cmd_running = false;

static void update_music_name(struct MusicChange *musicchange)
{
	size_t length;
	const char * new = lua_tolstring(gL, -6, &length);

	if (length < 7)
	{
		strcpy(musicchange->newname, new);
		lua_pushvalue(gL, -6);/* may as well keep it for next call */
	}
	else
	{
		memcpy(musicchange->newname, new, 6);
		musicchange->newname[6] = '\0';
		lua_pushlstring(gL, new, 6);
	}

	lua_replace(gL, -7);
}

static void res_musicchange(Hook_State *hook)
{
	struct MusicChange *musicchange = hook->userdata;

	// output 1: true, false, or string musicname override
	if (lua_isstring(gL, -6))
		update_music_name(musicchange);
	else if (lua_isboolean(gL, -6) && lua_toboolean(gL, -6))
		hook->status = true;

	// output 2: mflags override
	if (lua_isnumber(gL, -5))
		*musicchange->mflags = lua_tonumber(gL, -5);
	// output 3: looping override
	if (lua_isboolean(gL, -4))
		*musicchange->looping = lua_toboolean(gL, -4);
	// output 4: position override
	if (lua_isboolean(gL, -3))
		*musicchange->position = lua_tonumber(gL, -3);
	// output 5: prefadems override
	if (lua_isboolean(gL, -2))
		*musicchange->prefadems = lua_tonumber(gL, -2);
	// output 6: fadeinms override
	if (lua_isboolean(gL, -1))
		*musicchange->fadeinms = lua_tonumber(gL, -1);
}

int LUA_HookMusicChange(const char *oldname, struct MusicChange *param)
{
	Hook_State hook;
	if (prepare_hook(&hook, false, Hook(MusicChange)))
	{
		int n;
		int k;

		init_hook_call(&hook, 7, 6, res_musicchange);
		hook.userdata = param;

		lua_pushstring(gL, oldname);/* the only constant value */
		lua_pushstring(gL, param->newname);/* semi constant */

		get_hook_table(&hook);
		n = lua_objlen(gL, -1);

		for (k = 1; k <= n; ++k) {
			lua_pushvalue(gL, -3);
			lua_pushvalue(gL, -3);
			lua_pushinteger(gL, *param->mflags);
			lua_pushboolean(gL, *param->looping);
			lua_pushinteger(gL, *param->position);
			lua_pushinteger(gL, *param->prefadems);
			lua_pushinteger(gL, *param->fadeinms);

			call_single_hook_no_copy(&hook);
		}

		lua_settop(gL, 0);
	}
	return hook.status;
}
