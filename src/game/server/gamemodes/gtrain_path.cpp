#include "gtrain_path.h"

#include <algorithm>
#include <random>

void CGTrainPathGraph::AddTileEdges(int Index, int Width, int Height, const std::vector<int> &vTileNodes, std::vector<std::pair<int, CEdge>> &vEdges)
{
	const int Node = vTileNodes[Index];
	if(Node < 0)
		return;
	const int x = Index % Width, y = Index / Width;
	for(int dy = -1; dy <= 1; ++dy)
		for(int dx = -1; dx <= 1; ++dx)
		{
			if((dx == 0 && dy == 0) || x + dx < 0 || x + dx >= Width || y + dy < 0 || y + dy >= Height)
				continue;
			const int Destination = vTileNodes[(y + dy) * Width + x + dx];
			if(Destination < 0)
				continue;
			// Both tiles beside a diagonal must be safe. Merely checking its
			// destination would allow squeezing past solid or freeze corners.
			if(dx != 0 && dy != 0 && (vTileNodes[y * Width + x + dx] < 0 || vTileNodes[(y + dy) * Width + x] < 0))
				continue;
			const uint8_t Direction = (dx < 0 ? 1 : dx > 0 ? 2 :
									 0) |
						  (dy < 0 ? 4 : dy > 0 ? 8 :
									 0);
			vEdges.push_back({Node, {Destination, Direction}});
		}
}

void CGTrainPathGraph::InitEdges(const std::vector<std::pair<int, CEdge>> &vEdges)
{
	m_HasTeleports = false;
	m_vOutOffsets.assign(m_vNodes.size() + 1, 0);
	for(const auto &Edge : vEdges)
	{
		++m_vOutOffsets[Edge.first + 1];
		m_HasTeleports |= Edge.second.m_Direction == 0;
	}
	for(size_t i = 1; i < m_vOutOffsets.size(); ++i)
		m_vOutOffsets[i] += m_vOutOffsets[i - 1];
	m_vOutEdges.resize(vEdges.size());
	auto vCursor = m_vOutOffsets;
	for(const auto &Edge : vEdges)
		m_vOutEdges[vCursor[Edge.first]++] = Edge.second;
}

bool CGTrainPathGraph::IsTeleportStep(int From, int To) const
{
	for(int i = m_vOutOffsets[From]; i < m_vOutOffsets[From + 1]; ++i)
		if(m_vOutEdges[i].m_Node == To)
			return m_vOutEdges[i].m_Direction == 0;
	return false;
}

CGTrainPathfinder::CGTrainPathfinder(const CGTrainPathGraph &Graph) :
	m_Graph(Graph)
{
}

bool CGTrainPathfinder::COpen::operator<(const COpen &Other) const
{
	// Minimum total first; deeper nodes first on ties to avoid exploring
	// every equally promising tile in a large open rectangle.
	return m_Total != Other.m_Total ? m_Total > Other.m_Total : m_Cost < Other.m_Cost;
}

void CGTrainPathfinder::BeginSearch()
{
	m_vStates.resize(m_Graph.m_vNodes.size());
	if(++m_Stamp == 0)
	{
		for(auto &State : m_vStates)
			State.m_Stamp = 0;
		++m_Stamp;
	}
}

int CGTrainPathfinder::Heuristic(int Node, int Goal) const
{
	// Chebyshev distance is admissible when a diagonal counts as one tile
	// step, matching the goal-distance BFS. Teleports
	// can shortcut it arbitrarily: use a zero heuristic on those maps.
	if(m_Graph.m_HasTeleports)
		return 0;
	const vec2 Offset = m_Graph.m_vNodes[Node].m_Pos - m_Graph.m_vNodes[Goal].m_Pos;
	return round_to_int(std::max(std::abs(Offset.x), std::abs(Offset.y)) / 32.0f);
}

bool CGTrainPathfinder::FindPath(int Start, int Goal, std::vector<int> &vRoute)
{
	vRoute.clear();
	if(Start < 0 || Goal < 0 || Start >= (int)m_Graph.m_vNodes.size() || Goal >= (int)m_Graph.m_vNodes.size())
		return false;
	BeginSearch();
	m_vOpen.clear();
	m_vStates[Start] = {m_Stamp, 0, -1};
	m_vOpen.push_back({Start, 0, Heuristic(Start, Goal)});
	while(!m_vOpen.empty())
	{
		std::pop_heap(m_vOpen.begin(), m_vOpen.end());
		const COpen Current = m_vOpen.back();
		m_vOpen.pop_back();
		if(Current.m_Cost != m_vStates[Current.m_Node].m_Cost)
			continue;
		if(Current.m_Node == Goal)
		{
			for(int Node = Goal; Node >= 0; Node = m_vStates[Node].m_Parent)
				vRoute.push_back(Node);
			std::reverse(vRoute.begin(), vRoute.end());
			return true;
		}
		for(int i = m_Graph.m_vOutOffsets[Current.m_Node]; i < m_Graph.m_vOutOffsets[Current.m_Node + 1]; ++i)
		{
			const int Next = m_Graph.m_vOutEdges[i].m_Node;
			CState &State = m_vStates[Next];
			const int Cost = Current.m_Cost + 1;
			if(State.m_Stamp == m_Stamp && State.m_Cost <= Cost)
				continue;
			State = {m_Stamp, Cost, Current.m_Node};
			m_vOpen.push_back({Next, Cost, Cost + Heuristic(Next, Goal)});
			std::push_heap(m_vOpen.begin(), m_vOpen.end());
		}
	}
	return false;
}

void CGTrainPathfinder::Build(CGTrainGoal &Goal, int Start, int Distance, unsigned Seed)
{
	Goal.m_Ready = false;
	Goal.m_Goal = -1;
	Goal.m_Distance = -1;
	Goal.m_vInitialRoute.clear();
	if(Start < 0 || Start >= (int)m_Graph.m_vNodes.size())
		return;
	BeginSearch();
	m_vQueue.clear();
	m_vQueue.push_back(Start);
	m_vStates[Start] = {m_Stamp, 0, -1};
	std::mt19937 Random(Seed);
	int NumCandidates = 0;
	// BFS chooses a goal by actual path tile distance, not raw displacement.
	for(size_t Head = 0; Head < m_vQueue.size(); ++Head)
	{
		const int Node = m_vQueue[Head];
		const int Depth = m_vStates[Node].m_Cost;
		if(Depth > 0 && m_Graph.m_vNodes[Node].m_Goal)
		{
			if(Depth > Goal.m_Distance)
			{
				Goal.m_Distance = Depth;
				NumCandidates = 0;
			}
			if(std::uniform_int_distribution<int>(1, ++NumCandidates)(Random) == 1)
				Goal.m_Goal = Node;
		}
		if(Depth >= Distance)
			continue;
		for(int i = m_Graph.m_vOutOffsets[Node]; i < m_Graph.m_vOutOffsets[Node + 1]; ++i)
		{
			const int Next = m_Graph.m_vOutEdges[i].m_Node;
			if(m_vStates[Next].m_Stamp == m_Stamp)
				continue;
			m_vStates[Next] = {m_Stamp, Depth + 1, Node};
			m_vQueue.push_back(Next);
		}
	}
	if(Goal.m_Goal >= 0)
	{
		// The goal selection already found a shortest route. Reuse its
		// parents; A* is needed only when the player leaves this route.
		for(int Node = Goal.m_Goal; Node >= 0; Node = m_vStates[Node].m_Parent)
			Goal.m_vInitialRoute.push_back(Node);
		std::reverse(Goal.m_vInitialRoute.begin(), Goal.m_vInitialRoute.end());
		Goal.m_Ready = true;
	}
}
