function GoldMiningRegression:Write(line)
	local file = LuaMan:FileOpen("build-GoldMiningTests.rte/runtime.log", "a");
	if file >= 0 then LuaMan:FileWriteLine(file, line .. "\n"); LuaMan:FileClose(file); end
end

function GoldMiningRegression:GoldCount()
	local count = 0;
	for y = 0, SceneMan.SceneHeight - 1 do
		for x = 0, SceneMan.SceneWidth - 1 do
			if SceneMan:GetTerrMatter(x, y) == 2 then count = count + 1; end
		end
	end
	return count;
end

function GoldMiningRegression:StartActivity()
	assert(SceneMan.SceneWidth == 960 and SceneMan.SceneHeight == 540, "Wrong regression terrain was loaded");
	local file = LuaMan:FileOpen("build-GoldMiningTests.rte/runtime.log", "w");
	if file >= 0 then LuaMan:FileClose(file); end
	self.timer = Timer();
	TimerMan.TimeScale = 2;
	self.lastLog = -2000;
	self.miners = {};
	self.ActivityState = Activity.RUNNING;
	for i, x in ipairs({60, 350, 780}) do
		local miner = CreateAHuman("Soldier Light", "Coalition.rte");
		miner.Team = 0;
		miner.Pos = Vector(x, 100);
		miner.PlayerControllable = false;
		miner:AddInventoryItem(CreateHDFirearm(i == 1 and "Light Digger" or "Heavy Digger", "Base.rte"));
		miner.AIMode = Actor.AIMODE_GOLDDIG;
		miner:AddScript("build-GoldMiningTests.rte/Trace.lua");
		MovableMan:AddActor(miner);
		table.insert(self.miners, miner);
	end
	self.initialGold = self:GoldCount();
	assert(self.initialGold == 1371, "Unexpected terrain gold in the regression fixture");
	self:Write("START gold=" .. self.initialGold .. " width=" .. SceneMan.SceneWidth .. " height=" .. SceneMan.SceneHeight);
end

function GoldMiningRegression:UpdateActivity()
	local elapsed = self.timer.ElapsedSimTimeMS;
	if self.finished then
		if elapsed > self.finished + 1500 then os.exit(self.success and 0 or 1); end
		return;
	end
	if elapsed - self.lastLog >= 2000 then
		self.lastLog = elapsed;
		local count = self:GoldCount();
		self:Write(string.format("t=%.2f remaining=%d funds=%.2f", elapsed / 1000, count, self:GetTeamFunds(0)));
		for i, miner in ipairs(self.miners) do
			if MovableMan:ValidMO(miner) then
				self:Write(string.format("miner=%d pos=%.1f,%.1f mode=%d health=%.1f wp=%d path=%d waiting=%s item=%s %s", i, miner.Pos.X, miner.Pos.Y, miner.AIMode, miner.Health, miner:GetWaypointListSize(), miner.MovePathSize, tostring(miner.IsWaitingOnNewMovePath), miner.EquippedItem and miner.EquippedItem.PresetName or "none", miner:StringValueExists("MiningRegressionState") and miner:GetStringValue("MiningRegressionState") or "trace unavailable"));
			else
				self:Write("miner=" .. i .. " no longer alive");
			end
		end
		ConsoleMan:SaveAllText("build-mp/mining-native-console.log");
		if count == 0 or elapsed >= 600000 then
			self:Write(count == 0 and "PASS: all terrain gold mined" or "FAIL: terrain gold remains");
			FrameMan:SaveScreenToPNG("mining-native");
			self.finished, self.success = elapsed, count == 0;
		end
	end
end
