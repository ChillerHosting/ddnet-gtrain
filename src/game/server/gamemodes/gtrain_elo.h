#ifndef GAME_SERVER_GAMEMODES_GTRAIN_ELO_H
#define GAME_SERVER_GAMEMODES_GTRAIN_ELO_H

#include <engine/shared/protocol.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// GTrain captures earn points: the tiles they went through, each worth more on
// every difficulty step, on long goals and when captured fast. ELO shows a
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
	// The line goes on above Extreme with three stars, but no slower.
	inline constexpr double MIN_TILES_PER_SECOND = 2;
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

	// Steps between the whole ones, from the tiles of a capture.
	inline double PointsPerTile(double Step)
	{
		return POINTS_PER_TILE * std::pow(STEP_MULTIPLIER, Step);
	}

	// Captures go through more tiles than their goal is long, and adding up
	// the tiles makes the hard ones count for more than their mean did: on
	// the lain runs, rated tiles earned 2.2 times as much that way. They earn
	// half the points per tile instead.
	inline constexpr double RATED_TILE_DIVISOR = 2.0;
	inline double RatedTilePoints(double Step)
	{
		return PointsPerTile(Step) / RATED_TILE_DIVISOR;
	}

	// The capture's speed relative to the typical one of its difficulty.
	// Deranged maps have no speed bonus, every capture counts as typical.
	inline double SpeedFactor(double Step, bool Deranged, int Distance, double Seconds)
	{
		if(Deranged)
			return 1;
		const double Typical = std::max(EASY_TILES_PER_SECOND + (EXTREME_TILES_PER_SECOND - EASY_TILES_PER_SECOND) * Step / EXTREME_STEP, MIN_TILES_PER_SECOND);
		return std::clamp(Distance / std::max(Seconds, MIN_SECONDS) / Typical, MIN_SPEED_FACTOR, MAX_SPEED_FACTOR);
	}
	inline double SpeedFactor(int Step, int Distance, double Seconds)
	{
		return SpeedFactor((double)Step, Step == DERANGED_STEP, Distance, Seconds);
	}

	// With rated tiles (see CGTrainTileRatings), every tile a capture went
	// through earns by its own step instead of the map's category and stars.
	// The steps come from the tiles' difficulties along this curve, which
	// is only checked on means: those of whole races. These are the mean
	// difficulties along the
	// races of the KoG maps of steps 0 to 14, fitted as a curve over the
	// steps, so that the maps of each category and stars come out at that
	// step on average: 0.59 steps off over 664 maps, 95% of them within 1.5.
	// Between them the step is interpolated, beyond them extrapolated, and
	// it is at least 0. Above Extreme with three stars it goes on without an
	// end, as Deranged with more and more stars. Fit them again whenever the
	// ratings change, see CGTrainRatingSettings.
	inline constexpr double TILE_STEP_DIFFICULTIES[EXTREME_STEP + 1] = {
		0.4140, 1.3744, 2.3303, 3.2817, 4.2287, 5.1712, 6.1092, 7.0427, 7.9718, 8.8964, 9.8165, 10.7322, 11.6434, 12.5501, 13.4524};

	inline double TileStep(double Difficulty)
	{
		constexpr int LAST = EXTREME_STEP;
		const auto Between = [&](int Step) { return Step + (Difficulty - TILE_STEP_DIFFICULTIES[Step]) / (TILE_STEP_DIFFICULTIES[Step + 1] - TILE_STEP_DIFFICULTIES[Step]); };
		double Step;
		if(Difficulty <= TILE_STEP_DIFFICULTIES[0])
			Step = Between(0);
		else if(Difficulty >= TILE_STEP_DIFFICULTIES[LAST])
			Step = Between(LAST - 1);
		else
		{
			int Lower = 0;
			while(TILE_STEP_DIFFICULTIES[Lower + 1] < Difficulty)
				++Lower;
			Step = Between(Lower);
		}
		return std::max(Step, 0.0);
	}

	// Long goals are harder than their length, every tile is another chance
	// to fail. Twice the distance earns 2^1.5, about 2.8 times the points:
	// twice the tiles, each worth sqrt(2) times as much. Goals of the default
	// 100 tiles earn the points per tile.
	inline constexpr double DISTANCE_EXPONENT = 1.5;
	inline constexpr double REFERENCE_DISTANCE = 100;

	inline double DistanceMultiplier(int Distance)
	{
		return std::pow(Distance / REFERENCE_DISTANCE, DISTANCE_EXPONENT - 1);
	}

	// A capture earns the points of the tiles it went through, more on long
	// goals and when fast. Step is the difficulty of the capture as a whole,
	// which sets its typical speed.
	inline int64_t CapturePointsOfTiles(double TilePoints, double Step, bool Deranged, int Distance, double Seconds)
	{
		return std::llround(TilePoints * DistanceMultiplier(Distance) * SpeedFactor(Step, Deranged, Distance, Seconds));
	}
	// Every tile of the goal's distance at the same step, for maps without
	// tile ratings.
	inline int64_t CapturePoints(double Step, bool Deranged, int Distance, double Seconds)
	{
		return CapturePointsOfTiles(Distance * PointsPerTile(Step), Step, Deranged, Distance, Seconds);
	}
	inline int64_t CapturePoints(int Step, int Distance, double Seconds)
	{
		return CapturePoints((double)Step, Step == DERANGED_STEP, Distance, Seconds);
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
