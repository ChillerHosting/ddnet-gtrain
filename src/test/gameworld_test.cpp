#include "test.h"

#include <base/logger.h>
#include <base/types.h>

#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/server/databases/connection.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/register.h>
#include <engine/server/server.h>
#include <engine/server/server_logger.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/shared/masterserver.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gamemodes/gtrain.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>
#include <game/version.h>

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <thread>

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
		g_Config.m_SvSoloServer = 1;
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
	EXPECT_NE(CaptureLogger.ConcatenatedLines().find("captured the flag in 3.00 seconds."), std::string::npos);
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
	EXPECT_NE(NextCaptureLogger.ConcatenatedLines().find("captured the flag in 1.00 seconds."), std::string::npos);
}

TEST_F(GameWorld, GTrainTeamAndPracticeCommandsPreserveFight)
{
	PrepareTrainingCorridor();
	g_Config.m_SvPractice = 1;
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
		for(const char *pCommand : {"team", "team 0", "team 1", "practice", "practice 1", "practice 0"})
			m_pServer->Console()->ExecuteLineFlag(pCommand, CFGFLAG_CHAT, ClientId);
		const auto &vMessages = m_pServer->m_avChatMessages[ClientId];
		ASSERT_EQ(vMessages.size(), 6);
		EXPECT_EQ(vMessages[0], "/team is disabled in GTrain. Use /fight to join a fight group.");
		EXPECT_EQ(vMessages[3], "/practice is disabled in GTrain.");
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
	EXPECT_NE(CaptureLogger.ConcatenatedLines().find("'Runner' captured the flag in 1.00 seconds."), std::string::npos);
}

TEST_F(GameWorld, GTrainRetryRespectsFightLeader)
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

	// The leader retries everyone at the current start and goal.
	m_pServer->Console()->ExecuteLineFlag("r", CFGFLAG_CHAT, Anna);
	for(int ClientId : {Anna, Bob})
	{
		EXPECT_EQ(GameServer()->GetPlayerChar(ClientId)->m_Pos, Start);
		EXPECT_TRUE(GameServer()->GetPlayerChar(ClientId)->m_ZeroGravity);
		EXPECT_EQ(TrainingGoal(ClientId), Goal);
	}

	// Retry overrides the pending next-position reset after a leader's death.
	GameServer()->m_apPlayers[Anna]->KillCharacter(WEAPON_SELF);
	EXPECT_EQ(GameServer()->GetPlayerChar(Bob), nullptr);
	m_pServer->Console()->ExecuteLineFlag("retry", CFGFLAG_CHAT, Anna);
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
	EXPECT_NE(Messages.find("'Wilson' wins! Fight score:"), std::string::npos);
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
	EXPECT_NE(NextLogger.ConcatenatedLines().find("'Anna' wins! Fight score:"), std::string::npos);
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

	// Even if everyone else leaves, the remaining player's wins persist.
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
	EXPECT_NE(RemainingLogger.ConcatenatedLines().find("'Anna': 2"), std::string::npos);
	EXPECT_NE(RemainingLogger.ConcatenatedLines().find("'Bob': 1"), std::string::npos);
	EXPECT_EQ(RemainingLogger.ConcatenatedLines().find("'Wilson':"), std::string::npos);
}

TEST_F(GameWorld, GTrainFightTeamsMergeLeaveAndDisconnect)
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
	pController->Fight(Bob, "Ed"); // merges both groups; the target group's leader stays
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
