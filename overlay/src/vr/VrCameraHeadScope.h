#pragma once

#ifdef GTA_VR_OPENXR

// Changes only the submitted skin pose. Gameplay and subsequent cameras retain
// the original head, including the animated jaw and eye descendants.
class VrCameraHeadScope
{
	enum { MAX_BONES = 64 };
	RwMatrix *matrices;
	RwMatrix saved[MAX_BONES];
	int indices[MAX_BONES];
	int count;
	VrCameraHeadScope(const VrCameraHeadScope &) = delete;
	VrCameraHeadScope &operator=(const VrCameraHeadScope &) = delete;
public:
	explicit VrCameraHeadScope(CEntity *entity) : matrices(nil), count(0)
	{
		if(!entity || !OculusVR::ShouldHideCameraActorHead(entity) || !entity->m_rwObject ||
		   RwObjectGetType(entity->m_rwObject) != rpCLUMP ||
		   !IsClumpSkinned((RpClump*)entity->m_rwObject)) return;
		RpHAnimHierarchy *hierarchy = GetAnimHierarchyFromSkinClump((RpClump*)entity->m_rwObject);
		if(!hierarchy || !hierarchy->nodeInfo || hierarchy->numNodes <= 0 || hierarchy->numNodes > MAX_BONES) return;
		matrices = RpHAnimHierarchyGetMatrixArray(hierarchy);
		if(!matrices) return;
		bool inHead[MAX_BONES] = {};
		int stack[MAX_BONES];
		int parent = 0, depth = 0;
		for(int node = 0; node < hierarchy->numNodes; node++){
			const int flags = hierarchy->nodeInfo[node].flags;
			inHead[node] = (node > 0 && inHead[parent]) || hierarchy->nodeInfo[node].id == BONE_head;
			if(flags & rpHANIMPUSHPARENTMATRIX) stack[depth++] = parent;
			parent = node;
			if(flags & rpHANIMPOPPARENTMATRIX && depth > 0) parent = stack[--depth];
		}
		RwV3d zero = { 0.0f, 0.0f, 0.0f };
		for(int node = 0; node < hierarchy->numNodes; node++){
			if(!inHead[node]) continue;
			saved[count] = matrices[node];
			indices[count++] = node;
			RwMatrixScale(&matrices[node], &zero, rwCOMBINEPRECONCAT);
		}
	}
	~VrCameraHeadScope()
	{
		for(int i = 0; i < count; i++) matrices[indices[i]] = saved[i];
	}
};

#endif
