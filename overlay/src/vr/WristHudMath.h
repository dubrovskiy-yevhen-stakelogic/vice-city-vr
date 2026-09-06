#pragma once

inline void RotateWristAxes(CVector &first, CVector &second, float angle)
{
	const CVector a = first, b = second;
	first = a*cosf(angle)+b*sinf(angle);
	second = b*cosf(angle)-a*sinf(angle);
}

inline bool ApplyWristPanelRotation(CVector &right, CVector &up, CVector normal,
	float pitch, float yaw, float roll)
{
	RotateWristAxes(normal, right, yaw);
	RotateWristAxes(up, normal, pitch);
	RotateWristAxes(right, up, roll);
	const float rightLength = right.MagnitudeSqr();
	if(!(rightLength >= 0.0001f && rightLength <= FLT_MAX)) return false;
	right.Normalise();
	up -= right*DotProduct(up, right);
	const float upLength = up.MagnitudeSqr();
	if(!(upLength >= 0.0001f && upLength <= FLT_MAX)) return false;
	up.Normalise();
	return true;
}

inline XrQuaternionf WristQuaternion(const CVector &right, const CVector &up)
{
	const CVector back = CrossProduct(right, up);
	const float m00 = right.x, m11 = up.y, m22 = back.z;
	XrQuaternionf q = {};
	const float trace = m00+m11+m22;
	if(trace > 0.0f){
		const float scale = sqrtf(trace+1.0f)*2.0f;
		q.w = 0.25f*scale;
		q.x = (up.z-back.y)/scale;
		q.y = (back.x-right.z)/scale;
		q.z = (right.y-up.x)/scale;
	}else if(m00 > m11 && m00 > m22){
		const float scale = sqrtf(1.0f+m00-m11-m22)*2.0f;
		q.w = (up.z-back.y)/scale;
		q.x = 0.25f*scale;
		q.y = (up.x+right.y)/scale;
		q.z = (back.x+right.z)/scale;
	}else if(m11 > m22){
		const float scale = sqrtf(1.0f+m11-m00-m22)*2.0f;
		q.w = (back.x-right.z)/scale;
		q.x = (up.x+right.y)/scale;
		q.y = 0.25f*scale;
		q.z = (back.y+up.z)/scale;
	}else{
		const float scale = sqrtf(1.0f+m22-m00-m11)*2.0f;
		q.w = (right.y-up.x)/scale;
		q.x = (back.x+right.z)/scale;
		q.y = (back.y+up.z)/scale;
		q.z = 0.25f*scale;
	}
	XrQuaternionf normalized;
	if(NormalizeXrQuaternion(q, &normalized)) return normalized;
	q = {}; q.w = 1.0f;
	return q;
}
