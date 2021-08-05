local ecs = require "ecs.core"

local function get_attrib(opt, inout)
	if opt == nil then
		return { exist = true }
	end
	local desc = {}
	if opt == "?" then
		desc.opt = true
	else
		assert(opt == ":")
	end
	if inout == "in" then
		desc.r = true
	elseif inout == "out" then
		desc.w = true
	elseif inout == "update" then
		desc.r = true
		desc.w = true
	elseif inout == "exist" then
		desc.exist = true
		assert(not desc.opt)
	elseif inout == "absent" then
		desc.absent = true
		assert(not desc.opt)
	else
		assert(inout == "temp")
	end
	return desc
end

local function cache_world(obj, k)
	local c = {
		typenames = {},
		id = 0,
		select = {},
		ref = {},
	}

	local function gen_ref_pat(key)
		local typenames = c.typenames
		local desc = {}
		local tc = typenames[key]
		if tc == nil then
			error("Unknown type " .. key)
		end
		local a = {
			exist = true,
			name = tc.name,
			id = tc.id,
			type = tc.type,
		}
		local n = #tc
		for i=1,#tc do
			a[i] = tc[i]
		end
		desc[1] = a
		return desc
	end

	local function gen_all_pat()
		local desc = {}
		local i = 1
		local _ORDERKEY = ecs._ORDERKEY
		for name,t in pairs(c.typenames) do
			if t.size ~= _ORDERKEY then
				local a = {
					name = t.name,
					id = t.id,
					type = t.type,
					opt = true,
					r = true,
				}
				table.move(t, 1, #t, 1, a)
				desc[i] = a
				i = i + 1
			end
		end
		return desc
	end

	setmetatable(c, { __index = function(_, key)
		if key == "all" then
			local all = k:_groupiter(gen_all_pat())
			c.all = all
			return all
		end
	end })

	local function gen_select_pat(pat)
		local typenames = c.typenames
		local desc = {}
		local idx = 1
		for token in pat:gmatch "[^ ]+" do
			local key, index, padding = token:match "^([_%w]+)%(?([_%w]*)%)?(.*)"
			assert(key, "Invalid pattern")
			local opt, inout
			if padding ~= "" then
				opt, inout = padding:match "^([:?])(%l+)$"
				assert(opt, "Invalid pattern")
			end
			local tc = typenames[key]
			if tc == nil then
				error("Unknown type " .. key)
			end
			if index ~= "" then
				local indexc = typenames[index]
				if indexc == nil then
					error("Unknown index type "..index)
				end
				local a = get_attrib(opt, inout == "temp" and "temp" or "in")
				a.name = index
				a.id = indexc.id
				a.type = indexc.type
				a.ref = true
				desc[idx] = a
				idx = idx + 1
			end
			local a = get_attrib(opt, inout)
			a.name = tc.name
			a.id = tc.id
			a.type = tc.type
			local n = #tc
			for i=1,#tc do
				a[i] = tc[i]
			end
			desc[idx] = a
			idx = idx + 1
			if tc.ref and index == "" then
				local dead = typenames[key .. "_dead"]
				local a = {
					absent = true,
					name = dead.name,
					id = dead.id,
				}
				desc[idx] = a
				idx = idx + 1
			end
		end
		return desc
	end

	local function cache_select(cache, pat)
		local pat_desc = gen_select_pat(pat)
		cache[pat] = k:_groupiter(pat_desc)
		return cache[pat]
	end

	setmetatable(c.select, {
		__mode = "kv",
		__index = cache_select,
		})

	local function cache_ref(cache, pat)
		local pat_desc = gen_ref_pat(pat)
		cache[pat] = k:_groupiter(pat_desc)
		return cache[pat]
	end

	setmetatable(c.ref, {
		__mode = "kv",
		__index = cache_ref,
		})

	obj[k] = c
	return c
end

local context = setmetatable({}, { __index = cache_world })
local typeid = {
	int = assert(ecs._TYPEINT),
	float = assert(ecs._TYPEFLOAT),
	bool = assert(ecs._TYPEBOOL),
	int64 = assert(ecs._TYPEINT64),
	dword = assert(ecs._TYPEDWORD),
	word = assert(ecs._TYPEWORD),
	byte = assert(ecs._TYPEBYTE),
	double = assert(ecs._TYPEDOUBLE),
	userdata = assert(ecs._TYPEUSERDATA),
}
local typesize = {
	[typeid.int] = 4,
	[typeid.float] = 4,
	[typeid.bool] = 1,
	[typeid.int64] = 8,
	[typeid.dword] = 4,
	[typeid.word] = 2,
	[typeid.byte] = 1,
	[typeid.double] = 8,
	[typeid.userdata] = 8,
}

local M = ecs._METHODS

do	-- newtype
	local function parse(s)
		-- s is "name:typename"
		local name, typename = s:match "^([%w_]+):(%w+)$"
		local typeid = assert(typeid[typename])
		return { typeid, name }
	end

	local function align(c, field)
		local t = field[1]
		local tsize = typesize[t]
		local offset = ((c.size + tsize - 1) & ~(tsize-1))
		c.size = offset + tsize
		field[3] = offset
		return field
	end

	local function align_struct(c, t)
		if t then
			local s = typesize[t] - 1
			c.size = ((c.size + s) & ~s)
		end
	end

	function M:register(typeclass)
		local name = assert(typeclass.name)
		local ctx = context[self]
		ctx.all = nil	-- clear all pattern
		local typenames = ctx.typenames
		local id = ctx.id + 1
		assert(typenames[name] == nil and id <= ecs._MAXTYPE)
		ctx.id = id
		local c = {
			id = id,
			name = name,
			size = 0,
		}
		for i, v in ipairs(typeclass) do
			c[i] = align(c, parse(v))
		end
		local ttype = typeclass.type
		if ttype == "lua" then
			assert(c.size == 0)
			c.size = ecs._LUAOBJECT
			assert(c[1] == nil)
		elseif c.size > 0 then
			align_struct(c, typeclass[1][1])
		else
			-- size == 0, one value
			if ttype then
				local t = assert(typeid[typeclass.type])
				c.type = t
				c.size = typesize[t]
				c[1] = { t, "v", 0 }
			else
				c.tag = true
			end
		end
		typenames[name] = c
		self:_newtype(id, c.size)
		if typeclass.ref then
			c.ref = true
			self:register { name = name .. "_dead" }
		end
	end
end

local function dump(obj)
	for e,v in pairs(obj) do
		if type(v) == "table" then
			for k,v in pairs(v) do
				print(e,k,v)
			end
		else
			print(e,v)
		end
	end
end

function M:new(obj)
--	dump(obj)
	local eid = self:_newentity()
	local typenames = context[self].typenames
	for k,v in pairs(obj) do
		local tc = typenames[k]
		if not tc then
			error ("Invalid key : ".. k)
		end
		local id = self:_addcomponent(eid, tc.id)
		self:object(k, id, v)
	end
end

function M:ref(name, refobj)
	local obj = assert(refobj[name])
	local ctx = context[self]
	local typenames = ctx.typenames
	local tc = assert(typenames[name])
	local refid = self:_reuse(tc.id)
	refobj[2] = tc.id
	if refid then
		local p = context[self].select[name .. ":out"]
		refobj[1] = refid
		self:_sync(p, refobj)
	else
		local eid = self:_newentity()
		refid = self:_addcomponent(eid, tc.id)
		refobj[1] = refid
		self:object(name, refid, obj)
	end
	for k,v in pairs(refobj) do
		if (v == true or v == false) and name ~= k then
			local p = context[self].select[k .. "?out"]
			self:_sync(p, refobj)
		end
	end
	return refid
end

function M:object_ref(name, refid)
	local typenames = context[self].typenames
	return { refid, typenames[name].id }
end

function M:release(name, refid)
	local id = assert(context[self].typenames[name].id)
	self:_release(id, refid)
end

function M:context(t)
	local typenames = context[self].typenames
	local id = {}
	for i, name in ipairs(t) do
		local tc = typenames[name]
		if not tc then
			error ("Invalid component name " .. name)
		end
		id[i] = tc.id
	end
	return self:_context(id)
end

function M:select(pat)
	return context[self].select[pat]()
end

function M:sync(pat, iter)
	local p = context[self].select[pat]
	self:_sync(p, iter)
	return iter
end

function M:readall(iter)
	local p = context[self].all
	self:_sync(p, iter)
	return iter
end

function M:clear(name)
	local id = assert(context[self].typenames[name].id)
	self:_clear(id)
end

local function gen_sorted_id(self, sorted, name)
	local ctx = context[self]
	local typenames = ctx.typenames
	local t = assert(typenames[name])
	local stype = typenames[sorted]
	if stype == nil then
		local id = ctx.id + 1
		assert(id <= ecs._MAXTYPE)
		ctx.id = id
		stype = {
			id = id,
			name = sorted,
			size = ecs._ORDERKEY,
			tag = true
		}
		self:_newtype(id, stype.size)
		typenames[sorted] = stype
	else
		assert(stype.size == ecs._ORDERKEY)
	end
	return stype.id, t.id
end

function M:sort(sorted, name)
	self:_sortkey(gen_sorted_id(self, sorted, name))
end

function M:order(sorted, refname, order_array)
	local sid, rid = gen_sorted_id(self, sorted, refname)
	self:_orderkey(sid, rid, order_array)
end

function M:bsearch(sorted, name, value)
	local typenames = context[self].typenames
	local sorted_id = typenames[sorted].id
	local value_id = typenames[name].id
	return self:_bsearch(sorted_id, value_id, value)
end

function M:fetch(idtype, id)
	local c = context[self].typenames[idtype]
	local f = c.fetch
	if not f then
		f = setmetatable({}, { __mode = "kv" })
		c.fetch = f
	end
	local cid = c.id
	local iter = f[id]
	local ret = self:_fetch(cid, id, iter)
	if not iter then
		f[id] = ret
	end
	return ret
end

do
	local _object = M._object
	function M:object(name, refid, v)
		local pat = context[self].ref[name]
		return _object(pat, v, refid)
	end

	function M:singleton(name, v)
		local pat = context[self].ref[name]
		return _object(pat, v, 1)
	end
end

function ecs.world()
	local w = ecs._world()
	context[w].typenames.REMOVED = {
		name = "REMOVED",
		id = ecs._REMOVED,
		size = 0,
		tag = true,
	}
	return w
end

return ecs
