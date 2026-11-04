#include "gtrain.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/secure.h>

#include <engine/shared/config.h>
#include <engine/shared/console.h>
#include <engine/storage.h>

#include <generated/server_data.h>

#include <game/collision.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemodes/gtrain_elo.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/switch_layer.h>

#include <algorithm>
#include <iterator>
#include <string>

#define GAME_TYPE_NAME "GTrain"
#define TEST_TYPE_NAME "TestGTrain"

CGameControllerGTrain::CGameControllerGTrain(class CGameContext *pGameServer) :
	CGameControllerDDNet(pGameServer),
	m_Pathfinder(m_PathGraph)
{
	m_pGameType = g_Config.m_SvTestingCommands ? TEST_TYPE_NAME : GAME_TYPE_NAME;
	m_GameFlags = 0; // Fight wins are numeric scores, including on 0.7 clients.

	std::fill(std::begin(m_aPendingPlace), std::end(m_aPendingPlace), false);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		m_aFightGroup[ClientId] = ClientId;
	std::fill(std::begin(m_aTeamRequest), std::end(m_aTeamRequest), -1);
	m_FlagSnapId = Server()->SnapNewId();
	for(auto &SnapId : m_aDirectionSnapIds)
		SnapId = Server()->SnapNewId();
	for(auto &SnapId : m_aDoorSnapIds)
		SnapId = Server()->SnapNewId();

	// Hover doors need door collision, which only exists with a switch layer
	// (see AddSwitchLayer), and a switch number the map leaves free. It stays
	// off for every team except while a door holds one of its tees.
	CCollision *pCollision = GameServer()->Collision();
	if(pCollision->SwitchLayer() && pCollision->m_HighestSwitchNumber < 255)
	{
		m_DoorSwitch = pCollision->m_HighestSwitchNumber + 1;
		pCollision->m_HighestSwitchNumber = m_DoorSwitch;
		GameServer()->m_World.m_Core.InitSwitchers(m_DoorSwitch);
		SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
		Switcher.m_Initial = false;
		std::fill(std::begin(Switcher.m_aStatus), std::end(Switcher.m_aStatus), false);
	}

	// Runs need the random teleport choices made for them.
	GameServer()->m_World.m_Core.m_RandomHook = [this](int ClientId, int Value) {
		if(ClientId >= 0 && ClientId < MAX_CLIENTS && m_aAttempts[ClientId].m_Run)
		{
			CGTrainRun &Run = *m_aAttempts[ClientId].m_Run;
			Run.m_vRandom.push_back({Server()->Tick() - Run.m_Start.m_Tick, Value});
		}
		return Value;
	};

	HideUnrelatedChatCommands();
	FindTrainPositions();
	Teams().SetScoreboardTeams(m_aScoreboardTeams);
}

CGameControllerGTrain::~CGameControllerGTrain()
{
	GameServer()->m_World.m_Core.m_RandomHook = nullptr;
	if(m_FlagSnapId)
		Server()->SnapFreeId(*m_FlagSnapId);
	for(auto SnapId : m_aDirectionSnapIds)
		if(SnapId)
			Server()->SnapFreeId(*SnapId);
	for(auto SnapId : m_aDoorSnapIds)
		if(SnapId)
			Server()->SnapFreeId(*SnapId);
	// Map changes and shutdowns delete the controller before the database.
	if(GameServer()->Score())
		SaveAllStats();
}

bool CGameControllerGTrain::AddSwitchLayer(IStorage *pStorage, const char *pPath, char *pNewPath, int NewPathSize)
{
	// Maps without a switch layer are served with an empty one added, which
	// clients download like any changed map. The map_add_switch_layer tool
	// adds it to map files for good.
	pNewPath[0] = '\0';
	// The save path may lack the map's folder, its root always exists.
	char aTemp[IO_MAX_PATH_LENGTH];
	IStorage::FormatTmpPath(aTemp, sizeof(aTemp), fs_filename(pPath));
	switch(::AddSwitchLayer(pStorage, pPath, IStorage::TYPE_ALL, aTemp, IStorage::TYPE_SAVE))
	{
	case EAddSwitchLayer::FAILED: return false;
	case EAddSwitchLayer::PRESENT: return true;
	case EAddSwitchLayer::ADDED:
	case EAddSwitchLayer::MOVED: break;
	}
	str_copy(pNewPath, aTemp, NewPathSize);
	return true;
}

// Chat commands GTrain has a use for. The others are about races, teams and
// practice, and only clutter the chat's command completion.
static const char *const s_apChatCommands[] = {
	"help", "cmdlist", "info", "rules", "list",
	"r", "retry", "fight", "team", "accept", "a", "distance", "nodj", "forwards", "trail", "scorelimit", "freeplay", "settings",
	// practice binds, they retry
	"invincible", "tc", "telecursor", "rescue", "back", "tp", "teleport", "lasttp",
	"shotgun", "grenade", "laser", "rifle", "ninja",
	"points", "elo", "rank", "profile", "top", "top5", "top5points", "top5elo",
	"top5captures", "top5attempts", "top5respawns", "top5playtime",
	"map", "mapinfo", "timeout", "timer",
	"pause", "spec", "specteam", "pausevoted", "specvoted", "showothers", "showall", "ninjajetpack",
	"w", "whisper", "c", "converse", "whispers", "dnd", "emote", "eyeemote"};

static void ConTeamUp(IConsole::IResult *pResult, void *pUserData)
{
	auto *pController = dynamic_cast<CGameControllerGTrain *>(((CGameContext *)pUserData)->m_pController);
	if(pController && pResult->m_ClientId >= 0 && pResult->m_ClientId < MAX_CLIENTS)
		pController->TeamUp(pResult->m_ClientId, pResult->NumArguments() ? pResult->GetString(0) : "");
}

void CGameControllerGTrain::HideUnrelatedChatCommands()
{
	// /team asks players by name instead of joining DDRace team numbers.
	// CGameContext::OnInit registers DDNet's /team again before every map.
	GameServer()->Console()->Register("team", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeamUp, GameServer(), "Ask a player to play together in a team (no name to leave)");

	// Without the chat flag, they neither run from chat nor get sent to
	// clients. Rcon keeps the commands it has access to.
	IConsole *pConsole = GameServer()->Console();
	std::vector<std::string> vUnrelated;
	for(const IConsole::ICommandInfo *pCmd = pConsole->FirstCommandInfo(IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_CHAT); pCmd;
		pCmd = pConsole->NextCommandInfo(pCmd, IConsole::CLIENT_ID_UNSPECIFIED, CFGFLAG_CHAT))
	{
		if(std::none_of(std::begin(s_apChatCommands), std::end(s_apChatCommands), [&](const char *pName) { return str_comp_nocase(pName, pCmd->Name()) == 0; }))
			vUnrelated.emplace_back(pCmd->Name());
	}
	for(const std::string &Name : vUnrelated)
		pConsole->RemoveCommandFlags(Name.c_str(), CFGFLAG_CHAT);
}

static const char *const s_apEloDifficulties[] = {"Easy", "Main", "Hard", "Insane", "Extreme"};
static const char *const s_apEloStars[] = {"★☆☆", "★★☆", "★★★"};
// Above Extreme, without stars.
static const char *const s_pDeranged = "Deranged";

std::optional<int> CGameControllerGTrain::EloStep(const char *pServer, int Stars)
{
	if(str_comp_nocase(pServer, s_pDeranged) == 0)
		return GTrainElo::DERANGED_STEP;
	if(Stars < 1 || Stars > (int)std::size(s_apEloStars))
		return std::nullopt;
	for(int Difficulty = 0; Difficulty < (int)std::size(s_apEloDifficulties); ++Difficulty)
		if(str_comp_nocase(pServer, s_apEloDifficulties[Difficulty]) == 0)
			return Difficulty * (int)std::size(s_apEloStars) + Stars - 1;
	return std::nullopt;
}

void CGameControllerGTrain::LoadMapDifficulty()
{
	m_pMapDifficultyResult = std::make_shared<CScoreMapDifficultyResult>();
	GameServer()->Score()->LoadMapDifficulty(m_pMapDifficultyResult);
}

void CGameControllerGTrain::SetMapDifficulty(const char *pServer, int Stars)
{
	m_EloStep = EloStep(pServer, Stars);
	if(m_EloStep)
		log_info("gtrain", "map is %s with %d stars, captures are worth %.1f times as much as on Easy with one star", pServer, Stars, GTrainElo::PointsPerTile(*m_EloStep) / GTrainElo::PointsPerTile(0));
	else
		log_info("gtrain", "map is '%s' with %d stars, captures earn no ELO", pServer, Stars);
}

void CGameControllerGTrain::LoadStats(int ClientId)
{
	SaveStats(ClientId);
	// Database jobs run in order, so changes from now on are missing from
	// this result and get added to it.
	m_aStats[ClientId] = CGTrainStats();
	m_aPlaytimeTicks[ClientId] = 0;
	str_copy(m_aaStatsNames[ClientId], Server()->ClientName(ClientId));
	m_apStatsResults[ClientId] = std::make_shared<CScoreGTrainStatsResult>();
	GameServer()->Score()->LoadGTrainStats(m_aaStatsNames[ClientId], m_apStatsResults[ClientId]);
}

void CGameControllerGTrain::AddStats(int ClientId, const CGTrainStats &Stats)
{
	m_aStats[ClientId].Add(Stats);
	m_aUnsavedStats[ClientId].Add(Stats);
}

void CGameControllerGTrain::SaveStats(int ClientId)
{
	if(m_aUnsavedStats[ClientId].IsEmpty() || !m_aaStatsNames[ClientId][0])
		return;
	GameServer()->Score()->AddGTrainStats(m_aaStatsNames[ClientId], m_aUnsavedStats[ClientId]);
	m_aUnsavedStats[ClientId] = CGTrainStats();
}

void CGameControllerGTrain::SaveAllStats()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		SaveStats(ClientId);
}

void CGameControllerGTrain::EndAttempt(int ClientId, bool Captured)
{
	// Quick retries and skipped spots are no attempts, and rcon cheats would
	// make anything trivial.
	CAttempt &Attempt = m_aAttempts[ClientId];
	Attempt.m_Run.reset();
	if(!Attempt.m_Running)
		return;
	Attempt.m_Running = false;
	if((!Captured && Server()->Tick() - Attempt.m_StartTick < MIN_ATTEMPT_SECONDS * Server()->TickSpeed()) ||
		((CConsole *)GameServer()->Console())->Cheated())
		return;
	Attempt.m_SpotTried = true;
	CGTrainStats Stats;
	Stats.m_Attempts = 1;
	Stats.m_Captures = Captured;
	AddStats(ClientId, Stats);
}

void CGameControllerGTrain::StartRun(CCharacter *pChr)
{
	// From the release on, only the player's inputs move the tee. Kept
	// weapons earn no ELO, so their runs are not stored.
	const int ClientId = pChr->GetPlayer()->GetCid();
	CAttempt &Attempt = m_aAttempts[ClientId];
	if(Attempt.m_WithWeapons)
		return;
	Attempt.m_Run.emplace();
	Attempt.m_Run->m_Start.Save(pChr, m_DoorSwitch);
	Attempt.m_Run->m_Goal = m_PathGraph.m_vNodes[Attempt.m_pGoal->m_Goal].m_Pos;
}

void CGameControllerGTrain::SaveRun(CCharacter *pChr, const CEloGain &Gain)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	CAttempt &Attempt = m_aAttempts[ClientId];
	if(!Attempt.m_Run || !GameServer()->Score())
		return;
	Attempt.m_Run->m_EndPos = pChr->Core()->m_Pos;
	Attempt.m_Run->m_EndVel = pChr->Core()->m_Vel;
	GameServer()->Score()->SaveGTrainRun(Server()->ClientName(ClientId), Attempt.m_pGoal->m_Distance, Gain.m_Points, Gain.m_Elo, Gain.m_SpeedFactor, std::move(*Attempt.m_Run));
	Attempt.m_Run.reset();
}

std::optional<CGameControllerGTrain::CEloGain> CGameControllerGTrain::AwardElo(int ClientId, bool Frozen)
{
	// The goal's path distance, never more than the chosen goal distance.
	const CAttempt &Attempt = m_aAttempts[ClientId];
	const int Distance = Attempt.m_pGoal->m_Distance;
	if(!m_EloStep || Distance <= 0 || ((CConsole *)GameServer()->Console())->Cheated())
		return std::nullopt;
	// Teammates can carry each other through anything.
	if(InTeam(ClientId))
		return CEloGain{0.0, 0.0, "team"};
	// Frozen tees still capture, but only reaching the flag in control earns,
	// and only without the help of kept weapons.
	if(Frozen)
		return CEloGain{0.0, 0.0, "frozen"};
	if(Attempt.m_WithWeapons)
		return CEloGain{0.0, 0.0, "kept weapons"};
	// Goals made before the map's difficulty was known can be shorter.
	if(IsDeranged() && Attempt.m_TargetDistance < DERANGED_MIN_GOAL_DISTANCE)
		return CEloGain{0.0, 0.0, "goal under 100 tiles"};
	const double Seconds = (Server()->Tick() - Attempt.m_StartTick) / (double)Server()->TickSpeed();
	CGTrainStats Gain;
	Gain.m_Points = GTrainElo::CapturePoints(*m_EloStep, Distance, Seconds);
	const double EloBefore = GTrainElo::FromPoints(m_aStats[ClientId].m_Points);
	AddStats(ClientId, Gain);
	return CEloGain{GTrainElo::FromPoints(m_aStats[ClientId].m_Points) - EloBefore, GTrainElo::SpeedFactor(*m_EloStep, Distance, Seconds), nullptr, Gain.m_Points};
}

void CGameControllerGTrain::BanRuns(const char *pMap, const char *pName, bool Ban)
{
	if(!GameServer()->Score())
		return;
	GameServer()->Score()->BanGTrainRuns(pMap, pName, Ban);
	// Reloading after the ban, the database runs jobs in order, shows the
	// changed ELO here. Players on other servers see it once they rejoin.
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(m_aaStatsNames[ClientId][0] && (!pName[0] || str_comp(m_aaStatsNames[ClientId], pName) == 0))
			LoadStats(ClientId);
	}
}

void CGameControllerGTrain::FormatCaptureResult(char *pBuf, int BufSize, int Fails, const std::optional<CEloGain> &Gain)
{
	str_format(pBuf, BufSize, "%d fail%s", Fails, Fails == 1 ? "" : "s");
	if(!Gain)
		return;
	char aElo[64];
	if(Gain->m_pNoElo)
		str_format(aElo, sizeof(aElo), ", +0 ELO (%s)", Gain->m_pNoElo);
	else
		str_format(aElo, sizeof(aElo), ", +%.2f ELO, %.1fx speed", Gain->m_Elo, Gain->m_SpeedFactor);
	str_append(pBuf, aElo, BufSize);
}

void CGameControllerGTrain::FindTrainPositions()
{
	CCollision *pCollision = GameServer()->Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();

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
	m_Pathfinder.FindDistances(NodesBeside(TILE_FINISH), m_vFinishDistances);

	ExcludeStartArea();
	// Spots far enough from the finish for /forwards are a suffix then.
	std::stable_sort(m_vTrainPositions.begin(), m_vTrainPositions.end(), [&](vec2 A, vec2 B) {
		return m_vFinishDistances[PathNode(A)] < m_vFinishDistances[PathNode(B)];
	});
	for(vec2 Pos : m_vTrainPositions)
		m_PathGraph.m_vNodes[PathNode(Pos)].m_Goal = true;
}

std::vector<int> CGameControllerGTrain::NodesBeside(int Tile) const
{
	CCollision *pCollision = GameServer()->Collision();
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

void CGameControllerGTrain::ExcludeStartArea()
{
	// Map starts are usually easy. No spawn or goal is placed within the
	// configured path distance of the start line, measured like goals.
	const int Distance = g_Config.m_SvGtrainStartExclusion;
	if(Distance <= 0 || m_vTrainPositions.empty())
		return;
	// Tiles beside the start line are already one tile away from it.
	std::vector<int> vNearStart;
	m_Pathfinder.FindReachable(NodesBeside(TILE_START), Distance - 1, vNearStart);
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

int CGameControllerGTrain::RandomStart(int Group) const
{
	// Forward goals have their full distance from spots at least that far
	// from the finish, so the others are skipped. Maps shorter than that use
	// their spots farthest from the finish.
	auto Begin = m_vTrainPositions.begin();
	const CSettings &Settings = m_aSettings[Group];
	if(Settings.m_Forwards && !Settings.m_Freeplay)
	{
		const int MinDistance = std::min(GoalDistance(Group), m_vFinishDistances[PathNode(m_vTrainPositions.back())]);
		Begin = std::partition_point(Begin, m_vTrainPositions.end(), [&](vec2 Pos) {
			return m_vFinishDistances[PathNode(Pos)] < MinDistance;
		});
	}
	return PathNode(Begin[secure_rand_below((int)(m_vTrainPositions.end() - Begin))]);
}

void CGameControllerGTrain::PlaceRandomly(CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	if(m_vTrainPositions.empty())
	{
		m_aPendingPlace[ClientId] = false;
		return;
	}

	CAttempt &Attempt = m_aAttempts[ClientId];
	if(Attempt.m_pGoal.use_count() != 1)
		Attempt.m_pGoal = std::make_shared<CGTrainGoal>();
	const int Group = m_aFightGroup[ClientId];
	const int Start = RandomStart(Group);
	const int TargetDistance = GoalDistance(Group);
	if(m_aSettings[Group].m_Freeplay)
		*Attempt.m_pGoal = CGTrainGoal();
	else
		m_Pathfinder.Build(*Attempt.m_pGoal, Start, TargetDistance, secure_rand_below(1 << 30), m_aSettings[Group].m_Forwards ? &m_vFinishDistances : nullptr);
	PlaceForAttempt(pPlayer, Attempt.m_pGoal, Start, TargetDistance, false);
}

void CGameControllerGTrain::PlaceForAttempt(CPlayer *pPlayer, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance, bool Retry)
{
	const int ClientId = pPlayer->GetCid();
	const CSettings &Settings = m_aSettings[m_aFightGroup[ClientId]];
	const vec2 Pos = m_PathGraph.m_vNodes[Start].m_Pos;
	EndAttempt(ClientId, false);
	// Every attempt starts like after a kill, without the weapons and
	// powerups picked up during the previous one. Characters that spawned
	// for this placement are new already. The quiet death neither restarts
	// the player's fight nor shows, and also deletes a dead character before
	// its allocation pool slot is reused. Shots of the previous attempt are
	// gone too, the new character would keep them flying.
	GameServer()->m_World.RemoveEntitiesFromPlayer(ClientId);
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr || !m_aPendingPlace[ClientId])
	{
		const bool SyncFightDeaths = m_SyncFightDeaths;
		m_SyncFightDeaths = true;
		pPlayer->KillCharacter(WEAPON_GAME, false, false);
		m_SyncFightDeaths = SyncFightDeaths;
		pChr = pPlayer->ForceSpawn(Pos);
	}
	m_aPendingPlace[ClientId] = false;
	// Before the door, which holds the tee for its DDRace team.
	UpdateDDRaceTeam(pChr);
	KeepWeapons(pChr);
	CAttempt &Attempt = m_aAttempts[ClientId];
	// Goals are rebuilt in place, so a matching goal and start is not enough:
	// only retries count as fails.
	const bool SameSpot = Retry && Attempt.m_pGoal == pGoal && Attempt.m_Start == Start;
	Attempt.m_Fails = SameSpot ? Attempt.m_Fails + 1 : 0;
	if(!SameSpot)
		Attempt.m_SpotTried = false;
	Attempt.m_WithWeapons = false;
	Attempt.m_pGoal = pGoal;
	Attempt.m_Start = Start;
	Attempt.m_TargetDistance = TargetDistance;
	Attempt.m_StartTick = -1;
	if(Settings.m_Freeplay)
		Attempt.m_vRoute.clear();
	else
		Attempt.m_vRoute = pGoal->m_vInitialRoute;
	Attempt.m_RouteIndex = 0;
	Attempt.m_RouteNode = Start;
	pChr->SetDeepFrozen(false);
	pChr->SetLiveFrozen(false);
	pChr->Unfreeze();
	pChr->ResetHook();
	pChr->ResetJumps();
	if(Settings.m_NoDoubleJump)
		pChr->UseUpAirJumps();
	pChr->ResetVelocity();
	pChr->SetPosition(Pos);
	pChr->m_Pos = Pos;
	pChr->m_PrevPos = Pos;
	// the player didn't pass the start line, so finishing must not count
	pChr->m_DDRaceState = ERaceState::CHEATED;

	// Hover in place until the freeze is over. A tuning change only applies
	// on clients once it arrives, after their prediction passed the release.
	// A laser door below the tee instead holds it under normal gravity, and
	// its timed switch opens it on the last frozen tick: clients predict both
	// from snapshots, so the release looks the same at any ping.
	const int Hover = HoverTicks(m_aFightGroup[ClientId]);
	pChr->ForceFreeze(Hover);
	Attempt.m_DoorIndex = HoverDoorIndex(Start, pChr);
	HoldHoverDoor(pChr, Hover);
	pChr->SetZeroGravity(Attempt.m_DoorIndex < 0);
}

void CGameControllerGTrain::HoldHoverDoor(CCharacter *pChr, int Hover)
{
	const CAttempt &Attempt = m_aAttempts[pChr->GetPlayer()->GetCid()];
	if(Attempt.m_DoorIndex < 0)
		return;
	const vec2 DoorPos = GameServer()->Collision()->GetPos(Attempt.m_DoorIndex);
	GameServer()->Collision()->SetDoorCollisionAt(DoorPos.x, DoorPos.y, TILE_STOPA, 0, m_DoorSwitch);
	SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
	Switcher.m_aStatus[pChr->Team()] = true;
	Switcher.m_aEndTick[pChr->Team()] = Server()->Tick() + Hover - 1;
	Switcher.m_aType[pChr->Team()] = TILE_SWITCHTIMEDOPEN;
}

void CGameControllerGTrain::UpdateDDRaceTeam(CCharacter *pChr)
{
	// Team members share their leader's DDRace team, so they collide, hook
	// and hit each other, and share the map's switches. Everyone else plays
	// solo in a DDRace team of their own, which is what spawns give them.
	const int ClientId = pChr->GetPlayer()->GetCid();
	if(pChr->Team() == TEAM_SUPER)
		return;
	const bool Together = InTeam(ClientId);
	const int Team = Together ? m_aFightGroup[ClientId] : ClientId;
	if(pChr->Team() != Team)
		Teams().SetForceCharacterTeam(ClientId, Team);
	pChr->SetSolo(!Together);
}

int CGameControllerGTrain::HoverDoorIndex(int Start, CCharacter *pChr) const
{
	if(m_DoorSwitch < 0 || !g_Config.m_SvGtrainHoverDoor || pChr->Team() == TEAM_SUPER)
		return -1;
	CCollision *pCollision = GameServer()->Collision();
	const vec2 Below = m_PathGraph.m_vNodes[Start].m_Pos + vec2(0, 32);
	if(Below.y >= pCollision->GetHeight() * 32)
		return -1;
	// Clients only lay doors down from tiles without solid, death or laser
	// blockers. Doors of the map itself are kept.
	const int Index = pCollision->GetPureMapIndex(Below);
	CDoorTile Door;
	pCollision->GetDoorTile(Index, &Door);
	if(pCollision->GetTile(Below.x, Below.y) || pCollision->GetFrontTile(Below.x, Below.y) || (Door.m_Index && Door.m_Number != m_DoorSwitch))
		return -1;
	return Index;
}

void CGameControllerGTrain::DelayHoverDoor(CCharacter *pChr)
{
	// A paused tee stays frozen, so its door must not open meanwhile.
	if(m_DoorSwitch < 0 || m_aAttempts[pChr->GetPlayer()->GetCid()].m_DoorIndex < 0)
		return;
	SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
	if(Switcher.m_aStatus[pChr->Team()] && Switcher.m_aType[pChr->Team()] == TILE_SWITCHTIMEDOPEN)
		++Switcher.m_aEndTick[pChr->Team()];
}

void CGameControllerGTrain::OpenHoverDoor(CCharacter *pChr)
{
	// Hovers ending early must not leave the tee on its door, which clients
	// stop predicting at the release. Opens it like its timed switch would.
	if(m_DoorSwitch < 0 || m_aAttempts[pChr->GetPlayer()->GetCid()].m_DoorIndex < 0)
		return;
	SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
	const int Team = pChr->Team();
	if(!Switcher.m_aStatus[Team] || Switcher.m_aType[Team] != TILE_SWITCHTIMEDOPEN)
		return;
	Switcher.m_aStatus[Team] = false;
	Switcher.m_aEndTick[Team] = 0;
	Switcher.m_aType[Team] = TILE_SWITCHCLOSE;
}

void CGameControllerGTrain::OnCharacterSpawn(CCharacter *pChr)
{
	CGameControllerDDNet::OnCharacterSpawn(pChr);
	// Solo right away: clients predict collisions from the solo flag.
	UpdateDDRaceTeam(pChr);

	// the character is not fully initialized yet, place it in the tick
	m_aPendingPlace[pChr->GetPlayer()->GetCid()] = true;
}

int CGameControllerGTrain::FightSize(int Group) const
{
	int Size = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		if(m_aFightGroup[ClientId] == Group && GameServer()->m_apPlayers[ClientId])
			++Size;
	return Size;
}

int CGameControllerGTrain::MinGoalDistance() const
{
	return IsDeranged() ? DERANGED_MIN_GOAL_DISTANCE : MIN_GOAL_DISTANCE;
}

int CGameControllerGTrain::GoalDistance(int Group) const
{
	const int Distance = m_aSettings[Group].m_GoalDistance ? m_aSettings[Group].m_GoalDistance : g_Config.m_SvGtrainGoalDistance;
	return IsDeranged() ? std::max(Distance, (int)DERANGED_MIN_GOAL_DISTANCE) : Distance;
}

int CGameControllerGTrain::HoverTicks(int Group) const
{
	return std::max((int)HOVER_TICKS, m_aFightBreakEndTick[Group] - Server()->Tick());
}

bool CGameControllerGTrain::CheckFightLeader(int ClientId, const char *pAction)
{
	if(m_aFightGroup[ClientId] == ClientId)
		return true;
	char aMessage[128];
	str_format(aMessage, sizeof(aMessage), "Only the %s leader can %s.", GroupName(m_aFightGroup[ClientId]), pAction);
	GameServer()->SendChatTarget(ClientId, aMessage);
	return false;
}

void CGameControllerGTrain::SendFightChat(int Group, const char *pMessage)
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		if(m_aFightGroup[ClientId] == Group && GameServer()->m_apPlayers[ClientId])
			GameServer()->SendChatTarget(ClientId, pMessage);
}

void CGameControllerGTrain::KeepWeapons(CCharacter *pChr)
{
	KeepGTrainWeapons(pChr, m_aSettings[m_aFightGroup[pChr->GetPlayer()->GetCid()]].m_Weapons);
}

int CGameControllerGTrain::SnapPlayerScore(int SnappingClient, CPlayer *pPlayer)
{
	// Fighting players show their fight points, everyone else their ELO.
	const int ClientId = pPlayer->GetCid();
	if(pPlayer->GetTeam() != TEAM_SPECTATORS && m_aFightTeam[m_aFightGroup[ClientId]] != 0 && !InTeam(ClientId))
		return m_aFightWins[ClientId];
	return (int)GTrainElo::FromPoints(m_aStats[ClientId].m_Points);
}

void CGameControllerGTrain::UpdateFightTeams()
{
	int aSizes[MAX_CLIENTS] = {};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		if(GameServer()->m_apPlayers[ClientId] && GameServer()->m_apPlayers[ClientId]->GetTeam() != TEAM_SPECTATORS)
			++aSizes[m_aFightGroup[ClientId]];
	for(int Group = 0; Group < MAX_CLIENTS; ++Group)
		if(aSizes[Group] < 2)
			m_aFightTeam[Group] = 0;
	for(int Group = 0; Group < MAX_CLIENTS; ++Group)
	{
		if(aSizes[Group] < 2 || m_aFightTeam[Group] != 0)
			continue;
		for(int Team = 1; Team < TEAM_SUPER; ++Team)
		{
			if(std::find(std::begin(m_aFightTeam), std::end(m_aFightTeam), Team) == std::end(m_aFightTeam))
			{
				m_aFightTeam[Group] = Team;
				break;
			}
		}
	}
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		m_aScoreboardTeams[ClientId] = pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS ? m_aFightTeam[m_aFightGroup[ClientId]] : 0;
		if(pPlayer)
		{
			pPlayer->SetNameMarked(m_aScoreboardTeams[ClientId] != 0 && m_aFightGroup[ClientId] == ClientId);
			Server()->SetClientScore(ClientId, SnapPlayerScore(ClientId, pPlayer));
			if(CCharacter *pChr = pPlayer->GetCharacter())
				UpdateDDRaceTeam(pChr);
		}
	}
	Teams().SetScoreboardTeams(m_aScoreboardTeams);
}

void CGameControllerGTrain::RestartFight(int Group, std::optional<int> ChosenStart)
{
	m_aFightRestart[Group] = false;
	if(m_vTrainPositions.empty())
		return;
	const int Start = ChosenStart ? *ChosenStart : RandomStart(Group);
	const int TargetDistance = GoalDistance(Group);
	// All owners of this goal belong to the resetting group. Build it once,
	// then share it while keeping timers local to each player.
	auto pGoal = m_aAttempts[Group].m_pGoal;
	if(!pGoal)
		pGoal = std::make_shared<CGTrainGoal>();
	if(m_aSettings[Group].m_Freeplay)
		*pGoal = CGTrainGoal();
	else
		m_Pathfinder.Build(*pGoal, Start, TargetDistance, secure_rand_below(1 << 30), m_aSettings[Group].m_Forwards ? &m_vFinishDistances : nullptr);
	PlaceGroup(Group, pGoal, Start, TargetDistance, false);
}

void CGameControllerGTrain::PlaceGroup(int Group, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance, bool Retry)
{
	m_SyncFightDeaths = true;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(m_aFightGroup[ClientId] != Group || !pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		PlaceForAttempt(pPlayer, pGoal, Start, TargetDistance, Retry);
	}
	m_SyncFightDeaths = false;
	if(!m_aTeamGroup[Group])
		return;
	// Replacing a member's tee resets the switches of their DDRace team,
	// the hover door of the members placed before too.
	const int Hover = HoverTicks(Group);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		if(pChr && m_aFightGroup[ClientId] == Group && m_aAttempts[ClientId].m_StartTick < 0)
			HoldHoverDoor(pChr, Hover);
	}
}

bool CGameControllerGTrain::LeaveFight(int ClientId, bool NewAttempt)
{
	const int Group = m_aFightGroup[ClientId];
	if(FightSize(Group) <= 1)
	{
		m_aFightWins[ClientId] = 0;
		m_aTeamGroup[Group] = false;
		return false;
	}
	const bool Team = m_aTeamGroup[Group];
	m_aTeamGroup[Group] = false;
	const bool Restart = m_aFightRestart[Group];
	const int BreakEndTick = m_aFightBreakEndTick[Group];
	const CSettings Settings = m_aSettings[Group];
	const int ScoreboardTeam = m_aFightTeam[Group];
	m_aFightRestart[Group] = false;
	m_aFightBreakEndTick[Group] = 0;
	m_aFightTeam[Group] = 0;
	int RemainingGroup = Group == ClientId ? -1 : Group;
	if(RemainingGroup < 0)
		for(int Other = 0; Other < MAX_CLIENTS; ++Other)
			if(Other != ClientId && m_aFightGroup[Other] == Group && GameServer()->m_apPlayers[Other])
			{
				RemainingGroup = Other;
				break;
			}
	if(RemainingGroup >= 0)
	{
		for(int Other = 0; Other < MAX_CLIENTS; ++Other)
			if(Other != ClientId && m_aFightGroup[Other] == Group)
				m_aFightGroup[Other] = RemainingGroup;
		m_aFightRestart[RemainingGroup] = Restart;
		m_aFightBreakEndTick[RemainingGroup] = BreakEndTick;
		m_aFightTeam[RemainingGroup] = ScoreboardTeam;
		m_aSettings[RemainingGroup] = Settings;
	}
	m_aFightGroup[ClientId] = ClientId;
	m_aFightRestart[ClientId] = false;
	m_aFightBreakEndTick[ClientId] = 0;
	m_aFightWins[ClientId] = 0;
	m_aSettings[ClientId] = Settings;
	// A fight ends when only one player is left, so does their score.
	if(RemainingGroup >= 0 && FightSize(RemainingGroup) <= 1)
		m_aFightWins[RemainingGroup] = 0;
	// Teams start over without the leaver, the last one left alone.
	if(Team && RemainingGroup >= 0)
	{
		m_aTeamGroup[RemainingGroup] = FightSize(RemainingGroup) > 1;
		m_aFightRestart[RemainingGroup] = true;
		char aMessage[128];
		str_format(aMessage, sizeof(aMessage), m_aTeamGroup[RemainingGroup] ? "'%s' left the team." : "'%s' left the team, you train alone again.", Server()->ClientName(ClientId));
		SendFightChat(RemainingGroup, aMessage);
	}
	m_aAttempts[ClientId].m_pGoal.reset();
	if(NewAttempt)
		m_aPendingPlace[ClientId] = true;
	UpdateFightTeams();
	return true;
}

void CGameControllerGTrain::Fight(int ClientId, const char *pName)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	// Teams play together and fights race solo, so they don't mix.
	if(InTeam(ClientId))
	{
		GameServer()->SendChatTarget(ClientId, "You are in a team, which can't fight. /team alone leaves it.");
		return;
	}
	if(!pName[0])
	{
		GameServer()->SendChatTarget(ClientId, LeaveFight(ClientId, true) ? "You left fight mode." : "You are not in fight mode.");
		return;
	}
	const int Target = GameServer()->FindClientIdByName(pName).value_or(-1);
	if(Target < 0 || !GameServer()->m_apPlayers[Target])
	{
		GameServer()->SendChatTarget(ClientId, "Player not found. Use their full name.");
		return;
	}
	if(Target == ClientId)
	{
		GameServer()->SendChatTarget(ClientId, "You cannot fight yourself.");
		return;
	}
	if(InTeam(Target))
	{
		char aMessage[128];
		str_format(aMessage, sizeof(aMessage), "'%s' is in a team, which can't fight.", Server()->ClientName(Target));
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	CPlayer *pTarget = GameServer()->m_apPlayers[Target];
	// Paused and /spec players keep their characters and can fight.
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || pTarget->GetTeam() == TEAM_SPECTATORS)
	{
		GameServer()->SendChatTarget(ClientId, "Spectators cannot join a fight.");
		return;
	}
	if(m_vTrainPositions.empty())
	{
		GameServer()->SendChatTarget(ClientId, "Training is disabled on this map.");
		return;
	}
	const int SourceGroup = m_aFightGroup[ClientId];
	const int TargetGroup = m_aFightGroup[Target];
	if(SourceGroup == TargetGroup)
	{
		GameServer()->SendChatTarget(ClientId, "You are already in the same fight.");
		return;
	}
	// Joining players enter the target group's current attempt, so the target
	// group keeps its positions. Dead joiners are placed there on respawn.
	const CAttempt &TargetAttempt = m_aAttempts[TargetGroup];
	const bool JoinAttempt = TargetAttempt.m_pGoal && TargetAttempt.m_Start >= 0;
	// Joiners adopt the target group's settings, losing weapons it lacks.
	const int RemovedWeapons = m_aSettings[SourceGroup].m_Weapons & ~m_aSettings[TargetGroup].m_Weapons;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
	{
		if(m_aFightGroup[Other] != SourceGroup)
			continue;
		m_aFightGroup[Other] = TargetGroup;
		CCharacter *pChr = GameServer()->GetPlayerChar(Other);
		for(int Weapon = WEAPON_SHOTGUN; pChr && Weapon <= WEAPON_NINJA; ++Weapon)
			if((RemovedWeapons & (1 << Weapon)) && pChr->GetWeaponGot(Weapon))
				pChr->GiveWeapon(Weapon, true);
		if(JoinAttempt && pChr)
			PlaceForAttempt(GameServer()->m_apPlayers[Other], TargetAttempt.m_pGoal, TargetAttempt.m_Start, TargetAttempt.m_TargetDistance, false);
	}
	m_aFightRestart[SourceGroup] = false;
	m_aFightBreakEndTick[SourceGroup] = 0;
	if(!JoinAttempt)
		m_aFightRestart[TargetGroup] = true;
	UpdateFightTeams();
	char aMessage[160];
	str_format(aMessage, sizeof(aMessage), "'%s' joined the fight with '%s'. Use /fight to leave.",
		Server()->ClientName(ClientId), Server()->ClientName(Target));
	SendFightChat(TargetGroup, aMessage);
}

void CGameControllerGTrain::TeamUp(int ClientId, const char *pName)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	if(!pName[0])
	{
		if(!InTeam(ClientId))
		{
			GameServer()->SendChatTarget(ClientId, "You are not in a team. /team <player> asks a player to team up.");
			return;
		}
		LeaveFight(ClientId, true);
		GameServer()->SendChatTarget(ClientId, "You left the team.");
		return;
	}
	const int Target = GameServer()->FindClientIdByName(pName).value_or(-1);
	if(Target < 0 || !GameServer()->m_apPlayers[Target])
	{
		GameServer()->SendChatTarget(ClientId, "Player not found. Use their full name.");
		return;
	}
	if(Target == ClientId)
	{
		GameServer()->SendChatTarget(ClientId, "You cannot team up with yourself.");
		return;
	}
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || GameServer()->m_apPlayers[Target]->GetTeam() == TEAM_SPECTATORS)
	{
		GameServer()->SendChatTarget(ClientId, "Spectators cannot team up.");
		return;
	}
	if(m_vTrainPositions.empty())
	{
		GameServer()->SendChatTarget(ClientId, "Training is disabled on this map.");
		return;
	}
	char aMessage[160];
	const int Group = m_aFightGroup[ClientId];
	if(FightSize(Group) > 1 && !InTeam(ClientId))
	{
		GameServer()->SendChatTarget(ClientId, "You are in a fight, which can't team up. /fight alone leaves it.");
		return;
	}
	if(FightSize(m_aFightGroup[Target]) > 1 && !InTeam(Target))
	{
		str_format(aMessage, sizeof(aMessage), "'%s' is in a fight, which can't team up.", Server()->ClientName(Target));
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	if(m_aFightGroup[Target] == Group)
	{
		str_format(aMessage, sizeof(aMessage), "'%s' is already in your team.", Server()->ClientName(Target));
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	// A new request replaces the previous one.
	m_aTeamRequest[ClientId] = Target;
	m_aTeamRequestTick[ClientId] = Server()->Tick();
	str_format(aMessage, sizeof(aMessage), "'%s' wants to %s. Say /accept or /a within a minute to join.",
		Server()->ClientName(ClientId), InTeam(ClientId) ? "add you to their team" : "team up with you");
	GameServer()->SendChatTarget(Target, aMessage);
	str_format(aMessage, sizeof(aMessage), "You asked '%s' to team up. They can join with /accept or /a within a minute.", Server()->ClientName(Target));
	GameServer()->SendChatTarget(ClientId, aMessage);
}

void CGameControllerGTrain::AcceptTeam(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	// The latest request counts.
	int Inviter = -1;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
	{
		if(m_aTeamRequest[Other] != ClientId || !GameServer()->m_apPlayers[Other] ||
			Server()->Tick() - m_aTeamRequestTick[Other] > TEAM_REQUEST_SECONDS * Server()->TickSpeed())
			continue;
		if(Inviter < 0 || m_aTeamRequestTick[Other] > m_aTeamRequestTick[Inviter])
			Inviter = Other;
	}
	if(Inviter < 0)
	{
		GameServer()->SendChatTarget(ClientId, "You have no team request. Requests last a minute.");
		return;
	}
	char aMessage[160];
	const int Group = m_aFightGroup[Inviter];
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || GameServer()->m_apPlayers[Inviter]->GetTeam() == TEAM_SPECTATORS)
	{
		GameServer()->SendChatTarget(ClientId, "Spectators cannot team up.");
		return;
	}
	if(FightSize(m_aFightGroup[ClientId]) > 1 && !InTeam(ClientId))
	{
		GameServer()->SendChatTarget(ClientId, "You are in a fight, which can't team up. /fight alone leaves it.");
		return;
	}
	if(FightSize(Group) > 1 && !InTeam(Inviter))
	{
		str_format(aMessage, sizeof(aMessage), "'%s' is in a fight now, which can't team up.", Server()->ClientName(Inviter));
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	m_aTeamRequest[Inviter] = -1;
	if(m_aFightGroup[ClientId] == Group)
	{
		GameServer()->SendChatTarget(ClientId, "You are already in their team.");
		return;
	}
	// Leaving another team restarts it without this player.
	LeaveFight(ClientId, false);
	m_aFightGroup[ClientId] = Group;
	m_aTeamGroup[Group] = true;
	// Everyone starts over together, at a new spot.
	m_aFightRestart[Group] = true;
	UpdateFightTeams();
	str_format(aMessage, sizeof(aMessage), "'%s' joined the team of '%s'. /team alone leaves it.", Server()->ClientName(ClientId), Server()->ClientName(Group));
	SendFightChat(Group, aMessage);
}

void CGameControllerGTrain::Freeplay(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	const int Group = m_aFightGroup[ClientId];
	if(!CheckFightLeader(ClientId, "toggle freeplay"))
		return;
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused())
	{
		GameServer()->SendChatTarget(ClientId, "You must be playing and unpaused to toggle freeplay.");
		return;
	}
	bool &Freeplay = m_aSettings[Group].m_Freeplay;
	Freeplay = !Freeplay;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		if(Freeplay && m_aFightGroup[Other] == Group)
		{
			EndAttempt(Other, false);
			m_aAttempts[Other].m_vRoute.clear();
		}
	SendFightChat(Group, Freeplay ? "Freeplay enabled. Flags and paths are hidden; captures are disabled." : "Freeplay disabled. Starting a new training attempt.");
	if(!Freeplay)
	{
		if(FightSize(Group) > 1)
			RestartFight(Group);
		else if(pPlayer->GetCharacter())
			PlaceRandomly(pPlayer);
		else
			m_aPendingPlace[ClientId] = true;
	}
}

void CGameControllerGTrain::Retry(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused())
	{
		GameServer()->SendChatTarget(ClientId, "You must be playing and unpaused to retry.");
		return;
	}
	const int Group = m_aFightGroup[ClientId];
	const CAttempt &Attempt = m_aAttempts[Group];
	if(!Attempt.m_pGoal || Attempt.m_Start < 0)
	{
		GameServer()->SendChatTarget(ClientId, "There is no training attempt to retry yet.");
		return;
	}
	// A leader retry also cancels a pending new attempt after their death.
	// Peers keep their own state and respawn normally at the current goal.
	if(Group == ClientId || m_aTeamGroup[Group])
		m_aFightRestart[Group] = false;
	// Teams retry together, whoever asks.
	if(m_aTeamGroup[Group])
	{
		const std::shared_ptr<CGTrainGoal> pGoal = Attempt.m_pGoal;
		PlaceGroup(Group, pGoal, Attempt.m_Start, Attempt.m_TargetDistance, true);
		return;
	}
	PlaceForAttempt(pPlayer, Attempt.m_pGoal, Attempt.m_Start, Attempt.m_TargetDistance, true);
}

void CGameControllerGTrain::TeleCursor(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	const int Group = m_aFightGroup[ClientId];
	if(!m_aSettings[Group].m_Freeplay)
	{
		if(pPlayer->GetTeam() != TEAM_SPECTATORS && pPlayer->IsPaused())
			GameServer()->SendChatTarget(ClientId, "You must be playing and unpaused to retry. In /freeplay, /tc in /spec starts at the middle of your view.");
		else
			Retry(ClientId);
		return;
	}
	// The start is the fight's, retries and member respawns return to it.
	if(!CheckFightLeader(ClientId, "choose the start position"))
		return;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || !pChr)
	{
		GameServer()->SendChatTarget(ClientId, "You must be playing to choose a start position.");
		return;
	}
	// Force pauses by admins are positive, the player's own negative.
	if(pPlayer->IsPaused() > 0)
	{
		GameServer()->SendChatTarget(ClientId, "You are force-paused and can't choose a start position.");
		return;
	}
	if(m_vTrainPositions.empty())
	{
		GameServer()->SendChatTarget(ClientId, "Training is disabled on this map.");
		return;
	}
	// In /spec or /pause, it is the middle of the view, which the free view
	// moves anywhere on the map, or the spectated player's position.
	const bool Spectating = pPlayer->IsPaused();
	vec2 Pos = pPlayer->m_ViewPos;
	if(!Spectating)
	{
		const vec2 Target(pChr->Core()->m_Input.m_TargetX, pChr->Core()->m_Input.m_TargetY);
		Pos = pPlayer->m_CameraInfo.ConvertTargetToWorld(pChr->GetPos(), Target);
	}
	// Any tile paths can go through, except teleporters moving the tee away
	// right away. Unlike random spots, it needs no freeze below.
	CCollision *pCollision = GameServer()->Collision();
	const int Width = pCollision->GetWidth();
	int Start = -1;
	if(Pos.x >= 0 && Pos.y >= 0 && Pos.x < Width * 32.0f && Pos.y < pCollision->GetHeight() * 32.0f)
	{
		const int Index = (int)(Pos.y / 32) * Width + (int)(Pos.x / 32);
		if(!pCollision->IsTeleport(Index) && !pCollision->IsEvilTeleport(Index))
			Start = m_vTileNodes[Index];
	}
	if(Start < 0)
	{
		GameServer()->SendChatTarget(ClientId, Spectating ? "Move the middle of your view to a free tile between the start and the finish line to start there." : "Aim at a free tile between the start and the finish line to start there.");
		return;
	}
	// Starting there ends the pause, before the placement respawns the tee.
	if(Spectating)
		pPlayer->Pause(CPlayer::PAUSE_NONE, true);
	RestartFight(Group, Start);
}

void CGameControllerGTrain::Distance(int ClientId, std::optional<int> Tiles)
{
	if(!GameServer()->m_apPlayers[ClientId])
		return;
	const int Group = m_aFightGroup[ClientId];
	char aMessage[160];
	if(!Tiles)
	{
		str_format(aMessage, sizeof(aMessage), "Goal distance: %d tiles%s. Use /distance %d-%d to change it, 0 for the server default.",
			GoalDistance(Group), m_aSettings[Group].m_GoalDistance ? "" : " (server default)", MinGoalDistance(), (int)MAX_GOAL_DISTANCE);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	if(!CheckFightLeader(ClientId, "change the goal distance"))
		return;
	// Short goals mostly train the easy parts right after a start.
	if(*Tiles != 0 && (*Tiles < MinGoalDistance() || *Tiles > MAX_GOAL_DISTANCE))
	{
		str_format(aMessage, sizeof(aMessage), "The goal distance must be between %d and %d tiles%s, 0 for the server default.", MinGoalDistance(), (int)MAX_GOAL_DISTANCE, IsDeranged() ? " on Deranged maps" : "");
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	// The current goal stays, so retries keep their distance.
	m_aSettings[Group].m_GoalDistance = *Tiles;
	str_format(aMessage, sizeof(aMessage), "Goal distance set to %d tiles%s for new goals.", GoalDistance(Group), *Tiles ? "" : " (server default)");
	SendFightChat(Group, aMessage);
}

void CGameControllerGTrain::NoDoubleJump(int ClientId)
{
	if(!GameServer()->m_apPlayers[ClientId] || !CheckFightLeader(ClientId, "toggle /nodj"))
		return;
	bool &NoDoubleJump = m_aSettings[ClientId].m_NoDoubleJump;
	NoDoubleJump = !NoDoubleJump;
	SendFightChat(ClientId, NoDoubleJump ? "Attempts now start without double jump. Touching the ground gives it back." : "Attempts now start with double jump.");
}

void CGameControllerGTrain::Forwards(int ClientId)
{
	if(!GameServer()->m_apPlayers[ClientId] || !CheckFightLeader(ClientId, "toggle /forwards"))
		return;
	// The current goal stays, so retries keep their direction.
	bool &Forwards = m_aSettings[ClientId].m_Forwards;
	Forwards = !Forwards;
	SendFightChat(ClientId, Forwards ? "New goals only lead towards the finish line." : "New goals lead in any direction again.");
}

void CGameControllerGTrain::Trail(int ClientId)
{
	if(!GameServer()->m_apPlayers[ClientId])
		return;
	bool &HideTrail = m_aHideTrail[ClientId];
	HideTrail = !HideTrail;
	GameServer()->SendChatTarget(ClientId, HideTrail ? "The trail to the flag is hidden." : "The trail to the flag is shown again.");
}

void CGameControllerGTrain::ScoreLimit(int ClientId, std::optional<int> Limit)
{
	if(!GameServer()->m_apPlayers[ClientId])
		return;
	const int Group = m_aFightGroup[ClientId];
	char aMessage[160];
	if(!Limit)
	{
		char aLimit[16] = "none";
		if(m_aSettings[Group].m_ScoreLimit > 0)
			str_format(aLimit, sizeof(aLimit), "%d", m_aSettings[Group].m_ScoreLimit);
		str_format(aMessage, sizeof(aMessage), "Fight score limit: %s. Use /scorelimit 1-%d to change it, 0 for no limit.", aLimit, (int)MAX_SCORE_LIMIT);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	if(!CheckFightLeader(ClientId, "change the score limit"))
		return;
	if(*Limit < 0 || *Limit > MAX_SCORE_LIMIT)
	{
		str_format(aMessage, sizeof(aMessage), "The score limit must be between 1 and %d, 0 for no limit.", (int)MAX_SCORE_LIMIT);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	m_aSettings[Group].m_ScoreLimit = *Limit;
	if(*Limit > 0)
		str_format(aMessage, sizeof(aMessage), "The first to %d points wins the fight.", *Limit);
	else
		str_copy(aMessage, "The fight score limit is disabled.");
	SendFightChat(Group, aMessage);
}

static const char *const s_apWeaponNames[NUM_WEAPONS] = {"hammer", "gun", "shotgun", "grenade", "laser", "ninja"};

void CGameControllerGTrain::ToggleWeapon(int ClientId, int Weapon)
{
	const char *pName = s_apWeaponNames[Weapon];
	char aMessage[128];
	str_format(aMessage, sizeof(aMessage), "toggle /%s", pName);
	if(!GameServer()->m_apPlayers[ClientId] || !CheckFightLeader(ClientId, aMessage))
		return;
	int &Weapons = m_aSettings[ClientId].m_Weapons;
	Weapons ^= 1 << Weapon;
	const bool Enabled = Weapons & (1 << Weapon);
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(Other);
		if(m_aFightGroup[Other] == ClientId && pChr && pChr->GetWeaponGot(Weapon) != Enabled)
			pChr->GiveWeapon(Weapon, !Enabled);
	}
	if(Enabled)
		str_format(aMessage, sizeof(aMessage), "You keep the %s in every attempt until /%s is used again.", pName, pName);
	else
		str_format(aMessage, sizeof(aMessage), "The %s was removed.", pName);
	SendFightChat(ClientId, aMessage);
}

void CGameControllerGTrain::ShowSettings(int ClientId, const char *pName)
{
	int Target = ClientId;
	if(pName[0])
	{
		Target = GameServer()->FindClientIdByName(pName).value_or(-1);
		if(Target < 0 || !GameServer()->m_apPlayers[Target])
		{
			GameServer()->SendChatTarget(ClientId, "Player not found. Use their full name.");
			return;
		}
	}
	// Fight leaders choose the settings of everyone in their fight.
	const int Group = m_aFightGroup[Target];
	const CSettings &Settings = m_aSettings[Group];
	char aWhose[128];
	if(Target == ClientId)
		str_copy(aWhose, "Your settings");
	else
		str_format(aWhose, sizeof(aWhose), "Settings of '%s'", Server()->ClientName(Target));
	// The trail is the player's own, the rest comes from the leader.
	char aFromLeader[64] = "";
	if(FightSize(Group) > 1)
	{
		if(Group == Target)
			str_format(aWhose + str_length(aWhose), sizeof(aWhose) - str_length(aWhose), " (%s leader)", GroupName(Group));
		else
			str_format(aFromLeader, sizeof(aFromLeader), "from %s leader '%s': ", GroupName(Group), Server()->ClientName(Group));
	}
	char aWeapons[64] = "";
	for(int Weapon = WEAPON_SHOTGUN; Weapon <= WEAPON_NINJA; ++Weapon)
	{
		if(!(Settings.m_Weapons & (1 << Weapon)))
			continue;
		if(aWeapons[0])
			str_append(aWeapons, ", ");
		str_append(aWeapons, s_apWeaponNames[Weapon]);
	}
	char aScoreLimit[16] = "off";
	if(Settings.m_ScoreLimit > 0)
		str_format(aScoreLimit, sizeof(aScoreLimit), "%d", Settings.m_ScoreLimit);
	char aMessage[256];
	str_format(aMessage, sizeof(aMessage), "%s: /trail %s, %s/distance %d%s, /nodj %s, /forwards %s, /freeplay %s, /scorelimit %s, kept weapons: %s",
		aWhose, m_aHideTrail[Target] ? "off" : "on", aFromLeader, GoalDistance(Group), Settings.m_GoalDistance ? "" : " (server default)", Settings.m_NoDoubleJump ? "on" : "off",
		Settings.m_Forwards ? "on" : "off", Settings.m_Freeplay ? "on" : "off", aScoreLimit, aWeapons[0] ? aWeapons : "none");
	GameServer()->SendChatTarget(ClientId, aMessage);
}

void CGameControllerGTrain::Help()
{
	log_info("chatresp", "GTrain commands:");
	log_info("chatresp", "Kill: new position and flag");
	log_info("chatresp", "/r or /retry: retry the current position and flag");
	log_info("chatresp", "/tp, /teleport, /lasttp, /back, /rescue, /invincible: retry like /r, for practice binds");
	log_info("chatresp", "/tc or /telecursor: retry like /r, in freeplay start where you aim, or in /spec at the middle of your view");
	log_info("chatresp", "/fight <player>: join a player's fight, /fight alone leaves it");
	log_info("chatresp", "/team <player>: ask a player to play together, /accept or /a joins, /team alone leaves; teams earn no ELO");
	log_info("chatresp", "/distance <%d-%d>: goal distance in tiles, 0 for the server default", MinGoalDistance(), (int)MAX_GOAL_DISTANCE);
	log_info("chatresp", "/nodj: toggle starting attempts without double jump");
	log_info("chatresp", "/forwards: toggle goals only leading towards the finish line");
	log_info("chatresp", "/scorelimit <0-%d>: points to win a fight, 0 for no limit", (int)MAX_SCORE_LIMIT);
	log_info("chatresp", "/shotgun, /grenade, /laser or /rifle, /ninja: toggle keeping that weapon");
	log_info("chatresp", "/freeplay: toggle training without flags and paths");
	log_info("chatresp", "/trail: toggle showing the trail to the flag, just for you");
	log_info("chatresp", "/settings [player]: show your settings, or another player's");
	log_info("chatresp", "In a fight or team, only the leader (*name*) changes these settings.");
	log_info("chatresp", "/points or /elo [player]: show ELO, /top5, /top, /top5points or /top5elo: the ELO ladder");
	log_info("chatresp", "/rank or /profile [player]: stats and playtime, /top5captures, /top5attempts, /top5respawns, /top5playtime: their ladders");
	log_info("chatresp", "/map <name>: vote a map, /mapinfo: map details, /cmdlist: every chat command");
	if(m_EloStep)
	{
		char aDifficulty[32];
		if(*m_EloStep == GTrainElo::DERANGED_STEP)
			str_copy(aDifficulty, s_pDeranged);
		else
			str_format(aDifficulty, sizeof(aDifficulty), "%s %s", s_apEloDifficulties[*m_EloStep / std::size(s_apEloStars)], s_apEloStars[*m_EloStep % std::size(s_apEloStars)]);
		if(IsDeranged())
			log_info("chatresp", "Captures here are worth %.1f times as much as on Easy ★☆☆ maps (%s), longer ones more. Speed doesn't count, goals have at least %d tiles.", GTrainElo::PointsPerTile(*m_EloStep) / GTrainElo::PointsPerTile(0), aDifficulty, (int)DERANGED_MIN_GOAL_DISTANCE);
		else
			log_info("chatresp", "Captures here are worth %.1f times as much as on Easy ★☆☆ maps (%s), longer and faster ones more.", GTrainElo::PointsPerTile(*m_EloStep) / GTrainElo::PointsPerTile(0), aDifficulty);
	}
	else
		log_info("chatresp", "Captures on this map earn no ELO.");
}

std::optional<int> CGameControllerGTrain::RaceTimerStartTick(CCharacter *pChr)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	const CAttempt &Attempt = m_aAttempts[ClientId];
	if(Attempt.m_Start < 0)
		return CGameControllerDDNet::RaceTimerStartTick(pChr);
	// Every attempt times itself: zero while hovering, counting afterwards.
	if(m_aPendingPlace[ClientId] || Attempt.m_StartTick < 0)
		return Server()->Tick();
	return Attempt.m_StartTick;
}

int CGameControllerGTrain::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	const int Result = CGameControllerDDNet::OnCharacterDeath(pVictim, pKiller, Weapon);
	const int ClientId = pVictim->GetPlayer()->GetCid();
	const int Group = m_aFightGroup[ClientId];
	// Replaced characters and peers dying with their fight leader keep their
	// stats until they are placed again.
	if(m_SyncFightDeaths || !pVictim->IsAlive())
		return Result;
	EndAttempt(ClientId, false);
	// Solo players, fight leaders and team members give up their spot by
	// dying, a respawn once they tried it. Teams start over together.
	CAttempt &Attempt = m_aAttempts[ClientId];
	const bool Gives = ClientId == Group || m_aTeamGroup[Group];
	if(Gives && Attempt.m_SpotTried)
	{
		Attempt.m_SpotTried = false;
		CGTrainStats Stats;
		Stats.m_Respawns = 1;
		AddStats(ClientId, Stats);
	}
	if(!Gives || FightSize(Group) <= 1)
		return Result;
	m_aFightRestart[Group] = true;
	// Death callbacks run before the victim is marked dead. Suppress recursive
	// propagation while killing its peers; never delete the original victim.
	m_SyncFightDeaths = true;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		if(Other != ClientId && m_aFightGroup[Other] == Group && GameServer()->GetPlayerChar(Other))
			GameServer()->m_apPlayers[Other]->KillCharacter(WEAPON_SELF);
	m_SyncFightDeaths = false;
	return Result;
}

int CGameControllerGTrain::PathNode(vec2 Pos) const
{
	// Do not use the collision lookup's clamping outside the map.
	const int Width = GameServer()->Collision()->GetWidth();
	const int Height = GameServer()->Collision()->GetHeight();
	if(Pos.x < 0 || Pos.y < 0 || Pos.x >= Width * 32.0f || Pos.y >= Height * 32.0f)
		return -1;
	// Tees falling through freeze are on its falling node.
	const int Index = (int)(Pos.y / 32) * Width + (int)(Pos.x / 32);
	return m_vTileNodes[Index] >= 0 ? m_vTileNodes[Index] : m_vFallNodes[Index];
}

void CGameControllerGTrain::UpdatePath(CCharacter *pChr)
{
	CAttempt &Attempt = m_aAttempts[pChr->GetPlayer()->GetCid()];
	if(!Attempt.m_pGoal || !Attempt.m_pGoal->m_Ready)
		return;
	const int Node = Attempt.m_StartTick < 0 ? Attempt.m_Start : PathNode(pChr->m_Pos);
	if(Node == Attempt.m_RouteNode)
		return;
	if(Node < 0)
	{
		Attempt.m_RouteNode = -1;
		return;
	}
	// Normal movement and small backwards steps keep the drawn route stable.
	const size_t Begin = Attempt.m_RouteIndex > 8 ? Attempt.m_RouteIndex - 8 : 0;
	const size_t End = std::min(Attempt.m_vRoute.size(), Attempt.m_RouteIndex + 9);
	for(size_t i = Begin; i < End; ++i)
		if(Attempt.m_vRoute[i] == Node)
		{
			Attempt.m_RouteIndex = i;
			Attempt.m_RouteNode = Node;
			return;
		}
	// Deviations follow the goal's distance field, costing no search. Far
	// beyond the field the route stays hidden until the player returns.
	m_Pathfinder.FindPath(*Attempt.m_pGoal, Node, Attempt.m_vRoute);
	Attempt.m_RouteIndex = 0;
	Attempt.m_RouteNode = Node;
}

bool CGameControllerGTrain::AnnounceFightScore(int Winner, const std::optional<CEloGain> &Gain)
{
	const int Group = m_aFightGroup[Winner];
	const int ScoreLimit = m_aSettings[Group].m_ScoreLimit;
	++m_aFightWins[Winner];
	Server()->SetClientScore(Winner, SnapPlayerScore(Winner, GameServer()->m_apPlayers[Winner]));
	char aScore[64] = "Fight score:";
	if(ScoreLimit > 0)
		str_format(aScore, sizeof(aScore), "Fight score (first to %d):", ScoreLimit);
	char aResult[64];
	FormatCaptureResult(aResult, sizeof(aResult), m_aAttempts[Winner].m_Fails, Gain);
	char aMessage[MAX_CHAT_LENGTH];
	str_format(aMessage, sizeof(aMessage), "'%s' captured the flag (%s). %s", Server()->ClientName(Winner), aResult, aScore);
	bool First = true;
	// Leader first, then the remaining members. Split long standings into
	// complete chat messages rather than truncating names or scores.
	for(int i = -1; i < MAX_CLIENTS; ++i)
	{
		const int Other = i < 0 ? Group : i;
		if(i == Group || m_aFightGroup[Other] != Group || !GameServer()->m_apPlayers[Other])
			continue;
		char aEntry[128];
		str_format(aEntry, sizeof(aEntry), "%s '%s': %d", First ? "" : ",", Server()->ClientName(Other), m_aFightWins[Other]);
		if(str_length(aMessage) + str_length(aEntry) >= (int)sizeof(aMessage))
		{
			GameServer()->SendChat(-1, TEAM_ALL, aMessage);
			str_copy(aMessage, aScore);
			str_format(aEntry, sizeof(aEntry), " '%s': %d", Server()->ClientName(Other), m_aFightWins[Other]);
		}
		str_append(aMessage, aEntry);
		First = false;
	}
	GameServer()->SendChat(-1, TEAM_ALL, aMessage);

	if(ScoreLimit <= 0 || m_aFightWins[Winner] < ScoreLimit)
		return false;
	str_format(aMessage, sizeof(aMessage), "'%s' won the fight with %d points! The fight score starts over.", Server()->ClientName(Winner), m_aFightWins[Winner]);
	GameServer()->SendChat(-1, TEAM_ALL, aMessage);
	// The fight sees its final standings during the break, best first. Names
	// that do not fit are left out, the chat has them all.
	std::vector<int> vStandings;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		if(m_aFightGroup[Other] == Group && GameServer()->m_apPlayers[Other])
			vStandings.push_back(Other);
	std::stable_sort(vStandings.begin(), vStandings.end(), [&](int A, int B) { return m_aFightWins[A] > m_aFightWins[B]; });
	char aBroadcast[512];
	str_format(aBroadcast, sizeof(aBroadcast), "'%s' won the fight!\n", Server()->ClientName(Winner));
	for(size_t i = 0; i < vStandings.size(); ++i)
	{
		char aEntry[128];
		str_format(aEntry, sizeof(aEntry), "%s'%s': %d", i ? ", " : "", Server()->ClientName(vStandings[i]), m_aFightWins[vStandings[i]]);
		if(str_length(aBroadcast) + str_length(aEntry) >= (int)sizeof(aBroadcast))
			break;
		str_append(aBroadcast, aEntry);
	}
	for(const int Other : vStandings)
	{
		GameServer()->SendBroadcast(aBroadcast, Other);
		m_aFightWins[Other] = 0;
		Server()->SetClientScore(Other, SnapPlayerScore(Other, GameServer()->m_apPlayers[Other]));
	}
	return true;
}

void CGameControllerGTrain::CheckGoal(CCharacter *pChr)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	CAttempt &Attempt = m_aAttempts[ClientId];
	const CGTrainGoal &Target = *Attempt.m_pGoal;

	const vec2 Goal = m_PathGraph.m_vNodes[Target.m_Goal].m_Pos;
	// Check the movement segment too, so a fast tee cannot skip the flag.
	// A teleport's long displacement must not collect a flag along the way.
	vec2 Closest = pChr->m_Pos;
	const vec2 Movement = pChr->m_Pos - pChr->m_PrevPos;
	const float MovementSquared = dot(Movement, Movement);
	if(MovementSquared > 0 && MovementSquared <= 256.0f * 256.0f)
		Closest = pChr->m_PrevPos + Movement * std::clamp(dot(Goal - pChr->m_PrevPos, Movement) / MovementSquared, 0.0f, 1.0f);
	const vec2 GoalOffset = Closest - Goal;
	if(dot(GoalOffset, GoalOffset) < 42.0f * 42.0f)
	{
		const int Group = m_aFightGroup[ClientId];
		const bool Frozen = pChr->m_FreezeTime > 0 || pChr->Core()->m_DeepFrozen || pChr->Core()->m_LiveFrozen;
		const std::optional<CEloGain> Gain = AwardElo(ClientId, Frozen);
		// The runs stored are those of rated play.
		if(Gain && !Gain->m_pNoElo)
			SaveRun(pChr, *Gain);
		EndAttempt(ClientId, true);
		if(m_aTeamGroup[Group])
		{
			char aResult[64];
			FormatCaptureResult(aResult, sizeof(aResult), Attempt.m_Fails, Gain);
			char aMessage[256];
			const double Seconds = (Server()->Tick() - Attempt.m_StartTick) / (double)Server()->TickSpeed();
			str_format(aMessage, sizeof(aMessage), "'%s' captured the flag for their team in %.2f seconds (%s).", Server()->ClientName(ClientId), Seconds, aResult);
			GameServer()->SendChat(-1, TEAM_ALL, aMessage);
			RestartFight(Group);
		}
		else if(FightSize(Group) > 1)
		{
			if(AnnounceFightScore(ClientId, Gain))
				m_aFightBreakEndTick[Group] = Server()->Tick() + FIGHT_BREAK_SECONDS * Server()->TickSpeed();
			RestartFight(Group);
		}
		else
		{
			char aResult[64];
			FormatCaptureResult(aResult, sizeof(aResult), Attempt.m_Fails, Gain);
			char aMessage[256];
			const double Seconds = (Server()->Tick() - Attempt.m_StartTick) / (double)Server()->TickSpeed();
			str_format(aMessage, sizeof(aMessage), "'%s' captured the flag in %.2f seconds (%s).", Server()->ClientName(ClientId), Seconds, aResult);
			GameServer()->SendChat(-1, TEAM_ALL, aMessage);
			PlaceRandomly(pChr->GetPlayer());
		}
		for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		{
			CCharacter *pOther = GameServer()->GetPlayerChar(Other);
			if(m_aFightGroup[Other] != Group || !pOther)
				continue;
			if(Server()->IsSixup(Other))
				GameServer()->CreateSound(pOther->m_Pos, SOUND_CTF_CAPTURE, CClientMask().set(Other));
			else
				GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE, Other);
		}
	}
}

void CGameControllerGTrain::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	const int ClientId = pPlayer->GetCid();
	LeaveFight(ClientId, false);
	m_aFightBreakEndTick[ClientId] = 0;
	m_aTeamRequest[ClientId] = -1;
	for(int &Target : m_aTeamRequest)
		if(Target == ClientId)
			Target = -1;
	EndAttempt(ClientId, false);
	SaveStats(ClientId);
	m_aPendingPlace[ClientId] = false;
	m_aAttempts[ClientId] = CAttempt();
	m_aFightWins[ClientId] = 0;
	m_aSettings[ClientId] = CSettings();
	m_aHideTrail[ClientId] = false;
	m_aStats[ClientId] = CGTrainStats();
	m_apStatsResults[ClientId] = nullptr;
	m_aaStatsNames[ClientId][0] = '\0';
	m_aPlaytimeTicks[ClientId] = 0;
	CGameControllerDDNet::OnPlayerDisconnect(pPlayer, pReason);
}

void CGameControllerGTrain::OnPlayerConnect(CPlayer *pPlayer)
{
	CGameControllerDDNet::OnPlayerConnect(pPlayer);
	LoadStats(pPlayer->GetCid());
	UpdateFightTeams();
}

void CGameControllerGTrain::Snap(int SnappingClient)
{
	CGameControllerDDNet::Snap(SnappingClient);
	if(SnappingClient == SERVER_DEMO_CLIENT)
		return;
	CPlayer *pPlayer = GameServer()->m_apPlayers[SnappingClient];
	if(!pPlayer)
		return;
	int ClientId = SnappingClient;
	if((pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused()) && pPlayer->SpectatorId() != SPEC_FREEVIEW)
		ClientId = pPlayer->SpectatorId();
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
	// Only the held player gets their door, as a short laser on its tile.
	const CAttempt &Attempt = m_aAttempts[ClientId];
	if(pChr && Attempt.m_DoorIndex >= 0 && Attempt.m_StartTick < 0 && m_aDoorSnapIds[ClientId] &&
		GameServer()->Switchers()[m_DoorSwitch].m_aStatus[pChr->Team()])
	{
		const vec2 DoorPos = GameServer()->Collision()->GetPos(Attempt.m_DoorIndex);
		const CSnapContext Context(GameServer()->GetClientVersion(SnappingClient), Server()->IsSixup(SnappingClient), SnappingClient);
		GameServer()->SnapLaserObject(Context, *m_aDoorSnapIds[ClientId], DoorPos - vec2(12, 0), DoorPos + vec2(12, 0), -1, -1, LASERTYPE_DOOR, 0, m_DoorSwitch);
	}
	if(m_aSettings[m_aFightGroup[ClientId]].m_Freeplay)
		return;
	if(!pChr || !Attempt.m_pGoal || !Attempt.m_pGoal->m_Ready || !m_FlagSnapId)
		return;
	const vec2 Goal = m_PathGraph.m_vNodes[Attempt.m_pGoal->m_Goal].m_Pos;
	CNetObj_Flag Flag = {};
	Flag.m_X = (int)Goal.x;
	Flag.m_Y = (int)Goal.y;
	Flag.m_Team = TEAM_BLUE;
	Server()->SnapNewItem(*m_FlagSnapId, Flag);

	// The viewer's choice, also when watching someone else.
	if(m_aHideTrail[SnappingClient] || Attempt.m_RouteNode < 0 || Attempt.m_vRoute.empty())
		return;
	int NumParticles = 0;
	const float ClearRadius = g_Config.m_SvGtrainPathClearRadius;
	const float Spacing = g_Config.m_SvGtrainPathParticleSpacing;
	float DistanceToNext = Spacing;
	std::optional<vec2> LastParticle;
	const auto Emit = [&](vec2 Pos) {
		// Leave gameplay space clear, including where a winding path comes
		// back near the player. Only snapshot particles inside the viewport.
		Pos = vec2(round_to_int(Pos.x), round_to_int(Pos.y));
		const vec2 Offset = Pos - pChr->m_Pos;
		if(dot(Offset, Offset) < ClearRadius * ClearRadius || NetworkClipped(GameServer(), SnappingClient, Pos) || NumParticles == NUM_DIRECTION_PARTICLES)
			return;
		const auto SnapId = m_aDirectionSnapIds[NumParticles++];
		if(!SnapId)
			return;
		CNetObj_Projectile Particle = {};
		Particle.m_X = round_to_int(Pos.x);
		Particle.m_Y = round_to_int(Pos.y);
		// Hammer projectiles have no sprite but use the same bullet trail as
		// the gun. Zero velocity keeps them still and skips client prediction.
		Particle.m_Type = WEAPON_HAMMER;
		Server()->SnapNewItem(*SnapId, Particle);
		LastParticle = Pos;
	};
	// Carry the spacing across tile boundaries and turns. Sampling each tile
	// separately would still produce at least one particle per tile.
	const auto SampleSegment = [&](vec2 From, vec2 To) {
		const float Length = distance(From, To);
		if(Length == 0)
			return;
		while(DistanceToNext <= Length && NumParticles < NUM_DIRECTION_PARTICLES)
		{
			Emit(mix(From, To, DistanceToNext / Length));
			DistanceToNext += Spacing;
		}
		DistanceToNext -= Length;
	};
	vec2 Incoming = m_PathGraph.m_vNodes[Attempt.m_vRoute[Attempt.m_RouteIndex]].m_Pos;
	// Bounded traversal and a shared snapshot-ID pool: no per-snapshot heap
	// allocations or map searches, even with very distant goals.
	for(int Step = 0; Step < MAX_PATH_NODES && NumParticles < NUM_DIRECTION_PARTICLES; ++Step)
	{
		const size_t Index = Attempt.m_RouteIndex + Step;
		if(Index >= Attempt.m_vRoute.size())
			break;
		const int Node = Attempt.m_vRoute[Index];
		const vec2 Center = m_PathGraph.m_vNodes[Node].m_Pos;
		const int Next = Index + 1 < Attempt.m_vRoute.size() ? Attempt.m_vRoute[Index + 1] : -1;
		if(Next < 0)
		{
			if(Node == Attempt.m_pGoal->m_Goal)
			{
				SampleSegment(Incoming, Center);
				// Keep an endpoint even for a route shorter than the spacing.
				if(!LastParticle || *LastParticle != Center)
					Emit(Center);
			}
			break;
		}
		if(m_PathGraph.IsTeleportStep(Node, Next))
		{
			// A teleport is a discontinuity, not a line across the map.
			Incoming = m_PathGraph.m_vNodes[Next].m_Pos;
			DistanceToNext = Spacing;
			continue;
		}
		const vec2 Outgoing = (Center + m_PathGraph.m_vNodes[Next].m_Pos) * 0.5f;
		// Round turns through each tile center. This curve stays within the
		// traversable tiles and avoids a staircase of right-angle corners.
		const vec2 In = Center - Incoming, Out = Outgoing - Center;
		if(In.x * Out.y == In.y * Out.x)
		{
			SampleSegment(Incoming, Outgoing);
		}
		else
		{
			// Four short segments approximate the rounded turn safely inside
			// its tile; only points at the configured spacing are emitted.
			vec2 Previous = Incoming;
			for(int i = 1; i <= 4; ++i)
			{
				const float t = i * 0.25f;
				const vec2 Pos = mix(mix(Incoming, Center, t), mix(Center, Outgoing, t), t);
				SampleSegment(Previous, Pos);
				Previous = Pos;
			}
		}
		Incoming = Outgoing;
	}
}

void CGameControllerGTrain::Tick()
{
	CGameControllerDDNet::Tick();

	if(m_pMapDifficultyResult && m_pMapDifficultyResult->m_Completed)
	{
		if(m_pMapDifficultyResult->m_Success)
			SetMapDifficulty(m_pMapDifficultyResult->m_aServer, m_pMapDifficultyResult->m_Stars);
		m_pMapDifficultyResult = nullptr;
	}
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		auto &pStatsResult = m_apStatsResults[ClientId];
		if(pStatsResult && pStatsResult->m_Completed)
		{
			if(pStatsResult->m_Success)
				m_aStats[ClientId].Add(pStatsResult->m_Stats);
			pStatsResult = nullptr;
		}
		// The server browser shows the same scores. Loading race times after a
		// name change would replace them otherwise.
		if(CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId])
			Server()->SetClientScore(ClientId, SnapPlayerScore(ClientId, pPlayer));
	}
	// Stats are saved in batches, crashes lose at most this much.
	if(Server()->Tick() % (STATS_SAVE_SECONDS * Server()->TickSpeed()) == 0)
		SaveAllStats();

	// killing is how players choose a new position, never delay it
	g_Config.m_SvKillDelay = 0;
	if(IsGamePaused())
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			// Replays have no pauses, so these runs are not stored.
			m_aAttempts[ClientId].m_Run.reset();
			if(m_aAttempts[ClientId].m_StartTick >= 0)
				++m_aAttempts[ClientId].m_StartTick;
			// Breaks last as long as the hovers they hold.
			if(m_aFightBreakEndTick[ClientId] > Server()->Tick())
				++m_aFightBreakEndTick[ClientId];
			if(CCharacter *pChr = GameServer()->GetPlayerChar(ClientId))
			{
				KeepWeapons(pChr);
				DelayHoverDoor(pChr);
			}
		}
		return;
	}

	// Playtime counts while playing, not while paused or spectating.
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused() || ++m_aPlaytimeTicks[ClientId] < Server()->TickSpeed())
			continue;
		m_aPlaytimeTicks[ClientId] = 0;
		CGTrainStats Stats;
		Stats.m_Playtime = 1;
		AddStats(ClientId, Stats);
	}

	// Remove spectator links before group respawns, and coalesce all death
	// and spawn requests into one goal selection and one placement per group.
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer)
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			LeaveFight(ClientId, false);
		else if(m_aPendingPlace[ClientId] && (m_aFightGroup[ClientId] == ClientId || InTeam(ClientId)) && FightSize(m_aFightGroup[ClientId]) > 1)
			m_aFightRestart[m_aFightGroup[ClientId]] = true;
	}
	for(int Group = 0; Group < MAX_CLIENTS; ++Group)
		if(m_aFightRestart[Group])
			RestartFight(Group);

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		if(!pChr)
			continue;
		KeepWeapons(pChr);
		if(pChr->IsPaused())
		{
			m_aAttempts[ClientId].m_Run.reset();
			if(m_aAttempts[ClientId].m_StartTick >= 0)
				++m_aAttempts[ClientId].m_StartTick;
			DelayHoverDoor(pChr);
			continue;
		}

		if(m_aPendingPlace[ClientId])
		{
			const int Group = m_aFightGroup[ClientId];
			const CAttempt &GroupAttempt = m_aAttempts[Group];
			// A member's own respawn rejoins the current attempt without
			// moving peers or rebuilding their shared goal. Players already
			// in that attempt count it as a fail, joiners start fresh.
			if(Group != ClientId && FightSize(Group) > 1 && GroupAttempt.m_pGoal && GroupAttempt.m_Start >= 0)
				PlaceForAttempt(pChr->GetPlayer(), GroupAttempt.m_pGoal, GroupAttempt.m_Start, GroupAttempt.m_TargetDistance, true);
			else
				PlaceRandomly(pChr->GetPlayer());
			continue;
		}
		CAttempt &Attempt = m_aAttempts[ClientId];
		// The input the server applied in this tick. Replays have no pauses,
		// in which tees keep their last input, nor weapons changing midway.
		if(Attempt.m_Run)
		{
			if(pChr->GetPlayer()->IsPaused() || (int)Attempt.m_Run->m_vInputs.size() == CGTrainRun::MAX_TICKS ||
				Attempt.m_Run->m_KeptWeapons != m_aSettings[m_aFightGroup[ClientId]].m_Weapons)
				Attempt.m_Run.reset();
			else
				Attempt.m_Run->m_vInputs.push_back(GameServer()->GetLastPlayerInput(ClientId));
		}

		// Standing on the door gives the double jump back, /nodj takes it.
		if(Attempt.m_StartTick < 0 && Attempt.m_DoorIndex >= 0 && m_aSettings[m_aFightGroup[ClientId]].m_NoDoubleJump)
			pChr->UseUpAirJumps();
		// hover is over, let the player play again
		if(Attempt.m_StartTick < 0 && pChr->m_FreezeTime == 0)
		{
			if(pChr->m_ZeroGravity)
				pChr->SetZeroGravity(false);
			OpenHoverDoor(pChr);
			Attempt.m_StartTick = Server()->Tick();
			Attempt.m_Running = !m_aSettings[m_aFightGroup[ClientId]].m_Freeplay && Attempt.m_pGoal && Attempt.m_pGoal->m_Ready;
			Attempt.m_WithWeapons = m_aSettings[m_aFightGroup[ClientId]].m_Weapons != 0;
			// Team runs earn no ELO, and replays can't move the teammates.
			if(Attempt.m_Running && g_Config.m_SvGtrainRecordRuns && !InTeam(ClientId))
				StartRun(pChr);
		}
		else if(Attempt.m_StartTick >= 0 && m_aSettings[m_aFightGroup[ClientId]].m_Weapons)
			Attempt.m_WithWeapons = true;
		if(m_aSettings[m_aFightGroup[ClientId]].m_Freeplay)
			continue;
		UpdatePath(pChr);
		if(Attempt.m_StartTick >= 0 && Attempt.m_pGoal && Attempt.m_pGoal->m_Ready)
			CheckGoal(pChr);
	}
}
