-- Appended to an isolated copy of OneManArmy.lua for AWS load tests.
-- The released activity and native server executable remain unchanged.
local benchmarkOriginalUpdate = OneManArmy.UpdateActivity;
function OneManArmy:UpdateActivity()
    benchmarkOriginalUpdate(self);
    local configured = os.getenv("CCCP_MPBENCH_ACTORS");
    if not configured or self.ActivityState ~= Activity.RUNNING or self:IsPaused() then return end;
    local target = tonumber(configured) or 0;
    if not self.benchmarkClock then
        self.benchmarkClock = Timer();
        self.benchmarkLastSample = 0;
        self.benchmarkLastBurst = 0;
        self.benchmarkExplosions = 0;
        self.benchmarkSpawned = 0;
        self.benchmarkFrames = 0;
        self.benchmarkLog = assert(io.open(os.getenv("CCCP_USERDATA_PATH") .. "/load-benchmark.csv", "w"));
        self.benchmarkLog:write("real_ms,sim_ms,simulation_updates,actors,stress_alive,stress_spawned,stress_firing,stress_damaged,particles,moids,explosions\n");
    end;
    self.benchmarkFrames = self.benchmarkFrames + 1;
    local alive, firing, damaged = 0, 0, 0;
    for actor in MovableMan.Actors do
        if actor:NumberValueExists("MultiplayerBenchmarkActor") then
            alive = alive + 1;
            if actor:GetController():IsState(Controller.WEAPON_FIRE) then firing = firing + 1 end;
            if actor.Health < 99 then damaged = damaged + 1 end;
        end;
    end;
    local focus = self:GetControlledActor(1) or self:GetControlledActor(0);
    if focus then
        for i = 1, math.min(4, target - alive) do
            local soldier = CreateAHuman("Soldier Light", "Coalition.rte");
            soldier:AddInventoryItem(CreateHDFirearm("Assault Rifle", "Coalition.rte"));
            soldier.Team = (self.benchmarkSpawned % 2);
            soldier.AIMode = Actor.AIMODE_BRAINHUNT;
            local side = soldier.Team == 0 and -1 or 1;
            soldier.Pos = SceneMan:MovePointToGround(focus.Pos + Vector(side * (200 + (self.benchmarkSpawned % 20) * 14), -300), 30, 2);
            soldier.HFlipped = side > 0;
            soldier:SetNumberValue("MultiplayerBenchmarkActor", 1);
            MovableMan:AddActor(soldier);
            self.benchmarkSpawned = self.benchmarkSpawned + 1;
        end;
        local burst = tonumber(os.getenv("CCCP_MPBENCH_BURST")) or 0;
        if burst > 0 and self.benchmarkClock.ElapsedRealTimeMS - self.benchmarkLastBurst >= 3000 then
            self.benchmarkLastBurst = self.benchmarkClock.ElapsedRealTimeMS;
            for i = 1, burst do
                local grenade = CreateTDExplosive("Frag Grenade", "Base.rte");
                grenade.Pos = SceneMan:MovePointToGround(focus.Pos + Vector(700 + i * 12, -400), 100, 2);
                grenade:GibThis();
                self.benchmarkExplosions = self.benchmarkExplosions + 1;
            end;
            local craft = CreateACDropShip("Dropship MK1", "Base.rte");
            craft.Pos = focus.Pos + Vector(750, -180);
            craft:GibThis();
            self.benchmarkExplosions = self.benchmarkExplosions + 1;
        end;
    end;
    if self.benchmarkClock.ElapsedRealTimeMS - self.benchmarkLastSample >= 1000 then
        self.benchmarkLastSample = self.benchmarkClock.ElapsedRealTimeMS;
        local actors = 0;
        for actor in MovableMan.Actors do actors = actors + 1 end;
        self.benchmarkLog:write(string.format("%.0f,%.0f,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", self.benchmarkClock.ElapsedRealTimeMS, self.benchmarkClock.ElapsedSimTimeMS, self.benchmarkFrames, actors, alive, self.benchmarkSpawned, firing, damaged, MovableMan:GetParticleCount(), MovableMan:GetMOIDCount(), self.benchmarkExplosions));
        self.benchmarkLog:flush();
    end;
end;
