GoldMining = {};

local function StopTravel(AI, Owner)
	if AI.GoToBehavior then
		coroutine.resume(AI.GoToBehavior, AI, Owner, true);
	end
	if AI.GoToCleanup then AI.GoToCleanup(AI); end
	AI.GoToBehavior, AI.GoToCleanup, AI.GoToName = nil, nil, nil;
	AI.NextGoTo, AI.NextGoToCleanup, AI.NextGoToName = nil, nil, nil;
	Owner:ClearAIWaypoints();
	AI.lateralMoveState = Actor.LAT_STILL;
	AI.jump = false;
end

function GoldMining.Cleanup(AI, Owner)
	SceneMan:ReleaseGoldMiningTarget(Owner.UniqueID, 0);
	StopTravel(AI, Owner);
	AI.fire = false;
	AI.deviceState = AHuman.STILL;
	AI.MiningTarget = nil;
end

-- Keep the job alive while travelling, excavating, and waiting for new terrain gold.
function GoldMining.Dig(AI, Owner, Abort)
	local surveyTimer, progressTimer = Timer(), Timer();
	local target, progressPos, progressDistance, remaining;
	local resurvey = true;
	local phase = 0;
	local travelTarget;
	local excavate = false;
	while Owner.AIMode == Actor.AIMODE_GOLDDIG do
		if not Owner:EquipDiggingTool(false) then return true; end

		if resurvey or surveyTimer:IsPastSimMS(500) then
			surveyTimer:Reset();
			local found = SceneMan:AcquireGoldMiningTarget(Owner.Pos, Owner.UniqueID, Owner.Team);
			local count = SceneMan:GetGoldMiningTargetGoldCount(Owner.UniqueID);
			if found.X < 0 then
				if target then StopTravel(AI, Owner); end
				target, AI.MiningTarget, travelTarget = nil, nil, nil;
				AI.fire = false;
				AI.deviceState = AHuman.STILL;
				AI.lateralMoveState = Actor.LAT_STILL;
			else
				local distance = SceneMan:ShortestDistance(Owner.Pos, found, true).Magnitude;
				if not target or resurvey or (remaining and count < remaining) or distance < progressDistance - 6 or
					SceneMan:ShortestDistance(progressPos, Owner.Pos, true):MagnitudeIsGreaterThan(Owner.Height) then
					progressTimer:Reset();
					progressPos = Vector(Owner.Pos.X, Owner.Pos.Y);
					progressDistance = distance;
				end
				target, AI.MiningTarget, remaining = found, found, count;
				local heading = SceneMan:ShortestDistance(Owner.Pos, target, true);
				-- Excavate a direct tunnel through terrain our tool can remove. Use pathfinding
				-- to navigate open terrain and go around obstacles stronger than the digger.
				excavate = heading.Y >= -Owner.Height * 0.5 and SceneMan:CastStrengthRay(Owner.Pos, heading, 1, Vector(), 3, rte.doorID, true) and
					not SceneMan:CastStrengthRay(Owner.Pos, heading, Owner.DigStrength + 1, Vector(), 3, rte.doorID, true);
			end
			resurvey = false;
		end

		if target then
			local delta = SceneMan:ShortestDistance(Owner.Pos, target, true);
			if progressTimer:IsPastSimMS(8000) then
				-- Try another vein now; retry this patch after terrain or tools may have changed.
				SceneMan:ReleaseGoldMiningTarget(Owner.UniqueID, 15000);
				StopTravel(AI, Owner);
				target, AI.MiningTarget, travelTarget = nil, nil, nil;
				AI.fire = false;
				resurvey = true;
			elseif excavate or delta:MagnitudeIsLessThan(math.max(28, Owner.Height * 0.45)) then
				-- General pathing considers nearby waypoints reached. Finish the gold itself here.
				if AI.GoToBehavior or AI.NextGoTo or travelTarget then StopTravel(AI, Owner); travelTarget = nil; end
				if not AI.Target and Owner:EquipDiggingTool(true) then
					AI.deviceState = AHuman.DIGGING;
					AI.proneState = AHuman.NOTPRONE;
					phase = phase + TimerMan.AIDeltaTimeSecs * 4;
					local direction = delta.X < 0 and -1 or 1;
					local obstruction = SceneMan:CastNotMaterialRay(Owner.Pos,
						Vector(direction * Owner.Height * 0.36, 0), rte.airID, 2, false);
					local opening = math.abs(delta.X) > Owner.Height * 0.4 and obstruction >= 0;
					local aimPoint = opening and Owner.Pos + Vector(direction * math.max(1, obstruction), 0) or target;
					local shoulder = Owner.FGArm and Owner.FGArm.JointPos or Owner.EyePos;
					local aim = SceneMan:ShortestDistance(shoulder, aimPoint, true);
					AI.Ctrl.AnalogAim = aim.Normalized:RadRotate(math.sin(phase) * (opening and 0.7 or 0.45));
					AI.fire = not Owner.FirearmIsEmpty;
					if Owner.FirearmIsEmpty then AI.Ctrl:SetState(Controller.WEAPON_RELOAD, true); end
					-- Leave time to refuel while opening a shaft towards gold above us.
					local ceiling = SceneMan:CastNotMaterialRay(shoulder, Vector(0, -Owner.Height * 0.35), rte.airID, 2, false);
					AI.jump = delta.Y < -28 and ceiling < 0 and Owner.Jetpack and Owner.Jetpack.JetTimeLeft > TimerMan.AIDeltaTimeMS and phase % 10 < 7;
					-- Advance only after opening the tunnel in front of the body.
					-- A remnant inside the barrel's reach needs some distance before it can be hit.
					local tooClose = delta:MagnitudeIsLessThan(Owner.Height * 0.22);
					-- A wall behind the muzzle also needs a little room before firing can clear it.
					local backAway = tooClose or (opening and obstruction < Owner.Height * 0.28);
					if backAway then direction = -direction; end
					if (math.abs(delta.X) > 4 or tooClose) and (not opening or backAway) and SceneMan:CastNotMaterialRay(Owner.Pos,
						Vector(direction * Owner.Height * 0.16, 0), rte.airID, 2, false) < 0 then
						AI.lateralMoveState = direction < 0 and Actor.LAT_LEFT or Actor.LAT_RIGHT;
					else
						AI.lateralMoveState = Actor.LAT_STILL;
					end
				end
			else
				if not (AI.GoToBehavior or AI.NextGoTo) or not travelTarget or
					SceneMan:ShortestDistance(travelTarget, target, true):MagnitudeIsGreaterThan(24) then
					StopTravel(AI, Owner);
					Owner:AddAISceneWaypoint(target);
					AI:CreateGoToBehavior(Owner);
					travelTarget = Vector(target.X, target.Y);
				end
			end
		end

		local _ai, _owner, abort = coroutine.yield();
		if abort then return true; end
	end
	return true;
end
