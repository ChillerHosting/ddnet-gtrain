#include "gtrain_map.h"

#include <base/log.h>

#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>

void CGTrainMap::Init(CCollision *pCollision, int StartExclusion)
{
	m_Width = pCollision->GetWidth();
	m_Height = pCollision->GetHeight();
	const int Width = m_Width;
	const int Height = m_Height;
	m_vTrainPositions.clear();
	m_PathGraph = CGTrainPathGraph();
	CGTrainPathfinder Pathfinder(m_PathGraph);

	const auto HasTile = [&](int Index, int Tile) {
		return pCollision->GetTileIndex(Index) == Tile || pCollision->GetFrontTileIndex(Index) == Tile;
	};
	const auto IsBlocked = [&](int Index) {
		const int Tile = pCollision->GetTileIndex(Index);
		return Tile == TILE_SOLID || Tile == TILE_NOHOOK;
	};
	const auto IsCheckTeleport = [&](int Index) {
		return pCollision->IsCheckTeleport(Index) || pCollision->IsCheckEvilTeleport(Index);
	};
	const auto ForEachNeighbor = [&](int Index, auto &&Func) {
		const int x = Index % Width;
		const int y = Index / Width;
		if(x > 0)
			Func(Index - 1);
		if(x < Width - 1)
			Func(Index + 1);
		if(y > 0)
			Func(Index - Width);
		if(y < Height - 1)
			Func(Index + Width);
	};

	// flood-fill every side of the start line, the start and the finish
	// line are not crossed, so only the sides reaching the finish are kept
	std::vector<int> vSideOf(Width * Height, 0);
	std::vector<bool> vWalked(Width * Height, false); // reached from a start line neighbor without teleporting
	std::vector<bool> vKept(Width * Height, false);
	std::vector<bool> vDiscarded(Width * Height, false);
	std::vector<int> vSide;
	std::vector<int> vTeleported;
	std::vector<int> vTeleCheckpoints;
	int NumSides = 0;
	bool FoundStart = false;
	for(int StartIndex = 0; StartIndex < Width * Height; StartIndex++)
	{
		if(!HasTile(StartIndex, TILE_START))
			continue;
		FoundStart = true;

		ForEachNeighbor(StartIndex, [&](int SeedIndex) {
			if(vWalked[SeedIndex] || IsBlocked(SeedIndex) || HasTile(SeedIndex, TILE_START) || HasTile(SeedIndex, TILE_FINISH))
				return;

			const int Side = ++NumSides;
			bool HitsFinish = false;
			bool HitsCheckTeleport = false;
			const auto Reach = [&](int Index, std::vector<int> &vQueue) {
				if(HasTile(Index, TILE_FINISH))
					HitsFinish = true;
				else if(vSideOf[Index] != Side && !IsBlocked(Index) && !HasTile(Index, TILE_START))
				{
					vSideOf[Index] = Side;
					vQueue.push_back(Index);
				}
			};
			const auto TeleportTo = [&](const std::vector<vec2> &vTeleOuts) {
				for(const vec2 &TeleOut : vTeleOuts)
					Reach(pCollision->GetPureMapIndex(TeleOut), vTeleported);
			};
			const auto TeleportToCheckpoint = [&](int TeleCheckpoint) {
				// same order as the character, fall back to previous checkpoints
				for(int k = TeleCheckpoint - 1; k >= 0; k--)
				{
					if(!pCollision->TeleCheckOuts(k).empty())
					{
						TeleportTo(pCollision->TeleCheckOuts(k));
						return;
					}
				}
			};

			vSide.clear();
			vTeleported.clear();
			vTeleCheckpoints.clear();
			Reach(SeedIndex, vSide);
			size_t NumDone = 0;
			bool Walking = true;
			while(true)
			{
				for(; NumDone < vSide.size(); NumDone++)
				{
					const int Index = vSide[NumDone];
					if(Walking)
						vWalked[Index] = true;

					// teleporters can't be passed, the fill continues at their destinations
					const int Teleport = pCollision->IsTeleport(Index) ? pCollision->IsTeleport(Index) : pCollision->IsEvilTeleport(Index);
					if(Teleport)
					{
						TeleportTo(pCollision->TeleOuts(Teleport - 1));
						continue;
					}
					if(IsCheckTeleport(Index))
					{
						if(!HitsCheckTeleport)
						{
							HitsCheckTeleport = true;
							for(const int TeleCheckpoint : vTeleCheckpoints)
								TeleportToCheckpoint(TeleCheckpoint);
						}
						continue;
					}
					const int TeleCheckpoint = pCollision->IsTeleCheckpoint(Index);
					if(TeleCheckpoint && std::find(vTeleCheckpoints.begin(), vTeleCheckpoints.end(), TeleCheckpoint) == vTeleCheckpoints.end())
					{
						vTeleCheckpoints.push_back(TeleCheckpoint);
						if(HitsCheckTeleport)
							TeleportToCheckpoint(TeleCheckpoint);
					}

					ForEachNeighbor(Index, [&](int Neighbor) { Reach(Neighbor, vSide); });
				}
				if(vTeleported.empty())
					break;
				Walking = false;
				vSide.insert(vSide.end(), vTeleported.begin(), vTeleported.end());
				vTeleported.clear();
			}

			for(const int Index : vSide)
			{
				if(HitsFinish)
					vKept[Index] = true;
				else
					vDiscarded[Index] = true;
			}
		});
	}

	// positions must have freeze straight below, before any solid tile
	const auto HasFreezeBelow = [&](int Index) {
		for(int i = 1; i <= MAX_FREEZE_DISTANCE && Index + i * Width < Width * Height; i++)
		{
			const int Below = Index + i * Width;
			if(IsBlocked(Below))
				return false;
			if(HasTile(Below, TILE_FREEZE) || HasTile(Below, TILE_DFREEZE))
				return true;
		}
		return false;
	};

	for(int Index = 0; Index < Width * Height; Index++)
	{
		// sides not reaching the finish stay discarded, even if a teleporter leads back to them
		if(!vKept[Index] || vDiscarded[Index])
			continue;
		// death tiles and teleporters are discarded as well, to not kill or move the player instantly,
		// and unfreeze tiles, which would end the hover right away
		if(HasTile(Index, TILE_FREEZE) || HasTile(Index, TILE_DFREEZE) || HasTile(Index, TILE_DEATH) || HasTile(Index, TILE_UNFREEZE))
			continue;
		if(pCollision->IsTeleport(Index) || pCollision->IsEvilTeleport(Index) || IsCheckTeleport(Index))
			continue;
		if(!HasFreezeBelow(Index))
			continue;
		m_vTrainPositions.emplace_back((Index % Width) * 32.0f + 16.0f, (Index / Width) * 32.0f + 16.0f);
	}

	if(m_vTrainPositions.empty())
		log_warn("gtrain", "no tiles found between start and finish line%s, training is disabled on this map", FoundStart ? "" : " (map has no start line)");
	else
		log_info("gtrain", "found %d tiles between start and finish line", (int)m_vTrainPositions.size());

	// Spawn eligibility and path eligibility differ: routes may pass safe tiles
	// without freeze below, but never route through freeze, death or race lines.
	// Checkpoint teleports depend on character history, so they cannot be
	// represented as unconditional edges in a shared static graph.
	const auto IsNodeTile = [&](int Index) {
		return vKept[Index] && !vDiscarded[Index] && !IsBlocked(Index) &&
		       !HasTile(Index, TILE_FREEZE) && !HasTile(Index, TILE_DFREEZE) && !HasTile(Index, TILE_DEATH) &&
		       !HasTile(Index, TILE_START) && !HasTile(Index, TILE_FINISH) && !IsCheckTeleport(Index);
	};
	// Only falling straight through freeze onto unfreeze crosses freeze.
	// Anything else in the way, like hookable or unhookable tiles, stoppers,
	// teleporters, speedups or deep freeze, could stop the fall or change it.
	std::vector<uint8_t> vFall(Width * Height, CGTrainPathGraph::FALL_STOP);
	std::vector<bool> vNodeTiles(Width * Height);
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		vNodeTiles[Index] = IsNodeTile(Index);
		const int Tile = pCollision->GetTileIndex(Index);
		const int FrontTile = pCollision->GetFrontTileIndex(Index);
		const auto Only = [&](int Allowed) { return (Tile == Allowed || Tile == TILE_AIR) && (FrontTile == Allowed || FrontTile == TILE_AIR); };
		CDoorTile Door;
		pCollision->GetDoorTile(Index, &Door);
		if(Only(TILE_UNFREEZE) && (Tile == TILE_UNFREEZE || FrontTile == TILE_UNFREEZE) && vNodeTiles[Index])
			vFall[Index] = CGTrainPathGraph::FALL_UNFREEZE;
		else if(Only(TILE_FREEZE) && !pCollision->IsTeleport(Index) && !pCollision->IsEvilTeleport(Index) && !IsCheckTeleport(Index) &&
			!pCollision->IsTeleCheckpoint(Index) && !pCollision->IsSpeedup(Index) && !pCollision->GetSwitchType(Index) && !Door.m_Index)
			vFall[Index] = Tile == TILE_FREEZE || FrontTile == TILE_FREEZE ? CGTrainPathGraph::FALL_FREEZE : CGTrainPathGraph::FALL_EMPTY;
	}
	const std::vector<bool> vFrozenFalls = CGTrainPathGraph::FindFrozenFalls(Width, Height, vFall, vNodeTiles);
	const int NumFallTiles = std::count(vFrozenFalls.begin(), vFrozenFalls.end(), true);
	if(NumFallTiles)
		log_info("gtrain", "found %d tiles to fall through frozen onto unfreeze", NumFallTiles);
	// Nodes follow the map rows, falling ones too, so goal fields stay small.
	m_vTileNodes.assign(Width * Height, -1);
	m_vFallNodes.assign(Width * Height, -1);
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		const vec2 Pos((Index % Width) * 32.0f + 16.0f, (Index / Width) * 32.0f + 16.0f);
		if(vNodeTiles[Index])
		{
			m_vTileNodes[Index] = (int)m_PathGraph.m_vNodes.size();
			m_PathGraph.m_vNodes.push_back({Pos, false});
		}
		if(vFrozenFalls[Index])
		{
			m_vFallNodes[Index] = (int)m_PathGraph.m_vNodes.size();
			m_PathGraph.m_vNodes.push_back({Pos, false});
		}
	}

	// Paths cannot pass stoppers against their direction. One-way stoppers
	// also block moving off them, like CCollision::GetMoveRestrictions.
	static_assert((int)CGTrainPathGraph::DIRECTION_LEFT == CANTMOVE_LEFT && (int)CGTrainPathGraph::DIRECTION_RIGHT == CANTMOVE_RIGHT &&
		      (int)CGTrainPathGraph::DIRECTION_UP == CANTMOVE_UP && (int)CGTrainPathGraph::DIRECTION_DOWN == CANTMOVE_DOWN);
	std::vector<uint8_t> vStoppers(Width * Height);
	for(int Index = 0; Index < Width * Height; ++Index)
		vStoppers[Index] = pCollision->GetTileMoveRestrictions(Index, false) | pCollision->GetTileMoveRestrictions(Index, true) << 4;
	std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		const int Node = m_vTileNodes[Index];
		if(Node < 0)
			continue;
		const int Teleport = pCollision->IsTeleport(Index) ? pCollision->IsTeleport(Index) : pCollision->IsEvilTeleport(Index);
		if(Teleport)
		{
			for(vec2 Out : pCollision->TeleOuts(Teleport - 1))
			{
				const int Destination = m_vTileNodes[pCollision->GetPureMapIndex(Out)];
				if(Destination >= 0)
					vEdges.push_back({Node, {Destination, 0}});
			}
			continue;
		}
		CGTrainPathGraph::AddTileEdges(Index, Width, Height, m_vTileNodes, vStoppers, vEdges);
	}
	CGTrainPathGraph::AddFallEdges(Width, m_vTileNodes, m_vFallNodes, vStoppers, vEdges);
	m_PathGraph.InitEdges(vEdges);
	Pathfinder.FindDistances(NodesBeside(pCollision, TILE_FINISH), m_vFinishDistances);

	ExcludeOffRace(pCollision, Pathfinder);
	ExcludeStartArea(pCollision, Pathfinder, StartExclusion);
	// Spots far enough from the finish for /forwards are a suffix then.
	std::stable_sort(m_vTrainPositions.begin(), m_vTrainPositions.end(), [&](vec2 A, vec2 B) {
		return m_vFinishDistances[PathNode(A)] < m_vFinishDistances[PathNode(B)];
	});
	for(vec2 Pos : m_vTrainPositions)
		m_PathGraph.m_vNodes[PathNode(Pos)].m_Goal = true;
}

std::vector<int> CGTrainMap::NodesBeside(CCollision *pCollision, int Tile) const
{
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	std::vector<int> vNodes;
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		if(pCollision->GetTileIndex(Index) != Tile && pCollision->GetFrontTileIndex(Index) != Tile)
			continue;
		const int Column = Index % Width;
		const int Row = Index / Width;
		for(const int Neighbor : {Column > 0 ? Index - 1 : -1, Column < Width - 1 ? Index + 1 : -1, Row > 0 ? Index - Width : -1, Row < Height - 1 ? Index + Width : -1})
			if(Neighbor >= 0 && m_vTileNodes[Neighbor] >= 0)
				vNodes.push_back(m_vTileNodes[Neighbor]);
	}
	return vNodes;
}

void CGTrainMap::ExcludeOffRace(CCollision *pCollision, CGTrainPathfinder &Pathfinder)
{
	// Tiles farther from the start line than the finish line is, or farther
	// from the finish line than the start line is, are off the race, like
	// whatever lies past the finish or behind the start, which the fill only
	// reaches through freeze, or long dead ends. Path distances are measured
	// like goals, which never cross freeze, so tiles without a path from the
	// start or to the finish are off the race too.
	if(m_vTrainPositions.empty())
		return;
	const std::vector<int> vStartNodes = NodesBeside(pCollision, TILE_START);
	int RaceDistance = -1;
	for(const int Node : vStartNodes)
		if(m_vFinishDistances[Node] >= 0 && (RaceDistance < 0 || m_vFinishDistances[Node] < RaceDistance))
			RaceDistance = m_vFinishDistances[Node];
	if(RaceDistance < 0)
	{
		log_warn("gtrain", "no path from the start line to the finish line, so no tiles are skipped for being off the race");
		return;
	}
	// The distances count from the tiles beside the lines, one tile from
	// their line. So the lines are RaceDistance + 2 apart, and tiles up to
	// RaceDistance + 1 from the tiles beside a line are no farther from it.
	const int MaxDistance = RaceDistance + 1;
	std::vector<int> vNearStart;
	Pathfinder.FindReachable(vStartNodes, MaxDistance, vNearStart);
	std::vector<bool> vInRace(m_PathGraph.m_vNodes.size(), false);
	for(const int Node : vNearStart)
		vInRace[Node] = m_vFinishDistances[Node] >= 0 && m_vFinishDistances[Node] <= MaxDistance;
	std::vector<vec2> vRemaining;
	for(vec2 Pos : m_vTrainPositions)
		if(vInRace[PathNode(Pos)])
			vRemaining.push_back(Pos);
	if(vRemaining.empty())
	{
		log_warn("gtrain", "all tiles are farther from one line than the other, keeping them");
		return;
	}
	log_info("gtrain", "skipped %d tiles farther from the start or finish line than the lines are apart (%d tiles)", (int)(m_vTrainPositions.size() - vRemaining.size()), RaceDistance + 2);
	m_vTrainPositions = std::move(vRemaining);
}

void CGTrainMap::ExcludeStartArea(CCollision *pCollision, CGTrainPathfinder &Pathfinder, int Distance)
{
	// Map starts are usually easy. No spawn or goal is placed within the
	// configured path distance of the start line, measured like goals.
	if(Distance <= 0 || m_vTrainPositions.empty())
		return;
	// Tiles beside the start line are already one tile away from it.
	std::vector<int> vNearStart;
	Pathfinder.FindReachable(NodesBeside(pCollision, TILE_START), Distance - 1, vNearStart);
	std::vector<bool> vExcluded(m_PathGraph.m_vNodes.size(), false);
	for(const int Node : vNearStart)
		vExcluded[Node] = true;
	std::vector<vec2> vRemaining;
	for(vec2 Pos : m_vTrainPositions)
		if(!vExcluded[PathNode(Pos)])
			vRemaining.push_back(Pos);
	if(vRemaining.empty())
	{
		log_warn("gtrain", "all tiles are within %d tiles of the start line, keeping them", Distance);
		return;
	}
	log_info("gtrain", "skipped %d tiles within %d tiles of the start line", (int)(m_vTrainPositions.size() - vRemaining.size()), Distance);
	m_vTrainPositions = std::move(vRemaining);
}

int CGTrainMap::PathNode(vec2 Pos) const
{
	// Do not use the collision lookup's clamping outside the map.
	const int Width = m_Width;
	const int Height = m_Height;
	if(Pos.x < 0 || Pos.y < 0 || Pos.x >= Width * 32.0f || Pos.y >= Height * 32.0f)
		return -1;
	// Tees falling through freeze are on its falling node.
	const int Index = (int)(Pos.y / 32) * Width + (int)(Pos.x / 32);
	return m_vTileNodes[Index] >= 0 ? m_vTileNodes[Index] : m_vFallNodes[Index];
}
