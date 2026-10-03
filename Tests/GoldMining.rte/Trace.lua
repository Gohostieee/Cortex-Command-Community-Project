function ThreadedUpdateAI(self)
	local ai = self.AI;
	if ai then
		self:SetStringValue("MiningRegressionState", string.format("behavior=%s next=%s goto=%s fire=%s move=%s target=%s", tostring(ai.BehaviorName), tostring(ai.NextBehaviorName), tostring(ai.GoToName), tostring(ai.fire), tostring(ai.lateralMoveState), ai.MiningTarget and string.format("%.0f,%.0f", ai.MiningTarget.X, ai.MiningTarget.Y) or "none"));
	end
end
