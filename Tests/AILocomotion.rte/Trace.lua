function ThreadedUpdateAI(self)
	local ai = self.AI;
	if ai then
		local first;
		for point in self.MovePath do first = point; break; end
		local goal = self:GetLastAIWaypoint();
		self:SetStringValue("LocomotionRegressionState", string.format("jump=%s flying=%s refuel=%s run=%s move=%s path=%d wp=%d goal=%.0f,%.0f first=%s aim=%.2f flipped=%s", tostring(ai.jump), tostring(ai.flying), tostring(ai.refuel), tostring(ai.running), tostring(ai.lateralMoveState), self.MovePathSize, self:GetWaypointListSize(), goal.X, goal.Y, first and string.format("%.0f,%.0f", first.X, first.Y) or "none", self:GetAimAngle(false), tostring(self.HFlipped)));
	end
end
