#ifndef GAME_SERVER_GAMEMODES_GTRAIN_MAP_H
#define GAME_SERVER_GAMEMODES_GTRAIN_MAP_H

#include "gtrain_path.h"

#include <base/vmath.h>

#include <vector>

class CCollision;

// The spots GTrain places players and flags on, and the path graph between
// them. The server and the gtrain_rate_tiles tool find them the same way.
class CGTrainMap
{
	int m_Width = 0;
	int m_Height = 0;
	void ExcludeOffRace(CCollision *pCollision, CGTrainPathfinder &Pathfinder);
	void ExcludeStartArea(CCollision *pCollision, CGTrainPathfinder &Pathfinder, int Distance);

public:
	enum
	{
		MAX_FREEZE_DISTANCE = 20,
	};

	// centers of all non-freeze tiles between the start and the finish line,
	// no farther from either line than the lines are apart, ordered by their
	// path distance to the finish
	std::vector<vec2> m_vTrainPositions;
	CGTrainPathGraph m_PathGraph;
	std::vector<int> m_vTileNodes;
	// nodes of tiles tees fall through frozen onto unfreeze, -1 elsewhere
	std::vector<int> m_vFallNodes;
	// path distance of every node to the finish line, -1 without a path
	std::vector<int> m_vFinishDistances;

	// Finds everything for the map of the collision, without the doors of
	// its entities, which do not exist yet when the server does this.
	// StartExclusion is sv_gtrain_start_exclusion.
	void Init(CCollision *pCollision, int StartExclusion);
	// path nodes beside tiles of this game or front layer index
	std::vector<int> NodesBeside(CCollision *pCollision, int Tile) const;
	// The node of the tile at Pos, or its falling node in freeze, or -1.
	int PathNode(vec2 Pos) const;
};

#endif // GAME_SERVER_GAMEMODES_GTRAIN_MAP_H
