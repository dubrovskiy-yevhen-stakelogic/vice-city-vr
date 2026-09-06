#include "common.h"

#include "VrRagdoll.h"
#include "Bones.h"
#include "Camera.h"
#include "Particle.h"
#include "Ped.h"
#include "RwHelper.h"
#include "Timer.h"
#include "World.h"

// A dying ped's joints become verlet particles seeded from the pose and
// velocity the death interrupted, fall under gravity against the local
// ground plane, and are held together by distance constraints. Each frame
// the world-space hierarchy matrices -- which is what the skin pipeline
// consumes -- are rebuilt from the particles: a simulated bone keeps its
// death-pose basis rotated by the shortest arc from its death-pose segment
// direction to the current one, and every node the simulation does not
// carry (root, fingers) rides its nearest simulated ancestor rigidly. At
// the moment of death the rebuilt pose equals the animated one exactly, so
// the takeover is seamless.

namespace VrRagdoll
{

enum {
	MAX_RAGDOLLS = 6,
	MAX_NODES = 40,
	// Fixed-step simulation: variable steps make verlet constraints
	// breathe with the frame rate.
	MAX_STEPS = 3,
};

enum {
	RD_PELVIS = 0, RD_SPINE, RD_SPINE1, RD_NECK, RD_HEAD,
	RD_LCLAV, RD_LUARM, RD_LFARM, RD_LHAND,
	RD_RCLAV, RD_RUARM, RD_RFARM, RD_RHAND,
	RD_LTHIGH, RD_LCALF, RD_LFOOT,
	RD_RTHIGH, RD_RCALF, RD_RFOOT,
	SIM_BONES
};

static const int simBoneTags[SIM_BONES] = {
	BONE_pelvis, BONE_spine, BONE_spine1, BONE_neck, BONE_head,
	BONE_l_clavicle, BONE_l_upperarm, BONE_l_forearm, BONE_l_hand,
	BONE_r_clavicle, BONE_r_upperarm, BONE_r_forearm, BONE_r_hand,
	BONE_l_thigh, BONE_l_calf, BONE_l_foot,
	BONE_r_thigh, BONE_r_calf, BONE_r_foot
};

// The segment whose direction carries each bone's orientation. End bones
// reuse the segment that arrives at them, so their twist follows the limb.
static const uint8 dirFrom[SIM_BONES] = {
	RD_PELVIS, RD_SPINE, RD_SPINE1, RD_NECK, RD_NECK,
	RD_LCLAV, RD_LUARM, RD_LFARM, RD_LFARM,
	RD_RCLAV, RD_RUARM, RD_RFARM, RD_RFARM,
	RD_LTHIGH, RD_LCALF, RD_LCALF,
	RD_RTHIGH, RD_RCALF, RD_RCALF
};
static const uint8 dirTo[SIM_BONES] = {
	RD_SPINE1, RD_SPINE1, RD_NECK, RD_HEAD, RD_HEAD,
	RD_LUARM, RD_LFARM, RD_LHAND, RD_LHAND,
	RD_RUARM, RD_RFARM, RD_RHAND, RD_RHAND,
	RD_LCALF, RD_LFOOT, RD_LFOOT,
	RD_RCALF, RD_RFOOT, RD_RFOOT
};

struct Stick {
	uint8 a, b;
	// Zero holds the rest length rigidly. Non-zero is a minimum-only
	// stick: it never pulls in, it just refuses to let the two joints
	// approach closer than that percentage of rest -- the cheap
	// stand-in for angular limits.
	uint8 minPercent;
};

static const Stick sticks[] = {
	// the skeleton edges
	{ RD_PELVIS, RD_SPINE, 0 }, { RD_SPINE, RD_SPINE1, 0 },
	{ RD_SPINE1, RD_NECK, 0 }, { RD_NECK, RD_HEAD, 0 },
	{ RD_SPINE1, RD_LCLAV, 0 }, { RD_LCLAV, RD_LUARM, 0 },
	{ RD_LUARM, RD_LFARM, 0 }, { RD_LFARM, RD_LHAND, 0 },
	{ RD_SPINE1, RD_RCLAV, 0 }, { RD_RCLAV, RD_RUARM, 0 },
	{ RD_RUARM, RD_RFARM, 0 }, { RD_RFARM, RD_RHAND, 0 },
	{ RD_PELVIS, RD_LTHIGH, 0 }, { RD_LTHIGH, RD_LCALF, 0 },
	{ RD_LCALF, RD_LFOOT, 0 },
	{ RD_PELVIS, RD_RTHIGH, 0 }, { RD_RTHIGH, RD_RCALF, 0 },
	{ RD_RCALF, RD_RFOOT, 0 },
	// braces: hips and ribcage are one piece each, and the spine chain
	// alone folds like rope
	{ RD_LTHIGH, RD_RTHIGH, 0 }, { RD_PELVIS, RD_SPINE1, 0 },
	{ RD_SPINE, RD_NECK, 0 }, { RD_LCLAV, RD_RCLAV, 0 },
	{ RD_LCLAV, RD_NECK, 0 }, { RD_RCLAV, RD_NECK, 0 },
	// keep the body from folding through itself: back, jackknife at the
	// hips, and heels to the seat
	{ RD_PELVIS, RD_HEAD, 85 },
	{ RD_SPINE1, RD_LCALF, 60 },
	{ RD_SPINE1, RD_RCALF, 60 },
	{ RD_PELVIS, RD_LFOOT, 55 },
	{ RD_PELVIS, RD_RFOOT, 55 },
};
enum { NUM_STICKS = ARRAY_SIZE(sticks) };

struct Slot {
	CPed *ped;
	RpClump *clump;
	RpHAnimHierarchy *hierarchy;
	RwMatrix *matrixArray;
	int modelIndex;
	int numNodes;
	// A live slot rides a knockdown the ped may yet survive; it converts
	// to a death ragdoll in place or hands the body back to the
	// animations at the getup.
	bool live;
	bool asleep;
	float sleepTime;
	float stepDebt;
	float groundZ;
	CVector pos[SIM_BONES];
	CVector prev[SIM_BONES];
	// The pose the death interrupted, kept as the rotation reference.
	CVector deathDir[SIM_BONES];
	CVector deathRight[SIM_BONES];
	CVector deathUp[SIM_BONES];
	CVector deathAt[SIM_BONES];
	float stickLen[NUM_STICKS];
	int16 simMatrix[SIM_BONES];
	// Nodes the simulation does not carry, expressed in their driver's
	// death basis so they follow it rigidly.
	int16 passDriver[MAX_NODES];
	CVector passRight[MAX_NODES];
	CVector passUp[MAX_NODES];
	CVector passAt[MAX_NODES];
	CVector passPos[MAX_NODES];
};

static Slot slots[MAX_RAGDOLLS];
static bool enabled;
static int active;
// The TRAFFIC page prints these, so a headset says which gate refused.
enum { DENY_OFF, DENY_KIND, DENY_RANGE, DENY_SLOTS, DENY_BONES,
	DENY_COUNT };
static int taken;
static int denied[DENY_COUNT];

// Metres and seconds, unlike CPhysical's per-frame units.
static const float STEP = 1.0f/60.0f;
static const float FALL_GRAVITY = 9.8f;
static const float DAMPING = 0.995f;
static const float JOINT_RADIUS = 0.1f;
static const float GROUND_FRICTION = 0.5f;
static const float TAKE_RANGE_SQ = 30.0f*30.0f;
static const float SLEEP_SPEED = 0.15f;
static const float SLEEP_AFTER = 0.6f;

void
SetEnabled(bool on)
{
	// Active bodies finish their fall either way -- dropping the override
	// mid-air would snap the corpse back to the pose the animation left.
	enabled = on;
}

bool
IsEnabled(void)
{
	return enabled;
}

static void SeedVelocity(Slot *slot, CPed *ped);

static Slot*
FindSlot(CPed *ped)
{
	for(int i = 0; i < MAX_RAGDOLLS; i++)
		if(slots[i].ped == ped)
			return &slots[i];
	return nil;
}

static RpHAnimHierarchy*
GetPedHierarchy(CPed *ped)
{
	if(ped == nil || ped->m_rwObject == nil ||
	   RwObjectGetType(ped->m_rwObject) != rpCLUMP ||
	   !IsClumpSkinned(ped->GetClump()))
		return nil;
	RpHAnimHierarchy *hier = GetAnimHierarchyFromSkinClump(ped->GetClump());
	if(hier == nil || hier->nodeInfo == nil || hier->numNodes <= 0 ||
	   hier->numNodes > MAX_NODES || RpHAnimHierarchyGetMatrixArray(hier) == nil)
		return nil;
	return hier;
}

static bool
MatchesHierarchy(const Slot *slot, const CPed *ped, RpHAnimHierarchy *hier)
{
	return hier != nil && ped->m_rwObject == (RwObject*)slot->clump &&
		ped->GetModelIndex() == slot->modelIndex && hier == slot->hierarchy &&
		hier->numNodes == slot->numNodes &&
		RpHAnimHierarchyGetMatrixArray(hier) == slot->matrixArray;
}

// Shortest rotation taking unit vector a to unit vector b, applied to v.
static CVector
ArcRotate(const CVector &a, const CVector &b, const CVector &axis,
          float sinA, float cosA, const CVector &v)
{
	if(sinA < 1e-4f){
		if(cosA > 0.0f)
			return v;
		// Opposite directions: half a turn about anything perpendicular.
		CVector p = Abs(a.x) < 0.9f ?
			CrossProduct(a, CVector(1.0f, 0.0f, 0.0f)) :
			CrossProduct(a, CVector(0.0f, 0.0f, 1.0f));
		p.Normalise();
		return p*(2.0f*DotProduct(p, v)) - v;
	}
	CVector n = axis*(1.0f/sinA);
	return v*cosA + CrossProduct(n, v)*sinA +
		n*(DotProduct(n, v)*(1.0f - cosA));
}

static bool
StartRagdoll(CPed *ped, bool live)
{
	if(!enabled){
		denied[DENY_OFF]++;
		return false;
	}
	if(ped == nil || ped->IsPlayer() || ped->bInVehicle ||
	   ped->bIsInWater || ped->CharCreatedBy != RANDOM_CHAR ||
	   ped->GetClump() == nil){
		denied[DENY_KIND]++;
		return false;
	}
	if((ped->GetPosition() - TheCamera.GetPosition()).MagnitudeSqr() >
	   TAKE_RANGE_SQ){
		denied[DENY_RANGE]++;
		return false;
	}
	Slot *slot = FindSlot(ped);
	RpHAnimHierarchy *hier = GetPedHierarchy(ped);
	if(slot && !MatchesHierarchy(slot, ped, hier)){
		Release(ped);
		slot = nil;
	}
	if(hier == nil){
		denied[DENY_BONES]++;
		return false;
	}
	if(slot){
		// A knockdown already carries the body. A repeated live take
		// reseeds the velocity -- the kill path writes the launch into
		// m_vecMoveSpeed after its SetFall, and this hook order is how
		// that launch reaches the particles. A death mid-flight keeps
		// its motion and merely stops being reversible.
		if(live)
			SeedVelocity(slot, ped);
		else
			slot->live = false;
		return true;
	}
	slot = FindSlot(nil);
	if(slot == nil){
		denied[DENY_SLOTS]++;
		return false;
	}

	RwMatrix *mats = RpHAnimHierarchyGetMatrixArray(hier);

	int simOfNode[MAX_NODES];
	for(int i = 0; i < hier->numNodes; i++)
		simOfNode[i] = -1;
	for(int i = 0; i < SIM_BONES; i++){
		int idx = RpHAnimIDGetIndex(hier, simBoneTags[i]);
		if(idx < 0 || idx >= hier->numNodes){
			denied[DENY_BONES]++;
			return false;
		}
		slot->simMatrix[i] = idx;
		simOfNode[idx] = i;
		slot->pos[i] = mats[idx].pos;
		slot->deathRight[i] = mats[idx].right;
		slot->deathUp[i] = mats[idx].up;
		slot->deathAt[i] = mats[idx].at;
	}

	SeedVelocity(slot, ped);

	for(int i = 0; i < SIM_BONES; i++){
		CVector dir = slot->pos[dirTo[i]] - slot->pos[dirFrom[i]];
		float len = dir.Magnitude();
		slot->deathDir[i] = len > 1e-5f ?
			dir*(1.0f/len) : CVector(0.0f, 0.0f, 1.0f);
	}
	for(int i = 0; i < NUM_STICKS; i++)
		slot->stickLen[i] =
			(slot->pos[sticks[i].b] - slot->pos[sticks[i].a]).Magnitude();

	// Reconstruct each node's parent the way updateMatrices walks the
	// tree, then park every unsimulated node on its nearest simulated
	// ancestor, expressed in that driver's death basis.
	int parentOf[MAX_NODES];
	int stack[MAX_NODES + 1];
	int sp = 0, parent = -1;
	stack[sp++] = parent;
	for(int i = 0; i < hier->numNodes; i++){
		parentOf[i] = parent;
		int flags = hier->nodeInfo[i].flags;
		if(flags & rw::HAnimHierarchy::PUSH && sp <= MAX_NODES)
			stack[sp++] = parent;
		parent = i;
		if(flags & rw::HAnimHierarchy::POP && sp > 0)
			parent = stack[--sp];
	}
	for(int i = 0; i < hier->numNodes; i++){
		slot->passDriver[i] = -1;
		if(simOfNode[i] >= 0)
			continue;
		int up = parentOf[i];
		while(up >= 0 && simOfNode[up] < 0)
			up = parentOf[up];
		int driver = up >= 0 ? simOfNode[up] : RD_PELVIS;
		slot->passDriver[i] = driver;
		const CVector dr = slot->deathRight[driver];
		const CVector du = slot->deathUp[driver];
		const CVector da = slot->deathAt[driver];
		const CVector rel = CVector(mats[i].pos) - slot->pos[driver];
		slot->passPos[i] = CVector(DotProduct(rel, dr),
			DotProduct(rel, du), DotProduct(rel, da));
		const CVector nr = mats[i].right, nu = mats[i].up,
			na = mats[i].at;
		slot->passRight[i] = CVector(DotProduct(nr, dr),
			DotProduct(nr, du), DotProduct(nr, da));
		slot->passUp[i] = CVector(DotProduct(nu, dr),
			DotProduct(nu, du), DotProduct(nu, da));
		slot->passAt[i] = CVector(DotProduct(na, dr),
			DotProduct(na, du), DotProduct(na, da));
	}

	bool found = false;
	const CVector &pelvis = slot->pos[RD_PELVIS];
	float ground = CWorld::FindGroundZFor3DCoord(pelvis.x, pelvis.y,
		pelvis.z + 0.5f, &found);
	slot->groundZ = found ? ground : pelvis.z - 100.0f;
	slot->live = live;
	slot->asleep = false;
	slot->sleepTime = 0.0f;
	slot->stepDebt = 0.0f;
	slot->clump = ped->GetClump();
	slot->hierarchy = hier;
	slot->matrixArray = mats;
	slot->modelIndex = ped->GetModelIndex();
	slot->numNodes = hier->numNodes;
	slot->ped = ped;
	active++;
	taken++;
	return true;
}

// The launch the game computed, as world metres per second; capped, since
// a knock-off impulse can carry a spike that reads as a body shot from a
// catapult. The upper body gets more of it so the fall tumbles.
static void
SeedVelocity(Slot *slot, CPed *ped)
{
	CVector vel = ped->m_vecMoveSpeed*50.0f;
	float speed = vel.Magnitude();
	if(speed > 16.0f)
		vel *= 16.0f/speed;
	float pelvisZ = slot->pos[RD_PELVIS].z;
	for(int i = 0; i < SIM_BONES; i++){
		CVector v = vel*(1.0f + 0.25f*(slot->pos[i].z - pelvisZ));
		slot->prev[i] = slot->pos[i] - v*STEP;
	}
}

bool
Begin(CPed *ped)
{
	return StartRagdoll(ped, false);
}

bool
BeginFall(CPed *ped)
{
	return StartRagdoll(ped, true);
}

const char*
DebugLine(void)
{
	static char line[112];
	snprintf(line, sizeof(line),
		"RAGDOLL %s  ACT %d  TAKEN %d  DENY O/K/R/S/B %d/%d/%d/%d/%d",
		enabled ? "ON" : "OFF", active, taken,
		denied[DENY_OFF], denied[DENY_KIND], denied[DENY_RANGE],
		denied[DENY_SLOTS], denied[DENY_BONES]);
	return line;
}

static void
Simulate(Slot *slot)
{
	float maxTravelSq = 0.0f;
	for(int i = 0; i < SIM_BONES; i++){
		CVector p = slot->pos[i];
		CVector next = p + (p - slot->prev[i])*DAMPING;
		next.z -= FALL_GRAVITY*STEP*STEP;
		slot->prev[i] = p;
		slot->pos[i] = next;
		const float floor = slot->groundZ + JOINT_RADIUS;
		if(slot->pos[i].z < floor){
			slot->pos[i].z = floor;
			// Ground friction: bleed the tangential motion, or the
			// body ice-skates while it settles.
			slot->prev[i].x += (slot->pos[i].x - slot->prev[i].x)*
				GROUND_FRICTION;
			slot->prev[i].y += (slot->pos[i].y - slot->prev[i].y)*
				GROUND_FRICTION;
		}
		float travelSq = (slot->pos[i] - slot->prev[i]).MagnitudeSqr();
		if(travelSq > maxTravelSq)
			maxTravelSq = travelSq;
	}

	for(int iter = 0; iter < 4; iter++)
		for(int i = 0; i < NUM_STICKS; i++){
			CVector &a = slot->pos[sticks[i].a];
			CVector &b = slot->pos[sticks[i].b];
			CVector delta = b - a;
			float dist = delta.Magnitude();
			if(dist < 1e-5f)
				continue;
			float rest = slot->stickLen[i];
			if(sticks[i].minPercent){
				rest *= sticks[i].minPercent*0.01f;
				if(dist >= rest)
					continue;
			}
			CVector push = delta*(0.5f*(dist - rest)/dist);
			a += push;
			b -= push;
		}

	if(maxTravelSq < SLEEP_SPEED*STEP*SLEEP_SPEED*STEP){
		slot->sleepTime += STEP;
		if(slot->sleepTime > SLEEP_AFTER)
			slot->asleep = true;
	}else
		slot->sleepTime = 0.0f;
}

void
Update(void)
{
	if(active == 0)
		return;
	float dt = CTimer::GetTimeStepInSeconds();
	if(dt <= 0.0f)
		return;
	if(dt > 0.1f)
		dt = 0.1f;
	for(int s = 0; s < MAX_RAGDOLLS; s++){
		Slot *slot = &slots[s];
		if(slot->ped == nil)
			continue;
		if(slot->live){
			if(slot->ped->DyingOrDead())
				slot->live = false;
			else if(slot->ped->m_nPedState != PED_FALL){
				// Survived into the getup: the animations take the
				// body back from wherever it starts them.
				slot->ped = nil;
				active--;
				continue;
			}
		}
		if(slot->asleep)
			continue;
		// The body drifts, the floor under it may not be the floor it
		// died over; one query per frame keeps the plane honest.
		bool found = false;
		const CVector &pelvis = slot->pos[RD_PELVIS];
		float ground = CWorld::FindGroundZFor3DCoord(pelvis.x,
			pelvis.y, pelvis.z + 0.5f, &found);
		if(found)
			slot->groundZ = ground;
		slot->stepDebt += dt;
		int steps = 0;
		while(slot->stepDebt >= STEP && steps < MAX_STEPS){
			Simulate(slot);
			slot->stepDebt -= STEP;
			steps++;
		}
		if(slot->stepDebt > STEP)
			slot->stepDebt = STEP;
	}
}

void
Apply(CPed *ped)
{
	if(active == 0 || ped == nil)
		return;
	Slot *slot = FindSlot(ped);
	if(slot == nil)
		return;
	RpHAnimHierarchy *hier = GetPedHierarchy(ped);
	// Model replacements cannot reuse the previous skeleton's node indices.
	if(!MatchesHierarchy(slot, ped, hier)){
		Release(ped);
		return;
	}
	RwMatrix *mats = RpHAnimHierarchyGetMatrixArray(hier);

	CVector simRight[SIM_BONES], simUp[SIM_BONES], simAt[SIM_BONES];
	for(int i = 0; i < SIM_BONES; i++){
		CVector dir = slot->pos[dirTo[i]] - slot->pos[dirFrom[i]];
		float len = dir.Magnitude();
		dir = len > 1e-5f ? dir*(1.0f/len) : slot->deathDir[i];
		const CVector axis = CrossProduct(slot->deathDir[i], dir);
		const float sinA = axis.Magnitude();
		const float cosA = DotProduct(slot->deathDir[i], dir);
		simRight[i] = ArcRotate(slot->deathDir[i], dir, axis, sinA,
			cosA, slot->deathRight[i]);
		simUp[i] = ArcRotate(slot->deathDir[i], dir, axis, sinA,
			cosA, slot->deathUp[i]);
		simAt[i] = ArcRotate(slot->deathDir[i], dir, axis, sinA,
			cosA, slot->deathAt[i]);
		RwMatrix *m = &mats[slot->simMatrix[i]];
		m->right = simRight[i];
		m->up = simUp[i];
		m->at = simAt[i];
		m->pos = slot->pos[i];
		m->flags = 0;
	}
	for(int i = 0; i < hier->numNodes; i++){
		int driver = slot->passDriver[i];
		if(driver < 0)
			continue;
		const CVector dr = simRight[driver], du = simUp[driver],
			da = simAt[driver];
		RwMatrix *m = &mats[i];
		m->right = dr*slot->passRight[i].x + du*slot->passRight[i].y +
			da*slot->passRight[i].z;
		m->up = dr*slot->passUp[i].x + du*slot->passUp[i].y +
			da*slot->passUp[i].z;
		m->at = dr*slot->passAt[i].x + du*slot->passAt[i].y +
			da*slot->passAt[i].z;
		m->pos = slot->pos[driver] + dr*slot->passPos[i].x +
			du*slot->passPos[i].y + da*slot->passPos[i].z;
		m->flags = 0;
	}
}

// Squared distance between the segments [p1,q1] and [p2,q2], with the
// first segment's parameter of the closest point returned in s. The
// standard clamped solve -- both segments are short, so no degenerate
// case needs more than the endpoint clamps.
static float
SegSegDistSq(const CVector &p1, const CVector &q1,
             const CVector &p2, const CVector &q2, float *s)
{
	const CVector d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
	const float a = DotProduct(d1, d1), e = DotProduct(d2, d2);
	const float f = DotProduct(d2, r);
	const float EPS = 1e-6f;
	float sc, tc;
	if(a <= EPS){
		sc = 0.0f;
		tc = e <= EPS ? 0.0f : clamp(f/e, 0.0f, 1.0f);
	}else{
		const float c = DotProduct(d1, r);
		if(e <= EPS){
			tc = 0.0f;
			sc = clamp(-c/a, 0.0f, 1.0f);
		}else{
			const float b = DotProduct(d1, d2);
			const float denom = a*e - b*b;
			sc = denom > EPS ?
				clamp((b*f - c*e)/denom, 0.0f, 1.0f) : 0.0f;
			tc = (b*sc + f)/e;
			if(tc < 0.0f){
				tc = 0.0f;
				sc = clamp(-c/a, 0.0f, 1.0f);
			}else if(tc > 1.0f){
				tc = 1.0f;
				sc = clamp((b - c)/a, 0.0f, 1.0f);
			}
		}
	}
	*s = sc;
	const CVector diff = (p1 + d1*sc) - (p2 + d2*tc);
	return DotProduct(diff, diff);
}

void
BulletHit(const CVector &source, const CVector &end)
{
	if(active == 0)
		return;
	const float BONE_RADIUS = 0.14f;
	for(int sl = 0; sl < MAX_RAGDOLLS; sl++){
		Slot *slot = &slots[sl];
		// Only the corpses need this: a dead ped left the collision
		// world at SetDead, so no real bullet can find it. A still-live
		// flier keeps its collision and takes bullets the ordinary way.
		if(slot->ped == nil ||
		   slot->ped->m_nPedState != PED_DEAD)
			continue;
		// The body is a set of capsules, one per rigid bone; the ray is
		// tested against all of them and the nearest hit along the ray
		// wins. Point spheres left gaps a bullet could thread between
		// two joints of a sprawled corpse.
		int hitStick = -1;
		float hitParam = 2.0f;
		for(int i = 0; i < NUM_STICKS; i++){
			if(sticks[i].minPercent)
				continue;
			float param;
			float dSq = SegSegDistSq(source, end,
				slot->pos[sticks[i].a], slot->pos[sticks[i].b],
				&param);
			if(dSq < BONE_RADIUS*BONE_RADIUS && param < hitParam){
				hitParam = param;
				hitStick = i;
			}
		}
		if(hitStick < 0)
			continue;
		CVector ray = end - source;
		float len = ray.Magnitude();
		const CVector dir = len > 0.01f ? ray*(1.0f/len) :
			CVector(0.0f, 0.0f, 1.0f);
		const int a = sticks[hitStick].a, b = sticks[hitStick].b;
		const CVector hitPos = source + ray*hitParam;
		slot->asleep = false;
		slot->sleepTime = 0.0f;
		// Both ends of the struck bone take the kick, weighted to the
		// end the ray passed nearer.
		float wa = (slot->pos[a] - hitPos).Magnitude();
		float wb = (slot->pos[b] - hitPos).Magnitude();
		float sum = wa + wb + 1e-4f;
		slot->prev[a] -= dir*(7.0f*STEP*(wb/sum));
		slot->prev[b] -= dir*(7.0f*STEP*(wa/sum));
		CVector bloodDir = dir*0.05f;
		bloodDir.z += 0.03f;
		for(int i = 0; i < 3; i++)
			CParticle::AddParticle(PARTICLE_BLOOD, hitPos,
				bloodDir, nil, 0.0f, 0, 0, 0, 0);
	}
}

void
Release(CPed *ped)
{
	if(active == 0 || ped == nil)
		return;
	Slot *slot = FindSlot(ped);
	if(slot){
		slot->ped = nil;
		active--;
	}
}

}
