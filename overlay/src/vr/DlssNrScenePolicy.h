#pragma once

namespace DlssNrScenePolicy
{
// Keep the saved ordinary DLSS mode, but never reduce the scene for NR.
inline int ResolveQualityMode(int savedMode, bool neuralEnabled)
{
	return neuralEnabled || savedMode < 0 || savedMode > 3 ? 0 : savedMode;
}

// Migrate legacy NR work-scale settings only if no model-scale key exists.
inline int ModelScaleDefault(int savedMode, bool neuralEnabled)
{
	return neuralEnabled ? ResolveQualityMode(savedMode, false) : 0;
}
}
