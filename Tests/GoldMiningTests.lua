-- Source/scheduler regression tests. Physics are verified separately by GoldMining.rte.
local fixture = dofile(ROOT .. "/Tests/AIEngineFixtures.lua");
local function miner()
	local owner = fixture.owner();
	owner.UniqueID = 42;
	local ai = NativeHumanAI:Create(owner);
	ai.Ctrl = owner.ctrl;
	return owner, ai;
end
local claimed, releases, target = 0, 0, Vector(-1, -1);
function SceneMan:AcquireGoldMiningTarget() claimed = claimed + 1; return target; end
function SceneMan:GetGoldMiningTargetGoldCount() return target.X < 0 and 0 or 1; end
function SceneMan:ReleaseGoldMiningTarget(_, delay) releases = releases + 1; self.retryDelay = delay; end
function SceneMan:CastNotMaterialRay() return -1; end

local owner, ai = miner();
local co = coroutine.create(SharedBehaviors.GoToWpt);
fixture.resume(co, ai, owner);
assert(ai.NextBehaviorName ~= "Sentry", "arrived miner became a sentry");
for _ = 1, 5 do ai:Update(owner); end
assert(ai.BehaviorName == "GoldDig", "scheduler failed to restart mining");
assert(#ConsoleMan.errors == 0, table.concat(ConsoleMan.errors, "\n"));
print("PASS: arrived miner remains a miner through the actual scheduler");
ai.Behavior = nil;
ai.GoToBehavior = coroutine.create(function() while true do coroutine.yield(); end end);
ai:Update(owner);
assert(ai.NextBehaviorName == "GoldDig", "interrupted miner stayed in movement with no mining job");
print("PASS: interrupted mining restarts even while a movement coroutine remains");

owner, ai = miner();
co = coroutine.create(HumanBehaviors.GoldDig);
for _ = 1, 70 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
assert(coroutine.status(co) == "suspended" and claimed > 1, "empty scene stopped prospecting");
target = Vector(950, 900);
for _ = 1, 17 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
assert(owner.wpt.X == 950 and owner.wpt.Y == 900, "distant deep vein did not receive a route");
print("PASS: miners keep prospecting and route to distant, deep gold");

local previousReleases = releases;
for _ = 1, 270 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
assert(releases > previousReleases and SceneMan.retryDelay == 15000, "stalled route was never retried");
fixture.resume(co, ai, owner);
GoldMining.Cleanup(ai, owner);
assert(not ai.fire and ai.MiningTarget == nil, "cancelled job kept firing or holding a target");
print("PASS: stalled routes release targets and cancellation clears mining state");

target = Vector(502, 500);
owner, ai = miner();
co = coroutine.create(HumanBehaviors.GoldDig);
fixture.resume(co, ai, owner);
assert(ai.fire and ai.lateralMoveState == Actor.LAT_LEFT, "gold inside barrel reach was never approached from another position");
target = Vector(950, 900);
function SceneMan:CastStrengthRay(_, _, strength) return strength == 1; end
owner, ai = miner();
co = coroutine.create(HumanBehaviors.GoldDig);
fixture.resume(co, ai, owner);
assert(ai.fire and ai.NextGoTo == nil, "breakable terrain did not receive direct excavation");
function SceneMan:CastNotMaterialRay(_, ray) return ray.X > 0 and 5 or -1; end
owner, ai = miner();
co = coroutine.create(HumanBehaviors.GoldDig);
fixture.resume(co, ai, owner);
assert(ai.fire and ai.lateralMoveState == Actor.LAT_LEFT, "wall inside barrel reach left the tunnelling miner stuck");
target = Vector(500, 300);
owner, ai = miner();
co = coroutine.create(HumanBehaviors.GoldDig);
fixture.resume(co, ai, owner);
assert(ai.NextGoTo ~= nil, "high targets bypassed jetpack navigation");
function SceneMan:CastNotMaterialRay() return -1; end
function SceneMan:CastStrengthRay() return false; end
print("PASS: direct excavation and repositioning for close gold remnants");

local device = {ClassName="HDFirearm",UniqueID=123,Pos=Vector(550,500),Vel=Vector(),
	IsPickupableBy=function()return true end,IsActivated=function()return false end,
	HasObjectInGroup=function()return true end,IsTool=function()return true end};
function MovableMan:GetMOsInRadius()
	local done = false;
	return function() if not done then done = true; return device; end end;
end
local function search(behavior, valid, droppedTool, complete)
	local checks, dispatched = 0, 0;
	function MovableMan:ValidMO(m) checks = checks + 1; return m and (valid or checks == 1); end
	device.HasObjectInGroup = function(_, group) return not droppedTool; end;
	SceneMan.Scene = {CalculatePathAsync=function(_, callback)
		dispatched = dispatched + 1;
		if complete then callback({Status=PathRequest.NoSolution,PathLength=0}); end
	end};
	local owner, ai = miner();
	local co = coroutine.create(behavior);
	for _ = 1, 200 do
		fixture.advance(33.4);
		if coroutine.status(co) == "dead" then break; end
		fixture.resume(co, ai, owner);
	end
	assert(coroutine.status(co) == "dead", "equipment search never completed");
	return dispatched;
end
assert(search(HumanBehaviors.ToolSearch, false, false, false) == 0);
assert(search(HumanBehaviors.WeaponSearch, true, true, false) == 0);
assert(search(HumanBehaviors.ToolSearch, true, false, false) == 1);
assert(search(HumanBehaviors.ToolSearch, true, false, true) == 1);
print("PASS: vanished/skipped devices and missing/immediate callbacks cannot trap searches");

function MovableMan:ValidMO(m) return m ~= nil; end
function MovableMan:FindObjectByUniqueID() return device; end
device.IsDevice = function() return true; end;
SceneMan.Scene.CalculatePathAsync = function(_, callback) callback({Status=0,PathLength=1}); end;
owner, ai = miner();
function owner:AddAIMOWaypoint(p) self.wpt = p.Pos; end
function owner:UpdateMovePath() self.IsWaitingOnNewMovePath = true; end
co = coroutine.create(HumanBehaviors.ToolSearch);
for _ = 1, 200 do
	fixture.advance(33.4);
	if coroutine.status(co) == "dead" then break; end
	fixture.resume(co, ai, owner);
end
assert(coroutine.status(co) == "dead" and ai.PickupHD == nil, "pickup route wait never timed out");
print("PASS: a pickup route that never returns releases the search");
