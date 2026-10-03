package.path = ROOT .. '/Data/Base.rte/?.lua;' .. package.path
package.loaded.Constants = true
rte = {airID=0, goldID=2, grassID=3, doorID=4}
Actor = {AIMODE_NONE=0,AIMODE_SENTRY=1,AIMODE_GOTO=2,AIMODE_SQUAD=3,AIMODE_GOLDDIG=4,AIMODE_PATROL=5,AIMODE_BRAINHUNT=6,
 LAT_STILL=0,LAT_LEFT=1,LAT_RIGHT=2,NOTBLOCKED=0,BLOCKED=1,IGNORINGBLOCK=2,PROCEEDING=0,DIGPAUSING=1,INACTIVE=4}
AHuman = {NOTPRONE=0,GOPRONE=1,PRONE=2,NOTJUMPING=0,PREJUMP=1,UPJUMP=2,STILL=0,AIMING=1,POINTING=2,DIGGING=3,NOTDIGGING=0,STARTDIG=1}
Activity = {UNFAIRSKILL=100,NOTEAM=-1}
GameActivity = {NUTSDIFFICULTY=95}
Controller = setmetatable({}, {__index=function(t,k) rawset(t,k,k);return k end})
FrameMan = {PlayerScreenWidth=960,PlayerScreenHeight=540}
TimerMan = {DeltaTimeSecs=1/60,AIDeltaTimeSecs=1/30,AIDeltaTimeMS=1000/30}
SettingsMan = {AIUpdateInterval=2}
local now=0
function Timer()
 local t={start=now,limit=math.huge}
 function t:SetSimTimeLimitMS(ms) self.limit=ms end
 function t:IsPastSimMS(ms) return now-self.start>ms end
 function t:IsPastSimTimeLimit() return now-self.start>self.limit end
 function t:Reset() self.start=now end
 return setmetatable(t,{__index=function(self,k) if k=='ElapsedSimTimeMS' then return now-self.start end end})
end
function RangeRand(a,b) return (a+b)/2 end
function PosRand() return 0.5 end
function GetPPM() return 20 end
math.randomseed(42)
local vm={}
local vmt={}
function Vector(x,y) return setmetatable({X=x or 0,Y=y or 0},vmt) end
function vm:RadRotate(a) local x,y=self.X,self.Y;a=-a;self.X=x*math.cos(a)-y*math.sin(a);self.Y=x*math.sin(a)+y*math.cos(a);return self end
function vm:DegRotate(a) return self:RadRotate(math.rad(a)) end
function vm:SetMagnitude(m) local a=self.Magnitude;if a>0 then self.X,self.Y=self.X*m/a,self.Y*m/a end;return self end
function vm:Normalize() return self:SetMagnitude(1) end
function vm:CapMagnitude(m) if self.Magnitude>m then self:SetMagnitude(m) end;return self end
function vm:MagnitudeIsLessThan(m) return self.Magnitude<m end
function vm:MagnitudeIsGreaterThan(m) return self.Magnitude>m end
function vm:Cross(v) return self.X*v.Y-self.Y*v.X end
vmt.__index=function(v,k)
 if k=='Magnitude' then return math.sqrt(v.X*v.X+v.Y*v.Y)
 elseif k=='SqrMagnitude' then return v.X*v.X+v.Y*v.Y
 elseif k=='Largest' then return math.max(math.abs(v.X),math.abs(v.Y))
 elseif k=='AbsRadAngle' then local a=-math.atan2(v.Y,v.X);return a<-math.pi/2 and a+2*math.pi or a
 elseif k=='Normalized' then return Vector(v.X,v.Y):Normalize() end
 return vm[k]
end
vmt.__add=function(a,b) return Vector(a.X+b.X,a.Y+b.Y) end
vmt.__sub=function(a,b) return Vector(a.X-b.X,a.Y-b.Y) end
vmt.__mul=function(a,b) return Vector(a.X*b,a.Y*b) end
vmt.__div=function(a,b) return Vector(a.X/b,a.Y/b) end
SceneMan = {SceneWidth=1000,SceneHeight=1000,SceneWrapsX=false,SceneWrapsY=false}
function SceneMan:ShortestDistance(a,b) return b-a end
function SceneMan:CastMaterialRay() return false end
function SceneMan:CastStrengthSumRay() return 0 end
function SceneMan:CastStrengthRay() return false end
function SceneMan:GetTerrMatter() return 0 end
function SceneMan:IsUnseen() return false end
function SceneMan:MovePointToGround(p) return Vector(p.X,p.Y) end
function SceneMan:CastObstacleRay() return -1 end
function SceneMan:GetLastRayHitPos() return Vector() end
ActivityMan={}
function ActivityMan:GetActivity() return {IsHumanTeam=function()return true end,GetTeamAISkill=function()return 50 end} end
MovableMan={}
function MovableMan:ValidMO(m) return m and not m.invalid end
function MovableMan:IsDevice(m) return m and m.ClassName=='HDFirearm' end
function ToHeldDevice(m) return m end
function IsHeldDevice(m) return m.ClassName=='HDFirearm' end
function ToMOSprite(m) return m end
ConsoleMan={errors={}}
function ConsoleMan:PrintString(s) table.insert(self.errors,s) end
PathRequest={NoSolution=-1}
require('AI/NativeHumanAI')
local function owner()
 local o={Pos=Vector(500,500),PrevPos=Vector(500,500),EyePos=Vector(500,480),Vel=Vector(),Height=40,AimRange=1.5,Health=100,MaxHealth=100,
 Team=0,HFlipped=false,FlipFactor=1,AIMode=Actor.AIMODE_GOLDDIG,InventorySize=1,PresetName='Miner',Perceptiveness=0.5,DigStrength=100,
 FirearmIsEmpty=false,FirearmNeedsReload=false,FirearmIsReady=true,MovePathSize=0,MoveProximityLimit=10,Radius=20}
 o.digger={MuzzlePos=Vector(520,495),HasObjectInGroup=function(_,g)return g=='Tools - Diggers' end,GetModuleAndPresetName=function()return 'Digger' end}
 o.weapon={MuzzlePos=Vector(520,495),HasObjectInGroup=function(_,g)return g=='Weapons' end,GetModuleAndPresetName=function()return 'Rifle' end}
 o.EquippedItem=o.digger
 function o:NumberValueExists()return false end
 function o:HasObjectInGroup(g)return g=='Tools - Diggers' end
 function o:EquipDiggingTool(e)if e then self.EquippedItem=self.digger end;return true end
 function o:EquipFirearm(e)if e then self.EquippedItem=self.weapon end;return true end
 function o:EquipThrowable()return false end
 function o:GetController()return self.ctrl end
 function o:GetAlarmPoint()return Vector()end
 function o:LookForMOs()return nil end
 function o:GetAimAngle()return 0 end
 function o:SetAimAngle()end
 function o:UpdateMovePath()self.IsWaitingOnNewMovePath=false end
 function o:GetLastAIWaypoint()return self.wpt or self.Pos end
 function o:GetWaypointListSize()return self.waypointCount or 0 end
 function o:ClearAIWaypoints()self.waypointCount=0 end
 function o:ClearMovePath()self.MovePathSize=0 end
 function o:DrawWaypoints()end
 function o:AddAISceneWaypoint(p)self.wpt=p;self.waypointCount=1 end
 function o:ReloadFirearms()self.reloads=(self.reloads or 0)+1 end
 function o:RemoveNumberValue()end
 function o:EquipShieldInBGArm()end
 function o:EquipDeviceInGroup()return true end
 function o:SetNumberValue()end
 o.MovePath=function()return nil end
 o.ctrl={states={},AnalogAim=Vector(1,0)}
 function o.ctrl:SetState(k,v)self.states[k]=v end
 return o
end
local function resume(co,ai,o)
 local ok,res=coroutine.resume(co,ai,o,false)
 assert(ok,res)
 return res
end

return {owner=owner,resume=resume,advance=function(ms)now=now+ms end}
