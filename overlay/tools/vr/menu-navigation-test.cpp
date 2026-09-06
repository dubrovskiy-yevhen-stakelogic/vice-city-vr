#include "../../src/vr/MenuNavigation.h"
#include "../../src/save/SaveBlockBounds.h"
#include <cstdio>

int main()
{
	VrMenuNavigation input;
	int failures = 0;
	auto expect = [&](float axis, unsigned long long time, int expected) {
		const int actual = input.Pulse(axis, time);
		if(actual != expected){
			std::printf("FAIL axis=%.2f time=%llu expected=%d got=%d\n",
				axis, time, expected, actual);
			++failures;
		}
	};
	expect(0.67f, 100, 0);
	expect(0.70f, 110, -1);
	expect(0.60f, 200, 0); // Noise below engage must not release the stick.
	expect(0.69f, 539, 0);
	expect(0.69f, 540, -1);
	expect(0.69f, 649, 0);
	expect(0.69f, 650, -1);
	expect(-0.80f, 660, 1); // Reverse without a neutral sampled frame.
	expect(-0.80f, 700, 0);
	expect(0.0f, 710, 0);
	expect(-0.80f, 720, 1);
	expect(-0.80f, 5000, 1); // A stalled frame produces one step, no catch-up.
	expect(-0.80f, 5000, 0);
	input.Reset();
	expect(-0.80f, 5010, 1);
	expect(-0.33f, 5020, 0);
	expect(-0.67f, 5030, 0);
	if(!SaveBlockFits(0, 55000) || !SaveBlockFits(54997, 55000) ||
	   !SaveBlockFits(55000, 55000) || SaveBlockFits(55001, 55000) ||
	   SaveBlockFits(0xffffffffU, 55000) || SaveBlockFits(0xfffffffeU, 55000) ||
	   SaveBlockFits(1, 1) || !SaveBlockFits(4, 4)){
		std::printf("FAIL save block bounds\n");
		++failures;
	}
	std::printf("MENU_NAVIGATION_COMPLETE failures=%d\n", failures);
	return failures ? 1 : 0;
}
