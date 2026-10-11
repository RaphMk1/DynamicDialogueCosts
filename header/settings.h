#pragma once

#include "log.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace DynamicDialogueCosts
{
	inline constexpr auto kIniPath = L"Data/SKSE/Plugins/DynamicDialogueCosts/DynamicDialogueCosts.ini";
	inline constexpr auto kPresetsDirPath = L"Data/SKSE/Plugins/DynamicDialogueCosts/Presets";

	struct CategorySettings
	{
		std::string categoryID;
		int         priority{ 0 };
		std::string displayName;
		bool        persistentLeveledLists{ false };
		bool        usesSellBarterMult{ false };
		bool        hideCatInMenu{ false };
		bool        enabled{ true };
		float       multiplier{ 1.0f };
		bool        dynamicScaling{ false };
		bool        enableInflation{ false };
		bool        invertInflation{ false };
		float       inflationRate{ 0.001f };
		float       inflationResetDay{ 0.0f };
		bool        levelScale{ false };
		bool        invertLevelMult{ false };
		float       levelMult{ 0.01f };
	};

	struct Settings
	{
		bool                                              scaleCatValues{ true };
		std::unordered_map<std::string, CategorySettings> categories;

		bool  enableDynamicScaling{ true };
		bool  includePowerModifiers{ true };
		bool  includePerkModifiers{ true };
		bool  enableInflation{ true };
		float maxInflationCap{ 100.0f };
		bool  enableLevelScaling{ true };

		float       maxMultCap{ 1000.0f };
		bool        enableDebug{ true };
		int         debugLevel{ 2 };
		bool        hideCategories{ false };
		bool        useActivateHandler{ true };
		bool        useQuestWatcher{ false };
		std::string questOrderProfile{ "" };

		CategorySettings& GetOrCreateCategory(const std::string& a_categoryID)
		{
			std::string key = a_categoryID;
			std::transform(key.begin(), key.end(), key.begin(), ::tolower);
			return categories[key];
		}

		[[nodiscard]] const CategorySettings* GetCategory(const std::string& a_categoryID) const
		{
			std::string key = a_categoryID;
			std::transform(key.begin(), key.end(), key.begin(), ::tolower);
			auto it = categories.find(key);
			return (it != categories.end()) ? &it->second : nullptr;
		}
	};
	inline std::atomic<std::shared_ptr<const Settings>> g_settings{ std::make_shared<Settings>() };

	inline std::atomic<bool> g_isItemPatchingInProgress{ false };
	inline std::atomic<bool> g_itemsArePatched{ false };

	inline std::atomic<bool>  g_isDialogueScaled{ false };
	inline std::atomic<float> g_activeBuyRate{ 1.0f };
	inline std::atomic<float> g_activeSellRate{ 1.0f };

	namespace DialoguePriceTransformer
	{
		void                     ResetPersistentToBaseline();
		void                     ApplyActiveGlobalModifiers();
		inline std::atomic<bool> g_itemBucketsInitialized{ false };
		inline std::atomic<bool> g_leveledBucketsInitialized{ false };
		inline std::atomic<bool> g_globalBucketsInitialized{ false };
		void                     ResetGlobalsToBaseline();
		void                     ApplySettModifiers();
		void                     ApplyItemModifiers();
		void                     PokeQuestsRefresh();
	}

	void RebuildCategoryRegistry();
	void ReadPluginConfig();
	void LoadMenuTexts();
	void CheckAndApplyPersistentPatches(bool a_forceUpdate = false);
	void PreCacheQuestGlobals();
}