-- test object
local ecs = require "ecs"

local w = ecs.world()

w:register {
	name = "refobject",
	type = "int",
	ref = true,
}

local id1 = w:ref("refobject", { refobject = 42 })
local id2 = w:ref("refobject", { refobject = 0 })
local id3 = w:ref("refobject", { refobject = 100 })
print ("New", id1, id2, id3)
print(w:object("refobject", id1))

print("Release", id1)

w:release("refobject", id1)

for v in w:select "refobject:in" do
	print(v.refobject)
end

local id4 = w:ref("refobject", { refobject = -42 })
print ("New", id4)

print ("Release", id2)

w:release("refobject", id2)

print ("Release", id3)

w:release("refobject", id3)

print "List refobject"

for v in w:select "refobject:in" do
	print(v.refobject)
end

w:register {
	name = "index",
	type = "int",
}

w:new {
	index = id4
}

for v in w:select "index:in" do
	print(v.index)
end

