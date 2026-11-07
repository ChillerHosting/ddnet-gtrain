#include "switch_layer.h"

#include "gamecore.h"
#include "mapitems.h"
#include "mapitems_ex.h"

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <iterator>
#include <numeric>
#include <vector>

EAddSwitchLayer AddSwitchLayer(IStorage *pStorage, const char *pPath, int StorageType, const char *pDestination, int DestinationStorageType)
{
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, pPath, StorageType))
		return EAddSwitchLayer::FAILED;
	int GroupsStart, NumGroups, LayersStart, NumLayers;
	Reader.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &NumGroups);
	Reader.GetType(MAPITEMTYPE_LAYER, &LayersStart, &NumLayers);
	int GameLayer = -1, SwitchLayer = -1;
	int Width = 0, Height = 0;
	for(int i = 0; i < NumLayers; ++i)
	{
		const auto *pLayer = static_cast<const CMapItemLayerTilemap_v2 *>(Reader.GetItem(LayersStart + i));
		if(Reader.GetItemSize(LayersStart + i) < (int)sizeof(CMapItemLayerTilemap_v2) || pLayer->m_Layer.m_Type != LAYERTYPE_TILES)
			continue;
		if(pLayer->m_Flags & TILESLAYERFLAG_GAME)
		{
			GameLayer = i;
			Width = pLayer->m_Width;
			Height = pLayer->m_Height;
		}
		else if(pLayer->m_Flags & TILESLAYERFLAG_SWITCH)
			SwitchLayer = i;
	}
	if(Width <= 0 || Height <= 0)
		return EAddSwitchLayer::FAILED;

	// Groups own ranges of layers. The editor requires the switch layer in
	// the game group, so it goes to its end, also from another group, like
	// the one of its own this function used to add.
	struct CRange
	{
		int m_Start;
		int m_Num;
		bool Contains(int Layer) const { return Layer >= m_Start && Layer < m_Start + m_Num; }
	};
	std::vector<CRange> vRanges;
	int GameGroup = -1;
	for(int g = 0; g < NumGroups; ++g)
	{
		if(Reader.GetItemSize(GroupsStart + g) < (int)sizeof(CMapItemGroup_v1))
			return EAddSwitchLayer::FAILED;
		const auto *pGroup = static_cast<const CMapItemGroup_v1 *>(Reader.GetItem(GroupsStart + g));
		const CRange Range = {pGroup->m_StartLayer, pGroup->m_NumLayers};
		if(Range.m_Start < 0 || Range.m_Num < 0 || Range.m_Start + Range.m_Num > NumLayers)
			return EAddSwitchLayer::FAILED;
		if(Range.Contains(GameLayer))
			GameGroup = g;
		vRanges.push_back(Range);
	}
	if(GameGroup < 0)
		return EAddSwitchLayer::FAILED;
	if(SwitchLayer >= 0 && vRanges[GameGroup].Contains(SwitchLayer))
		return EAddSwitchLayer::PRESENT;

	// Old layer index of every new one, -1 for an added layer.
	std::vector<int> vOrder(NumLayers);
	std::iota(vOrder.begin(), vOrder.end(), 0);
	int MovedFrom = -1, MovedIndex = -1;
	if(SwitchLayer >= 0)
	{
		vOrder.erase(vOrder.begin() + SwitchLayer);
		for(int g = 0; g < NumGroups; ++g)
		{
			CRange &Range = vRanges[g];
			if(Range.Contains(SwitchLayer))
			{
				MovedFrom = g;
				MovedIndex = SwitchLayer - Range.m_Start;
				--Range.m_Num;
			}
			else if(Range.m_Start > SwitchLayer)
				--Range.m_Start;
		}
	}
	const int Position = vRanges[GameGroup].m_Start + vRanges[GameGroup].m_Num;
	vOrder.insert(vOrder.begin() + Position, SwitchLayer);
	for(int g = 0; g < NumGroups; ++g)
	{
		CRange &Range = vRanges[g];
		if(g == GameGroup)
			++Range.m_Num;
		else if(Range.m_Start >= Position)
			++Range.m_Start;
		else if(Range.m_Start + Range.m_Num > Position)
			return EAddSwitchLayer::FAILED; // overlaps the game group
	}
	// A group left empty at the end is that group of its own.
	const int DroppedGroup = MovedFrom == NumGroups - 1 && vRanges[MovedFrom].m_Num == 0 ? MovedFrom : -1;

	CDataFileWriter Writer;
	for(int i = 0; i < Reader.NumData(); ++i)
	{
		Writer.AddData(Reader.GetDataSize(i), Reader.GetData(i));
		Reader.UnloadData(i);
	}
	for(int i = 0; i < Reader.NumItems(); ++i)
	{
		// The writer adds the UUIDs of extended item types itself, like
		// map_resave. Copying them too would duplicate them.
		int Type, Id;
		CUuid Uuid;
		void *pData = Reader.GetItem(i, &Type, &Id, &Uuid);
		const int Size = Reader.GetItemSize(i);
		if(Type == ITEMTYPE_EX || Type == MAPITEMTYPE_LAYER || Type == MAPITEMTYPE_GROUP)
			continue;
		// Automapper configs refer to layers by their index in their group.
		if(Type == MAPITEMTYPE_AUTOMAPPER_CONFIG && MovedFrom >= 0 && Size >= (int)sizeof(CMapItemAutomapperConfig))
		{
			const char *pBytes = static_cast<const char *>(pData);
			std::vector<char> vConfig(pBytes, pBytes + Size);
			auto *pConfig = reinterpret_cast<CMapItemAutomapperConfig *>(vConfig.data());
			if(pConfig->m_GroupId == MovedFrom && pConfig->m_LayerId == MovedIndex)
			{
				pConfig->m_GroupId = GameGroup;
				pConfig->m_LayerId = vRanges[GameGroup].m_Num - 1;
			}
			else if(pConfig->m_GroupId == MovedFrom && pConfig->m_LayerId > MovedIndex)
				--pConfig->m_LayerId;
			Writer.AddItem(Type, Id, Size, vConfig.data(), &Uuid);
			continue;
		}
		Writer.AddItem(Type, Id, Size, pData, &Uuid);
	}

	for(int i = 0; i < (int)vOrder.size(); ++i)
	{
		if(vOrder[i] >= 0)
		{
			Writer.AddItem(MAPITEMTYPE_LAYER, i, Reader.GetItemSize(LayersStart + vOrder[i]), Reader.GetItem(LayersStart + vOrder[i]));
			continue;
		}
		CMapItemLayerTilemap Layer = {};
		Layer.m_Layer.m_Type = LAYERTYPE_TILES;
		Layer.m_Version = 3;
		Layer.m_Width = Width;
		Layer.m_Height = Height;
		Layer.m_Flags = TILESLAYERFLAG_SWITCH;
		Layer.m_Color = {255, 255, 255, 255};
		Layer.m_ColorEnv = -1;
		Layer.m_Image = -1;
		Layer.m_Tele = Layer.m_Speedup = Layer.m_Front = Layer.m_Tune = -1;
		const std::vector<CTile> vTiles((size_t)Width * Height, CTile{});
		const std::vector<CSwitchTile> vSwitchTiles((size_t)Width * Height, CSwitchTile{});
		Layer.m_Data = Writer.AddData(vTiles.size() * sizeof(CTile), vTiles.data());
		Layer.m_Switch = Writer.AddData(vSwitchTiles.size() * sizeof(CSwitchTile), vSwitchTiles.data());
		StrToInts(Layer.m_aName, std::size(Layer.m_aName), "Switch");
		Writer.AddItem(MAPITEMTYPE_LAYER, i, sizeof(Layer), &Layer);
	}
	for(int g = 0; g < NumGroups; ++g)
	{
		if(g == DroppedGroup)
			continue;
		int Id;
		const auto *pData = static_cast<const char *>(Reader.GetItem(GroupsStart + g, nullptr, &Id));
		std::vector<char> vGroup(pData, pData + Reader.GetItemSize(GroupsStart + g));
		auto *pGroup = reinterpret_cast<CMapItemGroup_v1 *>(vGroup.data());
		pGroup->m_StartLayer = vRanges[g].m_Start;
		pGroup->m_NumLayers = vRanges[g].m_Num;
		Writer.AddItem(MAPITEMTYPE_GROUP, Id, vGroup.size(), vGroup.data());
	}
	Reader.Close();

	if(!Writer.Open(pStorage, pDestination, DestinationStorageType))
		return EAddSwitchLayer::FAILED;
	Writer.Finish();
	return SwitchLayer >= 0 ? EAddSwitchLayer::MOVED : EAddSwitchLayer::ADDED;
}
