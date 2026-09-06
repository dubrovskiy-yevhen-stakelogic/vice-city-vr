#pragma once

class CVector;

namespace VRHandModel
{
// Releases RenderWare-owned hand resources while the RW device is still alive.
// The meshes can be loaded again after an in-process game restart.
void Shutdown(void);

// Renders the baked UltimateXR hand for the requested OpenXR hand (0 = left,
// 1 = right). Returns false when an asset cannot be loaded so the caller can
// keep the existing procedural hand as a safe fallback.
bool Render(int hand, const CVector &position, const CVector &right,
	const CVector &up, const CVector &forward, float grip, float trigger);
}
