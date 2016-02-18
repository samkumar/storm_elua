#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "platform.h"
#include "lrotable.h"
#include "platform_conf.h"
#include "auxmods.h"
#include <string.h>
#include <stdint.h>
#include <interface.h>
#include <stdlib.h>

#include "libstorm.h"

typedef struct { int8_t foo; } foo_t;
struct bwoob_hdr {
    char command[4];
    char space1;
    char framelength[10];
    char space2;
    char seqno[10];
    char newline;
};

void bwoob_setcmd(struct bwoob_hdr* oobh, const char* cmd) {
    memcpy(oobh->command, cmd, 4);
    oobh->space1 = ' ';
}

void bwoob_setframelength(struct bwoob_hdr* oobh, uint32_t framelen) {
    sprintf(oobh->framelength, "%010lu", framelen);
    oobh->space2 = ' ';
}

void bwoob_setseqno(struct bwoob_hdr* oobh, uint32_t seqno) {
    sprintf(oobh->seqno, "%010lu", seqno);
    oobh->newline = '\n';
}

#define KV_FIELDTYPE 0
#define PO_FIELDTYPE 1
#define RO_FIELDTYPE 2
/* storm.bw.sendfield(socket, fieldtype, key, value) */
int libbosswave_bw_sendfield(lua_State* L) {
    char buf[20];
    size_t fieldlen;
    int finaltop;
    int i = lua_gettop(L) + 1;
    int fieldtype = luaL_checkint(L, 2);
    if (fieldtype == KV_FIELDTYPE) {
        lua_pushstring(L, "kv ");
    } else if (fieldtype == PO_FIELDTYPE) {
        lua_pushstring(L, "po ");
    } else {
        lua_pushstring(L, "ro ");
    }
    lua_pushvalue(L, 3);
    luaL_checklstring(L, 4, &fieldlen);
    snprintf(buf, sizeof(buf), " %u\n", fieldlen);
    lua_pushstring(L, buf);
    lua_pushvalue(L, 4);
    lua_pushstring(L, "\n");
    finaltop = lua_gettop(L);
    for (; i <= finaltop; i++) {
        lua_pushlightfunction(L, libstorm_net_tcpsend);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, i);
        lua_call(L, 2, 0);
        // Squelch error messages for now.
    }
    return 0;
}

// Lua: storm.bw.sendhdr(socket, command, seqno)
// Sends a Bosswave Out-Of-Band header on an active TCP socket.
int libbosswave_bw_sendhdr(lua_State* L) {
    /* This converts the header to a Lua string.
       I could make this more efficient, but this is good enough for now. */
    struct bwoob_hdr oobh;
    const char* command = luaL_checkstring(L, 2);
    uint32_t seqno = luaL_checkint(L, 3);
    bwoob_setcmd(&oobh, command);
    // We're sending to a router, so this is enough.
    bwoob_setframelength(&oobh, 0);
    bwoob_setseqno(&oobh, seqno);
    
    lua_pushlightfunction(L, libstorm_net_tcpsend);
    lua_pushvalue(L, 1);
    lua_pushlstring(L, (const char*) &oobh, sizeof(oobh));
    lua_call(L, 2, 0);
    
    return 0;
}

// Lua: storm.bw.sendend(socket)
int libbosswave_bw_sendend(lua_State* L) {
    lua_pushlightfunction(L, libstorm_net_tcpsend);
    lua_pushvalue(L, 1);
    lua_pushstring(L, "end\n");
    lua_call(L, 2, 0);
    return 0;
}

// Lua: storm.bw.sendmsg(socket, seqno, msg, kv, po, ro)
int libbosswave_bw_sendmsg(lua_State* L) {
    int i;
    int numargs = lua_gettop(L);
    
    lua_pushlightfunction(L, libbosswave_bw_sendhdr);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 2);
    lua_call(L, 3, 0);
    
    for (i = 4; i <= numargs; i++) {
        if (lua_isnil(L, i)) {
            continue;
        }
        lua_pushnil(L);
        while (lua_next(L, i) != 0) {
            lua_pushlightfunction(L, libbosswave_bw_sendfield);
            lua_pushvalue(L, 1);
            lua_pushnumber(L, i - 4);
            lua_pushvalue(L, -5);
            lua_pushvalue(L, -5);
            lua_call(L, 4, 0);
            lua_pop(L, 1);
        }
    }
    
    lua_pushlightfunction(L, libbosswave_bw_sendend);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    
    return 1;
}

int recvhdr_tail(lua_State* L) {
    size_t hdrlen;
    size_t seqno, framelength;
    struct bwoob_hdr oobh;
    const char* recvd = luaL_checklstring(L, 1, &hdrlen);
    if (hdrlen != sizeof(oobh)) {
        goto error;
    }
    memcpy(&oobh, recvd, sizeof(oobh));
    if (oobh.space1 != ' ' || oobh.space2 != ' ' || oobh.newline != '\n') {
        goto error;
    }
    lua_pushlstring(L, oobh.seqno, sizeof(oobh.seqno));
    seqno = (size_t) lua_tointeger(L, -1);
    lua_pushlstring(L, oobh.framelength, sizeof(oobh.framelength));
    framelength = (size_t) lua_tointeger(L, -1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushlstring(L, oobh.command, sizeof(oobh.command));
    lua_pushnumber(L, seqno);
    lua_pushnumber(L, framelength);
    lua_call(L, 3, 0);
    return 0;
  error:
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnil(L);
    lua_call(L, 3, 0);
    return 0;
}

// Lua: storm.bw.recvhdr(socket, callback)
// callback takes three arguments: (1) the type of message, (2) the sequence number, and (3) the frame length
int libbosswave_bw_recvhdr(lua_State* L) {
    lua_pushlightfunction(L, libstorm_net_tcprecvfull);
    lua_pushvalue(L, 1);
    lua_pushnumber(L, sizeof(struct bwoob_hdr));
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, recvhdr_tail, 1);
    lua_call(L, 3, 0);
    return 0;
}

int readuntil_helper(lua_State* L) {
    size_t recvlen;
    const char* recvd = luaL_checklstring(L, 1, &recvlen);
    const char* match;
    int errno = luaL_checkint(L, 2);
    int concatindex;
    if (recvlen == 0 || errno != 0) {
        lua_pushvalue(L, lua_upvalueindex(3));
        lua_pushvalue(L, lua_upvalueindex(4));
        lua_pushvalue(L, 2); // errno
        lua_call(L, 2, 0);
        return 0;
    }
    match = lua_tostring(L, lua_upvalueindex(2));
    lua_pushvalue(L, lua_upvalueindex(4));
    lua_pushvalue(L, 1);
    lua_concat(L, 2);
    concatindex = lua_gettop(L);
    if (*match == *recvd) {
        // We're done
        lua_pushvalue(L, lua_upvalueindex(3));
        lua_pushvalue(L, concatindex); // the concatenated string
        lua_pushnumber(L, 0); // the errno, which is zero
        lua_call(L, 2, 0);
    } else {
        lua_pushlightfunction(L, libstorm_os_invoke_later);
        lua_pushnumber(L, 0);
        
        lua_pushlightfunction(L, libstorm_net_tcprecvfull);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushnumber(L, 1);
        
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_pushvalue(L, lua_upvalueindex(3));
        lua_pushvalue(L, concatindex);
        lua_pushcclosure(L, readuntil_helper, 4);
        
        lua_call(L, 5, 0);
    }
    return 0;
}

// First argument: active TCP socket. Second argument: character to read until. Third argument: callback to invoke upon completion.
int readuntil(lua_State* L) {
    size_t charlen;
    luaL_checklstring(L, 2, &charlen);
    if (charlen != 1) {
        return luaL_error(L, "Character must be at most one byte");
    }
    
    lua_pushlightfunction(L, libstorm_net_tcprecvfull);
    lua_pushvalue(L, 1);
    lua_pushnumber(L, 1);
    
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_pushstring(L, "");
    lua_pushcclosure(L, readuntil_helper, 4);
    
    lua_call(L, 3, 0);
    return 0;
}

#define BW_PARSE_SUCCESS 0
#define BW_PARSE_ERROR 1

int parse_field_header(const char* hdrstr, size_t hdrlen,
                        const char** type, size_t* typelen,
                        const char** key, size_t* keylen,
                        const char** value, size_t* valuelen) {
    register size_t i;
    size_t start;
    
    if (hdrlen == 0) {
        return BW_PARSE_ERROR;
    }
    
    *type = hdrstr;
    for (i = 0; hdrstr[i] != ' ' && hdrstr[i] != '\n' && i != hdrlen; i++);
    *typelen = i;
    
    if (i == hdrlen) {
        return BW_PARSE_ERROR;
    } else if (hdrstr[i] == '\n') {
        // This means we reached the end early
        *key = NULL;
        *keylen = 0;
        *value = NULL;
        *valuelen = 0;
        return 0;
    }
    
    start = i + 1;
    *key = &hdrstr[start];
    for (i = start; hdrstr[i] != ' ' && i != hdrlen; i++);
    *keylen = i - start;
    
    if (i == hdrlen) {
        return BW_PARSE_ERROR;
    }
    
    start = i + 1;
    *value = &hdrstr[start];
    for (i = start; hdrstr[i] != '\n' && i != hdrlen; i++);
    *valuelen = i - start;
    
    if (i != hdrlen - 1) {
        return BW_PARSE_ERROR;
    }
    
    return BW_PARSE_SUCCESS;
}

int recvfield_parse(lua_State* L) {
    const char* hdrstr;
    size_t hdrlen;
    
    int parserv;
    const char* type;
    size_t typelen;
    const char* key;
    size_t keylen;
    const char* value;
    size_t valuelen;
   
    size_t valueint;
    
    int errno = luaL_checkint(L, 2);
    if (errno != 0) {
        goto error;
    }
    hdrstr = lua_tolstring(L, 1, &hdrlen);
    
    /* Now that we have the header as a C string, we can actually parse it. */
    parserv = parse_field_header(hdrstr, hdrlen, &type, &typelen, &key, &keylen, &value, &valuelen);
    if (parserv != BW_PARSE_SUCCESS) {
        goto error;
    }
    
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushlstring(L, type, typelen);
    if (key != NULL) {
        lua_pushlstring(L, key, keylen);
    } else {
        lua_pushnil(L);
    }
    if (value != NULL) {
        // Convert value to integer
        lua_pushlstring(L, value, valuelen);
        valueint = (size_t) lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (valueint == 0) {
            goto error;
        }
        lua_pushnumber(L, valueint);
    } else {
        lua_pushnil(L);
    }
    lua_call(L, 3, 0);
    return 0;
    
  error:
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnil(L);
    lua_call(L, 3, 0);
    return 0;
}

// Lua: storm.bw.recvfieldhdr(socket, callback)
int libbosswave_bw_recvfieldhdr(lua_State* L) {
    lua_pushlightfunction(L, readuntil);
    lua_pushvalue(L, 1);
    lua_pushstring(L, "\n");
    
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, recvfield_parse, 1);
    
    lua_call(L, 3, 0);
    return 0;
}

/* Arguments are going to be: fieldvalue, error. */
int recvfield_tail(lua_State* L) {
    size_t valuelen, expvaluelen;
    const char* value;
    int errno = luaL_checkint(L, 2);
    if (errno != 0) {
        goto error;
    }
    expvaluelen = (size_t) lua_tointeger(L, lua_upvalueindex(3));
    value = luaL_checklstring(L, 1, &valuelen);
    if (valuelen != expvaluelen) {
        goto error;
    }
    
    lua_pushvalue(L, lua_upvalueindex(4));
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushlstring(L, value, valuelen - 1); // minus one to trim the newline
    lua_call(L, 3, 0);
    return 0;
    
  error:
    lua_pushvalue(L, lua_upvalueindex(4));
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnil(L);
    lua_call(L, 3, 0);
    return 0;
}

/* Arguments are going to be: fieldtype, key, valuelength. */
int recvfield_getvalue(lua_State* L) {
    int vallen;
    const char* fieldtype = luaL_checkstring(L, 1);
    if (strcmp(fieldtype, "end") == 0) {
        // Handle end of frame case
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        lua_pushnil(L);
        lua_call(L, 3, 0);
        return 0;
    } else if (lua_isnil(L, 1)) {
        // Handle error case
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        lua_call(L, 3, 0);
        return 0;
    }
    
    vallen = luaL_checkint(L, 3) + 1;
    lua_pushlightfunction(L, libstorm_net_tcprecvfull);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushinteger(L, vallen);
    
    lua_pushvalue(L, 1); // fieldtype
    lua_pushvalue(L, 2); // key
    lua_pushinteger(L, vallen); // expected length of read
    lua_pushvalue(L, lua_upvalueindex(2)); // callback
    lua_pushcclosure(L, recvfield_tail, 4);
    
    lua_call(L, 3, 0);
    return 0;
}

// Lua: storm.bw.recvfield(socket, callback)
int libbosswave_bw_recvfield(lua_State* L) {
    lua_pushlightfunction(L, libbosswave_bw_recvfieldhdr);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, recvfield_getvalue, 2);
    lua_call(L, 2, 0);
    return 0;
}

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"
const LUA_REG_TYPE libbosswave_bw_map[] =
{
    { LSTRKEY( "sendmsg" ), LFUNCVAL( libbosswave_bw_sendmsg ) },
    { LSTRKEY( "sendhdr" ), LFUNCVAL( libbosswave_bw_sendhdr ) },
    { LSTRKEY( "sendfield" ), LFUNCVAL( libbosswave_bw_sendfield ) },
    { LSTRKEY( "sendend" ), LFUNCVAL( libbosswave_bw_sendend ) },
    { LSTRKEY( "recvfield" ), LFUNCVAL( libbosswave_bw_recvfield) },
    { LSTRKEY( "recvhdr" ), LFUNCVAL( libbosswave_bw_recvhdr ) },
    { LSTRKEY( "recvfieldhdr" ), LFUNCVAL( libbosswave_bw_recvfieldhdr ) },
    { LSTRKEY( "KV_FIELDTYPE" ), LNUMVAL( KV_FIELDTYPE ) },
    { LSTRKEY( "PO_FIELDTYPE" ), LNUMVAL( PO_FIELDTYPE ) },
    { LSTRKEY( "RO_FIELDTYPE" ), LNUMVAL( RO_FIELDTYPE ) },
    { LNILKEY, LNILVAL }
};
