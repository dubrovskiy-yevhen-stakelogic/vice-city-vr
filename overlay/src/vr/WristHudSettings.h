#pragma once

#include <windows.h>
#include <stdio.h>
#include <string.h>

// Distances are tenths of a centimetre and angles tenths of a degree,
// matching the standalone HUD calibration keys.
struct WristHudSettings
{
	enum { MAP, STATUS, CLOCK, AMMO, PANEL_COUNT };
	enum { FOOT, CAR, BIKE, CONTEXT_COUNT };
	enum { ALONG, ACROSS, LIFT, PITCH, YAW, ROLL, SIZE, FIELD_COUNT };
	bool enabled[PANEL_COUNT] = {};
	bool underside[PANEL_COUNT] = { true, true, false, false };
	int hand[PANEL_COUNT] = { 0, 1, 1, 1 };
	int placement[PANEL_COUNT][CONTEXT_COUNT][2][FIELD_COUNT] = {};
	bool owned[PANEL_COUNT][CONTEXT_COUNT] = {};
	bool gaze = true;
	int gazeRangeCm = 60;
	bool inVehicle = false;
	bool classicWeapon = true;
	bool classicClock = true;
	int ammoColour[3] = { 90, 235, 120 };
	int panel = 0;
	int context = 0;
	bool editing = false;
	unsigned routingMask = 0;
	char settingsPath[MAX_PATH] = {};

	static int Bound(int value, int low, int high)
	{
		return value < low ? low : value > high ? high : value;
	}
	static const char *PanelName(int value)
	{
		static const char *const names[] = { "MINIMAP", "STATUS", "CLOCK", "AMMO" };
		return names[Bound(value, 0, PANEL_COUNT-1)];
	}
	static const char *PanelKey(int value)
	{
		static const char *const names[] = { "", "Status", "Clock", "Ammo" };
		return names[Bound(value, 0, PANEL_COUNT-1)];
	}
	static const char *EnableKey(int value)
	{
		static const char *const names[] = { "WristRadar", "WristStatus", "WristClock", "WristAmmo" };
		return names[Bound(value, 0, PANEL_COUNT-1)];
	}
	static const char *ContextKey(int value)
	{
		static const char *const names[] = { "", "Car", "Bike" };
		return names[Bound(value, 0, CONTEXT_COUNT-1)];
	}
	static const char *ContextName(int value)
	{
		static const char *const names[] = { "ON FOOT", "IN A CAR", "ON A BIKE" };
		return names[Bound(value, 0, CONTEXT_COUNT-1)];
	}
	static const char *FieldName(int field)
	{
		static const char *const names[] = { "Along", "Across", "Lift", "Pitch", "Yaw", "Roll", "Size" };
		return names[Bound(field, 0, FIELD_COUNT-1)];
	}
	static int DefaultValue(int panel, int context, int side, int field)
	{
		static const int foot[PANEL_COUNT][2][FIELD_COUNT] = {
			{{-61,-28,-25,95,-955,1200,55},{-55,-33,-52,95,-955,-539,55}},
			{{-62,-26,-16,30,-867,-1521,100},{-62,-32,-16,30,-803,-132,100}},
			{{-63,-28,-62,-100,-968,-1632,100},{-55,-33,-52,95,-955,-539,100}},
			{{-30,-20,-30,0,-900,0,70},{-30,-20,-30,0,-900,0,70}}
		};
		static const int vehicle[2][PANEL_COUNT][FIELD_COUNT] = {
			{{-74,-96,23,0,0,0,55},{-74,76,33,0,0,0,100},
			 {-74,97,-9,0,0,0,100},{-74,-96,-20,0,0,0,70}},
			{{-22,-118,50,0,0,0,85},{0,-1,80,0,0,0,100},
			 {0,0,105,0,0,0,100},{0,0,55,0,0,0,70}}
		};
		return context == FOOT ? foot[panel][side][field] : vehicle[context-1][panel][field];
	}
	void MakePlacementKey(char *key, int p, int c, int side, int field) const
	{
		snprintf(key, 96, "Wrist%s%s%s%s", PanelKey(p), ContextKey(c),
			side ? "Inner" : "Outer", FieldName(field));
	}
	int Read(const char *key, int fallback, int low, int high) const
	{
		return Bound((int)GetPrivateProfileIntA("VR", key, fallback, settingsPath), low, high);
	}
	void Save(const char *key, int value) const
	{
		char text[24];
		snprintf(text, sizeof(text), "%d", value);
		WritePrivateProfileStringA("VR", key, text, settingsPath);
	}
	void Load(const char *path)
	{
		strncpy(settingsPath, path, sizeof(settingsPath)-1);
		settingsPath[sizeof(settingsPath)-1] = 0;
		for(int p = 0; p < PANEL_COUNT; p++){
			char key[96];
			enabled[p] = Read(EnableKey(p), 0, 0, 1) != 0;
			snprintf(key, sizeof(key), "%sUnderside", EnableKey(p));
			underside[p] = Read(key, p < 2 ? 1 : 0, 0, 1) != 0;
			snprintf(key, sizeof(key), "%sHand", EnableKey(p));
			hand[p] = Read(key, p == MAP ? 0 : 1, 0, 1);
			for(int c = 0; c < CONTEXT_COUNT; c++){
				snprintf(key, sizeof(key), "Wrist%s%sCustom", PanelKey(p), ContextKey(c));
				owned[p][c] = c == FOOT || Read(key, 0, 0, 1) != 0;
				for(int side = 0; side < 2; side++)
					for(int field = 0; field < FIELD_COUNT; field++){
						MakePlacementKey(key, p, c, side, field);
						const int fallback = DefaultValue(p, c, side, field);
						placement[p][c][side][field] = owned[p][c] ?
							Read(key, fallback, field == SIZE ? 40 : field >= PITCH ? -1800 : -300,
								field == SIZE ? 250 : field >= PITCH ? 1800 : 300) : fallback;
					}
			}
		}
		gaze = Read("WristPanelGazeReveal", 1, 0, 1) != 0;
		gazeRangeCm = Read("WristPanelGazeRangeCm", 60, 20, 200);
		inVehicle = Read("WristPanelsInVehicle", 0, 0, 1) != 0;
		classicWeapon = Read("HudWeaponPanel", 1, 0, 1) != 0;
		classicClock = Read("HudClock", 1, 0, 1) != 0;
		static const char *const colourKeys[] = { "AmmoColourRed", "AmmoColourGreen", "AmmoColourBlue" };
		for(int channel = 0; channel < 3; channel++)
			ammoColour[channel] = Read(colourKeys[channel], ammoColour[channel], 0, 255);
	}
	void SetPreset(bool immersive)
	{
		for(int p = 0; p < PANEL_COUNT; p++){
			enabled[p] = immersive;
			Save(EnableKey(p), enabled[p]);
		}
		classicWeapon = classicClock = !immersive;
		inVehicle = immersive;
		gaze = immersive;
		Save("HudWeaponPanel", classicWeapon);
		Save("HudClock", classicClock);
		Save("WristPanelsInVehicle", inVehicle);
		Save("WristPanelGazeReveal", gaze);
	}
	const char *PresetName() const
	{
		int count = 0;
		for(int p = 0; p < PANEL_COUNT; p++) count += enabled[p] ? 1 : 0;
		if(count == 0 && classicWeapon && classicClock) return "CLASSIC";
		if(count == PANEL_COUNT && !classicWeapon && !classicClock) return "IMMERSIVE";
		return "CUSTOM";
	}
	void ChangePlacement(int field, int step)
	{
		int &value = placement[panel][context][underside[panel] ? 1 : 0][field];
		value = Bound(value+step, field == SIZE ? 40 : field >= PITCH ? -1800 : -300,
			field == SIZE ? 250 : field >= PITCH ? 1800 : 300);
		char key[96];
		MakePlacementKey(key, panel, context, underside[panel] ? 1 : 0, field);
		Save(key, value);
		if(!owned[panel][context]){
			owned[panel][context] = true;
			snprintf(key, sizeof(key), "Wrist%s%sCustom", PanelKey(panel), ContextKey(context));
			Save(key, 1);
		}
	}
	void ResetPlacement(bool copyOtherSide)
	{
		const int side = underside[panel] ? 1 : 0;
		for(int field = 0; field < FIELD_COUNT; field++){
			placement[panel][context][side][field] = copyOtherSide ?
				placement[panel][context][1-side][field] : DefaultValue(panel, context, side, field);
			ChangePlacement(field, 0);
		}
	}
};
