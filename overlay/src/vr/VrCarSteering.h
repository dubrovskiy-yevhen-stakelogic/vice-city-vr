#pragma once

#include <cmath>

namespace VrCarSteering
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float MaxAngle = 80.0f*Pi/180.0f;
constexpr float MaxStep = 45.0f*Pi/180.0f;

inline float Wrap(float angle)
{
	while(angle > Pi) angle -= 2.0f*Pi;
	while(angle < -Pi) angle += 2.0f*Pi;
	return angle;
}

inline float Unwrap(float angle, float reference)
{
	while(angle-reference > Pi) angle -= 2.0f*Pi;
	while(angle-reference < -Pi) angle += 2.0f*Pi;
	return angle;
}

inline float Limit(float angle)
{
	return angle < -MaxAngle ? -MaxAngle : angle > MaxAngle ? MaxAngle : angle;
}

inline bool UpdateGrip(bool grabbed, bool &gripDown, float grip,
	bool unavailable, float neutralSocketDistance)
{
	if(grabbed && (unavailable || grip <= 0.30f))
		grabbed = false;
	// As on Quest, reaching the neutral socket with an already-held squeeze
	// must not acquire the wheel. Release and squeeze again to take ownership.
	if(!grabbed && !unavailable && grip >= 0.65f && !gripDown &&
	   neutralSocketDistance <= 0.23f)
		grabbed = true;
	if(grip <= 0.30f)
		gripDown = false;
	else if(grip >= 0.65f)
		gripDown = true;
	return grabbed;
}

struct State
{
	const void *vehicle;
	unsigned int realHandMask;
	unsigned int captureCount;
	bool valid;
	float referenceAngle;
	float continuousAngle;
	State() : vehicle(nullptr), realHandMask(0), captureCount(0), valid(false),
		referenceAngle(0.0f), continuousAngle(0.0f) {}
};

struct Result
{
	float angle;
	float desiredAngle;
	float overflow;
	Result(float value) : angle(value), desiredAngle(value), overflow(0.0f) {}
};

// Two hands supply their labelled chord; one hand supplies its spoke from the
// wheel center. Both are projected into the calibrated wheel plane by the caller.
inline Result Update(State &state, const void *vehicle, unsigned int handMask,
	float planeRight, float planeUp, bool tracked, float physicalAngle)
{
	if(!vehicle || handMask == 0 || handMask > 3){
		state = State();
		return Result(0.0f);
	}
	if(!std::isfinite(physicalAngle)){
		state.valid = false;
		physicalAngle = 0.0f;
	}
	if(state.vehicle && state.vehicle != vehicle)
		physicalAngle = 0.0f;
	physicalAngle = Limit(physicalAngle);
	Result result(physicalAngle);
	const bool changed = state.vehicle != vehicle || state.realHandMask != handMask;
	state.vehicle = vehicle;
	state.realHandMask = handMask;
	const bool twoHands = handMask == 3;
	const float minRadius = twoHands ? 0.01f : 0.06f;
	const float lengthSquared = planeRight*planeRight+planeUp*planeUp;
	if(!tracked || !std::isfinite(planeRight) || !std::isfinite(planeUp) ||
	   !std::isfinite(lengthSquared) || lengthSquared <= minRadius*minRadius){
		state.valid = false;
		return result;
	}
	const float raw = std::atan2(planeUp, planeRight);
	if(changed || !state.valid){
		state.referenceAngle = Wrap(raw-physicalAngle);
		state.continuousAngle = physicalAngle;
		state.valid = true;
		++state.captureCount;
	}
	const float measured = Unwrap(Wrap(raw-state.referenceAngle), state.continuousAngle);
	result.desiredAngle = measured;
	if(std::fabs(measured-state.continuousAngle) > MaxStep){
		// A tracking jump establishes a new reference without moving the wheel.
		state.referenceAngle = Wrap(raw-physicalAngle);
		state.continuousAngle = physicalAngle;
		++state.captureCount;
	}else{
		result.angle = Limit(measured);
		if(twoHands){
			// Keep the controller angle past the stop: reversing must unwind the
			// same motion before leaving full lock, not shift the wheel's center.
			state.continuousAngle = measured;
		}else{
			state.continuousAngle = result.angle;
			if(result.angle != measured)
				state.referenceAngle = Wrap(raw-result.angle);
		}
	}
	result.overflow = measured-result.angle;
	return result;
}
}
