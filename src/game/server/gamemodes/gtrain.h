#ifndef GAME_SERVER_GAMEMODES_GTRAIN_H
#define GAME_SERVER_GAMEMODES_GTRAIN_H

#include "ddnet.h"
#include "gtrain_elo.h"
#include "gtrain_path.h"
#include "gtrain_run.h"

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct CScoreGTrainStatsResult;
struct CScoreMapDifficultyResult;

// Gores training: players get placed at random positions between
// the start and the finish line, killing places them again.
class CGameControllerGTrain : public CGameControllerDDNet
{
	enum
	{
		HOVER_TICKS = 50,
		// The hover after a won fight, to see its result.
		FIGHT_BREAK_SECONDS = 10,
		MAX_FREEZE_DISTANCE = 20,
		NUM_DIRECTION_PARTICLES = 256,
		MAX_PATH_NODES = 256,
		MIN_GOAL_DISTANCE = 50,
		// Short goals on Deranged maps were farmed, their goals are longer.
		DERANGED_MIN_GOAL_DISTANCE = 100,
		MAX_GOAL_DISTANCE = 1000,
		DEFAULT_SCORE_LIMIT = 10,
		MAX_SCORE_LIMIT = 1000,
		// Shorter attempts are no attempts, unless they capture the flag.
		MIN_ATTEMPT_SECONDS = 2,
		STATS_SAVE_SECONDS = 60,
		TEAM_REQUEST_SECONDS = 60,
	};

	// centers of all non-freeze tiles between the start and the finish line,
	// ordered by their path distance to the finish
	std::vector<vec2> m_vTrainPositions;
	bool m_aPendingPlace[MAX_CLIENTS];
	CGTrainPathGraph m_PathGraph;
	std::vector<int> m_vTileNodes;
	// nodes of tiles tees fall through frozen onto unfreeze, -1 elsewhere
	std::vector<int> m_vFallNodes;
	// path distance of every node to the finish line, -1 without a path
	std::vector<int> m_vFinishDistances;
	CGTrainPathfinder m_Pathfinder;
	struct CAttempt
	{
		std::shared_ptr<CGTrainGoal> m_pGoal;
		int m_Start = -1;
		int m_TargetDistance = 0;
		int m_StartTick = -1;
		// tile of the laser door holding the hovering tee, -1 for zero gravity
		int m_DoorIndex = -1;
		// retries of this start and goal, after death or /r
		int m_Fails = 0;
		// Released with a goal, until the attempt ends. Freeplay has none.
		bool m_Running = false;
		// An attempt at this start and goal counted, so giving it up by
		// dying is a respawn.
		bool m_SpotTried = false;
		// Kept weapons were on since the release, so a capture earns no ELO,
		// also once they are gone again.
		bool m_WithWeapons = false;
		std::vector<int> m_vRoute;
		size_t m_RouteIndex = 0;
		int m_RouteNode = -1;
		// Recorded from the release on, to store the run if its capture earns
		// ELO.
		std::optional<CGTrainRun> m_Run;
	};
	// Fight leaders choose these for their whole group. Leaving players and
	// promoted leaders keep the group's settings.
	struct CSettings
	{
		bool m_Freeplay = false;
		bool m_NoDoubleJump = false;
		// goals only on a shortest path towards the finish line
		bool m_Forwards = false;
		int m_GoalDistance = 0; // 0 uses sv_gtrain_goal_distance
		int m_ScoreLimit = DEFAULT_SCORE_LIMIT; // 0 disables the limit
		int m_Weapons = 0; // bit per weapon kept for good
	};
	CAttempt m_aAttempts[MAX_CLIENTS];
	// Players sharing an attempt form a group, named after its leader: a
	// fight, or a team with /team. Teams play together in one DDRace team,
	// fights race each other solo. Nobody is in both.
	int m_aFightGroup[MAX_CLIENTS];
	bool m_aTeamGroup[MAX_CLIENTS] = {};
	// The player each player asked to team up with, and when.
	int m_aTeamRequest[MAX_CLIENTS];
	int m_aTeamRequestTick[MAX_CLIENTS] = {};
	int m_aFightTeam[MAX_CLIENTS] = {};
	int m_aScoreboardTeams[MAX_CLIENTS] = {};
	bool m_aFightRestart[MAX_CLIENTS] = {};
	// Until this tick, every attempt of the group hovers, also of members
	// respawning, so none gets a head start out of the break.
	int m_aFightBreakEndTick[MAX_CLIENTS] = {};
	int m_aFightWins[MAX_CLIENTS] = {};
	CSettings m_aSettings[MAX_CLIENTS];
	// Every viewer chooses this for themselves, also in fights.
	bool m_aHideTrail[MAX_CLIENTS] = {};
	bool m_SyncFightDeaths = false;
	// switch number of the hover doors, -1 without door collision
	int m_DoorSwitch = -1;
	std::array<std::optional<int>, MAX_CLIENTS> m_aDoorSnapIds;
	std::optional<int> m_FlagSnapId;
	std::array<std::optional<int>, NUM_DIRECTION_PARTICLES> m_aDirectionSnapIds;
	std::shared_ptr<CScoreMapDifficultyResult> m_pMapDifficultyResult;
	// Difficulty step of the map for capture points, unset on maps without
	// ELO.
	std::optional<int> m_EloStep;
	// The stats of each player's name: their changes since requesting them
	// from the database, plus the loaded stats once the request completes.
	CGTrainStats m_aStats[MAX_CLIENTS];
	std::shared_ptr<CScoreGTrainStatsResult> m_apStatsResults[MAX_CLIENTS];
	// Changes not saved yet, for the name the stats were loaded for.
	CGTrainStats m_aUnsavedStats[MAX_CLIENTS];
	char m_aaStatsNames[MAX_CLIENTS][MAX_NAME_LENGTH] = {};
	int m_aPlaytimeTicks[MAX_CLIENTS] = {};

	void HideUnrelatedChatCommands();
	void FindTrainPositions();
	// path nodes beside tiles of this game or front layer index
	std::vector<int> NodesBeside(int Tile) const;
	void ExcludeStartArea();
	int RandomStart(int Group) const;
	int HoverDoorIndex(int Start, class CCharacter *pChr) const;
	void DelayHoverDoor(class CCharacter *pChr);
	void OpenHoverDoor(class CCharacter *pChr);
	void PlaceRandomly(class CPlayer *pPlayer);
	// Deletes the player's character if it played already, see the definition.
	void PlaceForAttempt(class CPlayer *pPlayer, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance, bool Retry);
	// Places the whole group at the chosen start, a random spot without one.
	void HoldHoverDoor(class CCharacter *pChr, int Hover);
	void RestartFight(int Group, std::optional<int> ChosenStart = std::nullopt);
	void PlaceGroup(int Group, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance, bool Retry);
	bool LeaveFight(int ClientId, bool NewAttempt);
	int FightSize(int Group) const;
	bool InTeam(int ClientId) const { return m_aTeamGroup[m_aFightGroup[ClientId]]; }
	const char *GroupName(int Group) const { return m_aTeamGroup[Group] ? "team" : "fight"; }
	void UpdateDDRaceTeam(class CCharacter *pChr);
	bool IsDeranged() const { return m_EloStep == GTrainElo::DERANGED_STEP; }
	// The shortest /distance, more on Deranged maps.
	int MinGoalDistance() const;
	// The group's /distance or the server default, at least
	// DERANGED_MIN_GOAL_DISTANCE on Deranged maps.
	int GoalDistance(int Group) const;
	int HoverTicks(int Group) const;
	bool CheckFightLeader(int ClientId, const char *pAction);
	void SendFightChat(int Group, const char *pMessage);
	void KeepWeapons(class CCharacter *pChr);
	void UpdateFightTeams();
	struct CEloGain
	{
		double m_Elo;
		double m_SpeedFactor;
		// why the capture earns nothing, like being frozen
		const char *m_pNoElo = nullptr;
		int64_t m_Points = 0;
	};
	static void FormatCaptureResult(char *pBuf, int BufSize, int Fails, const std::optional<CEloGain> &Gain);
	// Returns whether the capture won the fight.
	bool AnnounceFightScore(int Winner, const std::optional<CEloGain> &Gain);
	std::optional<CEloGain> AwardElo(int ClientId, bool Frozen);
	void AddStats(int ClientId, const CGTrainStats &Stats);
	void EndAttempt(int ClientId, bool Captured);
	void StartRun(class CCharacter *pChr);
	void SaveRun(class CCharacter *pChr, const CEloGain &Gain);
	void SaveStats(int ClientId);
	int PathNode(vec2 Pos) const;
	void UpdatePath(class CCharacter *pChr);
	void CheckGoal(class CCharacter *pChr);

public:
	CGameControllerGTrain(class CGameContext *pGameServer);
	~CGameControllerGTrain() override;
	static bool AddSwitchLayer(class IStorage *pStorage, const char *pPath, char *pNewPath, int NewPathSize);
	// 0 for Easy with one star up to 14 for Extreme with three and 16 for
	// Deranged, which has no stars and counts as Extreme with five. Every
	// step makes captures worth more.
	// Other categories and ratings have no ELO.
	static std::optional<int> EloStep(const char *pServer, int Stars);
	void LoadMapDifficulty();
	void SetMapDifficulty(const char *pServer, int Stars);
	// Loads the stats of the player's current name, after joining or
	// renaming. Changes to the previous name's stats are saved first.
	void LoadStats(int ClientId);
	// Before the database shows stats, so they include the latest changes.
	void SaveAllStats();
	const CGTrainStats &Stats(int ClientId) const { return m_aStats[ClientId]; }
	// Shadow-bans the stored runs on a map, of a player or both (empty for
	// all) or lifts the ban, see CScore::BanGTrainRuns.
	void BanRuns(const char *pMap, const char *pName, bool Ban);

	void OnCharacterSpawn(class CCharacter *pChr) override;
	int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon) override;
	void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason) override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	bool HasTimeScore() const override { return false; }
	CFinishTime SnapPlayerTime(int SnappingClient, CPlayer *pPlayer) override { return CFinishTime::Unset(); }
	void Snap(int SnappingClient) override;
	std::optional<int> RaceTimerStartTick(class CCharacter *pChr) override;
	void Fight(int ClientId, const char *pName);
	void TeamUp(int ClientId, const char *pName);
	void AcceptTeam(int ClientId);
	void Freeplay(int ClientId);
	void Retry(int ClientId);
	// Retries like the other practice binds, freeplay starts where the player
	// aims instead.
	void TeleCursor(int ClientId);
	void Distance(int ClientId, std::optional<int> Tiles);
	void NoDoubleJump(int ClientId);
	void Forwards(int ClientId);
	void Trail(int ClientId);
	void ScoreLimit(int ClientId, std::optional<int> Limit);
	void ToggleWeapon(int ClientId, int Weapon);
	// The settings of the player's group, pName empty for the player itself.
	void ShowSettings(int ClientId, const char *pName);
	void Help();

	void Tick() override;
};
#endif // GAME_SERVER_GAMEMODES_GTRAIN_H
