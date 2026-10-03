-- Exercise the real movement coroutine and scheduler, with deterministic engine inputs.
local fixture = dofile(ROOT .. "/Tests/AIEngineFixtures.lua");
AEJetpack = {Standard=0, JumpPack=1};
SceneMan.GlobalAcc = Vector(0, 20);
local failures = 0;
local function check(name, test)
	local ok, error = pcall(test);
	print((ok and "PASS: " or "FAIL: ") .. name .. (ok and "" or ": " .. tostring(error)));
	if not ok then failures = failures + 1; end
end
local function traveler()
	local owner = fixture.owner();
	owner.AIMode = Actor.AIMODE_GOTO;
	owner.Mass, owner.RotAngle, owner.ID, owner.IgnoresWhichTeam = 100, 0, 1, -1;
	owner.FGFoot = {Pos=Vector(495,520)};
	owner.BGFoot = {Pos=Vector(505,520)};
	owner.Jetpack = {JetTimeTotal=2000, JetTimeLeft=2000, BurstSpacing=100, JetpackType=AEJetpack.Standard, MinimumFuelRatio=0, JetAngleRange=0.16, ThrottleFactor=1, TotalBurstSize=8, CanAdjustAngleWhileFiring=true};
	function owner.Jetpack:EstimateImpulse(burst) return burst and 20 or 2; end
	function owner.Jetpack:CanTriggerBurst() return false; end
	function owner.Jetpack:IsEmitting() return false; end
	function owner:SendMessage() end
	function owner:SetAimAngle(angle) self.aimAngle = angle; end
	function owner:GetAimAngle() return self.aimAngle or 0; end
	owner.wpt, owner.waypointCount, owner.MovePathSize = Vector(900,500), 1, 1;
	owner.MovePath = function() return owner.wpt; end
	local ai = NativeHumanAI:Create(owner);
	ai.Ctrl, ai.lastAIMode = owner.ctrl, owner.AIMode;
	ai.jetImpulseFactor, ai.jetBurstFactor = 2400, 0;
	ai.GoToBehavior = coroutine.create(function() while true do coroutine.yield(); end end);
	ai.GoToName = "GoToWpt";
	return owner, ai;
end
check("low jetpack fuel does not stop flat-ground travel", function()
	local owner, ai = traveler();
	ai.refuel = true;
	owner.Jetpack.JetTimeLeft = 1000;
	local co = coroutine.create(SharedBehaviors.GoToWpt);
	for _ = 1, 3 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
	assert(ai.lateralMoveState == Actor.LAT_RIGHT, "actor remains motionless while recharging halfway-full pack");
end);
check("a falling actor can leave refuel mode with usable fuel", function()
	local owner, ai = traveler();
	ai.refuel, ai.flying = true, true;
	owner.Vel = Vector(0,5);
	owner.Jetpack.JetTimeLeft = 1000;
	fixture.resume(coroutine.create(SharedBehaviors.GoToWpt), ai, owner);
	assert(not ai.refuel, "descending actor remains locked in refuel mode");
end);
check("leaving the ground updates air steering within 300 ms", function()
	local owner, ai = traveler();
	for _ = 1, 9 do fixture.advance(33.4); ai:Update(owner); end
	assert(ai.flying, "air steering still thinks both feet are grounded");
	assert(#ConsoleMan.errors == 0, table.concat(ConsoleMan.errors, "\n"));
end);
check("an unobstructed travel order starts running immediately", function()
	local owner, ai = traveler();
	ai.lateralMoveState = Actor.LAT_RIGHT;
	ai:Update(owner);
	assert(owner.ctrl.states[Controller.MOVE_FAST], "travel starts at walking speed");
end);
check("jet steering matches nozzle range, facing, and strafing", function()
	local owner, ai = traveler();
	for _, range in ipairs({0.14, 0.16, 0.3}) do
		owner.Jetpack.JetAngleRange = range;
		for _, aim in ipairs({-0.7, 0, 1.4}) do
			for _, flipped in ipairs({false, true}) do
				for _, move in ipairs({Actor.LAT_STILL, Actor.LAT_LEFT, Actor.LAT_RIGHT}) do
					local strafe = (flipped and move == Actor.LAT_RIGHT) or (not flipped and move == Actor.LAT_LEFT);
					local angle = ((aim > 0 and aim * range or -aim * range * 0.5) - math.pi * 0.5 * range) * (strafe and -1 or 1) - math.pi * 0.5;
					local expected = Vector(-math.cos(angle) * (flipped and -1 or 1), math.sin(angle));
					local actual = SharedBehaviors.GetJetDirection(owner, aim, flipped, move);
					assert((actual - expected).Magnitude < 0.00001, "prediction differs from the engine's jet nozzle");
				end
			end
		end
	end
end);
check("burst prediction scales with actor mass and uses one time horizon", function()
	local owner, ai = traveler();
	ai.flying = true;
	owner.Jetpack.JetpackType = AEJetpack.JumpPack;
	owner.Vel = Vector(5,0);
	function owner.Jetpack:CanTriggerBurst() return true; end
	local position, falling, time = SharedBehaviors.PredictJetPosition(ai, owner, owner.Pos, 0, false, Actor.LAT_STILL);
	assert(math.abs(time - 0.4) < 0.00001);
	assert(math.abs(falling.X - (owner.Pos.X + 5 * GetPPM() * time)) < 0.00001, "velocity and acceleration use different horizons");
	local thrustDisplacement = position - falling;
	owner.Mass = owner.Mass * 2;
	local heavier, heavierFall = SharedBehaviors.PredictJetPosition(ai, owner, owner.Pos, 0, false, Actor.LAT_STILL);
	assert(math.abs((heavier - heavierFall).Magnitude * 2 - thrustDisplacement.Magnitude) < 0.00001, "burst displacement ignores actor mass");
	owner.Jetpack.JetTimeLeft = 200;
	position, falling, time = SharedBehaviors.PredictJetPosition(ai, owner, owner.Pos, 0, false, Actor.LAT_STILL);
	assert(time < 0.1 and math.abs(falling.X - (owner.Pos.X + 5 * GetPPM() * time)) < 0.00001, "burst fuel cost leaves an inconsistent velocity forecast");
end);
check("one-foot actors react to flight as quickly as two-foot actors", function()
	local owner, ai = traveler();
	owner.BGFoot = nil;
	for _ = 1, 9 do fixture.advance(33.4); ai:Update(owner); end
	assert(ai.flying, "missing leg slows down air steering");
end);
check("brief ground gaps are debounced and landing restores ground steering", function()
	local owner, ai = traveler();
	fixture.advance(100); ai:Update(owner);
	assert(not ai.flying, "one empty ground check triggers flight");
	fixture.advance(100); ai:Update(owner);
	assert(ai.flying);
	local originalMatter = SceneMan.GetTerrMatter;
	SceneMan.GetTerrMatter = function() return 10; end;
	fixture.advance(100); ai:Update(owner);
	SceneMan.GetTerrMatter = originalMatter;
	assert(not ai.flying, "landing leaves the air controller active");
end);
check("recharging allows travel but suppresses premature ground takeoff", function()
	local owner, ai = traveler();
	ai.refuel = true;
	owner.Jetpack.JetTimeLeft = 1000;
	owner.wpt = Vector(900,350);
	local co = coroutine.create(SharedBehaviors.GoToWpt);
	for _ = 1, 3 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
	assert(ai.refuel and not ai.jump and ai.lateralMoveState == Actor.LAT_RIGHT, "recharge either freezes travel or starts a weak takeoff");
end);
check("air travel brakes before overshooting a nearby destination", function()
	local owner, ai = traveler();
	owner.Vel = Vector(10,0);
	owner.wpt = Vector(560,450);
	ai.flying, ai.jump, ai.jumpState = true, true, AHuman.UPJUMP;
	ai.jetImpulseFactor = 100000;
	local co = coroutine.create(SharedBehaviors.GoToWpt);
	for _ = 1, 2 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
	assert(ai.jump and ai.lateralMoveState == Actor.LAT_LEFT, "air controller accelerates past its destination");
end);
check("running strides near terrain do not activate air steering", function()
	local owner, ai = traveler();
	local originalRay = SceneMan.CastStrengthRay;
	SceneMan.CastStrengthRay = function(_, _, ray) return ray.X == 0 and ray.Y > 0; end;
	for _ = 1, 9 do fixture.advance(33.4); ai:Update(owner); end
	SceneMan.CastStrengthRay = originalRay;
	assert(not ai.flying, "a running stride is treated as sustained flight");
end);
check("combat slows travel immediately and clearing combat restores running", function()
	local owner, ai = traveler();
	ai.Target = {ID=10,Pos=Vector(700,500)};
	ai:Update(owner);
	assert(not owner.ctrl.states[Controller.MOVE_FAST], "an acquired target leaves sprinting active");
	ai.Target = nil;
	ai:Update(owner);
	assert(owner.ctrl.states[Controller.MOVE_FAST], "cleared combat leaves a random walking delay");
end);
check("jump packs keep their minimum fuel requirement while walking to a route", function()
	local owner, ai = traveler();
	owner.Jetpack.JetpackType = AEJetpack.JumpPack;
	owner.Jetpack.MinimumFuelRatio = 1;
	owner.Jetpack.JetTimeLeft = 1900;
	ai.refuel = true;
	local co = coroutine.create(SharedBehaviors.GoToWpt);
	for _ = 1, 3 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
	assert(ai.refuel and not ai.jump and ai.lateralMoveState == Actor.LAT_RIGHT, "jump pack ignores required charge or prevents walking");
end);
check("airborne standard packs resume thrust without another fuel-heavy burst", function()
	local owner, ai = traveler();
	ai.flying, ai.jump = true, true;
	ai:Update(owner);
	assert(owner.ctrl.states[Controller.BODY_JUMP] and not owner.ctrl.states[Controller.BODY_JUMPSTART], "air steering starts another costly burst");
end);
check("excavating miners retain lift bursts while opening a shaft", function()
	local owner, ai = traveler();
	ai.flying, ai.jump, ai.deviceState = true, true, AHuman.DIGGING;
	ai:Update(owner);
	assert(owner.ctrl.states[Controller.BODY_JUMPSTART], "excavation loses its lift burst");
end);
check("digging tools do not impose a walking delay on an advancing miner", function()
	local owner, ai = traveler();
	ai.deviceState, ai.lateralMoveState = AHuman.DIGGING, Actor.LAT_RIGHT;
	ai:Update(owner);
	assert(owner.ctrl.states[Controller.MOVE_FAST], "an advancing miner is forced to walk despite a clear movement order");
end);
check("fixed nozzles retain their emitting angle during a maneuver", function()
	local owner, ai = traveler();
	owner.Jetpack.CanAdjustAngleWhileFiring = false;
	owner.Jetpack.EmitAngle = -math.pi * 0.5;
	function owner.Jetpack:IsEmitting() return true; end
	owner.RotAngle = 0.2;
	local direction = SharedBehaviors.GetJetDirection(owner,1.4,true,Actor.LAT_RIGHT);
	local expected = Vector(0,-1):RadRotate(owner.RotAngle);
	assert((direction - expected).Magnitude < 0.00001, "prediction moves a nozzle that cannot turn while emitting");
end);
check("a grounded destination within normal body clearance does not trigger takeoff", function()
	local owner, ai = traveler();
	owner.wpt = Vector(540,488);
	ai.jetImpulseFactor = 100000;
	local co = coroutine.create(SharedBehaviors.GoToWpt);
	for _ = 1, 2 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
	assert(not ai.jump, "walking to a slightly elevated destination launches the actor");
end);
check("an actor ascending above a lower destination releases lift", function()
	local owner, ai = traveler();
	owner.wpt = Vector(480,550);
	owner.Vel = Vector(8,-5);
	ai.flying, ai.jump, ai.jumpState = true, true, AHuman.UPJUMP;
	ai.jetImpulseFactor = 100000;
	local co = coroutine.create(SharedBehaviors.GoToWpt);
	for _ = 1, 2 do fixture.advance(33.4); fixture.resume(co, ai, owner); end
	assert(not ai.jump, "horizontal braking keeps lifting away from a lower destination");
end);
check("crab movement uses the same sustained air-thrust policy", function()
	ACrab = {NOTJUMPING=0, PREJUMP=2, UPJUMP=3, STILL=0, AIMING=1, POINTING=2};
	require("AI/NativeCrabAI");
	local owner = traveler();
	local ai = NativeCrabAI:Create(owner);
	ai.Ctrl, ai.lastAIMode = owner.ctrl, owner.AIMode;
	ai.Behavior = coroutine.create(function() while true do coroutine.yield(); end end);
	ai.BehaviorName = "GoToWpt";
	ai.flying, ai.jump = true, true;
	ai:Update(owner);
	assert(owner.ctrl.states[Controller.BODY_JUMP] and not owner.ctrl.states[Controller.BODY_JUMPSTART], "crab air steering disagrees with the shared flight model");
end);
assert(failures == 0, tostring(failures) .. " locomotion regressions");
