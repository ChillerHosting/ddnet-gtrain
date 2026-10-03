#ifndef GAME_SERVER_GAMEMODES_GTRAIN_H
#define GAME_SERVER_GAMEMODES_GTRAIN_H

#include "ddnet.h"
#include "gtrain_path.h"

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

// Gores training: players get placed at random positions between
// the start and the finish line, killing places them again.
class CGameControllerGTrain : public CGameControllerDDNet
{
	enum
	{
		HOVER_TICKS = 50,
		MAX_FREEZE_DISTANCE = 20,
		NUM_DIRECTION_PARTICLES = 256,
		MAX_PATH_NODES = 256,
	};

	// centers of all non-freeze tiles between the start and the finish line
	std::vector<vec2> m_vTrainPositions;
	bool m_aPendingPlace[MAX_CLIENTS];
	CGTrainPathGraph m_PathGraph;
	std::vector<int> m_vTileNodes;
	CGTrainPathfinder m_Pathfinder;
	struct CAttempt
	{
		std::shared_ptr<CGTrainGoal> m_pGoal;
		int m_Start = -1;
		int m_TargetDistance = 0;
		int m_StartTick = -1;
		std::vector<int> m_vRoute;
		size_t m_RouteIndex = 0;
		int m_RouteNode = -1;
		int m_LastPathTick = -1;
	};
	CAttempt m_aAttempts[MAX_CLIENTS];
	int m_aFightGroup[MAX_CLIENTS];
	int m_aFightTeam[MAX_CLIENTS] = {};
	int m_aScoreboardTeams[MAX_CLIENTS] = {};
	bool m_aFightRestart[MAX_CLIENTS] = {};
	int m_aFightWins[MAX_CLIENTS] = {};
	bool m_SyncFightDeaths = false;
	std::optional<int> m_FlagSnapId;
	std::array<std::optional<int>, NUM_DIRECTION_PARTICLES> m_aDirectionSnapIds;

	void FindTrainPositions();
	void PlaceRandomly(class CCharacter *pChr);
	void PlaceForAttempt(class CCharacter *pChr, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance);
	void RestartFight(int Group, bool KeepGoal = false);
	bool LeaveFight(int ClientId, bool NewAttempt);
	int FightSize(int Group) const;
	void UpdateFightTeams();
	void AnnounceFightScore(int Winner);
	int PathNode(vec2 Pos) const;
	void UpdatePath(class CCharacter *pChr);
	void CheckGoal(class CCharacter *pChr);

public:
	CGameControllerGTrain(class CGameContext *pGameServer);
	~CGameControllerGTrain() override;

	void OnCharacterSpawn(class CCharacter *pChr) override;
	int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon) override;
	void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason) override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	bool HasTimeScore() const override { return false; }
	CFinishTime SnapPlayerTime(int SnappingClient, CPlayer *pPlayer) override { return CFinishTime::Unset(); }
	void Snap(int SnappingClient) override;
	void Fight(int ClientId, const char *pName);
	void Retry(int ClientId);

	void Tick() override;
};
#endif // GAME_SERVER_GAMEMODES_GTRAIN_H
