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
	// Edge direction bits, also used for the directions stoppers block.
	enum
	{
		DIRECTION_LEFT = 1 << 0,
		DIRECTION_RIGHT = 1 << 1,
		DIRECTION_UP = 1 << 2,
		DIRECTION_DOWN = 1 << 3,
	};
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
	// Reverse adjacency, for searching towards a goal across one-way teleports.
	std::vector<int> m_vInOffsets;
	std::vector<int> m_vInNodes;

	// How a tee falling straight down passes a tile.
	enum
	{
		FALL_STOP, // anything that could stop or turn the fall
		FALL_EMPTY,
		FALL_FREEZE,
		FALL_UNFREEZE, // a node, where frozen tees land unfrozen
	};

	void InitEdges(const std::vector<std::pair<int, CEdge>> &vEdges);
	// vStoppers holds per tile the directions blocked moving onto it in the
	// low four bits and moving off it in the high four bits, or is empty.
	static void AddTileEdges(int Index, int Width, int Height, const std::vector<int> &vTileNodes, const std::vector<uint8_t> &vStoppers, std::vector<std::pair<int, CEdge>> &vEdges);
	// The tiles tees fall through frozen to an unfreeze tile, from a freeze
	// tile straight down with only freeze and empty tiles in between. Falls
	// start below a tile in vNodes, others can't be entered.
	static std::vector<bool> FindFrozenFalls(int Width, int Height, const std::vector<uint8_t> &vFall, const std::vector<bool> &vNodes);
	// Frozen tees can't steer, so the nodes of falls in vFallNodes only lead
	// down, to the next one or the unfreeze tile. Tees enter them from the
	// node of the tile above.
	static void AddFallEdges(int Width, const std::vector<int> &vTileNodes, const std::vector<int> &vFallNodes, const std::vector<uint8_t> &vStoppers, std::vector<std::pair<int, CEdge>> &vEdges);
	bool IsTeleportStep(int From, int To) const;
};

struct CGTrainGoal
{
	enum
	{
		NO_DISTANCE = 0xffff,
	};
	int m_Goal = -1;
	int m_Distance = -1;
	bool m_Ready = false;
	std::vector<int> m_vInitialRoute;
	// Path distances to the goal, indexed from the lowest node within the
	// field depth. Routes descend these distances instead of searching.
	int m_FieldBegin = 0;
	std::vector<uint16_t> m_vField;

	int FieldDistance(int Node) const
	{
		const int Index = Node - m_FieldBegin;
		if(Index < 0 || Index >= (int)m_vField.size() || m_vField[Index] == NO_DISTANCE)
			return -1;
		return m_vField[Index];
	}
};

// Search scratch is reused by all players. Visit stamps avoid clearing the
// entire map on each search. Each goal stores a distance field once, so
// routing a player who leaves the planned route never searches again.
class CGTrainPathfinder
{
	struct CState
	{
		uint32_t m_Stamp = 0;
		int m_Cost;
	};
	const CGTrainPathGraph &m_Graph;
	std::vector<CState> m_vStates;
	std::vector<int> m_vQueue;
	uint32_t m_Stamp = 0;
	void BeginSearch();

public:
	enum
	{
		// Routes cover straying behind the start by the goal distance again,
		// plus this many tiles, so short goals still allow some detours.
		FIELD_MARGIN = 32,
		MAX_FIELD_DEPTH = CGTrainGoal::NO_DISTANCE - 1,
	};
	explicit CGTrainPathfinder(const CGTrainPathGraph &Graph);
	// With pFinishDistances from FindDistances, goals must lie on a shortest
	// path from Start to the finish, so they are ahead of it. Starts without
	// a path to the finish allow every goal.
	void Build(CGTrainGoal &Goal, int Start, int Distance, unsigned Seed, const std::vector<int> *pFinishDistances = nullptr);
	void BuildField(CGTrainGoal &Goal, int Node, int Depth);
	bool FindPath(const CGTrainGoal &Goal, int Start, std::vector<int> &vRoute) const;
	// Every node within Depth steps of any source, sources included.
	void FindReachable(const std::vector<int> &vSources, int Depth, std::vector<int> &vReachable);
	// Path distance from every node to the nearest target, 0 for targets and
	// -1 without a path to one.
	void FindDistances(const std::vector<int> &vTargets, std::vector<int> &vDistances);
};

#endif // GAME_SERVER_GAMEMODES_GTRAIN_PATH_H
