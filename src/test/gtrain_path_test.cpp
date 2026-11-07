#include <game/server/gamemodes/gtrain_path.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <queue>
#include <string>

namespace
{
	CGTrainPathGraph Grid(int Width, int Height, const std::vector<int> &vBlocked = {}, const std::vector<uint8_t> &vStoppers = {})
	{
		CGTrainPathGraph Graph;
		std::vector<bool> vPassable(Width * Height, true);
		for(int Index : vBlocked)
			vPassable[Index] = false;
		for(int y = 0; y < Height; ++y)
			for(int x = 0; x < Width; ++x)
				Graph.m_vNodes.push_back({vec2(x * 32 + 16, y * 32 + 16), vPassable[y * Width + x]});
		std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
		std::vector<int> vTileNodes(Width * Height, -1);
		for(int i = 0; i < Width * Height; ++i)
			if(vPassable[i])
				vTileNodes[i] = i;
		for(int i = 0; i < Width * Height; ++i)
			CGTrainPathGraph::AddTileEdges(i, Width, Height, vTileNodes, vStoppers, vEdges);
		Graph.InitEdges(vEdges);
		return Graph;
	}

	// Rows of tiles: '.' empty, 'f' freeze, 'u' unfreeze, '#' anything that
	// stops a fall, 'v' empty with a stopper holding tees on it from above.
	struct CFallGrid
	{
		CGTrainPathGraph m_Graph;
		int m_Width;
		std::vector<int> m_vTileNodes;
		std::vector<int> m_vFallNodes;

		int Node(int x, int y) const { return m_vTileNodes[y * m_Width + x]; }
		int FallNode(int x, int y) const { return m_vFallNodes[y * m_Width + x]; }
	};

	CFallGrid FallGrid(const std::vector<std::string> &vRows)
	{
		CFallGrid Grid;
		const int Width = Grid.m_Width = vRows[0].size();
		const int Height = vRows.size();
		std::vector<uint8_t> vFall(Width * Height, CGTrainPathGraph::FALL_STOP);
		std::vector<uint8_t> vStoppers(Width * Height, 0);
		for(int y = 0; y < Height; ++y)
			for(int x = 0; x < Width; ++x)
			{
				const char Tile = vRows[y][x];
				vFall[y * Width + x] = Tile == '.' || Tile == 'v' ? CGTrainPathGraph::FALL_EMPTY : Tile == 'f' ? CGTrainPathGraph::FALL_FREEZE :
													   Tile == 'u'         ? CGTrainPathGraph::FALL_UNFREEZE :
																 CGTrainPathGraph::FALL_STOP;
				if(Tile == 'v')
					vStoppers[y * Width + x] = CGTrainPathGraph::DIRECTION_DOWN << 4;
			}
		std::vector<bool> vNodes(Width * Height);
		for(int Index = 0; Index < Width * Height; ++Index)
		{
			const char Tile = vRows[Index / Width][Index % Width];
			vNodes[Index] = Tile == '.' || Tile == 'v' || Tile == 'u';
		}
		const std::vector<bool> vFrozenFalls = CGTrainPathGraph::FindFrozenFalls(Width, Height, vFall, vNodes);
		Grid.m_vTileNodes.assign(Width * Height, -1);
		Grid.m_vFallNodes.assign(Width * Height, -1);
		for(int Index = 0; Index < Width * Height; ++Index)
		{
			const vec2 Pos((Index % Width) * 32 + 16, (Index / Width) * 32 + 16);
			if(vNodes[Index])
			{
				Grid.m_vTileNodes[Index] = Grid.m_Graph.m_vNodes.size();
				Grid.m_Graph.m_vNodes.push_back({Pos, true});
			}
			if(vFrozenFalls[Index])
			{
				Grid.m_vFallNodes[Index] = Grid.m_Graph.m_vNodes.size();
				Grid.m_Graph.m_vNodes.push_back({Pos, false});
			}
		}
		std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
		for(int i = 0; i < Width * Height; ++i)
			CGTrainPathGraph::AddTileEdges(i, Width, Height, Grid.m_vTileNodes, vStoppers, vEdges);
		CGTrainPathGraph::AddFallEdges(Width, Grid.m_vTileNodes, Grid.m_vFallNodes, vStoppers, vEdges);
		Grid.m_Graph.InitEdges(vEdges);
		return Grid;
	}

	int FallDistance(CFallGrid &Grid, int From, int To)
	{
		CGTrainPathfinder Finder(Grid.m_Graph);
		std::vector<int> vDistances;
		Finder.FindDistances({To}, vDistances);
		return vDistances[From];
	}

	int NumFallNodes(const CFallGrid &Grid)
	{
		return std::count_if(Grid.m_vFallNodes.begin(), Grid.m_vFallNodes.end(), [](int Node) { return Node >= 0; });
	}
}

TEST(GTrainPath, ExactPathDistanceAndObstacleRoute)
{
	const auto Graph = Grid(7, 5, {3, 10, 17, 24});
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 10, 123);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Distance, 10);
	ASSERT_EQ(Goal.m_vInitialRoute.size(), 11);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, Goal.m_vInitialRoute);
	EXPECT_EQ(vRoute.front(), 0);
	EXPECT_EQ(vRoute.back(), Goal.m_Goal);
	for(size_t i = 1; i < vRoute.size(); ++i)
	{
		const int From = vRoute[i - 1], To = vRoute[i];
		bool Adjacent = false;
		for(int j = Graph.m_vOutOffsets[From]; j < Graph.m_vOutOffsets[From + 1]; ++j)
			Adjacent |= Graph.m_vOutEdges[j].m_Node == To;
		EXPECT_TRUE(Adjacent);
	}
}

TEST(GTrainPath, ShortMapFallsBackToFarthestEligibleGoal)
{
	auto Graph = Grid(8, 1);
	Graph.m_vNodes[7].m_Goal = false;
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 100, 1);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Goal, 6);
	EXPECT_EQ(Goal.m_Distance, 6);
	EXPECT_EQ(Goal.m_vInitialRoute.size(), 7);
}

TEST(GTrainPath, DirectedTeleportAndDisconnectedRegion)
{
	CGTrainPathGraph Graph;
	for(int i = 0; i < 5; ++i)
		Graph.m_vNodes.push_back({vec2(i * 320, 16), i == 3});
	Graph.InitEdges({{0, {1, 2}}, {1, {2, 0}}, {2, {3, 8}}});
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 3, 1);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Goal, 3);
	EXPECT_EQ(Goal.m_vInitialRoute, (std::vector<int>{0, 1, 2, 3}));
	EXPECT_TRUE(Graph.IsTeleportStep(1, 2));
	EXPECT_FALSE(Graph.IsTeleportStep(0, 1));
	std::vector<int> vRoute;
	EXPECT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, Goal.m_vInitialRoute);
	EXPECT_FALSE(Finder.FindPath(Goal, 4, vRoute));
	EXPECT_TRUE(vRoute.empty());
	CGTrainGoal Back;
	Finder.BuildField(Back, 0, 100);
	EXPECT_FALSE(Finder.FindPath(Back, 3, vRoute));
	EXPECT_TRUE(vRoute.empty());
}

TEST(GTrainPath, TeleportShortcutAwayFromTheGoal)
{
	auto Graph = Grid(20, 1);
	std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
	for(int i = 0; i < 19; ++i)
		vEdges.push_back({i, {i + 1, 2}});
	// The optimal path first moves away from the goal, then teleports.
	vEdges.push_back({10, {0, 0}});
	vEdges.push_back({0, {19, 0}});
	Graph.InitEdges(vEdges);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 19, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 10, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{10, 0, 19}));
}

TEST(GTrainPath, SeparateGoalsAndRebuilding)
{
	const auto Graph = Grid(20, 1);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal First, Second;
	Finder.Build(First, 0, 19, 1);
	const auto vOriginal = First.m_vInitialRoute;
	Finder.Build(Second, 19, 19, 2);
	EXPECT_EQ(First.m_vInitialRoute, vOriginal);
	EXPECT_EQ(First.m_Goal, 19);
	EXPECT_EQ(Second.m_Goal, 0);
	Finder.Build(First, 10, 3, 3);
	ASSERT_TRUE(First.m_Ready);
	EXPECT_EQ(First.m_Distance, 3);
	EXPECT_EQ(First.m_vInitialRoute.size(), 4);
}

TEST(GTrainPath, NoGoalAndInvalidStart)
{
	const auto Graph = Grid(1, 1);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 100, 1);
	EXPECT_FALSE(Goal.m_Ready);
	Finder.Build(Goal, -1, 100, 1);
	EXPECT_FALSE(Goal.m_Ready);
	std::vector<int> vRoute;
	Finder.BuildField(Goal, 0, 100);
	EXPECT_FALSE(Finder.FindPath(Goal, -1, vRoute));
	EXPECT_FALSE(Finder.FindPath(Goal, 1, vRoute));
	EXPECT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{0}));
	Finder.BuildField(Goal, 1, 100);
	EXPECT_FALSE(Finder.FindPath(Goal, 0, vRoute));
}

TEST(GTrainPath, FieldRoutesMatchBreadthFirstSearchAroundWalls)
{
	const auto Graph = Grid(11, 9, {3, 14, 25, 36, 47, 58, 69, 71, 82, 93});
	CGTrainPathfinder Finder(Graph);
	std::vector<CGTrainGoal> vGoals(Graph.m_vNodes.size());
	for(int Goal = 0; Goal < (int)Graph.m_vNodes.size(); ++Goal)
		Finder.BuildField(vGoals[Goal], Goal, 1000);
	for(int Start : {0, 10, 44, 98})
	{
		std::vector<int> vDistances(Graph.m_vNodes.size(), -1);
		std::queue<int> Queue;
		vDistances[Start] = 0;
		Queue.push(Start);
		while(!Queue.empty())
		{
			const int Node = Queue.front();
			Queue.pop();
			for(int i = Graph.m_vOutOffsets[Node]; i < Graph.m_vOutOffsets[Node + 1]; ++i)
			{
				const int Next = Graph.m_vOutEdges[i].m_Node;
				if(vDistances[Next] >= 0)
					continue;
				vDistances[Next] = vDistances[Node] + 1;
				Queue.push(Next);
			}
		}
		for(int Goal = 0; Goal < (int)Graph.m_vNodes.size(); ++Goal)
		{
			std::vector<int> vRoute;
			EXPECT_EQ(Finder.FindPath(vGoals[Goal], Start, vRoute), vDistances[Goal] >= 0);
			if(vDistances[Goal] >= 0)
			{
				EXPECT_EQ((int)vRoute.size() - 1, vDistances[Goal]);
				EXPECT_EQ(vRoute.back(), Goal);
				for(size_t i = 1; i < vRoute.size(); ++i)
				{
					bool Adjacent = false;
					for(int j = Graph.m_vOutOffsets[vRoute[i - 1]]; j < Graph.m_vOutOffsets[vRoute[i - 1] + 1]; ++j)
						Adjacent |= Graph.m_vOutEdges[j].m_Node == vRoute[i];
					EXPECT_TRUE(Adjacent);
				}
			}
		}
	}
}

TEST(GTrainPath, DiagonalStepsAndGoalDistance)
{
	const auto Graph = Grid(4, 4);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Corner;
	Finder.BuildField(Corner, 15, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Corner, 0, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{0, 5, 10, 15}));
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 3, 9);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Distance, 3);
	EXPECT_EQ(Goal.m_vInitialRoute.size(), 4);
	const vec2 Offset = Graph.m_vNodes[Goal.m_Goal].m_Pos - Graph.m_vNodes[0].m_Pos;
	EXPECT_FLOAT_EQ(std::max(std::abs(Offset.x), std::abs(Offset.y)), 3 * 32.0f);
}

TEST(GTrainPath, DiagonalsRequireBothSideTilesToBeSafe)
{
	for(const auto &vBlocked : {std::vector<int>{1}, std::vector<int>{2}, std::vector<int>{1, 2}})
	{
		const auto Graph = Grid(2, 2, vBlocked);
		for(int i = Graph.m_vOutOffsets[0]; i < Graph.m_vOutOffsets[1]; ++i)
			EXPECT_NE(Graph.m_vOutEdges[i].m_Node, 3);
		CGTrainPathfinder Finder(Graph);
		CGTrainGoal Goal;
		Finder.BuildField(Goal, 3, 100);
		std::vector<int> vRoute;
		if(vBlocked.size() == 1)
		{
			ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
			EXPECT_EQ(vRoute.size(), 3); // must take two cardinal steps
		}
		else
			EXPECT_FALSE(Finder.FindPath(Goal, 0, vRoute));
	}
}

TEST(GTrainPath, FieldDepthCoversDetoursBehindTheStart)
{
	const auto Graph = Grid(200, 1);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 0, 5);
	std::vector<int> vRoute;
	EXPECT_TRUE(Finder.FindPath(Goal, 5, vRoute));
	EXPECT_EQ(vRoute.size(), 6);
	EXPECT_FALSE(Finder.FindPath(Goal, 6, vRoute));
	EXPECT_TRUE(vRoute.empty());
	EXPECT_EQ(Goal.m_vField.size(), 6); // only the reached node range is stored

	// Built goals allow straying as far behind the start as the goal is ahead.
	Finder.Build(Goal, 100, 10, 1);
	ASSERT_TRUE(Goal.m_Ready);
	const int Behind = Goal.m_Goal < 100 ? 1 : -1;
	EXPECT_TRUE(Finder.FindPath(Goal, 100 + Behind * (10 + CGTrainPathfinder::FIELD_MARGIN), vRoute));
	EXPECT_EQ((int)vRoute.size() - 1, 2 * 10 + CGTrainPathfinder::FIELD_MARGIN);
	EXPECT_FALSE(Finder.FindPath(Goal, 100 + Behind * (11 + CGTrainPathfinder::FIELD_MARGIN), vRoute));
}

TEST(GTrainPath, EqualRoutesHeadStraightForTheGoal)
{
	const auto Graph = Grid(8, 4);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 2 * 8 + 7, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	// Two diagonal steps reach the goal row, then the route continues straight.
	EXPECT_EQ(vRoute, (std::vector<int>{0, 9, 18, 19, 20, 21, 22, 23}));
}

TEST(GTrainPath, StoppersBlockRoutesAgainstTheirDirection)
{
	constexpr int Left = CGTrainPathGraph::DIRECTION_LEFT, Right = CGTrainPathGraph::DIRECTION_RIGHT;
	constexpr int Down = CGTrainPathGraph::DIRECTION_DOWN;
	const auto Reachable = [](const CGTrainPathGraph &Graph, int Start, int Goal) {
		CGTrainPathfinder Finder(Graph);
		CGTrainGoal Field;
		Finder.BuildField(Field, Goal, 100);
		std::vector<int> vRoute;
		return Finder.FindPath(Field, Start, vRoute);
	};

	// A one-way stopper lets routes pass in its direction only, also when
	// starting on top of it.
	std::vector<uint8_t> vStoppers(5);
	vStoppers[2] = Left | Left << 4;
	auto Graph = Grid(5, 1, {}, vStoppers);
	EXPECT_TRUE(Reachable(Graph, 0, 4));
	EXPECT_TRUE(Reachable(Graph, 2, 4));
	EXPECT_FALSE(Reachable(Graph, 4, 0));
	EXPECT_FALSE(Reachable(Graph, 4, 2));
	EXPECT_FALSE(Reachable(Graph, 2, 0));

	// Two-way stoppers block entering from both sides, but not leaving them.
	vStoppers[2] = Left | Right;
	Graph = Grid(5, 1, {}, vStoppers);
	EXPECT_FALSE(Reachable(Graph, 0, 4));
	EXPECT_FALSE(Reachable(Graph, 4, 0));
	EXPECT_TRUE(Reachable(Graph, 2, 0));
	EXPECT_TRUE(Reachable(Graph, 2, 4));

	// Diagonals need both ways around the corner. Here the tile right of the
	// start blocks moving down, so the route steps down first.
	vStoppers.assign(4, 0);
	vStoppers[1] = Down | Down << 4;
	Graph = Grid(2, 2, {}, vStoppers);
	for(int i = Graph.m_vOutOffsets[0]; i < Graph.m_vOutOffsets[1]; ++i)
		EXPECT_NE(Graph.m_vOutEdges[i].m_Node, 3);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 3, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{0, 2, 3}));
}

TEST(GTrainPath, ForwardGoalsLeadTowardsTheFinish)
{
	// The finish is beside the right column of a 9x3 room.
	const auto Graph = Grid(9, 3);
	CGTrainPathfinder Finder(Graph);
	std::vector<int> vDistances;
	Finder.FindDistances({8, 17, 26, 17, -1}, vDistances);
	EXPECT_EQ(vDistances[17], 0);
	EXPECT_EQ(vDistances[13], 4);
	EXPECT_EQ(vDistances[0], 8);

	// From the middle, goals 3 tiles away lie on both sides, forward ones
	// only on the finish's side, including diagonally.
	const int Start = 13;
	bool Backwards = false;
	for(unsigned Seed = 0; Seed < 64; ++Seed)
	{
		CGTrainGoal Goal;
		Finder.Build(Goal, Start, 3, Seed);
		Backwards |= Goal.m_Goal % 9 < 4;
		Finder.Build(Goal, Start, 3, Seed, &vDistances);
		ASSERT_TRUE(Goal.m_Ready);
		EXPECT_EQ(Goal.m_Distance, 3);
		EXPECT_EQ(Goal.m_Goal % 9, 7);
		EXPECT_EQ(vDistances[Goal.m_Goal], vDistances[Start] - 3);
	}
	EXPECT_TRUE(Backwards);

	// Close to the finish, forward goals are shorter.
	CGTrainGoal Goal;
	Finder.Build(Goal, 15, 5, 1, &vDistances);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Distance, 2);
	EXPECT_EQ(Goal.m_Goal % 9, 8);

	// Distances follow directed edges. Starts without a path to the finish
	// allow every goal.
	CGTrainPathGraph Teleport;
	for(int i = 0; i < 4; ++i)
		Teleport.m_vNodes.push_back({vec2(i * 320, 16), true});
	Teleport.InitEdges({{0, {3, 0}}, {3, {2, 1}}, {1, {0, 2}}});
	CGTrainPathfinder TeleportFinder(Teleport);
	TeleportFinder.FindDistances({2}, vDistances);
	EXPECT_EQ(vDistances, (std::vector<int>{2, 3, 0, 1}));
	TeleportFinder.FindDistances({1}, vDistances);
	EXPECT_EQ(vDistances, (std::vector<int>{-1, 0, -1, -1}));
	TeleportFinder.Build(Goal, 0, 2, 1, &vDistances);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Goal, 2);
}

TEST(GTrainPath, ReachableNodesWithinDepth)
{
	// Sources count as distance zero, duplicates and invalid sources are skipped.
	const auto Graph = Grid(8, 1);
	CGTrainPathfinder Finder(Graph);
	std::vector<int> vReachable;
	Finder.FindReachable({0, 0, -1, 100}, 2, vReachable);
	EXPECT_EQ(vReachable, (std::vector<int>{0, 1, 2}));
	Finder.FindReachable({2, 6}, 1, vReachable);
	std::sort(vReachable.begin(), vReachable.end());
	EXPECT_EQ(vReachable, (std::vector<int>{1, 2, 3, 5, 6, 7}));
	Finder.FindReachable({3}, 0, vReachable);
	EXPECT_EQ(vReachable, (std::vector<int>{3}));

	// Distances follow directed edges, teleports count as one step.
	CGTrainPathGraph Teleport;
	for(int i = 0; i < 4; ++i)
		Teleport.m_vNodes.push_back({vec2(i * 320, 16), true});
	Teleport.InitEdges({{0, {3, 0}}, {3, {2, 1}}, {1, {0, 2}}});
	CGTrainPathfinder TeleportFinder(Teleport);
	TeleportFinder.FindReachable({0}, 2, vReachable);
	EXPECT_EQ(vReachable, (std::vector<int>{0, 3, 2}));
}

TEST(GTrainPath, FallsThroughFreezeOntoUnfreeze)
{
	auto Grid = FallGrid({
		"...",
		"#f#",
		"#f#",
		"#u#",
		"...",
	});
	// Every tile of the fall counts, and there is no way back up.
	EXPECT_EQ(FallDistance(Grid, Grid.Node(1, 0), Grid.Node(1, 4)), 4);
	EXPECT_EQ(FallDistance(Grid, Grid.Node(1, 4), Grid.Node(1, 0)), -1);
	EXPECT_EQ(FallDistance(Grid, Grid.Node(0, 0), Grid.Node(2, 4)), 6);
	// Routes fall straight down.
	CGTrainPathfinder Finder(Grid.m_Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, Grid.Node(1, 4), 10);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, Grid.Node(1, 0), vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{Grid.Node(1, 0), Grid.FallNode(1, 1), Grid.FallNode(1, 2), Grid.Node(1, 3), Grid.Node(1, 4)}));
	EXPECT_FALSE(Grid.m_Graph.IsTeleportStep(Grid.Node(1, 0), Grid.FallNode(1, 1)));
}

TEST(GTrainPath, FallsNeedUnfreezeRightBelowTheFreeze)
{
	// Landing frozen, on the ground or in more freeze, is no way on.
	EXPECT_EQ(NumFallNodes(FallGrid({".", "f", "f", "#"})), 0);
	EXPECT_EQ(NumFallNodes(FallGrid({".", "f", ".", "#", "u"})), 0);
	EXPECT_EQ(NumFallNodes(FallGrid({".", "f", "f"})), 0);
	// Anything in the way stops the fall, and falls nothing can enter have
	// no nodes.
	auto Stopped = FallGrid({".", "f", "#", "f", "u"});
	EXPECT_EQ(NumFallNodes(Stopped), 0);
	EXPECT_EQ(FallDistance(Stopped, Stopped.Node(0, 0), Stopped.Node(0, 4)), -1);
	// Falls without freeze are just paths.
	EXPECT_EQ(NumFallNodes(FallGrid({".", ".", "u"})), 0);
	// Empty tiles between freeze are falling too, unfreeze only after it.
	auto Grid = FallGrid({".", "f", ".", "f", "u", "f", "u"});
	EXPECT_EQ(NumFallNodes(Grid), 4);
	EXPECT_LT(Grid.FallNode(0, 0), 0);
	EXPECT_GE(Grid.FallNode(0, 2), 0);
	EXPECT_LT(Grid.FallNode(0, 4), 0);
	EXPECT_EQ(FallDistance(Grid, Grid.Node(0, 0), Grid.Node(0, 6)), 6);
	// The empty tile in the fall is a node of its own too, to fall on from.
	EXPECT_EQ(FallDistance(Grid, Grid.Node(0, 2), Grid.Node(0, 4)), 2);
}

TEST(GTrainPath, FrozenTeesCantSteer)
{
	auto Grid = FallGrid({
		"#.#",
		"#f#",
		"...",
		"#f#",
		"#u#",
	});
	// Paths can't enter falls from the side or leave them, only the empty
	// tile in the middle has its sideways paths.
	for(int x : {0, 2})
	{
		EXPECT_EQ(FallDistance(Grid, Grid.Node(x, 2), Grid.Node(1, 4)), 3);
		EXPECT_EQ(FallDistance(Grid, Grid.Node(1, 0), Grid.Node(x, 2)), -1);
	}
	for(int Node : Grid.m_vFallNodes)
		if(Node >= 0)
			EXPECT_EQ(Grid.m_Graph.m_vOutOffsets[Node + 1] - Grid.m_Graph.m_vOutOffsets[Node], 1);
	// Stoppers holding tees above the freeze keep them out of the fall.
	auto Held = FallGrid({"v", "f", "u"});
	EXPECT_GE(Held.FallNode(0, 1), 0);
	EXPECT_EQ(FallDistance(Held, Held.Node(0, 0), Held.Node(0, 2)), -1);
}
