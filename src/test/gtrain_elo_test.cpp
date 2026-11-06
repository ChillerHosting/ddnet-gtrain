#include <game/server/gamemodes/gtrain_elo.h>

#include <gtest/gtest.h>

using namespace GTrainElo;

TEST(GTrainElo, PointsPerTileGrowByHalfPerStep)
{
	EXPECT_DOUBLE_EQ(PointsPerTile(0), 100);
	EXPECT_DOUBLE_EQ(PointsPerTile(1), 150);
	EXPECT_DOUBLE_EQ(PointsPerTile(3), 337.5);
	for(int Step = 1; Step <= DERANGED_STEP; ++Step)
		EXPECT_DOUBLE_EQ(PointsPerTile(Step) / PointsPerTile(Step - 1), 1.5);
	// Extreme with three stars is worth about 292 times Easy with one.
	EXPECT_NEAR(PointsPerTile(EXTREME_STEP) / PointsPerTile(0), 291.93, 0.01);
	// Deranged counts as Extreme with five stars, 2.25 times three stars.
	EXPECT_EQ(DERANGED_STEP, EXTREME_STEP + 2);
	EXPECT_DOUBLE_EQ(PointsPerTile(DERANGED_STEP), 2.25 * PointsPerTile(EXTREME_STEP));
	EXPECT_NEAR(PointsPerTile(DERANGED_STEP) / PointsPerTile(0), 656.84, 0.01);
}

TEST(GTrainElo, SpeedFactorFollowsTheTypicalSpeedOfTheDifficulty)
{
	// Typical speeds go linearly from Easy to Extreme.
	const double Easy = EASY_TILES_PER_SECOND, Extreme = EXTREME_TILES_PER_SECOND;
	EXPECT_DOUBLE_EQ(SpeedFactor(0, 100, 100 / Easy), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP / 2, 100, 100 / ((Easy + Extreme) / 2)), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP, 100, 100 / Extreme), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP, 150, 100 / Extreme), 1.5);
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP, 75, 100 / Extreme), std::max(0.75, MIN_SPEED_FACTOR));
	// Deranged maps have no speed bonus.
	EXPECT_DOUBLE_EQ(SpeedFactor(DERANGED_STEP, 100, 100 / Extreme), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(DERANGED_STEP, 400, 1), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(DERANGED_STEP, 100, 1000), 1);
	// Fast captures earn up to a maximum, slow ones a minimum. Instant
	// captures count as taking one tick, which always reaches the maximum.
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP, 350, 100 / Extreme), 3.5);
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP, 1000, 100 / Extreme), MAX_SPEED_FACTOR);
	EXPECT_DOUBLE_EQ(SpeedFactor(EXTREME_STEP, 1, 100), MIN_SPEED_FACTOR);
	EXPECT_DOUBLE_EQ(SpeedFactor(0, 1, 0), MAX_SPEED_FACTOR);
}

TEST(GTrainElo, CapturePoints)
{
	EXPECT_EQ(CapturePoints(0, 100, 100 / EASY_TILES_PER_SECOND), 10000);
	EXPECT_EQ(CapturePoints(7, 100, 100 / ((EASY_TILES_PER_SECOND + EXTREME_TILES_PER_SECOND) / 2)), 170859); // 100 * 1.5^7 * 100, rounded
	EXPECT_EQ(CapturePoints(EXTREME_STEP, 100, 100 / (4 * EXTREME_TILES_PER_SECOND)), std::llround(100 * PointsPerTile(EXTREME_STEP) * 4));
	EXPECT_EQ(CapturePoints(EXTREME_STEP, 100, 1000), std::llround(100 * PointsPerTile(EXTREME_STEP) * MIN_SPEED_FACTOR));
	EXPECT_EQ(CapturePoints(DERANGED_STEP, 100, 1000), std::llround(100 * 2.25 * PointsPerTile(EXTREME_STEP)));
	EXPECT_EQ(CapturePoints(DERANGED_STEP, 100, 1), CapturePoints(DERANGED_STEP, 100, 1000));
}

TEST(GTrainElo, CapturesOfRatedTilesEarnTheirSum)
{
	// Each tile earns by its own step, times the distance multiplier: a
	// capture without tile ratings counts every goal tile at the map's step.
	EXPECT_DOUBLE_EQ(RatedTilePoints(0), 50);
	EXPECT_DOUBLE_EQ(RatedTilePoints(7.5), PointsPerTile(7.5) / 2);
	EXPECT_DOUBLE_EQ(DistanceMultiplier(100), 1);
	EXPECT_DOUBLE_EQ(DistanceMultiplier(400), 2);
	EXPECT_EQ(CapturePointsOfTiles(PointsPerTile(0) + PointsPerTile(14), 7.0, false, 100, 100 / ((EASY_TILES_PER_SECOND + EXTREME_TILES_PER_SECOND) / 2)),
		std::llround(PointsPerTile(0) + PointsPerTile(14)));
	EXPECT_EQ(CapturePointsOfTiles(1000, 0.0, false, 400, 400 / EASY_TILES_PER_SECOND), 2000);
	for(const int Distance : {50, 100, 237, 500})
		EXPECT_EQ(CapturePointsOfTiles(Distance * PointsPerTile(9), 9.0, false, Distance, 7.5), CapturePoints(9, Distance, 7.5)) << Distance;
	// Only the step's typical speed matters for the speed factor.
	EXPECT_EQ(CapturePointsOfTiles(1000, EXTREME_STEP, false, 100, 100 / (2 * EXTREME_TILES_PER_SECOND)), 2000);
	EXPECT_EQ(CapturePointsOfTiles(1000, DERANGED_STEP, true, 100, 1), 1000);
}

TEST(GTrainElo, LongerGoalsEarnMoreThanTheirLength)
{
	// At the same speed, twice the distance earns 2^DISTANCE_EXPONENT times the points.
	ASSERT_GT(DISTANCE_EXPONENT, 1);
	const double Doubled = std::pow(2, DISTANCE_EXPONENT);
	const auto Typical = [](int Distance) { return CapturePoints(0, Distance, Distance / EASY_TILES_PER_SECOND); };
	EXPECT_EQ(Typical(100), 10000);
	EXPECT_EQ(Typical(200), std::llround(10000 * Doubled));
	EXPECT_NEAR((double)Typical(200) / Typical(100), Doubled, 0.001);
	EXPECT_NEAR((double)Typical(100) / Typical(50), Doubled, 0.001);
	EXPECT_NEAR((double)Typical(500) / Typical(100) / 5, std::pow(5, DISTANCE_EXPONENT - 1), 0.001);
	// Short goals earn less per tile.
	EXPECT_LT((double)Typical(50) / 50, 100);
}

TEST(GTrainElo, LogarithmicScaleUpToTheMaximum)
{
	EXPECT_EQ(FromPoints(0), 0);
	// Single captures of hard maps barely count for new players.
	EXPECT_LT(FromPoints(CapturePoints(EXTREME_STEP, 100, 100 / EXTREME_TILES_PER_SECOND)), 150);
	// Every doubling adds the same ELO once past the linear start.
	const double Doubling = FromPoints(200'000'000'000) - FromPoints(100'000'000'000);
	EXPECT_NEAR(FromPoints(400'000'000'000) - FromPoints(200'000'000'000), Doubling, 1);
	EXPECT_NEAR(Doubling, 758, 1);
	// The maximum needs the inhuman amount of points.
	EXPECT_LT(FromPoints((int64_t)(MAX_POINTS * 0.99)), MAX_ELO - 10);
	EXPECT_GT(FromPoints((int64_t)(MAX_POINTS * 0.99)), MAX_ELO - 12);
	EXPECT_EQ(FromPoints((int64_t)(MAX_POINTS * 1.01)), MAX_ELO);
	EXPECT_EQ(FromPoints(INT64_MAX / 2), MAX_ELO);
	int64_t Points = 1;
	for(int i = 0; i < 60; ++i, Points *= 2)
		EXPECT_LE(FromPoints(Points), FromPoints(Points * 2));
}
