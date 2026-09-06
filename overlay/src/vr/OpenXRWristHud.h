// Included inside the OpenXR implementation after the copy/pose helpers.
// One optional atlas reuses the existing HUD render targets; the normal
// CLASSIC path does not create, render, copy or submit a wrist atlas.

#include "WristHudMath.h"

// Legacy flat-screen radar/font stretching is not a wrist-panel calibration.
// Use the same square-pixel layout for atlas bounds and atlas contents, then
// return the player's flat HUD settings unchanged.
struct WristHudLayoutScope
{
#ifdef FIX_RADAR
	bool oldFixRadar;
#endif
#ifdef PROPER_SCALING
	bool oldProperScaling;
#endif
	WristHudLayoutScope()
	{
#ifdef FIX_RADAR
		oldFixRadar = CDraw::ms_bFixRadar;
		CDraw::ms_bFixRadar = true;
#endif
#ifdef PROPER_SCALING
		oldProperScaling = CDraw::ms_bProperScaling;
		CDraw::ms_bProperScaling = true;
#endif
	}
	~WristHudLayoutScope()
	{
#ifdef FIX_RADAR
		CDraw::ms_bFixRadar = oldFixRadar;
#endif
#ifdef PROPER_SCALING
		CDraw::ms_bProperScaling = oldProperScaling;
#endif
	}
};

bool BuildWristVehicleAnchor(CMatrix *matrix)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || !gWristHud.inVehicle || !IsImmersiveDrivingActive())
		return false;
	CMatrix left, right;
	if(vehicle->IsBike()){
		BikeHandlePose pose;
		if(!BuildBikeHandlePose((CBike*)vehicle, &pose)) return false;
		if(BuildBikeHandleMatrixInternal(0, &left, false) &&
		   BuildBikeHandleMatrixInternal(1, &right, false))
			pose.center = (left.GetPosition()+right.GetPosition())*0.5f;
		matrix->SetUnity();
		matrix->GetPosition() = pose.center;
		matrix->GetRight() = pose.right;
		matrix->GetUp() = pose.up;
		matrix->GetForward() = pose.forward;
		return true;
	}
	if(!BuildCarWheelMatrixInternal(0, &left, false) ||
	   !BuildCarWheelMatrixInternal(1, &right, false)) return false;
	*matrix = left;
	matrix->GetPosition() = (left.GetPosition()+right.GetPosition())*0.5f;
	return true;
}

struct WristHudAnchor
{
	CVector position, right, up, forward;
};

struct WristHudFrame
{
	WristHudAnchor anchors[EYE_COUNT];
	unsigned int anchorMask = 0;
	int context = WristHudSettings::FOOT;
};

void CaptureWristHudAnchor(WristHudAnchor *anchor, const CMatrix &matrix)
{
	anchor->position = ToTrackingPosition(matrix.GetPosition());
	anchor->right = ToTrackingVector(matrix.GetRight());
	anchor->up = ToTrackingVector(matrix.GetUp());
	anchor->forward = ToTrackingVector(matrix.GetForward());
}

void CaptureWristHudFrame(WristHudFrame *frame)
{
	frame->anchorMask = 0;
	frame->context = WristHudSettings::FOOT;
	if(!gFramePrepared || !gGameplayHudVisible || IsTrackedScopeActive() ||
	   !ShouldRouteGameplayHudToVr() || gWristHudFailed) return;
	unsigned int requestedHands = 0;
	for(int panel = 0; panel < WristHudSettings::PANEL_COUNT; panel++){
		const int hand = gWristHud.hand[panel];
		if(gWristHud.enabled[panel] && hand >= 0 && hand < EYE_COUNT)
			requestedHands |= 1u << hand;
	}
	if(!requestedHands) return;
	CVehicle *vehicle = FindPlayerVehicle();
	if(vehicle){
		CMatrix anchor;
		if(!BuildWristVehicleAnchor(&anchor)) return;
		frame->context = vehicle->IsBike() ? WristHudSettings::BIKE : WristHudSettings::CAR;
		CaptureWristHudAnchor(&frame->anchors[0], anchor);
		frame->anchorMask = 1;
	}else{
		for(int hand = 0; hand < EYE_COUNT; hand++){
			if(!(requestedHands & (1u << hand))) continue;
			CMatrix anchor;
			if(!GetTrackedVisualHandMatrix(hand, &anchor, nil, nil)) continue;
			CaptureWristHudAnchor(&frame->anchors[hand], anchor);
			frame->anchorMask |= 1u << hand;
		}
	}
}

bool BuildWristHudLayer(const WristHudFrame &frame, int panel,
	XrCompositionLayerQuad *layer, bool *visible)
{
	*visible = true;
	const bool vehicle = frame.context != WristHudSettings::FOOT;
	const int hand = gWristHud.hand[panel];
	const int index = vehicle ? 0 : hand;
	if(index < 0 || index >= EYE_COUNT || !(frame.anchorMask & (1u << index))) return false;
	const int *placement = gWristHud.placement[panel][frame.context][gWristHud.underside[panel] ? 1 : 0];
	const WristHudAnchor &anchor = frame.anchors[index];
	CVector centre, right, up, normal;
	if(vehicle){
		right = anchor.right;
		up = anchor.up;
		normal = CrossProduct(right, up);
		centre = anchor.position+
			anchor.forward*(placement[WristHudSettings::ALONG]*0.001f)+
			right*(placement[WristHudSettings::ACROSS]*0.001f)+
			up*(placement[WristHudSettings::LIFT]*0.001f);
	}else{
		const float sign = hand ? -1.0f : 1.0f;
		const float face = gWristHud.underside[panel] ? -1.0f : 1.0f;
		const CVector side = anchor.right*sign;
		const CVector palm = anchor.up*sign;
		const CVector backward = anchor.forward*-1.0f;
		right = side*face;
		up = backward*-1.0f;
		normal = palm*face;
		centre = anchor.position+
			backward*(0.07f+placement[WristHudSettings::ALONG]*0.001f)+
			palm*face*(0.035f+placement[WristHudSettings::LIFT]*0.001f)+
			right*(placement[WristHudSettings::ACROSS]*0.001f);
		if(panel == WristHudSettings::AMMO && hand == 0) right *= -1.0f;
	}
	if(!ApplyWristPanelRotation(right, up, normal,
		DEGTORAD(placement[WristHudSettings::PITCH]*0.1f),
		DEGTORAD(placement[WristHudSettings::YAW]*0.1f),
		DEGTORAD(placement[WristHudSettings::ROLL]*0.1f))) return false;
	if(!_finite(centre.x) || !_finite(centre.y) || !_finite(centre.z)) return false;
	if(gWristHud.gaze && !gVrHudMenuVisible){
		const XrVector3f &l = gLocatedViews[0].pose.position;
		const XrVector3f &r = gLocatedViews[1].pose.position;
		const CVector eye((l.x+r.x)*0.5f, (l.y+r.y)*0.5f, (l.z+r.z)*0.5f);
		CVector delta = centre-eye;
		const float distance = delta.Magnitude();
		const XrVector3f look = Rotate(gLocatedViews[0].pose.orientation, {0.0f,0.0f,-1.0f});
		if(distance > gWristHud.gazeRangeCm*0.01f || (distance > 0.01f &&
		   DotProduct(delta*(1.0f/distance), CVector(look.x,look.y,look.z)) < 0.83f))
			*visible = false;
	}
	float left, top, width, height;
	if(!GetVrWristHudPanelRect(panel, &left, &top, &width, &height)) return false;
	const int x = (int)floorf(left), y = (int)floorf(top);
	const int w = (int)ceilf(width), h = (int)ceilf(height);
	if(x < 0 || y < 0 || w <= 0 || h <= 0 ||
	   x+w > VR_HUD_WIDTH || y+h > VR_HUD_HEIGHT) return false;
	*layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer->space = gGameplaySpace;
	layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer->subImage.swapchain = gWristHudSwapchain.handle;
	layer->subImage.imageRect.offset = {x,y};
	layer->subImage.imageRect.extent = {w,h};
	layer->pose.position = {centre.x,centre.y,centre.z};
	layer->pose.orientation = WristQuaternion(right, up);
	static const float metres[] = {0.115f,0.130f,0.055f,0.070f};
	layer->size.width = metres[panel]*placement[WristHudSettings::SIZE]*0.01f;
	// The radar disc is round even when legacy HUD scaling makes its atlas
	// rectangle non-square. Text panels retain their own content proportions.
	layer->size.height = panel == WristHudSettings::MAP ? layer->size.width :
		layer->size.width*height/width;
	return true;
}

void PrepareWristHud(const WristHudFrame &frame)
{
	gWristHud.routingMask = 0;
	gWristHudLayerMask = 0;
	if(!frame.anchorMask || !gGameplayHudVisible || IsTrackedScopeActive() ||
	   !ShouldRouteGameplayHudToVr() || gWristHudFailed) return;
	bool any = false;
	for(int panel = 0; panel < WristHudSettings::PANEL_COUNT; panel++)
		any |= gWristHud.enabled[panel];
	if(!any) return;
	if(!gWristHudSwapchain.handle &&
	   !CreateSwapchain(gWristHudSwapchain, VR_HUD_WIDTH, VR_HUD_HEIGHT)){
		gWristHudFailed = true;
		VrLog("Wrist HUD atlas creation failed; keeping classic HUD\n");
		return;
	}
	const WristHudLayoutScope layout;
	for(int panel = 0; panel < WristHudSettings::PANEL_COUNT; panel++){
		if(!gWristHud.enabled[panel]) continue;
		// A gaze-hidden panel stays off the classic HUD as on standalone.
		bool visible = false;
		if(BuildWristHudLayer(frame, panel, &gWristHudLayers[panel], &visible)){
			gWristHud.routingMask |= 1u << panel;
			if(visible) gWristHudLayerMask |= 1u << panel;
		}
	}
}

void UpdateWristHudAtlas(RwCamera *camera)
{
	if(!gWristHudLayerMask) return;
	RwRGBA transparent = {0,0,0,0};
	RwCameraClear(camera, &transparent, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
	if(!RwCameraBeginUpdate(camera)){
		gWristHudLayerMask = 0;
		gWristHudFailed = true;
		return;
	}
	{
		const WristHudLayoutScope layout;
		RenderVrWristHudContents(gWristHudLayerMask);
	}
	RwCameraEndUpdate(camera);
	if(!CopyRasterToSwapchain(gHudColor, VR_HUD_WIDTH, VR_HUD_HEIGHT,
		gWristHudSwapchain,
#ifdef RW_D3D12
		0
#else
		GL_NEAREST
#endif
	)){
		gWristHudLayerMask = 0;
		gWristHudFailed = true;
		VrLog("Wrist HUD atlas copy failed; classic HUD restored next frame\n");
	}
}
