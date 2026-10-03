#ifndef GAME_SERVER_GAMEMODES_GTRAIN_PATH_H
#define GAME_SERVER_GAMEMODES_GTRAIN_PATH_H

#include <base/vmath.h>

#include <cstdint>
#include <utility>
#include <vector>

// Immutable directed graph shared by every player. Direction zero marks a
// teleport, which must never be drawn as a line through intervening tiles.
struct CGTrainPathGraph
{
	struct CNode
	{
		vec2 m_Pos;
		bool m_Goal;
	};
	struct CEdge
	{
		int m_Node;
		uint8_t m_Direction;
	};
	std::vector<CNode> m_vNodes;
	std::vector<int> m_vOutOffsets;
	std::vector<CEdge> m_vOutEdges;
	bool m_HasTeleports = false;

	void InitEdges(const std::vector<std::pair<int, CEdge>> &vEdges);
	static void AddTileEdges(int Index, int Width, int Height, const std::vector<int> &vTileNodes, std::vector<std::pair<int, CEdge>> &vEdges);
	bool IsTeleportStep(int From, int To) const;
};

struct CGTrainGoal
{
	int m_Goal = -1;
	int m_Distance = -1;
	bool m_Ready = false;
	std::vector<int> m_vInitialRoute;
};

// Search scratch is reused by all players. Visit stamps avoid clearing the
// entire map on each search; attempts store only their route, never a field.
class CGTrainPathfinder
{
	struct CState
	{
		uint32_t m_Stamp = 0;
		int m_Cost;
		int m_Parent;
	};
	struct COpen
	{
		int m_Node;
		int m_Cost;
		int m_Total;
		bool operator<(const COpen &Other) const;
	};
	const CGTrainPathGraph &m_Graph;
	std::vector<CState> m_vStates;
	std::vector<int> m_vQueue;
	std::vector<COpen> m_vOpen;
	uint32_t m_Stamp = 0;
	void BeginSearch();
	int Heuristic(int Node, int Goal) const;

public:
	explicit CGTrainPathfinder(const CGTrainPathGraph &Graph);
	void Build(CGTrainGoal &Goal, int Start, int Distance, unsigned Seed);
	bool FindPath(int Start, int Goal, std::vector<int> &vRoute);
};

#endif // GAME_SERVER_GAMEMODES_GTRAIN_PATH_H
