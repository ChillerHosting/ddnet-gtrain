#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/shared/map.h>
#include <engine/storage.h>

#include <game/collision.h>
#include <game/layers.h>
#include <game/mapitems.h>
#include <game/server/gamemodes/gtrain_map.h>
#include <game/server/gamemodes/gtrain_path.h>
#include <game/server/gamemodes/gtrain_tiles.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static const char *TOOL_NAME = "gtrain_rate_tiles";

// Rates how hard the GTrain spots of a map are (see CGTrainTileRatings). The
// ratings file is what the server loads, the route file compares maps with
// their difficulty ratings: the race from the start to the finish line, with
// the rating of every tile on it.
static void Usage()
{
	log_error(TOOL_NAME, "Usage: %s [-j <threads>] [-e <start exclusion>] [-p <settings>] [-o <ratings> [-f]] [-r <route csv> [-w <wall weight>] [-i <image ppm>]] <map>", TOOL_NAME);
	log_error(TOOL_NAME, "  -j: threads for the simulation, the number of cores by default");
	log_error(TOOL_NAME, "  -e: sv_gtrain_start_exclusion of the spots, 50 by default");
	log_error(TOOL_NAME, "  -o: rate all spots and save them, or load them if the file is up to date");
	log_error(TOOL_NAME, "  -f: rate all spots again, even if the ratings file is up to date");
	log_error(TOOL_NAME, "  -r: write the route with the survived ticks of its spots; without -o, only those are rated");
	log_error(TOOL_NAME, "  -w: how much the route avoids walls, freeze and the like, 8 by default");
	log_error(TOOL_NAME, "  -i: write the map with the route as an image, a pixel per tile");
	log_error(TOOL_NAME, "  -p: rating settings to try, like \"aimpercent=75,hookpercent=50\" (see CGTrainRatingSettings)");
}

static CGTrainRatingSettings s_Settings;

static bool RateSpots(const std::vector<uint8_t> &vMapData, CCollision *pCollision, const CGTrainMap &TrainMap, const SHA256_DIGEST &MapSha256, std::vector<int> vSpots, int NumThreads, CGTrainTileRatings &Ratings)
{
	CGTrainTileRatingJob Job;
	if(!Job.Start(vMapData.data(), vMapData.size(), pCollision, TrainMap.m_vTileNodes, MapSha256, std::move(vSpots), NumThreads, false, s_Settings))
	{
		log_error(TOOL_NAME, "ddnet_physics failed to load the map");
		return false;
	}
	const auto Start = std::chrono::steady_clock::now();
	auto LastReport = Start;
	while(!Job.Done())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const auto Now = std::chrono::steady_clock::now();
		if(Now - LastReport >= std::chrono::seconds(5))
		{
			LastReport = Now;
			log_info(TOOL_NAME, "rated %d of %d spots", Job.NumRated(), Job.NumSpots());
		}
	}
	if(Job.Failed())
	{
		log_error(TOOL_NAME, "the map has no spawn for the simulated tee");
		return false;
	}
	const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
	log_info(TOOL_NAME, "rated %d spots in %.1f s (%.0f spots/s)", Job.NumSpots(), Seconds, Job.NumSpots() / std::max(Seconds, 1e-9));
	Ratings = Job.TakeRatings();
	return true;
}

// The distance in tiles (diagonals count as one) of every tile to the nearest
// tile paths can't go through, like solid tiles, freeze or the lines, and to
// the outside of the map. 0 for those tiles themselves.
static std::vector<int> WallDistances(const CGTrainMap &TrainMap, int Width, int Height)
{
	std::vector<int> vDistances((size_t)Width * Height, -1);
	std::vector<int> vQueue;
	for(int Index = 0; Index < Width * Height; ++Index)
		if(TrainMap.m_vTileNodes[Index] < 0)
		{
			vDistances[Index] = 0;
			vQueue.push_back(Index);
		}
	// The outside is one tile beyond the border, so the border tiles come
	// right after all walls in the queue, which keeps it ordered by distance.
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		const int TileX = Index % Width, TileY = Index / Width;
		if(vDistances[Index] < 0 && (TileX == 0 || TileY == 0 || TileX == Width - 1 || TileY == Height - 1))
		{
			vDistances[Index] = 1;
			vQueue.push_back(Index);
		}
	}
	for(size_t Head = 0; Head < vQueue.size(); ++Head)
	{
		const int Index = vQueue[Head];
		const int TileX = Index % Width, TileY = Index / Width;
		for(int dy = -1; dy <= 1; ++dy)
			for(int dx = -1; dx <= 1; ++dx)
			{
				if(TileX + dx < 0 || TileX + dx >= Width || TileY + dy < 0 || TileY + dy >= Height)
					continue;
				const int Neighbor = Index + dy * Width + dx;
				if(vDistances[Neighbor] < 0)
				{
					vDistances[Neighbor] = vDistances[Index] + 1;
					vQueue.push_back(Neighbor);
				}
			}
	}
	return vDistances;
}

// The cheapest path from the start line to the finish line, where a tile
// costs 1 + WallWeight / its wall distance. Shortest paths hug the walls in
// every turn, players keep away from them where there is room. Frozen falls
// cost 1 per tile, nobody chooses where they go.
static std::vector<int> RaceRoute(CCollision *pCollision, const CGTrainMap &TrainMap, float WallWeight)
{
	const int Width = pCollision->GetWidth();
	const CGTrainPathGraph &Graph = TrainMap.m_PathGraph;
	const int NumNodes = Graph.m_vNodes.size();
	const std::vector<int> vWallDistances = WallDistances(TrainMap, Width, pCollision->GetHeight());
	std::vector<float> vCosts(NumNodes);
	for(int Node = 0; Node < NumNodes; ++Node)
	{
		const vec2 Pos = Graph.m_vNodes[Node].m_Pos;
		const int Index = (int)(Pos.y / 32) * Width + (int)(Pos.x / 32);
		vCosts[Node] = TrainMap.m_vTileNodes[Index] == Node ? 1.0f + WallWeight / std::max(vWallDistances[Index], 1) : 1.0f;
	}

	std::vector<float> vDistances(NumNodes, std::numeric_limits<float>::infinity());
	std::vector<int> vPrevious(NumNodes, -1);
	using CEntry = std::pair<float, int>;
	std::priority_queue<CEntry, std::vector<CEntry>, std::greater<>> Queue;
	for(const int Node : TrainMap.NodesBeside(pCollision, TILE_START))
		if(vDistances[Node] > 0)
		{
			vDistances[Node] = 0;
			Queue.emplace(0.0f, Node);
		}
	int End = -1;
	while(!Queue.empty())
	{
		const auto [Distance, Node] = Queue.top();
		Queue.pop();
		if(Distance > vDistances[Node])
			continue;
		if(TrainMap.m_vFinishDistances[Node] == 0)
		{
			End = Node;
			break;
		}
		for(int i = Graph.m_vOutOffsets[Node]; i < Graph.m_vOutOffsets[Node + 1]; ++i)
		{
			const int Next = Graph.m_vOutEdges[i].m_Node;
			const float NextDistance = Distance + vCosts[Next];
			if(NextDistance < vDistances[Next])
			{
				vDistances[Next] = NextDistance;
				vPrevious[Next] = Node;
				Queue.emplace(NextDistance, Next);
			}
		}
	}
	std::vector<int> vRoute;
	for(int Node = End; Node >= 0; Node = vPrevious[Node])
		vRoute.push_back(Node);
	std::reverse(vRoute.begin(), vRoute.end());
	return vRoute;
}

static bool WriteImage(const char *pFilename, CCollision *pCollision, const CGTrainMap &TrainMap, const std::vector<int> &vRoute)
{
	const int Width = pCollision->GetWidth(), Height = pCollision->GetHeight();
	std::vector<uint8_t> vPixels((size_t)Width * Height * 3);
	const auto Color = [&](int Index, uint8_t r, uint8_t g, uint8_t b) {
		vPixels[Index * 3] = r;
		vPixels[Index * 3 + 1] = g;
		vPixels[Index * 3 + 2] = b;
	};
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		const auto Has = [&](int Tile) { return pCollision->GetTileIndex(Index) == Tile || pCollision->GetFrontTileIndex(Index) == Tile; };
		if(Has(TILE_SOLID))
			Color(Index, 110, 110, 120);
		else if(Has(TILE_NOHOOK))
			Color(Index, 150, 90, 200);
		else if(Has(TILE_FREEZE) || Has(TILE_DFREEZE))
			Color(Index, 60, 150, 200);
		else if(Has(TILE_DEATH))
			Color(Index, 200, 50, 50);
		else if(Has(TILE_START) || Has(TILE_FINISH))
			Color(Index, 230, 200, 40);
		else if(Has(TILE_UNFREEZE))
			Color(Index, 60, 180, 90);
		else
			Color(Index, 15, 20, 35);
	}
	for(const int Node : vRoute)
	{
		const vec2 Pos = TrainMap.m_PathGraph.m_vNodes[Node].m_Pos;
		Color((int)(Pos.y / 32) * Width + (int)(Pos.x / 32), 255, 60, 60);
	}
	IOHANDLE File = io_open(pFilename, IOFLAG_WRITE);
	if(!File)
		return false;
	char aHeader[64];
	str_format(aHeader, sizeof(aHeader), "P6\n%d %d\n255\n", Width, Height);
	io_write(File, aHeader, str_length(aHeader));
	io_write(File, vPixels.data(), vPixels.size());
	return io_close(File) == 0;
}

static bool WriteRoute(const char *pFilename, CCollision *pCollision, const CGTrainMap &TrainMap, const std::vector<int> &vRoute, const CGTrainTileRatings &Ratings)
{
	const int Width = pCollision->GetWidth();
	const int NumSamples = Ratings.m_NumSamples;
	std::vector<int> vSpotOf((size_t)Width * pCollision->GetHeight(), -1);
	for(int Spot = 0; Spot < (int)Ratings.m_vSpots.size(); ++Spot)
		vSpotOf[Ratings.m_vSpots[Spot]] = Spot;
	IOHANDLE File = io_open(pFilename, IOFLAG_WRITE);
	if(!File)
		return false;
	// The first line has the settings that matter for reading it.
	std::string Header = "# maxticks=" + std::to_string(Ratings.m_MaxTicks) + " samples=" + std::to_string(NumSamples) + "\nx,y,spot,difficulty";
	for(int Sample = 0; Sample < NumSamples; ++Sample)
		Header += ",s" + std::to_string(Sample);
	Header += ",hookable,unhookable,open,depth,ceiling\n";
	io_write(File, Header.c_str(), Header.size());
	double Difficulty = 0;
	int NumRated = 0;
	for(const int Node : vRoute)
	{
		const vec2 Pos = TrainMap.m_PathGraph.m_vNodes[Node].m_Pos;
		const int TileX = (int)(Pos.x / 32), TileY = (int)(Pos.y / 32);
		const int Index = TileY * Width + TileX;
		// Tees falling through freeze are frozen, their tiles have no rating.
		const int Spot = TrainMap.m_vTileNodes[Index] == Node ? vSpotOf[Index] : -1;
		std::string Line = std::to_string(TileX) + "," + std::to_string(TileY) + "," + (Spot >= 0 ? "1," : "0,");
		if(Spot >= 0)
		{
			char aDifficulty[16];
			str_format(aDifficulty, sizeof(aDifficulty), "%.4f", Ratings.SpotDifficulty(Spot));
			Line += aDifficulty;
			for(int Sample = 0; Sample < NumSamples; ++Sample)
				Line += "," + std::to_string(Ratings.m_vSurvivedTicks[(size_t)Spot * NumSamples + Sample]);
			const uint8_t *pFeatures = &Ratings.m_vFeatures[(size_t)Spot * CGTrainTileRatings::NUM_FEATURES];
			char aFeatures[96];
			str_format(aFeatures, sizeof(aFeatures), ",%.4f,%.4f,%d,%d,%d",
				pFeatures[CGTrainTileRatings::FEATURE_HOOKABLE] / (float)CGTrainTileRatings::NUM_DIRECTIONS,
				pFeatures[CGTrainTileRatings::FEATURE_UNHOOKABLE] / (float)CGTrainTileRatings::NUM_DIRECTIONS,
				pFeatures[CGTrainTileRatings::FEATURE_OPEN], pFeatures[CGTrainTileRatings::FEATURE_DEPTH], pFeatures[CGTrainTileRatings::FEATURE_CEILING]);
			Line += aFeatures;
			Difficulty += Ratings.SpotDifficulty(Spot);
			++NumRated;
		}
		else
			Line += std::string(NumSamples + 5, ',');
		Line += "\n";
		io_write(File, Line.c_str(), Line.size());
	}
	log_info(TOOL_NAME, "route=%d rated=%d mean_route_difficulty=%.4f", (int)vRoute.size(), NumRated, NumRated ? Difficulty / NumRated : 0.0);
	return io_close(File) == 0;
}

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	int NumThreads = std::max(1u, std::thread::hardware_concurrency());
	int StartExclusion = 50;
	float WallWeight = 8.0f;
	bool Force = false;
	const char *pRatingsPath = nullptr;
	const char *pRoutePath = nullptr;
	const char *pImagePath = nullptr;
	const char *pMapPath = nullptr;
	for(int i = 1; i < argc; ++i)
	{
		if(str_comp(argv[i], "-j") == 0 && i + 1 < argc)
			NumThreads = str_toint(argv[++i]);
		else if(str_comp(argv[i], "-e") == 0 && i + 1 < argc)
			StartExclusion = str_toint(argv[++i]);
		else if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
			WallWeight = str_tofloat(argv[++i]);
		else if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
			pRatingsPath = argv[++i];
		else if(str_comp(argv[i], "-r") == 0 && i + 1 < argc)
			pRoutePath = argv[++i];
		else if(str_comp(argv[i], "-i") == 0 && i + 1 < argc)
			pImagePath = argv[++i];
		else if(str_comp(argv[i], "-p") == 0 && i + 1 < argc)
		{
			if(!s_Settings.Parse(argv[++i]))
			{
				log_error(TOOL_NAME, "Unknown rating settings '%s'", argv[i]);
				return -1;
			}
		}
		else if(str_comp(argv[i], "-f") == 0)
			Force = true;
		else if(!pMapPath && argv[i][0] != '-')
			pMapPath = argv[i];
		else
		{
			pMapPath = nullptr;
			break;
		}
	}
	if(!pMapPath || (!pRatingsPath && !pRoutePath) || NumThreads < 1)
	{
		Usage();
		return -1;
	}

	std::unique_ptr<IStorage> pStorage(CreateStorage(IStorage::EInitializationType::BASIC, argc, argv));
	if(!pStorage)
	{
		log_error(TOOL_NAME, "Error creating basic storage");
		return -1;
	}
	CMap Map;
	if(!Map.Load(pStorage.get(), pMapPath, IStorage::TYPE_ABSOLUTE))
	{
		log_error(TOOL_NAME, "Error loading map '%s'", pMapPath);
		return -1;
	}
	std::vector<uint8_t> vMapData;
	{
		IOHANDLE File = io_open(pMapPath, IOFLAG_READ);
		void *pData = nullptr;
		unsigned Size = 0;
		if(!File || !io_read_all(File, &pData, &Size))
		{
			log_error(TOOL_NAME, "Error reading map '%s'", pMapPath);
			if(File)
				io_close(File);
			return -1;
		}
		io_close(File);
		vMapData.assign((uint8_t *)pData, (uint8_t *)pData + Size);
		free(pData);
	}
	CLayers Layers;
	Layers.Init(&Map, false, false);
	if(!Layers.GameLayer())
	{
		log_error(TOOL_NAME, "The map '%s' has no game layer", pMapPath);
		return -1;
	}
	CCollision Collision;
	Collision.Init(&Layers);
	CGTrainMap TrainMap;
	TrainMap.Init(&Collision, StartExclusion);
	const int Width = Collision.GetWidth();

	const std::vector<int> vRoute = pRoutePath ? RaceRoute(&Collision, TrainMap, WallWeight) : std::vector<int>();
	if(pRoutePath && vRoute.empty())
		log_info(TOOL_NAME, "route=none");
	if(pImagePath && !WriteImage(pImagePath, &Collision, TrainMap, vRoute))
		log_error(TOOL_NAME, "Error writing '%s'", pImagePath);

	CGTrainTileRatings Ratings;
	if(pRatingsPath)
	{
		std::vector<int> vSpots;
		for(const vec2 Pos : TrainMap.m_vTrainPositions)
			vSpots.push_back((int)(Pos.y / 32) * Width + (int)(Pos.x / 32));
		std::sort(vSpots.begin(), vSpots.end());
		if(!Force && Ratings.Load(pRatingsPath, Map.Sha256()) && Ratings.m_vSpots == vSpots)
			log_info(TOOL_NAME, "'%s' is up to date", pRatingsPath);
		else
		{
			if(!RateSpots(vMapData, &Collision, TrainMap, Map.Sha256(), std::move(vSpots), NumThreads, Ratings))
				return -1;
			if(!Ratings.Save(pRatingsPath))
			{
				log_error(TOOL_NAME, "Error saving '%s'", pRatingsPath);
				return -1;
			}
		}
		double Difficulty = 0;
		for(int Spot = 0; Spot < (int)Ratings.m_vSpots.size(); ++Spot)
			Difficulty += Ratings.SpotDifficulty(Spot);
		log_info(TOOL_NAME, "spots=%d mean_spot_difficulty=%.4f", (int)Ratings.m_vSpots.size(), Ratings.m_vSpots.empty() ? 0.0 : Difficulty / Ratings.m_vSpots.size());
	}
	else if(!vRoute.empty())
	{
		// Only the spots on the route.
		std::vector<bool> vIsSpot((size_t)Width * Collision.GetHeight(), false);
		for(const vec2 Pos : TrainMap.m_vTrainPositions)
			vIsSpot[(int)(Pos.y / 32) * Width + (int)(Pos.x / 32)] = true;
		std::vector<int> vSpots;
		for(const int Node : vRoute)
		{
			const vec2 Pos = TrainMap.m_PathGraph.m_vNodes[Node].m_Pos;
			const int Index = (int)(Pos.y / 32) * Width + (int)(Pos.x / 32);
			if(TrainMap.m_vTileNodes[Index] == Node && vIsSpot[Index])
				vSpots.push_back(Index);
		}
		std::sort(vSpots.begin(), vSpots.end());
		vSpots.erase(std::unique(vSpots.begin(), vSpots.end()), vSpots.end());
		if(!RateSpots(vMapData, &Collision, TrainMap, Map.Sha256(), std::move(vSpots), NumThreads, Ratings))
			return -1;
	}
	if(!vRoute.empty() && !WriteRoute(pRoutePath, &Collision, TrainMap, vRoute, Ratings))
	{
		log_error(TOOL_NAME, "Error writing '%s'", pRoutePath);
		return -1;
	}
	return 0;
}
