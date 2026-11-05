// The map loader of ddnet_physics names tiles and entities like DDNet's
// game/mapitems.h does, so both are never included together.
#include "gtrain_tiles.h"

extern "C" {
#include <ddnet_map_loader.h>
}
#include <ddnet_physics/ddnet_physics.h>

#include <cstdlib>
#include <cstring>

CGTrainPhysicsMap::~CGTrainPhysicsMap()
{
	if(m_pCollision)
		ddnet_collision_free(m_pCollision);
	delete m_pCollision;
}

bool CGTrainPhysicsMap::Load(const void *pData, size_t Size)
{
	// The loader takes the buffer on success, and leaves it on failure.
	unsigned char *pBuffer = static_cast<unsigned char *>(malloc(Size));
	if(!pBuffer)
		return false;
	memcpy(pBuffer, pData, Size);
	map_data_t MapData = load_map_from_memory(pBuffer, Size);
	if(!MapData.game_layer.data)
	{
		free(pBuffer);
		return false;
	}
	ddnet_collision_t *pCollision = new ddnet_collision_t();
	const bool Loaded = ddnet_collision_init(pCollision, &MapData);
	free_map_data(&MapData);
	if(!Loaded)
	{
		delete pCollision;
		return false;
	}
	if(m_pCollision)
		ddnet_collision_free(m_pCollision);
	delete m_pCollision;
	m_pCollision = pCollision;
	return true;
}
