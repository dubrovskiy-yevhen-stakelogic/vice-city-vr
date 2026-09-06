#pragma once

// On-foot legacy button contributions only. OpenXR service chords, triggers,
// axes and tracked interactions are resolved by the caller before this map.
namespace VrPadBindings
{
enum Source { A, B, X, Y, LEFT_GRIP, RIGHT_GRIP, LEFT_CLICK, RIGHT_CLICK, SOURCE_COUNT };
enum Target {
	NONE, SQUARE, CROSS, CIRCLE, TRIANGLE, L1, R1, L2, R2, L3, R3,
	SPRINT, TARGET_COUNT
};
enum Layout { DEFAULT, SWAPPED_HANDS, CUSTOM };

inline int DefaultTarget(int source)
{
	static const int targets[SOURCE_COUNT] = { CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L3, SPRINT };
	return source >= 0 && source < SOURCE_COUNT ? targets[source] : NONE;
}
inline int SwappedTarget(int source)
{
	static const int targets[SOURCE_COUNT] = { SQUARE, TRIANGLE, CROSS, CIRCLE, L1, R1, L3, SPRINT };
	return source >= 0 && source < SOURCE_COUNT ? targets[source] : NONE;
}
inline const char *Key(int source)
{
	static const char *const keys[SOURCE_COUNT] = {
		"BindA", "BindB", "BindX", "BindY", "BindLeftGrip", "BindRightGrip",
		"BindLeftStickClick", "BindRightStickClick"
	};
	return source >= 0 && source < SOURCE_COUNT ? keys[source] : "";
}
inline const char *SourceName(int source)
{
	static const char *const names[SOURCE_COUNT] = {
		"A", "B", "X", "Y", "LEFT GRIP", "RIGHT GRIP", "LEFT CLICK", "RIGHT CLICK"
	};
	return source >= 0 && source < SOURCE_COUNT ? names[source] : "UNKNOWN";
}
inline const char *TargetName(int target)
{
	static const char *const names[TARGET_COUNT] = {
		"UNUSED", "JUMP [SQUARE]", "CROSS", "ATTACK [CIRCLE]", "ENTER / EXIT",
		"PICK UP [L1]", "TARGET [R1]", "PREVIOUS WEAPON", "NEXT WEAPON",
		"HORN / CROUCH", "LOOK BEHIND", "SPRINT"
	};
	return target >= 0 && target < TARGET_COUNT ? names[target] : "INVALID";
}

struct Bindings
{
	int target[SOURCE_COUNT];
	Bindings(){ Apply(DEFAULT); }
	void Set(int source, int value)
	{
		if(source >= 0 && source < SOURCE_COUNT)
			target[source] = value >= 0 && value < TARGET_COUNT ? value : DefaultTarget(source);
	}
	void Apply(Layout layout)
	{
		for(int source = 0; source < SOURCE_COUNT; source++)
			target[source] = layout == SWAPPED_HANDS ? SwappedTarget(source) : DefaultTarget(source);
	}
	Layout GetLayout() const
	{
		bool isDefault = true, isSwapped = true;
		for(int source = 0; source < SOURCE_COUNT; source++){
			isDefault = isDefault && target[source] == DefaultTarget(source);
			isSwapped = isSwapped && target[source] == SwappedTarget(source);
		}
		return isDefault ? DEFAULT : isSwapped ? SWAPPED_HANDS : CUSTOM;
	}
};

struct Context
{
	bool gameplay, onFoot, remote, consumed, stickCrouch;
	unsigned int capturedSources, capturedTargets;
	int padMode;
	Context() : gameplay(false), onFoot(false), remote(false), consumed(false),
		stickCrouch(false), capturedSources(0), capturedTargets(0), padMode(0) {}
};

inline int FireTarget(int padMode)
{
	return padMode == 2 ? CROSS : padMode == 3 ? R1 : CIRCLE;
}

inline void Resolve(const Bindings &bindings, const int *values, const Context &context,
	int (&output)[TARGET_COUNT])
{
	for(int target = 0; target < TARGET_COUNT; target++) output[target] = 0;
	if(!values || !context.gameplay || !context.onFoot || context.remote || context.consumed) return;
	for(int source = 0; source < SOURCE_COUNT; source++){
		if(context.capturedSources & (1u << source)) continue;
		int target = bindings.target[source];
		if(target <= NONE || target >= TARGET_COUNT) continue;
		if(source == LEFT_CLICK && target == L3 && !context.stickCrouch) continue;
		if(target == SPRINT)
			target = context.padMode == 2 ? CIRCLE : CROSS;
		else if(target == FireTarget(context.padMode))
			continue;
		if(context.capturedTargets & (1u << target)) continue;
		const int value = values[source] < 0 ? 0 : values[source] > 255 ? 255 : values[source];
		if(value > output[target]) output[target] = value;
	}
}
}
