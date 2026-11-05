#ifndef GAME_SERVER_GAMEMODES_GTRAIN_TILES_H
#define GAME_SERVER_GAMEMODES_GTRAIN_TILES_H

#include <base/hash.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

class CCollision;
struct ddnet_collision_t;
struct ddnet_world_t;

// How hard the spots of a map are. Tees start on every spot with random
// inputs, until they freeze, die or get sent back by a checkpoint
// teleporter, for at most m_MaxTicks. The sooner, the harder, and how the
// spot is built tells more, see SpotDifficulty().
class CGTrainTileRatings
{
public:
	enum
	{
		// Ratings of another version are computed again: bump it whenever
		// the simulation, the features or their default settings change.
		VERSION = 3,
		// the most ticks a sample can hold
		MAX_TICKS = 255,
		// Of 32 directions, see CGTrainTileSimulator::Rate(): those in which
		// the hook would grab within reach and those in which it hits
		// unhookable tiles first. Then the distance in tiles to the nearest
		// tile paths can't go through (at most 10), to the freeze, death or
		// wall below and to the ceiling (each at most 30).
		FEATURE_HOOKABLE = 0,
		FEATURE_UNHOOKABLE,
		FEATURE_OPEN,
		FEATURE_DEPTH,
		FEATURE_CEILING,
		NUM_FEATURES,
		NUM_DIRECTIONS = 32,
		MAX_OPEN = 10,
		MAX_DEPTH = 30,
	};

	SHA256_DIGEST m_MapSha256 = {};
	int m_Width = 0;
	int m_Height = 0;
	int m_NumSamples = 0;
	int m_MaxTicks = 0;
	// tile indices of the rated spots, and for each of them in the same
	// order m_NumSamples survived ticks and NUM_FEATURES features
	std::vector<int> m_vSpots;
	std::vector<uint8_t> m_vSurvivedTicks;
	std::vector<uint8_t> m_vFeatures;

	// Of the spot at this position in m_vSpots, in steps of the KoG ratings
	// before GTrainElo::TileStep() curves them: the survival ln(m_MaxTicks /
	// the mean ticks the tees lasted), from 0 when none froze to about 5.3
	// when all froze right away, and the features, weighted to fit the
	// ratings of the KoG maps. Open spots are harder than their survival
	// says, the more so the safer they seem.
	float SpotDifficulty(int Spot) const;
	// Of every tile, -1 where there is no spot.
	std::vector<float> TileDifficulties() const;

	bool Save(const char *pFilename) const;
	// Fails for missing files, other versions and other maps.
	bool Load(const char *pFilename, const SHA256_DIGEST &MapSha256);
};

// The map as ddnet_physics loads it, for the simulation.
class CGTrainPhysicsMap
{
	ddnet_collision_t *m_pCollision = nullptr;

public:
	CGTrainPhysicsMap() = default;
	CGTrainPhysicsMap(const CGTrainPhysicsMap &) = delete;
	CGTrainPhysicsMap &operator=(const CGTrainPhysicsMap &) = delete;
	~CGTrainPhysicsMap();
	// From the data of a map file, which is copied.
	bool Load(const void *pData, size_t Size);
	const ddnet_collision_t *Collision() const { return m_pCollision; }
};

// How the simulated tees play. Each input is held for a while, like a
// player's, instead of being drawn again every tick, which would let go of
// the hook right after pressing it. With these defaults the survival ranks
// the KoG maps closest to their ratings: aimed hooks and a moving start
// tell hard maps apart. Fixed inputs like hooking straight up, shorter
// holds, playing along the race, longer simulations, counting hopeless
// falls as freezing early and counting how often the tees get further along
// the race did not. Bump CGTrainTileRatings::VERSION when changing them.
struct CGTrainRatingSettings
{
	int m_Samples = 20;
	int m_MaxTicks = 100;
	int m_MinHoldTicks = 5;
	int m_MaxHoldTicks = 30;
	// of the inputs: aiming at something hookable in reach, if there is
	// anything, instead of anywhere
	int m_AimPercent = 100;
	int m_HookPercent = 70;
	int m_JumpPercent = 25;
	// of the tee at the start, in a random direction, in units per tick
	int m_StartSpeed = 16;

	// "name=value,..." with the member names without m_ and in lower case,
	// like "aimpercent=75". False for unknown names.
	bool Parse(const char *pSettings);
};

// What the simulation needs to know about the map besides its physics.
struct CGTrainTileMapInfo
{
	int m_Width = 0;
	int m_Height = 0;
	// Where checkpoint teleporters send tees back to: their exits, and the
	// spawns when no checkpoint was passed.
	std::vector<bool> m_vReturnTiles;
	// the features of every tile that the simulation doesn't need for them
	std::vector<uint8_t> m_vOpen;
	std::vector<uint8_t> m_vDepth;
	std::vector<uint8_t> m_vCeiling;

	// vTileNodes are the path nodes of CGTrainMap, -1 where paths can't go.
	void Init(CCollision *pCollision, const std::vector<int> &vTileNodes);
};

// Simulates the tees of the ratings, with the same physics and settings as
// GTrain. One per thread: they only share the collision and the map info.
class CGTrainTileSimulator
{
	const ddnet_collision_t *m_pCollision;
	const CGTrainTileMapInfo &m_MapInfo;
	std::unique_ptr<ddnet_world_t> m_pStart;
	std::unique_ptr<ddnet_world_t> m_pWorld;
	CGTrainRatingSettings m_Settings;
	bool m_Valid = false;

	int TileAt(float x, float y) const;

public:
	CGTrainTileSimulator(const ddnet_collision_t *pCollision, const CGTrainTileMapInfo &MapInfo, const CGTrainRatingSettings &Settings);
	~CGTrainTileSimulator();
	// False if the map has no spawn, where the simulated tee joins.
	bool Valid() const { return m_Valid; }
	// The samples and features of a tee starting on the tile, the same on
	// every run.
	void Rate(int Index, uint8_t *pSurvivedTicks, uint8_t *pFeatures);
};

// Rates the spots of a map on threads of its own, in the background.
class CGTrainTileRatingJob
{
	CGTrainPhysicsMap m_PhysicsMap;
	CGTrainTileMapInfo m_MapInfo;
	CGTrainTileRatings m_Ratings;
	std::vector<std::thread> m_vThreads;
	std::atomic<int> m_NextSpot = 0;
	std::atomic<int> m_NumRated = 0;
	std::atomic<int> m_NumRunning = 0;
	std::atomic<bool> m_Stop = false;
	std::atomic<bool> m_Failed = false;
	CGTrainRatingSettings m_Settings;
	void Work(bool LowPriority);
	void Join();

public:
	~CGTrainTileRatingJob();
	// Rates the spots, tile indices, of the map in pMapData. vTileNodes are
	// the path nodes of CGTrainMap. Fails if ddnet_physics can't load the
	// map. Low priority threads leave the cores to everything else, like
	// the server's ticks.
	bool Start(const void *pMapData, size_t MapSize, CCollision *pCollision, const std::vector<int> &vTileNodes, const SHA256_DIGEST &MapSha256, std::vector<int> vSpots, int NumThreads, bool LowPriority, const CGTrainRatingSettings &Settings = CGTrainRatingSettings());
	int NumSpots() const { return m_Ratings.m_vSpots.size(); }
	int NumRated() const { return m_NumRated; }
	bool Done() const { return m_NumRunning == 0; }
	// Done without ratings: the map has no spawn for the simulated tee.
	bool Failed() const { return m_Failed; }
	// Once done.
	CGTrainTileRatings TakeRatings();
};

#endif // GAME_SERVER_GAMEMODES_GTRAIN_TILES_H
