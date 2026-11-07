#include <base/fs.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/storage.h>

#include <game/switch_layer.h>

static const char *TOOL_NAME = "map_add_switch_layer";

// GTrain holds hovering tees with laser doors, which need a switch layer.
// Adding it to the map files lets the server serve them as they are, so map
// download servers can host the same files.
int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	if(argc < 2)
	{
		log_error(TOOL_NAME, "Usage: %s <map>...", TOOL_NAME);
		log_error(TOOL_NAME, "Adds an empty switch layer to maps without one, in place. Switch layers outside the game group are moved into it.");
		return -1;
	}

	std::unique_ptr<IStorage> pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::BASIC, argc, argv));
	if(!pStorage)
	{
		log_error(TOOL_NAME, "Error creating basic storage");
		return -1;
	}

	int NumAdded = 0, NumMoved = 0, NumPresent = 0, NumFailed = 0;
	for(int i = 1; i < argc; i++)
	{
		const char *pMap = argv[i];
		char aTemp[IO_MAX_PATH_LENGTH];
		str_format(aTemp, sizeof(aTemp), "%s.switch.tmp", pMap);
		const EAddSwitchLayer Result = AddSwitchLayer(pStorage.get(), pMap, IStorage::TYPE_ABSOLUTE, aTemp, IStorage::TYPE_ABSOLUTE);
		switch(Result)
		{
		case EAddSwitchLayer::FAILED:
			// Nothing is written then.
			log_error(TOOL_NAME, "Failed to add a switch layer to '%s'", pMap);
			NumFailed++;
			break;
		case EAddSwitchLayer::PRESENT:
			NumPresent++;
			break;
		case EAddSwitchLayer::ADDED:
		case EAddSwitchLayer::MOVED:
			if(fs_rename(aTemp, pMap) != 0)
			{
				log_error(TOOL_NAME, "Failed to replace '%s'", pMap);
				if(fs_remove(aTemp) != 0)
					log_error(TOOL_NAME, "Failed to remove '%s'", aTemp);
				NumFailed++;
			}
			else if(Result == EAddSwitchLayer::MOVED)
			{
				log_info(TOOL_NAME, "Moved the switch layer of '%s' into the game group", pMap);
				NumMoved++;
			}
			else
			{
				log_info(TOOL_NAME, "Added a switch layer to '%s'", pMap);
				NumAdded++;
			}
			break;
		}
	}
	log_info(TOOL_NAME, "%d added, %d moved, %d had one already, %d failed", NumAdded, NumMoved, NumPresent, NumFailed);
	return NumFailed ? 1 : 0;
}
