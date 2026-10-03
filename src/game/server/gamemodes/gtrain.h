#ifndef GAME_SERVER_GAMEMODES_GTRAIN_H
#define GAME_SERVER_GAMEMODES_GTRAIN_H

#include "ddnet.h"

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <vector>

// Gores training: players get placed at random positions between
// the start and the finish line, killing places them again.
class CGameControllerGTrain : public CGameControllerDDNet
{
	enum
	{
		HOVER_TICKS = 50,
	};

	// centers of all non-freeze tiles between the start and the finish line
	std::vector<vec2> m_vTrainPositions;
	bool m_aPendingPlace[MAX_CLIENTS];

	void FindTrainPositions();
	void PlaceRandomly(class CCharacter *pChr);

public:
	CGameControllerGTrain(class CGameContext *pGameServer);
	~CGameControllerGTrain() override;

	void OnCharacterSpawn(class CCharacter *pChr) override;

	void Tick() override;
};
#endif // GAME_SERVER_GAMEMODES_GTRAIN_H
