#define LUA_LIB

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "luaecs.h"

#define MAX_COMPONENT 256
#define ENTITY_REMOVED 0
#define DEFAULT_SIZE 128
#define DUMMY_PTR (void *)(uintptr_t)(~0)

struct component_pool {
	int cap;
	int n;
	int count;
	int stride;
	int last_lookup;
	unsigned int *id;
	void *buffer;
};

struct entity_world {
	unsigned int max_id;
	unsigned int wrap_begin;
	int wrap;
	struct component_pool c[MAX_COMPONENT];
};

static void
init_component_pool(struct entity_world *w, int index, int stride, int opt_size) {
	struct component_pool *c = &w->c[index];
	c->cap = opt_size;
	c->n = 0;
	c->count = 0;
	c->stride = stride;
	c->id = NULL;
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
		}
		if (c->buffer) {
			sz += c->cap * c->stride;
		}

		msz += c->n * (sizeof(unsigned int) + c->stride);
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
	} if (c->n < c->cap) {
		c->cap = c->n;
		c->id = (unsigned int *)lua_newuserdatauv(L, c->n * sizeof(unsigned int), 0);
		lua_setiuservalue(L, 1, id * 2 + 1);
		if (c->stride > 0) {
			c->buffer = lua_newuserdatauv(L, c->n * c->stride, 0);
			lua_setiuservalue(L, 1, id * 2 + 2);
		}
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

static void *
add_component_(lua_State *L, struct entity_world *w, int cid, unsigned int eid, const void *buffer) {
	struct component_pool *pool = &w->c[cid];
	int cap = pool->cap;
	int index = pool->n;
	int stride = pool->stride;
	if (pool->n == 0) {
		if (pool->id == NULL) {
			pool->id = (unsigned int *)lua_newuserdatauv(L, cap * sizeof(unsigned int), 0);
			lua_setiuservalue(L, 1, cid * 2 + 1);
		}
		if (pool->buffer == NULL) {
			pool->buffer = lua_newuserdatauv(L, cap *stride, 0);
			lua_setiuservalue(L, 1, cid * 2 + 2);
		}
	} else {
		if (pool->n >= pool->cap) {
			// expand pool
			int newcap = cap * 3 / 2;
			unsigned int *newid = (unsigned int *)lua_newuserdatauv(L, newcap * sizeof(unsigned int), 0);
			lua_setiuservalue(L, 1, cid * 2 + 1);
			memcpy(newid, pool->id,  cap * sizeof(unsigned int));
			pool->id = newid;
			if (stride > 0) {
				void *newbuffer = lua_newuserdatauv(L, newcap * stride, 0);
				lua_setiuservalue(L, 1, cid * 2 + 2);
				memcpy(newbuffer, pool->buffer, cap * stride);
				pool->buffer = newbuffer;
			}
			pool->cap = newcap;
		}
	}
	++pool->n;
	pool->id[index] = eid;
	char * ret = (char *)pool->buffer + index * stride;
	if (buffer) {
		memcpy(ret, buffer, stride);
	}
	return (void *)ret;
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
	size_t sz;
	const char *buffer = lua_tolstring(L, 4, &sz);
	int stride = w->c[cid].stride;
	if ((buffer == NULL && stride > 0) || (sz != stride)) {
		return luaL_error(L, "Invalid data (size=%d/%d) for type %d", (int)sz, stride, cid);
	}
	add_component_(L, w, cid, eid, buffer);
	return 0;
}

static int
lnew_entity(lua_State *L) {
	struct entity_world *w = getW(L);
	unsigned int eid = ++w->max_id;
	if (eid == 0) {
		assert(w->wrap == 0);
		w->wrap = 1;
	}
	lua_pushinteger(L, eid);
	return 1;
}


static void
remove_entity_(struct entity_world *w, int cid, int index, void *L) {
	struct component_pool *c = &w->c[cid];
	if (index < 0 || index >= c->count)
		luaL_error((lua_State *)L, "Invalid index %d/%d", index, c->count);
	unsigned int eid = c->id[index];
	add_component_((lua_State *)L, w, ENTITY_REMOVED, eid, NULL);
}

static int
lremove_entity(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = check_cid(L, w, 2);
	int index = luaL_checkinteger(L, 3);
	remove_entity_(w, cid, index, (void *)L);
	return 0;
}

static int
id_comp(const void *a , const void *b) {
    const unsigned int ai = *( const unsigned int* )a;
    const unsigned int bi = *( const unsigned int* )b;

    if( ai < bi )
        return -1;
    else if( ai > bi )
        return 1;
    else
        return 0;
}

static int
lsort_removed(lua_State *L) {
	struct entity_world *w = getW(L);
	struct component_pool *pool = &w->c[ENTITY_REMOVED];
	if (pool->n == 0)
		return 0;
	qsort(pool->id, pool->n, sizeof(unsigned int), id_comp);
	pool->count = pool->n;
	lua_pushinteger(L, pool->n);
	return 1;
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
	int n = pool->count;
	if (n == 0)
		return -1;
	if (guess_index + GUESS_RANGE >= n)
		return binary_search(pool->id, 0, pool->count, eid);
	unsigned int *a = pool->id;
	int higher = a[guess_index + GUESS_RANGE];
	if (eid > higher) {
		return binary_search(a, guess_index + GUESS_RANGE + 1, pool->count, eid);
	}
	int lower = a[guess_index];
	if (eid < lower) {
		return binary_search(a, 0, guess_index, eid);
	}
	return binary_search(a, guess_index, guess_index + GUESS_RANGE, eid);
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
		if (index < w->c[i].count) {
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
//		printf("arrange %d -> %d\n", new_id, w->c[cid].id[index]);
		w->c[cid].id[index] = new_id;
		if (current_id != last_id) {
			++new_id;
			last_id = current_id;
		}
		++ctx.ptr[cid-1];
	}
	int i,j;
	for (i=1;i<MAX_COMPONENT;i++) {
		struct component_pool *pool = &w->c[i];
		for (j=pool->count;j<pool->n;j++) {
//			printf("arrange new %d -> %d\n", pool->id[j], new_id + pool->id[j] - w->wrap_begin -1);
			pool->id[j] = new_id + pool->id[j] - w->wrap_begin -1;
		}
	}
	w->max_id = new_id + w->max_id - w->wrap_begin - 1;
}

static inline void
move_item(struct component_pool *pool, int from, int to) {
	if (from != to) {
		pool->id[to] = pool->id[from];
		memcpy((char *)pool->buffer + to * pool->stride, (char *)pool->buffer + from * pool->stride, pool->stride);
	}
}

static void
remove_all(struct component_pool *pool, struct component_pool *removed) {
	int index = 0;
	int i;
	unsigned int *id = removed->id;
	unsigned int last_id = 0;
	int count = 0;
	for (i=0;i<removed->n;i++) {
		if (id[i] != last_id) {
			int r = lookup_component(pool, id[i], index);
			if (r >= 0) {
				index = r;
				assert(pool->id[r] == id[i]);
				pool->id[r] = 0;
				++count;
			}
		}
	}
	if (count > 0) {
		index = 0;
		for (i=0;i<pool->n;i++) {
			if (pool->id[i] != 0) {
				move_item(pool, i, index);
				++index;
			}
		}
		pool->n -= count;
		pool->count -= count;
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
				remove_all(pool, removed);
		}
		removed->n = 0;
		removed->count = 0;
	}

	if (w->wrap) {
		rearrange(w);
		w->wrap = 0;
	}
	w->wrap_begin = w->max_id;
	// add componets
	for (i=1;i<MAX_COMPONENT;i++) {
		struct component_pool *c = &w->c[i];
		c->count = c->n;
	}
	return 0;
}

static void *
entity_iter_(struct entity_world *w, int cid, int index) {
	struct component_pool *c = &w->c[cid];
	assert(index >= 0);
	if (index >= c->count)
		return NULL;
	return (char *)c->buffer + c->stride * index;
}

static void
entity_clear_type_(struct entity_world *w, int cid) {
	struct component_pool *c = &w->c[cid];
	c->n = 0;
	c->count = 0;
}

static int
lclear_type(lua_State *L) {
	struct entity_world *w = getW(L);
	int cid = check_cid(L,w, 2);
	entity_clear_type_(w, cid);
	return 0;
}

static void *
entity_sibling_(struct entity_world *w, int cid, int index, int slibling_id) {
	struct component_pool *c = &w->c[cid];
	if (index < 0 || index >= c->count)
		return NULL;
	unsigned int eid = c->id[index];
	c = &w->c[slibling_id];
	int result_index = lookup_component(c, eid, c->last_lookup);
	if (result_index >= 0) {
		c->last_lookup = result_index;
		return (char *)c->buffer + c->stride * result_index;
	}
	return NULL;
}

static void *
entity_add_sibling_(struct entity_world *w, int cid, int index, int slibling_id, const void *buffer, void *L) {
	struct component_pool *c = &w->c[cid];
	assert(index >=0 && index < c->count);
	unsigned int eid = c->id[index];
	// todo: pcall add_component_
	void * ret = add_component_((lua_State *)L, w, slibling_id, eid, buffer);
	c->count = c->n;
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
	lua_setiuservalue(L, -2, 1);
	ctx->max_id = n;
	ctx->world = w;
	static struct ecs_capi c_api = {
		entity_iter_,
		entity_clear_type_,
		entity_sibling_,
		entity_add_sibling_,
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

struct field {
	const char *key;
	int offset;
	int type;
};

struct simple_iter {
	int id;
	int field_n;
	struct entity_world *world;
	struct field f[1];
};

static int
check_type(lua_State *L) {
	int type = lua_tointeger(L, -1);
	if (type != TYPE_INT &&
		type != TYPE_FLOAT &&
		type != TYPE_BOOL) {
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
leach_simple(lua_State *L) {
	struct simple_iter *iter = lua_touserdata(L, 1); 
	if (lua_rawgeti(L, 2, 1) != LUA_TNUMBER) {
		return luaL_error(L, "Invalid simple iterator");
	}
	int i = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (i > 0) {
		void * write_buffer = entity_iter_(iter->world, iter->id, i-1);
		if (write_buffer == NULL)
			return luaL_error(L, "Can't write to index %d", i);
		write_component(L, iter->field_n, iter->f, 2, (char *)write_buffer);
	}
	void * read_buffer = entity_iter_(iter->world, iter->id, i++);
	if (read_buffer == NULL) {
		return 0;
	}
	lua_pushinteger(L, i);
	lua_rawseti(L, 2, 1);
	read_component(L, iter->field_n, iter->f,  2, (const char *)read_buffer);
	lua_settop(L, 2);
	return 1;
}

static int
lpairs_simple(lua_State *L) {
	struct simple_iter *iter = lua_touserdata(L, 1); 
	lua_pushcfunction(L, leach_simple);
	lua_pushvalue(L, 1);
	lua_createtable(L, 1, iter->field_n);
	lua_pushinteger(L, 0);
	lua_rawseti(L, -2, 1);
	return 3;		
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

static int
lsimpleiter(lua_State *L) {
	struct entity_world *w = getW(L);
	luaL_checktype(L, 2, LUA_TTABLE);
	int n = get_len(L, 2);
	if (n == 0) {
		return luaL_error(L, "At least 1 field");
	}
	size_t sz = sizeof(struct simple_iter) + (n-1) * sizeof(struct field);
	struct simple_iter * iter = (struct simple_iter *)lua_newuserdatauv(L, sz, 1);
	// refer world
	lua_pushvalue(L, 1);
	lua_setiuservalue(L, -2, 1);
	iter->world = w;
	iter->field_n = n;
	if (lua_getfield(L, 2, "id") != LUA_TNUMBER) {
		return luaL_error(L, "Invalid id");
	}
	iter->id = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (iter->id < 0 || iter->id >= MAX_COMPONENT || iter->id == ENTITY_REMOVED || w->c[iter->id].cap == 0) {
		return luaL_error(L, "Invalid id %d", iter->id);
	}
	int i;
	for (i=0;i<n;i++) {
		if (lua_geti(L, 2, i+1) != LUA_TTABLE) {
			luaL_error(L, "Invalid field %d", i);
		}
		get_field(L, i+1, &iter->f[i]);
	}
	if (luaL_newmetatable(L, "ENTITY_SIMPLEITER")) {
		lua_pushcfunction(L, lpairs_simple);
		lua_setfield(L, -2, "__call");
	}
	lua_setmetatable(L, -2);
	return 1;
}

#define COMPONENT_IN 1
#define COMPONENT_OUT 2
#define COMPONENT_OPTIONAL 4

struct group_key {
	const char *name;
	int id;
	int field_n;
	int attrib;
};

static inline int
is_temporary(int attrib) {
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
get_write_component(lua_State *L, int lua_index, const char *name, int n, struct field *f) {
	if (n == 0) {
		// todo : support change tag
		return luaL_error(L, ".%s is a readonly tag", name);
	}
	switch (lua_getfield(L, lua_index, name)) {
	case LUA_TNIL:
		lua_pop(L, 1);
		// restore cache
		lua_getmetatable(L, lua_index);
		lua_getfield(L, -1, name);
		lua_setfield(L, lua_index, name);
		lua_pop(L, 1);	// pop metatable
		return 0;
	case LUA_TTABLE:
		return 1;
	default:
		if (f->key == NULL) {
			// value type
			return 1;
		}
		return luaL_error(L, "Invalid iterator type %s", lua_typename(L, lua_type(L, -1)));
	}
}

static void
write_component_in_field(lua_State *L, int lua_index, const char *name, int n, struct field *f, void *buffer) {
	if (f->key == NULL) {
		write_value(L, f, buffer);
	} else {
		write_component(L, n, f, -1, (char *)buffer);
		lua_pop(L, 1);
	}
}

static void
update_last_index(lua_State *L, int lua_index, struct group_iter *iter, int idx) {
	int i;
	int mainkey = iter->k[0].id;
	if ((iter->k[0].attrib & COMPONENT_OUT)
		&& get_write_component(L, lua_index, iter->k[0].name, iter->k[0].field_n, iter->f)) {
		void * buffer = entity_iter_(iter->world, mainkey, idx);
		if (buffer == NULL) {
			luaL_error(L, "Can't find component %s for index %d", iter->k[0].name, idx);
		}
		write_component_in_field(L, lua_index, iter->k[0].name, iter->k[0].field_n, iter->f, buffer);
	}

	struct field *f = iter->f + iter->k[0].field_n;

	for (i=1;i<iter->nkey;i++) {
		struct group_key *k = &iter->k[i];
		if ((k->attrib & COMPONENT_OUT)
			&& get_write_component(L, lua_index, k->name, k->field_n, f)) {
			void * buffer = entity_sibling_(iter->world, mainkey, idx, k->id);
			if (buffer == NULL) {
				luaL_error(L, "Can't find sibling %s of %s", k->name, iter->k[0].name);
			}
			write_component_in_field(L, lua_index, k->name, k->field_n, f, buffer);
		} else if (is_temporary(k->attrib)
			&& get_write_component(L, lua_index, k->name, k->field_n, f)) {
			void *buffer = entity_add_sibling_(iter->world, mainkey, idx, k->id, NULL, L);
			write_component_in_field(L, lua_index, k->name, k->field_n, f, buffer);
		}
		f += k->field_n;
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
		luaL_error(L, ".%s is missing", name);
	}
	read_component(L, n , f, lua_gettop(L), buffer);
	lua_pop(L, 1);
}

static int
leach_group(lua_State *L) {
	struct group_iter *iter = lua_touserdata(L, 1); 
	if (lua_rawgeti(L, 2, 1) != LUA_TNUMBER) {
		return luaL_error(L, "Invalid group iterator");
	}
	int i = lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (i > 0 && !iter->readonly) {
		update_last_index(L, 2, iter, i-1);
	}
	int mainkey = iter->k[0].id;
	void * buffer[MAX_COMPONENT];
	int j;
	do {
		buffer[0] = entity_iter_(iter->world, mainkey, i);
		if (buffer[0] == NULL) {
			return 0;
		}
		for (j=1;j<iter->nkey;j++) {
			struct group_key *k = &iter->k[j];
			if (!is_temporary(k->attrib)) {
				buffer[j] = entity_sibling_(iter->world, mainkey, i, k->id);
				if (buffer[j] == NULL) {
					if (!(k->attrib & COMPONENT_OPTIONAL)) {
						// required. try next
						break;
					}
				}
			} else {
				buffer[j] = NULL;
			}
		}
		++i;
	} while (j < iter->nkey);

	lua_pushinteger(L, i);
	lua_rawseti(L, 2, 1);

	struct field *f = iter->f;

	for (i=0;i<iter->nkey;i++) {
		struct group_key *k = &iter->k[i];
		if (k->attrib & COMPONENT_IN) {
			if (buffer[i]) {
				read_component_in_field(L, 2, k->name, k->field_n, f, buffer[i]);
			} else {
				lua_pushnil(L);
				lua_setfield(L, 2, k->name);
			}
		} else if (buffer[i] == NULL && !is_temporary(k->attrib)) {
			lua_pushnil(L);
			lua_setfield(L, 2, k->name);
		}
		f += k->field_n;
	}
	lua_settop(L, 2);
	return 1;
}

static void
create_key_cache(lua_State *L, struct group_key *k, struct field *f) {
	if (k->field_n == 0) {	// is tag?
		return;
	}
	if (k->field_n == 1 && f[0].key == NULL) {
		// value type
		switch (f[0].type) {
		case TYPE_INT:
			lua_pushinteger(L, 0);
			break;
		case TYPE_FLOAT:
			lua_pushnumber(L, 0);
			break;
		case TYPE_BOOL:
			lua_pushboolean(L, 0);
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
	lua_createtable(L, 1, iter->nkey);
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
	if (key->id < 0 || key->id >= MAX_COMPONENT || key->id == ENTITY_REMOVED || w->c[key->id].cap == 0) {
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
		return luaL_error(L, "Too mant keys");
	}
	for (i=0;i<nkey;i++) {
		if (lua_geti(L, 2, i+1) != LUA_TTABLE) {
			return luaL_error(L, "index %d is not a table", i);
		}
		field_n += get_len(L, -1);
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
		f += n;
		lua_pop(L, 1);
		int attrib = iter->k[i].attrib;
		int readonly = (attrib & COMPONENT_IN) && !(attrib & COMPONENT_OUT);
		if (!readonly)
			iter->readonly = 0;
	}
	if (iter->k[0].attrib & COMPONENT_OPTIONAL) {
		return luaL_error(L, "The first key should not be optional");
	}
	if (!(iter->k[0].attrib & COMPONENT_IN) && !(iter->k[0].attrib & COMPONENT_OUT)) {
		return luaL_error(L, "The main key can't be temporary");
	}
	if (luaL_newmetatable(L, "ENTITY_GROUPITER")) {
		lua_pushcfunction(L, lpairs_group);
		lua_setfield(L, -2, "__call");
	}
	lua_setmetatable(L, -2);
	return 1;
}

LUAMOD_API int
luaopen_ecs_core(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "world", lnew_world },
		{ "_MAXTYPE", NULL },
		{ "_METHODS", NULL },
		{ "_TYPEINT", NULL },
		{ "_TYPEFLOAT", NULL },
		{ "_TYPEBOOL", NULL },
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
			{ "remove", lremove_entity },
			{ "sort_removed", lsort_removed },
			{ "update", lupdate },
			{ "clear", lclear_type },
			{ "_context", lcontext },
			{ "_simpleiter", lsimpleiter },
			{ "_groupiter", lgroupiter },
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

	return 1;
}

#ifdef TEST_LUAECS

#include <stdio.h>

#define COMPONENT_VECTOR2 1
#define TAG_MARK 2
#define COMPONENT_ID 3

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


LUAMOD_API int
luaopen_ecs_ctest(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "test", ltest },
		{ "sum", lsum },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

#endif
