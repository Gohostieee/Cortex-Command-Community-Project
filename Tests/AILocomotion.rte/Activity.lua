function AILocomotionRegression:Write(line)
	local file = LuaMan:FileOpen("build-AILocomotionTests.rte/runtime.log", "a");
	if file >= 0 then LuaMan:FileWriteLine(file, line .. "\n"); LuaMan:FileClose(file); end
end

function AILocomotionRegression:StartActivity()
	local config = dofile("build-AILocomotionTests.rte/Config.lua");
	assert(SceneMan.SceneWidth == 1600 and SceneMan.SceneHeight == 1200, "Wrong locomotion terrain was loaded");
	local file = LuaMan:FileOpen("build-AILocomotionTests.rte/runtime.log", "w");
	if file >= 0 then LuaMan:FileClose(file); end
	self.timer, self.lastLog = Timer(), -1000;
	TimerMan.TimeScale = 2;
	self.ActivityState = Activity.RUNNING;
	self.cases = {
		{name="flat", preset="Soldier Light", start=Vector(100,200), target=Vector(1450,220)},
		{name="ledge-light", preset="Soldier Light", start=Vector(100,630), target=Vector(1450,470)},
		{name="ledge-heavy", preset="Soldier Heavy", start=Vector(100,1050), target=Vector(1450,890)}
	};
	for _, case in ipairs(self.cases) do
		if config.mirrored then
			case.start.X = SceneMan.SceneWidth - 1 - case.start.X;
			case.target.X = SceneMan.SceneWidth - 1 - case.target.X;
		end
		local actor = CreateAHuman(case.preset, "Coalition.rte");
		actor.Team, actor.Pos, actor.PlayerControllable = 0, case.start, false;
		actor.HFlipped = config.mirrored;
		actor:AddInventoryItem(CreateHDFirearm("Assault Rifle", "Coalition.rte"));
		actor.AIMode = Actor.AIMODE_GOTO;
		actor:AddAISceneWaypoint(case.target);
		actor:AddScript("build-AILocomotionTests.rte/Trace.lua");
		MovableMan:AddActor(actor);
		case.actor = actor;
	end
	self:Write("START locomotion flat=1350px ledges=180px mirrored=" .. tostring(config.mirrored));
end

function AILocomotionRegression:UpdateActivity()
	local elapsed = self.timer.ElapsedSimTimeMS;
	if self.finished then
		if elapsed > self.finished + 500 then os.exit(self.success and 0 or 1); end
		return;
	end
	if elapsed - self.lastLog < 250 then return; end
	self.lastLog = elapsed;
	local complete = true;
	for _, case in ipairs(self.cases) do
		local actor = case.actor;
		if MovableMan:ValidMO(actor) then
			local dist = SceneMan:ShortestDistance(actor.Pos, case.target, false);
			if actor.Pos.X < 0 or actor.Pos.X >= SceneMan.SceneWidth or actor.Pos.Y < 0 or actor.Pos.Y >= SceneMan.SceneHeight or actor.Health < 95 then
				self:Write("FAIL: " .. case.name .. " left the scene or suffered movement damage");
				self.finished, self.success = elapsed, false;
				ConsoleMan:SaveAllText("build-mp/locomotion-native-console.log");
				return;
			end
			if not case.arrived and math.abs(dist.X) < 50 and math.abs(dist.Y) < 65 and actor:GetWaypointListSize() == 0 and actor.MovePathSize == 0 and not actor.IsWaitingOnNewMovePath and actor.Vel.Magnitude < 3 then
				case.arrived = elapsed / 1000;
				self:Write(string.format("ARRIVED %s t=%.2f health=%.1f", case.name, case.arrived, actor.Health));
			end
			if case.arrived and (math.abs(dist.X) > 75 or math.abs(dist.Y) > 80) then
				self:Write("FAIL: " .. case.name .. " did not remain at its destination");
				self.finished, self.success = elapsed, false;
				ConsoleMan:SaveAllText("build-mp/locomotion-native-console.log");
				return;
			end
			self:Write(string.format("t=%.2f case=%s pos=%.1f,%.1f vel=%.1f,%.1f fuel=%.0f health=%.1f %s", elapsed / 1000, case.name, actor.Pos.X, actor.Pos.Y, actor.Vel.X, actor.Vel.Y, actor.Jetpack and actor.Jetpack.JetTimeLeft or 0, actor.Health, actor:StringValueExists("LocomotionRegressionState") and actor:GetStringValue("LocomotionRegressionState") or "trace unavailable"));
		else
			self:Write("FAIL: " .. case.name .. " is no longer alive");
			self.finished, self.success = elapsed, false;
			ConsoleMan:SaveAllText("build-mp/locomotion-native-console.log");
			return;
		end
		complete = complete and case.arrived ~= nil;
	end
	if complete and not self.allArrivedAt then self.allArrivedAt = elapsed; end
	local stable = complete and elapsed - self.allArrivedAt >= 1500;
	if stable or elapsed >= 45000 then
		self:Write(stable and "PASS: all movement orders completed" or "FAIL: movement order exceeded 45 seconds");
		ConsoleMan:SaveAllText("build-mp/locomotion-native-console.log");
		FrameMan:SaveScreenToPNG("locomotion-native");
		self.finished, self.success = elapsed, stable;
	end
end
