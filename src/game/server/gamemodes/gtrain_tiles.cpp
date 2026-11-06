#include "gtrain_tiles.h"

#include <base/detect.h>
#include <base/io.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <game/collision.h>
#include <game/mapitems.h>

#include <ddnet_physics/ddnet_physics.h>

#include <algorithm>
#include <cmath>
#include <utility>

#if defined(CONF_PLATFORM_LINUX)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
	constexpr char RATINGS_MAGIC[4] = {'G', 'T', 'R', 'T'};

	// a bit shorter than the hook, which flies from the tee's edge
	constexpr float HOOK_REACH = 370.0f;

	// How much each part of a spot counts, in steps of the KoG ratings,
	// fitted to the races of the rated KoG maps, see SpotDifficulty().
	// Over 664 maps they rank the maps like their ratings with a Spearman
	// of 0.965, and the Insane and Extreme ones with 0.816 (survival alone:
	// 0.954 and 0.774), cross-validated.
	constexpr float WEIGHT_CONSTANT = -14.703f;
	constexpr float WEIGHT_SURVIVAL = 9.070f;
	constexpr float WEIGHT_HOOKABLE = 1.019f;
	constexpr float WEIGHT_UNHOOKABLE = 6.338f;
	constexpr float WEIGHT_OPEN = 3.715f;
	constexpr float WEIGHT_DEPTH = -0.655f;
	constexpr float WEIGHT_CEILING = 0.294f;
	constexpr float WEIGHT_SURVIVAL_OPEN = -1.690f;

	void WriteInt(std::vector<uint8_t> &vData, int Value)
	{
		for(int Byte = 0; Byte < 4; ++Byte)
			vData.push_back((uint32_t)Value >> (8 * Byte) & 0xff);
	}

	bool ReadInt(const std::vector<uint8_t> &vData, size_t &Offset, int &Value)
	{
		if(Offset + 4 > vData.size())
			return false;
		uint32_t Result = 0;
		for(int Byte = 0; Byte < 4; ++Byte)
			Result |= (uint32_t)vData[Offset + Byte] << (8 * Byte);
		Offset += 4;
		Value = (int)Result;
		return true;
	}

	uint32_t SplitMix(uint64_t &State)
	{
		uint64_t Value = (State += 0x9e3779b97f4a7c15ULL);
		Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ULL;
		Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebULL;
		return (uint32_t)((Value ^ (Value >> 31)) >> 32);
	}

	// See CGTrainRatingSettings.
	class CRandomInput
	{
		const CGTrainRatingSettings &m_Settings;
		const ddnet_collision_t *m_pCollision;
		uint64_t m_State;
		int m_TicksLeft = 0;
		bool m_JumpNow = false;
		ddnet_input_t m_Input = {};

		uint32_t Next() { return SplitMix(m_State); }
		bool Chance(int Percent) { return (int)(Next() % 100) < Percent; }

		// Tries a few directions for one in which the hook would grab.
		bool AimAtHookable(ddnet_vec2_t Pos)
		{
			for(int Try = 0; Try < 8; ++Try)
			{
				const float Angle = Next() / 4294967296.0f * 2.0f * pi;
				const ddnet_vec2_t Direction = {std::cos(Angle), std::sin(Angle)};
				const ddnet_vec2_t End = {Pos.x + Direction.x * HOOK_REACH, Pos.y + Direction.y * HOOK_REACH};
				if(ddnet_collision_intersect_line(m_pCollision, Pos, End, nullptr, nullptr) == TILE_SOLID)
				{
					m_Input.target_x = (int)(Direction.x * 100.0f);
					m_Input.target_y = (int)(Direction.y * 100.0f);
					return true;
				}
			}
			return false;
		}

	public:
		CRandomInput(const CGTrainRatingSettings &Settings, const ddnet_collision_t *pCollision, uint64_t Seed) :
			m_Settings(Settings), m_pCollision(pCollision), m_State(Seed) {}

		void Apply(ddnet_input_t &Input, ddnet_vec2_t Pos)
		{
			if(m_TicksLeft == 0)
			{
				m_TicksLeft = m_Settings.m_MinHoldTicks + Next() % (m_Settings.m_MaxHoldTicks - m_Settings.m_MinHoldTicks + 1);
				m_Input.direction = (int)(Next() % 3) - 1;
				if(!Chance(m_Settings.m_AimPercent) || !AimAtHookable(Pos))
				{
					m_Input.target_x = (int)(Next() % 201) - 100;
					m_Input.target_y = (int)(Next() % 201) - 100;
					if(m_Input.target_x == 0 && m_Input.target_y == 0)
						m_Input.target_y = -100;
				}
				// A held hook stays where it is when the next inputs hold it too.
				m_Input.hook = Chance(m_Settings.m_HookPercent);
				// Jumps are pressed for one tick, so that the next one counts.
				m_JumpNow = Chance(m_Settings.m_JumpPercent);
			}
			--m_TicksLeft;
			Input.direction = m_Input.direction;
			Input.target_x = m_Input.target_x;
			Input.target_y = m_Input.target_y;
			Input.hook = m_Input.hook;
			Input.jump = m_JumpNow;
			m_JumpNow = false;
		}
	};
} // namespace

float CGTrainTileRatings::SpotDifficulty(int Spot) const
{
	int Survived = 0;
	for(int Sample = 0; Sample < m_NumSamples; ++Sample)
		Survived += m_vSurvivedTicks[(size_t)Spot * m_NumSamples + Sample];
	// Half a tick for tees that freeze right away.
	const float Survival = std::log(m_MaxTicks / std::max(Survived / (float)m_NumSamples, 0.5f));
	const uint8_t *pFeatures = &m_vFeatures[(size_t)Spot * NUM_FEATURES];
	const float Open = pFeatures[FEATURE_OPEN];
	return WEIGHT_CONSTANT + WEIGHT_SURVIVAL * Survival +
	       WEIGHT_HOOKABLE * pFeatures[FEATURE_HOOKABLE] / NUM_DIRECTIONS + WEIGHT_UNHOOKABLE * pFeatures[FEATURE_UNHOOKABLE] / NUM_DIRECTIONS +
	       WEIGHT_OPEN * Open + WEIGHT_DEPTH * pFeatures[FEATURE_DEPTH] + WEIGHT_CEILING * pFeatures[FEATURE_CEILING] +
	       WEIGHT_SURVIVAL_OPEN * Survival * Open;
}

std::vector<float> CGTrainTileRatings::TileDifficulties() const
{
	std::vector<float> vDifficulties((size_t)m_Width * m_Height, UNRATED);
	for(int Spot = 0; Spot < (int)m_vSpots.size(); ++Spot)
		vDifficulties[m_vSpots[Spot]] = SpotDifficulty(Spot);
	return vDifficulties;
}

std::vector<float> BlurTileDifficulties(const std::vector<float> &vDifficulties, int Width, int Radius)
{
	if(Width <= 0 || Radius <= 0)
		return vDifficulties;
	const int Height = (int)(vDifficulties.size() / Width);
	std::vector<std::pair<int, int>> vOffsets;
	for(int y = -Radius; y <= Radius; ++y)
		for(int x = -Radius; x <= Radius; ++x)
			if(x * x + y * y <= Radius * Radius + Radius)
				vOffsets.emplace_back(x, y);
	std::vector<float> vBlurred(vDifficulties.size(), CGTrainTileRatings::UNRATED);
	for(int y = 0; y < Height; ++y)
		for(int x = 0; x < Width; ++x)
		{
			if(!CGTrainTileRatings::IsRated(vDifficulties[(size_t)y * Width + x]))
				continue;
			double Sum = 0;
			int Count = 0;
			for(const auto &[OffsetX, OffsetY] : vOffsets)
			{
				const int OtherX = x + OffsetX, OtherY = y + OffsetY;
				if(OtherX < 0 || OtherY < 0 || OtherX >= Width || OtherY >= Height)
					continue;
				const float Difficulty = vDifficulties[(size_t)OtherY * Width + OtherX];
				if(CGTrainTileRatings::IsRated(Difficulty))
				{
					Sum += Difficulty;
					++Count;
				}
			}
			vBlurred[(size_t)y * Width + x] = (float)(Sum / Count);
		}
	return vBlurred;
}

bool CGTrainTileRatings::Save(const char *pFilename) const
{
	std::vector<uint8_t> vData(std::begin(RATINGS_MAGIC), std::end(RATINGS_MAGIC));
	WriteInt(vData, VERSION);
	vData.insert(vData.end(), std::begin(m_MapSha256.data), std::end(m_MapSha256.data));
	for(int Value : {m_Width, m_Height, m_MaxTicks, m_NumSamples, (int)m_vSpots.size()})
		WriteInt(vData, Value);
	for(int Spot = 0; Spot < (int)m_vSpots.size(); ++Spot)
	{
		WriteInt(vData, m_vSpots[Spot]);
		const auto Samples = m_vSurvivedTicks.begin() + (size_t)Spot * m_NumSamples;
		vData.insert(vData.end(), Samples, Samples + m_NumSamples);
		const auto Features = m_vFeatures.begin() + (size_t)Spot * NUM_FEATURES;
		vData.insert(vData.end(), Features, Features + NUM_FEATURES);
	}
	IOHANDLE File = io_open(pFilename, IOFLAG_WRITE);
	if(!File)
		return false;
	const bool Written = io_write(File, vData.data(), vData.size()) == vData.size();
	return io_close(File) == 0 && Written;
}

bool CGTrainTileRatings::Load(const char *pFilename, const SHA256_DIGEST &MapSha256)
{
	IOHANDLE File = io_open(pFilename, IOFLAG_READ);
	if(!File)
		return false;
	const int64_t Length = io_length(File);
	std::vector<uint8_t> vData(std::max<int64_t>(Length, 0));
	const bool Read = Length >= 0 && io_read(File, vData.data(), vData.size()) == vData.size();
	io_close(File);
	if(!Read || vData.size() < sizeof(RATINGS_MAGIC) || mem_comp(vData.data(), RATINGS_MAGIC, sizeof(RATINGS_MAGIC)) != 0)
		return false;
	size_t Offset = sizeof(RATINGS_MAGIC);
	int Version, MaxTicks, NumSamples, NumSpots;
	if(!ReadInt(vData, Offset, Version) || Version != VERSION || Offset + sizeof(MapSha256.data) > vData.size())
		return false;
	SHA256_DIGEST Sha256;
	mem_copy(Sha256.data, vData.data() + Offset, sizeof(Sha256.data));
	Offset += sizeof(Sha256.data);
	if(sha256_comp(Sha256, MapSha256) != 0 ||
		!ReadInt(vData, Offset, m_Width) || !ReadInt(vData, Offset, m_Height) || !ReadInt(vData, Offset, MaxTicks) ||
		!ReadInt(vData, Offset, NumSamples) || !ReadInt(vData, Offset, NumSpots) ||
		MaxTicks < 1 || MaxTicks > MAX_TICKS || NumSamples < 1 || NumSpots < 0 || m_Width <= 0 || m_Height <= 0 ||
		vData.size() - Offset != (size_t)NumSpots * (4 + NumSamples + NUM_FEATURES))
		return false;
	m_MapSha256 = Sha256;
	m_MaxTicks = MaxTicks;
	m_NumSamples = NumSamples;
	m_vSpots.resize(NumSpots);
	m_vSurvivedTicks.clear();
	m_vFeatures.clear();
	for(int Spot = 0; Spot < NumSpots; ++Spot)
	{
		ReadInt(vData, Offset, m_vSpots[Spot]);
		if(m_vSpots[Spot] < 0 || m_vSpots[Spot] >= m_Width * m_Height)
			return false;
		m_vSurvivedTicks.insert(m_vSurvivedTicks.end(), vData.begin() + Offset, vData.begin() + Offset + NumSamples);
		Offset += NumSamples;
		m_vFeatures.insert(m_vFeatures.end(), vData.begin() + Offset, vData.begin() + Offset + NUM_FEATURES);
		Offset += NUM_FEATURES;
	}
	return true;
}

bool CGTrainRatingSettings::Parse(const char *pSettings)
{
	const std::pair<const char *, int *> aMembers[] = {
		{"samples", &m_Samples},
		{"maxticks", &m_MaxTicks},
		{"minholdticks", &m_MinHoldTicks},
		{"maxholdticks", &m_MaxHoldTicks},
		{"aimpercent", &m_AimPercent},
		{"hookpercent", &m_HookPercent},
		{"jumppercent", &m_JumpPercent},
		{"startspeed", &m_StartSpeed},
	};
	while(*pSettings)
	{
		char aSetting[64];
		const char *pEnd = str_find(pSettings, ",");
		str_truncate(aSetting, sizeof(aSetting), pSettings, pEnd ? pEnd - pSettings : str_length(pSettings));
		pSettings = pEnd ? pEnd + 1 : pSettings + str_length(pSettings);
		const char *pValue = str_find(aSetting, "=");
		if(!pValue)
			return false;
		bool Found = false;
		for(const auto &[pName, pMember] : aMembers)
			if(str_comp_num(aSetting, pName, pValue - aSetting) == 0 && str_length(pName) == pValue - aSetting)
			{
				*pMember = str_toint(pValue + 1);
				Found = true;
			}
		if(!Found)
			return false;
	}
	m_Samples = std::max(m_Samples, 1);
	m_MaxTicks = std::clamp<int>(m_MaxTicks, 1, CGTrainTileRatings::MAX_TICKS);
	m_MinHoldTicks = std::max(m_MinHoldTicks, 1);
	m_MaxHoldTicks = std::max(m_MaxHoldTicks, m_MinHoldTicks);
	return true;
}

void CGTrainTileMapInfo::Init(CCollision *pCollision, const std::vector<int> &vTileNodes)
{
	m_Width = pCollision->GetWidth();
	m_Height = pCollision->GetHeight();
	const int NumTiles = m_Width * m_Height;
	const auto Has = [&](int Index, int Tile) { return pCollision->GetTileIndex(Index) == Tile || pCollision->GetFrontTileIndex(Index) == Tile; };
	const auto Blocked = [&](int Index) { return pCollision->GetTileIndex(Index) == TILE_SOLID || pCollision->GetTileIndex(Index) == TILE_NOHOOK; };

	m_vReturnTiles.assign(NumTiles, false);
	for(int Number = 0; Number < 256; ++Number)
		for(const vec2 &Pos : pCollision->TeleCheckOuts(Number))
			m_vReturnTiles[pCollision->GetPureMapIndex(Pos)] = true;
	for(int Index = 0; Index < NumTiles; ++Index)
		for(int Spawn : {ENTITY_SPAWN, ENTITY_SPAWN_RED, ENTITY_SPAWN_BLUE})
			if(Has(Index, ENTITY_OFFSET + Spawn))
				m_vReturnTiles[Index] = true;

	// The distance to the nearest tile paths can't go through, diagonals
	// counting as one, with the outside one tile beyond the border.
	std::vector<int> vDistances(NumTiles, -1);
	std::vector<int> vQueue;
	for(int Index = 0; Index < NumTiles; ++Index)
		if(vTileNodes[Index] < 0)
		{
			vDistances[Index] = 0;
			vQueue.push_back(Index);
		}
	// The border tiles come right after all walls, which keeps the queue
	// ordered by distance.
	for(int Index = 0; Index < NumTiles; ++Index)
	{
		const int TileX = Index % m_Width, TileY = Index / m_Width;
		if(vDistances[Index] < 0 && (TileX == 0 || TileY == 0 || TileX == m_Width - 1 || TileY == m_Height - 1))
		{
			vDistances[Index] = 1;
			vQueue.push_back(Index);
		}
	}
	for(size_t Head = 0; Head < vQueue.size(); ++Head)
	{
		const int Index = vQueue[Head];
		const int TileX = Index % m_Width, TileY = Index / m_Width;
		for(int dy = -1; dy <= 1; ++dy)
			for(int dx = -1; dx <= 1; ++dx)
			{
				if(TileX + dx < 0 || TileX + dx >= m_Width || TileY + dy < 0 || TileY + dy >= m_Height)
					continue;
				const int Neighbor = Index + dy * m_Width + dx;
				if(vDistances[Neighbor] < 0)
				{
					vDistances[Neighbor] = vDistances[Index] + 1;
					vQueue.push_back(Neighbor);
				}
			}
	}
	m_vOpen.resize(NumTiles);
	for(int Index = 0; Index < NumTiles; ++Index)
		m_vOpen[Index] = std::min<int>(vDistances[Index], CGTrainTileRatings::MAX_OPEN);

	// Down to anything that stops the fall, and up to the ceiling.
	m_vDepth.resize(NumTiles);
	m_vCeiling.resize(NumTiles);
	for(int TileX = 0; TileX < m_Width; ++TileX)
	{
		for(int TileY = m_Height - 1; TileY >= 0; --TileY)
		{
			const int Index = TileY * m_Width + TileX;
			const int Below = Index + m_Width;
			const bool Stops = TileY + 1 < m_Height && (Blocked(Below) || Has(Below, TILE_FREEZE) || Has(Below, TILE_DFREEZE) || Has(Below, TILE_DEATH));
			m_vDepth[Index] = TileY + 1 >= m_Height || Stops ? 1 : std::min<int>(m_vDepth[Below] + 1, CGTrainTileRatings::MAX_DEPTH);
		}
		for(int TileY = 0; TileY < m_Height; ++TileY)
		{
			const int Index = TileY * m_Width + TileX;
			const int Above = Index - m_Width;
			m_vCeiling[Index] = TileY == 0 || Blocked(Above) ? 1 : std::min<int>(m_vCeiling[Above] + 1, CGTrainTileRatings::MAX_DEPTH);
		}
	}
}

CGTrainTileSimulator::CGTrainTileSimulator(const ddnet_collision_t *pCollision, const CGTrainTileMapInfo &MapInfo, const CGTrainRatingSettings &Settings) :
	m_pCollision(pCollision),
	m_MapInfo(MapInfo),
	m_pStart(std::make_unique<ddnet_world_t>()),
	m_pWorld(std::make_unique<ddnet_world_t>()),
	m_Settings(Settings)
{
	// The settings GTrain forces, see CGameContext::OnInit.
	ddnet_config_t Config = ddnet_config_default(DDNET_MODE_DDRACE);
	ddnet_config_set(&Config, "sv_solo_server", 1);
	ddnet_config_set(&Config, "sv_team", DDNET_SV_TEAM_FORCED_SOLO);
	ddnet_config_set(&Config, "sv_no_weak_hook", 1);
	m_Valid = ddnet_world_init(m_pStart.get(), pCollision, &Config) &&
		  ddnet_player_join(m_pStart.get(), 0) &&
		  ddnet_player_spawn(m_pStart.get(), 0) &&
		  ddnet_world_copy(m_pWorld.get(), m_pStart.get());
}

CGTrainTileSimulator::~CGTrainTileSimulator()
{
	ddnet_world_free(m_pWorld.get());
	ddnet_world_free(m_pStart.get());
}

int CGTrainTileSimulator::TileAt(float x, float y) const
{
	if(x < 0 || y < 0 || x >= m_MapInfo.m_Width * 32.0f || y >= m_MapInfo.m_Height * 32.0f)
		return -1;
	return (int)(y / 32) * m_MapInfo.m_Width + (int)(x / 32);
}

void CGTrainTileSimulator::Rate(int Index, uint8_t *pSurvivedTicks, uint8_t *pFeatures)
{
	const ddnet_vec2_t Pos = {(Index % m_MapInfo.m_Width) * 32.0f + 16.0f, (Index / m_MapInfo.m_Width) * 32.0f + 16.0f};
	int Hookable = 0, Unhookable = 0;
	for(int Direction = 0; Direction < CGTrainTileRatings::NUM_DIRECTIONS; ++Direction)
	{
		const float Angle = Direction * 2.0f * pi / CGTrainTileRatings::NUM_DIRECTIONS;
		const ddnet_vec2_t End = {Pos.x + std::cos(Angle) * HOOK_REACH, Pos.y + std::sin(Angle) * HOOK_REACH};
		const int Hit = ddnet_collision_intersect_line(m_pCollision, Pos, End, nullptr, nullptr);
		Hookable += Hit == TILE_SOLID;
		Unhookable += Hit == TILE_NOHOOK;
	}
	pFeatures[CGTrainTileRatings::FEATURE_HOOKABLE] = Hookable;
	pFeatures[CGTrainTileRatings::FEATURE_UNHOOKABLE] = Unhookable;
	pFeatures[CGTrainTileRatings::FEATURE_OPEN] = m_MapInfo.m_vOpen[Index];
	pFeatures[CGTrainTileRatings::FEATURE_DEPTH] = m_MapInfo.m_vDepth[Index];
	pFeatures[CGTrainTileRatings::FEATURE_CEILING] = m_MapInfo.m_vCeiling[Index];

	for(int Sample = 0; Sample < m_Settings.m_Samples; ++Sample)
	{
		// Like a tee passing through: unfrozen, at some speed.
		ddnet_world_copy(m_pWorld.get(), m_pStart.get());
		ddnet_character_t *pChr = ddnet_world_character(m_pWorld.get(), 0);
		pChr->core.pos = Pos;
		pChr->pos = Pos;
		pChr->prev_pos = Pos;
		const uint64_t Seed = (uint64_t)Index * m_Settings.m_Samples + Sample;
		pChr->core.vel = {0.0f, 0.0f};
		if(m_Settings.m_StartSpeed)
		{
			uint64_t State = ~Seed;
			const float Angle = SplitMix(State) / 4294967296.0f * 2.0f * pi;
			pChr->core.vel = {std::cos(Angle) * m_Settings.m_StartSpeed, std::sin(Angle) * m_Settings.m_StartSpeed};
		}
		ddnet_character_changed(m_pWorld.get(), 0);

		CRandomInput Random(m_Settings, m_pCollision, Seed);
		int Survived = m_Settings.m_MaxTicks;
		for(int Tick = 0; Tick < m_Settings.m_MaxTicks; ++Tick)
		{
			ddnet_input_t Input = {};
			Input.player_flags = DDNET_PLAYERFLAG_PLAYING;
			Random.Apply(Input, pChr->core.pos);
			m_pWorld->players[0].input = Input;
			const ddnet_vec2_t Before = pChr->core.pos;
			const float SpeedBefore = std::hypot(pChr->core.vel.x, pChr->core.vel.y);
			ddnet_world_tick(m_pWorld.get());

			pChr = ddnet_world_character(m_pWorld.get(), 0);
			if(!pChr || pChr->freeze_time > 0 || pChr->core.deep_frozen || pChr->core.live_frozen)
			{
				Survived = Tick;
				break;
			}
			// Sent back: on the center of a return tile, farther away than it
			// could have moved.
			const ddnet_vec2_t After = pChr->core.pos;
			const int Tile = TileAt(After.x, After.y);
			if(Tile >= 0 && After.x == (Tile % m_MapInfo.m_Width) * 32.0f + 16.0f && After.y == (Tile / m_MapInfo.m_Width) * 32.0f + 16.0f && m_MapInfo.m_vReturnTiles[Tile] &&
				std::hypot(After.x - Before.x, After.y - Before.y) > std::max(SpeedBefore, std::hypot(pChr->core.vel.x, pChr->core.vel.y)) + 32.0f)
			{
				Survived = Tick;
				break;
			}
		}
		pSurvivedTicks[Sample] = Survived;
	}
}

CGTrainTileRatingJob::~CGTrainTileRatingJob()
{
	m_Stop = true;
	Join();
}

void CGTrainTileRatingJob::Join()
{
	for(std::thread &Thread : m_vThreads)
		Thread.join();
	m_vThreads.clear();
}

bool CGTrainTileRatingJob::Start(const void *pMapData, size_t MapSize, CCollision *pCollision, const std::vector<int> &vTileNodes, const SHA256_DIGEST &MapSha256, std::vector<int> vSpots, int NumThreads, bool LowPriority, const CGTrainRatingSettings &Settings)
{
	m_Settings = Settings;
	if(!m_PhysicsMap.Load(pMapData, MapSize))
		return false;
	m_MapInfo.Init(pCollision, vTileNodes);
	m_Ratings.m_MapSha256 = MapSha256;
	m_Ratings.m_Width = pCollision->GetWidth();
	m_Ratings.m_Height = pCollision->GetHeight();
	m_Ratings.m_NumSamples = Settings.m_Samples;
	m_Ratings.m_MaxTicks = Settings.m_MaxTicks;
	std::sort(vSpots.begin(), vSpots.end());
	m_Ratings.m_vSpots = std::move(vSpots);
	m_Ratings.m_vSurvivedTicks.assign(m_Ratings.m_vSpots.size() * Settings.m_Samples, 0);
	m_Ratings.m_vFeatures.assign(m_Ratings.m_vSpots.size() * CGTrainTileRatings::NUM_FEATURES, 0);
	// Threads that finish early count down already while others start.
	NumThreads = std::max(NumThreads, 1);
	m_NumRunning = NumThreads;
	for(int Thread = 0; Thread < NumThreads; ++Thread)
		m_vThreads.emplace_back(&CGTrainTileRatingJob::Work, this, LowPriority);
	return true;
}

void CGTrainTileRatingJob::Work(bool LowPriority)
{
#if defined(CONF_PLATFORM_LINUX)
	// Linux schedules threads like processes: this one alone gets nicer.
	if(LowPriority)
		setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
#endif
	CGTrainTileSimulator Simulator(m_PhysicsMap.Collision(), m_MapInfo, m_Settings);
	if(!Simulator.Valid())
		m_Failed = true;
	// Neighboring spots are rated together, they share map data in the caches.
	constexpr int CHUNK = 64;
	const int NumSpots = m_Ratings.m_vSpots.size();
	for(int Begin = m_NextSpot.fetch_add(CHUNK); Begin < NumSpots && !m_Stop && !m_Failed; Begin = m_NextSpot.fetch_add(CHUNK))
	{
		const int End = std::min(Begin + CHUNK, NumSpots);
		for(int Spot = Begin; Spot < End && !m_Stop; ++Spot)
			Simulator.Rate(m_Ratings.m_vSpots[Spot], &m_Ratings.m_vSurvivedTicks[(size_t)Spot * m_Settings.m_Samples], &m_Ratings.m_vFeatures[(size_t)Spot * CGTrainTileRatings::NUM_FEATURES]);
		m_NumRated += End - Begin;
	}
	--m_NumRunning;
}

CGTrainTileRatings CGTrainTileRatingJob::TakeRatings()
{
	Join();
	return std::move(m_Ratings);
}
