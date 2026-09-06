#pragma once

struct VrMenuInputRouting
{
	unsigned int shortcutButtons = 0;
	unsigned int touchControls = 0;
	unsigned int consumedControls = 0;
	bool touchPauseDown = false;
	bool legacyPauseDown = false;
	bool touchPauseBlocked = false;
	bool legacyPauseBlocked = false;
	bool legacyInputDown = false;
	bool legacyReleasePending = false;

	static unsigned int Controls(bool menu, bool a, bool b, bool x, bool y,
		bool leftClick, bool rightClick, float leftGrip, float rightGrip,
		float leftTrigger, float rightTrigger)
	{
		return (menu ? 1u : 0u) | (a ? 2u : 0u) | (b ? 4u : 0u) |
			(x ? 8u : 0u) | (y ? 16u : 0u) | (leftClick ? 32u : 0u) |
			(rightClick ? 64u : 0u) | (leftGrip >= 0.45f ? 128u : 0u) |
			(rightGrip >= 0.45f ? 256u : 0u) |
			(leftTrigger >= 0.45f ? 512u : 0u) |
			(rightTrigger >= 0.45f ? 1024u : 0u);
	}

	template <typename Pad> void BeginLegacyFrame(Pad &pad, bool uiOwnsInput)
	{
		legacyPauseDown = pad.Start != 0;
		// Axes need no release latch: a small XInput drift must not hold all Touch
		// gameplay behind the closing-button gate. The visible UI still clears them.
		legacyInputDown = pad.Start || pad.Select || pad.Square || pad.Triangle ||
			pad.Cross || pad.Circle || pad.LeftShoulder1 || pad.LeftShoulder2 ||
			pad.RightShoulder1 || pad.RightShoulder2 || pad.DPadUp || pad.DPadDown ||
			pad.DPadLeft || pad.DPadRight || pad.LeftShock || pad.RightShock || pad.NetworkTalk;
		if(!legacyInputDown) legacyReleasePending = false;
		if(!legacyPauseDown) legacyPauseBlocked = false;
		if(uiOwnsInput && legacyPauseDown) legacyPauseBlocked = true;
		if(uiOwnsInput && legacyInputDown) legacyReleasePending = true;
		if(legacyPauseBlocked) pad.Start = 0;
		if(uiOwnsInput || legacyReleasePending) pad.Clear();
	}

	void BeginTouchFrame(bool menu, unsigned int controls)
	{
		touchPauseDown = menu;
		if(!menu) touchPauseBlocked = false;
		touchControls = controls;
		consumedControls &= controls;
	}

	template <typename Pad> void Consume(Pad &pad)
	{
		touchPauseBlocked |= touchPauseDown;
		legacyPauseBlocked |= legacyPauseDown;
		legacyReleasePending |= legacyInputDown;
		consumedControls |= touchControls;
		pad.Clear();
	}

	bool WaitingForRelease() const
	{
		return consumedControls != 0 || legacyReleasePending;
	}

	bool AllowTouchPause() const { return touchPauseDown && !touchPauseBlocked; }

	bool ToggleRequested(bool legacyShortcut, bool alternateShortcut,
		bool menu, bool x, bool leftStickClick)
	{
		const unsigned int buttons = (menu ? 1u : 0u) | (x ? 2u : 0u) |
			(leftStickClick ? 4u : 0u);
		// A grip or trigger crossing its threshold cannot re-arm a held shortcut.
		// Release its Menu/X/L3 button before deliberately toggling again.
		shortcutButtons &= buttons;
		if(shortcutButtons) return false;
		const unsigned int pressed = legacyShortcut ? 1u :
			alternateShortcut ? buttons & 6u : 0u;
		if(!pressed) return false;
		shortcutButtons = pressed;
		return true;
	}
};
