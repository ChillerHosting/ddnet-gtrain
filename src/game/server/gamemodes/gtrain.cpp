#include "gtrain.h"

#include <base/log.h>
#include <base/secure.h>

#include <engine/shared/config.h>

#include <game/collision.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <algorithm>
#include <iterator>

#define GAME_TYPE_NAME "GTrain"
#define TEST_TYPE_NAME "TestGTrain"

CGameControllerGTrain::CGameControllerGTrain(class CGameContext *pGameServer) :
	CGameControllerDDNet(pGameServer)
{
	m_pGameType = g_Config.m_SvTestingCommands ? TEST_TYPE_NAME : GAME_TYPE_NAME;

	std::fill(std::begin(m_aPendingPlace), std::end(m_aPendingPlace), false);

	FindTrainPositions();
}

CGameControllerGTrain::~CGameControllerGTrain() = default;

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
		// death tiles and teleporters are discarded as well, to not kill or move the player instantly
		if(HasTile(Index, TILE_FREEZE) || HasTile(Index, TILE_DFREEZE) || HasTile(Index, TILE_DEATH))
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
}

void CGameControllerGTrain::PlaceRandomly(CCharacter *pChr)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	m_aPendingPlace[ClientId] = false;
	if(m_vTrainPositions.empty())
		return;

	const vec2 Pos = m_vTrainPositions[secure_rand_below(m_vTrainPositions.size())];
	pChr->SetDeepFrozen(false);
	pChr->SetLiveFrozen(false);
	pChr->Unfreeze();
	pChr->ResetHook();
	pChr->ResetJumps();
	pChr->ResetVelocity();
	pChr->SetPosition(Pos);
	pChr->m_Pos = Pos;
	pChr->m_PrevPos = Pos;
	// the player didn't pass the start line, so finishing must not count
	pChr->m_DDRaceState = ERaceState::CHEATED;

	// hover in place until the freeze is over
	pChr->ForceFreeze(HOVER_TICKS);
	pChr->m_ZeroGravity = true;
	GameServer()->SendTuningParams(ClientId, pChr->m_TuneZone);
}

void CGameControllerGTrain::OnCharacterSpawn(CCharacter *pChr)
{
	CGameControllerDDNet::OnCharacterSpawn(pChr);

	// the character is not fully initialized yet, place it in the tick
	m_aPendingPlace[pChr->GetPlayer()->GetCid()] = true;
}

void CGameControllerGTrain::Tick()
{
	CGameControllerDDNet::Tick();

	// killing is how players choose a new position, never delay it
	g_Config.m_SvKillDelay = 0;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		if(!pChr || pChr->IsPaused())
			continue;

		if(m_aPendingPlace[ClientId])
		{
			PlaceRandomly(pChr);
			continue;
		}

		// hover is over, let the player play again
		if(pChr->m_ZeroGravity && pChr->m_FreezeTime == 0)
		{
			pChr->m_ZeroGravity = false;
			GameServer()->SendTuningParams(ClientId, pChr->m_TuneZone);
		}
	}
}
