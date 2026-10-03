#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace RTE {

	/// A scene-wide, exact-pixel gold index. SceneMan serializes access from AI workers.
	class GoldMiningSurvey {
	public:
		struct Target { int x = -1; int y = -1; int count = 0; };

		template <typename IsGold>
		void Reset(int width, int height, IsGold isGold) {
			m_Width = width;
			m_Height = height;
			m_Columns = (width + c_CellSize - 1) / c_CellSize;
			m_Cells.assign(m_Columns * ((height + c_CellSize - 1) / c_CellSize), {});
			m_Claims.clear();
			m_Rejected.clear();
			m_Cursor = 0;
			for (int cell = 0; cell < static_cast<int>(m_Cells.size()); ++cell) { Scan(cell, isGold); }
		}

		template <typename IsGold>
		void Refresh(int cellBudget, IsGold isGold) {
			for (int i = 0; i < cellBudget && !m_Cells.empty(); ++i) {
				Scan(m_Cursor, isGold);
				m_Cursor = (m_Cursor + 1) % m_Cells.size();
			}
		}

		template <typename IsGold>
		Target Acquire(int actor, int team, float x, float y, bool wrapsX, bool wrapsY, double nowMS, IsGold isGold) {
			Prune(nowMS);
			if (auto claim = m_Claims.find(actor); claim != m_Claims.end()) {
				Scan(claim->second.cell, isGold);
				if (m_Cells[claim->second.cell].count > 0) {
					claim->second.expires = nowMS + c_LeaseMS;
					return NearestPixel(claim->second.cell, x, y, wrapsX, wrapsY, isGold);
				}
				m_Claims.erase(claim);
			}

			// Reserve patches rather than individual pixels so team-mates do not crowd a vein.
			std::vector<bool> occupied(m_Cells.size(), false);
			std::vector<bool> assistance(m_Cells.size(), false);
			for (const auto& [owner, claim] : m_Claims) {
				if (claim.team == team) {
					occupied[claim.cell] = true;
					if (nowMS - claim.started >= c_AssistanceMS) { assistance[claim.cell] = true; }
				}
			}
			while (true) {
				int best = -1;
				float bestDistance = INFINITY;
				for (int cell = 0; cell < static_cast<int>(m_Cells.size()); ++cell) {
					const Target& target = m_Cells[cell];
					if (target.count == 0 || occupied[cell] || m_Rejected.contains(Key(actor, cell))) { continue; }
					float distance = Distance(x, y, target.x, target.y, wrapsX, wrapsY);
					if (distance < bestDistance) { best = cell; bestDistance = distance; }
				}
				// Once every patch has a miner, idle team-mates may help finish older jobs.
				if (best < 0) {
					for (int cell = 0; cell < static_cast<int>(m_Cells.size()); ++cell) {
						const Target& target = m_Cells[cell];
						if (target.count == 0 || !assistance[cell] || m_Rejected.contains(Key(actor, cell))) { continue; }
						float distance = Distance(x, y, target.x, target.y, wrapsX, wrapsY);
						if (distance < bestDistance) { best = cell; bestDistance = distance; }
					}
				}
				if (best < 0) { return {}; }
				Scan(best, isGold); // Terrain may have changed since the rolling survey.
				if (m_Cells[best].count == 0) { continue; }
				m_Claims[actor] = {best, team, nowMS + c_LeaseMS, nowMS};
				return NearestPixel(best, x, y, wrapsX, wrapsY, isGold);
			}
		}

		void Release(int actor, double retryDelayMS, double nowMS) {
			if (auto claim = m_Claims.find(actor); claim != m_Claims.end()) {
				if (retryDelayMS > 0) { m_Rejected[Key(actor, claim->second.cell)] = nowMS + retryDelayMS; }
				m_Claims.erase(claim);
			}
		}

		int Remaining(int actor) const {
			auto claim = m_Claims.find(actor);
			return claim == m_Claims.end() ? 0 : m_Cells[claim->second.cell].count;
		}

	private:
		static constexpr int c_CellSize = 24;
		static constexpr double c_LeaseMS = 5000;
		static constexpr double c_AssistanceMS = 8000;
		struct Claim { int cell; int team; double expires; double started; };
		int m_Width = 0, m_Height = 0, m_Columns = 0;
		std::size_t m_Cursor = 0;
		std::vector<Target> m_Cells;
		std::unordered_map<int, Claim> m_Claims;
		std::unordered_map<std::uint64_t, double> m_Rejected;

		static std::uint64_t Key(int actor, int cell) {
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(actor)) << 32) | static_cast<std::uint32_t>(cell);
		}

		void Prune(double nowMS) {
			std::erase_if(m_Claims, [nowMS](const auto& entry) { return entry.second.expires <= nowMS; });
			std::erase_if(m_Rejected, [nowMS](const auto& entry) { return entry.second <= nowMS; });
		}

		float Distance(float x, float y, int tx, int ty, bool wrapsX, bool wrapsY) const {
			float dx = std::abs(x - tx), dy = std::abs(y - ty);
			if (wrapsX) { dx = std::fmod(dx, static_cast<float>(m_Width)); dx = std::min(dx, m_Width - dx); }
			if (wrapsY) { dy = std::fmod(dy, static_cast<float>(m_Height)); dy = std::min(dy, m_Height - dy); }
			return dx * dx + dy * dy;
		}

		template <typename IsGold>
		void Scan(int cell, IsGold isGold) {
			Target target;
			const int left = cell % m_Columns * c_CellSize, top = cell / m_Columns * c_CellSize;
			for (int y = top; y < std::min(top + c_CellSize, m_Height); ++y) {
				for (int x = left; x < std::min(left + c_CellSize, m_Width); ++x) {
					if (isGold(x, y)) {
						if (target.count == 0) { target.x = x; target.y = y; }
						++target.count;
					}
				}
			}
			m_Cells[cell] = target;
		}

		template <typename IsGold>
		Target NearestPixel(int cell, float ox, float oy, bool wrapsX, bool wrapsY, IsGold isGold) const {
			Target target = m_Cells[cell];
			float nearest = INFINITY;
			const int left = cell % m_Columns * c_CellSize, top = cell / m_Columns * c_CellSize;
			for (int y = top; y < std::min(top + c_CellSize, m_Height); ++y) {
				for (int x = left; x < std::min(left + c_CellSize, m_Width); ++x) {
					if (isGold(x, y)) {
						float distance = Distance(ox, oy, x, y, wrapsX, wrapsY);
						if (distance < nearest) { nearest = distance; target.x = x; target.y = y; }
					}
				}
			}
			return target;
		}
	};
}
