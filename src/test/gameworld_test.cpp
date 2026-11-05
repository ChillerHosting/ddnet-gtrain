#include "test.h"

#include <base/fs.h>
#include <base/hash.h>
#include <base/logger.h>
#include <base/types.h>

#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/map.h>
#include <engine/server/databases/connection.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/register.h>
#include <engine/server/server.h>
#include <engine/server/server_logger.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/masterserver.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/layers.h>
#include <game/mapitems.h>
#include <game/mapitems_ex.h>
#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gamemodes/gtrain.h>
#include <game/server/gamemodes/gtrain_elo.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/switch_layer.h>
#include <game/version.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

bool IsInterrupted()
{
	return false;
}

#if defined(CONF_PLATFORM_ANDROID)
std::vector<std::string> FetchAndroidServerCommandQueue()
{
	return {};
}
#endif

class CGameWorldTestServer : public CServer
{
public:
	struct CClientIdentity
	{
		int m_ClientId;
		bool m_Local;
		std::string m_Name;
	};
	std::optional<CTuningParams> m_aLastTuning[MAX_CLIENTS];
	std::vector<std::string> m_avChatMessages[MAX_CLIENTS];
	std::vector<std::string> m_avBroadcasts[MAX_CLIENTS];
	std::vector<CClientIdentity> m_avClientIdentities[MAX_CLIENTS];

	void AdvanceTicks(int Ticks) { m_CurrentGameTick += Ticks; }

	int SendMsg(CMsgPacker *pMsg, int Flags, int ClientId) override
	{
		if(pMsg->m_NoTranslate && pMsg->m_MsgId == protocol7::NETMSGTYPE_SV_CLIENTINFO && ClientId >= 0 && IsSixup(ClientId))
		{
			CUnpacker Unpacker;
			Unpacker.Reset(pMsg->Data(), pMsg->Size());
			const int Id = Unpacker.GetInt();
			const bool Local = Unpacker.GetInt() != 0;
			Unpacker.GetInt(); // team
			m_avClientIdentities[ClientId].push_back({Id, Local, Unpacker.GetString()});
			EXPECT_FALSE(Unpacker.Error());
		}
		if(pMsg->m_MsgId == NETMSGTYPE_SV_CHAT && ClientId >= 0 && !IsSixup(ClientId))
		{
			CUnpacker Unpacker;
			Unpacker.Reset(pMsg->Data(), pMsg->Size());
			Unpacker.GetInt(); // team
			Unpacker.GetInt(); // sender
			m_avChatMessages[ClientId].emplace_back(Unpacker.GetString());
			EXPECT_FALSE(Unpacker.Error());
		}
		if(pMsg->m_MsgId == NETMSGTYPE_SV_BROADCAST && ClientId >= 0 && !IsSixup(ClientId))
		{
			CUnpacker Unpacker;
			Unpacker.Reset(pMsg->Data(), pMsg->Size());
			m_avBroadcasts[ClientId].emplace_back(Unpacker.GetString());
			EXPECT_FALSE(Unpacker.Error());
		}
		if(pMsg->m_MsgId == NETMSGTYPE_SV_TUNEPARAMS && ClientId >= 0)
		{
			CTuningParams Params;
			CUnpacker Unpacker;
			Unpacker.Reset(pMsg->Data(), pMsg->Size());
			for(int i = 0; i < CTuningParams::Num(); ++i)
			{
				if(i == 30 && IsSixup(ClientId))
					continue;
				Params.NetworkArray()[i] = Unpacker.GetInt();
			}
			EXPECT_FALSE(Unpacker.Error());
			m_aLastTuning[ClientId] = Params;
		}
		return CServer::SendMsg(pMsg, Flags, ClientId);
	}
};

class GameWorld : public ::testing::Test // NOLINT(readability-identifier-naming)
{
public:
	IGameServer *m_pGameServer = nullptr;
	CGameWorldTestServer *m_pServer = nullptr;
	std::unique_ptr<IKernel> m_pKernel;
	CTestInfo m_TestInfo;
	std::unique_ptr<IStorage> m_pStorage;
	CConfig m_ConfigBackup;

	CGameContext *GameServer() // NOLINT(readability-make-member-function-const)
	{
		return (CGameContext *)m_pGameServer;
	}

	GameWorld()
	{
		m_ConfigBackup = g_Config;

		CGameWorldTestServer *pServer = new CGameWorldTestServer();
		m_pServer = pServer;

		m_pKernel = std::unique_ptr<IKernel>(IKernel::Create());
		m_pKernel->RegisterInterface(m_pServer);

		IEngine *pEngine = CreateTestEngine(GAME_NAME);
		m_pKernel->RegisterInterface(pEngine);

		m_TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_TestInfo.CreateTestStorage();
		EXPECT_NE(m_pStorage, nullptr);
		m_pKernel->RegisterInterface(m_pStorage.get(), false);

		IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON).release();
		m_pKernel->RegisterInterface(pConsole);

		IConfigManager *pConfigManager = CreateConfigManager();
		m_pKernel->RegisterInterface(pConfigManager);

		IEngineHttp *pEngineHttp = CreateEngineHttp();
		m_pKernel->RegisterInterface(pEngineHttp); // IEngineHttp
		m_pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);

		IEngineAntibot *pEngineAntibot = CreateEngineAntibot();
		m_pKernel->RegisterInterface(pEngineAntibot);
		m_pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);

		m_pGameServer = CreateGameServer();
		m_pKernel->RegisterInterface(m_pGameServer);

		pEngine->Init();
		pConsole->Init();
		pConfigManager->Init();

		m_pServer->RegisterCommands();

		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		m_pServer->m_RunServer = CServer::RUNNING;

		m_pServer->m_AuthManager.Init();

		{
			int Size = GameServer()->PersistentClientDataSize();
			for(auto &Client : m_pServer->m_aClients)
			{
				Client.m_HasPersistentData = false;
				Client.m_pPersistentData = malloc(Size);
			}
		}
		m_pServer->m_pPersistentData = malloc(GameServer()->PersistentDataSize());
		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		EXPECT_TRUE(pEngineHttp->Init(std::chrono::seconds{2})) << "Failed to initialize the HTTP client";

		pServer->m_NetServer.SetCallbacks(
			CServer::NewClientCallback,
			CServer::NewClientNoAuthCallback,
			CServer::ClientRejoinCallback,
			CServer::DelClientCallback, pServer);

		pServer->m_Econ.Init(pServer->Config(), pServer->Console(), &pServer->m_ServerBan);

		pServer->m_Fifo.Init(pServer->Console(), pServer->Config()->m_SvInputFifo, CFGFLAG_SERVER);
		m_pServer->Antibot()->Init();
		// General world tests exercise DDNet. Training tests select GTrain below.
		str_copy(g_Config.m_SvGametype, "ddnet");
		GameServer()->OnInit(nullptr);
		pServer->ReadAnnouncementsFile();
		pServer->InitMaplist();
		// Rating the spots simulates the map file, which tests change in
		// memory. Tests of the ratings turn them on. (The config manager
		// reset the config to its defaults above.)
		g_Config.m_SvGtrainTileRatings = 0;
	}

	void PrepareTrainingCorridor()
	{
		// Replace the loaded collision tiles with a simple training corridor.
		// Its one-tile height makes shortest-path distance unambiguous.
		CCollision *pCollision = GameServer()->Collision();
		CLayers *pLayers = GameServer()->Layers();
		const int Width = pCollision->GetWidth();
		ASSERT_GE(Width, 10);
		ASSERT_GE(pCollision->GetHeight(), 5);
		if(pLayers->FrontLayer())
			mem_zero(pLayers->Map()->GetData(pLayers->FrontLayer()->m_Front), pLayers->Map()->GetDataSize(pLayers->FrontLayer()->m_Front));
		if(pLayers->TeleLayer())
			mem_zero(pLayers->Map()->GetData(pLayers->TeleLayer()->m_Tele), pLayers->Map()->GetDataSize(pLayers->TeleLayer()->m_Tele));
		for(int y = 0; y < pCollision->GetHeight(); ++y)
			for(int x = 0; x < Width; ++x)
				pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, TILE_SOLID);
		for(int x = 1; x < Width - 1; ++x)
		{
			pCollision->SetCollisionAt(x * 32 + 16, 80, TILE_AIR);
			pCollision->SetCollisionAt(x * 32 + 16, 112, TILE_FREEZE);
		}
		pCollision->SetCollisionAt(48, 80, TILE_START);
		pCollision->SetCollisionAt((Width - 2) * 32 + 16, 80, TILE_FINISH);
		g_Config.m_SvGtrainGoalDistance = 3;
		// Most tests check the zero gravity hover, hover doors have their own.
		g_Config.m_SvGtrainHoverDoor = 0;
		// The corridor is short, skipping its start has its own test.
		g_Config.m_SvGtrainStartExclusion = 0;
		g_Config.m_SvSoloServer = 1;
		g_Config.m_SvNoWeakHook = 1;
		g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;
		GameServer()->GlobalTuning()->Set("player_collision", 0);
		GameServer()->GlobalTuning()->Set("player_hooking", 0);
		for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
		{
			GameServer()->TuningList()[Zone].Set("player_collision", 0);
			GameServer()->TuningList()[Zone].Set("player_hooking", 0);
		}
		delete GameServer()->m_pController;
		GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	}

	vec2 TrainingGoal(int ClientId)
	{
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init(m_pServer->IsSixup(ClientId));
		GameServer()->m_pController->Snap(ClientId);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
			if(pSnap->GetItemType(i) == NETOBJTYPE_FLAG)
			{
				const auto *pFlag = (const CNetObj_Flag *)pSnap->GetItem(i)->Data();
				return vec2(pFlag->m_X, pFlag->m_Y);
			}
		ADD_FAILURE() << "No training flag for client " << ClientId;
		return vec2(0, 0);
	}

	void Say(int ClientId, const char *pText, bool Team = false)
	{
		char aText[512];
		str_copy(aText, pText);
		CNetMsg_Cl_Say Msg = {};
		Msg.m_pMessage = aText;
		Msg.m_Team = Team;
		GameServer()->OnSayNetMessage(&Msg, ClientId, nullptr);
	}

	int TrainingMarkers(int ClientId)
	{
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init(m_pServer->IsSixup(ClientId));
		GameServer()->m_pController->Snap(ClientId);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		int Markers = 0;
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
			if(pSnap->GetItemType(i) == NETOBJTYPE_FLAG || pSnap->GetItemType(i) == NETOBJTYPE_PROJECTILE)
				++Markers;
		return Markers;
	}

	std::vector<int> SpawnTrainingPlayers(std::initializer_list<const char *> Names)
	{
		g_Config.m_DbgDummies = Names.size();
		m_pServer->UpdateDebugDummies(false);
		std::vector<int> vIds;
		int ClientId = m_pServer->MaxClients() - 1;
		for(const char *pName : Names)
		{
			m_pServer->SetClientName(ClientId, pName);
			GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(80, 80));
			vIds.push_back(ClientId--);
		}
		GameServer()->m_pController->Tick();
		return vIds;
	}

	void ClearMapExtras()
	{
		// The test map's speedups, switches, tunes, doors, draggers and turrets would interfere.
		CCollision *pCollision = GameServer()->Collision();
		CLayers *pLayers = GameServer()->Layers();
		for(CMapItemLayerTilemap *pLayer : {pLayers->SpeedupLayer(), pLayers->SwitchLayer(), pLayers->TuneLayer()})
		{
			ASSERT_NE(pLayer, nullptr);
			const int Data = pLayer == pLayers->SpeedupLayer() ? pLayer->m_Speedup : pLayer == pLayers->SwitchLayer() ? pLayer->m_Switch :
																    pLayer->m_Tune;
			mem_zero(pLayers->Map()->GetData(Data), pLayers->Map()->GetDataSize(Data));
		}
		for(int y = 0; y < pCollision->GetHeight(); ++y)
			for(int x = 0; x < pCollision->GetWidth(); ++x)
				pCollision->SetDoorCollisionAt(x * 32 + 16, y * 32 + 16, 0, 0, 0);
		for(CEntity *pEntity = GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_LASER); pEntity; pEntity = pEntity->TypeNext())
			pEntity->Reset();
	}

	// One server tick with this input, applied like CServer::Run does.
	void TickWithInput(int ClientId, const CNetObj_PlayerInput &Input)
	{
		GameServer()->OnClientPredictedEarlyInput(ClientId, &Input);
		m_pServer->AdvanceTicks(1);
		GameServer()->OnClientPredictedInput(ClientId, &Input);
		GameServer()->OnTick();
	}

	// The database runs jobs in order on its own thread, so this waits for
	// all earlier ones. It fails until the database is registered.
	bool WaitForDatabase()
	{
		auto pLast = std::make_shared<CScoreGTrainStatsResult>();
		GameServer()->Score()->LoadGTrainStats("", pLast);
		for(int i = 0; i < 1000 && !pLast->m_Completed; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		EXPECT_TRUE(pLast->m_Completed);
		return pLast->m_Success;
	}

	void UseTestDatabase(const char *pName, char *pPath, int PathSize)
	{
		str_format(pPath, PathSize, "%s/%s", m_TestInfo.m_aFilename, pName);
		// The pool keeps no more of the path.
		ASSERT_LT(str_length(pPath), 64);
		m_pServer->DbPool()->RegisterSqliteDatabase(CDbConnectionPool::READ, pPath);
		m_pServer->DbPool()->RegisterSqliteDatabase(CDbConnectionPool::WRITE, pPath);
		bool Ready = false;
		for(int i = 0; i < 100 && !Ready; ++i)
			Ready = WaitForDatabase();
		ASSERT_TRUE(Ready);
	}

	struct CStoredRun
	{
		std::string m_Map;
		std::string m_MapSha256;
		std::string m_Name;
		int m_Ticks;
		int m_Distance;
		float m_Elo;
		float m_SpeedFactor;
		int64_t m_Points;
		CGTrainRun m_Run;
	};

	std::vector<CStoredRun> StoredRuns(const char *pDatabase)
	{
		EXPECT_TRUE(WaitForDatabase());
		std::vector<CStoredRun> vRuns;
		std::unique_ptr<IDbConnection> pConnection = CreateSqliteConnection(pDatabase, false);
		char aError[256];
		EXPECT_TRUE(pConnection->Connect(aError, sizeof(aError))) << aError;
		EXPECT_TRUE(pConnection->PrepareStatement("SELECT Map, MapSha256, Name, Ticks, Distance, Elo, SpeedFactor, Run, Points FROM record_gtrain_runs ORDER BY rowid", aError, sizeof(aError))) << aError;
		bool End = false;
		while(pConnection->Step(&End, aError, sizeof(aError)) && !End)
		{
			CStoredRun &Stored = vRuns.emplace_back();
			char aBuf[128];
			pConnection->GetString(1, aBuf, sizeof(aBuf));
			Stored.m_Map = aBuf;
			pConnection->GetString(2, aBuf, sizeof(aBuf));
			Stored.m_MapSha256 = aBuf;
			pConnection->GetString(3, aBuf, sizeof(aBuf));
			Stored.m_Name = aBuf;
			Stored.m_Ticks = pConnection->GetInt(4);
			Stored.m_Distance = pConnection->GetInt(5);
			Stored.m_Elo = pConnection->GetFloat(6);
			Stored.m_SpeedFactor = pConnection->GetFloat(7);
			std::vector<unsigned char> vData(1 << 20);
			vData.resize(pConnection->GetBlob(8, vData.data(), vData.size()));
			EXPECT_TRUE(Stored.m_Run.Decode(vData.data(), vData.size()));
			Stored.m_Points = pConnection->GetInt64(9);
		}
		EXPECT_TRUE(End) << aError;
		pConnection->Disconnect();
		return vRuns;
	}

	~GameWorld() override
	{
		m_pServer->m_Econ.Shutdown();
		m_pServer->m_Fifo.Shutdown();
		m_pGameServer->OnShutdown(nullptr);
		m_pServer->DbPool()->OnShutdown();

		g_Config = m_ConfigBackup;
	}
};

TEST_F(GameWorld, DebugDummiesConnectAndDrop)
{
	g_Config.m_DbgDummies = 2;
	m_pServer->UpdateDebugDummies(false);

	const int FirstDummy = m_pServer->MaxClients() - 1;
	const int SecondDummy = m_pServer->MaxClients() - 2;
	EXPECT_TRUE(m_pServer->ClientIngame(FirstDummy));
	EXPECT_TRUE(m_pServer->ClientIngame(SecondDummy));

	g_Config.m_DbgDummies = 1;
	m_pServer->UpdateDebugDummies(false);

	EXPECT_TRUE(m_pServer->ClientIngame(FirstDummy));
	EXPECT_FALSE(m_pServer->ClientIngame(SecondDummy));
}

TEST_F(GameWorld, GTrainSwitchLayerKeepsUuidItems)
{
	// A map with two UUID item types, written in the order that gives them
	// colliding type ids when copied naively.
	ASSERT_TRUE(m_pStorage->CreateFolder("maps", IStorage::TYPE_SAVE));
	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(m_pStorage.get(), "maps/uuid_items.map"));
		CMapItemVersion Version = {};
		Version.m_Version = 1;
		Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(Version), &Version);
		const std::vector<CTile> vTiles(4 * 3, CTile{});
		CMapItemLayerTilemap Game = {};
		Game.m_Layer.m_Type = LAYERTYPE_TILES;
		Game.m_Version = 3;
		Game.m_Width = 4;
		Game.m_Height = 3;
		Game.m_Flags = TILESLAYERFLAG_GAME;
		Game.m_Image = Game.m_ColorEnv = -1;
		Game.m_Tele = Game.m_Speedup = Game.m_Front = Game.m_Switch = Game.m_Tune = -1;
		Game.m_Data = Writer.AddData(vTiles.size() * sizeof(CTile), vTiles.data());
		Writer.AddItem(MAPITEMTYPE_LAYER, 0, sizeof(Game), &Game);
		CMapItemGroup Group = {};
		Group.m_Version = 3;
		Group.m_NumLayers = 1;
		Writer.AddItem(MAPITEMTYPE_GROUP, 0, sizeof(Group), &Group);
		const int aAutomapper[] = {11, 12};
		const int aBezier[] = {21, 22, 23};
		Writer.AddItem(MAPITEMTYPE_AUTOMAPPER_CONFIG, 0, sizeof(aAutomapper), aAutomapper);
		Writer.AddItem(MAPITEMTYPE_ENVPOINTS_BEZIER, 0, sizeof(aBezier), aBezier);
		Writer.Finish();
	}

	char aPath[IO_MAX_PATH_LENGTH];
	ASSERT_TRUE(CGameControllerGTrain::AddSwitchLayer(m_pStorage.get(), "maps/uuid_items.map", aPath, sizeof(aPath)));
	ASSERT_STRNE(aPath, "");
	CDataFileReader Reader;
	ASSERT_TRUE(Reader.Open(m_pStorage.get(), aPath, IStorage::TYPE_ALL));
	const auto ItemData = [&](int Type) {
		int Start, Num;
		Reader.GetType(Type, &Start, &Num);
		std::vector<int> vData;
		if(Num == 1)
		{
			const int *pData = static_cast<const int *>(Reader.GetItem(Start));
			vData.assign(pData, pData + Reader.GetItemSize(Start) / sizeof(int));
		}
		return vData;
	};
	EXPECT_EQ(ItemData(MAPITEMTYPE_AUTOMAPPER_CONFIG), (std::vector<int>{11, 12}));
	EXPECT_EQ(ItemData(MAPITEMTYPE_ENVPOINTS_BEZIER), (std::vector<int>{21, 22, 23}));
	// Every type id maps to one UUID.
	int Start, Num;
	Reader.GetType(ITEMTYPE_EX, &Start, &Num);
	EXPECT_EQ(Num, 2);
	Reader.Close();
	EXPECT_TRUE(m_pStorage->RemoveFile(aPath, IStorage::TYPE_SAVE));
}

TEST_F(GameWorld, GTrainAddsSwitchLayerToServedMaps)
{
	char aPath[IO_MAX_PATH_LENGTH];
	// Maps with a switch layer are served as they are.
	ASSERT_TRUE(CGameControllerGTrain::AddSwitchLayer(m_pStorage.get(), "maps/coverage.map", aPath, sizeof(aPath)));
	EXPECT_STREQ(aPath, "");

	// Others get an empty one at the end of the game group, which the editor
	// requires. The rest stays.
	ASSERT_TRUE(CGameControllerGTrain::AddSwitchLayer(m_pStorage.get(), "maps/ctf1.map", aPath, sizeof(aPath)));
	ASSERT_STRNE(aPath, "");
	CDataFileReader Original, Modified;
	ASSERT_TRUE(Original.Open(m_pStorage.get(), "maps/ctf1.map", IStorage::TYPE_ALL));
	ASSERT_TRUE(Modified.Open(m_pStorage.get(), aPath, IStorage::TYPE_ALL));
	EXPECT_EQ(Modified.NumItems(), Original.NumItems() + 1);
	ASSERT_EQ(Modified.NumData(), Original.NumData() + 2);
	for(int i = 0; i < Original.NumData(); ++i)
	{
		ASSERT_EQ(Modified.GetDataSize(i), Original.GetDataSize(i));
		EXPECT_EQ(mem_comp(Modified.GetData(i), Original.GetData(i), Original.GetDataSize(i)), 0);
	}
	// The layer items of every group, as bytes.
	const auto GroupLayers = [](CDataFileReader &Reader) {
		int GroupsStart, NumGroups, LayersStart, NumLayers;
		Reader.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &NumGroups);
		Reader.GetType(MAPITEMTYPE_LAYER, &LayersStart, &NumLayers);
		std::vector<std::vector<std::vector<char>>> vvvLayers(NumGroups);
		for(int g = 0; g < NumGroups; ++g)
		{
			const auto *pGroup = static_cast<const CMapItemGroup_v1 *>(Reader.GetItem(GroupsStart + g));
			for(int l = pGroup->m_StartLayer; l < pGroup->m_StartLayer + pGroup->m_NumLayers; ++l)
			{
				const char *pLayer = static_cast<const char *>(Reader.GetItem(LayersStart + l));
				vvvLayers[g].emplace_back(pLayer, pLayer + Reader.GetItemSize(LayersStart + l));
			}
		}
		return vvvLayers;
	};
	const auto vvvOriginal = GroupLayers(Original);
	auto vvvModified = GroupLayers(Modified);
	ASSERT_EQ(vvvModified.size(), vvvOriginal.size());
	int NumChanged = 0;
	for(size_t g = 0; g < vvvOriginal.size(); ++g)
	{
		if(vvvModified[g] == vvvOriginal[g])
			continue;
		++NumChanged;
		ASSERT_EQ(vvvModified[g].size(), vvvOriginal[g].size() + 1);
		const auto *pSwitch = reinterpret_cast<const CMapItemLayerTilemap *>(vvvModified[g].back().data());
		EXPECT_EQ(pSwitch->m_Flags, TILESLAYERFLAG_SWITCH);
		vvvModified[g].pop_back();
		EXPECT_EQ(vvvModified[g], vvvOriginal[g]);
		EXPECT_TRUE(std::any_of(vvvOriginal[g].begin(), vvvOriginal[g].end(), [](const std::vector<char> &vLayer) {
			return reinterpret_cast<const CMapItemLayerTilemap *>(vLayer.data())->m_Flags & TILESLAYERFLAG_GAME;
		}));
	}
	EXPECT_EQ(NumChanged, 1);
	Original.Close();
	Modified.Close();

	std::unique_ptr<IMap> pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(m_pStorage.get(), aPath, IStorage::TYPE_ALL));
	CLayers Layers;
	Layers.Init(pMap.get(), false, false);
	ASSERT_NE(Layers.GameLayer(), nullptr);
	ASSERT_NE(Layers.SwitchLayer(), nullptr);
	EXPECT_EQ(Layers.SwitchLayer()->m_Width, Layers.GameLayer()->m_Width);
	EXPECT_EQ(Layers.SwitchLayer()->m_Height, Layers.GameLayer()->m_Height);
	const int NumTiles = Layers.GameLayer()->m_Width * Layers.GameLayer()->m_Height;
	ASSERT_EQ(pMap->GetDataSize(Layers.SwitchLayer()->m_Switch), NumTiles * (int)sizeof(CSwitchTile));
	const auto *pSwitchTiles = static_cast<const CSwitchTile *>(pMap->GetData(Layers.SwitchLayer()->m_Switch));
	for(int i = 0; i < NumTiles; ++i)
		ASSERT_EQ(pSwitchTiles[i].m_Type, 0);
	pMap->Unload();
	EXPECT_TRUE(m_pStorage->RemoveFile(aPath, IStorage::TYPE_SAVE));
}

TEST_F(GameWorld, GTrainMovesSwitchLayersIntoTheGameGroup)
{
	// The editor refuses switch layers in other groups, like the group of
	// their own that switch layers used to be added in.
	ASSERT_TRUE(m_pStorage->CreateFolder("maps", IStorage::TYPE_SAVE));
	const auto WriteMap = [&](const char *pPath, const std::vector<std::vector<int>> &vvFlags, CMapItemAutomapperConfig Config) {
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(m_pStorage.get(), pPath));
		CMapItemVersion Version = {};
		Version.m_Version = 1;
		Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(Version), &Version);
		const std::vector<CTile> vTiles(4 * 3, CTile{});
		const std::vector<CSwitchTile> vSwitchTiles(4 * 3, CSwitchTile{});
		int NumLayers = 0;
		for(size_t g = 0; g < vvFlags.size(); ++g)
		{
			CMapItemGroup Group = {};
			Group.m_Version = 3;
			Group.m_StartLayer = NumLayers;
			Group.m_NumLayers = vvFlags[g].size();
			StrToInts(Group.m_aName, std::size(Group.m_aName), "Group");
			Writer.AddItem(MAPITEMTYPE_GROUP, g, sizeof(Group), &Group);
			for(const int Flags : vvFlags[g])
			{
				CMapItemLayerTilemap Layer = {};
				Layer.m_Layer.m_Type = LAYERTYPE_TILES;
				Layer.m_Version = 3;
				Layer.m_Width = 4;
				Layer.m_Height = 3;
				Layer.m_Flags = Flags;
				Layer.m_Image = Layer.m_ColorEnv = -1;
				Layer.m_Tele = Layer.m_Speedup = Layer.m_Front = Layer.m_Switch = Layer.m_Tune = -1;
				StrToInts(Layer.m_aName, std::size(Layer.m_aName), "Tiles");
				Layer.m_Data = Writer.AddData(vTiles.size() * sizeof(CTile), vTiles.data());
				if(Flags & TILESLAYERFLAG_SWITCH)
					Layer.m_Switch = Writer.AddData(vSwitchTiles.size() * sizeof(CSwitchTile), vSwitchTiles.data());
				Writer.AddItem(MAPITEMTYPE_LAYER, NumLayers++, sizeof(Layer), &Layer);
			}
		}
		Writer.AddItem(MAPITEMTYPE_AUTOMAPPER_CONFIG, 0, sizeof(Config), &Config);
		Writer.Finish();
	};
	const auto ReadMap = [&](const char *pPath, CMapItemAutomapperConfig &Config) {
		std::vector<std::vector<int>> vvFlags;
		CDataFileReader Reader;
		if(!Reader.Open(m_pStorage.get(), pPath, IStorage::TYPE_ALL))
		{
			ADD_FAILURE() << pPath;
			return vvFlags;
		}
		int GroupsStart, NumGroups, LayersStart, NumLayers, ConfigStart, NumConfigs;
		Reader.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &NumGroups);
		Reader.GetType(MAPITEMTYPE_LAYER, &LayersStart, &NumLayers);
		for(int g = 0; g < NumGroups; ++g)
		{
			const auto *pGroup = static_cast<const CMapItemGroup *>(Reader.GetItem(GroupsStart + g));
			vvFlags.emplace_back();
			for(int l = pGroup->m_StartLayer; l < pGroup->m_StartLayer + pGroup->m_NumLayers; ++l)
				vvFlags.back().push_back(static_cast<const CMapItemLayerTilemap *>(Reader.GetItem(LayersStart + l))->m_Flags);
		}
		Reader.GetType(MAPITEMTYPE_AUTOMAPPER_CONFIG, &ConfigStart, &NumConfigs);
		EXPECT_EQ(NumConfigs, 1);
		if(NumConfigs == 1)
			Config = *static_cast<const CMapItemAutomapperConfig *>(Reader.GetItem(ConfigStart));
		return vvFlags;
	};
	const int Game = TILESLAYERFLAG_GAME, Switch = TILESLAYERFLAG_SWITCH;
	struct SCase
	{
		std::vector<std::vector<int>> m_vvBefore;
		std::vector<std::vector<int>> m_vvAfter;
		int m_aConfigBefore[2];
		int m_aConfigAfter[2];
	};
	const SCase aCases[] = {
		// A group of its own at the end goes away.
		{{{Game}, {0}, {Switch}}, {{Game, Switch}, {0}}, {1, 0}, {1, 0}},
		// Layers after it in its group move up, automapper configs follow.
		{{{0}, {Game}, {Switch, 0}}, {{0}, {Game, Switch}, {0}}, {2, 1}, {2, 0}},
		{{{Game, 0}, {Switch}, {0}}, {{Game, 0, Switch}, {}, {0}}, {2, 0}, {2, 0}},
	};
	for(const SCase &Case : aCases)
	{
		WriteMap("maps/switch_group.map", Case.m_vvBefore, {1, Case.m_aConfigBefore[0], Case.m_aConfigBefore[1], 3, 4, 0});
		ASSERT_EQ(AddSwitchLayer(m_pStorage.get(), "maps/switch_group.map", IStorage::TYPE_SAVE, "maps/switch_moved.map", IStorage::TYPE_SAVE), EAddSwitchLayer::MOVED);
		CMapItemAutomapperConfig Config = {};
		EXPECT_EQ(ReadMap("maps/switch_moved.map", Config), Case.m_vvAfter);
		EXPECT_EQ(Config.m_GroupId, Case.m_aConfigAfter[0]);
		EXPECT_EQ(Config.m_LayerId, Case.m_aConfigAfter[1]);
		EXPECT_EQ(Config.m_AutomapperConfig, 3);
		EXPECT_EQ(AddSwitchLayer(m_pStorage.get(), "maps/switch_moved.map", IStorage::TYPE_SAVE, "maps/switch_again.map", IStorage::TYPE_SAVE), EAddSwitchLayer::PRESENT);

		std::unique_ptr<IMap> pMap = CreateMap();
		ASSERT_TRUE(pMap->Load(m_pStorage.get(), "maps/switch_moved.map", IStorage::TYPE_SAVE));
		CLayers Layers;
		Layers.Init(pMap.get(), false, false);
		EXPECT_NE(Layers.SwitchLayer(), nullptr);
		pMap->Unload();
		EXPECT_TRUE(m_pStorage->RemoveFile("maps/switch_moved.map", IStorage::TYPE_SAVE));
	}
	EXPECT_TRUE(m_pStorage->RemoveFile("maps/switch_group.map", IStorage::TYPE_SAVE));
}

TEST_F(GameWorld, GTrainHoverDoorHoldsTheTeeUntilTheRelease)
{
	PrepareTrainingCorridor();
	ClearMapExtras();
	CCollision *pCollision = GameServer()->Collision();
	CLayers *pLayers = GameServer()->Layers();
	g_Config.m_SvGtrainHoverDoor = 1;
	m_pServer->AdvanceTicks(10); // switch end ticks are only sent after tick 0
	const auto vIds = SpawnTrainingPlayers({"Runner", "Observer"});
	const int Runner = vIds[0], Observer = vIds[1];
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);

	const auto Doors = [&](int ClientId) {
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init();
		GameServer()->m_pController->Snap(ClientId);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		std::vector<CNetObj_DDNetLaser> vDoors;
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
			if(pSnap->GetItemType(i) == NETOBJTYPE_DDNETLASER && ((const CNetObj_DDNetLaser *)pSnap->GetItem(i)->Data())->m_Type == LASERTYPE_DOOR)
				vDoors.push_back(*(const CNetObj_DDNetLaser *)pSnap->GetItem(i)->Data());
		return vDoors;
	};
	const auto Release = [&](CCharacter *pTee) {
		// The door holds the tee in place under normal gravity until its release.
		const vec2 Start = pTee->m_Pos;
		const int PlacementTick = m_pServer->Tick();
		for(int i = 0; i < 49; ++i)
		{
			m_pServer->AdvanceTicks(1);
			GameServer()->OnTick();
			EXPECT_EQ(pTee->m_Pos, Start);
		}
		EXPECT_EQ(pTee->m_FreezeTime, 0);
		EXPECT_EQ(m_pServer->Tick(), PlacementTick + 49);
		m_pServer->AdvanceTicks(1);
		GameServer()->OnTick();
		EXPECT_GT(pTee->m_Pos.y, Start.y); // falling freely
	};

	// No tuning changes, so clients predict the hover like everything else.
	EXPECT_FALSE(pChr->m_ZeroGravity);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[Runner]->m_Gravity, 0.5f);
	// Every player gets only their own door, on the tile below their tee.
	for(int ClientId : vIds)
	{
		const auto vDoors = Doors(ClientId);
		ASSERT_EQ(vDoors.size(), 1u);
		EXPECT_EQ(vec2(vDoors[0].m_ToX, vDoors[0].m_ToY), GameServer()->GetPlayerChar(ClientId)->m_Pos + vec2(-12, 32));
		EXPECT_EQ(vDoors[0].m_Owner, -1);
	}
	const int DoorSwitch = Doors(Runner)[0].m_SwitchNumber;
	EXPECT_GT(DoorSwitch, 0);
	// Its switch tells the client when the door opens: on the last frozen tick.
	{
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init();
		GameServer()->SnapSwitchers(Runner);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		const CNetObj_SwitchState *pState = nullptr;
		for(int i = 0; i < pSnap->NumItems(); ++i)
			if(pSnap->GetItemType(i) == NETOBJTYPE_SWITCHSTATE)
				pState = (const CNetObj_SwitchState *)pSnap->GetItem(i)->Data();
		ASSERT_NE(pState, nullptr);
		EXPECT_GE(pState->m_HighestSwitchNumber, DoorSwitch);
		EXPECT_TRUE((pState->m_aStatus[DoorSwitch / 32] >> (DoorSwitch % 32)) & 1);
		EXPECT_EQ(pState->m_aSwitchNumbers[0], DoorSwitch);
		EXPECT_EQ(pState->m_aEndTicks[0], m_pServer->Tick() + 49);
	}
	Release(pChr);
	EXPECT_TRUE(Doors(Runner).empty());
	EXPECT_FALSE(GameServer()->Switchers()[DoorSwitch].m_aStatus[pChr->Team()]);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[Runner]->m_Gravity, 0.5f);

	// The door gives back the double jump, /nodj takes it again.
	m_pServer->Console()->ExecuteLineFlag("nodj", CFGFLAG_CHAT, Runner);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	Release(pChr);
	EXPECT_EQ(pChr->Core()->m_Jumped & 2, 2);

	// Hovers ending early, like on an unfreeze tile, open the door at once.
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	pChr = GameServer()->GetPlayerChar(Runner);
	ASSERT_EQ(Doors(Runner).size(), 1u);
	pChr->Unfreeze();
	m_pServer->AdvanceTicks(1);
	GameServer()->OnTick();
	EXPECT_FALSE(GameServer()->Switchers()[DoorSwitch].m_aStatus[pChr->Team()]);
	EXPECT_TRUE(Doors(Runner).empty());
	const float EarlyY = pChr->m_Pos.y;
	m_pServer->AdvanceTicks(1);
	GameServer()->OnTick();
	EXPECT_GT(pChr->m_Pos.y, EarlyY); // falling freely

	// Without room for a door the zero gravity hover remains.
	auto *pFront = static_cast<CTile *>(pLayers->Map()->GetData(pLayers->FrontLayer()->m_Front));
	pFront[pCollision->GetPureMapIndex(pChr->m_Pos) + pCollision->GetWidth()].m_Index = TILE_DEATH;
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	EXPECT_TRUE(pChr->m_ZeroGravity);
	EXPECT_TRUE(Doors(Runner).empty());
}

TEST_F(GameWorld, GTrainPersonalGoalTrailAndCollection)
{
	PrepareTrainingCorridor();
	g_Config.m_SvGtrainPathParticleSpacing = 8;
	g_Config.m_DbgDummies = 2;
	m_pServer->UpdateDebugDummies(false);
	const int FirstId = m_pServer->MaxClients() - 1;
	const int SecondId = FirstId - 1;
	CCharacter *pFirst = GameServer()->m_apPlayers[FirstId]->ForceSpawn(vec2(80, 80));
	CCharacter *pSecond = GameServer()->m_apPlayers[SecondId]->ForceSpawn(vec2(80, 80));
	GameServer()->m_pController->Tick();
	const CEntity *pMapProjectile = GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
	pFirst->SetVelocity(vec2(1, 0));
	pSecond->SetVelocity(vec2(1, 0));

	const auto Snapshot = [&](int ClientId, CSnapshotBuffer &Buffer) {
		GameServer()->m_apPlayers[ClientId]->m_ViewPos = GameServer()->GetPlayerChar(ClientId)->m_Pos;
		m_pServer->m_SnapshotBuilder.Init(m_pServer->IsSixup(ClientId));
		GameServer()->m_pController->Snap(ClientId);
		EXPECT_GT(m_pServer->m_SnapshotBuilder.Finish(&Buffer), 0);
	};
	const auto FindGoal = [&](const CSnapshotBuffer &Buffer, vec2 &Goal) {
		int Flags = 0;
		int Lasers = 0;
		int Particles = 0;
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
		{
			if(pSnap->GetItemType(i) == NETOBJTYPE_FLAG)
			{
				const auto *pFlag = (const CNetObj_Flag *)pSnap->GetItem(i)->Data();
				EXPECT_EQ(pFlag->m_Team, TEAM_BLUE);
				Goal = vec2(pFlag->m_X, pFlag->m_Y);
				++Flags;
			}
			if(pSnap->GetItemType(i) == NETOBJTYPE_DDNETLASER)
				++Lasers;
			if(pSnap->GetItemType(i) == NETOBJTYPE_PROJECTILE)
			{
				const auto *pParticle = (const CNetObj_Projectile *)pSnap->GetItem(i)->Data();
				EXPECT_EQ(pParticle->m_Type, WEAPON_HAMMER); // trail particles without a bullet sprite
				EXPECT_EQ(pParticle->m_VelX, 0);
				EXPECT_EQ(pParticle->m_VelY, 0);
				++Particles;
			}
		}
		EXPECT_EQ(Flags, 1); // each snapshot contains only that player's goal
		EXPECT_EQ(Lasers, 0);
		EXPECT_EQ(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE), pMapProjectile);
		return Particles;
	};
	CSnapshotBuffer Buffer;
	vec2 FirstGoal;
	Snapshot(FirstId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, FirstGoal), 5);
	EXPECT_FLOAT_EQ(distance(pFirst->m_Pos, FirstGoal), 3 * 32.0f);
	for(int i = 0; i < Buffer.AsSnapshot()->NumItems(); ++i)
		if(Buffer.AsSnapshot()->GetItemType(i) == NETOBJTYPE_PROJECTILE)
		{
			const auto *pParticle = (const CNetObj_Projectile *)Buffer.AsSnapshot()->GetItem(i)->Data();
			const vec2 Offset = vec2(pParticle->m_X, pParticle->m_Y) - pFirst->m_Pos;
			EXPECT_GE(length(Offset), 64.0f);
			EXPECT_LE(length(Offset), 96.0f);
			EXPECT_GT(dot(Offset, FirstGoal - pFirst->m_Pos), 0);
		}
	vec2 SecondGoal;
	Snapshot(SecondId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, SecondGoal), 5);
	EXPECT_FLOAT_EQ(distance(pSecond->m_Pos, SecondGoal), 3 * 32.0f);
	// The radius is applied on each snapshot without rebuilding the route.
	g_Config.m_SvGtrainPathClearRadius = 128;
	Snapshot(FirstId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, FirstGoal), 0);
	g_Config.m_SvGtrainPathClearRadius = 32;
	Snapshot(FirstId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, FirstGoal), 9);
	g_Config.m_SvGtrainPathClearRadius = 64;
	pFirst->SetVelocity(vec2(0, 0));
	Snapshot(FirstId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, FirstGoal), 5); // stationary: still a 64-pixel clear radius
	pFirst->SetVelocity(vec2(0, 2));
	Snapshot(FirstId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, FirstGoal), 5); // faster movement does not change the radius
	pFirst->SetVelocity(vec2(1, 0));

	m_pServer->AdvanceTicks(50); // the initial hover must not count as finish time
	pFirst->Unfreeze();
	GameServer()->m_pController->Tick();
	EXPECT_FALSE(pFirst->m_ZeroGravity);
	Snapshot(FirstId, Buffer);
	EXPECT_EQ(FindGoal(Buffer, FirstGoal), 5); // trail continues throughout the attempt
	const vec2 Direction = normalize(FirstGoal - pFirst->m_Pos);
	m_pServer->AdvanceTicks(3 * m_pServer->TickSpeed());
	pFirst->ResetVelocity();
	// Collection radius is 42 pixels: stationary tees just outside stay active.
	const vec2 OutsideCapture = FirstGoal - Direction * 43.0f;
	pFirst->SetPosition(OutsideCapture);
	pFirst->m_Pos = pFirst->m_PrevPos = OutsideCapture;
	GameServer()->m_pController->Tick();
	EXPECT_FALSE(pFirst->m_ZeroGravity);
	EXPECT_EQ(TrainingGoal(FirstId), FirstGoal);
	// Just inside the enlarged radius collects without touching the flag center.
	const vec2 InsideCapture = FirstGoal - Direction * 41.0f;
	pFirst->SetPosition(InsideCapture);
	pFirst->m_Pos = pFirst->m_PrevPos = InsideCapture;
	m_pServer->m_aClients[FirstId].m_Sixup = true;
	GameServer()->m_Events.Clear();
	CMemoryLogger CaptureLogger;
	{
		CLogScope Scope(&CaptureLogger);
		GameServer()->m_pController->Tick();
	}
	EXPECT_NE(CaptureLogger.ConcatenatedLines().find("captured the flag in 3.00 seconds (0 fails)."), std::string::npos);
	EXPECT_EQ(CaptureLogger.ConcatenatedLines().find("score"), std::string::npos);
	EXPECT_TRUE(pFirst->m_ZeroGravity);
	EXPECT_EQ(pFirst->m_FreezeTime, 50);
	Snapshot(FirstId, Buffer);
	vec2 NewGoal;
	EXPECT_EQ(FindGoal(Buffer, NewGoal), 5); // frozen attempts keep the same clear radius
	EXPECT_FLOAT_EQ(distance(pFirst->m_Pos, NewGoal), 3 * 32.0f);
	Snapshot(SecondId, Buffer);
	vec2 UnchangedGoal;
	FindGoal(Buffer, UnchangedGoal);
	EXPECT_EQ(SecondGoal, UnchangedGoal);

	// Sixup has no global sound message: check its private capture sound event.
	GameServer()->m_apPlayers[FirstId]->m_ViewPos = pFirst->m_Pos;
	m_pServer->m_SnapshotBuilder.Init(true);
	GameServer()->m_Events.Snap(FirstId);
	EXPECT_GT(m_pServer->m_SnapshotBuilder.Finish(&Buffer), 0);
	const auto CaptureSounds = [&](int Type) {
		int Count = 0;
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
			if(pSnap->GetItemType(i) == Type)
			{
				const auto *pSound = (const CNetEvent_SoundWorld *)pSnap->GetItem(i)->Data();
				if(pSound->m_SoundId == SOUND_CTF_CAPTURE)
					++Count;
			}
		return Count;
	};
	EXPECT_EQ(CaptureSounds(protocol7::NETEVENTTYPE_SOUNDWORLD), 1);
	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->m_Events.Snap(SecondId);
	m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	EXPECT_EQ(CaptureSounds(NETEVENTTYPE_SOUNDWORLD), 0);

	// The new attempt must restart the timer.
	pFirst->Unfreeze();
	GameServer()->m_pController->Tick();
	m_pServer->AdvanceTicks(m_pServer->TickSpeed());
	pFirst->SetPosition(NewGoal);
	pFirst->m_Pos = NewGoal;
	pFirst->m_PrevPos = NewGoal;
	pFirst->ResetVelocity();
	CMemoryLogger NextCaptureLogger;
	{
		CLogScope Scope(&NextCaptureLogger);
		GameServer()->m_pController->Tick();
	}
	EXPECT_NE(NextCaptureLogger.ConcatenatedLines().find("captured the flag in 1.00 seconds (0 fails)."), std::string::npos);
}

TEST_F(GameWorld, GTrainTeamAndPracticeCommandsPreserveFight)
{
	PrepareTrainingCorridor();
	g_Config.m_SvPractice = 1;
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob"});
	const int Anna = vIds[0], Bob = vIds[1];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	m_pServer->Console()->ExecuteLineFlag("fight Anna", CFGFLAG_CHAT, Bob);
	pController->Tick();
	const int FightTeam = pController->Teams().ScoreboardTeam(Anna);
	ASSERT_GT(FightTeam, 0);
	const vec2 Position = GameServer()->GetPlayerChar(Anna)->m_Pos;
	const vec2 Goal = TrainingGoal(Anna);
	for(int ClientId : vIds)
	{
		const int PhysicalTeam = GameServer()->GetDDRaceTeam(ClientId);
		m_pServer->m_avChatMessages[ClientId].clear();
		// /team asks players by name, fights can't. Practice is an unknown
		// command in GTrain, chat replies to it.
		const std::string Other = m_pServer->ClientName(ClientId == Anna ? Bob : Anna);
		for(const std::string &Command : {std::string("/team"), std::string("/team 1"), "/team " + Other, std::string("/practice"), std::string("/practice 1"), std::string("/practice 0")})
			Say(ClientId, Command.c_str());
		const auto &vMessages = m_pServer->m_avChatMessages[ClientId];
		ASSERT_EQ(vMessages.size(), 6);
		EXPECT_EQ(vMessages[0], "You are not in a team. /team <player> asks a player to team up.");
		EXPECT_EQ(vMessages[1], "Player not found. Use their full name.");
		EXPECT_EQ(vMessages[2], "You are in a fight, which can't team up. /fight alone leaves it.");
		EXPECT_EQ(vMessages[3], "No such command: practice. Use /cmdlist for a list of all commands.");
		pController->Tick();
		ASSERT_NE(GameServer()->GetPlayerChar(ClientId), nullptr);
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, Position);
		EXPECT_EQ(TrainingGoal(ClientId), Goal);
		EXPECT_EQ(GameServer()->GetDDRaceTeam(ClientId), PhysicalTeam);
		EXPECT_FALSE(pController->Teams().IsPractice(PhysicalTeam));
		EXPECT_FALSE(GameServer()->m_apPlayers[ClientId]->m_VotedForPractice);
		EXPECT_EQ(pController->Teams().ScoreboardTeam(ClientId), FightTeam);
	}
	EXPECT_TRUE(GameServer()->m_apPlayers[Anna]->IsNameMarked());
	m_pServer->Console()->ExecuteLineFlag("fight", CFGFLAG_CHAT, Bob);
	pController->Tick();
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Bob), 0);
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Anna), 0);
}

TEST_F(GameWorld, GTrainParticlePathRoundsWallsAndLeavesPlayerClear)
{
	PrepareTrainingCorridor();
	g_Config.m_SvGtrainPathParticleSpacing = 8;
	CCollision *pCollision = GameServer()->Collision();
	ASSERT_GE(pCollision->GetWidth(), 17);
	ASSERT_GE(pCollision->GetHeight(), 9);
	for(int y = 0; y < pCollision->GetHeight(); ++y)
		for(int x = 0; x < pCollision->GetWidth(); ++x)
			pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, TILE_SOLID);
	for(int x = 2; x <= 8; ++x)
	{
		pCollision->SetCollisionAt(x * 32 + 16, 80, TILE_AIR);
		pCollision->SetCollisionAt(x * 32 + 16, 112, TILE_FREEZE);
	}
	for(int y = 2; y <= 6; ++y)
		pCollision->SetCollisionAt(272, y * 32 + 16, TILE_AIR);
	for(int x = 8; x <= 14; ++x)
	{
		pCollision->SetCollisionAt(x * 32 + 16, 208, TILE_AIR);
		pCollision->SetCollisionAt(x * 32 + 16, 240, TILE_FREEZE);
	}
	pCollision->SetCollisionAt(48, 80, TILE_START);
	pCollision->SetCollisionAt(496, 208, TILE_FINISH);
	g_Config.m_SvGtrainGoalDistance = 100;
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	const int ClientId = SpawnTrainingPlayers({"Runner"})[0];
	CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
	const vec2 Goal = TrainingGoal(ClientId);
	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	// Move to the opposite end, forcing an A* route when needed.
	const vec2 Position = Goal.y == 80 ? vec2(464, 208) : vec2(80, 80);
	pChr->m_Pos = pChr->m_PrevPos = Position;
	pChr->SetPosition(Position);
	pChr->SetVelocity(vec2(1, 0));
	GameServer()->m_pController->Tick();
	GameServer()->m_apPlayers[ClientId]->m_ShowAll = true;
	CSnapshotBuffer Buffer;
	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->m_pController->Snap(ClientId);
	m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	const CSnapshot *pSnap = Buffer.AsSnapshot();
	int Particles = 0;
	bool Top = false, Bottom = false, RoundedTurn = false, AtGoal = false;
	for(int i = 0; i < pSnap->NumItems(); ++i)
	{
		if(pSnap->GetItemType(i) != NETOBJTYPE_PROJECTILE)
			continue;
		const auto *pParticle = (const CNetObj_Projectile *)pSnap->GetItem(i)->Data();
		const vec2 Pos(pParticle->m_X, pParticle->m_Y);
		EXPECT_GE(distance(Pos, Position), 64.0f);
		const int Tile = pCollision->GetTileIndex(pCollision->GetPureMapIndex(Pos));
		EXPECT_EQ(Tile, TILE_AIR); // no particles inside walls or freeze
		Top |= Pos.y == 80;
		Bottom |= Pos.y == 208;
		RoundedTurn |= Pos.y != 80 && Pos.y != 208 && Pos.x != 272;
		AtGoal |= Pos == Goal;
		++Particles;
	}
	EXPECT_GT(Particles, 20);
	EXPECT_LE(Particles, 256);
	EXPECT_TRUE(Top);
	EXPECT_TRUE(Bottom);
	EXPECT_TRUE(RoundedTurn);
	EXPECT_TRUE(AtGoal);
}

TEST_F(GameWorld, GTrainParticlePathUsesDiagonalsInOpenSpace)
{
	PrepareTrainingCorridor();
	g_Config.m_SvGtrainPathParticleSpacing = 8;
	CCollision *pCollision = GameServer()->Collision();
	ASSERT_GE(pCollision->GetHeight(), 8);
	for(int y = 0; y < pCollision->GetHeight(); ++y)
		for(int x = 0; x < pCollision->GetWidth(); ++x)
			pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, TILE_SOLID);
	for(int y = 2; y <= 5; ++y)
		for(int x = 2; x <= 5; ++x)
			pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, TILE_AIR);
	for(int x = 2; x <= 5; ++x)
		pCollision->SetCollisionAt(x * 32 + 16, 208, TILE_FREEZE);
	pCollision->SetCollisionAt(48, 80, TILE_START);
	pCollision->SetCollisionAt(208, 176, TILE_FINISH);
	g_Config.m_SvGtrainPathClearRadius = 0;
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	const int ClientId = SpawnTrainingPlayers({"Runner"})[0];
	CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
	const vec2 Goal = TrainingGoal(ClientId);
	const vec2 InitialOffset = Goal - pChr->m_Pos;
	// Interior spawns fall back to two steps: all corners are closer than
	// the configured three-step distance on this small map.
	const float Farthest = std::max(std::max(pChr->m_Pos.x - 80, 176 - pChr->m_Pos.x), std::max(pChr->m_Pos.y - 80, 176 - pChr->m_Pos.y));
	EXPECT_FLOAT_EQ(std::max(std::abs(InitialOffset.x), std::abs(InitialOffset.y)), Farthest);
	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	const vec2 Position(Goal.x < 128 ? 176 : 80, Goal.y < 128 ? 176 : 80);
	pChr->m_Pos = pChr->m_PrevPos = Position;
	pChr->SetPosition(Position);
	GameServer()->m_pController->Tick();
	GameServer()->m_apPlayers[ClientId]->m_ShowAll = true;
	CSnapshotBuffer Buffer;
	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->m_pController->Snap(ClientId);
	m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	const CSnapshot *pSnap = Buffer.AsSnapshot();
	vec2 Previous = Position;
	float PathLength = 0;
	int Particles = 0;
	for(int i = 0; i < pSnap->NumItems(); ++i)
	{
		if(pSnap->GetItemType(i) != NETOBJTYPE_PROJECTILE)
			continue;
		const auto *pParticle = (const CNetObj_Projectile *)pSnap->GetItem(i)->Data();
		const vec2 Pos(pParticle->m_X, pParticle->m_Y);
		EXPECT_EQ(pCollision->GetTileIndex(pCollision->GetPureMapIndex(Pos)), TILE_AIR);
		PathLength += distance(Previous, Pos);
		Previous = Pos;
		++Particles;
	}
	EXPECT_GT(Particles, 0);
	EXPECT_EQ(Previous, Goal);
	const vec2 Offset = Goal - Position;
	EXPECT_LT(PathLength, std::abs(Offset.x) + std::abs(Offset.y) - 10.0f);
}

TEST_F(GameWorld, GTrainParticleSpacingAccumulatesAcrossTiles)
{
	PrepareTrainingCorridor();
	g_Config.m_SvGtrainGoalDistance = 12;
	g_Config.m_SvGtrainPathParticleSpacing = 96;
	const int ClientId = SpawnTrainingPlayers({"Runner"})[0];
	const vec2 Start = GameServer()->GetPlayerChar(ClientId)->m_Pos;
	const vec2 Goal = TrainingGoal(ClientId);
	ASSERT_FLOAT_EQ(distance(Start, Goal), 12 * 32.0f);
	GameServer()->m_apPlayers[ClientId]->m_ShowAll = true;
	const auto Particles = [&] {
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init();
		GameServer()->m_pController->Snap(ClientId);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		std::vector<vec2> vPositions;
		for(int i = 0; i < pSnap->NumItems(); ++i)
		{
			if(pSnap->GetItemType(i) != NETOBJTYPE_PROJECTILE)
				continue;
			const auto *pParticle = (const CNetObj_Projectile *)pSnap->GetItem(i)->Data();
			const vec2 Pos(pParticle->m_X, pParticle->m_Y);
			EXPECT_GE(distance(Start, Pos), 64.0f);
			vPositions.push_back(Pos);
		}
		return vPositions;
	};
	const auto vSparse = Particles();
	ASSERT_EQ(vSparse.size(), 4);
	for(size_t i = 0; i < vSparse.size(); ++i)
		EXPECT_FLOAT_EQ(distance(Start, vSparse[i]), (i + 1) * 96.0f);
	EXPECT_EQ(vSparse.back(), Goal);

	// Spacing changes immediately and does not select a new goal.
	g_Config.m_SvGtrainPathParticleSpacing = 32;
	const auto vDense = Particles();
	ASSERT_EQ(vDense.size(), 11);
	for(size_t i = 1; i < vDense.size(); ++i)
		EXPECT_FLOAT_EQ(distance(vDense[i - 1], vDense[i]), 32.0f);
	EXPECT_EQ(TrainingGoal(ClientId), Goal);

	// Non-divisible spacing adds just the endpoint after the regular samples.
	g_Config.m_SvGtrainPathParticleSpacing = 160;
	const auto vUneven = Particles();
	ASSERT_EQ(vUneven.size(), 3);
	EXPECT_FLOAT_EQ(distance(Start, vUneven[0]), 160.0f);
	EXPECT_FLOAT_EQ(distance(Start, vUneven[1]), 320.0f);
	EXPECT_EQ(vUneven.back(), Goal);
	g_Config.m_SvGtrainPathParticleSpacing = 4096;
	EXPECT_EQ(Particles(), (std::vector<vec2>{Goal}));
}

TEST_F(GameWorld, GTrainTrailIsEachViewersChoice)
{
	PrepareTrainingCorridor();
	g_Config.m_SvGtrainGoalDistance = 12;
	const auto vIds = SpawnTrainingPlayers({"Runner", "Viewer"});
	const int Runner = vIds[0], Viewer = vIds[1];
	for(const int ClientId : vIds)
		GameServer()->m_apPlayers[ClientId]->m_ShowAll = true;
	const auto Command = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};

	// Hiding the trail keeps the flag.
	EXPECT_GT(TrainingMarkers(Runner), 1);
	EXPECT_EQ(Command(Runner, "trail"), "The trail to the flag is hidden.");
	EXPECT_EQ(TrainingMarkers(Runner), 1);
	EXPECT_GT(TrainingMarkers(Viewer), 1);

	// The viewer's choice also applies while watching someone else.
	GameServer()->m_apPlayers[Viewer]->SetTeam(TEAM_SPECTATORS, false);
	GameServer()->m_apPlayers[Viewer]->SetSpectatorId(Runner);
	EXPECT_EQ(TrainingGoal(Viewer), TrainingGoal(Runner));
	EXPECT_GT(TrainingMarkers(Viewer), 1);
	EXPECT_EQ(Command(Viewer, "trail"), "The trail to the flag is hidden.");
	EXPECT_EQ(TrainingMarkers(Viewer), 1);

	EXPECT_EQ(Command(Runner, "trail"), "The trail to the flag is shown again.");
	EXPECT_GT(TrainingMarkers(Runner), 1);
}

TEST_F(GameWorld, GTrainFreeplayHidesGoalsAndDisablesCaptures)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Runner", "Observer"});
	const int Runner = vIds[0], Observer = vIds[1];
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
	const vec2 Start = pChr->m_Pos;
	const vec2 Goal = TrainingGoal(Runner);
	const vec2 ObserverGoal = TrainingGoal(Observer);
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Runner);
	EXPECT_EQ(pChr->m_Pos, Start);
	EXPECT_EQ(TrainingMarkers(Runner), 0);
	EXPECT_EQ(TrainingGoal(Observer), ObserverGoal);
	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	m_pServer->AdvanceTicks(2 * m_pServer->TickSpeed());
	pChr->m_Pos = pChr->m_PrevPos = Goal;
	pChr->SetPosition(Goal);
	CMemoryLogger CaptureLogger;
	{
		CLogScope Scope(&CaptureLogger);
		GameServer()->m_pController->Tick();
	}
	EXPECT_EQ(pChr->m_Pos, Goal);
	EXPECT_FALSE(pChr->m_ZeroGravity);
	EXPECT_EQ(CaptureLogger.ConcatenatedLines().find("captured the flag"), std::string::npos);
	EXPECT_EQ(TrainingMarkers(Runner), 0);

	// Retrying and dying keep freeplay active, with no new goal to capture.
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Runner);
	EXPECT_EQ(pChr->m_Pos, Start);
	EXPECT_EQ(TrainingMarkers(Runner), 0);
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
	GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
	GameServer()->m_pController->Tick();
	EXPECT_EQ(TrainingMarkers(Runner), 0);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	EXPECT_EQ(TrainingMarkers(Runner), 0);

	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Runner);
	pChr = GameServer()->GetPlayerChar(Runner);
	EXPECT_GT(TrainingMarkers(Runner), 0);
	EXPECT_TRUE(pChr->m_ZeroGravity);
	EXPECT_EQ(pChr->m_FreezeTime, 50);
	EXPECT_FLOAT_EQ(distance(pChr->m_Pos, TrainingGoal(Runner)), 3 * 32.0f);
	EXPECT_EQ(TrainingGoal(Observer), ObserverGoal);
}

TEST_F(GameWorld, GTrainFreeplayFollowsFightLeaderAndMembership)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson", "Observer"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2], Observer = vIds[3];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	pController->Fight(Bob, "Anna");
	pController->Tick();
	m_pServer->m_avChatMessages[Bob].clear();
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Bob);
	ASSERT_FALSE(m_pServer->m_avChatMessages[Bob].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "Only the fight leader can toggle freeplay.");
	EXPECT_GT(TrainingMarkers(Anna), 0);
	EXPECT_GT(TrainingMarkers(Bob), 0);
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Anna);
	EXPECT_EQ(TrainingMarkers(Anna), 0);
	EXPECT_EQ(TrainingMarkers(Bob), 0);
	EXPECT_GT(TrainingMarkers(Observer), 0);

	// Chained joins adopt the target group's mode; group resets keep it.
	pController->Fight(Wilson, "Bob");
	pController->Tick();
	GameServer()->m_apPlayers[Anna]->KillCharacter(WEAPON_SELF);
	pController->Tick();
	for(int ClientId : {Anna, Bob, Wilson})
	{
		ASSERT_NE(GameServer()->GetPlayerChar(ClientId), nullptr);
		EXPECT_EQ(TrainingMarkers(ClientId), 0);
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, GameServer()->GetPlayerChar(Anna)->m_Pos);
	}
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Bob);
	EXPECT_EQ(TrainingMarkers(Anna), 0);

	// Promotion carries freeplay to the new leader; the leaver stays in
	// freeplay individually and cannot change the remaining group's mode.
	pController->Fight(Anna, "");
	pController->Tick();
	const int Leader = GameServer()->m_apPlayers[Bob]->IsNameMarked() ? Bob : Wilson;
	const int Member = Leader == Bob ? Wilson : Bob;
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Anna);
	EXPECT_GT(TrainingMarkers(Anna), 0);
	EXPECT_EQ(TrainingMarkers(Leader), 0);
	EXPECT_EQ(TrainingMarkers(Member), 0);
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Leader);
	EXPECT_GT(TrainingMarkers(Leader), 0);
	EXPECT_EQ(TrainingGoal(Leader), TrainingGoal(Member));
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Member);
	EXPECT_GT(TrainingMarkers(Member), 0);

	// Disconnecting the leader also preserves the mode for the survivor.
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Leader);
	CServer::DelClientCallback(Leader, "test", m_pServer);
	EXPECT_EQ(TrainingMarkers(Member), 0);
	m_pServer->Console()->ExecuteLineFlag("freeplay", CFGFLAG_CHAT, Member);
	EXPECT_GT(TrainingMarkers(Member), 0);
	EXPECT_GT(TrainingMarkers(Observer), 0);
}

TEST_F(GameWorld, GTrainFreeplayTeleCursorChoosesTheStart)
{
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob"});
	const int Anna = vIds[0], Bob = vIds[1];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Tile = [](int x, int y) { return vec2(x * 32 + 16, y * 32 + 16); };
	const auto AimAndSay = [&](int ClientId, vec2 Pos, const char *pCommand) {
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		CCharacterCore Core = pChr->GetCore();
		Core.m_Input.m_TargetX = round_to_int(Pos.x - pChr->m_Pos.x);
		Core.m_Input.m_TargetY = round_to_int(Pos.y - pChr->m_Pos.y);
		pChr->SetCore(Core);
		m_pServer->m_avChatMessages[ClientId].clear();
		Say(ClientId, pCommand);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	const auto Pos = [&](int ClientId) { return GameServer()->GetPlayerChar(ClientId)->m_Pos; };

	// Outside freeplay, it retries like the other practice binds.
	const vec2 Start = Pos(Anna);
	AimAndSay(Anna, Tile(3, 2), "/tc");
	EXPECT_EQ(Pos(Anna), Start);

	Say(Anna, "/freeplay");
	for(const char *pCommand : {"/tc", "/telecursor"})
	{
		const vec2 Chosen = Tile(str_comp(pCommand, "/tc") == 0 ? 3 : 4, 2);
		EXPECT_EQ(AimAndSay(Anna, Chosen, pCommand), "") << pCommand;
		CCharacter *pChr = GameServer()->GetPlayerChar(Anna);
		EXPECT_EQ(pChr->m_Pos, Chosen) << pCommand;
		EXPECT_EQ(pChr->m_FreezeTime, 50) << pCommand;
		EXPECT_TRUE(pChr->m_ZeroGravity) << pCommand;
		EXPECT_EQ(TrainingMarkers(Anna), 0) << pCommand;
		// Retries return to the chosen start.
		pChr->Unfreeze();
		pController->Tick();
		pChr->SetPosition(Chosen + vec2(64, 0));
		pChr->m_Pos = Chosen + vec2(64, 0);
		Say(Anna, "/r");
		EXPECT_EQ(Pos(Anna), Chosen) << pCommand;
	}

	// Walls, freeze and the race lines are no starts.
	const int Width = GameServer()->Collision()->GetWidth();
	for(vec2 Invalid : {Tile(3, 1), Tile(3, 3), Tile(1, 2), Tile(Width - 2, 2), vec2(-100, -100)})
	{
		EXPECT_EQ(AimAndSay(Anna, Invalid, "/tc"), "Aim at a free tile between the start and the finish line to start there.");
		EXPECT_EQ(Pos(Anna), Tile(4, 2));
	}

	// In a fight, the leader chooses the start of everyone.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(AimAndSay(Bob, Tile(5, 2), "/tc"), "Only the fight leader can choose the start position.");
	AimAndSay(Anna, Tile(6, 2), "/tc");
	EXPECT_EQ(Pos(Anna), Tile(6, 2));
	EXPECT_EQ(Pos(Bob), Tile(6, 2));
	GameServer()->m_apPlayers[Bob]->KillCharacter(WEAPON_SELF);
	GameServer()->m_apPlayers[Bob]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	EXPECT_EQ(Pos(Bob), Tile(6, 2));

	// In /spec or /pause, it is the middle of the view instead, which can be
	// anywhere on the map, and the leader plays again.
	CPlayer *pAnna = GameServer()->m_apPlayers[Anna];
	const auto SpecAndSay = [&](int PauseType, vec2 View) {
		pAnna->Pause(PauseType, true);
		if(PauseType == CPlayer::PAUSE_SPEC)
			GameServer()->GetPlayerChar(Anna)->Pause(true);
		pAnna->m_ViewPos = View;
		return AimAndSay(Anna, Tile(3, 2), "/tc");
	};
	const vec2 Far = Tile(Width - 3, 2);
	EXPECT_EQ(SpecAndSay(CPlayer::PAUSE_SPEC, Far), "");
	EXPECT_EQ(pAnna->IsPaused(), 0);
	EXPECT_FALSE(GameServer()->GetPlayerChar(Anna)->IsPaused());
	EXPECT_EQ(Pos(Anna), Far);
	EXPECT_EQ(Pos(Bob), Far);
	EXPECT_EQ(SpecAndSay(CPlayer::PAUSE_PAUSED, Tile(5, 2)), "");
	EXPECT_EQ(pAnna->IsPaused(), 0);
	EXPECT_EQ(Pos(Anna), Tile(5, 2));
	EXPECT_EQ(Pos(Bob), Tile(5, 2));
	// Views off the path keep the pause.
	EXPECT_EQ(SpecAndSay(CPlayer::PAUSE_SPEC, Tile(3, 1)), "Move the middle of your view to a free tile between the start and the finish line to start there.");
	EXPECT_EQ(pAnna->IsPaused(), -CPlayer::PAUSE_SPEC);
	EXPECT_EQ(Pos(Anna), Tile(5, 2));
	pAnna->Pause(CPlayer::PAUSE_NONE, true);

	// Spectators have no tee to start.
	GameServer()->m_apPlayers[Bob]->SetTeam(TEAM_SPECTATORS, false);
	pController->Tick();
	EXPECT_EQ(AimAndSay(Anna, Tile(3, 2), "/tc"), "");
	m_pServer->m_avChatMessages[Bob].clear();
	Say(Bob, "/tc");
	ASSERT_FALSE(m_pServer->m_avChatMessages[Bob].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "You must be playing to choose a start position.");

	// Outside freeplay, /tc in /spec can't retry.
	Say(Anna, "/freeplay");
	const vec2 Spot = Pos(Anna);
	EXPECT_EQ(SpecAndSay(CPlayer::PAUSE_SPEC, Tile(6, 2)), "You must be playing and unpaused to retry. In /freeplay, /tc in /spec starts at the middle of your view.");
	EXPECT_EQ(Pos(Anna), Spot);
	pAnna->Pause(CPlayer::PAUSE_NONE, true);

	// Admin force pauses can't be left with it.
	Say(Anna, "/freeplay");
	pAnna->ForcePause(10);
	pAnna->m_ViewPos = Tile(6, 2);
	EXPECT_EQ(AimAndSay(Anna, Tile(3, 2), "/tc"), "You are force-paused and can't choose a start position.");
	EXPECT_GT(pAnna->IsPaused(), 0);
}

TEST_F(GameWorld, GTrainTeamsSeeTheDoorsHoldingTheirTeammates)
{
	// Clients predict teammates with their team's switch states, so they
	// need the doors holding them too, or teammates would seem to fall.
	PrepareTrainingCorridor();
	ClearMapExtras();
	g_Config.m_SvGtrainHoverDoor = 1;
	g_Config.m_SvSpamprotection = 0;
	g_Config.m_SvGtrainGoalDistance = 6;
	m_pServer->AdvanceTicks(10); // switch end ticks are only sent after tick 0
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Carl"});
	const int Anna = vIds[0], Bob = vIds[1], Carl = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Doors = [&](int ClientId) {
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init();
		GameServer()->m_pController->Snap(ClientId);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		std::vector<vec2> vDoors;
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
			if(pSnap->GetItemType(i) == NETOBJTYPE_DDNETLASER && ((const CNetObj_DDNetLaser *)pSnap->GetItem(i)->Data())->m_Type == LASERTYPE_DOOR)
			{
				const auto *pDoor = (const CNetObj_DDNetLaser *)pSnap->GetItem(i)->Data();
				vDoors.emplace_back(pDoor->m_ToX, pDoor->m_ToY);
			}
		std::sort(vDoors.begin(), vDoors.end(), [](vec2 A, vec2 B) { return A.x < B.x; });
		return vDoors;
	};
	const auto DoorUnder = [&](int ClientId) { return GameServer()->GetPlayerChar(ClientId)->m_Pos + vec2(-12, 32); };

	Say(Anna, "/team Bob");
	Say(Bob, "/a");
	pController->Tick();
	ASSERT_EQ(GameServer()->GetPlayerChar(Bob)->Team(), Anna);
	ASSERT_NE(GameServer()->GetPlayerChar(Anna)->m_Pos, GameServer()->GetPlayerChar(Bob)->m_Pos);
	std::vector<vec2> vTeam = {DoorUnder(Anna), DoorUnder(Bob)};
	std::sort(vTeam.begin(), vTeam.end(), [](vec2 A, vec2 B) { return A.x < B.x; });
	EXPECT_EQ(Doors(Anna), vTeam);
	EXPECT_EQ(Doors(Bob), vTeam);
	// Solo tees only see their own.
	EXPECT_EQ(Doors(Carl), std::vector<vec2>{DoorUnder(Carl)});
}

TEST_F(GameWorld, GTrainTeamsPlayTogetherWithoutElo)
{
	PrepareTrainingCorridor();
	// GTrain keeps the map's player collision and hooking for teams.
	for(int Zone = 0; Zone < TuneZone::NUM; ++Zone)
	{
		GameServer()->TuningList()[Zone].Set("player_collision", 1);
		GameServer()->TuningList()[Zone].Set("player_hooking", 1);
	}
	g_Config.m_SvSpamprotection = 0;
	g_Config.m_SvGtrainGoalDistance = 6;
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Carl"});
	const int Anna = vIds[0], Bob = vIds[1], Carl = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	CGameTeams &Teams = pController->Teams();
	const auto LastMessage = [&](int ClientId) {
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	const auto Reply = [&](int ClientId, const char *pText) {
		m_pServer->m_avChatMessages[ClientId].clear();
		Say(ClientId, pText);
		return LastMessage(ClientId);
	};
	const auto Chr = [&](int ClientId) { return GameServer()->GetPlayerChar(ClientId); };
	// Members share a flag and a DDRace team, collide and hook each other,
	// and show as a team. They hover at spots of their own, two tiles apart
	// and close to the leader's. Everyone else stays solo.
	const auto CheckTogether = [&](std::initializer_list<int> Members) {
		const int Leader = *Members.begin();
		ASSERT_NE(Chr(Leader), nullptr);
		for(int ClientId : Members)
		{
			ASSERT_NE(Chr(ClientId), nullptr);
			EXPECT_EQ(Chr(ClientId)->m_FreezeTime, 50);
			EXPECT_LE(distance(Chr(ClientId)->m_Pos, Chr(Leader)->m_Pos), 8 * 32.0f);
			for(int Other : Members)
				if(Other != ClientId)
					EXPECT_GE(distance(Chr(ClientId)->m_Pos, Chr(Other)->m_Pos), 2 * 32.0f);
			EXPECT_EQ(TrainingGoal(ClientId), TrainingGoal(Leader));
			EXPECT_EQ(Chr(ClientId)->Team(), Leader);
			EXPECT_FALSE(Teams.m_Core.GetSolo(ClientId));
			EXPECT_TRUE(Teams.m_Core.CanCollide(ClientId, Leader));
			EXPECT_EQ(Teams.ScoreboardTeam(ClientId), Teams.ScoreboardTeam(Leader));
			EXPECT_EQ(pController->SnapPlayerScore(ClientId, GameServer()->m_apPlayers[ClientId]), (int)GTrainElo::FromPoints(pController->Stats(ClientId).m_Points));
		}
		EXPECT_GT(Teams.ScoreboardTeam(Leader), 0);
		EXPECT_TRUE(GameServer()->m_apPlayers[Leader]->IsNameMarked());
	};
	const auto CheckSolo = [&](int ClientId) {
		ASSERT_NE(Chr(ClientId), nullptr);
		EXPECT_TRUE(Teams.m_Core.GetSolo(ClientId));
		EXPECT_EQ(Chr(ClientId)->Team(), ClientId);
		EXPECT_EQ(Teams.ScoreboardTeam(ClientId), 0);
	};
	for(int ClientId : vIds)
		CheckSolo(ClientId);

	// Requests by name, accepted with /accept or /a within a minute.
	EXPECT_EQ(Reply(Bob, "/accept"), "You have no team request. Requests last a minute.");
	EXPECT_EQ(Reply(Anna, "/team Anna"), "You cannot team up with yourself.");
	EXPECT_EQ(Reply(Anna, "/team Carl"), "You asked 'Carl' to team up. They can join with /accept or /a within a minute.");
	m_pServer->AdvanceTicks((60 + 1) * m_pServer->TickSpeed());
	EXPECT_EQ(Reply(Carl, "/a"), "You have no team request. Requests last a minute.");
	EXPECT_EQ(Reply(Anna, "/team Bob"), "You asked 'Bob' to team up. They can join with /accept or /a within a minute.");
	EXPECT_EQ(LastMessage(Bob), "'Anna' wants to team up with you. Say /accept or /a within a minute to join.");
	EXPECT_EQ(Reply(Bob, "/a"), "'Bob' joined the team of 'Anna'. /team alone leaves it.");
	EXPECT_EQ(LastMessage(Anna), "'Bob' joined the team of 'Anna'. /team alone leaves it.");
	EXPECT_EQ(Reply(Bob, "/a"), "You have no team request. Requests last a minute.");
	pController->Tick();
	CheckTogether({Anna, Bob});
	CheckSolo(Carl);
	EXPECT_FALSE(Teams.m_Core.CanCollide(Anna, Carl));
	EXPECT_EQ(Reply(Anna, "/team Bob"), "'Bob' is already in your team.");

	// Teams and fights don't mix.
	EXPECT_EQ(Reply(Bob, "/fight Carl"), "You are in a team, which can't fight. /team alone leaves it.");
	EXPECT_EQ(Reply(Bob, "/fight"), "You are in a team, which can't fight. /team alone leaves it.");
	EXPECT_EQ(Reply(Carl, "/fight Anna"), "'Anna' is in a team, which can't fight.");
	CheckTogether({Anna, Bob});
	CheckSolo(Carl);

	// The leader chooses the settings.
	EXPECT_EQ(Reply(Bob, "/distance 60"), "Only the team leader can change the goal distance.");

	// Any member's capture counts for the team and earns no ELO, then the
	// team starts over together.
	pController->SetMapDifficulty("Hard", 2);
	Chr(Anna)->Unfreeze();
	Chr(Bob)->Unfreeze();
	pController->Tick();
	m_pServer->AdvanceTicks(2 * m_pServer->TickSpeed());
	const CGTrainStats Before = pController->Stats(Bob);
	const vec2 Goal = TrainingGoal(Bob);
	Chr(Bob)->SetPosition(Goal);
	Chr(Bob)->m_Pos = Chr(Bob)->m_PrevPos = Goal;
	Chr(Bob)->ResetVelocity();
	CMemoryLogger Logger;
	{
		CLogScope Scope(&Logger);
		pController->Tick();
	}
	EXPECT_NE(Logger.ConcatenatedLines().find("'Bob' captured the flag for their team in 2.00 seconds (0 fails, +0 ELO (team))."), std::string::npos) << Logger.ConcatenatedLines();
	EXPECT_EQ(pController->Stats(Bob).m_Points, Before.m_Points);
	EXPECT_EQ(pController->Stats(Bob).m_Captures, Before.m_Captures + 1);
	CheckTogether({Anna, Bob});

	// Any member's /r retries the whole team, everyone at their spot.
	const vec2 AnnaSpot = Chr(Anna)->m_Pos, BobSpot = Chr(Bob)->m_Pos;
	Chr(Anna)->Unfreeze();
	Chr(Bob)->Unfreeze();
	pController->Tick();
	for(int ClientId : {Anna, Bob})
	{
		Chr(ClientId)->SetPosition(AnnaSpot + vec2(32, 0));
		Chr(ClientId)->m_Pos = Chr(ClientId)->m_PrevPos = AnnaSpot + vec2(32, 0);
	}
	Say(Bob, "/r");
	CheckTogether({Anna, Bob});
	EXPECT_EQ(Chr(Anna)->m_Pos, AnnaSpot);
	EXPECT_EQ(Chr(Bob)->m_Pos, BobSpot);

	// Any member's death starts the team over.
	GameServer()->m_apPlayers[Bob]->KillCharacter(WEAPON_SELF);
	EXPECT_EQ(Chr(Anna), nullptr);
	ASSERT_NE(Chr(Carl), nullptr);
	pController->Tick();
	CheckTogether({Anna, Bob});

	// Members can add players too, who join everyone.
	EXPECT_EQ(Reply(Bob, "/team Carl"), "You asked 'Carl' to team up. They can join with /accept or /a within a minute.");
	EXPECT_EQ(LastMessage(Carl), "'Bob' wants to add you to their team. Say /accept or /a within a minute to join.");
	Say(Carl, "/accept");
	pController->Tick();
	CheckTogether({Anna, Bob, Carl});

	// Leaving starts the others over, the last one alone.
	EXPECT_EQ(Reply(Anna, "/team"), "You left the team.");
	EXPECT_EQ(LastMessage(Bob), "'Anna' left the team.");
	pController->Tick();
	CheckSolo(Anna);
	CheckTogether({Carl, Bob});
	Say(Bob, "/team");
	EXPECT_EQ(LastMessage(Carl), "'Bob' left the team, you train alone again.");
	pController->Tick();
	for(int ClientId : vIds)
		CheckSolo(ClientId);
	EXPECT_EQ(Reply(Carl, "/team"), "You are not in a team. /team <player> asks a player to team up.");

	// Fights can't team up.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(Reply(Bob, "/team Carl"), "You are in a fight, which can't team up. /fight alone leaves it.");
	EXPECT_EQ(Reply(Carl, "/team Anna"), "'Anna' is in a fight, which can't team up.");
	for(int ClientId : vIds)
		EXPECT_TRUE(Teams.m_Core.GetSolo(ClientId));

	// Spectators leave their team.
	pController->Fight(Bob, "");
	pController->Tick();
	Say(Carl, "/team Anna");
	Say(Anna, "/a");
	pController->Tick();
	CheckTogether({Carl, Anna});
	GameServer()->m_apPlayers[Anna]->SetTeam(TEAM_SPECTATORS, false);
	pController->Tick();
	EXPECT_EQ(LastMessage(Carl), "'Anna' left the team, you train alone again.");
	pController->Tick();
	CheckSolo(Carl);
}

TEST_F(GameWorld, GTrainRetryPreservesGoalAndRestartsTimer)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Runner", "Observer"});
	const int Runner = vIds[0], Observer = vIds[1];
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
	const vec2 Start = pChr->m_Pos;
	const vec2 Goal = TrainingGoal(Runner);
	const vec2 ObserverPosition = GameServer()->GetPlayerChar(Observer)->m_Pos;
	const vec2 ObserverGoal = TrainingGoal(Observer);
	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	m_pServer->AdvanceTicks(5 * m_pServer->TickSpeed());
	pChr->m_Pos = pChr->m_PrevPos = Start - normalize(Goal - Start) * 32.0f;
	pChr->SetPosition(pChr->m_Pos);
	pChr->SetVelocity(vec2(3, 4));
	g_Config.m_SvGtrainGoalDistance = 7; // a retry must not pick a new goal
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Runner);
	EXPECT_EQ(pChr->m_Pos, Start);
	EXPECT_EQ(pChr->Core()->m_Vel, vec2(0, 0));
	EXPECT_TRUE(pChr->m_ZeroGravity);
	EXPECT_EQ(pChr->m_FreezeTime, 50);
	EXPECT_EQ(TrainingGoal(Runner), Goal);

	// /r also retries a dead character before its normal spawn is processed.
	pChr->Die(Runner, WEAPON_WORLD);
	EXPECT_EQ(GameServer()->GetPlayerChar(Runner), nullptr);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	pChr = GameServer()->GetPlayerChar(Runner);
	ASSERT_NE(pChr, nullptr);
	GameServer()->m_pController->Tick();
	EXPECT_EQ(pChr->m_Pos, Start);
	EXPECT_EQ(TrainingGoal(Runner), Goal);
	EXPECT_TRUE(pChr->m_ZeroGravity);
	EXPECT_EQ(pChr->m_FreezeTime, 50);
	EXPECT_EQ(GameServer()->GetPlayerChar(Observer)->m_Pos, ObserverPosition);
	EXPECT_EQ(TrainingGoal(Observer), ObserverGoal);

	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	m_pServer->AdvanceTicks(m_pServer->TickSpeed());
	pChr->m_Pos = pChr->m_PrevPos = Goal;
	pChr->SetPosition(Goal);
	CMemoryLogger CaptureLogger;
	{
		CLogScope Scope(&CaptureLogger);
		GameServer()->m_pController->Tick();
	}
	EXPECT_NE(CaptureLogger.ConcatenatedLines().find("'Runner' captured the flag in 1.00 seconds (2 fails)."), std::string::npos);
}

TEST_F(GameWorld, GTrainRetryOnlyResetsRequestingFightMember)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Observer"});
	const int Anna = vIds[0], Bob = vIds[1], Observer = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	pController->Fight(Bob, "Anna");
	pController->Tick();
	const vec2 Start = GameServer()->GetPlayerChar(Anna)->m_Pos;
	const vec2 Goal = TrainingGoal(Anna);
	const vec2 ObserverPosition = GameServer()->GetPlayerChar(Observer)->m_Pos;
	const vec2 ObserverGoal = TrainingGoal(Observer);
	for(int ClientId : {Anna, Bob})
		GameServer()->GetPlayerChar(ClientId)->Unfreeze();
	pController->Tick();
	CCharacter *pLeader = GameServer()->GetPlayerChar(Anna);
	const vec2 LeaderPosition = Start - normalize(Goal - Start) * 32.0f;
	pLeader->m_Pos = pLeader->m_PrevPos = LeaderPosition;
	pLeader->SetPosition(LeaderPosition);
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Bob);
	EXPECT_EQ(pLeader->m_Pos, LeaderPosition);
	EXPECT_FALSE(pLeader->m_ZeroGravity);
	EXPECT_EQ(pLeader->m_FreezeTime, 0);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob)->m_Pos, Start);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob)->m_FreezeTime, 50);
	EXPECT_EQ(TrainingGoal(Bob), Goal);

	CCharacter *pMember = GameServer()->GetPlayerChar(Bob);
	pMember->Unfreeze();
	pController->Tick();
	const vec2 MemberPosition = Start + normalize(Goal - Start) * 32.0f;
	pMember->m_Pos = pMember->m_PrevPos = MemberPosition;
	pMember->SetPosition(MemberPosition);
	pMember->SetVelocity(vec2(3, 4));
	m_pServer->AdvanceTicks(m_pServer->TickSpeed());
	g_Config.m_SvGtrainGoalDistance = 7; // leader retries keep the shared goal

	// The leader retries only themselves, including through the /r alias.
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Anna);
	pController->Tick();
	EXPECT_EQ(pLeader->m_Pos, Start);
	EXPECT_TRUE(pLeader->m_ZeroGravity);
	EXPECT_EQ(pLeader->m_FreezeTime, 50);
	EXPECT_EQ(TrainingGoal(Anna), Goal);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), pMember);
	EXPECT_EQ(pMember->m_Pos, MemberPosition);
	EXPECT_EQ(pMember->Core()->m_Vel, vec2(3, 4));
	EXPECT_FALSE(pMember->m_ZeroGravity);
	EXPECT_EQ(pMember->m_FreezeTime, 0);
	EXPECT_EQ(TrainingGoal(Bob), Goal);

	// Retrying the leader does not respawn a dead member.
	GameServer()->m_apPlayers[Bob]->KillCharacter(WEAPON_SELF);
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Anna);
	pController->Tick();
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), nullptr);
	EXPECT_EQ(TrainingGoal(Anna), Goal);
	pMember = GameServer()->m_apPlayers[Bob]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	EXPECT_EQ(pMember->m_Pos, Start);
	EXPECT_EQ(TrainingGoal(Bob), Goal);

	// Retry overrides the pending next-position reset after a leader's death.
	GameServer()->m_apPlayers[Anna]->KillCharacter(WEAPON_SELF);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), nullptr);
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Anna);
	pController->Tick();
	ASSERT_NE(GameServer()->GetPlayerChar(Anna), nullptr);
	EXPECT_EQ(GameServer()->GetPlayerChar(Anna)->m_Pos, Start);
	EXPECT_EQ(GameServer()->GetPlayerChar(Anna)->m_FreezeTime, 50);
	EXPECT_EQ(TrainingGoal(Anna), Goal);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), nullptr);
	GameServer()->m_apPlayers[Bob]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	for(int ClientId : {Anna, Bob})
	{
		ASSERT_NE(GameServer()->GetPlayerChar(ClientId), nullptr);
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, Start);
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_FreezeTime, 50);
		EXPECT_EQ(TrainingGoal(ClientId), Goal);
	}
	EXPECT_EQ(GameServer()->GetPlayerChar(Observer)->m_Pos, ObserverPosition);
	EXPECT_EQ(TrainingGoal(Observer), ObserverGoal);
}

TEST_F(GameWorld, GTrainZeroGravityUsesCurrentTuneZoneAndSnapshot)
{
	PrepareTrainingCorridor();
	CCollision *pCollision = GameServer()->Collision();
	auto *pTune = const_cast<CTuneTile *>(pCollision->TuneLayer());
	ASSERT_NE(pTune, nullptr);
	for(int i = 0; i < pCollision->GetWidth() * pCollision->GetHeight(); ++i)
		pTune[i] = {1, TILE_TUNE};
	const vec2 OriginalPosition(48, 80); // start line, excluded from training spawns
	pTune[pCollision->GetPureMapIndex(OriginalPosition)].m_Number = 2;
	GameServer()->TuningList()[1].Set("gravity", 2.0f);
	GameServer()->TuningList()[1].Set("ground_control_speed", 8.0f);
	GameServer()->TuningList()[2].Set("gravity", 5.0f);
	GameServer()->TuningList()[2].Set("ground_control_speed", 17.0f);
	g_Config.m_DbgDummies = 1;
	m_pServer->UpdateDebugDummies(false);
	const int ClientId = m_pServer->MaxClients() - 1;
	m_pServer->AdvanceTicks(1);
	CCharacter *pChr = GameServer()->m_apPlayers[ClientId]->ForceSpawn(OriginalPosition);
	EXPECT_EQ(pChr->m_TuneZone, 2);
	pChr->TickDeferred(); // establish a cached snapshot of the original position
	GameServer()->m_pController->Tick();
	EXPECT_NE(pChr->m_Pos, OriginalPosition);
	EXPECT_EQ(pChr->m_TuneZone, 1);
	EXPECT_FLOAT_EQ(pChr->Core()->m_Tuning.m_Gravity, 0.0f);
	ASSERT_TRUE(m_pServer->m_aLastTuning[ClientId].has_value());
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[ClientId]->m_Gravity, 0.0f);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[ClientId]->m_GroundControlSpeed, 8.0f);
	EXPECT_FLOAT_EQ(GameServer()->TuningList()[1].m_Gravity, 2.0f);

	CSnapshotBuffer Buffer;
	m_pServer->m_SnapshotBuilder.Init();
	pChr->Snap(ClientId);
	m_pServer->m_SnapshotBuilder.Finish(&Buffer);
	const CSnapshot *pSnap = Buffer.AsSnapshot();
	int Characters = 0;
	for(int i = 0; i < pSnap->NumItems(); ++i)
		if(pSnap->GetItemType(i) == NETOBJTYPE_CHARACTER)
		{
			const auto *pCharacter = (const CNetObj_Character *)pSnap->GetItem(i)->Data();
			EXPECT_EQ(vec2(pCharacter->m_X, pCharacter->m_Y), pChr->m_Pos);
			++Characters;
		}
	EXPECT_EQ(Characters, 1);

	// Crossing into another zone while hovering keeps its other tuning values.
	const vec2 NewPosition = pChr->m_Pos - normalize(TrainingGoal(ClientId) - pChr->m_Pos) * 32.0f;
	pTune[pCollision->GetPureMapIndex(NewPosition)].m_Number = 2;
	pChr->SetPosition(NewPosition);
	pChr->m_Pos = pChr->m_PrevPos = NewPosition;
	pChr->SetZeroGravity(true);
	EXPECT_EQ(pChr->m_TuneZone, 2);
	EXPECT_FLOAT_EQ(pChr->Core()->m_Tuning.m_Gravity, 0.0f);
	EXPECT_FLOAT_EQ(pChr->Core()->m_Tuning.m_GroundControlSpeed, 17.0f);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[ClientId]->m_Gravity, 0.0f);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[ClientId]->m_GroundControlSpeed, 17.0f);

	// Thawing restores the current zone, including the 0.7 tuning packet layout.
	m_pServer->m_aClients[ClientId].m_Sixup = true;
	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	EXPECT_FALSE(pChr->m_ZeroGravity);
	EXPECT_FLOAT_EQ(pChr->Core()->m_Tuning.m_Gravity, 5.0f);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[ClientId]->m_Gravity, 5.0f);
	EXPECT_FLOAT_EQ(m_pServer->m_aLastTuning[ClientId]->m_GroundControlSpeed, 17.0f);
	EXPECT_FLOAT_EQ(GameServer()->TuningList()[2].m_Gravity, 5.0f);
}

TEST_F(GameWorld, GTrainFightChainingDeathAndCapture)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson", "Observer"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2], Observer = vIds[3];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const vec2 ObserverPosition = GameServer()->GetPlayerChar(Observer)->m_Pos;
	const vec2 ObserverGoal = TrainingGoal(Observer);
	m_pServer->Console()->ExecuteLineFlag("fight Anna", CFGFLAG_CHAT, Bob);
	pController->Tick();
	m_pServer->Console()->ExecuteLineFlag("fight Bob", CFGFLAG_CHAT, Wilson);
	pController->Tick();
	const auto CheckSharedAttempt = [&] {
		const vec2 Position = GameServer()->GetPlayerChar(Anna)->m_Pos;
		const vec2 Goal = TrainingGoal(Anna);
		for(int ClientId : {Anna, Bob, Wilson})
		{
			CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
			ASSERT_NE(pChr, nullptr);
			EXPECT_EQ(pChr->m_Pos, Position);
			EXPECT_EQ(TrainingGoal(ClientId), Goal);
			EXPECT_TRUE(pChr->m_ZeroGravity);
			EXPECT_EQ(pChr->m_FreezeTime, 50);
			EXPECT_EQ(pController->Teams().ScoreboardTeam(ClientId), pController->Teams().ScoreboardTeam(Anna));
		}
		EXPECT_GT(pController->Teams().ScoreboardTeam(Anna), 0);
		EXPECT_TRUE(GameServer()->m_apPlayers[Anna]->IsNameMarked());
		EXPECT_FALSE(GameServer()->m_apPlayers[Bob]->IsNameMarked());
		EXPECT_FALSE(GameServer()->m_apPlayers[Wilson]->IsNameMarked());
		EXPECT_EQ(pController->Teams().ScoreboardTeam(Observer), 0);
		EXPECT_EQ(GameServer()->GetPlayerChar(Observer)->m_Pos, ObserverPosition);
		EXPECT_EQ(TrainingGoal(Observer), ObserverGoal);
	};
	CheckSharedAttempt();

	// A member can die and respawn without resetting their active leader.
	CCharacter *pLeader = GameServer()->GetPlayerChar(Anna);
	CCharacter *pPeer = GameServer()->GetPlayerChar(Wilson);
	const vec2 GroupStart = pLeader->m_Pos;
	const vec2 GroupGoal = TrainingGoal(Anna);
	const vec2 LeaderPosition = GroupStart + normalize(GroupGoal - GroupStart) * 32.0f;
	pLeader->Unfreeze();
	pLeader->SetPosition(LeaderPosition);
	pLeader->m_Pos = pLeader->m_PrevPos = LeaderPosition;
	pController->Tick();
	GameServer()->m_apPlayers[Bob]->KillCharacter(WEAPON_SELF);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), nullptr);
	pController->Tick();
	EXPECT_EQ(GameServer()->GetPlayerChar(Anna), pLeader);
	EXPECT_EQ(GameServer()->GetPlayerChar(Wilson), pPeer);
	EXPECT_EQ(pLeader->m_Pos, LeaderPosition);
	EXPECT_FALSE(pLeader->m_ZeroGravity);
	EXPECT_EQ(TrainingGoal(Anna), GroupGoal);
	CCharacter *pRespawned = GameServer()->m_apPlayers[Bob]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	EXPECT_EQ(pRespawned->m_Pos, GroupStart);
	EXPECT_EQ(TrainingGoal(Bob), GroupGoal);
	EXPECT_TRUE(pRespawned->m_ZeroGravity);
	EXPECT_EQ(pRespawned->m_FreezeTime, 50);
	EXPECT_EQ(pLeader->m_Pos, LeaderPosition);
	EXPECT_FALSE(pLeader->m_ZeroGravity);
	EXPECT_EQ(GameServer()->GetPlayerChar(Wilson), pPeer);

	// Only the leader's death propagates and respawns the entire group.
	GameServer()->m_apPlayers[Anna]->KillCharacter(WEAPON_SELF);
	for(int ClientId : {Anna, Bob, Wilson})
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId), nullptr);
	ASSERT_NE(GameServer()->GetPlayerChar(Observer), nullptr);
	pController->Tick();
	CheckSharedAttempt();

	// Only the winner's capture time is announced, then the whole group resets.
	const vec2 Goal = TrainingGoal(Anna);
	for(int ClientId : {Anna, Bob, Wilson})
		GameServer()->GetPlayerChar(ClientId)->Unfreeze();
	pController->Tick();
	m_pServer->AdvanceTicks(2 * m_pServer->TickSpeed());
	CCharacter *pWinner = GameServer()->GetPlayerChar(Wilson);
	pWinner->SetPosition(Goal);
	pWinner->m_Pos = pWinner->m_PrevPos = Goal;
	pWinner->ResetVelocity();
	CMemoryLogger CaptureLogger;
	{
		CLogScope Scope(&CaptureLogger);
		pController->Tick();
	}
	const std::string Messages = CaptureLogger.ConcatenatedLines();
	EXPECT_NE(Messages.find("'Wilson' captured the flag (0 fails). Fight score (first to 10):"), std::string::npos);
	EXPECT_NE(Messages.find("'Wilson': 1"), std::string::npos);
	EXPECT_NE(Messages.find("'Anna': 0"), std::string::npos);
	EXPECT_NE(Messages.find("'Bob': 0"), std::string::npos);
	EXPECT_EQ(Messages.find("seconds"), std::string::npos);
	CheckSharedAttempt();

	// The next winner adds a point without losing the previous winner's score.
	for(int ClientId : {Anna, Bob, Wilson})
		GameServer()->GetPlayerChar(ClientId)->Unfreeze();
	pController->Tick();
	m_pServer->AdvanceTicks(m_pServer->TickSpeed());
	pWinner = GameServer()->GetPlayerChar(Anna);
	pWinner->m_Pos = pWinner->m_PrevPos = TrainingGoal(Anna);
	pWinner->SetPosition(pWinner->m_Pos);
	CMemoryLogger NextLogger;
	{
		CLogScope Scope(&NextLogger);
		pController->Tick();
	}
	EXPECT_NE(NextLogger.ConcatenatedLines().find("'Anna' captured the flag (0 fails). Fight score (first to 10):"), std::string::npos);
	EXPECT_NE(NextLogger.ConcatenatedLines().find("'Anna': 1"), std::string::npos);
	EXPECT_NE(NextLogger.ConcatenatedLines().find("'Wilson': 1"), std::string::npos);
	EXPECT_EQ(NextLogger.ConcatenatedLines().find("seconds"), std::string::npos);
	CheckSharedAttempt();

	// Deaths and retries preserve the fight standings.
	GameServer()->m_apPlayers[Anna]->KillCharacter(WEAPON_SELF);
	pController->Tick();
	pController->Retry(Anna);
	for(int ClientId : {Anna, Bob, Wilson})
		GameServer()->GetPlayerChar(ClientId)->Unfreeze();
	pController->Tick();
	pWinner = GameServer()->GetPlayerChar(Wilson);
	pWinner->m_Pos = pWinner->m_PrevPos = TrainingGoal(Wilson);
	pWinner->SetPosition(pWinner->m_Pos);
	CMemoryLogger RepeatLogger;
	{
		CLogScope Scope(&RepeatLogger);
		pController->Tick();
	}
	EXPECT_NE(RepeatLogger.ConcatenatedLines().find("'Wilson': 2"), std::string::npos);
	EXPECT_NE(RepeatLogger.ConcatenatedLines().find("'Anna': 1"), std::string::npos);
	CheckSharedAttempt();

	// Leaving clears that player's wins; rejoining keeps the group's scores.
	pController->Fight(Wilson, "");
	pController->Fight(Wilson, "Anna");
	pController->Tick();
	for(int ClientId : {Anna, Bob, Wilson})
		GameServer()->GetPlayerChar(ClientId)->Unfreeze();
	pController->Tick();
	pWinner = GameServer()->GetPlayerChar(Anna);
	pWinner->m_Pos = pWinner->m_PrevPos = TrainingGoal(Anna);
	pWinner->SetPosition(pWinner->m_Pos);
	CMemoryLogger RejoinLogger;
	{
		CLogScope Scope(&RejoinLogger);
		pController->Tick();
	}
	EXPECT_NE(RejoinLogger.ConcatenatedLines().find("'Anna': 2"), std::string::npos);
	EXPECT_NE(RejoinLogger.ConcatenatedLines().find("'Wilson': 0"), std::string::npos);
	CheckSharedAttempt();

	// Once everyone else leaves, the fight is over and its last score too.
	pController->Fight(Bob, "");
	pController->Fight(Wilson, "");
	pController->Fight(Bob, "Anna");
	pController->Tick();
	for(int ClientId : {Anna, Bob})
		GameServer()->GetPlayerChar(ClientId)->Unfreeze();
	pController->Tick();
	pWinner = GameServer()->GetPlayerChar(Bob);
	pWinner->m_Pos = pWinner->m_PrevPos = TrainingGoal(Bob);
	pWinner->SetPosition(pWinner->m_Pos);
	CMemoryLogger RemainingLogger;
	{
		CLogScope Scope(&RemainingLogger);
		pController->Tick();
	}
	EXPECT_NE(RemainingLogger.ConcatenatedLines().find("'Anna': 0"), std::string::npos);
	EXPECT_NE(RemainingLogger.ConcatenatedLines().find("'Bob': 1"), std::string::npos);
	EXPECT_EQ(RemainingLogger.ConcatenatedLines().find("'Wilson':"), std::string::npos);
}

TEST_F(GameWorld, GTrainFightJoinKeepsTargetAttempt)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson", "Observer"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2], Observer = vIds[3];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	CCharacter *pTarget = GameServer()->GetPlayerChar(Anna);
	CCharacter *pJoiner = GameServer()->GetPlayerChar(Bob);
	const vec2 Start = pTarget->m_Pos;
	const vec2 Goal = TrainingGoal(Anna);
	const vec2 ObserverPosition = GameServer()->GetPlayerChar(Observer)->m_Pos;
	const vec2 ObserverGoal = TrainingGoal(Observer);
	for(CCharacter *pChr : {pTarget, pJoiner})
		pChr->Unfreeze();
	pController->Tick();
	const vec2 TargetPosition = Start + normalize(Goal - Start) * 32.0f;
	pTarget->m_Pos = pTarget->m_PrevPos = TargetPosition;
	pTarget->SetPosition(TargetPosition);
	pTarget->SetVelocity(vec2(3, 4));
	const vec2 JoinerPosition = Start - normalize(Goal - Start) * 32.0f;
	pJoiner->m_Pos = pJoiner->m_PrevPos = JoinerPosition;
	pJoiner->SetPosition(JoinerPosition);
	pJoiner->SetVelocity(vec2(5, 6));
	const auto CheckTargetKept = [&] {
		EXPECT_EQ(GameServer()->GetPlayerChar(Anna), pTarget);
		EXPECT_EQ(pTarget->m_Pos, TargetPosition);
		EXPECT_EQ(pTarget->Core()->m_Vel, vec2(3, 4));
		EXPECT_FALSE(pTarget->m_ZeroGravity);
		EXPECT_EQ(pTarget->m_FreezeTime, 0);
		EXPECT_EQ(TrainingGoal(Anna), Goal);
		EXPECT_EQ(GameServer()->GetPlayerChar(Observer)->m_Pos, ObserverPosition);
		EXPECT_EQ(TrainingGoal(Observer), ObserverGoal);
	};

	// The joiner starts the target's current attempt; the target keeps playing.
	m_pServer->m_avChatMessages[Anna].clear();
	m_pServer->Console()->ExecuteLineFlag("fight Anna", CFGFLAG_CHAT, Bob);
	pController->Tick();
	CheckTargetKept();
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), pJoiner);
	EXPECT_EQ(pJoiner->m_Pos, Start);
	EXPECT_EQ(pJoiner->Core()->m_Vel, vec2(0, 0));
	EXPECT_TRUE(pJoiner->m_ZeroGravity);
	EXPECT_EQ(pJoiner->m_FreezeTime, 50);
	EXPECT_EQ(TrainingGoal(Bob), Goal);
	EXPECT_GT(pController->Teams().ScoreboardTeam(Anna), 0);
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Bob), pController->Teams().ScoreboardTeam(Anna));
	ASSERT_FALSE(m_pServer->m_avChatMessages[Anna].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Anna].back(), "'Bob' joined the fight with 'Anna'. Use /fight to leave.");

	// A dead player joining through a member enters the same attempt on respawn.
	GameServer()->m_apPlayers[Wilson]->KillCharacter(WEAPON_SELF);
	pController->Fight(Wilson, "Bob");
	pController->Tick();
	CheckTargetKept();
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), pJoiner);
	EXPECT_EQ(pJoiner->m_Pos, Start);
	CCharacter *pRespawned = GameServer()->m_apPlayers[Wilson]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	CheckTargetKept();
	EXPECT_EQ(pRespawned->m_Pos, Start);
	EXPECT_TRUE(pRespawned->m_ZeroGravity);
	EXPECT_EQ(TrainingGoal(Wilson), Goal);
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Wilson), pController->Teams().ScoreboardTeam(Anna));
}

TEST_F(GameWorld, GTrainFightAllowsPausedPlayersButNotSpectators)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson", "Viewer"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2], Viewer = vIds[3];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	CCharacter *pTarget = GameServer()->GetPlayerChar(Anna);
	const vec2 Start = pTarget->m_Pos;
	const vec2 Goal = TrainingGoal(Anna);

	// A /spec target can be fought and stays paused in place.
	GameServer()->m_apPlayers[Anna]->Pause(CPlayer::PAUSE_SPEC, true);
	pTarget->Pause(true);
	m_pServer->m_avChatMessages[Bob].clear();
	m_pServer->Console()->ExecuteLineFlag("fight Anna", CFGFLAG_CHAT, Bob);
	pController->Tick();
	ASSERT_FALSE(m_pServer->m_avChatMessages[Bob].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "'Bob' joined the fight with 'Anna'. Use /fight to leave.");
	const int Team = pController->Teams().ScoreboardTeam(Anna);
	EXPECT_GT(Team, 0);
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Bob), Team);
	EXPECT_EQ(GameServer()->GetPlayerChar(Anna), pTarget);
	EXPECT_TRUE(pTarget->IsPaused());
	EXPECT_EQ(pTarget->m_Pos, Start);
	EXPECT_EQ(TrainingGoal(Anna), Goal);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob)->m_Pos, Start);
	EXPECT_EQ(TrainingGoal(Bob), Goal);

	// A paused player can join too and enters the current attempt.
	GameServer()->m_apPlayers[Wilson]->Pause(CPlayer::PAUSE_PAUSED, true);
	ASSERT_NE(GameServer()->m_apPlayers[Wilson]->IsPaused(), 0);
	pController->Fight(Wilson, "Bob");
	pController->Tick();
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Wilson), Team);
	EXPECT_EQ(GameServer()->GetPlayerChar(Wilson)->m_Pos, Start);
	EXPECT_EQ(TrainingGoal(Wilson), Goal);
	EXPECT_EQ(pTarget->m_Pos, Start);

	// Players in the spectator team can neither fight nor be fought.
	GameServer()->m_apPlayers[Viewer]->SetTeam(TEAM_SPECTATORS, false);
	for(const auto &[ClientId, Name] : {std::pair{Viewer, "Anna"}, {Bob, "Viewer"}})
	{
		m_pServer->m_avChatMessages[ClientId].clear();
		pController->Fight(ClientId, Name);
		ASSERT_FALSE(m_pServer->m_avChatMessages[ClientId].empty());
		EXPECT_EQ(m_pServer->m_avChatMessages[ClientId].back(), "Spectators cannot join a fight.");
	}
	pController->Tick();
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Viewer), 0);
	for(int ClientId : {Anna, Bob, Wilson})
		EXPECT_EQ(pController->Teams().ScoreboardTeam(ClientId), Team);
}

TEST_F(GameWorld, GTrainFightGoesOnWhenSomeoneLeaves)
{
	// However players leave, the others keep their spot and flag. The leader
	// joining the spectators is no death of the leader.
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson", "Dee"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2], Dee = vIds[3];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	for(int ClientId : {Bob, Wilson, Dee})
		pController->Fight(ClientId, "Anna");
	pController->Tick();
	const vec2 Position = GameServer()->GetPlayerChar(Anna)->m_Pos;
	const vec2 Goal = TrainingGoal(Anna);
	const auto ExpectKept = [&](std::initializer_list<int> Ids, const char *pHow) {
		for(int i = 0; i < 3; ++i)
			pController->Tick();
		for(int ClientId : Ids)
		{
			ASSERT_TRUE(GameServer()->GetPlayerChar(ClientId)) << pHow;
			EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, Position) << pHow;
			EXPECT_EQ(TrainingGoal(ClientId), Goal) << pHow;
		}
	};
	pController->DoTeamChange(GameServer()->m_apPlayers[Anna], TEAM_SPECTATORS, false);
	ExpectKept({Bob, Wilson, Dee}, "the leader spectating");
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Anna), 0);
	EXPECT_NE(pController->Teams().ScoreboardTeam(Bob), 0);
	m_pServer->Console()->ExecuteLineFlag("fight", CFGFLAG_CHAT, Bob);
	ExpectKept({Wilson, Dee}, "the new leader using /fight");
	pController->DoTeamChange(GameServer()->m_apPlayers[Dee], TEAM_SPECTATORS, false);
	ExpectKept({Wilson}, "the last member spectating");
}

TEST_F(GameWorld, GTrainFightTeamsSwitchLeaveAndDisconnect)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson", "Dee", "Ed"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2], Dee = vIds[3], Ed = vIds[4];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	for(const auto &[ClientId, Name] : {std::pair{Bob, "Anna"}, {Wilson, "Bob"}, {Ed, "Dee"}})
		pController->Fight(ClientId, Name);
	pController->Tick();
	EXPECT_NE(pController->Teams().ScoreboardTeam(Anna), pController->Teams().ScoreboardTeam(Dee));
	const int Team = pController->Teams().ScoreboardTeam(Dee);
	const int OldTeam = pController->Teams().ScoreboardTeam(Anna);
	const vec2 OldPosition = GameServer()->GetPlayerChar(Anna)->m_Pos;
	const vec2 OldGoal = TrainingGoal(Anna);
	// Only the player switches fights, the rest of theirs goes on.
	pController->Fight(Bob, "Ed");
	pController->Tick();
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Bob), Team);
	EXPECT_EQ(TrainingGoal(Bob), TrainingGoal(Dee));
	for(int ClientId : {Anna, Wilson})
	{
		EXPECT_EQ(pController->Teams().ScoreboardTeam(ClientId), OldTeam);
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, OldPosition);
		EXPECT_EQ(TrainingGoal(ClientId), OldGoal);
	}
	EXPECT_TRUE(GameServer()->m_apPlayers[Anna]->IsNameMarked());
	// The leader switching leaves the rest to a promoted leader.
	pController->Fight(Anna, "Dee");
	pController->Tick();
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Anna), Team);
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Wilson), 0); // alone now
	pController->Fight(Wilson, "Dee");
	pController->Tick();
	for(int ClientId : vIds)
	{
		EXPECT_EQ(pController->Teams().ScoreboardTeam(ClientId), Team);
		EXPECT_EQ(TrainingGoal(ClientId), TrainingGoal(Dee));
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, GameServer()->GetPlayerChar(Dee)->m_Pos);
	}
	EXPECT_TRUE(GameServer()->m_apPlayers[Dee]->IsNameMarked());
	EXPECT_EQ(pController->SnapPlayerScore(Anna, GameServer()->m_apPlayers[Anna]), 0);

	// Leaving a nonleader preserves the leader and the other players' attempt.
	const vec2 GroupPosition = GameServer()->GetPlayerChar(Dee)->m_Pos;
	const vec2 GroupGoal = TrainingGoal(Dee);
	m_pServer->Console()->ExecuteLineFlag("fight", CFGFLAG_CHAT, Bob);
	pController->Tick();
	EXPECT_EQ(pController->Teams().ScoreboardTeam(Bob), 0);
	EXPECT_TRUE(GameServer()->m_apPlayers[Dee]->IsNameMarked());
	EXPECT_EQ(GameServer()->GetPlayerChar(Dee)->m_Pos, GroupPosition);
	EXPECT_EQ(TrainingGoal(Dee), GroupGoal);
	GameServer()->m_apPlayers[Bob]->KillCharacter(WEAPON_SELF);
	for(int ClientId : {Anna, Wilson, Dee, Ed})
		ASSERT_NE(GameServer()->GetPlayerChar(ClientId), nullptr);

	// A new group starts with zero wins on both protocols.
	for(bool Sixup : {false, true})
	{
		m_pServer->m_aClients[Dee].m_Sixup = Sixup;
		if(Sixup)
		{
			GameServer()->m_PlayerMapping.InitPlayerMap(Dee);
			GameServer()->m_PlayerMapping.ForceInsertPlayer(Anna, Dee);
		}
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init(Sixup);
		// Switch prediction must use the visible team's ID with solo state.
		GameServer()->m_World.m_Core.InitSwitchers(1);
		GameServer()->Switchers()[1].m_aStatus[GameServer()->GetDDRaceTeam(Dee)] = false;
		GameServer()->Switchers()[1].m_aStatus[GameServer()->GetDDRaceTeam(Anna)] = true;
		pController->Snap(Dee);
		GameServer()->m_apPlayers[Dee]->Snap(Dee);
		GameServer()->m_apPlayers[Anna]->Snap(Dee);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		int PlayerInfos = 0;
		int SwitchStates = 0;
		for(int i = 0; i < pSnap->NumItems(); ++i)
		{
			const CSnapshotItem *pItem = pSnap->GetItem(i);
			if(pSnap->GetItemType(i) == (Sixup ? (int)protocol7::NETOBJTYPE_PLAYERINFO : (int)NETOBJTYPE_PLAYERINFO))
			{
				const int Score = Sixup ? ((const protocol7::CNetObj_PlayerInfo *)pItem->Data())->m_Score : ((const CNetObj_PlayerInfo *)pItem->Data())->m_Score;
				EXPECT_EQ(Score, 0);
				++PlayerInfos;
			}
			if(pSnap->GetItemType(i) == NETOBJTYPE_GAMEINFOEX)
			{
				EXPECT_EQ(((const CNetObj_GameInfoEx *)pItem->Data())->m_Flags & GAMEINFOFLAG_TIMESCORE, 0);
			}
			if(pSnap->GetItemType(i) == NETOBJTYPE_DDNETPLAYER)
			{
				EXPECT_EQ(((const CNetObj_DDNetPlayer *)pItem->Data())->m_FinishTimeSeconds, FinishTime::UNSET);
			}
			if(pSnap->GetItemType(i) == NETOBJTYPE_SWITCHSTATE)
			{
				EXPECT_EQ(pItem->Id(), Team);
				EXPECT_EQ(((const CNetObj_SwitchState *)pItem->Data())->m_aStatus[0] & (1 << 1), 0);
				++SwitchStates;
			}
		}
		EXPECT_EQ(PlayerInfos, 2);
		EXPECT_EQ(SwitchStates, 1);
	}
	EXPECT_EQ(pController->GameFlags() & protocol7::GAMEFLAG_RACE, 0);

	// Disconnecting the leader promotes one member without killing the group.
	CServer::DelClientCallback(Dee, "test", m_pServer);
	EXPECT_EQ(GameServer()->m_apPlayers[Dee], nullptr);
	for(int ClientId : {Anna, Wilson, Ed})
	{
		ASSERT_NE(GameServer()->GetPlayerChar(ClientId), nullptr);
		EXPECT_EQ(pController->Teams().ScoreboardTeam(ClientId), Team);
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, GroupPosition);
	}
	EXPECT_TRUE(GameServer()->m_apPlayers[Ed]->IsNameMarked());
	GameServer()->m_apPlayers[Wilson]->KillCharacter(WEAPON_SELF);
	ASSERT_NE(GameServer()->GetPlayerChar(Anna), nullptr);
	ASSERT_NE(GameServer()->GetPlayerChar(Ed), nullptr);
	// Group death permission follows the promoted leader.
	GameServer()->m_apPlayers[Ed]->KillCharacter(WEAPON_SELF);
	for(int ClientId : {Anna, Wilson, Ed})
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId), nullptr);
	pController->Tick();
	EXPECT_EQ(TrainingGoal(Anna), TrainingGoal(Ed));
}

TEST_F(GameWorld, ChatFlagFilterCountsLiveChatTicksAndMessageCharacters)
{
	PrepareTrainingCorridor();
	g_Config.m_SvRequireChatFlagToChat = 1;
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Speaker", "Observer"});
	const int Speaker = vIds[0], Observer = vIds[1];
	CPlayer *pPlayer = GameServer()->m_apPlayers[Speaker];
	pPlayer->m_PlayerFlags = 0;
	m_pServer->m_avChatMessages[Speaker].clear();
	m_pServer->m_avChatMessages[Observer].clear();
	Say(Speaker, "hi");
	EXPECT_TRUE(m_pServer->m_avChatMessages[Observer].empty());
	ASSERT_FALSE(m_pServer->m_avChatMessages[Speaker].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Speaker].back(), "You are not allowed to use the chat yet. Please wait 20 seconds.");
	pPlayer->m_PlayerFlags = PLAYERFLAG_CHATTING;
	pPlayer->Tick();
	EXPECT_EQ(pPlayer->m_TicksSpentChatting, 1);
	Say(Speaker, "hi");
	EXPECT_TRUE(m_pServer->m_avChatMessages[Observer].empty());
	pPlayer->Tick();
	EXPECT_EQ(pPlayer->m_TicksSpentChatting, 2);
	Say(Speaker, "hi");
	ASSERT_FALSE(m_pServer->m_avChatMessages[Observer].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Observer].back(), "hi");
	// Thresholds use UTF-8 characters, not bytes, and include exactly ten.
	Say(Speaker, "éééééééééé");
	EXPECT_EQ(m_pServer->m_avChatMessages[Observer].back(), "éééééééééé");
	m_pServer->m_avChatMessages[Observer].clear();
	Say(Speaker, "ééééééééééé");
	EXPECT_TRUE(m_pServer->m_avChatMessages[Observer].empty());
	for(int i = 2; i < 20; ++i)
		pPlayer->Tick();
	EXPECT_EQ(pPlayer->m_TicksSpentChatting, 20);
	pPlayer->m_PlayerFlags = 0;
	pPlayer->Tick();
	Say(Speaker, "hello world");
	ASSERT_FALSE(m_pServer->m_avChatMessages[Observer].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Observer].back(), "hello world");
	pPlayer->Reset();
	EXPECT_EQ(pPlayer->m_TicksSpentChatting, 0);
}

TEST_F(GameWorld, ChatFlagFilterAllowsSpectatorsAndBindsAfterTwentySeconds)
{
	PrepareTrainingCorridor();
	g_Config.m_SvRequireChatFlagToChat = 1;
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Spectator", "Observer"});
	const int Speaker = vIds[0], Observer = vIds[1];
	CPlayer *pPlayer = GameServer()->m_apPlayers[Speaker];
	pPlayer->SetTeam(TEAM_SPECTATORS, false);
	pPlayer->m_PlayerFlags = PLAYERFLAG_CHATTING;
	for(int i = 0; i < 20; ++i)
		pPlayer->Tick();
	EXPECT_EQ(pPlayer->m_TicksSpentChatting, 0); // no live character
	m_pServer->m_avChatMessages[Observer].clear();
	m_pServer->AdvanceTicks(20 * m_pServer->TickSpeed() - 1);
	Say(Speaker, "hi");
	EXPECT_TRUE(m_pServer->m_avChatMessages[Observer].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Speaker].back(), "You are not allowed to use the chat yet. Please wait 1 seconds.");
	m_pServer->AdvanceTicks(1);
	Say(Speaker, "a spectator chat bind");
	ASSERT_FALSE(m_pServer->m_avChatMessages[Observer].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Observer].back(), "a spectator chat bind");
}

TEST_F(GameWorld, ChatFlagFilterIsOptionalAndLeavesCommandsAvailable)
{
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Speaker", "Observer"});
	const int Speaker = vIds[0], Observer = vIds[1];
	g_Config.m_SvRequireChatFlagToChat = 0;
	m_pServer->m_avChatMessages[Observer].clear();
	Say(Speaker, "a chat bind immediately after joining");
	ASSERT_FALSE(m_pServer->m_avChatMessages[Observer].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Observer].back(), "a chat bind immediately after joining");
	g_Config.m_SvRequireChatFlagToChat = 1;
	Say(Speaker, "/fight Observer");
	GameServer()->m_pController->Tick();
	EXPECT_TRUE(GameServer()->m_apPlayers[Observer]->IsNameMarked());
	m_pServer->m_avChatMessages[Speaker].clear();
	Say(Speaker, "team chat", true);
	ASSERT_FALSE(m_pServer->m_avChatMessages[Speaker].empty());
	EXPECT_NE(m_pServer->m_avChatMessages[Speaker].back().find("You are not allowed to use the chat yet."), std::string::npos);
	Say(Observer, "/freeplay");
	EXPECT_EQ(TrainingMarkers(Observer), 0);
	EXPECT_EQ(TrainingMarkers(Speaker), 0);
}

TEST_F(GameWorld, GTrainScoreboardWinsAndLeaderNamesKeepCanonicalServerNames)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Viewer"});
	const int Anna = vIds[0], Bob = vIds[1], Viewer = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	for(int ClientId : {Anna, Viewer})
	{
		m_pServer->m_aClients[ClientId].m_Sixup = true;
		GameServer()->m_PlayerMapping.InitPlayerMap(ClientId);
	}
	for(int ClientId : {Anna, Viewer})
	{
		for(int Other : vIds)
			GameServer()->m_PlayerMapping.ForceInsertPlayer(Other, ClientId);
		m_pServer->m_avClientIdentities[ClientId].clear();
	}
	pController->Fight(Bob, "Anna");
	pController->Tick();
	char aName[MAX_NAME_LENGTH];
	GameServer()->m_apPlayers[Anna]->GetDisplayName(aName, sizeof(aName));
	EXPECT_STREQ(aName, "*Anna*");
	EXPECT_STREQ(m_pServer->ClientName(Anna), "Anna");
	EXPECT_EQ(GameServer()->FindClientIdByName("Anna"), Anna);
	EXPECT_EQ(GameServer()->FindClientIdByName("*Anna*"), Anna);
	for(int ClientId : {Anna, Viewer})
	{
		SCOPED_TRACE(ClientId);
		int MappedAnna = Anna;
		ASSERT_TRUE(m_pServer->Translate(MappedAnna, ClientId));
		bool Found = false;
		for(const auto &Info : m_pServer->m_avClientIdentities[ClientId])
			if(Info.m_ClientId == MappedAnna && Info.m_Name == "*Anna*")
			{
				EXPECT_EQ(Info.m_Local, ClientId == Anna);
				Found = true;
			}
		EXPECT_TRUE(Found); // 0.7 names update for the leader and observers
	}
	for(int Winner : {Bob, Bob, Anna})
	{
		for(int ClientId : {Anna, Bob})
			GameServer()->GetPlayerChar(ClientId)->Unfreeze();
		pController->Tick();
		CCharacter *pChr = GameServer()->GetPlayerChar(Winner);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(Winner);
		pChr->SetPosition(pChr->m_Pos);
		pController->Tick();
	}
	EXPECT_EQ(pController->SnapPlayerScore(Anna, GameServer()->m_apPlayers[Anna]), 1);
	EXPECT_EQ(pController->SnapPlayerScore(Bob, GameServer()->m_apPlayers[Bob]), 2);
	EXPECT_EQ(m_pServer->m_aClients[Anna].m_Score, 1);
	EXPECT_EQ(m_pServer->m_aClients[Bob].m_Score, 2);
	for(bool Sixup : {false, true})
	{
		m_pServer->m_aClients[Viewer].m_Sixup = Sixup;
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init(Sixup);
		for(int ClientId : {Anna, Bob})
			GameServer()->m_apPlayers[ClientId]->Snap(Viewer);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		int Infos = 0;
		for(int i = 0; i < pSnap->NumItems(); ++i)
		{
			const auto *pItem = pSnap->GetItem(i);
			int MappedAnna = Anna;
			ASSERT_TRUE(m_pServer->Translate(MappedAnna, Viewer));
			const bool Leader = pItem->Id() == MappedAnna;
			if(pSnap->GetItemType(i) == (Sixup ? (int)protocol7::NETOBJTYPE_PLAYERINFO : (int)NETOBJTYPE_PLAYERINFO))
			{
				const int Score = Sixup ? ((const protocol7::CNetObj_PlayerInfo *)pItem->Data())->m_Score : ((const CNetObj_PlayerInfo *)pItem->Data())->m_Score;
				EXPECT_EQ(Score, Leader ? 1 : 2);
				++Infos;
			}
			if(!Sixup && pSnap->GetItemType(i) == NETOBJTYPE_CLIENTINFO)
			{
				const auto *pInfo = (const CNetObj_ClientInfo *)pItem->Data();
				EXPECT_TRUE(IntsToStr(pInfo->m_aName, std::size(pInfo->m_aName), aName, sizeof(aName)));
				EXPECT_STREQ(aName, Leader ? "*Anna*" : "Bob");
			}
		}
		EXPECT_EQ(Infos, 2);
	}
	// The actual server-info packet keeps the original player name.
	m_pServer->m_aClients[Anna].m_DebugDummy = false;
	CServer::CCache Cache;
	m_pServer->CacheServerInfo(&Cache, SERVERINFO_EXTENDED, true);
	bool FoundCanonical = false;
	for(const auto &Chunk : Cache.m_vCache)
	{
		const std::string Data((const char *)Chunk.m_vData.data(), Chunk.m_vData.size());
		EXPECT_EQ(Data.find("*Anna*"), std::string::npos);
		FoundCanonical |= Data.find(std::string("Anna\0", 5)) != std::string::npos;
	}
	EXPECT_TRUE(FoundCanonical);
	m_pServer->m_aClients[Anna].m_DebugDummy = true;

	// Leaving transfers the leader decoration and resets only the leaver.
	m_pServer->m_aClients[Viewer].m_Sixup = true;
	pController->Fight(Viewer, "*Anna*");
	pController->Tick();
	pController->Fight(Anna, "");
	pController->Tick();
	EXPECT_FALSE(GameServer()->m_apPlayers[Anna]->IsNameMarked());
	EXPECT_EQ(pController->SnapPlayerScore(Anna, GameServer()->m_apPlayers[Anna]), 0);
	EXPECT_EQ(pController->SnapPlayerScore(Bob, GameServer()->m_apPlayers[Bob]), 2);
	const int Leader = GameServer()->m_apPlayers[Bob]->IsNameMarked() ? Bob : Viewer;
	GameServer()->m_apPlayers[Leader]->GetDisplayName(aName, sizeof(aName));
	EXPECT_EQ(aName[0], '*');
	EXPECT_EQ(aName[str_length(aName) - 1], '*');
	EXPECT_STREQ(m_pServer->ClientName(Anna), "Anna");
}

TEST_F(GameWorld, GTrainGoalsAndPathsRespectStoppers)
{
	PrepareTrainingCorridor();
	CCollision *pCollision = GameServer()->Collision();
	// A one-way stopper in the corridor that only lets tees pass to the right.
	const int StopperX = pCollision->GetWidth() / 2;
	const int Stopper = pCollision->GetPureMapIndex(vec2(StopperX * 32 + 16, 80));
	auto *pTiles = const_cast<CTile *>(pCollision->GameLayer());
	pTiles[Stopper].m_Index = TILE_STOP;
	pTiles[Stopper].m_Flags = ROTATION_90;
	ASSERT_EQ(pCollision->GetTileMoveRestrictions(Stopper, false), CANTMOVE_LEFT);
	ASSERT_EQ(pCollision->GetTileMoveRestrictions(Stopper, true), CANTMOVE_LEFT);
	g_Config.m_SvGtrainGoalDistance = 100;
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	GameServer()->m_apPlayers[Runner]->m_ShowAll = true;
	bool Crossed = false;
	for(int Attempt = 0; Attempt < 40; ++Attempt)
	{
		const float StartX = GameServer()->GetPlayerChar(Runner)->m_Pos.x;
		const float GoalX = TrainingGoal(Runner).x;
		const float StopperCenter = StopperX * 32 + 16;
		// Goals are never behind the stopper, and paths never cross it leftwards.
		if(StartX >= StopperCenter)
		{
			EXPECT_GT(GoalX, StopperCenter);
		}
		Crossed |= StartX < StopperCenter && GoalX > StopperCenter;
		GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
		pController->Tick();
	}
	EXPECT_TRUE(Crossed); // passing in the stopper's direction still works
}

TEST_F(GameWorld, GTrainPathsFallThroughFreezeOntoUnfreeze)
{
	PrepareTrainingCorridor();
	ClearMapExtras();
	CCollision *pCollision = GameServer()->Collision();
	CLayers *pLayers = GameServer()->Layers();
	ASSERT_NE(pLayers->FrontLayer(), nullptr);
	auto *pFront = static_cast<CTile *>(pLayers->Map()->GetData(pLayers->FrontLayer()->m_Front));
	const int Width = pCollision->GetWidth();
	const int Shaft = Width / 2;
	ASSERT_GE(pCollision->GetHeight(), 8);
	g_Config.m_SvGtrainGoalDistance = 2 * Width;
	int Runner = -1;
	// An upper corridor from the start line above freeze, with a shaft in
	// its middle down to a lower corridor, which leads to the finish. The
	// farthest goal from the left of the upper corridor is in the lower one
	// when the shaft's tiles let tees fall into it.
	const auto GoalFromTheLeft = [&](int Tile, int FrontTile, int UnfreezeTile, int UnfreezeFrontTile) {
		if(Runner >= 0)
			GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
		for(int y = 0; y < pCollision->GetHeight(); ++y)
			for(int x = 0; x < Width; ++x)
			{
				pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, TILE_SOLID);
				pFront[y * Width + x].m_Index = TILE_AIR;
			}
		for(int x = 1; x < Width - 1; ++x)
		{
			pCollision->SetCollisionAt(x * 32 + 16, 48, TILE_AIR);
			pCollision->SetCollisionAt(x * 32 + 16, 80, TILE_FREEZE);
			pCollision->SetCollisionAt(x * 32 + 16, 176, TILE_AIR);
			pCollision->SetCollisionAt(x * 32 + 16, 208, TILE_FREEZE);
		}
		pCollision->SetCollisionAt(Shaft * 32 + 16, 112, Tile);
		pFront[3 * Width + Shaft].m_Index = FrontTile;
		pCollision->SetCollisionAt(Shaft * 32 + 16, 144, UnfreezeTile);
		pFront[4 * Width + Shaft].m_Index = UnfreezeFrontTile;
		pCollision->SetCollisionAt(48, 48, TILE_START);
		pCollision->SetCollisionAt((Width - 2) * 32 + 16, 176, TILE_FINISH);
		delete GameServer()->m_pController;
		GameServer()->m_pController = new CGameControllerGTrain(GameServer());
		if(Runner < 0)
			Runner = SpawnTrainingPlayers({"Runner"})[0];
		for(int Attempt = 0; Attempt < 200; ++Attempt)
		{
			if(CCharacter *pChr = GameServer()->GetPlayerChar(Runner); pChr && pChr->m_Pos.y == 48 && pChr->m_Pos.x < Shaft * 32)
				return TrainingGoal(Runner);
			GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
			GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
			GameServer()->m_pController->Tick();
		}
		ADD_FAILURE() << "No start left of the shaft";
		return vec2(0, 0);
	};

	// Freeze and unfreeze of either layer.
	EXPECT_EQ(GoalFromTheLeft(TILE_FREEZE, TILE_AIR, TILE_UNFREEZE, TILE_AIR).y, 176);
	// The path shows while falling through the freeze too.
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
	pChr->Unfreeze();
	GameServer()->m_pController->Tick();
	pChr->m_Pos = pChr->m_PrevPos = vec2(Shaft * 32 + 16, 112);
	pChr->SetPosition(pChr->m_Pos);
	GameServer()->m_pController->Tick();
	GameServer()->m_apPlayers[Runner]->m_ShowAll = true;
	EXPECT_GT(TrainingMarkers(Runner), 1);
	EXPECT_EQ(GoalFromTheLeft(TILE_AIR, TILE_FREEZE, TILE_AIR, TILE_UNFREEZE).y, 176);
	// Deep freeze needs more than unfreeze, stoppers stop the fall.
	EXPECT_EQ(GoalFromTheLeft(TILE_DFREEZE, TILE_AIR, TILE_UNFREEZE, TILE_AIR).y, 48);
	EXPECT_EQ(GoalFromTheLeft(TILE_FREEZE, TILE_STOPA, TILE_UNFREEZE, TILE_AIR).y, 48);
	// So does landing in freeze.
	EXPECT_EQ(GoalFromTheLeft(TILE_FREEZE, TILE_AIR, TILE_FREEZE, TILE_AIR).y, 48);
}

TEST_F(GameWorld, GTrainCaptureCountsFailsAtTheSamePosition)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Runner", "Anna", "Bob"});
	const int Runner = vIds[0], Anna = vIds[1], Bob = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Capture = [&](int ClientId) {
		for(int Other : vIds)
			if(CCharacter *pChr = GameServer()->GetPlayerChar(Other))
				pChr->Unfreeze();
		pController->Tick();
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(ClientId);
		pChr->SetPosition(pChr->m_Pos);
		CMemoryLogger Logger;
		{
			CLogScope Scope(&Logger);
			pController->Tick();
		}
		return Logger.ConcatenatedLines();
	};

	// Every retry of the same start and goal is a fail, also after dying.
	const vec2 Goal = TrainingGoal(Runner);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Runner);
	pController->Tick();
	EXPECT_EQ(TrainingGoal(Runner), Goal);
	EXPECT_NE(Capture(Runner).find("'Runner' captured the flag in 0.00 seconds (2 fails)."), std::string::npos);
	// The capture's new position starts without fails, as does a kill.
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	EXPECT_NE(Capture(Runner).find("'Runner' captured the flag in 0.00 seconds (1 fail)."), std::string::npos);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
	GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	EXPECT_NE(Capture(Runner).find("'Runner' captured the flag in 0.00 seconds (0 fails)."), std::string::npos);

	// Fight members respawning at their fight's position fail as well.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	GameServer()->m_apPlayers[Bob]->KillCharacter(WEAPON_SELF);
	GameServer()->m_apPlayers[Bob]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Bob);
	EXPECT_NE(Capture(Bob).find("'Bob' captured the flag (2 fails). Fight score (first to 10): 'Anna': 0, 'Bob': 1"), std::string::npos);
	EXPECT_NE(Capture(Anna).find("'Anna' captured the flag (0 fails)."), std::string::npos);
}

TEST_F(GameWorld, GTrainAttemptsStartWithoutPickups)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Runner", "Anna", "Bob"});
	const int Runner = vIds[0], Anna = vIds[1], Bob = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Pickup = [&](int ClientId) {
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->GiveWeapon(WEAPON_SHOTGUN);
		pChr->GiveWeapon(WEAPON_LASER);
		pChr->SetJetpack(true);
		pChr->SetEndlessHook(true);
		pChr->SetEndlessJump(true);
		pChr->SetCollisionDisabled(true);
	};
	const auto HasPickups = [&](int ClientId) {
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		const CCharacterCore *pCore = pChr->Core();
		return pChr->GetWeaponGot(WEAPON_SHOTGUN) || pChr->GetWeaponGot(WEAPON_LASER) ||
		       pCore->m_Jetpack || pCore->m_EndlessHook || pCore->m_EndlessJump || pCore->m_CollisionDisabled;
	};
	const auto Pos = [&](int ClientId) { return GameServer()->GetPlayerChar(ClientId)->m_Pos; };
	const auto DeathEvents = [&](vec2 Where) {
		GameServer()->m_apPlayers[Runner]->m_ViewPos = Where;
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init();
		GameServer()->m_Events.Snap(Runner);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		int Deaths = 0;
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
			Deaths += pSnap->GetItemType(i) == NETEVENTTYPE_DEATH;
		return Deaths;
	};
	const auto Capture = [&](int ClientId) {
		for(int Other : vIds)
			if(CCharacter *pChr = GameServer()->GetPlayerChar(Other))
				pChr->Unfreeze();
		pController->Tick();
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(ClientId);
		pChr->SetPosition(pChr->m_Pos);
		CMemoryLogger Logger;
		{
			CLogScope Scope(&Logger);
			pController->Tick();
		}
		return Logger.ConcatenatedLines();
	};

	const auto Shots = [&](int ClientId) {
		int Num = 0;
		for(int Type : {CGameWorld::ENTTYPE_PROJECTILE, CGameWorld::ENTTYPE_LASER})
			for(CEntity *pEntity = GameServer()->m_World.FindFirst(Type); pEntity; pEntity = pEntity->TypeNext())
				Num += pEntity->GetOwnerId() == ClientId;
		return Num;
	};

	// /r starts over like a kill, only at the same spot and flag. Kept
	// weapons stay, shots are gone, and there is no death splash.
	m_pServer->Console()->ExecuteLineFlag("grenade", CFGFLAG_CHAT, Runner);
	const vec2 Start = Pos(Runner);
	const vec2 Goal = TrainingGoal(Runner);
	Pickup(Runner);
	for(int ClientId : {Runner, Anna})
	{
		new CProjectile(&GameServer()->m_World, WEAPON_GRENADE, ClientId, Pos(ClientId), vec2(0, -1), 100, false, true, SOUND_GRENADE_EXPLODE, vec2(0, -1));
		new CLaser(&GameServer()->m_World, Pos(ClientId), vec2(1, 0), 200, ClientId, WEAPON_SHOTGUN);
		ASSERT_EQ(Shots(ClientId), 2);
	}
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
	pChr->SetPosition(Start + vec2(64, 0));
	pChr->m_Pos = Start + vec2(64, 0);
	ASSERT_TRUE(HasPickups(Runner));
	GameServer()->m_Events.Clear();
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	EXPECT_FALSE(HasPickups(Runner));
	EXPECT_TRUE(GameServer()->GetPlayerChar(Runner)->GetWeaponGot(WEAPON_GRENADE));
	EXPECT_EQ(Shots(Runner), 0);
	EXPECT_EQ(Shots(Anna), 2);
	EXPECT_EQ(Pos(Runner), Start);
	EXPECT_EQ(TrainingGoal(Runner), Goal);
	EXPECT_EQ(DeathEvents(Start + vec2(64, 0)), 0);
	pController->Tick();
	EXPECT_FALSE(HasPickups(Runner));
	EXPECT_EQ(Pos(Runner), Start);

	// The next spot after a capture starts over as well.
	Pickup(Runner);
	EXPECT_NE(Capture(Runner).find("'Runner' captured the flag in 0.00 seconds (1 fail)."), std::string::npos);
	EXPECT_FALSE(HasPickups(Runner));
	EXPECT_TRUE(GameServer()->GetPlayerChar(Runner)->GetWeaponGot(WEAPON_GRENADE));
	EXPECT_EQ(DeathEvents(Goal), 0);
	// Kills still show.
	const vec2 KillPos = Pos(Runner);
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
	EXPECT_EQ(DeathEvents(KillPos), 1);
	GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
	pController->Tick();

	// In a fight, /r only starts over for its user, the leader included.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	const vec2 FightGoal = TrainingGoal(Anna);
	Pickup(Anna);
	Pickup(Bob);
	const vec2 AnnaPos = Pos(Anna) + vec2(32, 0);
	GameServer()->GetPlayerChar(Anna)->SetPosition(AnnaPos);
	GameServer()->GetPlayerChar(Anna)->m_Pos = AnnaPos;
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Bob);
	pController->Tick();
	EXPECT_FALSE(HasPickups(Bob));
	EXPECT_TRUE(HasPickups(Anna));
	EXPECT_EQ(Pos(Anna), AnnaPos);
	Pickup(Bob);
	const vec2 BobPos = Pos(Bob) + vec2(32, 0);
	GameServer()->GetPlayerChar(Bob)->SetPosition(BobPos);
	GameServer()->GetPlayerChar(Bob)->m_Pos = BobPos;
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Anna);
	pController->Tick();
	EXPECT_FALSE(HasPickups(Anna));
	EXPECT_TRUE(HasPickups(Bob));
	EXPECT_EQ(Pos(Bob), BobPos);
	EXPECT_EQ(TrainingGoal(Anna), FightGoal);
	EXPECT_EQ(TrainingGoal(Bob), FightGoal);
}

TEST_F(GameWorld, GTrainCapturesEarnEloByDifficultySpeedAndGoalDistance)
{
	// Each star and difficulty is worth more, other maps earn nothing.
	EXPECT_EQ(CGameControllerGTrain::EloStep("Easy", 1), 0);
	EXPECT_EQ(CGameControllerGTrain::EloStep("Easy", 3), 2);
	EXPECT_EQ(CGameControllerGTrain::EloStep("Main", 1), 3);
	EXPECT_EQ(CGameControllerGTrain::EloStep("hard", 2), 7);
	EXPECT_EQ(CGameControllerGTrain::EloStep("Insane", 3), 11);
	EXPECT_EQ(CGameControllerGTrain::EloStep("Extreme", 3), 14);
	// Deranged maps have no stars, a rating doesn't change them. They count
	// as Extreme with five stars.
	EXPECT_EQ(CGameControllerGTrain::EloStep("Deranged", 0), 16);
	EXPECT_EQ(CGameControllerGTrain::EloStep("deranged", 2), 16);
	const std::pair<const char *, int> aWithoutElo[] = {{"Mod", 2}, {"Solo", 1}, {"Others", 3}, {"Novice", 1}, {"", 1}, {"Hard", 0}, {"Hard", 4}};
	for(const auto &[pServer, Stars] : aWithoutElo)
		EXPECT_FALSE(CGameControllerGTrain::EloStep(pServer, Stars)) << pServer << " " << Stars;

	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Runner", "Anna", "Bob"});
	const int Runner = vIds[0], Bob = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	// The corridor is one tile high, so goals are as many tiles away as they are wide.
	const auto GoalTiles = [&](int ClientId) {
		return (int)std::abs(TrainingGoal(ClientId).x - GameServer()->GetPlayerChar(ClientId)->m_Pos.x) / 32;
	};
	const auto Capture = [&](int ClientId, int Ticks = 0, bool Frozen = false) {
		for(int Other : vIds)
			if(CCharacter *pChr = GameServer()->GetPlayerChar(Other))
				pChr->Unfreeze();
		pController->Tick();
		m_pServer->AdvanceTicks(Ticks);
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(ClientId);
		pChr->SetPosition(pChr->m_Pos);
		if(Frozen)
			pChr->Freeze();
		CMemoryLogger Logger;
		{
			CLogScope Scope(&Logger);
			pController->Tick();
		}
		return Logger.ConcatenatedLines();
	};
	// The ELO a capture shows depends on the points the player had before.
	int64_t aPoints[MAX_CLIENTS] = {};
	const auto Gain = [&](int ClientId, int Step, int Tiles, double Seconds) {
		const int64_t Before = aPoints[ClientId];
		aPoints[ClientId] += GTrainElo::CapturePoints(Step, Tiles, Seconds);
		char aGain[64];
		str_format(aGain, sizeof(aGain), "+%.2f ELO, %.1fx speed", GTrainElo::FromPoints(aPoints[ClientId]) - GTrainElo::FromPoints(Before), GTrainElo::SpeedFactor(Step, Tiles, Seconds));
		return std::string(aGain);
	};
	const auto Help = [&]() {
		m_pServer->m_avChatMessages[Runner].clear();
		Say(Runner, "/help");
		return m_pServer->m_avChatMessages[Runner].back();
	};

	// Until the map's difficulty is known, captures earn nothing.
	std::string Messages = Capture(Runner);
	EXPECT_NE(Messages.find("'Runner' captured the flag in 0.00 seconds (0 fails)."), std::string::npos);
	EXPECT_EQ(Messages.find("ELO"), std::string::npos);

	// Instant captures are as fast as it gets.
	pController->SetMapDifficulty("Hard", 2);
	EXPECT_EQ(Help(), "Captures here are worth 17.1 times as much as on Easy ★☆☆ maps (Hard ★★☆), longer and faster ones more.");
	int Tiles = GoalTiles(Runner);
	ASSERT_GT(Tiles, 0);
	EXPECT_NE(Capture(Runner).find("(0 fails, " + Gain(Runner, 7, Tiles, 0) + ")."), std::string::npos);
	// Their speed bonus is the highest.
	EXPECT_EQ(aPoints[Runner], std::llround(100 * std::pow(Tiles / 100.0, GTrainElo::DISTANCE_EXPONENT) * GTrainElo::PointsPerTile(7) * GTrainElo::MAX_SPEED_FACTOR));
	// Longer goals earn more, retries don't change what a capture earns. The
	// corridor is too short for /distance, which takes at least 50 tiles.
	g_Config.m_SvGtrainGoalDistance = 6;
	// Starts close to the finish get shorter goals.
	for(int Attempt = 0; Attempt < 100 && Tiles != 6; ++Attempt)
	{
		GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
		pController->Tick();
		Tiles = GoalTiles(Runner);
	}
	ASSERT_EQ(Tiles, 6);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	EXPECT_NE(Capture(Runner).find("(1 fail, " + Gain(Runner, 7, Tiles, 0) + ")."), std::string::npos);

	// Frozen tees still capture, without ELO.
	const int Captures = pController->Stats(Runner).m_Captures;
	EXPECT_NE(Capture(Runner, 0, true).find("'Runner' captured the flag in 0.00 seconds (0 fails, +0 ELO (frozen))."), std::string::npos);
	EXPECT_EQ(pController->Stats(Runner).m_Captures, Captures + 1);
	EXPECT_EQ(pController->Stats(Runner).m_Points, aPoints[Runner]);

	// Kept weapons earn nothing either, also when they are gone again before
	// the capture.
	const auto Command = [&](const char *pCommand) { m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, Runner); };
	Command("ninja");
	EXPECT_NE(Capture(Runner).find("(0 fails, +0 ELO (kept weapons))."), std::string::npos);
	Command("ninja");
	GameServer()->GetPlayerChar(Runner)->Unfreeze();
	pController->Tick();
	Command("grenade");
	pController->Tick();
	Command("grenade");
	EXPECT_NE(Capture(Runner).find("(0 fails, +0 ELO (kept weapons))."), std::string::npos);
	EXPECT_EQ(pController->Stats(Runner).m_Captures, Captures + 3);
	EXPECT_EQ(pController->Stats(Runner).m_Points, aPoints[Runner]);
	// Weapons kept while hovering only, then given up, don't count.
	Command("laser");
	Command("laser");
	Tiles = GoalTiles(Runner);
	EXPECT_NE(Capture(Runner).find("(0 fails, " + Gain(Runner, 7, Tiles, 0) + ")."), std::string::npos);

	// Half the typical speed of an Extreme map is slow.
	pController->SetMapDifficulty("Extreme", 3);
	Tiles = GoalTiles(Runner);
	const int SlowTicks = (int)std::ceil(2 * Tiles / GTrainElo::EXTREME_TILES_PER_SECOND * (double)SERVER_TICK_SPEED);
	const double SlowSeconds = SlowTicks / (double)SERVER_TICK_SPEED;
	ASSERT_LT(GTrainElo::SpeedFactor(14, Tiles, SlowSeconds), 1);
	char aSlow[64];
	str_format(aSlow, sizeof(aSlow), "in %.2f seconds (0 fails, ", SlowSeconds);
	EXPECT_NE(Capture(Runner, SlowTicks).find(aSlow + Gain(Runner, 14, Tiles, SlowSeconds) + ")."), std::string::npos);

	// The capturer of a fight earns ELO, the others don't.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	Tiles = GoalTiles(Bob);
	Messages = Capture(Bob);
	EXPECT_NE(Messages.find("'Bob' captured the flag (0 fails, " + Gain(Bob, 14, Tiles, 0) + "). Fight score (first to 10): 'Anna': 0, 'Bob': 1"), std::string::npos);
	EXPECT_EQ(Messages.find("'Anna' captured"), std::string::npos);
	// Frozen fight captures score too.
	Messages = Capture(Bob, 0, true);
	EXPECT_NE(Messages.find("'Bob' captured the flag (0 fails, +0 ELO (frozen)). Fight score (first to 10): 'Anna': 0, 'Bob': 2"), std::string::npos);
	EXPECT_EQ(pController->Stats(Bob).m_Points, aPoints[Bob]);

	// Deranged maps have no stars and are worth what Extreme ★★★★★ would be.
	// Speed doesn't count there, and goals have at least 100 tiles.
	pController->SetMapDifficulty("Deranged", 0);
	EXPECT_EQ(Help(), "Captures here are worth 656.8 times as much as on Easy ★☆☆ maps (Deranged), longer ones more. Speed doesn't count, goals have at least 100 tiles.");
	const auto Reply = [&](const char *pMessage) {
		m_pServer->m_avChatMessages[Runner].clear();
		Say(Runner, pMessage);
		return m_pServer->m_avChatMessages[Runner].back();
	};
	EXPECT_EQ(Reply("/distance 60"), "The goal distance must be between 100 and 1000 tiles on Deranged maps, 0 for the server default.");
	EXPECT_EQ(Reply("/distance"), "Goal distance: 100 tiles (server default). Use /distance 100-1000 to change it, 0 for the server default.");
	// The goal from before the difficulty was known is shorter.
	EXPECT_NE(Capture(Runner).find("(0 fails, +0 ELO (goal under 100 tiles))."), std::string::npos);
	EXPECT_EQ(pController->Stats(Runner).m_Points, aPoints[Runner]);
	// New goals aim for 100 tiles, which the short corridor cuts. An instant
	// capture counts as typical speed.
	Tiles = GoalTiles(Runner);
	ASSERT_LT(Tiles, 100);
	Messages = Capture(Runner);
	EXPECT_NE(Messages.find("(0 fails, " + Gain(Runner, GTrainElo::DERANGED_STEP, Tiles, 0) + ")."), std::string::npos);
	EXPECT_NE(Messages.find(", 1.0x speed)."), std::string::npos);
	EXPECT_EQ(pController->Stats(Runner).m_Points, aPoints[Runner]);

	pController->SetMapDifficulty("Mod", 3);
	EXPECT_EQ(Help(), "Captures on this map earn no ELO.");
	Messages = Capture(Runner);
	EXPECT_NE(Messages.find("'Runner' captured the flag in 0.00 seconds (0 fails)."), std::string::npos);
	EXPECT_EQ(Messages.find("ELO"), std::string::npos);
}

TEST_F(GameWorld, GTrainStatsCountAttemptsRespawnsCapturesAndPlaytime)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Runner", "Anna", "Bob"});
	const int Runner = vIds[0], Anna = vIds[1], Bob = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Stats = [&](int ClientId) { return pController->Stats(ClientId); };
	// Ends the hover, then plays for the given time.
	const auto Play = [&](std::initializer_list<int> Ids, int Ticks) {
		for(int ClientId : Ids)
			GameServer()->GetPlayerChar(ClientId)->Unfreeze();
		pController->Tick();
		m_pServer->AdvanceTicks(Ticks);
	};
	const auto Kill = [&](int ClientId) {
		GameServer()->m_apPlayers[ClientId]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(80, 80));
		pController->Tick();
	};
	const auto Capture = [&](int ClientId) {
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(ClientId);
		pChr->SetPosition(pChr->m_Pos);
		pController->Tick();
	};
	const auto Retry = [&](int ClientId) { m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, ClientId); };
	constexpr int Second = SERVER_TICK_SPEED;

	// Attempts count after two seconds of play.
	Play({Runner}, 2 * Second - 1);
	Retry(Runner);
	EXPECT_EQ(Stats(Runner).m_Attempts, 0);
	Play({Runner}, 2 * Second);
	Retry(Runner);
	EXPECT_EQ(Stats(Runner).m_Attempts, 1);
	// Giving up a tried spot is a respawn, also while hovering after a retry.
	Kill(Runner);
	EXPECT_EQ(Stats(Runner).m_Respawns, 1);
	// Skipping a spot without trying it is none.
	Kill(Runner);
	EXPECT_EQ(Stats(Runner).m_Respawns, 1);
	// Captures always count as attempts, and their spots never as respawns.
	Play({Runner}, 0);
	Capture(Runner);
	EXPECT_EQ(Stats(Runner).m_Captures, 1);
	EXPECT_EQ(Stats(Runner).m_Attempts, 2);
	EXPECT_EQ(Stats(Runner).m_Respawns, 1);
	Play({Runner}, 3 * Second);
	Kill(Runner);
	EXPECT_EQ(Stats(Runner).m_Attempts, 3);
	EXPECT_EQ(Stats(Runner).m_Respawns, 2);

	// Fight members dying retry the spot, only the leader gives it up.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	Play({Anna, Bob}, 2 * Second);
	Kill(Bob);
	EXPECT_EQ(Stats(Bob).m_Attempts, 1);
	EXPECT_EQ(Stats(Bob).m_Respawns, 0);
	EXPECT_EQ(Stats(Anna).m_Attempts, 0);
	Kill(Anna);
	EXPECT_EQ(Stats(Anna).m_Attempts, 1);
	EXPECT_EQ(Stats(Anna).m_Respawns, 1);
	EXPECT_EQ(Stats(Bob).m_Attempts, 1);
	EXPECT_EQ(Stats(Bob).m_Respawns, 0);

	// Freeplay has no attempts.
	pController->Freeplay(Runner);
	Play({Runner}, 3 * Second);
	Kill(Runner);
	EXPECT_EQ(Stats(Runner).m_Attempts, 3);
	EXPECT_EQ(Stats(Runner).m_Respawns, 2);
	pController->Freeplay(Runner);

	// Playtime counts while playing, not while paused or spectating. Bob
	// leaves the fight first, his leader's death would kill him too.
	pController->Fight(Bob, "");
	pController->Tick();
	GameServer()->m_apPlayers[Anna]->SetTeam(TEAM_SPECTATORS);
	ASSERT_EQ(GameServer()->m_apPlayers[Bob]->Pause(CPlayer::PAUSE_PAUSED, true), CPlayer::PAUSE_PAUSED);
	pController->Tick();
	const int RunnerPlaytime = Stats(Runner).m_Playtime;
	const int AnnaPlaytime = Stats(Anna).m_Playtime;
	const int BobPlaytime = Stats(Bob).m_Playtime;
	for(int i = 0; i < 3 * Second; ++i)
		pController->Tick();
	EXPECT_EQ(Stats(Runner).m_Playtime, RunnerPlaytime + 3);
	EXPECT_EQ(Stats(Anna).m_Playtime, AnnaPlaytime);
	EXPECT_EQ(Stats(Bob).m_Playtime, BobPlaytime);
}

TEST_F(GameWorld, GTrainEloScoreboard)
{
	// ELO points are stored per name in the database, which runs jobs in order on its own thread.
	char aDatabase[64];
	ASSERT_LT(str_length(m_TestInfo.m_aFilename) + str_length("/elo.sqlite"), (int)sizeof(aDatabase));
	str_format(aDatabase, sizeof(aDatabase), "%s/elo.sqlite", m_TestInfo.m_aFilename);
	m_pServer->DbPool()->RegisterSqliteDatabase(CDbConnectionPool::READ, aDatabase);
	m_pServer->DbPool()->RegisterSqliteDatabase(CDbConnectionPool::WRITE, aDatabase);
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	g_Config.m_SvSqlQueriesDelay = 0;
	const auto vIds = SpawnTrainingPlayers({"Runner", "Anna", "Bob"});
	const int Runner = vIds[0], Anna = vIds[1], Bob = vIds[2];
	// Players only get query results after their first snapshots.
	for(int ClientId : vIds)
		GameServer()->m_apPlayers[ClientId]->m_SentSnaps = 3;
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Score = [&](int ClientId) { return pController->SnapPlayerScore(ClientId, GameServer()->m_apPlayers[ClientId]); };
	const auto Elo = [](int64_t Points) { return (int)GTrainElo::FromPoints(Points); };
	// A job queued last completes after all earlier ones.
	const auto WaitForDatabase = [&]() {
		auto pLast = std::make_shared<CScoreGTrainStatsResult>();
		GameServer()->Score()->LoadGTrainStats("Runner", pLast);
		for(int i = 0; i < 1000 && !pLast->m_Completed; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		EXPECT_TRUE(pLast->m_Completed);
		pController->Tick();
		for(int ClientId : vIds)
			GameServer()->m_apPlayers[ClientId]->Tick();
		return pLast->m_Success;
	};
	// Reads fail until the database is registered.
	bool Ready = false;
	for(int i = 0; i < 100 && !Ready; ++i)
		Ready = WaitForDatabase();
	ASSERT_TRUE(Ready);
	const auto GoalTiles = [&](int ClientId) {
		return (int)std::abs(TrainingGoal(ClientId).x - GameServer()->GetPlayerChar(ClientId)->m_Pos.x) / 32;
	};
	// Instant captures of an Extreme map with three stars.
	const auto Capture = [&](int ClientId) {
		for(int Other : vIds)
			if(CCharacter *pChr = GameServer()->GetPlayerChar(Other))
				pChr->Unfreeze();
		pController->Tick();
		const int64_t Points = GTrainElo::CapturePoints(GTrainElo::EXTREME_STEP, GoalTiles(ClientId), 0);
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(ClientId);
		pChr->SetPosition(pChr->m_Pos);
		pController->Tick();
		return Points;
	};
	const auto Rename = [&](int ClientId, const char *pName) {
		CNetMsg_Cl_ChangeInfo Msg = {};
		Msg.m_pName = pName;
		Msg.m_pClan = "";
		Msg.m_pSkin = "default";
		GameServer()->OnChangeInfoNetMessage(&Msg, ClientId);
		EXPECT_STREQ(m_pServer->ClientName(ClientId), pName);
	};

	const auto AddPoints = [&](const char *pName, int64_t Points) {
		CGTrainStats Stats;
		Stats.m_Points = Points;
		GameServer()->Score()->AddGTrainStats(pName, Stats);
	};

	// Points from earlier grinding load once the names are set.
	AddPoints("Runner", 2'000'000'000);
	AddPoints("Bob", 1'000'000'000);
	for(int ClientId : vIds)
		pController->LoadStats(ClientId);
	ASSERT_TRUE(WaitForDatabase());
	EXPECT_EQ(Score(Runner), Elo(2'000'000'000));
	EXPECT_GT(Score(Runner), 0);
	pController->SetMapDifficulty("Extreme", 3);
	int64_t RunnerPoints = 2'000'000'000 + Capture(Runner);
	EXPECT_EQ(Score(Runner), Elo(RunnerPoints));
	pController->Tick();
	EXPECT_EQ(m_pServer->m_aClients[Runner].m_Score, Elo(RunnerPoints));

	// Another name has its own ELO. Coming back loads the saved points, plus
	// what was gained while loading them.
	Rename(Runner, "Walker");
	EXPECT_EQ(Score(Runner), 0);
	ASSERT_TRUE(WaitForDatabase());
	EXPECT_EQ(Score(Runner), 0);
	Rename(Runner, "Runner");
	RunnerPoints += Capture(Runner);
	ASSERT_TRUE(WaitForDatabase());
	EXPECT_EQ(Score(Runner), Elo(RunnerPoints));
	pController->LoadStats(Runner);
	ASSERT_TRUE(WaitForDatabase());
	EXPECT_EQ(Score(Runner), Elo(RunnerPoints));
	EXPECT_EQ(pController->Stats(Runner).m_Captures, 2);
	EXPECT_EQ(pController->Stats(Runner).m_Attempts, 2);

	// Fighting players show their fight points, until they leave the fight.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(Score(Bob), 0);
	const int64_t BobPoints = 1'000'000'000 + Capture(Bob);
	EXPECT_EQ(Score(Bob), 1);
	EXPECT_EQ(Score(Anna), 0);
	pController->Fight(Bob, "");
	pController->Tick();
	EXPECT_EQ(Score(Bob), Elo(BobPoints));
	EXPECT_EQ(m_pServer->m_aClients[Bob].m_Score, Elo(BobPoints));

	// The server browser gets points rather than times.
	m_pServer->m_aClients[Bob].m_DebugDummy = false;
	CServer::CCache Cache;
	m_pServer->CacheServerInfo(&Cache, SERVERINFO_EXTENDED, true);
	m_pServer->m_aClients[Bob].m_DebugDummy = true;
	std::string Info;
	for(const auto &Chunk : Cache.m_vCache)
		Info.append((const char *)Chunk.m_vData.data(), Chunk.m_vData.size());
	EXPECT_NE(Info.find(std::string("Bob\0", 4)), std::string::npos);
	EXPECT_NE(Info.find('\0' + std::to_string(Elo(BobPoints)) + '\0'), std::string::npos);
	EXPECT_EQ(Info.find("-" + std::to_string(Elo(BobPoints))), std::string::npos);

	// /points and /top5points show the ELO from the database.
	m_pServer->AdvanceTicks(10);
	m_pServer->m_avChatMessages[Anna].clear();
	Say(Anna, "/points Runner");
	ASSERT_TRUE(WaitForDatabase());
	ASSERT_FALSE(m_pServer->m_avChatMessages[Anna].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Anna].back(), "1. Runner ELO: " + std::to_string(Elo(RunnerPoints)) + ", requested by Anna");
	m_pServer->AdvanceTicks(10);
	m_pServer->m_avChatMessages[Anna].clear();
	Say(Anna, "/top5elo");
	ASSERT_TRUE(WaitForDatabase());
	const auto &vTop = m_pServer->m_avChatMessages[Anna];
	ASSERT_EQ(vTop.size(), 4u);
	EXPECT_EQ(vTop[0], "-------- Top ELO --------");
	EXPECT_EQ(vTop[1], "1. Runner ELO: " + std::to_string(Elo(RunnerPoints)));
	EXPECT_EQ(vTop[2], "2. Bob ELO: " + std::to_string(Elo(BobPoints)));

	// /rank and /profile show all stats, captures are attempts as well.
	for(const char *pCommand : {"/rank Runner", "/profile Runner"})
	{
		m_pServer->AdvanceTicks(10);
		m_pServer->m_avChatMessages[Anna].clear();
		Say(Anna, pCommand);
		ASSERT_TRUE(WaitForDatabase());
		ASSERT_FALSE(m_pServer->m_avChatMessages[Anna].empty()) << pCommand;
		const std::string &Profile = m_pServer->m_avChatMessages[Anna].back();
		EXPECT_TRUE(Profile.starts_with("1. Runner ELO: " + std::to_string(Elo(RunnerPoints)) + ", 2 captures, 2 attempts, 0 respawns, ")) << Profile;
		EXPECT_TRUE(Profile.ends_with(" played, requested by Anna")) << Profile;
	}
	m_pServer->AdvanceTicks(10);
	m_pServer->m_avChatMessages[Anna].clear();
	Say(Anna, "/top5captures");
	ASSERT_TRUE(WaitForDatabase());
	ASSERT_EQ(vTop.size(), 4u);
	EXPECT_EQ(vTop[0], "-------- Top Captures --------");
	EXPECT_EQ(vTop[1], "1. Runner Captures: 2");
	EXPECT_EQ(vTop[2], "2. Bob Captures: 1");
	// /top5 has no race times to show either.
	m_pServer->AdvanceTicks(10);
	m_pServer->m_avChatMessages[Anna].clear();
	Say(Anna, "/top5");
	ASSERT_TRUE(WaitForDatabase());
	ASSERT_EQ(vTop.size(), 4u);
	EXPECT_EQ(vTop[0], "-------- Top ELO --------");
	EXPECT_EQ(vTop[1], "1. Runner ELO: " + std::to_string(Elo(RunnerPoints)));

	// ELO stops at its maximum.
	AddPoints("Anna", 1'000'000'000'000'000);
	pController->LoadStats(Anna);
	ASSERT_TRUE(WaitForDatabase());
	EXPECT_EQ(Score(Anna), GTrainElo::MAX_ELO);
}

TEST_F(GameWorld, GTrainChatOnlyHasGTrainCommands)
{
	IConsole *pConsole = m_pServer->Console();
	const auto IsChatCommand = [&](const char *pName) { return pConsole->GetCommandInfo(pName, CFGFLAG_CHAT, false) != nullptr; };
	ASSERT_TRUE(IsChatCommand("team"));
	ASSERT_TRUE(IsChatCommand("solo"));
	PrepareTrainingCorridor();
	// Races, DDRace teams and practice have no use in GTrain.
	for(const char *pName : {"practice", "save", "load", "kill", "times", "swap", "solo", "weapons", "unlaser", "tpxy", "invite", "join", "lock"})
		EXPECT_FALSE(IsChatCommand(pName)) << pName;
	for(const char *pName : {"r", "retry", "fight", "team", "accept", "a", "distance", "forwards", "trail", "settings", "laser", "rifle", "top5", "rank", "profile", "top5playtime", "spec", "pause", "w", "help", "cmdlist", "map", "timeout"})
		EXPECT_TRUE(IsChatCommand(pName)) << pName;
	// Practice binds stay, they retry.
	for(const char *pName : {"invincible", "tc", "telecursor", "rescue", "back", "tp", "teleport", "lasttp"})
		EXPECT_TRUE(IsChatCommand(pName)) << pName;
	// Rcon keeps them.
	EXPECT_NE(pConsole->GetCommandInfo("practice", CFGFLAG_SERVER, false), nullptr);
	// GTrain's /team takes player names.
	EXPECT_STREQ(pConsole->GetCommandInfo("team", CFGFLAG_CHAT, false)->Params(), "?r[player name]");
}

TEST_F(GameWorld, GTrainSettingsShowsThoseOfPlayersAndFights)
{
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Runner"});
	const int Anna = vIds[0], Bob = vIds[1], Runner = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Settings = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		Say(ClientId, pCommand);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};

	EXPECT_EQ(Settings(Anna, "/settings"), "Your settings: /trail on, /distance 3 (server default), /nodj off, /forwards off, /freeplay off, /scorelimit 10, kept weapons: none");
	for(const char *pCommand : {"/distance 120", "/nodj", "/forwards", "/ninja", "/shotgun", "/scorelimit 0", "/trail"})
		Say(Anna, pCommand);
	const std::string AnnaSettings = "/distance 120, /nodj on, /forwards on, /freeplay off, /scorelimit off, kept weapons: shotgun, ninja";
	EXPECT_EQ(Settings(Anna, "/settings"), "Your settings: /trail off, " + AnnaSettings);
	EXPECT_EQ(Settings(Runner, "/settings Anna"), "Settings of 'Anna': /trail off, " + AnnaSettings);
	EXPECT_EQ(Settings(Runner, "/settings Nobody"), "Player not found. Use their full name.");

	// In a fight, the leader's settings apply to everyone, except the trail.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(Settings(Bob, "/settings"), "Your settings: /trail on, from fight leader 'Anna': " + AnnaSettings);
	EXPECT_EQ(Settings(Runner, "/settings Bob"), "Settings of 'Bob': /trail on, from fight leader 'Anna': " + AnnaSettings);
	EXPECT_EQ(Settings(Runner, "/settings *Anna*"), "Settings of 'Anna' (fight leader): /trail off, " + AnnaSettings);
	EXPECT_EQ(Settings(Anna, "/settings"), "Your settings (fight leader): /trail off, " + AnnaSettings);
}

TEST_F(GameWorld, GTrainPracticeBindsRetry)
{
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	const vec2 Start = GameServer()->GetPlayerChar(Runner)->m_Pos;
	const vec2 Goal = TrainingGoal(Runner);
	for(const char *pCommand : {"/r", "/retry", "/tc", "/telecursor", "/invincible", "/rescue", "/back", "/tp", "/teleport Runner", "/lasttp"})
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
		pChr->SetPosition(Start + vec2(64, 0));
		pChr->m_Pos = Start + vec2(64, 0);
		Say(Runner, pCommand);
		EXPECT_EQ(GameServer()->GetPlayerChar(Runner)->m_Pos, Start) << pCommand;
		EXPECT_EQ(TrainingGoal(Runner), Goal) << pCommand;
	}
}

TEST_F(GameWorld, GTrainNeverPlacesOnUnfreezeTiles)
{
	// Unfreeze tiles would end the hover right away.
	PrepareTrainingCorridor();
	CCollision *pCollision = GameServer()->Collision();
	for(int x = 10; x <= 20; ++x)
		pCollision->SetCollisionAt(x * 32 + 16, 80, TILE_UNFREEZE);
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	const auto OnUnfreeze = [](vec2 Pos) { return Pos.x >= 10 * 32 && Pos.x < 21 * 32; };
	bool FoundBehind = false;
	for(int Attempt = 0; Attempt < 200; ++Attempt)
	{
		const vec2 Start = GameServer()->GetPlayerChar(Runner)->m_Pos;
		ASSERT_FALSE(OnUnfreeze(Start)) << Start.x;
		ASSERT_FALSE(OnUnfreeze(TrainingGoal(Runner))) << TrainingGoal(Runner).x;
		FoundBehind |= Start.x > 21 * 32;
		GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
		GameServer()->m_pController->Tick();
	}
	// The tiles still belong to the path.
	EXPECT_TRUE(FoundBehind);
}

TEST_F(GameWorld, GTrainSkipsTheStartOfTheMap)
{
	PrepareTrainingCorridor();
	// The start line is at x = 1, so x = 2 to 6 is within 5 tiles of it.
	g_Config.m_SvGtrainStartExclusion = 5;
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	const float FirstAllowed = 7 * 32 + 16;
	const auto NextAttempt = [&]() {
		GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
		GameServer()->m_pController->Tick();
	};
	bool FoundFirstAllowed = false;
	for(int Attempt = 0; Attempt < 1000 && !FoundFirstAllowed; ++Attempt)
	{
		const float StartX = GameServer()->GetPlayerChar(Runner)->m_Pos.x;
		// Goals behind such a start would be in the start area too.
		ASSERT_GE(StartX, FirstAllowed);
		ASSERT_GE(TrainingGoal(Runner).x, FirstAllowed);
		FoundFirstAllowed = StartX == FirstAllowed;
		NextAttempt();
	}
	EXPECT_TRUE(FoundFirstAllowed);

	// Maps entirely within the distance keep all their tiles. Characters
	// keep the controller's teams, so the runner must not outlive it.
	g_Config.m_SvGtrainStartExclusion = 10000;
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	bool FoundBesideStart = false;
	for(int Attempt = 0; Attempt < 1000 && !FoundBesideStart; ++Attempt)
	{
		NextAttempt();
		TrainingGoal(Runner);
		FoundBesideStart = GameServer()->GetPlayerChar(Runner)->m_Pos.x == 2 * 32 + 16;
	}
	EXPECT_TRUE(FoundBesideStart);
}

TEST_F(GameWorld, GTrainOnlyPlacesWithinTheRace)
{
	// Spots and goals farther from the start line than the finish line is,
	// or farther from the finish line than the start line is, are off the
	// race. The fill reaches past both lines through the freeze below.
	PrepareTrainingCorridor();
	CCollision *pCollision = GameServer()->Collision();
	const int Width = pCollision->GetWidth();
	ASSERT_GE(Width, 30);
	const int StartX = 10, FinishX = Width - 10;
	const auto Tile = [](int x, int y) { return vec2(x * 32 + 16, y * 32 + 16); };
	const auto SetTile = [&](int x, int y, int Index) { pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, Index); };
	SetTile(1, 2, TILE_AIR);
	SetTile(Width - 2, 2, TILE_AIR);
	SetTile(StartX, 2, TILE_START);
	SetTile(FinishX, 2, TILE_FINISH);
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	// Every spot, in rows, as starts and goals of many attempts.
	std::vector<vec2> vStarts, vGoals;
	const auto CollectSpots = [&]() {
		// Characters keep the controller's teams, they must not outlive it.
		GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
		delete GameServer()->m_pController;
		GameServer()->m_pController = new CGameControllerGTrain(GameServer());
		vStarts.clear();
		vGoals.clear();
		const auto Add = [](std::vector<vec2> &vSpots, vec2 Pos) {
			if(std::find(vSpots.begin(), vSpots.end(), Pos) == vSpots.end())
				vSpots.push_back(Pos);
		};
		for(int Attempt = 0; Attempt < 1500; ++Attempt)
		{
			GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
			GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
			GameServer()->m_pController->Tick();
			Add(vStarts, GameServer()->GetPlayerChar(Runner)->m_Pos);
			Add(vGoals, TrainingGoal(Runner));
		}
		const auto Order = [](vec2 A, vec2 B) { return A.y != B.y ? A.y < B.y : A.x < B.x; };
		std::sort(vStarts.begin(), vStarts.end(), Order);
		std::sort(vGoals.begin(), vGoals.end(), Order);
	};
	std::vector<vec2> vBetween;
	for(int x = StartX + 1; x < FinishX; ++x)
		vBetween.push_back(Tile(x, 2));

	// Behind the start, no path leads to the finish, and none leads past
	// the finish from the start.
	CollectSpots();
	EXPECT_EQ(vStarts, vBetween);
	EXPECT_EQ(vGoals, vBetween);

	// Around the lines, the tiles above them connect both sides. Going around
	// a line takes longer than passing it, so the tiles above the lines and
	// behind them are farther from the other line than this line is. Those
	// beside the race are not.
	for(int x : {StartX - 1, StartX, StartX + 1, FinishX - 1, FinishX, FinishX + 1})
		SetTile(x, 1, TILE_AIR);
	std::vector<vec2> vInRace = vBetween;
	vInRace.insert(vInRace.begin(), {Tile(StartX + 1, 1), Tile(FinishX - 1, 1)});
	CollectSpots();
	EXPECT_EQ(vStarts, vInRace);
	EXPECT_EQ(vGoals, vInRace);
}

TEST_F(GameWorld, GTrainRatesSpotsWhileEveryoneSpectates)
{
	PrepareTrainingCorridor();
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	ASSERT_TRUE(GameServer()->GetPlayerChar(Runner));
	g_Config.m_SvGtrainTileRatings = 1;
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
	delete GameServer()->m_pController;
	auto *pController = new CGameControllerGTrain(GameServer());
	GameServer()->m_pController = pController;
	ASSERT_TRUE(pController->RatingTiles());
	char aReason[128] = "";
	EXPECT_FALSE(pController->CanJoinTeam(TEAM_GAME, Runner, aReason, sizeof(aReason)));
	EXPECT_TRUE(str_find(aReason, "rated")) << aReason;

	// Everyone spectates with the progress shown, and plays once it is done.
	m_pServer->AdvanceTicks(SERVER_TICK_SPEED - m_pServer->Tick() % SERVER_TICK_SPEED);
	m_pServer->m_avBroadcasts[Runner].clear();
	for(int Wait = 0; Wait < 6000 && pController->RatingTiles(); ++Wait)
	{
		pController->Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_FALSE(pController->RatingTiles());
	EXPECT_EQ(GameServer()->m_apPlayers[Runner]->GetTeam(), TEAM_GAME);
	ASSERT_GE(m_pServer->m_avBroadcasts[Runner].size(), 2u);
	EXPECT_TRUE(str_find(m_pServer->m_avBroadcasts[Runner].front().c_str(), "Rating this map's spots")) << m_pServer->m_avBroadcasts[Runner].front();
	EXPECT_EQ(m_pServer->m_avBroadcasts[Runner].back(), "");
	EXPECT_TRUE(pController->CanJoinTeam(TEAM_GAME, Runner, aReason, sizeof(aReason)));

	// The ratings are saved for this version of the map, and loaded again.
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(GameServer()->Map()->Sha256(), aSha256, sizeof(aSha256));
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "gtrain_ratings/%s_%s.gtr", fs_filename(g_Config.m_SvMap), aSha256);
	EXPECT_TRUE(m_pStorage->FileExists(aPath, IStorage::TYPE_SAVE)) << aPath;
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
	delete GameServer()->m_pController;
	CMemoryLogger Logger;
	{
		CLogScope Scope(&Logger);
		GameServer()->m_pController = new CGameControllerGTrain(GameServer());
	}
	EXPECT_FALSE(static_cast<CGameControllerGTrain *>(GameServer()->m_pController)->RatingTiles());
	EXPECT_TRUE(str_find(Logger.ConcatenatedLines().c_str(), "loaded the ratings of")) << Logger.ConcatenatedLines();
}

TEST_F(GameWorld, GTrainCapturesEarnByTheirTiles)
{
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	CCollision *pCollision = GameServer()->Collision();
	const int Width = pCollision->GetWidth();
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	// The stars don't count with rated tiles, only that the category earns.
	pController->SetMapDifficulty("Easy", 1);
	const auto TileX = [](vec2 Pos) { return (int)(Pos.x / 32); };
	// A spot with two corridor tiles behind it, away from the goal.
	int Direction = 0;
	for(int Attempt = 0; Attempt < 1000; ++Attempt)
	{
		const int StartX = TileX(GameServer()->GetPlayerChar(Runner)->m_Pos);
		Direction = TileX(TrainingGoal(Runner)) > StartX ? 1 : -1;
		if(StartX - 2 * Direction >= 2 && StartX - 2 * Direction <= Width - 3)
			break;
		GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
		pController->Tick();
		Direction = 0;
	}
	ASSERT_NE(Direction, 0);
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
	const vec2 Start = pChr->m_Pos, Goal = TrainingGoal(Runner);
	const int Distance = std::abs(TileX(Goal) - TileX(Start));
	const vec2 Behind = Start - vec2(32.0f * Direction, 0), FarBehind = Start - vec2(64.0f * Direction, 0);
	const auto &aSteps = GTrainElo::TILE_STEP_DIFFICULTIES;
	std::vector<float> vDifficulties((size_t)Width * pCollision->GetHeight(), -1.0f);
	const auto Rate = [&](vec2 Pos, float Difficulty) { vDifficulties[pCollision->GetPureMapIndex(Pos)] = Difficulty; };
	Rate(Start, aSteps[6]);
	Rate(Behind, aSteps[8]);
	Rate(FarBehind, aSteps[14]); // only reached frozen
	Rate(Goal, aSteps[7]);
	pController->SetTileDifficulties(vDifficulties);

	const auto MoveTo = [&](vec2 Pos) {
		pChr->m_Pos = pChr->m_PrevPos = Pos;
		pChr->SetPosition(Pos);
		CMemoryLogger Logger;
		{
			CLogScope Scope(&Logger);
			pController->Tick();
		}
		return Logger.ConcatenatedLines();
	};
	pChr->Unfreeze();
	MoveTo(Start); // the release
	MoveTo(Behind);
	pChr->Freeze();
	MoveTo(FarBehind);
	pChr->Unfreeze();
	MoveTo(Behind);
	MoveTo(Start); // each tile counts once
	const std::string Messages = MoveTo(Goal);

	// The start, the tile behind it and the goal, about Hard with two stars.
	const double Step = GTrainElo::TileStep((aSteps[6] + aSteps[7] + aSteps[8]) / 3);
	EXPECT_NEAR(Step, 7, 0.1);
	const double Seconds = GTrainElo::MIN_SECONDS;
	char aExpected[128];
	str_format(aExpected, sizeof(aExpected), "(0 fails, +%.2f ELO, %.1fx speed, Hard ★★☆)",
		GTrainElo::FromPoints(GTrainElo::CapturePoints(Step, false, Distance, Seconds)), GTrainElo::SpeedFactor(Step, false, Distance, Seconds));
	EXPECT_TRUE(str_find(Messages.c_str(), aExpected)) << Messages << "\nexpected " << aExpected;
}

TEST_F(GameWorld, GTrainTileStepsFollowTheKoGRatings)
{
	// Each whole step is at its difficulty, steps between are interpolated,
	// those beyond extrapolated, at least Easy with one star and without an
	// end above Extreme with three.
	const auto &aSteps = GTrainElo::TILE_STEP_DIFFICULTIES;
	for(int Step = 0; Step <= GTrainElo::EXTREME_STEP; ++Step)
		EXPECT_NEAR(GTrainElo::TileStep(aSteps[Step]), Step, 1e-9);
	EXPECT_NEAR(GTrainElo::TileStep((aSteps[3] + aSteps[4]) / 2), 3.5, 1e-9);
	EXPECT_NEAR(GTrainElo::TileStep(aSteps[14] + (aSteps[14] - aSteps[13])), 15, 1e-9);
	EXPECT_NEAR(GTrainElo::TileStep(aSteps[14] + 6 * (aSteps[14] - aSteps[13])), 20, 1e-9);
	EXPECT_EQ(GTrainElo::TileStep(-100), 0);

	// Every category has three stars, Deranged as many as it takes.
	const std::pair<double, const char *> aLabels[] = {{-1.0, "Easy ★☆☆"}, {0.4, "Easy ★☆☆"}, {2.6, "Main ★☆☆"}, {7.0, "Hard ★★☆"},
		{14.0, "Extreme ★★★"}, {14.6, "Deranged ★☆☆"}, {16.0, "Deranged ★★☆"}, {17.0, "Deranged ★★★"}, {19.0, "Deranged ★★★★★"}};
	for(const auto &[Step, pLabel] : aLabels)
	{
		char aLabel[64];
		CGameControllerGTrain::FormatStep(aLabel, sizeof(aLabel), Step);
		EXPECT_STREQ(aLabel, pLabel) << Step;
	}
	// Whole steps earn what the categories and stars did.
	EXPECT_EQ(GTrainElo::CapturePoints(7.0, false, 100, 10), GTrainElo::CapturePoints(7, 100, 10));
	EXPECT_EQ(GTrainElo::CapturePoints(16.0, true, 100, 10), GTrainElo::CapturePoints(GTrainElo::DERANGED_STEP, 100, 10));
}

TEST_F(GameWorld, GTrainRaceTimerRestartsWithEveryAttempt)
{
	PrepareTrainingCorridor();
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	m_pServer->m_aClients[Runner].m_DDNetVersion = VERSION_DDNET_GAMETICK;
	GameServer()->m_apPlayers[Runner]->m_TimerType = CPlayer::TIMERTYPE_GAMETIMER;
	const auto SnappedStartTick = [&]() -> std::optional<int> {
		CSnapshotBuffer Buffer;
		m_pServer->m_SnapshotBuilder.Init();
		pController->Snap(Runner);
		m_pServer->m_SnapshotBuilder.Finish(&Buffer);
		const CSnapshot *pSnap = Buffer.AsSnapshot();
		for(int i = 0; i < pSnap->NumItems(); ++i)
		{
			if(pSnap->GetItemType(i) != NETOBJTYPE_GAMEINFO)
				continue;
			const auto *pInfo = (const CNetObj_GameInfo *)pSnap->GetItem(i)->Data();
			if(pInfo->m_GameStateFlags & GAMESTATEFLAG_RACETIME)
				return -pInfo->m_WarmupTimer;
		}
		return std::nullopt;
	};

	// The timer stays at zero while hovering, then counts the attempt.
	EXPECT_EQ(SnappedStartTick(), m_pServer->Tick());
	m_pServer->AdvanceTicks(20);
	EXPECT_EQ(SnappedStartTick(), m_pServer->Tick());
	CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
	pChr->Unfreeze();
	pController->Tick();
	const int StartTick = m_pServer->Tick();
	m_pServer->AdvanceTicks(3 * m_pServer->TickSpeed());
	EXPECT_EQ(SnappedStartTick(), StartTick);

	// Retrying and killing both start the timer from zero again.
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Runner);
	EXPECT_EQ(SnappedStartTick(), m_pServer->Tick());
	pChr->Unfreeze();
	pController->Tick();
	m_pServer->AdvanceTicks(m_pServer->TickSpeed());
	EXPECT_EQ(pController->RaceTimerStartTick(pChr), m_pServer->Tick() - m_pServer->TickSpeed());
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
	pChr = GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
	EXPECT_EQ(SnappedStartTick(), m_pServer->Tick()); // placement is still pending
	pController->Tick();
	EXPECT_EQ(SnappedStartTick(), m_pServer->Tick());
	EXPECT_EQ(pChr->m_DDRaceState, ERaceState::CHEATED); // the finish line still never counts
}

TEST_F(GameWorld, GTrainGoalDistanceIsPersonalAndSetByFightLeaders)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob"});
	const int Anna = vIds[0], Bob = vIds[1];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto GoalDistance = [&](int ClientId) {
		return distance(GameServer()->GetPlayerChar(ClientId)->m_Pos, TrainingGoal(ClientId)) / 32.0f;
	};
	const auto NewAttempt = [&](int ClientId) {
		GameServer()->m_apPlayers[ClientId]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(80, 80));
		pController->Tick();
	};
	const auto Command = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	// The corridor is shorter than the shortest /distance, but every spot has
	// more than the server default of 3 tiles to one of its sides.
	const auto LongGoal = [&](int ClientId) { return GoalDistance(ClientId) > 3.0f; };

	EXPECT_EQ(Command(Anna, "distance"), "Goal distance: 3 tiles (server default). Use /distance 50-1000 to change it, 0 for the server default.");
	for(const char *pInvalid : {"distance 1001", "distance 49", "distance 1", "distance -1"})
		EXPECT_EQ(Command(Anna, pInvalid), "The goal distance must be between 50 and 1000 tiles, 0 for the server default.") << pInvalid;

	// Retries keep the current goal, new goals use the player's own distance.
	const vec2 Goal = TrainingGoal(Anna);
	EXPECT_EQ(Command(Anna, "distance 50"), "Goal distance set to 50 tiles for new goals.");
	EXPECT_TRUE(m_pServer->m_avChatMessages[Bob].empty() || m_pServer->m_avChatMessages[Bob].back().find("Goal distance") == std::string::npos);
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Anna);
	EXPECT_EQ(TrainingGoal(Anna), Goal);
	NewAttempt(Anna);
	EXPECT_TRUE(LongGoal(Anna));
	NewAttempt(Bob);
	EXPECT_FLOAT_EQ(GoalDistance(Bob), 3.0f);
	EXPECT_EQ(Command(Anna, "distance"), "Goal distance: 50 tiles. Use /distance 50-1000 to change it, 0 for the server default.");

	// In a fight, the leader's distance applies to everyone.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(Command(Bob, "distance 60"), "Only the fight leader can change the goal distance.");
	EXPECT_EQ(Command(Bob, "distance"), "Goal distance: 50 tiles. Use /distance 50-1000 to change it, 0 for the server default.");
	m_pServer->m_avChatMessages[Bob].clear();
	EXPECT_EQ(Command(Anna, "distance 1000"), "Goal distance set to 1000 tiles for new goals.");
	ASSERT_FALSE(m_pServer->m_avChatMessages[Bob].empty());
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "Goal distance set to 1000 tiles for new goals.");
	NewAttempt(Anna);
	for(int ClientId : vIds)
	{
		EXPECT_TRUE(LongGoal(ClientId));
		EXPECT_EQ(TrainingGoal(ClientId), TrainingGoal(Anna));
	}
	EXPECT_EQ(Command(Anna, "distance 0"), "Goal distance set to 3 tiles (server default) for new goals.");
	NewAttempt(Anna);
	for(int ClientId : vIds)
		EXPECT_FLOAT_EQ(GoalDistance(ClientId), 3.0f);
}

TEST_F(GameWorld, GTrainNoDoubleJumpStartsAttemptsWithoutAirJump)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob"});
	const int Anna = vIds[0], Bob = vIds[1];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Command = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	CCharacter *pChr = GameServer()->GetPlayerChar(Anna);
	EXPECT_EQ(Command(Anna, "nodj"), "Attempts now start without double jump. Touching the ground gives it back.");
	EXPECT_EQ(pChr->Core()->m_Jumped, 0); // the current attempt keeps it
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Anna);
	EXPECT_EQ(pChr->Core()->m_Jumped, 2);
	EXPECT_EQ(pChr->Core()->m_JumpedTotal, pChr->Core()->m_Jumps - 1);

	// The feet stay dark in the air, touching the ground lights them again.
	GameServer()->m_World.Tick();
	EXPECT_EQ(pChr->Core()->m_Jumped & 2, 2);
	GameServer()->Collision()->SetCollisionAt(pChr->m_Pos.x, pChr->m_Pos.y + 32, TILE_SOLID);
	GameServer()->m_World.Tick();
	EXPECT_EQ(pChr->Core()->m_Jumped & 2, 0);

	// Fight members start attempts with their leader's setting.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob)->Core()->m_Jumped, 2);
	EXPECT_EQ(Command(Bob, "nodj"), "Only the fight leader can toggle /nodj.");
	EXPECT_EQ(Command(Anna, "nodj"), "Attempts now start with double jump.");
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "Attempts now start with double jump.");
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Bob);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob)->Core()->m_Jumped, 0);
}

TEST_F(GameWorld, GTrainForwardsGoalsLeadTowardsTheFinish)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob"});
	const int Anna = vIds[0], Bob = vIds[1];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Command = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	const auto NewAttempt = [&](int ClientId) {
		GameServer()->m_apPlayers[ClientId]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(80, 80));
		pController->Tick();
	};
	// The finish is on the right of the corridor.
	const auto SpotX = [&](int ClientId) { return (int)GameServer()->GetPlayerChar(ClientId)->m_Pos.x / 32; };
	const auto GoalOffset = [&](int ClientId) { return (int)TrainingGoal(ClientId).x / 32 - SpotX(ClientId); };
	const int FinishX = GameServer()->Collision()->GetWidth() - 2;

	bool Backwards = false;
	for(int i = 0; i < 100 && !Backwards; ++i)
	{
		NewAttempt(Anna);
		Backwards = GoalOffset(Anna) < 0;
	}
	EXPECT_TRUE(Backwards);

	// Spots closer to the finish than the goal distance are skipped, the
	// tiles beside it are 0 tiles away.
	EXPECT_EQ(Command(Anna, "forwards"), "New goals only lead towards the finish line.");
	for(int i = 0; i < 100; ++i)
	{
		NewAttempt(Anna);
		EXPECT_EQ(GoalOffset(Anna), 3);
		EXPECT_LE(SpotX(Anna), FinishX - 1 - 3);
	}

	// The corridor is shorter than 50 tiles, so every goal leads from its
	// farthest spot to the finish.
	Command(Anna, "distance 50");
	NewAttempt(Anna);
	EXPECT_EQ(SpotX(Anna), 2);
	EXPECT_EQ((int)TrainingGoal(Anna).x / 32, FinishX - 1);
	Command(Anna, "distance 0");

	// In a fight, the leader's setting applies to everyone.
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(Command(Bob, "forwards"), "Only the fight leader can toggle /forwards.");
	for(int i = 0; i < 20; ++i)
	{
		NewAttempt(Anna);
		EXPECT_EQ(GoalOffset(Bob), 3);
	}
	EXPECT_EQ(Command(Anna, "forwards"), "New goals lead in any direction again.");
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "New goals lead in any direction again.");
}

TEST_F(GameWorld, GTrainScoreLimitEndsTheFight)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob"});
	const int Anna = vIds[0], Bob = vIds[1];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	pController->Fight(Bob, "Anna");
	pController->Tick();
	const auto Command = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	const auto Capture = [&](int ClientId) {
		for(int Other : vIds)
			GameServer()->GetPlayerChar(Other)->Unfreeze();
		pController->Tick();
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(ClientId);
		pChr->SetPosition(pChr->m_Pos);
		CMemoryLogger Logger;
		{
			CLogScope Scope(&Logger);
			pController->Tick();
		}
		return Logger.ConcatenatedLines();
	};
	const auto Score = [&](int ClientId) { return pController->SnapPlayerScore(ClientId, GameServer()->m_apPlayers[ClientId]); };

	EXPECT_EQ(Command(Bob, "scorelimit"), "Fight score limit: 10. Use /scorelimit 1-1000 to change it, 0 for no limit.");
	EXPECT_EQ(Command(Bob, "scorelimit 2"), "Only the fight leader can change the score limit.");
	EXPECT_EQ(Command(Anna, "scorelimit 1001"), "The score limit must be between 1 and 1000, 0 for no limit.");
	EXPECT_EQ(Command(Anna, "scorelimit 2"), "The first to 2 points wins the fight.");
	EXPECT_EQ(m_pServer->m_avChatMessages[Bob].back(), "The first to 2 points wins the fight.");

	EXPECT_EQ(Capture(Bob).find("won the fight"), std::string::npos);
	EXPECT_EQ(Capture(Anna).find("won the fight"), std::string::npos);
	const std::string Messages = Capture(Bob);
	EXPECT_NE(Messages.find("'Bob' captured the flag (0 fails). Fight score (first to 2): 'Anna': 1, 'Bob': 2"), std::string::npos);
	EXPECT_NE(Messages.find("'Bob' won the fight with 2 points! The fight score starts over."), std::string::npos);
	const int BreakTicks = 10 * m_pServer->TickSpeed();
	for(int ClientId : vIds)
	{
		EXPECT_EQ(Score(ClientId), 0);
		EXPECT_EQ(m_pServer->m_aClients[ClientId].m_Score, 0);
		ASSERT_FALSE(m_pServer->m_avBroadcasts[ClientId].empty());
		EXPECT_EQ(m_pServer->m_avBroadcasts[ClientId].back(), "'Bob' won the fight!\n'Bob': 2, 'Anna': 1");
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_FreezeTime, BreakTicks);
	}
	// Members dying during the break hover until it ends, the next ones not.
	const auto Respawn = [&](int ClientId) {
		GameServer()->m_apPlayers[ClientId]->KillCharacter(WEAPON_SELF);
		GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(80, 80));
		pController->Tick();
		return GameServer()->GetPlayerChar(ClientId)->m_FreezeTime;
	};
	m_pServer->AdvanceTicks(BreakTicks / 2);
	EXPECT_EQ(Respawn(Bob), BreakTicks / 2);
	m_pServer->AdvanceTicks(BreakTicks / 2);
	EXPECT_EQ(Respawn(Bob), 50);
	EXPECT_EQ(Respawn(Anna), 50);

	// Without a limit, captures only add up.
	EXPECT_EQ(Command(Anna, "scorelimit 0"), "The fight score limit is disabled.");
	EXPECT_EQ(Command(Bob, "scorelimit"), "Fight score limit: none. Use /scorelimit 1-1000 to change it, 0 for no limit.");
	for(int i = 0; i < 2; ++i)
		EXPECT_EQ(Capture(Bob).find("won the fight"), std::string::npos);
	EXPECT_NE(Capture(Bob).find("Fight score: 'Anna': 0, 'Bob': 3"), std::string::npos);

	// Leaving ends the fight's score for both, rejoining starts from zero.
	pController->Fight(Bob, "");
	pController->Tick();
	EXPECT_EQ(Score(Bob), 0);
	pController->Fight(Bob, "Anna");
	pController->Tick();
	EXPECT_EQ(Score(Bob), 0);
	EXPECT_NE(Capture(Anna).find("'Anna': 1, 'Bob': 0"), std::string::npos);
}

TEST_F(GameWorld, GTrainHelpListsOnlyGTrainCommands)
{
	PrepareTrainingCorridor();
	g_Config.m_SvSpamprotection = 0;
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	m_pServer->m_avChatMessages[Runner].clear();
	Say(Runner, "/help");
	const auto &vMessages = m_pServer->m_avChatMessages[Runner];
	ASSERT_EQ(vMessages.size(), 20);
	EXPECT_EQ(vMessages.front(), "GTrain commands:");
	EXPECT_EQ(vMessages.back(), "Captures on this map earn no ELO.");
	std::string Help;
	for(const auto &Message : vMessages)
		Help += Message + "\n";
	const auto Lists = [&](const std::string &Command) {
		for(size_t Pos = Help.find(Command); Pos != std::string::npos; Pos = Help.find(Command, Pos + 1))
			if(Pos + Command.size() == Help.size() || !std::isalnum((unsigned char)Help[Pos + Command.size()]))
				return true;
		return false;
	};
	EXPECT_TRUE(Lists("Kill:"));
	// Every chat command is listed, except DDNet's general ones.
	const std::vector<std::string> vGeneral = {"help", "info", "rules", "list", "timeout", "timer", "pause", "spec", "specteam", "pausevoted", "specvoted",
		"showothers", "showall", "ninjajetpack", "w", "whisper", "c", "converse", "whispers", "dnd", "emote", "eyeemote"};
	IConsole *pConsole = GameServer()->Console();
	for(const IConsole::ICommandInfo *pCmd = pConsole->FirstCommandInfo(IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_CHAT); pCmd;
		pCmd = pConsole->NextCommandInfo(pCmd, IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_CHAT))
		if(std::find(vGeneral.begin(), vGeneral.end(), pCmd->Name()) == vGeneral.end())
			EXPECT_TRUE(Lists(std::string("/") + pCmd->Name())) << pCmd->Name();
	for(const char *pOther : {"/practice", "/save", "/join"})
		EXPECT_FALSE(Lists(pOther)) << pOther;

	// Help for a single command still shows its usage.
	m_pServer->m_avChatMessages[Runner].clear();
	Say(Runner, "/help distance");
	ASSERT_FALSE(vMessages.empty());
	EXPECT_EQ(vMessages.front(), "Usage: distance ?i[tiles]");
}

TEST_F(GameWorld, GTrainWeaponCommandsKeepWeaponsAndFollowTheFight)
{
	PrepareTrainingCorridor();
	const auto vIds = SpawnTrainingPlayers({"Anna", "Bob", "Wilson"});
	const int Anna = vIds[0], Bob = vIds[1], Wilson = vIds[2];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Command = [&](int ClientId, const char *pCommand) {
		m_pServer->m_avChatMessages[ClientId].clear();
		m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		return m_pServer->m_avChatMessages[ClientId].empty() ? std::string() : m_pServer->m_avChatMessages[ClientId].back();
	};
	const auto HasWeapon = [&](int ClientId, int Weapon) {
		return GameServer()->GetPlayerChar(ClientId)->Core()->m_aWeapons[Weapon].m_Got;
	};

	EXPECT_EQ(Command(Anna, "shotgun"), "You keep the shotgun in every attempt until /shotgun is used again.");
	EXPECT_TRUE(HasWeapon(Anna, WEAPON_SHOTGUN));
	EXPECT_FALSE(HasWeapon(Bob, WEAPON_SHOTGUN));
	GameServer()->m_apPlayers[Anna]->KillCharacter(WEAPON_SELF);
	GameServer()->m_apPlayers[Anna]->ForceSpawn(vec2(80, 80));
	pController->Tick();
	EXPECT_TRUE(HasWeapon(Anna, WEAPON_SHOTGUN)); // new attempts keep it

	// Ninja does not run out.
	Command(Anna, "ninja");
	EXPECT_TRUE(HasWeapon(Anna, WEAPON_NINJA));
	m_pServer->AdvanceTicks(20 * m_pServer->TickSpeed());
	pController->Tick();
	EXPECT_EQ(GameServer()->GetPlayerChar(Anna)->Core()->m_Ninja.m_ActivationTick, m_pServer->Tick());
	EXPECT_EQ(Command(Anna, "shotgun"), "The shotgun was removed.");
	EXPECT_FALSE(HasWeapon(Anna, WEAPON_SHOTGUN));

	// Joiners get the fight's weapons and lose the ones it does not have.
	Command(Wilson, "grenade");
	EXPECT_TRUE(HasWeapon(Wilson, WEAPON_GRENADE));
	pController->Fight(Bob, "Anna");
	pController->Fight(Wilson, "Anna");
	pController->Tick();
	for(int ClientId : {Bob, Wilson})
		EXPECT_TRUE(HasWeapon(ClientId, WEAPON_NINJA));
	EXPECT_FALSE(HasWeapon(Wilson, WEAPON_GRENADE));
	EXPECT_EQ(Command(Bob, "laser"), "Only the fight leader can toggle /laser.");
	EXPECT_FALSE(HasWeapon(Bob, WEAPON_LASER));
	EXPECT_EQ(Command(Anna, "rifle"), "You keep the laser in every attempt until /laser is used again.");
	for(int ClientId : vIds)
		EXPECT_TRUE(HasWeapon(ClientId, WEAPON_LASER));

	// Leavers keep their fight's weapons.
	pController->Fight(Bob, "");
	pController->Tick();
	EXPECT_TRUE(HasWeapon(Bob, WEAPON_LASER));
	EXPECT_TRUE(HasWeapon(Bob, WEAPON_NINJA));
	EXPECT_EQ(Command(Bob, "ninja"), "The ninja was removed.");
	EXPECT_FALSE(HasWeapon(Bob, WEAPON_NINJA));
	EXPECT_TRUE(HasWeapon(Wilson, WEAPON_NINJA));
}

// A corridor between the start and the finish line, with a shaft down to
// freeze in its middle: every spot has its goal straight above or below,
// within reach of the hook.
static void PrepareTrainingShaft(CGameContext *pGameServer)
{
	CCollision *pCollision = pGameServer->Collision();
	const int Width = pCollision->GetWidth();
	const int Shaft = Width / 2;
	ASSERT_GE(pCollision->GetHeight(), 14);
	for(int y = 0; y < pCollision->GetHeight(); ++y)
		for(int x = 0; x < Width; ++x)
			pCollision->SetCollisionAt(x * 32 + 16, y * 32 + 16, TILE_SOLID);
	for(int x = 1; x < Width - 1; ++x)
		pCollision->SetCollisionAt(x * 32 + 16, 48, TILE_AIR);
	for(int y = 2; y <= 11; ++y)
		pCollision->SetCollisionAt(Shaft * 32 + 16, y * 32 + 16, TILE_AIR);
	pCollision->SetCollisionAt(Shaft * 32 + 16, 12 * 32 + 16, TILE_FREEZE);
	pCollision->SetCollisionAt(48, 48, TILE_START);
	pCollision->SetCollisionAt((Width - 2) * 32 + 16, 48, TILE_FINISH);
	g_Config.m_SvGtrainGoalDistance = 4;
	delete pGameServer->m_pController;
	pGameServer->m_pController = new CGameControllerGTrain(pGameServer);
}

TEST_F(GameWorld, GTrainCapturesReplayExactly)
{
	char aDatabase[IO_MAX_PATH_LENGTH];
	UseTestDatabase("r.db", aDatabase, sizeof(aDatabase));
	// Only servers that want the runs store them.
	g_Config.m_SvGtrainRecordRuns = 1;
	PrepareTrainingCorridor();
	ClearMapExtras();
	PrepareTrainingShaft(GameServer());
	// Only captures that earn ELO are stored.
	static_cast<CGameControllerGTrain *>(GameServer()->m_pController)->SetMapDifficulty("Main", 1);
	g_Config.m_SvGtrainHoverDoor = 1;
	m_pServer->AdvanceTicks(10);
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	const auto Command = [&](const char *pCommand) { m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, Runner); };

	// GTrain's hover doors have the last switch, which replays leave alone.
	const int DoorSwitch = (int)GameServer()->Switchers().size() - 1;
	const auto State = [&](CCharacter *pChr) {
		CGTrainRunState Result;
		Result.Save(pChr, DoorSwitch);
		return Result;
	};

	// What the capturing attempt did: the state at the release and after
	// every tick before the capture, the input of every tick after the
	// release.
	struct CLive
	{
		std::vector<CGTrainRunState> m_vStates;
		std::vector<CNetObj_PlayerInput> m_vInputs;
	};

	// Plays attempts until one captures: keys held through the hover, firing
	// the weapon chosen for the release, then up the hook or down the shaft,
	// switching weapons and firing them at the walls.
	CNetObj_PlayerInput Input = {};
	int Scrolls = 0;
	const auto Play = [&](std::optional<int> ReleaseWeapon) {
		const bool HoldKeys = ReleaseWeapon.has_value();
		auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
		for(int Attempt = 0; Attempt < 20; ++Attempt)
		{
			CLive Live;
			Input = {};
			for(int i = 0; i < 100 && GameServer()->GetPlayerChar(Runner)->m_FreezeTime > 0; ++i)
			{
				// Upwards, a kept ninja dashes at the release.
				Input.m_TargetX = i % 3 - 1;
				Input.m_TargetY = -100 - 3 * i;
				Input.m_Jump = HoldKeys;
				if(HoldKeys && i == 10)
					++Input.m_Fire;
				// Frozen tees switch weapons too, an odd number of times.
				if(i % 16 == 8)
					Input.m_NextWeapon = Scrolls += 2;
				if(HoldKeys && i >= 40)
					Input.m_WantedWeapon = *ReleaseWeapon + 1;
				TickWithInput(Runner, Input);
			}
			Live.m_vStates.push_back(State(GameServer()->GetPlayerChar(Runner)));
			const vec2 Goal = TrainingGoal(Runner);
			const int Captures = pController->Stats(Runner).m_Captures;
			for(int Tick = 0; Tick < 300; ++Tick)
			{
				const vec2 Pos = GameServer()->GetPlayerChar(Runner)->m_Pos;
				const bool Up = Goal.y < Pos.y;
				// Back to the shaft, in it against its walls.
				const int Side = (Tick / 4) % 2 ? 1 : -1;
				Input.m_Direction = std::abs(Goal.x - Pos.x) > 4 ? (Goal.x > Pos.x ? 1 : -1) : Side;
				// The hook flies where it was aimed when fired, weapons hit the walls.
				Input.m_Hook = Up && Tick % 40 != 0;
				Input.m_TargetX = Tick % 40 < 3 ? Tick % 7 - 3 : Side * (100 + Tick * 37 % 50);
				Input.m_TargetY = Tick % 40 < 3 ? -300 : 0;
				if(Tick == 3 || Tick == 8 || Tick == 11)
					Input.m_Jump = Tick == 8;
				if(Tick % 7 == 0 || (Tick % 7 == 3 && Input.m_Fire % 2))
					++Input.m_Fire;
				// Not right away, the release's scrolling would not count.
				if(Tick % 15 == 14)
					Input.m_WantedWeapon = 1 + (Tick / 15) % NUM_WEAPONS;
				TickWithInput(Runner, Input);
				Live.m_vInputs.push_back(Input);
				if(pController->Stats(Runner).m_Captures > Captures)
					return Live;
				Live.m_vStates.push_back(State(GameServer()->GetPlayerChar(Runner)));
			}
			// A spot this bot cannot do, on to another.
			GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_SELF);
			GameServer()->m_apPlayers[Runner]->ForceSpawn(vec2(80, 80));
			TickWithInput(Runner, CNetObj_PlayerInput{});
		}
		ADD_FAILURE() << "The runner did not capture the flag";
		return CLive();
	};

	// Held by a door. The release fires the gun, its bullet is in flight
	// when the run starts.
	std::vector<CLive> vRated;
	vRated.push_back(Play(WEAPON_GUN));
	// Hovering in zero gravity without double jump, holding nothing.
	g_Config.m_SvGtrainHoverDoor = 0;
	Command("nodj");
	Command("r");
	vRated.push_back(Play(std::nullopt));

	const std::vector<CStoredRun> vStored = StoredRuns(aDatabase);
	ASSERT_EQ(vStored.size(), vRated.size());
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(GameServer()->Map()->Sha256(), aSha256, sizeof(aSha256));
	for(const CStoredRun &Stored : vStored)
	{
		EXPECT_EQ(Stored.m_Map, "coverage");
		EXPECT_EQ(Stored.m_MapSha256, aSha256);
		EXPECT_EQ(Stored.m_Name, "Runner");
		EXPECT_EQ(Stored.m_Ticks, (int)Stored.m_Run.m_vInputs.size());
		EXPECT_GT(Stored.m_Distance, 0);
		EXPECT_EQ(Stored.m_Run.m_KeptWeapons, 0);
		// What the capture earned, at the speed of its stored ticks.
		EXPECT_GT(Stored.m_Elo, 0.0f);
		EXPECT_FLOAT_EQ(Stored.m_SpeedFactor, GTrainElo::SpeedFactor(*CGameControllerGTrain::EloStep("Main", 1), Stored.m_Distance, Stored.m_Ticks / (double)SERVER_TICK_SPEED));
		EXPECT_EQ(Stored.m_Points, GTrainElo::CapturePoints(*CGameControllerGTrain::EloStep("Main", 1), Stored.m_Distance, Stored.m_Ticks / (double)SERVER_TICK_SPEED));
	}
	ASSERT_EQ(vStored[0].m_Run.m_Start.m_vShots.size(), 1u);
	EXPECT_FALSE(vStored[0].m_Run.m_Start.m_vShots[0].m_Laser);
	EXPECT_EQ(vStored[1].m_Run.m_Start.m_Jumped & 2, 2);
	EXPECT_TRUE(vStored[1].m_Run.m_Start.m_vShots.empty());

	// Kept weapons earn no ELO, so their runs are not stored, but replays
	// handle them all the same. These runs leave out the capture tick, whose
	// end the test does not see.
	std::vector<std::pair<CLive, int>> vWithWeapons;
	// Held by a door, keeping the grenade, laser and shotgun. The release
	// fires a grenade.
	g_Config.m_SvGtrainHoverDoor = 1;
	Command("nodj");
	Command("grenade");
	Command("laser");
	Command("shotgun");
	Command("r");
	vWithWeapons.emplace_back(Play(WEAPON_GRENADE), (1 << WEAPON_SHOTGUN) | (1 << WEAPON_GRENADE) | (1 << WEAPON_LASER));
	// Hovering in zero gravity without double jump, keeping ninja, which
	// dashes at the release.
	g_Config.m_SvGtrainHoverDoor = 0;
	Command("grenade");
	Command("laser");
	Command("shotgun");
	Command("ninja");
	Command("nodj");
	Command("r");
	vWithWeapons.emplace_back(Play(WEAPON_NINJA), 1 << WEAPON_NINJA);
	// A shotgun laser in flight at the release.
	g_Config.m_SvGtrainHoverDoor = 1;
	Command("ninja");
	Command("nodj");
	Command("shotgun");
	Command("r");
	vWithWeapons.emplace_back(Play(WEAPON_SHOTGUN), 1 << WEAPON_SHOTGUN);
	EXPECT_EQ(StoredRuns(aDatabase).size(), vStored.size());

	std::vector<std::pair<CGTrainRun, CLive>> vReplays;
	for(size_t i = 0; i < vStored.size(); ++i)
		vReplays.emplace_back(vStored[i].m_Run, vRated[i]);
	for(const auto &[Live, KeptWeapons] : vWithWeapons)
	{
		CGTrainRun Run;
		Run.m_Start = Live.m_vStates.front();
		Run.m_KeptWeapons = KeptWeapons;
		Run.m_vInputs.assign(Live.m_vInputs.begin(), Live.m_vInputs.end() - 1);
		Run.m_EndPos = Live.m_vStates.back().m_Pos;
		Run.m_EndVel = Live.m_vStates.back().m_Vel;
		vReplays.emplace_back(Run, Live);
	}
	ASSERT_EQ(vReplays[2].first.m_Start.m_vShots.size(), 1u);
	EXPECT_FALSE(vReplays[2].first.m_Start.m_vShots[0].m_Laser);
	EXPECT_EQ(vReplays[3].first.m_Start.m_Jumped & 2, 2);
	EXPECT_GT(vReplays[3].first.m_Start.m_NinjaCurrentMoveTime, 0);
	ASSERT_EQ(vReplays[4].first.m_Start.m_vShots.size(), 1u);
	EXPECT_TRUE(vReplays[4].first.m_Start.m_vShots[0].m_Laser);

	// Replays need a world GTrain does not control, without the lasers still
	// bouncing. They start at the same ticks as the runs did.
	GameServer()->m_apPlayers[Runner]->KillCharacter(WEAPON_GAME);
	delete GameServer()->m_pController;
	GameServer()->m_pController = new CGameControllerDDNet(GameServer());
	for(int i = 0; i < SERVER_TICK_SPEED; ++i)
	{
		m_pServer->AdvanceTicks(1);
		GameServer()->OnTick();
	}
	// Every tick ends in the same state, not just at the same position.
	for(size_t i = 0; i < vReplays.size(); ++i)
	{
		const auto &[Run, Live] = vReplays[i];
		const std::vector<CGTrainRunState> &vLive = Live.m_vStates;
		EXPECT_TRUE(Run.m_Start == vLive[0]) << "run " << i;
		m_pServer->AdvanceTicks(Run.m_Start.m_Tick - m_pServer->Tick());
		std::vector<CGTrainRunState> vReplayed;
		EXPECT_TRUE(ReplayGTrainRun(GameServer(), Runner, Run, [&]() { m_pServer->AdvanceTicks(1); }, [&](CCharacter *pChr) { vReplayed.push_back(State(pChr)); })) << "run " << i;
		// Replays leave out the release, stored ones end with the capture.
		ASSERT_EQ(vReplayed.size(), Run.m_vInputs.size()) << "run " << i;
		ASSERT_GE(vReplayed.size() + 1, vLive.size()) << "run " << i;
		for(size_t Tick = 1; Tick < vLive.size(); ++Tick)
			ASSERT_TRUE(vReplayed[Tick - 1] == vLive[Tick]) << "run " << i << ", tick " << Tick << ": " << vReplayed[Tick - 1].m_Pos.x << "," << vReplayed[Tick - 1].m_Pos.y << " instead of " << vLive[Tick].m_Pos.x << "," << vLive[Tick].m_Pos.y;
		EXPECT_EQ(vReplayed.back().m_Pos, Run.m_EndPos);
	}
	// Stored runs end where their last tick reached the flag.
	for(size_t i = 0; i < vStored.size(); ++i)
	{
		const CGTrainRun &Run = vStored[i].m_Run;
		EXPECT_LT(distance(Run.m_EndPos, Run.m_Goal), 42.0f + distance(Run.m_EndPos, vRated[i].m_vStates.back().m_Pos)) << "run " << i;
	}
}

TEST_F(GameWorld, GTrainStoresOnlyCapturesThatEarnElo)
{
	char aDatabase[IO_MAX_PATH_LENGTH];
	UseTestDatabase("r.db", aDatabase, sizeof(aDatabase));
	// Only servers that want the runs store them.
	g_Config.m_SvGtrainRecordRuns = 1;
	PrepareTrainingCorridor();
	ClearMapExtras();
	const int Runner = SpawnTrainingPlayers({"Runner"})[0];
	auto *pController = static_cast<CGameControllerGTrain *>(GameServer()->m_pController);
	const auto Command = [&](const char *pCommand) { m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, Runner); };
	const auto Release = [&]() {
		for(int i = 0; i < 100 && GameServer()->GetPlayerChar(Runner)->m_FreezeTime > 0; ++i)
			TickWithInput(Runner, CNetObj_PlayerInput{});
		TickWithInput(Runner, CNetObj_PlayerInput{});
	};
	const auto Capture = [&](bool Frozen) {
		CCharacter *pChr = GameServer()->GetPlayerChar(Runner);
		if(Frozen)
			pChr->Freeze();
		pChr->m_Pos = pChr->m_PrevPos = TrainingGoal(Runner);
		pChr->SetPosition(pChr->m_Pos);
		pController->Tick();
	};

	// Maps without ELO store nothing.
	Release();
	Capture(false);
	EXPECT_EQ(StoredRuns(aDatabase).size(), 0u);
	pController->SetMapDifficulty("Main", 1);
	// Runs keep the ELO the capture added, not the player's total.
	const auto CaptureEarning = [&]() {
		const double EloBefore = GTrainElo::FromPoints(pController->Stats(Runner).m_Points);
		Release();
		Capture(false);
		return GTrainElo::FromPoints(pController->Stats(Runner).m_Points) - EloBefore;
	};
	const auto ExpectEarned = [&](double Elo) {
		const CStoredRun Stored = StoredRuns(aDatabase).back();
		EXPECT_GT(Elo, 0.0);
		EXPECT_FLOAT_EQ(Stored.m_Elo, Elo);
		// Instant captures are as fast as it gets.
		EXPECT_FLOAT_EQ(Stored.m_SpeedFactor, GTrainElo::MAX_SPEED_FACTOR);
	};
	double Elo = CaptureEarning();
	EXPECT_EQ(StoredRuns(aDatabase).size(), 1u);
	ExpectEarned(Elo);
	// Neither do frozen captures.
	Release();
	Capture(true);
	EXPECT_EQ(StoredRuns(aDatabase).size(), 1u);
	// Nor those with kept weapons, also when they were on only midway.
	Command("grenade");
	Release();
	Capture(false);
	EXPECT_EQ(StoredRuns(aDatabase).size(), 1u);
	Command("grenade");
	Release();
	Command("grenade");
	TickWithInput(Runner, CNetObj_PlayerInput{});
	Command("grenade");
	Capture(false);
	EXPECT_EQ(StoredRuns(aDatabase).size(), 1u);
	// Pauses cannot be replayed.
	Release();
	ASSERT_EQ(GameServer()->m_apPlayers[Runner]->Pause(CPlayer::PAUSE_PAUSED, true), CPlayer::PAUSE_PAUSED);
	TickWithInput(Runner, CNetObj_PlayerInput{});
	ASSERT_EQ(GameServer()->m_apPlayers[Runner]->Pause(CPlayer::PAUSE_NONE, true), CPlayer::PAUSE_NONE);
	TickWithInput(Runner, CNetObj_PlayerInput{});
	Capture(false);
	EXPECT_EQ(StoredRuns(aDatabase).size(), 1u);
	// The server can turn storing off.
	g_Config.m_SvGtrainRecordRuns = 0;
	Release();
	Capture(false);
	EXPECT_EQ(StoredRuns(aDatabase).size(), 1u);
	g_Config.m_SvGtrainRecordRuns = 1;
	Elo = CaptureEarning();
	EXPECT_EQ(StoredRuns(aDatabase).size(), 2u);
	ExpectEarned(Elo);
}

TEST_F(GameWorld, WeaponChatCommandsStillRequirePracticeOutsideGTrain)
{
	g_Config.m_DbgDummies = 1;
	m_pServer->UpdateDebugDummies(false);
	const int ClientId = m_pServer->MaxClients() - 1;
	CCharacter *pChr = GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(80, 80));
	m_pServer->m_avChatMessages[ClientId].clear();
	m_pServer->Console()->ExecuteLineFlag("shotgun", CFGFLAG_CHAT, ClientId);
	EXPECT_FALSE(pChr->Core()->m_aWeapons[WEAPON_SHOTGUN].m_Got);
	ASSERT_FALSE(m_pServer->m_avChatMessages[ClientId].empty());
	EXPECT_NE(m_pServer->m_avChatMessages[ClientId].back().find("/practice"), std::string::npos);
}

TEST_F(GameWorld, ClosestCharacter)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChr1 = new(0) CCharacter(&GameServer()->m_World, Input);
	pChr1->m_Pos = vec2(0, 0);
	GameServer()->m_World.InsertEntity(pChr1);

	CCharacter *pChr2 = new(1) CCharacter(&GameServer()->m_World, Input);
	pChr2->m_Pos = vec2(10, 10);
	GameServer()->m_World.InsertEntity(pChr2);

	CCharacter *pClosest = GameServer()->m_World.ClosestCharacter(vec2(1, 1), 20, nullptr);
	EXPECT_EQ(pClosest, pChr1);
}

TEST_F(GameWorld, IntersectEntity)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChrLeft = new(0) CCharacter(&GameServer()->m_World, Input);
	pChrLeft->m_Pos = vec2(15, 10);
	GameServer()->m_World.InsertEntity(pChrLeft);

	CCharacter *pChrRight = new(1) CCharacter(&GameServer()->m_World, Input);
	pChrRight->m_Pos = vec2(16, 10);
	GameServer()->m_World.InsertEntity(pChrRight);

	float Radius = 5.0f;
	vec2 IntersectAt;
	CCharacter *pIntersectedChar;

	// both tees are exactly on the line
	// if we go intersect left to right we find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// if we intersect right to left we find the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);

	// but not if we ignore the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrRight, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// or we force find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// pNotThis == pThisOnly => nullptr

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrLeft, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, nullptr);

	// the tee closer to the start of the intersection line
	// will not be matched if it is further than Radius away
	// from the line

	vec2 CloserToFromButTooFarFromLine = vec2(11, 11 + Radius + pChrLeft->GetProximityRadius());
	pChrLeft->SetPosition(CloserToFromButTooFarFromLine);
	pChrLeft->m_Pos = CloserToFromButTooFarFromLine;

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);
}

static bool FindFreeHorizontalSegment(const CCollision *pCollision, float Length, vec2 &OutFrom)
{
	for(int y = 2; y < pCollision->GetHeight() - 2; y++)
	{
		for(int x = 2; x < pCollision->GetWidth() - 2; x++)
		{
			const vec2 From((x + 0.5f) * 32.0f, (y + 0.5f) * 32.0f);
			if(pCollision->IntersectLine(From, From + vec2(Length, 0.0f), nullptr, nullptr) != 0)
				continue;
			bool Free = true;
			for(float Offset = 0.0f; Offset <= Length && Free; Offset += 16.0f)
			{
				const int Index = pCollision->GetPureMapIndex(From + vec2(Offset, 0.0f));
				Free = pCollision->GetTileIndex(Index) == 0 && pCollision->GetFrontTileIndex(Index) == 0 &&
				       pCollision->IsTeleport(Index) == 0 && pCollision->IsTeleportWeapon(Index) == 0;
			}
			if(Free)
			{
				OutFrom = From;
				return true;
			}
		}
	}
	return false;
}

TEST_F(GameWorld, LaserOfPlayerWithoutCharacterHitsOthers)
{
	vec2 From;
	ASSERT_TRUE(FindFreeHorizontalSegment(GameServer()->Collision(), 200.0f, From));
	g_Config.m_SvHit = 1;

	g_Config.m_DbgDummies = 1;
	m_pServer->UpdateDebugDummies(false);
	GameServer()->OnTick();
	CCharacter *pTarget = GameServer()->GetPlayerChar(m_pServer->MaxClients() - 1);
	ASSERT_NE(pTarget, nullptr);
	pTarget->m_Pos = From + vec2(150.0f, 0.0f);
	pTarget->Freeze(10);
	ASSERT_NE(pTarget->m_FreezeTime, 0);

	g_Config.m_DbgDummies = 2;
	m_pServer->UpdateDebugDummies(false);
	const int OwnerId = m_pServer->MaxClients() - 2;
	ASSERT_EQ(GameServer()->GetPlayerChar(OwnerId), nullptr);

	new CLaser(&GameServer()->m_World, From, vec2(1.0f, 0.0f), 800.0f, OwnerId, WEAPON_LASER);

	EXPECT_EQ(pTarget->m_FreezeTime, 0); // NOLINT(clang-analyzer-unix.Malloc)
}

TEST_F(GameWorld, BasicTick)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	const int StartTeam = GameServer()->m_pController->GetAutoTeam(ClientId);
	GameServer()->CreatePlayer(ClientId, StartTeam, Afk, LastWhisperTo);

	GameServer()->OnTick();
}

TEST_F(GameWorld, CharacterEmote)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, Afk, LastWhisperTo);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pChr = pPlayer->GetCharacter();
	ASSERT_NE(pChr, nullptr);

	// afk
	pPlayer->SetAfk(true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// not afk
	pPlayer->SetAfk(false);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// frozen and paused
	pPlayer->Pause(CPlayer::PAUSE_PAUSED, true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// ninja jetpack
	pPlayer->Pause(CPlayer::PAUSE_NONE, true);
	pChr->Unfreeze();
	pPlayer->m_NinjaJetpack = true;
	pChr->m_NinjaJetpack = true;
	pChr->SetJetpack(true);
	pChr->SetActiveWeapon(WEAPON_GUN);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_HAPPY);

	// /emote angry 3 chat command
	pChr->SetEmote(EMOTE_ANGRY, GameServer()->Server()->Tick() + GameServer()->Server()->TickSpeed() * 3);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);

	// /emote angry 3 chat command and frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);
}

TEST(Tunings, OutOfRangeBecomesIntMin)
{
	const float IntMin = std::numeric_limits<int>::min() / 100.0f;
	CTuneParam Param;
	EXPECT_EQ((float)(Param = 555555555555555.0f), IntMin);
	EXPECT_EQ((float)(Param = -555555555555555.0f), IntMin);
	EXPECT_EQ((float)(Param = std::numeric_limits<float>::quiet_NaN()), IntMin);
	EXPECT_EQ((float)(Param = 0.5f), 0.5f);
}
