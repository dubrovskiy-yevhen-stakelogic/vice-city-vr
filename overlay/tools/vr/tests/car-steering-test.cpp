#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include "../../../src/vr/VrCarSteering.h"

static int checks;
static void Check(bool value, const char *name)
{
	++checks;
	if(!value){ std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
static float Radians(float degrees) { return degrees*VrCarSteering::Pi/180.0f; }
static void Angle(float actual, float expectedDegrees, const char *name)
{
	if(!std::isfinite(actual) || std::fabs(actual-Radians(expectedDegrees)) > Radians(0.003f)){
		std::fprintf(stderr, "%s: actual %.6f degrees, expected %.6f\n", name,
			actual*180.0f/VrCarSteering::Pi, expectedDegrees);
		Check(false, name);
	}
	Check(true, name);
}

struct Wheel
{
	VrCarSteering::State state;
	int carA = 1, carB = 2;
	const void *vehicle = &carA;
	unsigned int mask = 3;
	float angle = 0.0f;
	VrCarSteering::Result Projected(float x, float y, bool tracked = true)
	{
		const VrCarSteering::Result result = VrCarSteering::Update(
			state, vehicle, mask, x, y, tracked, angle);
		angle = result.angle;
		Check(std::isfinite(angle) && std::fabs(angle) <= VrCarSteering::MaxAngle,
			"finite bounded wheel output");
		return result;
	}
	VrCarSteering::Result Step(float degrees, float radius = 0.2f, bool tracked = true)
	{
		return Projected(radius*std::cos(Radians(degrees)),
			radius*std::sin(Radians(degrees)), tracked);
	}
};

static void TwoHands()
{
	for(int sign : {-1, 1}){
		Wheel wheel;
		for(int degree = 0; degree <= 110; ++degree) wheel.Step(float(sign*degree));
		Angle(wheel.angle, float(sign*80), "two hands at 110");
		Angle(wheel.state.continuousAngle, float(sign*110), "retain overshoot");
		for(int degree = 109; degree >= 0; --degree){
			wheel.Step(float(sign*degree));
			Angle(wheel.angle, float(sign*(degree > 80 ? 80 : degree)), "unwind to original center");
		}
		Check(wheel.state.captureCount == 1, "no stop recapture");
		for(int degree = 1; degree <= 360; ++degree){
			wheel.Step(float(sign*degree));
			Angle(wheel.state.continuousAngle, float(sign*degree), "unwrap full turn");
		}
		for(int degree = 359; degree >= 0; --degree){
			wheel.Step(float(sign*degree));
			Angle(wheel.state.continuousAngle, float(sign*degree), "unwrap returning full turn");
			Angle(wheel.angle, float(sign*(degree > 80 ? 80 : degree)), "full turn lock retention");
		}
		Check(wheel.state.captureCount == 1, "no false tracking jump past 125 or 180 degrees");
	}
	for(int sign : {-1, 1}){
		Wheel wheel;
		wheel.Step(float(sign*170));
		for(int degree = 171; degree <= 200; ++degree){
			wheel.Step(float(sign*degree));
			Angle(wheel.angle, float(sign*(degree-170)), "cross atan2 branch from nonzero grab");
		}
	}
}

static void OneHand()
{
	for(unsigned int hand : {1u, 2u}) for(int sign : {-1, 1}){
		Wheel wheel;
		wheel.mask = hand;
		const float offset = hand == 1u ? 180.0f : 0.0f;
		for(int degree = 0; degree <= 120; ++degree){
			wheel.Step(offset+float(sign*degree));
			Angle(wheel.angle, float(sign*(degree > 80 ? 80 : degree)), "one-hand circle does not fold at 90");
		}
		wheel.Step(offset+float(sign*119));
		Angle(wheel.angle, float(sign*79), "one-hand stop reseats and responds on reverse");
		wheel.Step(offset+float(sign*118));
		Angle(wheel.angle, float(sign*78), "one-hand reverse remains continuous");
		Check(wheel.state.captureCount == 1, "one-hand stop is not a tracking jump");
	}
}

static void Ownership()
{
	Wheel wheel;
	for(int degree = 0; degree <= 35; ++degree) wheel.Step(float(degree));
	wheel.mask = 1;
	wheel.Step(125.0f);
	Angle(wheel.angle, 35, "two to one hand reseat");
	wheel.Step(126.0f);
	Angle(wheel.angle, 36, "one hand after transition");
	wheel.mask = 2;
	wheel.Step(-54.0f);
	Angle(wheel.angle, 36, "left to right hand reseat");
	wheel.mask = 3;
	wheel.Step(0.0f);
	Angle(wheel.angle, 36, "one to two hands reseat");
	for(int degree = 1; degree <= 100; ++degree) wheel.Step(float(degree));
	Angle(wheel.angle, 80, "transition fixture at lock");
	wheel.mask = 1;
	wheel.Step(-90.0f);
	Angle(wheel.angle, 80, "two to one hand at lock");
	wheel.Step(-91.0f);
	Angle(wheel.angle, 79, "stop transition drops old mode overshoot");
	wheel.mask = 3;
	wheel.Step(160.0f);
	Angle(wheel.angle, 79, "one to two hands near lock");
	wheel.mask = 0;
	wheel.Step(160.0f);
	Angle(wheel.angle, 0, "release resets steering");
	Check(!wheel.state.valid, "release invalidates reference");
	wheel.mask = 3;
	wheel.Step(-100.0f);
	Angle(wheel.angle, 0, "regrab captures new reference");
	wheel.Step(-99.0f);
	Angle(wheel.angle, 1, "regrab moves from new reference");
	wheel.vehicle = &wheel.carB;
	wheel.Projected(0, 0);
	Angle(wheel.angle, 0, "new vehicle resets even before a valid sample");
	wheel.Step(70.0f);
	Angle(wheel.angle, 0, "new vehicle first valid sample reseats");
}

static void Recenter()
{
	for(unsigned int mask : {1u, 2u, 3u}) for(int sign : {-1, 1}){
		Wheel wheel;
		wheel.mask = mask;
		for(int degree = 0; degree <= 35; ++degree) wheel.Step(float(sign*degree));
		// Recreating OpenXR space clears the reference but keeps held grips and
		// the visible wheel angle until the first sample in the new space.
		wheel.state = VrCarSteering::State();
		wheel.Step(124.0f);
		Angle(wheel.angle, float(sign*35), "recenter preserves held wheel angle");
		wheel.Step(124.0f+float(sign));
		Angle(wheel.angle, float(sign*36), "recenter follows next new-space delta");
	}
	Wheel wheel;
	for(int degree = 0; degree <= 150; ++degree) wheel.Step(float(degree));
	wheel.state = VrCarSteering::State();
	wheel.Step(-170.0f);
	Angle(wheel.angle, 80, "recenter at lock keeps physical angle");
	wheel.Step(-171.0f);
	Angle(wheel.angle, 79, "recenter discards old-space overshoot");
}

static void InvalidTracking()
{
	for(unsigned int mask : {1u, 2u, 3u}){
		Wheel wheel;
		wheel.mask = mask;
		wheel.Step(0);
		wheel.Step(20);
		wheel.Step(20, 0.2f, false);
		Angle(wheel.angle, 20, "tracking lost holds wheel");
		Check(!wheel.state.valid, "tracking loss invalidates angle");
		wheel.Step(-150);
		Angle(wheel.angle, 20, "tracking recovery reseats");
		wheel.Step(-149);
		Angle(wheel.angle, 21, "tracking recovery resumes");
		wheel.Step(70, mask == 3 ? 0.005f : 0.04f);
		Angle(wheel.angle, 21, "center or collapsed chord holds wheel");
		wheel.Step(70);
		Angle(wheel.angle, 21, "leaving center reseats");
		for(float bad : {std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()}){
			wheel.Projected(bad, bad);
			Angle(wheel.angle, 21, "nonfinite or overflowing point holds wheel");
			Check(!wheel.state.valid, "nonfinite point invalidates reference");
			wheel.Step(0);
			Angle(wheel.angle, 21, "nonfinite recovery reseats");
		}
		const unsigned int captures = wheel.state.captureCount;
		wheel.Step(100);
		Angle(wheel.angle, 21, "large tracking jump rejected");
		Check(wheel.state.captureCount == captures+1, "tracking jump recaptures reference");
		wheel.Step(101);
		Angle(wheel.angle, 22, "tracking jump recovery resumes");
		wheel.angle = std::numeric_limits<float>::quiet_NaN();
		wheel.Step(30);
		Angle(wheel.angle, 0, "invalid output resets reference");
	}
	Wheel wheel;
	for(int degree = 0; degree <= 200; ++degree) wheel.Step(float(degree));
	const unsigned int captures = wheel.state.captureCount;
	wheel.Step(300);
	Angle(wheel.angle, 80, "jump beyond stop keeps physical wheel");
	Check(wheel.state.captureCount == captures+1, "jump beyond stop is rejected against previous input");
}

static void Jitter()
{
	for(unsigned int mask : {1u, 2u, 3u}){
		Wheel wheel;
		wheel.mask = mask;
		wheel.Step(0);
		for(int i = 0; i < 2000; ++i){
			const float input = i % 2 ? 0.2f : -0.2f;
			wheel.Step(input, i % 3 ? 0.3f : 0.1f);
			Angle(wheel.angle, input, "angular jitter independent of radial reach");
		}
		for(int degree = 1; degree <= 120; ++degree) wheel.Step(float(degree));
		for(int i = 0; i < 2000; ++i){
			wheel.Step(i % 2 ? 120.02f : 119.98f);
			Check(wheel.angle >= Radians(79.95f), "stop jitter cannot drift away from lock");
		}
		Check(wheel.state.captureCount == 1, "jitter does not repeatedly recapture");
	}
}

static void QuestGripReference(unsigned int &grabbedMask, unsigned int &downMask,
	unsigned int unavailableMask, const float grips[2], const float distances[2])
{
	// The acquisition/release order in QuestDrivingVR::UpdateImmersiveCarInput.
	for(unsigned int hand = 0; hand < 2; ++hand){
		const unsigned int bit = 1u << hand;
		const bool unavailable = (unavailableMask & bit) != 0;
		if((grabbedMask & bit) && (unavailable || grips[hand] <= 0.30f))
			grabbedMask &= ~bit;
		if(!(grabbedMask & bit) && !unavailable && grips[hand] >= 0.65f &&
		   !(downMask & bit) && distances[hand] <= 0.23f)
			grabbedMask |= bit;
		if(grips[hand] <= 0.30f)
			downMask &= ~bit;
		else if(grips[hand] >= 0.65f)
			downMask |= bit;
	}
}

static void GripReferenceGrid()
{
	const float levels[] = {0.0f, 0.2999f, 0.30f, 0.3001f, 0.6499f, 0.65f, 1.0f};
	const float reaches[] = {0.0f, 0.23f, 0.2301f, 1000.0f};
	for(unsigned int grabbed = 0; grabbed < 4; ++grabbed)
	for(unsigned int down = 0; down < 4; ++down)
	for(unsigned int unavailable = 0; unavailable < 4; ++unavailable)
	for(float leftGrip : levels) for(float rightGrip : levels)
	for(float leftDistance : reaches) for(float rightDistance : reaches){
		const float grips[] = {leftGrip, rightGrip};
		const float distances[] = {leftDistance, rightDistance};
		unsigned int expectedGrabbed = grabbed, expectedDown = down;
		QuestGripReference(expectedGrabbed, expectedDown, unavailable, grips, distances);
		unsigned int actualGrabbed = 0, actualDown = 0;
		for(unsigned int hand = 0; hand < 2; ++hand){
			const unsigned int bit = 1u << hand;
			bool gripDown = (down & bit) != 0;
			if(VrCarSteering::UpdateGrip((grabbed & bit) != 0, gripDown,
				grips[hand], (unavailable & bit) != 0, distances[hand]))
				actualGrabbed |= bit;
			if(gripDown) actualDown |= bit;
		}
		Check(actualGrabbed == expectedGrabbed, "grab mask matches Quest threshold grid");
		Check(actualDown == expectedDown, "grip latch mask matches Quest threshold grid");
	}
}

struct GripHand
{
	bool grabbed = false, down = false;
	bool Step(float grip, float neutralDistance = 0.18f, bool unavailable = false)
	{
		grabbed = VrCarSteering::UpdateGrip(grabbed, down, grip, unavailable, neutralDistance);
		return grabbed;
	}
};

static void GripTransitions()
{
	GripHand hand;
	Check(!hand.Step(0.6499f), "below press threshold cannot acquire");
	Check(!hand.down, "below press threshold does not latch");
	Check(hand.Step(0.65f, 0.23f), "press and neutral reach thresholds are inclusive");
	Check(hand.down, "press latches grip");
	for(float grip : {0.64f, 0.31f, 0.3001f}){
		Check(hand.Step(grip, 1.0f), "grab survives hysteresis and leaving socket");
		Check(hand.down, "hysteresis retains press latch");
	}
	Check(!hand.Step(0.30f), "release threshold is inclusive");
	Check(!hand.down, "release clears press latch");
	Check(!hand.Step(0.64f), "partial squeeze after release cannot acquire");
	Check(hand.Step(0.65f), "fresh squeeze reacquires");
	Check(!hand.Step(1.0f, 0.18f, true), "unavailable hand releases ownership");
	Check(hand.down, "tracking loss retains held squeeze latch");
	Check(!hand.Step(1.0f), "tracking recovery with held squeeze cannot reacquire");
	Check(!hand.Step(0.30f), "release after tracking recovery stays ungrabbed");
	Check(hand.Step(0.65f), "fresh squeeze after tracking recovery reacquires");

	GripHand missed;
	Check(!missed.Step(0.65f, 0.2301f), "new squeeze outside neutral reach misses");
	Check(missed.down, "missed squeeze still latches");
	Check(!missed.Step(1.0f, 0.0f), "moving held controller into socket cannot acquire");
	Check(!missed.Step(0.30f, 0.0f), "missed squeeze must first release");
	Check(missed.Step(0.65f, 0.0f), "new squeeze at socket succeeds");

	GripHand blocked;
	Check(!blocked.Step(0.65f, 0.0f, true), "unavailable hand cannot acquire on press");
	Check(blocked.down, "unavailable press still latches");
	Check(!blocked.Step(0.65f, 0.0f), "availability alone does not create a press edge");
	Check(!blocked.Step(0.0f, 0.0f), "release clears unavailable press");
	Check(blocked.Step(0.65f, 0.0f), "available fresh squeeze succeeds");
}

static void RotatingSocketCapture()
{
	const float radius = 0.18f, idleReach = 0.25f;
	for(int side : {-1, 1}){
		const float idleX = 0.0f, idleY = float(side)*idleReach;
		const float neutralX = float(side)*radius;
		const float neutralDistance = std::hypot(idleX-neutralX, idleY);
		Check(std::fabs(neutralDistance-0.30805844f) < 0.000001f,
			"idle controller is 30.8 cm from neutral socket");
		GripHand held;
		Check(!held.Step(1.0f, neutralDistance), "idle controller misses initial squeeze");
		for(float degrees : {28.0f, 80.0f}){
			const float rotatedX = float(side)*radius*std::cos(Radians(degrees));
			const float rotatedY = float(side)*radius*std::sin(Radians(degrees));
			const float rotatedDistance = std::hypot(idleX-rotatedX, idleY-rotatedY);
			const float expectedDistance = degrees == 28.0f ? 0.229451f : 0.0791663f;
			Check(std::fabs(rotatedDistance-expectedDistance) < 0.000001f,
				"rotated socket passes within reach at 28 and 80 degrees");
			Check(rotatedDistance <= 0.23f, "old level-only rotated socket would capture idle hand");
			Check(!held.Step(1.0f, neutralDistance), "turning wheel cannot capture held idle hand");
			Check(!held.Step(1.0f, rotatedDistance), "press edge guard independently rejects held hand");
			GripHand fresh;
			Check(!fresh.Step(0.65f, neutralDistance), "fresh squeeze needs neutral not rotated reach");
			Check(!fresh.Step(0.30f, 0.18f), "release before intentional wheel acquisition");
			Check(fresh.Step(0.65f, 0.18f), "intentional new squeeze near neutral socket succeeds");
		}
	}
}

int main()
{
	TwoHands();
	OneHand();
	Ownership();
	Recenter();
	InvalidTracking();
	Jitter();
	Check(checks == 30612, "all existing angle solver regression checks retained");
	GripReferenceGrid();
	GripTransitions();
	RotatingSocketCapture();
	std::printf("car steering: PASS (%d checks, production VrCarSteering.h)\n", checks);
}
