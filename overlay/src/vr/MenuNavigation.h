#pragma once

// Held navigation is shared by settings, cheats and mission lists. Hysteresis
// prevents noisy sticks from turning a single held direction into extra taps.
struct VrMenuNavigation
{
	int direction = 0;
	unsigned long long repeatAt = 0;

	void Reset() { direction = 0; repeatAt = 0; }

	int Pulse(float axis, unsigned long long now)
	{
		const int requested = axis >= 0.68f ? -1 : (axis <= -0.68f ? 1 : 0);
		if(axis >= -0.34f && axis <= 0.34f){
			Reset();
			return 0;
		}
		if(requested && requested != direction){
			direction = requested;
			repeatAt = now+430;
			return direction;
		}
		if(direction && now >= repeatAt){
			// Do not catch up missed pulses after a stalled frame.
			repeatAt = now+110;
			return direction;
		}
		return 0;
	}
};
