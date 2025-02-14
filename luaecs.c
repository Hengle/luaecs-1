#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "luaecs.h"

#define MAX_COMPONENT 256
#define ENTITY_REMOVED 0
#define DEFAULT_SIZE 128
#define STRIDE_TAG 0
#define STRIDE_LUA -1
#define STRIDE_ORDER -2
#define DUMMY_PTR (void *)(uintptr_t)(~0)
#define REARRANGE_THRESHOLD 0x80000000

struct component_pool {
	int cap;
	int n;
	int stride;	// -1 means lua object
	int last_lookup;
	unsigned int *id;
	void *buffer;
};

struct entity_world {
	unsigned int max_id;
	struct component_pool c[MAX_COMPONENT];
};

static void
init_component_pool(struct entity_world *w, int index, int stride, int opt_size) {
	struct component_pool *c = &w->c[index];
	c->cap = opt_size;
	c->n = 0;
	c->stride = stride;
	c->id = NULL;
	c->last_lookup = 0;
	if (stride > 0) {
		c->buffer = NULL;
	} else {
		c->buffer = DUMMY_PTR;
	}
}

static void
entity_new_type(lua_State *L, struct entity_world *w, int cid, int stride, int opt_size) {
	if (opt_size <= 0) {
		opt_size = DEFAULT_SIZE;
	}
	if (cid < 0 || cid >=MAX_COMPONENT || w->c[cid].cap != 0) {
		luaL_error(L, "Can't new type %d", cid);
	}
	init_component_pool(w, cid, stride, opt_size);
}

static inline struct entity_world *
getW(lua_State *L) {
	return (struct entity_world *)luaL_checkudata(L, 1, "ENTITY_WORLD");
}

static int
lnew_type(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = luaL_checkinteger(L, 2);
	int stride = luaL_checkinteger(L, 3);
	int size = luaL_optinteger(L, 4, 0);
	entity_new_type(L, w, cid, stride, size);
	return 0;
}

static int
lcount_memory(lua_State *L) {
	struct entity_world *w = getW(L);
	size_t sz = sizeof(*w);
	int i;
	size_t msz = sz;
	for (i=0;i<MAX_COMPONENT;i++) {
		struct component_pool *c = &w->c[i];
		if (c->id) {
			sz += c->cap * sizeof(unsigned int);
			msz += c->n * sizeof(unsigned int);
		}
		if (c->buffer != DUMMY_PTR) {
			sz += c->cap * c->stride;
			msz += c->cap * c->stride;
		}
	}
	lua_pushinteger(L, sz);
	lua_pushinteger(L, msz);
	return 2;
}

static void
shrink_component_pool(lua_State *L, struct component_pool *c, int id) {
	if (c->id == NULL)
		return;
	if (c->n == 0) {
		c->id = NULL;
		if (c->stride > 0)
			c->buffer = NULL;
		lua_pushnil(L);
		lua_setiuservalue(L, 1, id * 2 + 1);
		lua_pushnil(L);
		lua_setiuservalue(L, 1, id * 2 + 2);
	} else if (c->stride > 0 && c->n < c->cap) {
		c->cap = c->n;
		c->id = (unsigned int *)lua_newuserdatauv(L, c->n * sizeof(unsigned int), 0);
		lua_setiuservalue(L, 1, id * 2 + 1);
		c->buffer = lua_newuserdatauv(L, c->n * c->stride, 0);
		lua_setiuservalue(L, 1, id * 2 + 2);
	}
}

static int
lcollect_memory(lua_State *L) {
	struct entity_world *w = getW(L);
	int i;
	for (i=0;i<MAX_COMPONENT;i++) {
		shrink_component_pool(L, &w->c[i], i);
	}
	return 0;
}

static int
add_component_id_(lua_State *L, int world_index, struct entity_world *w, int cid, unsigned int eid) {
	struct component_pool *pool = &w->c[cid];
	int cap = pool->cap;
	int index = pool->n;
	if (pool->n == 0) {
		if (pool->id == NULL) {
			pool->id = (unsigned int *)lua_newuserdatauv(L, cap * sizeof(unsigned int), 0);
			lua_setiuservalue(L, world_index, cid * 2 + 1);
		}
		if (pool->buffer == NULL) {
			pool->buffer = lua_newuserdatauv(L, cap * pool->stride, 0);
			lua_setiuservalue(L, world_index, cid * 2 + 2);
		} else if (pool->stride == STRIDE_LUA) {
			lua_newtable(L);
			lua_setiuservalue(L, world_index, cid * 2 + 2);
		}
	} else if (pool->n >= pool->cap) {
		// expand pool
		int newcap = cap * 3 / 2;
		unsigned int *newid = (unsigned int *)lua_newuserdatauv(L, newcap * sizeof(unsigned int), 0);
		lua_setiuservalue(L, world_index, cid * 2 + 1);
		memcpy(newid, pool->id,  cap * sizeof(unsigned int));
		pool->id = newid;
		int stride = pool->stride;
		if (stride > 0) {
			void *newbuffer = lua_newuserdatauv(L, newcap * stride, 0);
			lua_setiuservalue(L, world_index, cid * 2 + 2);
			memcpy(newbuffer, pool->buffer, cap * stride);
			pool->buffer = newbuffer;
		}
		pool->cap = newcap;
	}
	++pool->n;
	pool->id[index] = eid;
	if (pool->stride != STRIDE_ORDER && index > 0 && eid < pool->id[index-1]) {
		luaL_error(L, "Add component %d fail", cid);
	}
	return index;
}

static inline void *
get_ptr(struct component_pool *c, int index) {
	if (c->stride > 0)
		return (void *)((char *)c->buffer + c->stride * index);
	else
		return DUMMY_PTR;
}

static void *
add_component_(lua_State *L, int world_index, struct entity_world *w, int cid, unsigned int eid, const void *buffer) {
	int index = add_component_id_(L, world_index, w, cid, eid);
	struct component_pool *pool = &w->c[cid];
	void *ret = get_ptr(pool, index);
	if (buffer) {
		assert(pool->stride >= 0);
		memcpy(ret, buffer, pool->stride);
	}
	return ret;
}

static inline int
check_cid(lua_State *L, struct entity_world *w, int index) {
	int cid = luaL_checkinteger(L, index);
	struct component_pool *c = &w->c[cid];
	if (cid < 0 || 	cid >=MAX_COMPONENT || c->cap == 0) {
		luaL_error(L, "Invalid type %d", cid);
	}
	return cid;
}

static int
ladd_component(lua_State *L) {
	struct entity_world *w = getW(L);
	unsigned int eid = luaL_checkinteger(L, 2);
	int cid = check_cid(L, w, 3);
	int index = add_component_id_(L, 1, w, cid, eid);
	lua_pushinteger(L, index + 1);
	return 1;
}

static int
lnew_entity(lua_State *L) {
	struct entity_world *w = getW(L);
	unsigned int eid = ++w->max_id;
	assert(eid != 0);
	lua_pushinteger(L, eid);
	return 1;
}

static void
insert_id(lua_State *L, int world_index, struct entity_world *w, int cid, unsigned int eid) {
	struct component_pool *c = &w->c[cid];
	assert(c->stride == STRIDE_TAG);
	int from = 0;
	int to = c->n;
	while(from < to) {
		int mid = (from + to)/2;
		int aa = c->id[mid];
		if (aa == eid)
			return;
		else if (aa < eid) {
			from = mid + 1;
		} else {
			to = mid;
		}
	}
	// insert eid at [from]
	if (from < c->n - 1) {
		int i;
		// Any dup id ?
		for (i=from;i<c->n-1;i++) {
			if (c->id[i] == c->id[i+1]) {
				memmove(c->id + from + 1, c->id + from, sizeof(unsigned int) * (i - from));
				c->id[from] = eid;
				return;
			}
		}
	}
	// 0xffffffff max uint avoid check
	add_component_id_(L, world_index, w, cid, 0xffffffff);
	memmove(c->id + from + 1, c->id + from, sizeof(unsigned int) * (c->n - from - 1));
	c->id[from] = eid;
}

static void
entity_enable_tag_(struct entity_world *w, int cid, int index, int tag_id, void *L, int world_index) {
	struct component_pool *c = &w->c[cid];
	assert(index >=0 && index < c->n);
	unsigned int eid = c->id[index];
	insert_id((lua_State *)L, world_index, w, tag_id, eid);
}

static int
binary_search(unsigned int *a, int from, int to, unsigned int v) {
	while(from < to) {
		int mid = (from + to)/2;
		int aa = a[mid];
		if (aa == v)
			return mid;
		else if (aa < v) {
			from = mid + 1;
		} else {
			to = mid;
		}
	}
	return -1;
}

#define GUESS_RANGE 64

static inline int
lookup_component(struct component_pool *pool, unsigned int eid, int guess_index) {
	int n = pool->n;
	if (n == 0)
		return -1;
	if (guess_index < 0 || guess_index >= pool->n)
		return binary_search(pool->id, 0, pool->n, eid);
	unsigned int *a = pool->id;
	int lower = a[guess_index];
	if (eid <= lower) {
		if (eid == lower)
			return guess_index;
		return binary_search(a, 0, guess_index, eid);
	}
	if (guess_index + GUESS_RANGE*2 >= pool->n) {
		return binary_search(a, guess_index + 1, pool->n, eid);
	}
	int higher = a[guess_index + GUESS_RANGE];
	if (eid > higher) {
		return binary_search(a, guess_index + GUESS_RANGE + 1, pool->n, eid);
	}
	return binary_search(a, guess_index + 1, guess_index + GUESS_RANGE + 1, eid);
}

static inline void
replace_id(struct component_pool *c, int from, int to, unsigned int eid) {
	int i;
	for (i=from;i<to;i++) {
		c->id[i] = eid;
	}
}

static void
entity_disable_tag_(struct entity_world *w, int cid, int index, int tag_id) {
	struct component_pool *c = &w->c[cid];
	assert(index >=0 && index < c->n);
	unsigned int eid = c->id[index];
	if (cid != tag_id) {
		c = &w->c[tag_id];
		index = lookup_component(c, eid, c->last_lookup);
		if (index < 0)
			return;
	}
	int from,to;
	// find next tag. You may disable subsquent tags in iteration.
	// For example, The sequence is 1 3 5 7 9 . We are now on 5 , and disable 7 .
	// We should change 7 to 9 ( 1 3 5 9 9 ) rather than 7 to 5 ( 1 3 5 5 9 )
	//                   iterator ->   ^                                ^
	for (to = index+1; to<c->n; to++) {
		if (c->id[to] != eid) {
			for (from = index-1; from>=0; from--) {
				if (c->id[from] != eid)
					break;
			}
			replace_id(c, from+1, to, c->id[to]);
			return;
		}
	}
	for (from = index-1; from>=0; from--) {
		if (c->id[from] != eid)
			break;
	}
	c->n = from + 1;
}

static void
entity_remove_(struct entity_world *w, int cid, int index, void *L, int world_index) {
	entity_enable_tag_(w, cid, index, ENTITY_REMOVED, L, world_index);
}

struct rearrange_context {
	struct entity_world *w;
	unsigned int ptr[MAX_COMPONENT-1];
};

static int
find_min(struct rearrange_context *ctx) {
	unsigned int m = ~0;
	int i;
	int r = -1;
	struct entity_world *w = ctx->w;
	for (i=1;i<MAX_COMPONENT;i++) {
		int index = ctx->ptr[i-1];
		if (index < w->c[i].n) {
			if (w->c[i].id[index] <= m) {
				m = w->c[i].id[index];
				r = i;
			}
		}
	}
	return r;
}

static void
rearrange(struct entity_world *w) {
	struct rearrange_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.w = w;
	int cid;
	unsigned int new_id = 1;
	unsigned int last_id = 0;
	while ((cid = find_min(&ctx)) >= 0) {
		int index = ctx.ptr[cid-1];
		unsigned int current_id = w->c[cid].id[index];
//		printf("arrange %d <- %d\n", new_id, w->c[cid].id[index]);
		w->c[cid].id[index] = new_id;
		if (current_id != last_id) {
			++new_id;
			last_id = current_id;
		}
		++ctx.ptr[cid-1];
	}
	w->max_id = new_id;
}

static inline void
move_tag(struct component_pool *pool, int from, int to) {
	if (from != to) {
		pool->id[to] = pool->id[from];
	}
}

static inline void
move_item(struct component_pool *pool, int from, int to) {
	if (from != to) {
		pool->id[to] = pool->id[from];
		int stride = pool->stride;
		memcpy((char *)pool->buffer + to * stride, (char *)pool->buffer + from * stride, stride);
	}
}

static void
move_object(lua_State *L, struct component_pool *pool, int from, int to) {
	if (from != to) {
		pool->id[to] = pool->id[from];
		lua_rawgeti(L, -1, from+1);
		lua_rawseti(L, -2, to+1);
	}
}

static void
remove_all(lua_State *L, struct component_pool *pool, struct component_pool *removed, int cid) {
	int index = 0;
	int count = 0;
	int i;
	if (pool->stride != STRIDE_ORDER) {
		unsigned int *id = removed->id;
		unsigned int last_id = 0;
		for (i=0;i<removed->n;i++) {
			if (id[i] != last_id) {
				// todo : order
				int r = lookup_component(pool, id[i], index);
				if (r >= 0) {
					index = r;
					pool->id[r] = 0;
					++count;
				}
			}
		}
	} else {
		unsigned int *id = pool->id;
		for (i=0;i<pool->n;i++) {
			int r = lookup_component(removed, id[i], 0);
			if (r >= 0) {
				id[i] = 0;
				++count;
			}
		}
	}
	if (count > 0) {
		index = 0;
		switch (pool->stride) {
		case STRIDE_LUA:
			if (lua_getiuservalue(L, 1, cid * 2 + 2) != LUA_TTABLE) {
				luaL_error(L, "Missing lua object table for type %d", cid);
			}
			for (i=0;i<pool->n;i++) {
				if (pool->id[i] != 0) {
					move_object(L, pool, i, index);
					++index;
				}
			}
			lua_pop(L, 1);	// pop lua object table
			break;
		case STRIDE_TAG:
		case STRIDE_ORDER:
			for (i=0;i<pool->n;i++) {
				if (pool->id[i] != 0) {
					move_tag(pool, i, index);
					++index;
				}
			}
			break;
		default:
			for (i=0;i<pool->n;i++) {
				if (pool->id[i] != 0) {
					move_item(pool, i, index);
					++index;
				}
			}
			break;
		}
		pool->n -= count;
	}
}

static int
lupdate(lua_State *L) {
	struct entity_world *w = getW(L);
	struct component_pool *removed = &w->c[ENTITY_REMOVED];
	int i;
	if (removed->n > 0) {
		// mark removed
		assert(ENTITY_REMOVED == 0);
		for (i=1;i<MAX_COMPONENT;i++) {
			struct component_pool *pool = &w->c[i];
			if (pool->n > 0)
				remove_all(L, pool, removed, i);
		}
		removed->n = 0;
	}

	if (w->max_id > REARRANGE_THRESHOLD) {
		rearrange(w);
	}

	return 0;
}

static void
remove_dup(struct component_pool *c, int index) {
	int i;
	unsigned int eid = c->id[index];
	int to = index;
	for (i=index+1;i<c->n;i++) {
		if (c->id[i] != eid) {
			eid = c->id[i];
			c->id[to] = eid;
			++to;
		}
	}
	c->n = to;
}

static void *
entity_iter_(struct entity_world *w, int cid, int index) {
	struct component_pool *c = &w->c[cid];
	assert(index >= 0);
	if (index >= c->n)
		return NULL;
	if (c->stride == STRIDE_TAG) {
		// it's a tag
		unsigned int eid = c->id[index];
		if (index < c->n - 1 && eid == c->id[index+1]) {
			remove_dup(c, index+1);
		}
		return DUMMY_PTR;
	}
	return get_ptr(c, index);
}

static void *
entity_iter_lua_(struct entity_world *w, int cid, int index, void *L, int world_index) {
	void * ret = entity_iter_(w, cid, index);
	if (ret != DUMMY_PTR)
		return ret;
	if (lua_getiuservalue(L, world_index, cid * 2 + 2) != LUA_TTABLE) {
		lua_pop(L, 1);
		return NULL;
	}
	int t = lua_rawgeti(L, -1, index+1);
	switch(t) {
	case LUA_TSTRING:
		ret = (void *)lua_tostring(L, -1);
		break;
	case LUA_TUSERDATA:
	case LUA_TLIGHTUSERDATA:
		ret = (void *)lua_touserdata(L, -1);
		break;
	default:
		ret = NULL;
		break;
	}
	lua_pop(L, 2);
	return ret;
}

static int
entity_assign_lua_(struct entity_world *w, int cid, int index, void *L, int world_index) {
	struct component_pool *c = &w->c[cid];
	++index;
	assert(lua_gettop(L) > 1);
	if (c->stride != STRIDE_LUA || index <=0 || index > c->n) {
		lua_pop(L, 1);
		return 0;
	}
	if (lua_getiuservalue(L, world_index, cid * 2 + 2) != LUA_TTABLE) {
		lua_pop(L, 2);
		return 0;
	}
	lua_insert(L, -2);
	// table value
	lua_rawseti(L, -2, index);
	lua_pop(L, 1);
	return 1;
}

static void
entity_clear_type_(struct entity_world *w, int cid) {
	struct component_pool *c = &w->c[cid];
	c->n = 0;
}

static int
lclear_type(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = check_cid(L,w, 2);
	entity_clear_type_(w, cid);
	return 0;
}

static int
entity_sibling_index_(struct entity_world *w, int cid, int index, int silbling_id) {
	struct component_pool *c = &w->c[cid];
	if (index < 0 || index >= c->n)
		return 0;
	unsigned int eid = c->id[index];
	c = &w->c[silbling_id];
	assert(c->stride != STRIDE_ORDER);
	int result_index = lookup_component(c, eid, c->last_lookup);
	if (result_index >= 0) {
		c->last_lookup = result_index;
		return result_index + 1;
	}
	return 0;
}

static void *
entity_add_sibling_(struct entity_world *w, int cid, int index, int silbling_id, const void *buffer, void *L, int world_index) {
	struct component_pool *c = &w->c[cid];
	assert(index >=0 && index < c->n);
	unsigned int eid = c->id[index];
	// todo: pcall add_component_
	return add_component_((lua_State *)L, world_index, w, silbling_id, eid, buffer);
}

static int
entity_new_(struct entity_world *w, int cid, const void *buffer, void *L, int world_index) {
	unsigned int eid = ++w->max_id;
	assert(eid != 0);
	struct component_pool *c = &w->c[cid];
	assert(c->cap > 0);
	if (buffer == NULL) {
		return add_component_id_(L, world_index, w, cid, eid);
	} else {
		assert(c->stride >= 0);
		int index = add_component_id_(L, world_index, w, cid, eid);
		void *ret = get_ptr(c, index);
		memcpy(ret, buffer, c->stride);
		return index;
	}
}

static int
entity_add_sibling_index_(lua_State *L, int world_index, struct entity_world *w, int cid, int index, int slibling_id) {
	struct component_pool *c = &w->c[cid];
	assert(index >=0 && index < c->n);
	unsigned int eid = c->id[index];
	// todo: pcall add_component_
	int ret = add_component_id_(L, world_index, w, slibling_id, eid);
	return ret;
}

static int
lcontext(lua_State *L) {
	struct entity_world *w = getW(L);
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_len(L, 2);
	int n = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (n <= 0) {
		return luaL_error(L, "Invalid length %d of table", n);
	}
	size_t sz = sizeof(struct ecs_context) + sizeof(int) * n;
	struct ecs_context *ctx = (struct ecs_context *)lua_newuserdatauv(L, sz, 1);
	ctx->L = (void *)lua_newthread(L);
	lua_pushvalue(L, 1);
	lua_xmove(L, ctx->L, 1);	// put world in the index 1 of newthread
	lua_setiuservalue(L, -2, 1);
	ctx->max_id = n;
	ctx->world = w;
	static struct ecs_capi c_api = {
		entity_iter_,
		entity_clear_type_,
		entity_sibling_index_,
		entity_add_sibling_,
		entity_new_,
		entity_remove_,
		entity_enable_tag_,
		entity_disable_tag_,
		entity_iter_lua_,
		entity_assign_lua_,
	};
	ctx->api = &c_api;
	ctx->cid[0] = ENTITY_REMOVED;
	int i;
	for (i=1;i<=n;i++) {
		if (lua_geti(L, 2, i) != LUA_TNUMBER) {
			return luaL_error(L, "Invalid id at index %d", i);
		}
		ctx->cid[i] = lua_tointeger(L, -1);
		lua_pop(L, 1);
		int cid = ctx->cid[i];
		if (cid == ENTITY_REMOVED || cid < 0 || cid >= MAX_COMPONENT)
			return luaL_error(L, "Invalid id (%d) at index %d", cid, i);
	}
	return 1;
}

static int
lnew_world(lua_State *L) {
	size_t sz = sizeof(struct entity_world);
	struct entity_world *w = (struct entity_world *)lua_newuserdatauv(L, sz, MAX_COMPONENT * 2);
	memset(w, 0, sz);
	// removed set
	entity_new_type(L, w, ENTITY_REMOVED, 0, 0);
	luaL_getmetatable(L, "ENTITY_WORLD");
	lua_setmetatable(L, -2);
	return 1;
}

#define TYPE_INT 0
#define TYPE_FLOAT 1
#define TYPE_BOOL 2
#define TYPE_INT64 3
#define TYPE_DWORD 4
#define TYPE_WORD 5
#define TYPE_BYTE 6
#define TYPE_DOUBLE 7
#define TYPE_USERDATA 8
#define TYPE_COUNT 9

struct field {
	const char *key;
	int offset;
	int type;
};

static int
check_type(lua_State *L) {
	int type = lua_tointeger(L, -1);
	if (type < 0 || type >= TYPE_COUNT) {
		luaL_error(L, "Invalid field type(%d)", type);
	}
	lua_pop(L, 1);
	return type;
}

static void
get_field(lua_State *L, int i, struct field *f) {
	if (lua_geti(L, -1, 1) != LUA_TNUMBER) {
		luaL_error(L, "Invalid field %d [1] type", i);
	}
	f->type = check_type(L);

	if (lua_geti(L, -1, 2) != LUA_TSTRING) {
		luaL_error(L, "Invalid field %d [2] key", i);
	}
	f->key = lua_tostring(L, -1);
	lua_pop(L, 1);

	if (lua_geti(L, -1, 3) != LUA_TNUMBER) {
		luaL_error(L, "Invalid field %d [3] offset", i);
	}
	f->offset = lua_tointeger(L, -1);
	lua_pop(L, 1);

	lua_pop(L, 1);
}

static void
write_value(lua_State *L, struct field *f, char *buffer) {
	int luat = lua_type(L, -1);
	char *ptr = buffer + f->offset;
	switch (f->type) {
		case TYPE_INT:
			if (!lua_isinteger(L, -1))
				luaL_error(L, "Invalid .%s type %s (int)", f->key ? f->key : "*", lua_typename(L, luat));
			*(int *)ptr = lua_tointeger(L, -1);
			break;
		case TYPE_FLOAT:
			if (luat != LUA_TNUMBER)
				luaL_error(L, "Invalid .%s type %s (float)", f->key ? f->key : "*", lua_typename(L, luat));
			*(float *)ptr = lua_tonumber(L, -1);
			break;
		case TYPE_BOOL:
			if (luat != LUA_TBOOLEAN)
				luaL_error(L, "Invalid .%s type %s (bool)", f->key ? f->key : "*", lua_typename(L, luat));
			*(unsigned char *)ptr = lua_toboolean(L, -1);
			break;
		case TYPE_INT64:
			if (!lua_isinteger(L, -1))
				luaL_error(L, "Invalid .%s type %s (int64)", f->key ? f->key : "*", lua_typename(L, luat));
			*(int64_t *)ptr = lua_tointeger(L, -1);
			break;
		case TYPE_DWORD:
			if (!lua_isinteger(L, -1))
				luaL_error(L, "Invalid .%s type %s (uint32)", f->key ? f->key : "*", lua_typename(L, luat));
			else {
				int64_t v = lua_tointeger(L, -1);
				if (v < 0 || v > 0xffffffff) {
					luaL_error(L, "Invalid DWORD %d", (int)v);
				}
				*(uint32_t *)ptr = v;
			}
			break;
		case TYPE_WORD:
			if (!lua_isinteger(L, -1))
				luaL_error(L, "Invalid .%s type %s (uint16)", f->key ? f->key : "*", lua_typename(L, luat));
			else {
				int v = lua_tointeger(L, -1);
				if (v < 0 || v > 0xffff) {
					luaL_error(L, "Invalid WORD %d", v);
				}
				*(uint16_t *)ptr = v;
			}
			break;
		case TYPE_BYTE:
			if (!lua_isinteger(L, -1))
				luaL_error(L, "Invalid .%s type %s (uint8)", f->key ? f->key : "*", lua_typename(L, luat));
			else {
				int v = lua_tointeger(L, -1);
				if (v < 0 || v > 255) {
					luaL_error(L, "Invalid BYTE %d", v);
				}
				*(uint16_t *)ptr = v;
			}
			break;
		case TYPE_DOUBLE:
			if (luat != LUA_TNUMBER)
				luaL_error(L, "Invalid .%s type %s (double)", f->key ? f->key : "*", lua_typename(L, luat));
			*(double *)ptr = lua_tonumber(L, -1);
			break;
		case TYPE_USERDATA:
			if (luat !=  LUA_TLIGHTUSERDATA)
				luaL_error(L, "Invalid .%s type %s (pointer)", f->key ? f->key : "*", lua_typename(L, luat));
			*(void **)ptr = lua_touserdata(L, -1);
			break;
	}
	lua_pop(L, 1);
}

static inline void
write_component(lua_State *L, int field_n, struct field *f, int index, char *buffer) {
	int i;
	for (i=0; i < field_n; i++) {
		lua_getfield(L, index, f[i].key);
		write_value(L, &f[i], buffer);
	}
}

static void
read_value(lua_State *L, struct field *f, const char *buffer) {
	const char * ptr = buffer + f->offset;
	switch (f->type) {
	case TYPE_INT:
		lua_pushinteger(L, *(const int *)ptr);
		break;
	case TYPE_FLOAT:
		lua_pushnumber(L, *(const float *)ptr);
		break;
	case TYPE_BOOL:
		lua_pushboolean(L, *ptr);
		break;
	case TYPE_INT64:
		lua_pushinteger(L, *(const int64_t *)ptr);
		break;
	case TYPE_DWORD:
		lua_pushinteger(L, *(const uint32_t *)ptr);
		break;
	case TYPE_WORD:
		lua_pushinteger(L, *(const uint16_t *)ptr);
		break;
	case TYPE_BYTE:
		lua_pushinteger(L, *(const uint8_t *)ptr);
		break;
	case TYPE_DOUBLE:
		lua_pushnumber(L, *(const double *)ptr);
		break;
	case TYPE_USERDATA:
		lua_pushlightuserdata(L, *(void **)ptr);
		break;
	default:
		// never here
		luaL_error(L, "Invalid field type %d", f->type);
		break;
	}
}

static void
read_component(lua_State *L, int field_n, struct field *f, int index, const char * buffer) {
	int i;
	for (i=0; i < field_n; i++) {
		read_value(L, &f[i], buffer);
		lua_setfield(L, index, f[i].key);
	}
}

static int
get_len(lua_State *L, int index) {
	lua_len(L, index);
	if (lua_type(L, -1) != LUA_TNUMBER) {
		return luaL_error(L, "Invalid table length");
	}
	int n = lua_tointeger(L, -1);
	if (n < 0) {
		return luaL_error(L, "Invalid table length %d", n);
	}
	lua_pop(L, 1);
	return n;
}

#define COMPONENT_IN 1
#define COMPONENT_OUT 2
#define COMPONENT_OPTIONAL 4
#define COMPONENT_OBJECT 8
#define COMPONENT_EXIST 0x10
#define COMPONENT_ABSENT 0x20
#define COMPONENT_FILTER (COMPONENT_EXIST | COMPONENT_ABSENT)

struct group_key {
	const char *name;
	int id;
	int field_n;
	int attrib;
};

static inline int
is_temporary(int attrib) {
	if (attrib & COMPONENT_FILTER)
		return 0;
	return (attrib & COMPONENT_IN) == 0 && (attrib & COMPONENT_OUT) == 0;
}

struct group_iter {
	struct entity_world *world;
	struct field *f;
	int nkey;
	int readonly;
	struct group_key k[1];
};

static int
get_write_component(lua_State *L, int lua_index, const char *name, struct field *f, struct component_pool *c) {
	switch (lua_getfield(L, lua_index, name)) {
	case LUA_TNIL:
		lua_pop(L, 1);
		// restore cache (metatable can be absent during sync)
		if (lua_getmetatable(L, lua_index)) {
			lua_getfield(L, -1, name);
			lua_setfield(L, lua_index, name);
			lua_pop(L, 1);	// pop metatable
		}
		return 0;
	case LUA_TTABLE:
		return 1;
	default:
		if (c->stride == STRIDE_LUA) {
			// lua object
			return 1;
		}
		if (f->key == NULL) {
			// value type
			return 1;
		}
		return luaL_error(L, "Invalid iterator type %s", lua_typename(L, lua_type(L, -1)));
	}
}

static void
write_component_object(lua_State *L, int n, struct field *f, void *buffer) {
	if (f->key == NULL) {
		write_value(L, f, buffer);
	} else {
		write_component(L, n, f, -1, (char *)buffer);
		lua_pop(L, 1);
	}
}

static int
remove_tag(lua_State *L, int lua_index, const char *name) {
	int r = 0;
	switch (lua_getfield(L, lua_index, name)) {
	case LUA_TNIL:
		r = 1;
		break;
	case LUA_TBOOLEAN:
		r = !lua_toboolean(L, -1);
		break;
	default:
		return luaL_error(L, "Invalid tag type %s", lua_typename(L, lua_type(L, -1)));
	}
	lua_pop(L, 1);
	return r;
}

static void
update_iter(lua_State *L, int world_index, int lua_index, struct group_iter *iter, int idx, int mainkey, int skip) {
	struct field *f = iter->f;

	int i;
	for (i=0;i<skip;i++) {
		f += iter->k[i].field_n;
	}
	for (i=skip;i<iter->nkey;i++) {
		struct group_key *k = &iter->k[i];
		if (!(k->attrib & COMPONENT_FILTER)) {
			struct component_pool *c = &iter->world->c[k->id];
			if (c->stride == STRIDE_TAG) {
				// It's a tag
				if ((k->attrib & COMPONENT_OUT)) {
					switch (lua_getfield(L, lua_index, k->name)) {
					case LUA_TNIL:
						break;
					case LUA_TBOOLEAN:
						if (lua_toboolean(L, -1)) {
							entity_enable_tag_(iter->world, mainkey, idx, k->id, L, world_index);
						} else {
							entity_disable_tag_(iter->world, mainkey, idx, k->id);
						}
						if (!(k->attrib & COMPONENT_IN)) {
							// reset tag
							lua_pushnil(L);
							lua_setfield(L, lua_index, k->name);
						}
						break;
					default:
						luaL_error(L, ".%s is a tag , should be a boolean or nil. It's %s", k->name, lua_typename(L, lua_type(L, -1)));
					}
					lua_pop(L, 1);
				}
			} else if (c->stride == STRIDE_ORDER) {
				assert(is_temporary(k->attrib));
				if (lua_getfield(L, lua_index, k->name) != LUA_TNIL) {
					if (!lua_isboolean(L, -1) || lua_toboolean(L, -1) == 0)
						luaL_error(L, "Only support true for order key .%s", k->name);
					lua_pop(L, 1);
					entity_add_sibling_(iter->world, mainkey, idx, k->id, NULL, L, world_index);
					lua_pushnil(L);
					lua_setfield(L, lua_index, k->name);
				}
			} else if ((k->attrib & COMPONENT_OUT)
				&& get_write_component(L, lua_index, k->name, f, c)) {
				int index = entity_sibling_index_(iter->world, mainkey, idx, k->id);
				if (index == 0) {
					luaL_error(L, "Can't find sibling %s of %s", k->name, iter->k[0].name);
				}
				if (c->stride == STRIDE_LUA) {
					if (lua_getiuservalue(L, world_index, k->id * 2 + 2) != LUA_TTABLE) {
						luaL_error(L, "Missing lua table for %d", k->id);
					}
					lua_insert(L, -2);
					lua_rawseti(L, -2, index);
				} else {
					void *buffer = get_ptr(c, index - 1);
					write_component_object(L, k->field_n, f, buffer);
				}
			} else if (is_temporary(k->attrib)
				&& get_write_component(L, lua_index, k->name, f, c)) {
				if (c->stride == STRIDE_LUA) {
					int index = entity_add_sibling_index_(L, world_index, iter->world, mainkey, idx, k->id);
					if (lua_getiuservalue(L, world_index, k->id * 2 + 2) != LUA_TTABLE) {
						luaL_error(L, "Missing lua table for %d", k->id);
					}
					lua_insert(L, -2);
					lua_rawseti(L, -2, index + 1);
				} else {
					void *buffer = entity_add_sibling_(iter->world, mainkey, idx, k->id, NULL, L, world_index);
					write_component_object(L, k->field_n, f, buffer);
				}
			}
		}
		f += k->field_n;
	}
}

static void
update_last_index(lua_State *L, int world_index, int lua_index, struct group_iter *iter, int idx) {
	int mainkey = iter->k[0].id;
	struct component_pool *c = &iter->world->c[mainkey];
	int disable_mainkey = 0;
	if (!(iter->k[0].attrib & COMPONENT_FILTER)) {
		if (c->stride == STRIDE_TAG) {
			// The mainkey is a tag, delay disable
			disable_mainkey = ((iter->k[0].attrib & COMPONENT_OUT) && remove_tag(L, lua_index, iter->k[0].name));
		} else if ((iter->k[0].attrib & COMPONENT_OUT)
			&& get_write_component(L, lua_index, iter->k[0].name, iter->f, c)) {
			struct component_pool *c = &iter->world->c[mainkey];
			if (c->n <= idx) {
				luaL_error(L, "Can't find component %s for index %d", iter->k[0].name, idx);
			}
			if (c->stride == STRIDE_LUA) {
				if (lua_getiuservalue(L, world_index, mainkey * 2 + 2) != LUA_TTABLE) {
					luaL_error(L, "Missing lua table for %d", mainkey);
				}
				lua_insert(L, -2);
				lua_rawseti(L, -2, idx+1);
			} else {
				void * buffer = get_ptr(c, idx);
				write_component_object(L, iter->k[0].field_n, iter->f, buffer);
			}
		}
	}

	update_iter(L, world_index, lua_index, iter, idx, mainkey, 1);

	if (disable_mainkey) {
		entity_disable_tag_(iter->world, mainkey, idx, mainkey);
	}
}

static void
read_component_in_field(lua_State *L, int lua_index, const char *name, int n, struct field *f, void *buffer) {
	if (n == 0) {
		// It's tag
		lua_pushboolean(L, buffer ? 1 : 0);
		lua_setfield(L, lua_index, name);
		return;
	}
	if (f->key == NULL) {
		// value type
		read_value(L, f, buffer);
		lua_setfield(L, lua_index, name);
		return;
	}
	if (lua_getfield(L, lua_index, name) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, lua_index, name);
	}
	read_component(L, n , f, lua_gettop(L), buffer);
	lua_pop(L, 1);
}

// -1 : end ; 0 : next ; 1 : succ
static int
query_index(struct group_iter *iter, int skip, int mainkey, int idx, unsigned int index[MAX_COMPONENT]) {
	if (entity_iter_(iter->world, mainkey, idx) == NULL) {
		return -1;
	}
	int j;
	for (j=skip;j<iter->nkey;j++) {
		struct group_key *k = &iter->k[j];
		if (k->attrib & COMPONENT_ABSENT) {
			if (entity_sibling_index_(iter->world, mainkey, idx, k->id)) {
				// exist. try next
				return 0;
			}
			index[j] = 0;
		} else if (!is_temporary(k->attrib)) {
			index[j] = entity_sibling_index_(iter->world, mainkey, idx, k->id);
			if (index[j] == 0) {
				if (!(k->attrib & COMPONENT_OPTIONAL)) {
					// required. try next
					return 0;
				}
			}
		} else {
			index[j] = 0;
		}
	}
	return 1;
}

static void
check_index(lua_State *L, struct group_iter *iter, int mainkey, int idx) {
	int i;
	for (i=0;i<iter->nkey;i++) {
		struct group_key *k = &iter->k[i];
		if (k->attrib & COMPONENT_ABSENT) {
			if (entity_sibling_index_(iter->world, mainkey, idx, k->id)) {
				luaL_error(L, ".%s should be absent", k->name);
			}
		} else if (!is_temporary(k->attrib)) {
			if (entity_sibling_index_(iter->world, mainkey, idx, k->id) == 0) {
				if (!(k->attrib & COMPONENT_OPTIONAL)) {
					luaL_error(L, ".%s not found", k->name);
				}
			}
		}
	}
}

static void
read_iter(lua_State *L, int world_index, int obj_index, struct group_iter *iter, unsigned int index[MAX_COMPONENT]) {
	struct field *f = iter->f;
	int i;
	for (i=0;i<iter->nkey;i++) {
		struct group_key *k = &iter->k[i];
		if (!(k->attrib & COMPONENT_FILTER)) {
			struct component_pool *c = &iter->world->c[k->id];
			if (c->stride == STRIDE_LUA) {
				// lua object component
				if (index[i]) {
					if (lua_getiuservalue(L, world_index, k->id * 2 + 2) != LUA_TTABLE) {
						luaL_error(L, "Missing lua table for %d", k->id);
					}

					lua_rawgeti(L, -1, index[i]);
					lua_setfield(L, obj_index, k->name);
					lua_pop(L, 1);
				} else {
					lua_pushnil(L);
					lua_setfield(L, obj_index, k->name);
				}
			} else if (c->stride != STRIDE_ORDER) {
				if (k->attrib & COMPONENT_IN) {
					if (index[i]) {
						void *ptr = get_ptr(c, index[i]-1);
						read_component_in_field(L, obj_index, k->name, k->field_n, f, ptr);
					} else {
						lua_pushnil(L);
						lua_setfield(L, obj_index, k->name);
					}
				} else if (index[i] == 0 && !is_temporary(k->attrib)) {
					lua_pushnil(L);
					lua_setfield(L, obj_index, k->name);
				}
			}
		}
		f += k->field_n;
	}
}

static int
get_integer(lua_State *L, int index, int i, const char *key) {
	if (lua_rawgeti(L, index, i) != LUA_TNUMBER) {
		return luaL_error(L, "Can't find %s in iterator", key);
	}
	int r = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (r <= 0)
		return luaL_error(L, "Invalid %s (%d)", key, r);
	return r;
}

static int
lsync(lua_State *L) {
	struct group_iter *iter = luaL_checkudata(L, 2, "ENTITY_GROUPITER");
	luaL_checktype(L, 3, LUA_TTABLE);
	int idx = get_integer(L, 3, 1, "index") - 1;
	int mainkey = get_integer(L, 3, 2, "mainkey");
	unsigned int index[MAX_COMPONENT];
	int r = query_index(iter, 0, mainkey, idx, index);
	if (r <= 0) {
		if (r < 0) {
			return luaL_error(L, "Invalid iterator of mainkey (%d)", mainkey);
		} else {
			check_index(L, iter, mainkey, idx);	// raise error
			return 0;
		}
	}

	if (!iter->readonly) {
		update_iter(L, 1, 3, iter, idx, mainkey, 0);
	}
	read_iter(L, 1, 3, iter, index);
	return 0;
}

static int
lread(lua_State *L) {
	struct group_iter *iter = luaL_checkudata(L, 2, "ENTITY_GROUPITER");
	luaL_checktype(L, 3, LUA_TTABLE);
	int idx = get_integer(L, 3, 1, "index") - 1;
	int mainkey = get_integer(L, 3, 2, "mainkey");
	unsigned int index[MAX_COMPONENT];
	int r = query_index(iter, 0, mainkey, idx, index);
	if (r <= 0) {
		return 0;
	}

	if (!iter->readonly) {
		return luaL_error(L, "Pattern is not readonly");
	}
	read_iter(L, 1, 3, iter, index);
	return 1;
}

static int
postpone(lua_State *L, struct group_iter *iter, struct component_pool *c) {
	int ret = 0;
	if (c->stride == STRIDE_ORDER) {
		if (lua_getfield(L, 2, iter->k[0].name) == LUA_TBOOLEAN) {
			ret = (lua_toboolean(L, -1) == 0);
			lua_pushnil(L);
			lua_setfield(L, 2, iter->k[0].name);
		}
		lua_pop(L, 1);
	}
	return ret;
}

static int
leach_group(lua_State *L) {
	struct group_iter *iter = lua_touserdata(L, 1); 
	if (lua_rawgeti(L, 2, 1) != LUA_TNUMBER) {
		return luaL_error(L, "Invalid group iterator");
	}
	int i = lua_tointeger(L, -1);
	if (i < 0)
		return luaL_error(L, "Invalid iterator index %d", i);
	lua_pop(L, 1);

	if (lua_getiuservalue(L, 1, 1) != LUA_TUSERDATA) {
		return luaL_error(L, "Missing world object for iterator");
	}

	int world_index = lua_gettop(L);

	unsigned int index[MAX_COMPONENT];
	int mainkey = iter->k[0].id;

	struct component_pool *c = &iter->world->c[mainkey];
	if (i>0) {
		if (postpone(L, iter, c)) {
			--i;
			unsigned int tmp = c->id[i];
			memmove(&c->id[i], &c->id[i+1], (c->n-i-1) * sizeof(c->id[0]));
			c->id[c->n-1] = tmp;
		} else if (!iter->readonly) {
			update_last_index(L, world_index, 2, iter, i-1);
		}
	}
	for (;;) {
		int idx = i++;
		index[0] = idx + 1;
		int ret = query_index(iter, 1, mainkey, idx, index);
		if (ret < 0)
			return 0;
		if (ret > 0)
			break;
	}

	lua_pushinteger(L, i);
	lua_rawseti(L, 2, 1);

	read_iter(L, world_index, 2, iter, index);

	lua_settop(L, 2);
	return 1;
}

static void
create_key_cache(lua_State *L, struct group_key *k, struct field *f) {
	if (k->field_n == 0 // is tag or object?
		|| (k->attrib & COMPONENT_FILTER)) {	// existence or ref
		return;
	}
	if (k->field_n == 1 && f[0].key == NULL) {
		// value type
		switch (f[0].type) {
		case TYPE_INT:
		case TYPE_INT64:
		case TYPE_DWORD:
		case TYPE_WORD:
		case TYPE_BYTE:
			lua_pushinteger(L, 0);
			break;
		case TYPE_FLOAT:
		case TYPE_DOUBLE:
			lua_pushnumber(L, 0);
			break;
		case TYPE_BOOL:
			lua_pushboolean(L, 0);
			break;
		case TYPE_USERDATA:
			lua_pushlightuserdata(L, NULL);
			break;
		default:
			lua_pushnil(L);
			break;
		}
	} else {
		lua_createtable(L, 0, k->field_n);
	}
	lua_setfield(L, -2, k->name);
}

static int
lpairs_group(lua_State *L) {
	struct group_iter *iter = lua_touserdata(L, 1); 
	lua_pushcfunction(L, leach_group);
	lua_pushvalue(L, 1);
	lua_createtable(L, 2, iter->nkey);
	int i;
	int opt = 0;
	struct field *f = iter->f;
	for (i=0;i<iter->nkey;i++) {
		struct group_key *k = &iter->k[i];
		create_key_cache(L, k, f);
		f += k->field_n;
		if (k->attrib & COMPONENT_OPTIONAL)
			++opt;
	}
	if (opt) {
		// create backup table in metatable
		lua_createtable(L, 0, opt);
		for (i=0;i<iter->nkey;i++) {
			struct group_key *k = &iter->k[i];
			if (k->attrib & COMPONENT_OPTIONAL) {
				lua_getfield(L, -2, k->name);
				lua_setfield(L, -2, k->name);
			}
		}
		lua_setmetatable(L, -2);
	}
	lua_pushinteger(L, 0);
	lua_rawseti(L, -2, 1);
	lua_pushinteger(L, iter->k[0].id);	// mainkey
	lua_rawseti(L, -2, 2);
	return 3;		
}

static int
check_boolean(lua_State *L, const char * key) {
	int r = 0;
	switch (lua_getfield(L, -1, key)) {
	case LUA_TNIL:
		break;
	case LUA_TBOOLEAN:
		r = lua_toboolean(L, -1);
		break;
	default:
		return luaL_error(L, "Invalid boolean type %s", lua_typename(L, lua_type(L, -1)));
	}
	lua_pop(L, 1);
	return r;
}

static int
is_value(lua_State *L, struct field *f) {
	switch (lua_getfield(L, -1, "type")) {
	case LUA_TNIL:
		lua_pop(L, 1);
		return 0;
	case LUA_TNUMBER:
		f->key = NULL;
		f->offset = 0;
		f->type = check_type(L);
		return 1;
	default:
		return luaL_error(L, "Invalid value type %s", lua_typename(L, lua_type(L, -1)));
	}
}

static int
get_key(struct entity_world *w, lua_State *L, struct group_key *key, struct field *f) {
	if (lua_getfield(L, -1, "id") != LUA_TNUMBER) {
		return luaL_error(L, "Invalid id");
	}
	key->id = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (key->id < 0 || key->id >= MAX_COMPONENT || w->c[key->id].cap == 0) {
		return luaL_error(L, "Invalid id %d", key->id);
	}
	if (lua_getfield(L, -1, "name") != LUA_TSTRING) {
		return luaL_error(L, "Invalid component name");
	}
	key->name = lua_tostring(L, -1);
	lua_pop(L, 1);
	int attrib = 0;
	if (check_boolean(L, "r")) {
		attrib |= COMPONENT_IN;
	}
	if (check_boolean(L, "w")) {
		attrib |= COMPONENT_OUT;
	}
	if (check_boolean(L, "opt")) {
		attrib |= COMPONENT_OPTIONAL;
	}
	if (check_boolean(L, "exist")) {
		attrib |= COMPONENT_EXIST;
	}
	if (check_boolean(L, "absent")) {
		attrib |= COMPONENT_ABSENT;
	}
	key->attrib = attrib;
	if (is_value(L, f)) {
		key->field_n = 1;
		return 1;
	} else {
		int i = 0;
		int ttype;
		while ((ttype = lua_geti(L, -1, i+1)) != LUA_TNIL) {
			if (ttype != LUA_TTABLE) {
				return luaL_error(L, "Invalid field %d", i+1);
			}
			get_field(L, i+1, &f[i]);
			++i;
		}
		key->field_n = i;
		lua_pop(L, 1);
		return i;
	}
}

static int
lgroupiter(lua_State *L) {
	struct entity_world *w = getW(L);
	luaL_checktype(L, 2, LUA_TTABLE);
	int nkey = get_len(L, 2);
	int field_n = 0;
	int i;
	if (nkey == 0) {
		return luaL_error(L, "At least one key");
	}
	if (nkey > MAX_COMPONENT) {
		return luaL_error(L, "Too many keys");
	}
	for (i=0;i<nkey;i++) {
		if (lua_geti(L, 2, i+1) != LUA_TTABLE) {
			return luaL_error(L, "index %d is not a table", i);
		}
		int n = get_len(L, -1);
		if (n == 0) {
			struct field f;
			if (is_value(L, &f)) {
				n = 1;
			}
		}
		field_n += n;
		lua_pop(L, 1);
	}
	size_t header_size = sizeof(struct group_iter) + sizeof(struct group_key) * (nkey-1);
	const int align_size = sizeof(void *);
	// align
	header_size = (header_size + align_size - 1) & ~(align_size - 1);
	size_t size = header_size + field_n * sizeof(struct field);
	struct group_iter *iter = (struct group_iter *)lua_newuserdatauv(L, size, 1);
	// refer world
	lua_pushvalue(L, 1);
	lua_setiuservalue(L, -2, 1);
	iter->nkey = nkey;
	iter->world = w;
	iter->readonly = 1;
	struct field *f = (struct field *)((char *)iter + header_size);
	iter->f = f;
	for (i=0; i< nkey; i++) {
		lua_geti(L, 2, i+1);
		int n = get_key(w, L, &iter->k[i], f);
		struct component_pool *c = &w->c[iter->k[i].id];
		if (c->stride == STRIDE_TAG && is_temporary(iter->k[i].attrib)) {
			return luaL_error(L, "%s is a tag, use %s?out instead", iter->k[i].name, iter->k[i].name);
		}
		f += n;
		lua_pop(L, 1);
		if (c->stride == STRIDE_LUA) {
			if (n != 0)
				return luaL_error(L, ".%s is object component, no fields needed", iter->k[i].name);
			iter->k[i].attrib |= COMPONENT_OBJECT;
		} else if (c->stride == STRIDE_ORDER) {
			if (i != 0) {
				if (!is_temporary(iter->k[i].attrib)) {
					return luaL_error(L, ".%s is an order key, must be main key or temporary", iter->k[i].name);
				}
			} else if (!(iter->k[0].attrib & COMPONENT_EXIST)) {
				return luaL_error(L, ".%s is an order key, it can only be exist", iter->k[0].name);
			}
		}
		int attrib = iter->k[i].attrib;
		if (!(attrib & COMPONENT_FILTER)) {
			int readonly = (attrib & COMPONENT_IN) && !(attrib & COMPONENT_OUT);
			if (!readonly)
				iter->readonly = 0;
		}
	}
	int mainkey_attrib = iter->k[0].attrib;
	if (mainkey_attrib & COMPONENT_ABSENT) {
		return luaL_error(L, "The main key can't be absent");
	}
	if (luaL_newmetatable(L, "ENTITY_GROUPITER")) {
		lua_pushcfunction(L, lpairs_group);
		lua_setfield(L, -2, "__call");
	}
	lua_setmetatable(L, -2);
	return 1;
}

static int
lremove(lua_State *L) {
	struct entity_world *w = getW(L);
	luaL_checktype(L, 2, LUA_TTABLE);
	int iter = get_integer(L, 2, 1, "index") - 1;
	int mainkey = get_integer(L, 2, 2, "mainkey");
	entity_remove_(w, mainkey, iter, L, 1);
	return 0;
}

static int
lobject(lua_State *L) {
	struct group_iter *iter = luaL_checkudata(L, 1, "ENTITY_GROUPITER");
	int index = luaL_checkinteger(L, 3) - 1;
	int cid = iter->k[0].id;
	struct entity_world * w = iter->world;
	if (cid < 0 || cid >=MAX_COMPONENT) {
		return luaL_error(L, "Invalid object %d", cid);
	}
	lua_settop(L, 2);
	struct component_pool *c = &w->c[cid];
	if (c->n <= index) {
		return luaL_error(L, "No object %d", cid);
	}
	if (c->stride == STRIDE_LUA) {
		// lua object
		if (lua_getiuservalue(L, 1, 1) != LUA_TUSERDATA) {
			return luaL_error(L, "No world");
		}
		if (lua_getiuservalue(L, -1, cid * 2 + 2) != LUA_TTABLE) {
			return luaL_error(L, "Missing lua table for %d", cid);
		}
		if (lua_isnil(L, 2)) {
			lua_rawgeti(L, -1, index + 1);
		} else {
			lua_pushvalue(L, 2);
			lua_rawseti(L, -2, index + 1);
			lua_settop(L, 2);
		}
		return 1;
	} else if (c->stride == 0) {
		if (lua_type(L, 2) != LUA_TBOOLEAN)
			return luaL_error(L, "%s is a tag, need boolean", iter->k[0].name);
		if (!lua_toboolean(L, 2)) {
			entity_disable_tag_(w, cid, index, cid);
		}
		return 1;
	} else if (c->stride < 0) {
		return luaL_error(L, "Invalid object %d", cid);
	}
	struct field *f = iter->f;
	void * buffer = get_ptr(c, index);
	if (lua_isnoneornil(L, 2)) {
		// read object
		if (f->key == NULL) {
			// value type
			read_value(L, f, buffer);
		} else {
			lua_createtable(L, 0, iter->k[0].field_n);
			int lua_index = lua_gettop(L);
			read_component(L, iter->k[0].field_n, f, lua_index, buffer);
		}
	} else {
		// write object
		lua_pushvalue(L, 2);
		write_component_object(L, iter->k[0].field_n, f, buffer);
	}
	return 1;
}

static int
lrelease(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = check_cid(L, w, 2);
	int refid = luaL_checkinteger(L, 3) - 1;
	int dead_tag = cid + 1;
	entity_enable_tag_(w, cid, refid, dead_tag, L, 1);
	return 0;
}

static int
lreuse(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = check_cid(L, w, 2);
	int dead_tagid = cid + 1;
	struct component_pool *c = &w->c[dead_tagid];
	if (c->stride != STRIDE_TAG) {
		return luaL_error(L, "%d is not a tag", dead_tagid);
	}
	if (c->n == 0)
		return 0;
	int id = entity_sibling_index_(w, dead_tagid, 0, cid);
	if (id == 0)
		return luaL_error(L, "Invalid ref component %d", cid);
	entity_disable_tag_(w, dead_tagid, 0, dead_tagid);
	lua_pushinteger(L, id);
	return 1;
}

static int
find_boundary(int from, int to, unsigned int *a, unsigned int eid) {
	while(from < to) {
		int mid = (from + to)/2;
		unsigned int aa = a[mid];
		if (aa == eid)
			return mid;
		else if (aa < eid) {
			from = mid + 1;
		} else {
			to = mid;
		}
	}
	return to;
}

static inline int
next_removed_index(int removed_index, struct component_pool *removed, unsigned int *removed_eid) {
	for (;;) {
		++removed_index;
		if (removed_index >= removed->n) {
			*removed_eid = 0;
			break;
		}
		unsigned int last_eid = *removed_eid;
		*removed_eid = removed->id[removed_index];
		if (*removed_eid != last_eid)
			break;
	}
	return removed_index;
}

// remove reference object where id == 0
static void
remove_unused_reference(lua_State *L, struct component_pool *c, int from) {
	int i;
	int to = from;
	for (i=from; i < c->n; i++) {
		if (c->id[i]) {
			c->id[to] = c->id[i];
			lua_geti(L, -1, i+1);
			lua_seti(L, -2, to+1);
			++to;
		}
	}
	for (i=to;i<=c->n;i++) {
		lua_pushnil(L);
		lua_seti(L, -2, i+1);
	}
	c->n = to;
}

static int
lupdate_reference(lua_State *L) {
	struct entity_world *w = getW(L);
	struct component_pool *removed = &w->c[ENTITY_REMOVED];
	if (removed->n == 0)	// no removed entity
		return 0;
	int cid = check_cid(L, w, 2);
	struct component_pool *reference = &w->c[cid];
	if (reference->n == 0)
		return 0;	// no reference
	if (lua_getiuservalue(L, 1, cid * 2 + 2) != LUA_TTABLE) {
		return luaL_error(L, "Invalid reference component %d", cid);
	}
	int i;
	int removed_index = 0;
	unsigned int removed_eid = removed->id[removed_index];
	int index = find_boundary(0, reference->n, reference->id, removed_eid);
	int reference_index = index + 1;
	int removed_reference = 0;
	for (i=index; i< reference->n; i++) {
		int rtype = lua_geti(L, -1, i+1);
		if (rtype != LUA_TBOOLEAN && rtype != LUA_TTABLE) {
			// false means removed reference
			return luaL_error(L, "Invalid reference object");
		}
		if (removed_eid) {
			while (removed_eid < reference->id[i]) {
				removed_index = next_removed_index(removed_index, removed, &removed_eid);
			}
		}
		if (removed_eid == reference->id[i]) {
			// removed reference, clear reference id
			lua_pushnil(L);
			removed_index = next_removed_index(removed_index, removed, &removed_eid);
		} else {
			// update reference id
			lua_pushinteger(L, reference_index);
			++reference_index;
		}
		if (rtype == LUA_TBOOLEAN) {
			// set id = 0, so removed_reference() can remove them
			--reference_index;
			reference->id[i] = 0;
			if (removed_reference == 0) {
				removed_reference = i + 1;
			}
			lua_pop(L, 2);
		} else {
			lua_seti(L, -2, 1);
			lua_pop(L, 1);
		}
	}
	if (removed_reference) {
		remove_unused_reference(L, reference, removed_reference - 1);
	}
	return 0;
}

static int
ldumpid(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = check_cid(L, w, 2);
	struct component_pool *c = &w->c[cid];
	lua_createtable(L, c->n, 0);
	int i;
	for (i=0;i<c->n;i++) {
		lua_pushinteger(L, c->id[i]);
		lua_rawseti(L, -2, i+1);
	}
	return 1;
}

LUAMOD_API int
luaopen_ecs_core(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "_world", lnew_world },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);
	lua_pushinteger(L, MAX_COMPONENT-1);
	lua_setfield(L, -2, "_MAXTYPE");
	if (luaL_newmetatable(L, "ENTITY_WORLD")) {
		luaL_Reg l[] = {
			{ "__index", NULL },
			{ "memory", lcount_memory },
			{ "collect", lcollect_memory },
			{ "_newtype",lnew_type },
			{ "_newentity", lnew_entity },
			{ "_addcomponent", ladd_component },
			{ "_update", lupdate },
			{ "_clear", lclear_type },
			{ "_context", lcontext },
			{ "_groupiter", lgroupiter },
			{ "remove", lremove },
			{ "_object", lobject },
			{ "_sync", lsync },
			{ "_read", lread },
			{ "_release", lrelease },
			{ "_reuse", lreuse },
			{ "_update_reference", lupdate_reference },
			{ "_dumpid", ldumpid },
			{ NULL, NULL },
		};
		luaL_setfuncs(L,l,0);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
	} else {
		return luaL_error(L, "ENTITY_WORLD exist");
	}
	lua_setfield(L, -2, "_METHODS");
	lua_pushinteger(L, TYPE_INT);
	lua_setfield(L, -2, "_TYPEINT");
	lua_pushinteger(L, TYPE_FLOAT);
	lua_setfield(L, -2, "_TYPEFLOAT");
	lua_pushinteger(L, TYPE_BOOL);
	lua_setfield(L, -2, "_TYPEBOOL");
	lua_pushinteger(L, TYPE_INT64);
	lua_setfield(L, -2, "_TYPEINT64");
	lua_pushinteger(L, TYPE_DWORD);
	lua_setfield(L, -2, "_TYPEDWORD");
	lua_pushinteger(L, TYPE_WORD);
	lua_setfield(L, -2, "_TYPEWORD");
	lua_pushinteger(L, TYPE_BYTE);
	lua_setfield(L, -2, "_TYPEBYTE");
	lua_pushinteger(L, TYPE_DOUBLE);
	lua_setfield(L, -2, "_TYPEDOUBLE");
	lua_pushinteger(L, TYPE_USERDATA);
	lua_setfield(L, -2, "_TYPEUSERDATA");
	lua_pushinteger(L, STRIDE_LUA);
	lua_setfield(L, -2, "_LUAOBJECT");
	lua_pushinteger(L, ENTITY_REMOVED);
	lua_setfield(L, -2, "_REMOVED");
	lua_pushinteger(L, STRIDE_ORDER);
	lua_setfield(L, -2, "_ORDERKEY");
	lua_pushlightuserdata(L, NULL);
	lua_setfield(L, -2, "NULL");

	return 1;
}

#ifdef TEST_LUAECS

#include <stdio.h>

#define COMPONENT_VECTOR2 1
#define TAG_MARK 2
#define COMPONENT_ID 3
#define SINGLETON_STRING 4

struct vector2 {
	float x;
	float y;
};

struct id {
	int v;
};

static int
ltest(lua_State *L) {
	struct ecs_context *ctx = lua_touserdata(L, 1);
	struct vector2 *v;
	int i;
	for (i=0;(v=(struct vector2 *)entity_iter(ctx, COMPONENT_VECTOR2, i));i++) {
		printf("vector2 %d: x=%f y=%f\n", i, v->x, v->y);
		struct id * id = (struct id *)entity_sibling(ctx, COMPONENT_VECTOR2, i, COMPONENT_ID);
		if (id) {
			printf("\tid = %d\n", id->v);
		}
		void * mark = entity_sibling(ctx, COMPONENT_VECTOR2, i, TAG_MARK);
		if (mark) {
			printf("\tMARK\n");
		}
	}

	return 0;
}

static int
lsum(lua_State *L) {
	struct ecs_context *ctx = lua_touserdata(L, 1);
	struct vector2 *v;
	int i;
	float s = 0;
	for (i=0;(v=(struct vector2 *)entity_iter(ctx, COMPONENT_VECTOR2, i));i++) {
		s += v->x + v->y;
	}
	lua_pushnumber(L, s);
	return 1;
}

static int
lget(lua_State *L) {
	struct ecs_context *ctx = lua_touserdata(L, 1);
	const char *ret = (const char *)entity_ref_object(ctx, SINGLETON_STRING, 1);
	if (ret) {
		lua_pushfstring(L, "[string:%s]", ret);
	}
	return 1;
}

struct userdata_t {
	unsigned char a;
	void *b;
};

static int
ltestuserdata(lua_State *L) {
	struct ecs_context *ctx = lua_touserdata(L, 1);
	struct userdata_t *ud = entity_iter(ctx, 1, 0);
	ud->a = 1 - ud->a;
	ud->b = ctx;
	return 0;
}

LUAMOD_API int
luaopen_ecs_ctest(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "test", ltest },
		{ "sum", lsum },
		{ "get", lget },
		{ "testuserdata", ltestuserdata },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

#endif
