#ifndef GAME_SWITCH_LAYER_H
#define GAME_SWITCH_LAYER_H

class IStorage;

enum class EAddSwitchLayer
{
	FAILED,
	PRESENT, // the map has a switch layer already, nothing was written
	ADDED,
	MOVED, // from another group into the game group
};

// Door collision only exists in maps with a switch layer, on the server and
// on clients. Writes the map at pPath to pDestination with an empty switch
// layer added to the game group, unless it has one there already. Switch
// layers in other groups, which the editor refuses, are moved there.
EAddSwitchLayer AddSwitchLayer(IStorage *pStorage, const char *pPath, int StorageType, const char *pDestination, int DestinationStorageType);

#endif // GAME_SWITCH_LAYER_H
