#ifndef GAME_SERVER_GAMEMODES_GTRAIN_RUN_H
#define GAME_SERVER_GAMEMODES_GTRAIN_RUN_H

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <functional>
#include <vector>

class CCharacter;
class CGameContext;

// GTrain stores every capture made in control as the tee's state when its
// hover released it, plus the input the server applied in each tick until the
// capture. A solo tee depends on nothing else but the map, so the server's
// physics simulate the same path from that, see ReplayGTrainRun.

// Everything of a character that its next ticks read, at a tick boundary,
// with its shots in flight. Positions and velocities of characters are
// quantized there, all floats are stored exactly anyway.
class CGTrainRunState
{
public:
	// A switch of the character's team that differs from its initial state.
	struct CSwitch
	{
		int m_Number = 0;
		bool m_Status = false;
		int m_Type = 0;
		int m_EndTick = 0;
		bool operator==(const CSwitch &Other) const = default;
	};
	// A projectile or laser of the player, like those fired in the tick
	// that ends a hover.
	struct CShot
	{
		bool m_Laser = false;
		int m_Type = 0;
		vec2 m_Pos = vec2(0, 0);
		vec2 m_Dir = vec2(0, 0);
		// a projectile's start, a laser's last bounce
		int m_Tick = 0;
		int m_TuneZone = 0;
		int m_Layer = 0;
		int m_Number = 0;
		// projectiles only
		int m_LifeSpan = 0;
		int m_SoundImpact = 0;
		int m_Bouncing = 0;
		bool m_Explosive = false;
		bool m_Freeze = false;
		vec2 m_InitDir = vec2(0, 0);
		// lasers only
		vec2 m_From = vec2(0, 0);
		vec2 m_PrevPos = vec2(0, 0);
		vec2 m_TelePos = vec2(0, 0);
		float m_Energy = 0;
		int m_Bounces = 0;
		bool m_WasTele = false;
		bool m_ZeroEnergyBounceInLastTick = false;
		bool m_TeleportCancelled = false;
		bool m_IsBlueTeleport = false;
	};

	// Server tick of the state. Other ticks are stored as absolute ticks too,
	// loading moves them along when replaying at another tick.
	int m_Tick = 0;
	vec2 m_Pos = vec2(0, 0);
	vec2 m_PrevPos = vec2(0, 0);
	vec2 m_Vel = vec2(0, 0);
	vec2 m_HookPos = vec2(0, 0);
	vec2 m_HookDir = vec2(0, 0);
	vec2 m_HookTeleBase = vec2(0, 0);
	int m_HookTick = 0;
	int m_HookState = 0;
	bool m_NewHook = false;
	int m_Jumped = 0;
	int m_JumpedTotal = 0;
	int m_Jumps = 0;
	int m_ActiveWeapon = 0;
	int m_LastWeapon = 0;
	int m_QueuedWeapon = -1;
	int m_ReloadTimer = 0;
	bool m_aWeaponGot[NUM_WEAPONS] = {};
	int m_aWeaponAmmo[NUM_WEAPONS] = {};
	vec2 m_NinjaActivationDir = vec2(0, 0);
	int m_NinjaActivationTick = 0;
	int m_NinjaCurrentMoveTime = 0;
	int m_NinjaOldVelAmount = 0;
	int m_FreezeTime = 0;
	int m_FreezeStart = 0;
	bool m_FrozenLastTick = false;
	bool m_DeepFrozen = false;
	bool m_LiveFrozen = false;
	bool m_Solo = false;
	bool m_Jetpack = false;
	bool m_NinjaJetpack = false;
	bool m_CollisionDisabled = false;
	bool m_HookHitDisabled = false;
	bool m_EndlessHook = false;
	bool m_EndlessJump = false;
	bool m_HammerHitDisabled = false;
	bool m_ShotgunHitDisabled = false;
	bool m_GrenadeHitDisabled = false;
	bool m_LaserHitDisabled = false;
	bool m_HasTelegunGun = false;
	bool m_HasTelegunGrenade = false;
	bool m_HasTelegunLaser = false;
	bool m_ZeroGravity = false;
	bool m_LeftWall = false;
	bool m_LastRefillJumps = false;
	bool m_LastPenalty = false;
	bool m_LastBonus = false;
	int m_Colliding = 0;
	int m_MoveRestrictions = 0;
	int m_TuneZone = 0;
	int m_TeleCheckpoint = 0;
	int m_DDRaceState = 0;
	int m_StartTime = 0;
	CNetObj_PlayerInput m_SavedInput = {};
	// the previous input, with what freezes take away from it
	CNetObj_PlayerInput m_Input = {};
	// the last input weapons saw, older than the saved one after chatting
	CNetObj_PlayerInput m_LatestInput = {};
	// only whether this is above one matters
	int m_NumInputs = 0;
	std::vector<CSwitch> m_vSwitches;
	// newest first, like the world lists them
	std::vector<CShot> m_vShots;

	// The hover doors' switch is left out, it is GTrain's own.
	void Save(CCharacter *pChr, int IgnoredSwitch);
	// For a character spawned in this tick, which also reset its switches
	// and has no shots yet.
	void Load(CCharacter *pChr) const;
	// Whom the character's hook holds, once all of a team's tees exist.
	static void LoadHookedPlayer(CCharacter *pChr, int HookedPlayer);

	bool operator==(const CGTrainRunState &Other) const;

	// Writes or reads every member, with TSelf const for writing.
	template<typename TSelf, typename TStream>
	static void Transfer(TSelf &Self, TStream &Stream);
};

class CGTrainRun
{
public:
	// Changes of the encoding or of what replays need bump it.
	static constexpr int FORMAT_VERSION = 1;
	// Longer captures are not stored.
	static constexpr int MAX_TICKS = 10 * 60 * SERVER_TICK_SPEED;

	// A random teleport choice for the tee, its hook or its weapons.
	struct CRandom
	{
		int m_Tick = 0; // after the start
		int m_Value = 0;
		bool operator==(const CRandom &Other) const = default;
	};

	CGTrainRunState m_Start;
	// Weapons GTrain gives back after every tick, a bit per weapon.
	int m_KeptWeapons = 0;
	vec2 m_Goal = vec2(0, 0);
	// The input of every tick after the start, the last one captured.
	std::vector<CNetObj_PlayerInput> m_vInputs;
	std::vector<CRandom> m_vRandom;
	// Where the capture happened, to check replays against.
	vec2 m_EndPos = vec2(0, 0);
	vec2 m_EndVel = vec2(0, 0);

	// Compressed, see the README for the format.
	std::vector<unsigned char> Encode() const;
	bool Decode(const unsigned char *pData, int Size);

	bool operator==(const CGTrainRun &Other) const;
};

// A /team's capture: every member's run from the team's shared release until
// the last flag, also after their own. Teammates collide, hook and hit each
// other, so they only replay together, in one DDRace team: each tick applies
// the inputs by client id and moves the tees in the world's order, which
// ReplayGTrainTeamRun restores from the members' ids and order.
class CGTrainTeamRun
{
public:
	// Solo runs keep CGTrainRun::FORMAT_VERSION, readers tell them apart by
	// it, see GTrainRunVersion().
	static constexpr int FORMAT_VERSION = 2;

	struct CMember
	{
		char m_aName[MAX_NAME_LENGTH] = "";
		int m_ClientId = -1;
		// Ticks from the release until the member reached their flag.
		int m_CaptureTicks = 0;
		// The teammate the member's hook held at the release, by client id.
		int m_HookedPlayer = -1;
		// m_Goal is the member's flag, m_EndPos and m_EndVel are where the
		// member was when the last flag was reached. Every member has the
		// same number of inputs, the whole run.
		CGTrainRun m_Run;
	};

	// The DDRace team the members played in.
	int m_Team = 0;
	// In the order the world moves them, first to move first.
	std::vector<CMember> m_vMembers;

	std::vector<unsigned char> Encode() const;
	bool Decode(const unsigned char *pData, int Size);

	bool operator==(const CGTrainTeamRun &Other) const;
};

// The format version of an encoded run, 0 if it has none.
int GTrainRunVersion(const unsigned char *pData, int Size);

// GTrain's kept weapons, given back every tick.
void KeepGTrainWeapons(CCharacter *pChr, int Weapons);

// Plays the run back on a new character of ClientId. The game type must not
// be GTrain, which would place the tee itself. AdvanceTick moves the server
// to its next tick, between the early and the normal input like CServer::Run.
// Starting at the run's tick keeps map entities like turrets in phase.
// AfterTick sees the character after every tick. Returns whether it ended
// where the stored run did.
bool ReplayGTrainRun(CGameContext *pGameServer, int ClientId, const CGTrainRun &Run, const std::function<void()> &AdvanceTick, const std::function<void(CCharacter *pChr)> &AfterTick = nullptr);

// Plays the team run back like ReplayGTrainRun, on new characters of the
// members' client ids, which must have players. AfterTick sees each member
// after every tick, with their index in m_vMembers. Returns whether every
// member ended where the stored run did.
bool ReplayGTrainTeamRun(CGameContext *pGameServer, const CGTrainTeamRun &Run, const std::function<void()> &AdvanceTick, const std::function<void(int Member, CCharacter *pChr)> &AfterTick = nullptr);

#endif // GAME_SERVER_GAMEMODES_GTRAIN_RUN_H
