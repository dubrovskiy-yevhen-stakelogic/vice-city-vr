#pragma once

class CPed;
class CVector;

// Verlet ragdoll for dying ambient peds: a particle per joint, distance
// constraints along the skeleton, and the world-space hierarchy matrices
// rewritten after the animation update. Purely presentational -- the ped
// state machine runs exactly as it always did, only the canned death
// animation is skipped while a ragdoll owns the fall.
namespace VrRagdoll
{
void SetEnabled(bool enabled);
bool IsEnabled(void);
// Takes the fall over at SetDie. False when the option is off, the ped
// does not qualify (player, mission char, in a vehicle, in water, too
// far) or every slot is busy -- the canned animation then plays as ever.
bool Begin(CPed *ped);
// Takes a live ped the moment a vehicle knocks it flying, so the flight
// itself ragdolls: released back to the animations when the ped survives
// into its getup, converted in place when death arrives mid-flight.
bool BeginFall(CPed *ped);
// One-line counters for the TRAFFIC page.
const char *DebugLine(void);
// A fired bullet's resolved ray. Corpses leave the collision world at
// SetDead and stop taking real bullets, so every weapon ray is retested
// here against the particles: the joint it finds takes a kick and bleeds.
void BulletHit(const CVector &source, const CVector &end);
// Advances every active ragdoll; once per CWorld::Process, so a paused
// game freezes the bodies with everything else.
void Update(void);
// Overwrites the ped's bone matrices with the ragdoll pose; called right
// after UpdateRpHAnim so the existing PreRender overrides compose on top.
void Apply(CPed *ped);
void Release(CPed *ped);
}
