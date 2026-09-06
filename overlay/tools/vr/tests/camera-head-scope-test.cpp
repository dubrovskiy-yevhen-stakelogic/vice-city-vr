#include <cassert>
#include <cstdio>
#include <initializer_list>

#define GTA_VR_OPENXR
#define nil nullptr
enum { rpCLUMP = 2, BONE_head = 12, rpHANIMPUSHPARENTMATRIX = 1,
       rpHANIMPOPPARENTMATRIX = 2, rwCOMBINEPRECONCAT = 0 };
struct RwMatrix { float value; };
struct RwV3d { float x, y, z; };
struct NodeInfo { int id, flags; };
struct RpHAnimHierarchy { int numNodes; NodeInfo *nodeInfo; RwMatrix *matrices; };
struct RpClump { int type; bool skinned; RpHAnimHierarchy *hierarchy; };
struct CEntity { RpClump *m_rwObject; };
static CEntity *target;
namespace OculusVR { bool ShouldHideCameraActorHead(CEntity *entity) { return entity == target; } }
static int RwObjectGetType(RpClump *clump) { return clump->type; }
static bool IsClumpSkinned(RpClump *clump) { return clump->skinned; }
static RpHAnimHierarchy *GetAnimHierarchyFromSkinClump(RpClump *clump) { return clump->hierarchy; }
static RwMatrix *RpHAnimHierarchyGetMatrixArray(RpHAnimHierarchy *hierarchy) { return hierarchy->matrices; }
static void RwMatrixScale(RwMatrix *matrix, RwV3d *, int) { matrix->value = 0; }
#include "../../../src/vr/VrCameraHeadScope.h"

int main()
{
    // root -> head -> jaw / eye, followed by a non-head sibling arm.
    NodeInfo nodes[64] = { {0, 0}, {BONE_head, rpHANIMPUSHPARENTMATRIX},
        {13, rpHANIMPUSHPARENTMATRIX | rpHANIMPOPPARENTMATRIX},
        {14, rpHANIMPOPPARENTMATRIX}, {15, 0} };
    RwMatrix matrices[64];
    for(int i = 0; i < 64; i++) matrices[i].value = float(i+1);
    RpHAnimHierarchy hierarchy = {5, nodes, matrices};
    RpClump clump = {rpCLUMP, true, &hierarchy};
    CEntity actor = {&clump}, other = {&clump};
    target = &actor;
    {
        VrCameraHeadScope scope(&other);
        assert(matrices[1].value == 2);
    }
    {
        VrCameraHeadScope scope(&actor);
        assert(matrices[0].value == 1 && matrices[4].value == 5);
        assert(matrices[1].value == 0 && matrices[2].value == 0 && matrices[3].value == 0);
    }
    for(int i = 0; i < 64; i++) assert(matrices[i].value == float(i+1));
    for(int badCount : {0, 65}) {
        hierarchy.numNodes = badCount;
        VrCameraHeadScope scope(&actor);
        assert(matrices[1].value == 2);
    }
    hierarchy.numNodes = 64;
    for(int i = 0; i < 64; i++) nodes[i] = {i == 0 ? BONE_head : i+100, rpHANIMPUSHPARENTMATRIX};
    {
        VrCameraHeadScope scope(&actor);
        for(int i = 0; i < 64; i++) assert(matrices[i].value == 0);
    }
    for(int i = 0; i < 64; i++) assert(matrices[i].value == float(i+1));
    hierarchy.nodeInfo = nullptr;
    { VrCameraHeadScope scope(&actor); }
    actor.m_rwObject = nullptr;
    { VrCameraHeadScope scope(&actor); }
    target = nullptr;
    { VrCameraHeadScope scope(nullptr); }
    std::puts("camera head scope: PASS (branch, restore, target, null, 0/64/65-node bounds)");
}
