#include <cassert>
#include <cstdio>
#include "../../../src/vr/VrPadBindings.h"

using namespace VrPadBindings;
static void ExpectEmpty(const int (&output)[TARGET_COUNT])
{
	for(int target = 0; target < TARGET_COUNT; target++) assert(output[target] == 0);
}
static void LegacyDefaultReference(const int *input, const Context &context, int (&output)[TARGET_COUNT])
{
	for(int field = 0; field < TARGET_COUNT; field++) output[field] = 0;
	int values[SOURCE_COUNT];
	for(int source = 0; source < SOURCE_COUNT; source++)
		values[source] = (context.capturedSources & (1u << source)) ? 0 : input[source];
	const auto merge = [&](int field, int value){ if(value > output[field]) output[field] = value; };
	if(context.padMode != 2) merge(CROSS, values[A]);
	if(context.padMode != 0 && context.padMode != 1) merge(CIRCLE, values[B]);
	merge(SQUARE, values[X]); merge(TRIANGLE, values[Y]);
	merge(L1, values[LEFT_GRIP]);
	if(context.padMode != 3) merge(R1, values[RIGHT_GRIP]);
	if(context.stickCrouch) merge(L3, values[LEFT_CLICK]);
	merge(context.padMode == 2 ? CIRCLE : CROSS, values[RIGHT_CLICK]);
}
int main()
{
	Bindings bindings;
	assert(bindings.GetLayout() == DEFAULT);
	assert(DefaultTarget(-1) == NONE && DefaultTarget(SOURCE_COUNT) == NONE);
	bindings.Set(A, -1); assert(bindings.target[A] == CROSS);
	bindings.Set(A, TARGET_COUNT); assert(bindings.target[A] == CROSS);
	bindings.Set(-1, CIRCLE); bindings.Set(SOURCE_COUNT, CIRCLE);
	bindings.Apply(SWAPPED_HANDS); assert(bindings.GetLayout() == SWAPPED_HANDS);
	assert(bindings.target[A] == SQUARE && bindings.target[B] == TRIANGLE);
	assert(bindings.target[X] == CROSS && bindings.target[Y] == CIRCLE);
	bindings.Set(A, NONE); assert(bindings.GetLayout() == CUSTOM);
	bindings.Apply(DEFAULT);
	Context context;
	context.gameplay = context.onFoot = context.stickCrouch = true;
	int values[SOURCE_COUNT] = {};
	int output[TARGET_COUNT];
	for(int mode = 0; mode < 4; mode++) for(int crouch = 0; crouch < 2; crouch++)
		for(int mask = 0; mask < 4; mask++) for(int presses = 0; presses < 256; presses++){
			context.padMode = mode; context.stickCrouch = crouch != 0;
			context.capturedSources = ((mask & 1) ? 1u << LEFT_GRIP : 0) |
				((mask & 2) ? 1u << RIGHT_GRIP : 0);
			for(int source = 0; source < SOURCE_COUNT; source++)
				values[source] = presses & (1 << source) ? (source == LEFT_GRIP ? 140 : 255) : 0;
			int expected[TARGET_COUNT];
			LegacyDefaultReference(values, context, expected);
			Resolve(bindings, values, context, output);
			for(int target = 0; target < TARGET_COUNT; target++) assert(output[target] == expected[target]);
		}
	context.capturedSources = 0; context.stickCrouch = true;
	for(int source = 0; source < SOURCE_COUNT; source++) values[source] = 0;
	// Every source can reach every target. Fire actions are reserved for tracked
	// weapons; SPRINT retains the existing PC control-mode-dependent action.
	for(int mode = 0; mode < 4; mode++){
		context.padMode = mode;
		for(int source = 0; source < SOURCE_COUNT; source++){
			for(int target = 0; target < TARGET_COUNT; target++){
				bindings.Set(source, target); values[source] = 173;
				Resolve(bindings, values, context, output);
				const int resolved = target == SPRINT ? (mode == 2 ? CIRCLE : CROSS) : target;
				const bool suppressed = target == NONE || (target != SPRINT && target == FireTarget(mode));
				for(int field = 0; field < TARGET_COUNT; field++)
					assert(output[field] == (!suppressed && field == resolved ? 173 : 0));
				context.capturedSources = 1u << source;
				Resolve(bindings, values, context, output); ExpectEmpty(output);
				context.capturedSources = 0;
				context.capturedTargets = 1u << resolved;
				Resolve(bindings, values, context, output); ExpectEmpty(output);
				context.capturedTargets = 0; values[source] = 0;
			}
			bindings.Set(source, DefaultTarget(source));
		}
	}
	context.padMode = 0;
	bindings.Set(A, SQUARE); bindings.Set(B, SQUARE);
	values[A] = 90; values[B] = 200;
	Resolve(bindings, values, context, output); assert(output[SQUARE] == 200);
	values[A] = 300; values[B] = -20;
	Resolve(bindings, values, context, output); assert(output[SQUARE] == 255);
	context.consumed = true; Resolve(bindings, values, context, output); ExpectEmpty(output);
	context.consumed = false; context.remote = true;
	Resolve(bindings, values, context, output); ExpectEmpty(output);
	context.remote = false; context.onFoot = false;
	Resolve(bindings, values, context, output); ExpectEmpty(output);
	context.onFoot = true; context.gameplay = false;
	Resolve(bindings, values, context, output); ExpectEmpty(output);
	context.gameplay = true;
	Resolve(bindings, nullptr, context, output); ExpectEmpty(output);
	bindings.Apply(DEFAULT);
	for(int source = 0; source < SOURCE_COUNT; source++) values[source] = 0;
	values[LEFT_CLICK] = 255; context.stickCrouch = false;
	Resolve(bindings, values, context, output); ExpectEmpty(output);
	context.stickCrouch = true;
	Resolve(bindings, values, context, output); assert(output[L3] == 255);
	context.stickCrouch = false; bindings.Set(LEFT_CLICK, SQUARE);
	Resolve(bindings, values, context, output); assert(output[SQUARE] == 255);
	std::puts("pad bindings: PASS (8192 legacy comparisons, 384 mappings, capture masks, presets, collisions, context gates, bounds)");
}
