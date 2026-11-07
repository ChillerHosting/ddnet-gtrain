#include <base/mem.h>

#include <game/mapitems.h>
#include <game/server/gamemodes/gtrain_run.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cmath>

static CGTrainRun ExampleRun(int Ticks)
{
	CGTrainRun Run;
	CGTrainRunState &Start = Run.m_Start;
	Start.m_Tick = 123456;
	Start.m_Pos = vec2(1616, 848);
	Start.m_PrevPos = vec2(1616, 846);
	// Quantized velocities are multiples of 1/256, directions are not.
	Start.m_Vel = vec2(-3.5f, 12.34765625f);
	Start.m_HookDir = normalize(vec2(3, -7));
	Start.m_NinjaActivationDir = vec2(-0.0f, 1);
	Start.m_Jumped = 2;
	Start.m_JumpedTotal = 1;
	Start.m_Jumps = 2;
	Start.m_ActiveWeapon = WEAPON_GRENADE;
	Start.m_LastWeapon = WEAPON_GUN;
	Start.m_aWeaponGot[WEAPON_HAMMER] = Start.m_aWeaponGot[WEAPON_GUN] = Start.m_aWeaponGot[WEAPON_GRENADE] = true;
	std::fill(std::begin(Start.m_aWeaponAmmo), std::end(Start.m_aWeaponAmmo), -1);
	Start.m_FreezeStart = 123400;
	Start.m_Solo = Start.m_ZeroGravity = Start.m_LeftWall = true;
	Start.m_MoveRestrictions = 4;
	Start.m_DDRaceState = 3;
	Start.m_SavedInput.m_TargetX = 100;
	Start.m_SavedInput.m_TargetY = -40;
	Start.m_SavedInput.m_Jump = 1;
	Start.m_SavedInput.m_Fire = 7;
	Start.m_Input = Start.m_SavedInput;
	Start.m_Input.m_Jump = 0;
	Start.m_LatestInput = Start.m_SavedInput;
	Start.m_LatestInput.m_PlayerFlags = 1;
	Start.m_NumInputs = 2;
	Start.m_vSwitches = {{3, true, TILE_SWITCHTIMEDOPEN, 123500}, {7, false, TILE_SWITCHCLOSE, 0}};
	CGTrainRunState::CShot Laser;
	Laser.m_Laser = true;
	Laser.m_Type = WEAPON_SHOTGUN;
	Laser.m_Pos = vec2(1700.25f, 830.5f);
	Laser.m_Dir = normalize(vec2(1, -0.3f));
	Laser.m_Tick = Start.m_Tick;
	Laser.m_From = Start.m_Pos;
	Laser.m_PrevPos = Start.m_Pos;
	Laser.m_Energy = 712.345f;
	Laser.m_Bounces = 1;
	Laser.m_ZeroEnergyBounceInLastTick = true;
	CGTrainRunState::CShot Grenade;
	Grenade.m_Type = WEAPON_GRENADE;
	Grenade.m_Pos = vec2(1626.5f, 840.25f);
	Grenade.m_Dir = normalize(vec2(1, -1));
	Grenade.m_InitDir = Grenade.m_Dir;
	Grenade.m_Tick = Start.m_Tick - 3;
	Grenade.m_LifeSpan = 100;
	Grenade.m_SoundImpact = 7;
	Grenade.m_Explosive = true;
	Start.m_vShots = {Laser, Grenade};
	Run.m_KeptWeapons = 1 << WEAPON_GRENADE;
	Run.m_Goal = vec2(2000, 848);
	// A mouse moving in small steps, keys pressed now and then, and stretches
	// without any change.
	CNetObj_PlayerInput Input = Start.m_SavedInput;
	for(int Tick = 0; Tick < Ticks; ++Tick)
	{
		if(Tick % 40 < 30)
		{
			Input.m_TargetX += (Tick * 7) % 11 - 5;
			Input.m_TargetY += (Tick * 13) % 7 - 3;
		}
		if(Tick % 25 == 0)
			Input.m_Direction = (Tick / 25) % 3 - 1;
		if(Tick % 17 == 0)
			Input.m_Jump ^= 1;
		if(Tick % 31 == 0)
			Input.m_Hook ^= 1;
		if(Tick % 45 == 0)
			++Input.m_Fire;
		if(Tick % 90 == 0)
			Input.m_WantedWeapon = 1 + (Tick / 90) % NUM_WEAPONS;
		Run.m_vInputs.push_back(Input);
	}
	Run.m_vRandom = {{10, 2}, {10, 0}, {200, 5}};
	Run.m_EndPos = vec2(1990, 840);
	Run.m_EndVel = vec2(15.5f, -0.25f);
	return Run;
}

static CGTrainRun Decoded(const std::vector<unsigned char> &vEncoded)
{
	CGTrainRun Run;
	EXPECT_TRUE(Run.Decode(vEncoded.data(), vEncoded.size()));
	return Run;
}

TEST(GTrainRun, EncodingKeepsEverything)
{
	const CGTrainRun Run = ExampleRun(500);
	const CGTrainRun Result = Decoded(Run.Encode());
	EXPECT_TRUE(Result == Run);
	EXPECT_TRUE(Result.m_Start == Run.m_Start);
	ASSERT_EQ(Result.m_vInputs.size(), 500u);
	EXPECT_EQ(mem_comp(Result.m_vInputs.data(), Run.m_vInputs.data(), 500 * sizeof(CNetObj_PlayerInput)), 0);
	EXPECT_EQ(Result.m_Start.m_Vel, Run.m_Start.m_Vel);
	EXPECT_EQ(Result.m_Start.m_HookDir, Run.m_Start.m_HookDir);
	EXPECT_TRUE(std::signbit(Result.m_Start.m_NinjaActivationDir.x));
	EXPECT_EQ(Result.m_Start.m_vSwitches, Run.m_Start.m_vSwitches);
	ASSERT_EQ(Result.m_Start.m_vShots.size(), 2u);
	EXPECT_TRUE(Result.m_Start.m_vShots[0].m_Laser);
	EXPECT_EQ(Result.m_Start.m_vShots[0].m_Energy, 712.345f);
	EXPECT_TRUE(Result.m_Start.m_vShots[0].m_ZeroEnergyBounceInLastTick);
	EXPECT_FALSE(Result.m_Start.m_vShots[1].m_Laser);
	EXPECT_TRUE(Result.m_Start.m_vShots[1].m_Explosive);
	EXPECT_EQ(Result.m_Start.m_vShots[1].m_Dir, Run.m_Start.m_vShots[1].m_Dir);
	EXPECT_EQ(Result.m_vRandom, Run.m_vRandom);
	EXPECT_EQ(Result.m_EndPos, Run.m_EndPos);
	EXPECT_EQ(Result.m_EndVel, Run.m_EndVel);
	EXPECT_EQ(Result.m_KeptWeapons, Run.m_KeptWeapons);
}

TEST(GTrainRun, ComparisonSeesSmallDifferences)
{
	const CGTrainRun Run = ExampleRun(50);
	CGTrainRun Other = Run;
	EXPECT_TRUE(Other == Run);
	Other.m_Start.m_Vel.x = std::nextafter(Other.m_Start.m_Vel.x, 0.0f);
	EXPECT_FALSE(Other == Run);
	Other = Run;
	Other.m_vInputs[20].m_PrevWeapon = 1;
	EXPECT_FALSE(Other == Run);
	Other = Run;
	Other.m_Start.m_LatestInput.m_TargetY = 0;
	EXPECT_FALSE(Other == Run);
}

TEST(GTrainRun, InstantCapturesHaveNoInputs)
{
	CGTrainRun Run = ExampleRun(0);
	Run.m_vRandom.clear();
	EXPECT_TRUE(Decoded(Run.Encode()) == Run);
}

TEST(GTrainRun, ClientInputsCanBeAnything)
{
	CGTrainRun Run = ExampleRun(4);
	for(int Value : {INT_MIN, INT_MAX, -1, 0})
	{
		CNetObj_PlayerInput Input;
		mem_zero(&Input, sizeof(Input));
		Input.m_TargetX = Value;
		Input.m_TargetY = -Value - 1;
		Input.m_Fire = Value;
		Input.m_PlayerFlags = Value;
		Input.m_PrevWeapon = Value;
		Run.m_vInputs.push_back(Input);
	}
	Run.m_Start.m_SavedInput.m_TargetX = INT_MIN;
	const CGTrainRun Result = Decoded(Run.Encode());
	EXPECT_TRUE(Result == Run);
	EXPECT_EQ(Result.m_vInputs.back().m_PrevWeapon, 0);
	EXPECT_EQ(Result.m_vInputs[5].m_TargetX, INT_MAX);
}

TEST(GTrainRun, UnusualFloatsKeepTheirBits)
{
	CGTrainRun Run = ExampleRun(1);
	Run.m_Start.m_Pos = vec2(1e-30f, -123456.789f);
	Run.m_Start.m_Vel = vec2(INFINITY, (float)(1 << 30));
	Run.m_EndVel = vec2(-0.0f, 0.1f);
	const CGTrainRun Result = Decoded(Run.Encode());
	EXPECT_TRUE(Result == Run);
	EXPECT_EQ(Result.m_Start.m_Pos, Run.m_Start.m_Pos);
	EXPECT_EQ(Result.m_Start.m_Vel, Run.m_Start.m_Vel);
	EXPECT_TRUE(std::signbit(Result.m_EndVel.x));
	EXPECT_EQ(Result.m_EndVel.y, 0.1f);
}

TEST(GTrainRun, DamagedDataIsRejected)
{
	const std::vector<unsigned char> vEncoded = ExampleRun(100).Encode();
	CGTrainRun Run;
	for(size_t Size = 0; Size < vEncoded.size(); ++Size)
		EXPECT_FALSE(Run.Decode(vEncoded.data(), Size)) << Size;
	std::vector<unsigned char> vOtherVersion = vEncoded;
	vOtherVersion[0] = CGTrainRun::FORMAT_VERSION + 1;
	EXPECT_FALSE(Run.Decode(vOtherVersion.data(), vOtherVersion.size()));
	std::vector<unsigned char> vDamaged = vEncoded;
	vDamaged[vDamaged.size() / 2] ^= 0x55;
	EXPECT_FALSE(Run.Decode(vDamaged.data(), vDamaged.size()));
}

TEST(GTrainRun, BusyInputsStayCompact)
{
	// Ten seconds of input changing in almost every tick.
	EXPECT_LT(ExampleRun(500).Encode().size(), 500u * 2);
}
