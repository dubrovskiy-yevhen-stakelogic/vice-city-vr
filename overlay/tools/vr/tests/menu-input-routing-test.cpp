#include "../../../src/vr/VrMenuInputRouting.h"
#include "../../../src/vr/MenuNavigation.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static unsigned int checks;
static unsigned int failures;
static void Check(bool value, const char *message)
{
	++checks;
	if(!value){
		++failures;
		std::printf("FAIL: %s\n", message);
	}
}

struct Pad
{
	int Start = 0, Select = 0, Cross = 0, RightShoulder2 = 0, Stick = 0;
	int Square = 0, Triangle = 0, Circle = 0, LeftShoulder1 = 0, LeftShoulder2 = 0;
	int RightShoulder1 = 0, DPadUp = 0, DPadDown = 0, DPadLeft = 0, DPadRight = 0;
	int LeftShock = 0, RightShock = 0, NetworkTalk = 0;
	bool CheckForInput()
	{
		return Start || Select || Cross || RightShoulder2 || Stick || Square || Triangle ||
			Circle || LeftShoulder1 || LeftShoulder2 || RightShoulder1 || DPadUp ||
			DPadDown || DPadLeft || DPadRight || LeftShock || RightShock || NetworkTalk;
	}
	void Clear() { *this = {}; }
};

// The engine merges this pad after ApplyTouchInput, regardless of its result.
struct InputFrame
{
	Pad pad;
	unsigned int controls = 0;
	bool session = true, running = true, sync = true, connected = true;
	bool shortcut = false, alternateShortcut = false, close = false;
};

struct RoutingFixture
{
	VrMenuInputRouting routing;
	bool visible = false;
	unsigned int toggles = 0, gameplayFrames = 0;
	bool Frame(InputFrame &frame)
	{
		if(!frame.session || !frame.running || !frame.sync || !frame.connected)
			return false;
		routing.BeginLegacyFrame(frame.pad, visible);
		const bool menu = (frame.controls & 1u) != 0;
		routing.BeginTouchFrame(menu, frame.controls);
		if(!visible && routing.WaitingForRelease()){
			routing.Consume(frame.pad);
			return true;
		}
		const bool toggle = routing.ToggleRequested(frame.shortcut,
			frame.alternateShortcut, menu, (frame.controls & 8u) != 0,
			(frame.controls & 32u) != 0);
		if(toggle){ visible = !visible; ++toggles; }
		if(visible){
			if(frame.close) visible = false;
			routing.Consume(frame.pad);
			return true;
		}
		if(toggle){ routing.Consume(frame.pad); return true; }
		++gameplayFrames;
		if(routing.AllowTouchPause()) frame.pad.Start = 255;
		frame.pad.Cross |= (frame.controls & 2u) ? 255 : 0;
		frame.pad.RightShoulder2 |= (frame.controls & 1024u) ? 255 : 0;
		return false;
	}
};

static void TestLegacyOwnership()
{
	RoutingFixture fixture;
	fixture.visible = true;
	InputFrame frame;
	frame.pad = {255,255,255,255,64};
	const int keyboardEscape = 255;
	Check(fixture.Frame(frame), "visible menu consumes frame");
	Check(!frame.pad.CheckForInput(), "inherited legacy controls cleared");
	Check(keyboardEscape == 255, "keyboard state is separate");
	fixture.visible = false;
	for(int i = 0; i < 50; ++i){
		frame.pad = {255,255,255,255,64};
		Check(fixture.Frame(frame), "legacy held close input remains owned");
		Check(!frame.pad.CheckForInput(), "no inherited Start/select/fire after close");
	}
	frame.pad.Clear();
	Check(!fixture.Frame(frame), "legacy neutral sample releases ownership");
	frame.pad.Start = 255;
	Check(!fixture.Frame(frame) && frame.pad.Start == 255,
		"deliberate legacy pause after release works");
}

static void TestShortcutJitter()
{
	for(unsigned int mode = 0; mode < 3; ++mode){
		RoutingFixture fixture;
		InputFrame frame;
		const unsigned int button = mode == 0 ? 1u : (mode == 1 ? 8u : 32u);
		frame.controls = button | 128u | 256u;
		if(mode) frame.controls |= 512u | 1024u;
		for(unsigned int i = 0; i < 1000; ++i){
			frame.shortcut = mode == 0 && (i % 3) == 0;
			frame.alternateShortcut = mode != 0 && (i % 3) == 0;
			frame.pad.Start = 255;
			fixture.Frame(frame);
			Check(fixture.visible && fixture.toggles == 1,
				"grip/trigger jitter cannot re-toggle a held shortcut button");
			Check(!frame.pad.CheckForInput(), "jitter cannot route ordinary pause");
		}
		frame = {};
		fixture.Frame(frame);
		frame.controls = button | 128u | 256u;
		frame.shortcut = mode == 0;
		frame.alternateShortcut = mode != 0;
		if(mode) frame.controls |= 512u | 1024u;
		Check(fixture.Frame(frame) && !fixture.visible && fixture.toggles == 2,
			"fresh deliberate chord closes menu");
		frame.shortcut = frame.alternateShortcut = false;
		frame.controls = button;
		Check(fixture.Frame(frame) && !frame.pad.CheckForInput(),
			"releasing grips before shortcut button cannot emit gameplay");
		frame = {};
		fixture.Frame(frame);
		frame.controls = 1;
		Check(!fixture.Frame(frame) && frame.pad.Start == 255,
			"intentional plain Touch Menu after complete release pauses");
	}
	VrMenuInputRouting route;
	Check(route.ToggleRequested(false, true, false, true, true),
		"alternate chord can contain both X and L3");
	Check(!route.ToggleRequested(false, true, false, false, true),
		"held L3 keeps ownership after X releases");
	Check(!route.ToggleRequested(false, false, false, false, false),
		"neutral cannot toggle");
	Check(route.ToggleRequested(false, true, false, true, false),
		"alternate chord rearms after its buttons release");
}

static void TestCloseAndReleaseOrders()
{
	for(unsigned int controls = 1; controls < 2048; ++controls){
		RoutingFixture fixture;
		fixture.visible = true;
		InputFrame frame;
		frame.controls = controls;
		frame.close = true;
		frame.pad = {255,255,255,255,0};
		Check(fixture.Frame(frame) && !fixture.visible,
			"back/select close frame consumed");
		Check(!frame.pad.CheckForInput(), "close frame has no legacy residue");
		frame.close = false;
		const unsigned int gameplayBefore = fixture.gameplayFrames;
		for(unsigned int bit = 0; bit < 11; ++bit){
			// Exercise both low-to-high and high-to-low partial release orders.
			const unsigned int index = controls & 1u ? bit : 10u-bit;
			if(frame.controls){
				fixture.Frame(frame);
				Check(fixture.gameplayFrames == gameplayBefore,
					"consumed buttons/grips/triggers never reach gameplay held");
				Check(!frame.pad.CheckForInput(), "no ghost Start/select/fire");
			}
			frame.controls &= ~(1u << index);
		}
		frame = {};
		Check(!fixture.Frame(frame), "complete release resumes gameplay");
		frame.controls = 1u | 2u | 1024u;
		Check(!fixture.Frame(frame) && frame.pad.Start && frame.pad.Cross && frame.pad.RightShoulder2,
			"fresh Menu/select/fire remain available");
	}
}

static void TestMissingInputSamples()
{
	for(int failure = 0; failure < 4; ++failure){
		RoutingFixture fixture;
		fixture.visible = true;
		InputFrame frame;
		frame.controls = 1u | 1024u;
		frame.close = true;
		fixture.Frame(frame);
		frame = {};
		if(failure == 0) frame.session = false;
		if(failure == 1) frame.running = false;
		if(failure == 2) frame.sync = false;
		if(failure == 3) frame.connected = false;
		for(int staleMenu = 0; staleMenu < 2; ++staleMenu){
			fixture.visible = staleMenu != 0;
			frame.pad = {255,255,255,255,64};
			Check(!fixture.Frame(frame), "missing input session does not claim legacy pad");
			Check(frame.pad.Start && frame.pad.Select && frame.pad.Cross && frame.pad.RightShoulder2 && frame.pad.Stick,
				"legacy passthrough despite stale UI flags when inactive");
			Check(fixture.routing.WaitingForRelease(),
				"failed sync is not mistaken for Touch release");
		}
		fixture.visible = false;
		frame = {};
		frame.controls = 1u | 1024u;
		Check(fixture.Frame(frame) && !frame.pad.CheckForInput(),
			"reconnected held closing controls remain blocked");
		frame = {};
		Check(!fixture.Frame(frame), "real neutral reconnect clears release barrier");
		frame.controls = 1;
		Check(!fixture.Frame(frame) && frame.pad.Start,
			"deliberate pause works after hotplug neutral");
	}
}

static void TestControlMaskAndScroll()
{
	{
		RoutingFixture driftFixture;
		driftFixture.visible = true;
		InputFrame frame;
		frame.controls = 4u;
		frame.close = true;
		frame.pad.Stick = 1;
		Check(driftFixture.Frame(frame) && !frame.pad.CheckForInput(),
			"visible UI consumes even a tiny inherited stick axis");
		frame.controls = 0;
		frame.close = false;
		frame.pad.Stick = 1;
		Check(!driftFixture.Frame(frame) && !driftFixture.routing.WaitingForRelease(),
			"legacy axis drift of 1 cannot trap Touch gameplay after close");
		frame.pad.Stick = 128;
		Check(!driftFixture.Frame(frame), "analog axes are not closing buttons");
	}
	Check(VrMenuInputRouting::Controls(false,false,false,false,false,false,false,0,0,0,0) == 0,
		"neutral mask");
	Check(VrMenuInputRouting::Controls(true,true,true,true,true,true,true,1,1,1,1) == 2047u,
		"all Touch controls captured");
	Check(VrMenuInputRouting::Controls(false,false,false,false,false,false,false,.44f,.44f,.44f,.44f) == 0,
		"analog release threshold");
	RoutingFixture fixture;
	fixture.visible = true;
	VrMenuNavigation ownedNavigation, ordinaryNavigation;
	int pulses = 0;
	for(unsigned long long time = 0; time < 5000; time += 10){
		InputFrame frame;
		frame.controls = 1u | 128u | 256u;
		frame.pad.Stick = 128;
		fixture.Frame(frame);
		const float axis = time % 30 == 0 ? .60f : .80f;
		const int actual = ownedNavigation.Pulse(axis, time);
		Check(actual == ordinaryNavigation.Pulse(axis, time),
			"ownership does not alter cumulative menu scrolling");
		pulses += actual != 0;
	}
	Check(pulses > 30, "held menu scrolling continues repeating");
}

static void TestProductionBindings(const char *sourcePath)
{
	std::ifstream input(sourcePath, std::ios::binary);
	Check(input.good(), "production OpenXRVR.cpp is readable");
	std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
	const auto begin = source.find("bool ApplyTouchInput(CControllerState *state)");
	Check(begin != std::string::npos, "production ApplyTouchInput exists");
	if(begin == std::string::npos) return;
	source = source.substr(begin);
	const auto owner = source.find("gVrMenuInputRouting.BeginLegacyFrame(*state,");
	Check(owner != std::string::npos, "production consumes inherited pad through tested helper");
	const char *earlyGuards[] = {
		"if(!state || !gSession || !gSessionRunning || !PollEvents()) return false;",
		"if(XR_FAILED(xrSyncActions(gSession, &sync))) return false;",
		"if(!connected){", "ResetImmersiveDrivingInteraction();\n\t\treturn false;"
	};
	for(const char *guard : earlyGuards)
		Check(source.find(guard) < owner, "legacy ownership follows session/sync/connection early exits");
	Check(source.find("gVrMenuInputRouting.BeginTouchFrame(menu, VrMenuInputRouting::Controls(\n\t\tmenu, a, b, x, y, leftStickClick, rightStickClick,\n\t\tleftGrip, rightGrip, leftTrigger, rightTrigger));") != std::string::npos,
		"production mask captures exact tested controls");
	const auto release = source.find("if(!gVrMenuVisible && !gCheatMenuVisible &&\n\t   gVrMenuInputRouting.WaitingForRelease()){");
	const auto gameplay = source.find("UpdateImmersiveBikeInput(physicalGrips,");
	Check(owner < release && release < gameplay,
		"release gate precedes driving/grab/fire routing but not visible navigation");
	Check(source.find("gVrMenuInputRouting.Consume(*state);\n\t\treturn true;", release) < gameplay,
		"release gate actually clears pad and returns");
	Check(source.find("const bool vrMenuToggled=gVrMenuInputRouting.ToggleRequested(\n\t\tlegacyVrMenuShortcut, alternateVrMenuShortcut, menu, x, leftStickClick);") != std::string::npos,
		"production menu toggle uses jitter-resistant helper");
	Check(source.find("HandleVrMenuInput(sticks[0], leftTrigger>=0.55f,\n\t\t\trightTrigger>=0.55f, a || rightStickClick,\n\t\t\tb || leftStickClick);\n\t\tgVrMenuInputRouting.Consume(*state);\n\t\treturn true;") != std::string::npos,
		"VR handler consumes its closing/back frame");
	Check(source.find("HandleCheatMenuInput(sticks[0], a || rightStickClick,\n\t\t\tb || leftStickClick);\n\t\tgVrMenuInputRouting.Consume(*state);\n\t\treturn true;") != std::string::npos,
		"cheat handler consumes its closing/back frame");
	Check(source.find("if(vrMenuToggled || cheatMenuToggled || neuralComparisonToggled){\n\t\tgVrMenuInputRouting.Consume(*state);\n\t\treturn true;") != std::string::npos,
		"shortcut closing frame consumes input");
	Check(source.find("MergeButton(state->Start, gVrMenuInputRouting.AllowTouchPause());") != std::string::npos &&
		source.find("MergeButton(state->Start,gVrMenuInputRouting.AllowTouchPause());") != std::string::npos,
		"both cutscene/gameplay pause sites use release-safe routing");
	Check(source.find("MergeButton(state->Start,menu)") == std::string::npos &&
		source.find("MergeButton(state->Start, menu)") == std::string::npos,
		"no raw Touch Menu-to-Start mapping remains");
}

int main(int argc, char **argv)
{
	if(argc != 2){
		std::printf("Usage: menu-input-routing-test.exe <overlay/src/vr/OpenXRVR.cpp>\n");
		return 2;
	}
	TestLegacyOwnership();
	TestShortcutJitter();
	TestCloseAndReleaseOrders();
	TestMissingInputSamples();
	TestControlMaskAndScroll();
	TestProductionBindings(argv[1]);
	std::printf("MENU_INPUT_ROUTING_COMPLETE checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}
