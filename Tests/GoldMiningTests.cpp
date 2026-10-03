#include "GoldMiningSurvey.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

static void Check(bool condition, const char* message) {
	if (!condition) { throw std::runtime_error(message); }
}

int main() {
	try {
		constexpr int width = 103, height = 97;
		std::vector<unsigned char> pixels(width * height);
		auto gold = [&](int x, int y) { return pixels[y * width + x] != 0; };
		for (auto [x, y] : std::vector<std::pair<int, int>>{{0, 0}, {102, 96}, {0, 96}, {102, 0}, {24, 24}, {25, 24}, {60, 70}}) {
			pixels[y * width + x] = 1;
		}
		RTE::GoldMiningSurvey survey;
		survey.Reset(width, height, gold);
		int mined = 0;
		for (int tick = 0; tick < 20; ++tick) {
			auto target = survey.Acquire(1, 0, 50, 10, false, false, tick * 100, gold);
			if (target.x < 0) { break; }
			Check(gold(target.x, target.y), "survey returned depleted terrain");
			pixels[target.y * width + target.x] = 0;
			++mined;
		}
		Check(mined == 7, "survey missed a remnant, deep vein, or boundary pixel");
		Check(survey.Acquire(1, 0, 50, 10, false, false, 2500, gold).x < 0, "empty map still has a target");
		std::cout << "PASS: every gold pixel, deep veins, boundaries, and remnants\n";

		pixels[30 * width + 30] = pixels[70 * width + 70] = 1;
		survey.Reset(width, height, gold);
		auto first = survey.Acquire(1, 0, 30, 30, false, false, 0, gold);
		auto second = survey.Acquire(2, 0, 30, 30, false, false, 0, gold);
		Check(first.x != second.x, "miners claimed the same patch");
		Check(survey.Acquire(3, 0, 30, 30, false, false, 0, gold).x < 0, "busy patches caused a pileup");
		survey.Release(1, 0, 0);
		Check(survey.Acquire(3, 0, 30, 30, false, false, 1, gold).x == 30, "released patch remained reserved");
		survey.Release(3, 10000, 1);
		Check(survey.Acquire(3, 0, 30, 30, false, false, 2, gold).x < 0, "blocked patch was immediately retried");
		Check(survey.Acquire(3, 0, 30, 30, false, false, 10002, gold).x == 30, "blocked patch was abandoned permanently");
		Check(survey.Acquire(4, 0, 70, 70, false, false, 16000, gold).x == 70, "dead miner's reservation never expired");
		std::cout << "PASS: team reservations, release, expiration, and bounded retry\n";
		survey.Reset(width, height, gold);
		for (int time : {0, 3000, 6000, 9000}) {
			survey.Acquire(1, 0, 30, 30, false, false, time, gold);
			survey.Acquire(2, 0, 70, 70, false, false, time, gold);
		}
		Check(survey.Acquire(3, 0, 30, 30, false, false, 9001, gold).x == 30, "idle miners cannot help finish the last patches");
		std::cout << "PASS: idle miners help finish older remaining patches\n";

		std::fill(pixels.begin(), pixels.end(), 0);
		pixels[20 * width + 1] = pixels[20 * width + 60] = 1;
		survey.Reset(width, height, gold);
		Check(survey.Acquire(1, 0, 100, 20, true, false, 0, gold).x == 1, "wrapped distance ignored the nearby vein");
		survey.Release(1, 0, 0);
		pixels[90 * width + 90] = 1;
		for (int i = 0; i < 100; ++i) { survey.Refresh(1, gold); }
		Check(survey.Acquire(2, 0, 90, 90, false, false, 100, gold).x == 90, "newly settled gold was never surveyed");
		std::fill(pixels.begin(), pixels.end(), 0);
		survey.Reset(width, height, gold);
		Check(survey.Acquire(2, 0, 90, 90, false, false, 101, gold).x < 0, "scene reset kept old targets");
		std::cout << "PASS: wrapping, new terrain gold, and scene reset\n";
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
