#ifndef GAME_SERVER_GAMEMODES_GTRAIN_ELO_H
#define GAME_SERVER_GAMEMODES_GTRAIN_ELO_H

#include <engine/shared/protocol.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// GTrain captures earn points: the goal's distance in tiles, worth more on
// every difficulty step of the map and when captured fast. ELO shows a
// player's total points on a logarithmic scale up to MAX_ELO: it grows by the
// same amount whenever they double, which harder maps make them do faster.
namespace GTrainElo
{
	inline constexpr int MAX_ELO = 9999;
	inline constexpr int EXTREME_STEP = 14; // Extreme with three stars
	// Deranged maps have no stars and count as Extreme with five would, two
	// steps above Extreme with three.
	inline constexpr int DERANGED_STEP = EXTREME_STEP + 2;
	inline constexpr double POINTS_PER_TILE = 100; // Easy with one star
	inline constexpr double STEP_MULTIPLIER = 1.5;
	// Typical capture speeds in path tiles per second, a bit below those of
	// the first captures on the KoG maps. Harder maps are slower, which must
	// not cost points. Deranged continues the line past Extreme.
	inline constexpr double EASY_TILES_PER_SECOND = 12;
	inline constexpr double EXTREME_TILES_PER_SECOND = 6;
	inline constexpr double MIN_SPEED_FACTOR = 0.5;
	inline constexpr double MAX_SPEED_FACTOR = 4;
	// Instant captures count as taking one tick.
	inline constexpr double MIN_SECONDS = 1.0 / (double)SERVER_TICK_SPEED;
	// Up to about this many points, ELO grows linearly with them, so single
	// captures of hard maps don't make new players jump up.
	inline constexpr double LINEAR_POINTS = 50'000'000;

	constexpr double PointsPerTile(int Step)
	{
		double Points = POINTS_PER_TILE;
		for(int i = 0; i < Step; ++i)
			Points *= STEP_MULTIPLIER;
		return Points;
	}

	// The maximum takes 1000 hours of Extreme 3 star captures of 100 tile
	// goals at 8000 tiles per hour, all twice as fast as typical. Good players
	// managed about half that pace at a typical speed, so this is completely
	// inhuman.
	inline constexpr double MAX_POINTS = 1000.0 * 8000 * PointsPerTile(EXTREME_STEP) * 2;

	// The capture's speed relative to the typical one of its difficulty.
	inline double SpeedFactor(int Step, int Distance, double Seconds)
	{
		const double Typical = EASY_TILES_PER_SECOND + (EXTREME_TILES_PER_SECOND - EASY_TILES_PER_SECOND) * Step / EXTREME_STEP;
		return std::clamp(Distance / std::max(Seconds, MIN_SECONDS) / Typical, MIN_SPEED_FACTOR, MAX_SPEED_FACTOR);
	}

	// Long goals are harder than their length, every tile is another chance
	// to fail. Twice the distance earns 2^1.5, about 2.8 times the points.
	// Goals of the default 100 tiles earn the points per tile.
	inline constexpr double DISTANCE_EXPONENT = 1.5;
	inline constexpr double REFERENCE_DISTANCE = 100;

	inline int64_t CapturePoints(int Step, int Distance, double Seconds)
	{
		const double Tiles = REFERENCE_DISTANCE * std::pow(Distance / REFERENCE_DISTANCE, DISTANCE_EXPONENT);
		return std::llround(Tiles * PointsPerTile(Step) * SpeedFactor(Step, Distance, Seconds));
	}

	inline double FromPoints(int64_t Points)
	{
		const double Elo = MAX_ELO * std::log2(1 + Points / LINEAR_POINTS) / std::log2(1 + MAX_POINTS / LINEAR_POINTS);
		return std::min(Elo, (double)MAX_ELO);
	}
}

// A player's GTrain totals, or changes to them.
struct CGTrainStats
{
	int64_t m_Points = 0;
	int m_Captures = 0;
	int m_Attempts = 0;
	int m_Respawns = 0;
	int m_Playtime = 0; // seconds

	void Add(const CGTrainStats &Other)
	{
		m_Points += Other.m_Points;
		m_Captures += Other.m_Captures;
		m_Attempts += Other.m_Attempts;
		m_Respawns += Other.m_Respawns;
		m_Playtime += Other.m_Playtime;
	}
	bool IsEmpty() const
	{
		return !m_Points && !m_Captures && !m_Attempts && !m_Respawns && !m_Playtime;
	}
};

// The stats with a /top5 ladder.
enum class EGTrainStat
{
	POINTS,
	CAPTURES,
	ATTEMPTS,
	RESPAWNS,
	PLAYTIME,
};

#endif // GAME_SERVER_GAMEMODES_GTRAIN_ELO_H
