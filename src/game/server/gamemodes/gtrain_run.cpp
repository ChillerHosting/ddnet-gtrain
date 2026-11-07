#include "gtrain_run.h"

#include <base/dbg.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/shared/compression.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace
{
	static_assert(sizeof(CNetObj_PlayerInput) == 10 * sizeof(int));

	// Input fields in the order of their bits in change masks, the ones changing
	// most often first, so most masks fit in the first byte of a varint.
	constexpr int INPUT_FIELDS[] = {
		offsetof(CNetObj_PlayerInput, m_TargetX) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_TargetY) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_Direction) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_Jump) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_Hook) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_Fire) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_PlayerFlags) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_WantedWeapon) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_NextWeapon) / sizeof(int),
		offsetof(CNetObj_PlayerInput, m_PrevWeapon) / sizeof(int),
	};
	static_assert(std::size(INPUT_FIELDS) == sizeof(CNetObj_PlayerInput) / sizeof(int));

	constexpr int MAX_RAW_SIZE = 16 * 1024 * 1024;
	constexpr int MAX_LIST_SIZE = 256;

	int InputField(const CNetObj_PlayerInput &Input, int Field)
	{
		int Value;
		mem_copy(&Value, reinterpret_cast<const unsigned char *>(&Input) + Field * sizeof(int), sizeof(Value));
		return Value;
	}

	void SetInputField(CNetObj_PlayerInput &Input, int Field, int Value)
	{
		mem_copy(reinterpret_cast<unsigned char *>(&Input) + Field * sizeof(int), &Value, sizeof(Value));
	}

	int ChangedInputFields(const CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &Prev)
	{
		int Mask = 0;
		for(int i = 0; i < (int)std::size(INPUT_FIELDS); ++i)
			if(InputField(Input, INPUT_FIELDS[i]) != InputField(Prev, INPUT_FIELDS[i]))
				Mask |= 1 << i;
		return Mask;
	}

	// Everything is a varint. Inputs come from clients, so their differences
	// wrap around instead of overflowing.
	class CRunWriter
	{
	public:
		std::vector<unsigned char> m_vData;

		void Int(int Value)
		{
			unsigned char aBuf[CVariableInt::MAX_BYTES_PACKED];
			unsigned char *pEnd = CVariableInt::Pack(aBuf, Value, sizeof(aBuf));
			m_vData.insert(m_vData.end(), aBuf, pEnd);
		}

		// Quantized values are multiples of 1/256 and take an even number,
		// anything else keeps its bits after a one.
		void Float(float Value)
		{
			const float Scaled = Value * 256.0f;
			if(std::trunc(Scaled) == Scaled && std::fabs(Scaled) < (float)(1 << 29) && !(Value == 0.0f && std::signbit(Value)))
			{
				Int((int)Scaled * 2);
				return;
			}
			int Bits;
			mem_copy(&Bits, &Value, sizeof(Bits));
			Int(1);
			Int(Bits);
		}

		void Vec(vec2 Value)
		{
			Float(Value.x);
			Float(Value.y);
		}

		template<size_t N>
		void Flags(const std::array<const bool *, N> &apFlags)
		{
			static_assert(N < 31);
			int Flags = 0;
			for(size_t i = 0; i < N; ++i)
				if(*apFlags[i])
					Flags |= 1 << i;
			Int(Flags);
		}

		// The mask of changed fields, then their differences.
		void Input(const CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &Prev)
		{
			const int Mask = ChangedInputFields(Input, Prev);
			Int(Mask);
			for(int i = 0; i < (int)std::size(INPUT_FIELDS); ++i)
				if(Mask & (1 << i))
					Int((int)((unsigned)InputField(Input, INPUT_FIELDS[i]) - (unsigned)InputField(Prev, INPUT_FIELDS[i])));
		}

		// Unchanged inputs are a zero mask and how many more follow.
		void Inputs(const std::vector<CNetObj_PlayerInput> &vInputs, CNetObj_PlayerInput Prev)
		{
			for(size_t i = 0; i < vInputs.size();)
			{
				if(ChangedInputFields(vInputs[i], Prev) != 0)
				{
					Input(vInputs[i], Prev);
					Prev = vInputs[i++];
					continue;
				}
				size_t Repeats = 1;
				while(i + Repeats < vInputs.size() && ChangedInputFields(vInputs[i + Repeats], Prev) == 0)
					++Repeats;
				Int(0);
				Int((int)Repeats - 1);
				i += Repeats;
			}
		}

		template<typename T>
		void Count(const std::vector<T> &vList)
		{
			Int((int)vList.size());
		}

		void Str(const char *pStr)
		{
			const int Length = str_length(pStr);
			Int(Length);
			m_vData.insert(m_vData.end(), pStr, pStr + Length);
		}
	};

	class CRunReader
	{
		const unsigned char *m_pCurrent;
		const unsigned char *m_pEnd;
		bool m_Error = false;

	public:
		CRunReader(const unsigned char *pData, int Size) :
			m_pCurrent(pData), m_pEnd(pData + Size)
		{
		}

		bool Error() const { return m_Error; }
		bool AtEnd() const { return m_pCurrent == m_pEnd; }
		const unsigned char *Current() const { return m_pCurrent; }
		int Remaining() const { return m_pEnd - m_pCurrent; }
		void Fail() { m_Error = true; }

		void Int(int &Value)
		{
			Value = 0;
			if(m_Error)
				return;
			const unsigned char *pNext = m_pCurrent < m_pEnd ? CVariableInt::Unpack(m_pCurrent, &Value, m_pEnd - m_pCurrent) : nullptr;
			if(!pNext)
			{
				Value = 0;
				m_Error = true;
				return;
			}
			m_pCurrent = pNext;
		}

		void Float(float &Value)
		{
			int Encoded;
			Int(Encoded);
			if(!(Encoded & 1))
			{
				Value = (Encoded / 2) / 256.0f;
				return;
			}
			if(Encoded != 1)
				Fail();
			int Bits;
			Int(Bits);
			mem_copy(&Value, &Bits, sizeof(Value));
		}

		void Vec(vec2 &Value)
		{
			Float(Value.x);
			Float(Value.y);
		}

		template<size_t N>
		void Flags(const std::array<bool *, N> &apFlags)
		{
			int Flags;
			Int(Flags);
			for(size_t i = 0; i < N; ++i)
				*apFlags[i] = Flags & (1 << i);
		}

		void Input(CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &Prev)
		{
			int Mask;
			Int(Mask);
			InputWithMask(Input, Prev, Mask);
		}

		void InputWithMask(CNetObj_PlayerInput &Input, const CNetObj_PlayerInput &Prev, int Mask)
		{
			if(Mask < 0 || Mask >= 1 << std::size(INPUT_FIELDS))
				Fail();
			Input = Prev;
			for(int i = 0; i < (int)std::size(INPUT_FIELDS); ++i)
			{
				if(!(Mask & (1 << i)))
					continue;
				int Difference;
				Int(Difference);
				SetInputField(Input, INPUT_FIELDS[i], (int)((unsigned)InputField(Prev, INPUT_FIELDS[i]) + (unsigned)Difference));
			}
		}

		void Inputs(std::vector<CNetObj_PlayerInput> &vInputs, int Num, CNetObj_PlayerInput Prev)
		{
			vInputs.clear();
			vInputs.reserve(Num);
			while((int)vInputs.size() < Num && !m_Error)
			{
				int Mask;
				Int(Mask);
				if(Mask != 0)
				{
					InputWithMask(Prev, Prev, Mask);
					vInputs.push_back(Prev);
					continue;
				}
				int Repeats;
				Int(Repeats);
				if(Repeats < 0 || Repeats >= Num - (int)vInputs.size())
				{
					Fail();
					return;
				}
				vInputs.insert(vInputs.end(), Repeats + 1, Prev);
			}
		}

		template<typename T>
		void Count(std::vector<T> &vList)
		{
			int Num;
			Int(Num);
			if(Num < 0 || Num > MAX_LIST_SIZE)
			{
				Fail();
				Num = 0;
			}
			vList.resize(Num);
		}

		void Str(char *pBuf, int BufSize)
		{
			int Length;
			Int(Length);
			pBuf[0] = '\0';
			if(Length < 0 || Length >= BufSize || Length > Remaining())
			{
				Fail();
				return;
			}
			mem_copy(pBuf, m_pCurrent, Length);
			pBuf[Length] = '\0';
			m_pCurrent += Length;
		}
	};
} // namespace

template<typename TSelf, typename TStream>
void CGTrainRunState::Transfer(TSelf &Self, TStream &Stream)
{
	Stream.Int(Self.m_Tick);
	Stream.Vec(Self.m_Pos);
	Stream.Vec(Self.m_PrevPos);
	Stream.Vec(Self.m_Vel);
	Stream.Vec(Self.m_HookPos);
	Stream.Vec(Self.m_HookDir);
	Stream.Vec(Self.m_HookTeleBase);
	Stream.Int(Self.m_HookTick);
	Stream.Int(Self.m_HookState);
	Stream.Int(Self.m_Jumped);
	Stream.Int(Self.m_JumpedTotal);
	Stream.Int(Self.m_Jumps);
	Stream.Int(Self.m_ActiveWeapon);
	Stream.Int(Self.m_LastWeapon);
	Stream.Int(Self.m_QueuedWeapon);
	Stream.Int(Self.m_ReloadTimer);
	for(auto &Ammo : Self.m_aWeaponAmmo)
		Stream.Int(Ammo);
	Stream.Vec(Self.m_NinjaActivationDir);
	Stream.Int(Self.m_NinjaActivationTick);
	Stream.Int(Self.m_NinjaCurrentMoveTime);
	Stream.Int(Self.m_NinjaOldVelAmount);
	Stream.Int(Self.m_FreezeTime);
	Stream.Int(Self.m_FreezeStart);
	Stream.Int(Self.m_Colliding);
	Stream.Int(Self.m_MoveRestrictions);
	Stream.Int(Self.m_TuneZone);
	Stream.Int(Self.m_TeleCheckpoint);
	Stream.Int(Self.m_DDRaceState);
	Stream.Int(Self.m_StartTime);
	static_assert(NUM_WEAPONS == 6);
	Stream.Flags(std::array{
		&Self.m_aWeaponGot[0], &Self.m_aWeaponGot[1], &Self.m_aWeaponGot[2], &Self.m_aWeaponGot[3], &Self.m_aWeaponGot[4], &Self.m_aWeaponGot[5],
		&Self.m_NewHook, &Self.m_FrozenLastTick, &Self.m_DeepFrozen, &Self.m_LiveFrozen, &Self.m_Solo, &Self.m_Jetpack, &Self.m_NinjaJetpack,
		&Self.m_CollisionDisabled, &Self.m_HookHitDisabled, &Self.m_EndlessHook, &Self.m_EndlessJump,
		&Self.m_HammerHitDisabled, &Self.m_ShotgunHitDisabled, &Self.m_GrenadeHitDisabled, &Self.m_LaserHitDisabled,
		&Self.m_HasTelegunGun, &Self.m_HasTelegunGrenade, &Self.m_HasTelegunLaser,
		&Self.m_ZeroGravity, &Self.m_LeftWall, &Self.m_LastRefillJumps, &Self.m_LastPenalty, &Self.m_LastBonus});
	Stream.Input(Self.m_SavedInput, CNetObj_PlayerInput{});
	Stream.Input(Self.m_Input, Self.m_SavedInput);
	Stream.Input(Self.m_LatestInput, Self.m_SavedInput);
	Stream.Int(Self.m_NumInputs);
	Stream.Count(Self.m_vSwitches);
	for(auto &Switch : Self.m_vSwitches)
	{
		Stream.Int(Switch.m_Number);
		Stream.Flags(std::array{&Switch.m_Status});
		Stream.Int(Switch.m_Type);
		Stream.Int(Switch.m_EndTick);
	}
	Stream.Count(Self.m_vShots);
	for(auto &Shot : Self.m_vShots)
	{
		Stream.Flags(std::array{&Shot.m_Laser});
		Stream.Int(Shot.m_Type);
		Stream.Vec(Shot.m_Pos);
		Stream.Vec(Shot.m_Dir);
		Stream.Int(Shot.m_Tick);
		Stream.Int(Shot.m_TuneZone);
		Stream.Int(Shot.m_Layer);
		Stream.Int(Shot.m_Number);
		if(Shot.m_Laser)
		{
			Stream.Vec(Shot.m_From);
			Stream.Vec(Shot.m_PrevPos);
			Stream.Vec(Shot.m_TelePos);
			Stream.Float(Shot.m_Energy);
			Stream.Int(Shot.m_Bounces);
			Stream.Flags(std::array{&Shot.m_WasTele, &Shot.m_ZeroEnergyBounceInLastTick, &Shot.m_TeleportCancelled, &Shot.m_IsBlueTeleport});
		}
		else
		{
			Stream.Int(Shot.m_LifeSpan);
			Stream.Int(Shot.m_SoundImpact);
			Stream.Int(Shot.m_Bouncing);
			Stream.Vec(Shot.m_InitDir);
			Stream.Flags(std::array{&Shot.m_Explosive, &Shot.m_Freeze});
		}
	}
}

void CGTrainRunState::Save(CCharacter *pChr, int IgnoredSwitch)
{
	const CCharacterCore &Core = pChr->m_Core;
	m_Tick = pChr->Server()->Tick();
	m_Pos = Core.m_Pos;
	m_PrevPos = pChr->m_PrevPos;
	m_Vel = Core.m_Vel;
	m_HookPos = Core.m_HookPos;
	m_HookDir = Core.m_HookDir;
	m_HookTeleBase = Core.m_HookTeleBase;
	m_HookTick = Core.m_HookTick;
	m_HookState = Core.m_HookState;
	m_NewHook = Core.m_NewHook;
	m_Jumped = Core.m_Jumped;
	m_JumpedTotal = Core.m_JumpedTotal;
	m_Jumps = Core.m_Jumps;
	m_ActiveWeapon = Core.m_ActiveWeapon;
	m_LastWeapon = pChr->m_LastWeapon;
	m_QueuedWeapon = pChr->m_QueuedWeapon;
	m_ReloadTimer = pChr->m_ReloadTimer;
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		m_aWeaponGot[i] = Core.m_aWeapons[i].m_Got;
		m_aWeaponAmmo[i] = Core.m_aWeapons[i].m_Ammo;
	}
	m_NinjaActivationDir = Core.m_Ninja.m_ActivationDir;
	m_NinjaActivationTick = Core.m_Ninja.m_ActivationTick;
	m_NinjaCurrentMoveTime = Core.m_Ninja.m_CurrentMoveTime;
	m_NinjaOldVelAmount = Core.m_Ninja.m_OldVelAmount;
	m_FreezeTime = pChr->m_FreezeTime;
	m_FreezeStart = Core.m_FreezeStart;
	m_FrozenLastTick = pChr->m_FrozenLastTick;
	m_DeepFrozen = Core.m_DeepFrozen;
	m_LiveFrozen = Core.m_LiveFrozen;
	m_Solo = Core.m_Solo;
	m_Jetpack = Core.m_Jetpack;
	m_NinjaJetpack = pChr->m_NinjaJetpack;
	m_CollisionDisabled = Core.m_CollisionDisabled;
	m_HookHitDisabled = Core.m_HookHitDisabled;
	m_EndlessHook = Core.m_EndlessHook;
	m_EndlessJump = Core.m_EndlessJump;
	m_HammerHitDisabled = Core.m_HammerHitDisabled;
	m_ShotgunHitDisabled = Core.m_ShotgunHitDisabled;
	m_GrenadeHitDisabled = Core.m_GrenadeHitDisabled;
	m_LaserHitDisabled = Core.m_LaserHitDisabled;
	m_HasTelegunGun = Core.m_HasTelegunGun;
	m_HasTelegunGrenade = Core.m_HasTelegunGrenade;
	m_HasTelegunLaser = Core.m_HasTelegunLaser;
	m_ZeroGravity = pChr->m_ZeroGravity;
	m_LeftWall = Core.m_LeftWall;
	m_LastRefillJumps = pChr->m_LastRefillJumps;
	m_LastPenalty = pChr->m_LastPenalty;
	m_LastBonus = pChr->m_LastBonus;
	m_Colliding = Core.m_Colliding;
	m_MoveRestrictions = pChr->m_MoveRestrictions;
	m_TuneZone = pChr->m_TuneZone;
	m_TeleCheckpoint = pChr->m_TeleCheckpoint;
	m_DDRaceState = (int)pChr->m_DDRaceState;
	m_StartTime = pChr->m_StartTime;
	m_SavedInput = pChr->m_SavedInput;
	m_Input = pChr->m_Input;
	m_LatestInput = pChr->m_LatestInput;
	m_NumInputs = std::min(pChr->m_NumInputs, 2);

	m_vShots.clear();
	const int ClientId = pChr->GetPlayer()->GetCid();
	CGameWorld &World = pChr->GameServer()->m_World;
	for(CEntity *pEntity = World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEntity; pEntity = pEntity->TypeNext())
	{
		const CProjectile *pProjectile = static_cast<CProjectile *>(pEntity);
		if(pProjectile->m_Owner != ClientId || pProjectile->m_MarkedForDestroy)
			continue;
		CShot &Shot = m_vShots.emplace_back();
		Shot.m_Type = pProjectile->m_Type;
		Shot.m_Pos = pProjectile->m_Pos;
		Shot.m_Dir = pProjectile->m_Direction;
		Shot.m_Tick = pProjectile->m_StartTick;
		Shot.m_TuneZone = pProjectile->m_TuneZone;
		Shot.m_Layer = pProjectile->m_Layer;
		Shot.m_Number = pProjectile->m_Number;
		Shot.m_LifeSpan = pProjectile->m_LifeSpan;
		Shot.m_SoundImpact = pProjectile->m_SoundImpact;
		Shot.m_Bouncing = pProjectile->m_Bouncing;
		Shot.m_Explosive = pProjectile->m_Explosive;
		Shot.m_Freeze = pProjectile->m_Freeze;
		Shot.m_InitDir = pProjectile->m_InitDir;
	}
	for(CEntity *pEntity = World.FindFirst(CGameWorld::ENTTYPE_LASER); pEntity; pEntity = pEntity->TypeNext())
	{
		const CLaser *pLaser = static_cast<CLaser *>(pEntity);
		if(pLaser->m_Owner != ClientId || pLaser->m_MarkedForDestroy)
			continue;
		CShot &Shot = m_vShots.emplace_back();
		Shot.m_Laser = true;
		Shot.m_Type = pLaser->m_Type;
		Shot.m_Pos = pLaser->m_Pos;
		Shot.m_Dir = pLaser->m_Dir;
		Shot.m_Tick = pLaser->m_EvalTick;
		Shot.m_TuneZone = pLaser->m_TuneZone;
		Shot.m_Layer = pLaser->m_Layer;
		Shot.m_Number = pLaser->m_Number;
		Shot.m_From = pLaser->m_From;
		Shot.m_PrevPos = pLaser->m_PrevPos;
		Shot.m_TelePos = pLaser->m_TelePos;
		Shot.m_Energy = pLaser->m_Energy;
		Shot.m_Bounces = pLaser->m_Bounces;
		Shot.m_WasTele = pLaser->m_WasTele;
		Shot.m_ZeroEnergyBounceInLastTick = pLaser->m_ZeroEnergyBounceInLastTick;
		Shot.m_TeleportCancelled = pLaser->m_TeleportCancelled;
		Shot.m_IsBlueTeleport = pLaser->m_IsBlueTeleport;
	}

	m_vSwitches.clear();
	const int Team = pChr->Team();
	if(Team == TEAM_SUPER)
		return;
	const std::vector<SSwitchers> &vSwitchers = pChr->GameServer()->Switchers();
	for(int Number = 0; Number < (int)vSwitchers.size(); ++Number)
	{
		// Switches are reset to their initial state with type 0 or open.
		const SSwitchers &Switcher = vSwitchers[Number];
		const int Type = Switcher.m_aType[Team];
		if(Number != IgnoredSwitch && (Switcher.m_aStatus[Team] != Switcher.m_Initial || Switcher.m_aEndTick[Team] || (Type != 0 && Type != TILE_SWITCHOPEN)))
			m_vSwitches.push_back({Number, Switcher.m_aStatus[Team], Type, Switcher.m_aEndTick[Team]});
	}
}

void CGTrainRunState::Load(CCharacter *pChr) const
{
	const int Shift = pChr->Server()->Tick() - m_Tick;
	const auto LoadTick = [&](int Tick) { return Tick ? Tick + Shift : 0; };
	CCharacterCore &Core = pChr->m_Core;
	Core.m_Pos = m_Pos;
	pChr->m_Pos = m_Pos;
	pChr->m_PrevPos = m_PrevPos;
	Core.m_Vel = m_Vel;
	Core.m_HookPos = m_HookPos;
	Core.m_HookDir = m_HookDir;
	Core.m_HookTeleBase = m_HookTeleBase;
	Core.m_HookTick = m_HookTick;
	Core.m_HookState = m_HookState;
	Core.SetHookedPlayer(-1);
	Core.m_NewHook = m_NewHook;
	Core.m_Jumped = m_Jumped;
	Core.m_JumpedTotal = m_JumpedTotal;
	Core.m_Jumps = m_Jumps;
	Core.m_ActiveWeapon = m_ActiveWeapon;
	pChr->m_LastWeapon = m_LastWeapon;
	pChr->m_QueuedWeapon = m_QueuedWeapon;
	pChr->m_ReloadTimer = m_ReloadTimer;
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		Core.m_aWeapons[i].m_Got = m_aWeaponGot[i];
		Core.m_aWeapons[i].m_Ammo = m_aWeaponAmmo[i];
	}
	Core.m_Ninja.m_ActivationDir = m_NinjaActivationDir;
	Core.m_Ninja.m_ActivationTick = LoadTick(m_NinjaActivationTick);
	Core.m_Ninja.m_CurrentMoveTime = m_NinjaCurrentMoveTime;
	Core.m_Ninja.m_OldVelAmount = m_NinjaOldVelAmount;
	pChr->m_FreezeTime = m_FreezeTime;
	Core.m_FreezeStart = LoadTick(m_FreezeStart);
	pChr->m_FrozenLastTick = m_FrozenLastTick;
	Core.m_DeepFrozen = m_DeepFrozen;
	Core.m_LiveFrozen = m_LiveFrozen;
	pChr->SetSolo(m_Solo);
	Core.m_Jetpack = m_Jetpack;
	pChr->m_NinjaJetpack = m_NinjaJetpack;
	Core.m_CollisionDisabled = m_CollisionDisabled;
	Core.m_HookHitDisabled = m_HookHitDisabled;
	Core.m_EndlessHook = m_EndlessHook;
	Core.m_EndlessJump = m_EndlessJump;
	Core.m_HammerHitDisabled = m_HammerHitDisabled;
	Core.m_ShotgunHitDisabled = m_ShotgunHitDisabled;
	Core.m_GrenadeHitDisabled = m_GrenadeHitDisabled;
	Core.m_LaserHitDisabled = m_LaserHitDisabled;
	Core.m_HasTelegunGun = m_HasTelegunGun;
	Core.m_HasTelegunGrenade = m_HasTelegunGrenade;
	Core.m_HasTelegunLaser = m_HasTelegunLaser;
	pChr->m_ZeroGravity = m_ZeroGravity;
	Core.m_LeftWall = m_LeftWall;
	pChr->m_LastRefillJumps = m_LastRefillJumps;
	pChr->m_LastPenalty = m_LastPenalty;
	pChr->m_LastBonus = m_LastBonus;
	Core.m_Colliding = m_Colliding;
	pChr->m_MoveRestrictions = m_MoveRestrictions;
	pChr->m_TuneZone = m_TuneZone;
	pChr->m_TeleCheckpoint = m_TeleCheckpoint;
	pChr->m_DDRaceState = (ERaceState)m_DDRaceState;
	pChr->m_StartTime = LoadTick(m_StartTime);
	pChr->m_SavedInput = m_SavedInput;
	pChr->m_Input = pChr->m_PrevInput = m_Input;
	pChr->m_LatestInput = pChr->m_LatestPrevInput = pChr->m_LatestPrevPrevInput = m_LatestInput;
	pChr->m_NumInputs = m_NumInputs;

	// Oldest first, the world lists new entities first.
	const int ClientId = pChr->GetPlayer()->GetCid();
	CGameWorld *pWorld = &pChr->GameServer()->m_World;
	for(auto pShot = m_vShots.rbegin(); pShot != m_vShots.rend(); ++pShot)
	{
		const CShot &Shot = *pShot;
		if(Shot.m_Laser)
		{
			// Without energy, the laser's first bounce only marks it for
			// destruction.
			CLaser *pLaser = new CLaser(pWorld, Shot.m_Pos, Shot.m_Dir, -1.0f, ClientId, Shot.m_Type);
			pLaser->m_MarkedForDestroy = false;
			pLaser->m_Layer = Shot.m_Layer;
			pLaser->m_Number = Shot.m_Number;
			pLaser->m_EvalTick = LoadTick(Shot.m_Tick);
			pLaser->m_TuneZone = Shot.m_TuneZone;
			pLaser->m_From = Shot.m_From;
			pLaser->m_PrevPos = Shot.m_PrevPos;
			pLaser->m_TelePos = Shot.m_TelePos;
			pLaser->m_Energy = Shot.m_Energy;
			pLaser->m_Bounces = Shot.m_Bounces;
			pLaser->m_WasTele = Shot.m_WasTele;
			pLaser->m_ZeroEnergyBounceInLastTick = Shot.m_ZeroEnergyBounceInLastTick;
			pLaser->m_TeleportCancelled = Shot.m_TeleportCancelled;
			pLaser->m_IsBlueTeleport = Shot.m_IsBlueTeleport;
		}
		else
		{
			CProjectile *pProjectile = new CProjectile(pWorld, Shot.m_Type, ClientId, Shot.m_Pos, Shot.m_Dir, Shot.m_LifeSpan, Shot.m_Freeze, Shot.m_Explosive, Shot.m_SoundImpact, Shot.m_InitDir, Shot.m_Layer, Shot.m_Number);
			pProjectile->m_StartTick = LoadTick(Shot.m_Tick);
			pProjectile->m_TuneZone = Shot.m_TuneZone;
			pProjectile->m_Bouncing = Shot.m_Bouncing;
		}
	}

	const int Team = pChr->Team();
	if(Team == TEAM_SUPER)
		return;
	std::vector<SSwitchers> &vSwitchers = pChr->GameServer()->Switchers();
	for(SSwitchers &Switcher : vSwitchers)
	{
		Switcher.m_aStatus[Team] = Switcher.m_Initial;
		Switcher.m_aEndTick[Team] = 0;
		Switcher.m_aType[Team] = TILE_SWITCHOPEN;
	}
	for(const CSwitch &Switch : m_vSwitches)
	{
		if(Switch.m_Number < 0 || Switch.m_Number >= (int)vSwitchers.size())
			continue;
		SSwitchers &Switcher = vSwitchers[Switch.m_Number];
		Switcher.m_aStatus[Team] = Switch.m_Status;
		Switcher.m_aType[Team] = Switch.m_Type;
		Switcher.m_aEndTick[Team] = LoadTick(Switch.m_EndTick);
	}
}

void CGTrainRunState::LoadHookedPlayer(CCharacter *pChr, int HookedPlayer)
{
	pChr->m_Core.SetHookedPlayer(HookedPlayer);
}

bool CGTrainRunState::operator==(const CGTrainRunState &Other) const
{
	// The encoding keeps every bit, the input structs have no comparison.
	CRunWriter Writer, OtherWriter;
	Transfer(*this, Writer);
	Transfer(Other, OtherWriter);
	return Writer.m_vData == OtherWriter.m_vData;
}

static void WriteRun(const CGTrainRun &Run, CRunWriter &Writer)
{
	CGTrainRunState::Transfer(Run.m_Start, Writer);
	Writer.Int(Run.m_KeptWeapons);
	Writer.Vec(Run.m_Goal);
	Writer.Int((int)Run.m_vInputs.size());
	Writer.Inputs(Run.m_vInputs, Run.m_Start.m_SavedInput);
	Writer.Int((int)Run.m_vRandom.size());
	int PrevTick = 0;
	for(const CGTrainRun::CRandom &Random : Run.m_vRandom)
	{
		Writer.Int(Random.m_Tick - PrevTick);
		Writer.Int(Random.m_Value);
		PrevTick = Random.m_Tick;
	}
	Writer.Vec(Run.m_EndPos);
	Writer.Vec(Run.m_EndVel);
}

static bool ReadRun(CGTrainRun &Run, CRunReader &Reader)
{
	CGTrainRunState::Transfer(Run.m_Start, Reader);
	Reader.Int(Run.m_KeptWeapons);
	Reader.Vec(Run.m_Goal);
	int NumInputs;
	Reader.Int(NumInputs);
	if(NumInputs < 0 || NumInputs > CGTrainRun::MAX_TICKS)
		return false;
	Reader.Inputs(Run.m_vInputs, NumInputs, Run.m_Start.m_SavedInput);
	int NumRandom;
	Reader.Int(NumRandom);
	if(NumRandom < 0 || NumRandom > Reader.Remaining())
		return false;
	Run.m_vRandom.resize(NumRandom);
	int Tick = 0;
	for(CGTrainRun::CRandom &Random : Run.m_vRandom)
	{
		int TickDifference;
		Reader.Int(TickDifference);
		Tick += TickDifference;
		Random.m_Tick = Tick;
		Reader.Int(Random.m_Value);
	}
	Reader.Vec(Run.m_EndPos);
	Reader.Vec(Run.m_EndVel);
	return !Reader.Error();
}

// The version, the raw size, then the raw data compressed.
static std::vector<unsigned char> Compress(int Version, const CRunWriter &Raw)
{
	CRunWriter Header;
	Header.Int(Version);
	Header.Int((int)Raw.m_vData.size());
	std::vector<unsigned char> vEncoded = Header.m_vData;
	const size_t HeaderSize = vEncoded.size();
	uLongf CompressedSize = compressBound(Raw.m_vData.size());
	vEncoded.resize(HeaderSize + CompressedSize);
	const int Result = compress2(vEncoded.data() + HeaderSize, &CompressedSize, Raw.m_vData.data(), Raw.m_vData.size(), Z_BEST_COMPRESSION);
	dbg_assert(Result == Z_OK, "compressing a GTrain run failed");
	vEncoded.resize(HeaderSize + CompressedSize);
	return vEncoded;
}

static bool Uncompress(int Version, const unsigned char *pData, int Size, std::vector<unsigned char> &vRaw)
{
	CRunReader Header(pData, Size);
	int EncodedVersion, RawSize;
	Header.Int(EncodedVersion);
	Header.Int(RawSize);
	if(Header.Error() || EncodedVersion != Version || RawSize <= 0 || RawSize > MAX_RAW_SIZE)
		return false;
	vRaw.resize(RawSize);
	uLongf UncompressedSize = RawSize;
	return uncompress(vRaw.data(), &UncompressedSize, Header.Current(), Header.Remaining()) == Z_OK && UncompressedSize == (uLongf)RawSize;
}

std::vector<unsigned char> CGTrainRun::Encode() const
{
	CRunWriter Raw;
	WriteRun(*this, Raw);
	return Compress(FORMAT_VERSION, Raw);
}

bool CGTrainRun::Decode(const unsigned char *pData, int Size)
{
	*this = CGTrainRun();
	std::vector<unsigned char> vRaw;
	if(!Uncompress(FORMAT_VERSION, pData, Size, vRaw))
		return false;
	CRunReader Reader(vRaw.data(), vRaw.size());
	return ReadRun(*this, Reader) && Reader.AtEnd();
}

static void WriteTeamRun(const CGTrainTeamRun &Run, CRunWriter &Writer)
{
	Writer.Int(Run.m_Team);
	Writer.Count(Run.m_vMembers);
	for(const CGTrainTeamRun::CMember &Member : Run.m_vMembers)
	{
		Writer.Str(Member.m_aName);
		Writer.Int(Member.m_ClientId);
		Writer.Int(Member.m_CaptureTicks);
		Writer.Int(Member.m_HookedPlayer);
		WriteRun(Member.m_Run, Writer);
	}
}

std::vector<unsigned char> CGTrainTeamRun::Encode() const
{
	CRunWriter Raw;
	WriteTeamRun(*this, Raw);
	return Compress(FORMAT_VERSION, Raw);
}

bool CGTrainTeamRun::Decode(const unsigned char *pData, int Size)
{
	*this = CGTrainTeamRun();
	std::vector<unsigned char> vRaw;
	if(!Uncompress(FORMAT_VERSION, pData, Size, vRaw))
		return false;
	CRunReader Reader(vRaw.data(), vRaw.size());
	Reader.Int(m_Team);
	Reader.Count(m_vMembers);
	if(Reader.Error() || m_vMembers.empty() || (int)m_vMembers.size() > MAX_CLIENTS)
		return false;
	for(CMember &Member : m_vMembers)
	{
		Reader.Str(Member.m_aName, sizeof(Member.m_aName));
		Reader.Int(Member.m_ClientId);
		Reader.Int(Member.m_CaptureTicks);
		Reader.Int(Member.m_HookedPlayer);
		if(!ReadRun(Member.m_Run, Reader) || Member.m_ClientId < 0 || Member.m_ClientId >= MAX_CLIENTS ||
			Member.m_Run.m_vInputs.size() != m_vMembers.front().m_Run.m_vInputs.size() ||
			Member.m_CaptureTicks < 0 || Member.m_CaptureTicks > (int)Member.m_Run.m_vInputs.size())
			return false;
	}
	return !Reader.Error() && Reader.AtEnd();
}

bool CGTrainTeamRun::operator==(const CGTrainTeamRun &Other) const
{
	CRunWriter Writer, OtherWriter;
	WriteTeamRun(*this, Writer);
	WriteTeamRun(Other, OtherWriter);
	return Writer.m_vData == OtherWriter.m_vData;
}

int GTrainRunVersion(const unsigned char *pData, int Size)
{
	CRunReader Header(pData, Size);
	int Version;
	Header.Int(Version);
	return Header.Error() ? 0 : Version;
}

bool CGTrainRun::operator==(const CGTrainRun &Other) const
{
	CRunWriter Writer, OtherWriter;
	WriteRun(*this, Writer);
	WriteRun(Other, OtherWriter);
	return Writer.m_vData == OtherWriter.m_vData;
}

void KeepGTrainWeapons(CCharacter *pChr, int Weapons)
{
	// Giving them every tick also keeps ninja from running out.
	for(int Weapon = WEAPON_SHOTGUN; Weapon <= WEAPON_NINJA; ++Weapon)
		if(Weapons & (1 << Weapon))
			pChr->GiveWeapon(Weapon);
}

bool ReplayGTrainRun(CGameContext *pGameServer, int ClientId, const CGTrainRun &Run, const std::function<void()> &AdvanceTick, const std::function<void(CCharacter *pChr)> &AfterTick)
{
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientId];
	if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
		return false;
	// A new character like every GTrain attempt has. GTrain also removes the
	// shots of earlier attempts, the state brings back those of the run.
	pPlayer->KillCharacter(WEAPON_GAME, false, false);
	pGameServer->m_World.RemoveEntitiesFromPlayer(ClientId);
	CCharacter *pChr = pPlayer->ForceSpawn(Run.m_Start.m_Pos);
	Run.m_Start.Load(pChr);

	// Random choices for this tee come from the run, in their order.
	CWorldCore &WorldCore = pGameServer->m_World.m_Core;
	const auto RandomHook = std::move(WorldCore.m_RandomHook);
	size_t NextRandom = 0;
	WorldCore.m_RandomHook = [&](int RandomClientId, int Value) {
		if(RandomClientId != ClientId || NextRandom == Run.m_vRandom.size())
			return Value;
		return Run.m_vRandom[NextRandom++].m_Value;
	};
	for(const CNetObj_PlayerInput &Input : Run.m_vInputs)
	{
		pGameServer->OnClientPredictedEarlyInput(ClientId, &Input);
		AdvanceTick();
		pGameServer->OnClientPredictedInput(ClientId, &Input);
		pGameServer->OnTick();
		pChr = pGameServer->GetPlayerChar(ClientId);
		if(!pChr)
			break;
		KeepGTrainWeapons(pChr, Run.m_KeptWeapons);
		if(AfterTick)
			AfterTick(pChr);
	}
	WorldCore.m_RandomHook = RandomHook;
	return pChr && pChr->Core()->m_Pos == Run.m_EndPos && pChr->Core()->m_Vel == Run.m_EndVel && NextRandom == Run.m_vRandom.size();
}

bool ReplayGTrainTeamRun(CGameContext *pGameServer, const CGTrainTeamRun &Run, const std::function<void()> &AdvanceTick, const std::function<void(int Member, CCharacter *pChr)> &AfterTick)
{
	for(const CGTrainTeamRun::CMember &Member : Run.m_vMembers)
	{
		CPlayer *pPlayer = pGameServer->m_apPlayers[Member.m_ClientId];
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			return false;
		pPlayer->KillCharacter(WEAPON_GAME, false, false);
		pGameServer->m_World.RemoveEntitiesFromPlayer(Member.m_ClientId);
	}
	// The world moves the newest character first, so the last member to move
	// spawns first. All of them are in the team before any state loads its
	// switches, and hooks hold teammates once they all exist.
	for(auto pMember = Run.m_vMembers.rbegin(); pMember != Run.m_vMembers.rend(); ++pMember)
	{
		pGameServer->m_apPlayers[pMember->m_ClientId]->ForceSpawn(pMember->m_Run.m_Start.m_Pos);
		pGameServer->m_pController->Teams().SetForceCharacterTeam(pMember->m_ClientId, Run.m_Team);
	}
	for(const CGTrainTeamRun::CMember &Member : Run.m_vMembers)
		Member.m_Run.m_Start.Load(pGameServer->GetPlayerChar(Member.m_ClientId));
	for(const CGTrainTeamRun::CMember &Member : Run.m_vMembers)
		if(Member.m_HookedPlayer >= 0 && Member.m_HookedPlayer < MAX_CLIENTS)
			CGTrainRunState::LoadHookedPlayer(pGameServer->GetPlayerChar(Member.m_ClientId), Member.m_HookedPlayer);

	// Each member's random choices come from their run, in their order.
	CWorldCore &WorldCore = pGameServer->m_World.m_Core;
	const auto RandomHook = std::move(WorldCore.m_RandomHook);
	std::vector<size_t> vNextRandom(Run.m_vMembers.size(), 0);
	WorldCore.m_RandomHook = [&](int RandomClientId, int Value) {
		for(size_t i = 0; i < Run.m_vMembers.size(); ++i)
		{
			const std::vector<CGTrainRun::CRandom> &vRandom = Run.m_vMembers[i].m_Run.m_vRandom;
			if(Run.m_vMembers[i].m_ClientId == RandomClientId && vNextRandom[i] < vRandom.size())
				return vRandom[vNextRandom[i]++].m_Value;
		}
		return Value;
	};
	// The server applies inputs by client id.
	std::vector<size_t> vByClientId(Run.m_vMembers.size());
	for(size_t i = 0; i < vByClientId.size(); ++i)
		vByClientId[i] = i;
	std::sort(vByClientId.begin(), vByClientId.end(), [&](size_t A, size_t B) { return Run.m_vMembers[A].m_ClientId < Run.m_vMembers[B].m_ClientId; });
	bool Alive = true;
	const size_t NumTicks = Run.m_vMembers.front().m_Run.m_vInputs.size();
	for(size_t Tick = 0; Tick < NumTicks && Alive; ++Tick)
	{
		for(const size_t Member : vByClientId)
			pGameServer->OnClientPredictedEarlyInput(Run.m_vMembers[Member].m_ClientId, &Run.m_vMembers[Member].m_Run.m_vInputs[Tick]);
		AdvanceTick();
		for(const size_t Member : vByClientId)
			pGameServer->OnClientPredictedInput(Run.m_vMembers[Member].m_ClientId, &Run.m_vMembers[Member].m_Run.m_vInputs[Tick]);
		pGameServer->OnTick();
		for(size_t i = 0; i < Run.m_vMembers.size(); ++i)
		{
			CCharacter *pChr = pGameServer->GetPlayerChar(Run.m_vMembers[i].m_ClientId);
			if(!pChr)
			{
				Alive = false;
				continue;
			}
			KeepGTrainWeapons(pChr, Run.m_vMembers[i].m_Run.m_KeptWeapons);
			if(AfterTick)
				AfterTick(i, pChr);
		}
	}
	WorldCore.m_RandomHook = RandomHook;
	if(!Alive)
		return false;
	for(size_t i = 0; i < Run.m_vMembers.size(); ++i)
	{
		const CGTrainRun &MemberRun = Run.m_vMembers[i].m_Run;
		const CCharacter *pChr = pGameServer->GetPlayerChar(Run.m_vMembers[i].m_ClientId);
		if(pChr->Core()->m_Pos != MemberRun.m_EndPos || pChr->Core()->m_Vel != MemberRun.m_EndVel || vNextRandom[i] != MemberRun.m_vRandom.size())
			return false;
	}
	return true;
}
