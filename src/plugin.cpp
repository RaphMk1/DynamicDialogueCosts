#include "Hook.h"
#include "MCPddcMenu.h"
#include "PCH.h"
#include "QOManager.h"
#include "Serialization.h"
#include "dialogueRegistry.h"
#include "settings.h"

#include <SimpleIni.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define VECTORIZE __pragma(loop(ivdep))

namespace DynamicDialogueCosts
{
	RE::ObjectRefHandle g_targetSpeakerHandle{};

	namespace
	{
		float                   g_lastPatchedDay{ -1.0f };
		float                   g_cachedDaysPassed{ 0.0f };
		std::uint16_t           g_lastPatchedLevel{ 0 };
		constexpr std::uint16_t invCID = 0xFFFF;
		std::shared_mutex       g_modifierMutex;

		std::unordered_map<std::string, std::uint16_t> g_categoryToIDMap;
		std::vector<std::string>                       g_idToCategoryMap;

		struct ComputedCategoryRates
		{
			float combinedBuyMult{ 1.0f };
			float combinedSellMult{ 1.0f };
			float combinedItemMult{ 1.0f };
			float externalGlobalMult{ 1.0f };
			float flatAlienOffset{ 0.0f };
			bool  dynamicScaling{ false };
			bool  usesSellBarterMult{ false };
			bool  enabled{ true };
			bool  isValid{ false };
		};
		std::vector<ComputedCategoryRates> g_categoryRateCache;

		struct EconomyCache
		{
			float fBarterMax = 3.3f;
			float fBarterMin = 2.0f;

			void Refresh()
			{
				auto gsc = RE::GameSettingCollection::GetSingleton();
				if (gsc) {
					if (auto settingMax = gsc->GetSetting("fBarterMax")) {
						fBarterMax = settingMax->GetFloat();
					}
					if (auto settingMin = gsc->GetSetting("fBarterMin")) {
						fBarterMin = settingMin->GetFloat();
					}
				}
			}
		};
		EconomyCache g_ecoCache;

		struct StaticCategoryBase
		{
			float baseEngineMult{ 1.0f };
			float dynBuy{ 1.0f };
			float dynSell{ 1.0f };
			bool  isValid{ false };
		};
		std::vector<StaticCategoryBase> g_staticCategoryBaseCache;
		std::vector<RE::TESGlobal*>     g_pendingPokeGlobals;

		std::atomic<bool> g_questWatcherSOS{ false };
	}

	static std::uint16_t GetOrRegisterCategoryID(std::string_view catID)
	{
		std::string key(catID);
		std::transform(key.begin(), key.end(), key.begin(), ::tolower);

		auto it = g_categoryToIDMap.find(key);
		if (it != g_categoryToIDMap.end()) {
			return it->second;
		}

		std::uint16_t newID = static_cast<std::uint16_t>(g_idToCategoryMap.size());
		g_categoryToIDMap[key] = newID;
		g_idToCategoryMap.push_back(key);
		return newID;
	}

	static std::uint16_t GetCategoryID(std::string_view catID)
	{
		std::string key(catID);
		std::transform(key.begin(), key.end(), key.begin(), ::tolower);

		auto it = g_categoryToIDMap.find(key);
		return (it != g_categoryToIDMap.end()) ? it->second : invCID;
	}

	static const std::string& GetCategoryStringFromID(std::uint16_t catIndex)
	{
		static const std::string emptyCategory{};
		if (catIndex < g_idToCategoryMap.size()) {
			return g_idToCategoryMap[catIndex];
		}
		return emptyCategory;
	}

	void RebuildCategoryRegistry()
	{
		std::unique_lock<std::shared_mutex> lock(g_modifierMutex);
		g_categoryToIDMap.clear();
		g_idToCategoryMap.clear();

		auto cfg = g_settings.load();
		if (cfg) {
			for (const auto& [catID, x] : cfg->categories) {
				GetOrRegisterCategoryID(catID);
			}
		}

		for (const auto& [catID, x] : DialogueGlobalRegistry::g_categoryDiskDefaults) {
			GetOrRegisterCategoryID(catID);
		}

		const size_t totalCats = g_idToCategoryMap.size();
		g_categoryRateCache.assign(totalCats, {});
	}

	void ReadPluginConfig()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		const auto path = L"Data/SKSE/Plugins/DynamicDialogueCosts/DynamicDialogueCosts.ini";

		auto newSettings = std::make_shared<Settings>();

		for (const auto& [catID, diskSettings] : DialogueGlobalRegistry::g_categoryDiskDefaults) {
			newSettings->categories[catID] = diskSettings;
		}

		if (ini.LoadFile(path) >= 0) {
			newSettings->scaleCatValues = ini.GetBoolValue("Settings", "bScaleValues", true);
			newSettings->enableDynamicScaling = ini.GetBoolValue("Settings", "bEnableBarterMult", true);
			newSettings->includePowerModifiers = ini.GetBoolValue("Settings", "bIncludePowerModifiers", true);
			newSettings->includePerkModifiers = ini.GetBoolValue("Settings", "bIncludePerkModifiers", true);

			newSettings->enableInflation = ini.GetBoolValue("Settings", "bEnableInflation", true);
			newSettings->maxInflationCap = std::clamp(static_cast<float>(ini.GetDoubleValue("Settings", "fMaxInflationCap", 10.0)), 1.0f, 100.0f);

			newSettings->enableLevelScaling = ini.GetBoolValue("Settings", "bEnableLevelScaling", true);
			newSettings->useQuestWatcher = ini.GetBoolValue("Settings", "bUseQuestWatcher", false);
			newSettings->questOrderProfile = ini.GetValue("Settings", "sQuestOrderProfile", "");
			newSettings->maxMultCap = std::clamp(static_cast<float>(ini.GetDoubleValue("Settings", "fMaxMultCap", 100.0)), 0.0f, 100000.0f);

			newSettings->hideCategories = ini.GetBoolValue("Settings", "bHideCategories", false);
			newSettings->enableDebug = ini.GetBoolValue("Settings", "bEnableDebugMode", true);
			newSettings->debugLevel = std::clamp(static_cast<int>(ini.GetLongValue("Settings", "iDebugLevel", 2)), 0, 5);
			spdlog::set_level(static_cast<spdlog::level::level_enum>(newSettings->debugLevel));

			newSettings->useActivateHandler = ini.GetBoolValue("Settings", "bUseActivateHandler", true);

			CSimpleIniA::TNamesDepend sections;
			ini.GetAllSections(sections);

			for (const auto& section : sections) {
				std::string secName = section.pItem;
				if (secName.rfind("Category_", 0) == 0) {
					std::string catID = secName.substr(9);
					std::string lowerCatID = catID;
					std::transform(lowerCatID.begin(), lowerCatID.end(), lowerCatID.begin(), ::tolower);

					auto& cat = newSettings->GetOrCreateCategory(lowerCatID);
					cat.categoryID = lowerCatID;
					cat.displayName = ini.GetValue(secName.c_str(), "sDisplayName", cat.displayName.empty() ? catID.c_str() : cat.displayName.c_str());
					cat.enabled = ini.GetBoolValue(secName.c_str(), "bScaleValues", cat.enabled);
					cat.multiplier = std::clamp(static_cast<float>(ini.GetDoubleValue(secName.c_str(), "fStaticMultiplier", cat.multiplier)), 0.0f, 1000.0f);
					cat.dynamicScaling = ini.GetBoolValue(secName.c_str(), "bEnableBarterMult", cat.dynamicScaling);
					cat.persistentLeveledLists = ini.GetBoolValue(secName.c_str(), "bPersistLeveledLists", cat.persistentLeveledLists);
					cat.usesSellBarterMult = ini.GetBoolValue(secName.c_str(), "bUsesSellBarterMult", cat.usesSellBarterMult);
					cat.hideCatInMenu = ini.GetBoolValue(secName.c_str(), "bHideCatInMenu", cat.hideCatInMenu);
					cat.enableInflation = ini.GetBoolValue(secName.c_str(), "bEnableInflation", cat.enableInflation);
					cat.invertInflation = ini.GetBoolValue(secName.c_str(), "bInvertInflation", cat.invertInflation);
					cat.inflationRate = std::clamp(static_cast<float>(ini.GetDoubleValue(secName.c_str(), "fInflationRate", cat.inflationRate)), 0.0f, 1.0f);
					cat.inflationResetDay = std::max(0.0f, static_cast<float>(ini.GetDoubleValue(secName.c_str(), "fInflationResetDay", cat.inflationResetDay)));
					cat.levelScale = ini.GetBoolValue(secName.c_str(), "bLevelScale", cat.levelScale);
					cat.invertLevelMult = ini.GetBoolValue(secName.c_str(), "bInvertLevelMult", cat.invertLevelMult);
					cat.levelMult = std::clamp(static_cast<float>(ini.GetDoubleValue(secName.c_str(), "fLevelMult", cat.levelMult)), 0.0f, 0.1f);
				}
			}
		}
		g_settings.store(newSettings);
		RebuildCategoryRegistry();
		if (auto cfg = g_settings.load()) {
			int activeLevel = cfg->enableDebug ? cfg->debugLevel : 2;
			SetLogLevel(activeLevel);
		}
	}

	static float ComputePerkPriceRate(float basePrice, bool isBuyPrice, RE::Actor* merchant = nullptr)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return basePrice;
		}

		RE::Actor* targetActor = merchant;
		if (!targetActor) {
			if (auto handlePtr = g_targetSpeakerHandle.get()) {
				targetActor = handlePtr->As<RE::Actor>();
			}
		}

		float adjustedPrice = basePrice;
		auto  entryPoint = isBuyPrice ? RE::BGSEntryPoint::ENTRY_POINT::kModBuyPrices : RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices;

		RE::BGSEntryPoint::HandleEntryPoint(entryPoint, player, targetActor, std::addressof(adjustedPrice));

		return adjustedPrice;
	}

	static float CalculateSpeakerCostRate(bool isBuyPrice, RE::Actor* merchant = nullptr, bool includePerks = true)
	{
		auto cfg = g_settings.load();
		if (!cfg || !cfg->enableDynamicScaling) {
			return 1.0f;
		}

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return 1.0f;
		}

		float speechSkill = player->AsActorValueOwner() ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech) : 0.0f;

		float perkMod = 1.0f;
		if (cfg->includePerkModifiers && includePerks) {
			perkMod = ComputePerkPriceRate(1.0f, isBuyPrice, merchant);
		}

		float powerMod = 1.0f;
		if (cfg->includePowerModifiers) {
			if (auto avOwner = player->AsActorValueOwner()) {
				speechSkill += avOwner->GetActorValue(RE::ActorValue::kSpeechcraftModifier);
				float rawPower = avOwner->GetActorValue(RE::ActorValue::kSpeechcraftPowerModifier) / 100.0f;
				powerMod = isBuyPrice ? std::max(0.0f, 1.0f - rawPower) : (1.0f + rawPower);
			}
		}

		speechSkill = std::clamp(speechSkill, 0.0f, 100.0f);
		float progress = speechSkill / 100.0f;
		float barterFactor = g_ecoCache.fBarterMax - ((g_ecoCache.fBarterMax - g_ecoCache.fBarterMin) * progress);
		barterFactor = std::clamp(barterFactor, 1.0f, g_ecoCache.fBarterMax);

		float barterMult = perkMod * powerMod;
		float finalBarterMult = isBuyPrice ? (barterMult * barterFactor) : (barterMult / barterFactor);
		if (isBuyPrice) {
			finalBarterMult = std::max(finalBarterMult, 1.05f);
		} else {
			finalBarterMult = std::min(finalBarterMult, 1.00f);
		}

		return finalBarterMult;
	}

	static float GetCategoryLevelMultiplier(const CategorySettings* the_cat)
	{
		if (!the_cat || !the_cat->enabled || !the_cat->levelScale)
			return 1.0f;

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return 1.0f;

		const std::uint16_t level = player->GetLevel();
		if (level <= 1)
			return 1.0f;

		const float levelFactor = 1.0f + (static_cast<float>(level - 1) * the_cat->levelMult);
		const float finalMultiplier = the_cat->invertLevelMult ? (1.0f / levelFactor) : levelFactor;
		return std::max(0.0f, finalMultiplier);
	}

	static float GetCategoryInflationMultiplier(const CategorySettings* the_cat, float daysPassed, float maxInflationCap)
	{
		if (!the_cat || !the_cat->enabled || !the_cat->enableInflation)
			return 1.0f;

		float effDays = std::max(0.0f, daysPassed - the_cat->inflationResetDay);
		float rawFactor = 1.0f + (effDays * the_cat->inflationRate);
		float maxCap = std::clamp(maxInflationCap, 1.0f, 100.0f);
		float cFactor = std::min(rawFactor, maxCap);

		if (the_cat->invertInflation) {
			return (cFactor > 0.0f) ? (1.0f / cFactor) : 1.0f;
		}

		return cFactor;
	}

	static void InitializeCategoryBaseRateCache()
	{
		const size_t totalCats = g_idToCategoryMap.size();
		g_staticCategoryBaseCache.assign(totalCats, {});
		auto cfg = g_settings.load();
		if (!cfg)
			return;

		const float daysPassed = g_cachedDaysPassed;
		const float buyRate = std::isfinite(g_activeBuyRate.load()) ? g_activeBuyRate.load() : 1.0f;
		const float sellRate = std::isfinite(g_activeSellRate.load()) ? g_activeSellRate.load() : 1.0f;

		for (intptr_t i = 0; i < static_cast<intptr_t>(totalCats); ++i) {
			const std::uint16_t catIndex = static_cast<std::uint16_t>(i);
			const std::string&  catID = g_idToCategoryMap[catIndex];
			const auto*         catSettings = cfg->GetCategory(catID);
			if (!catSettings || !catSettings->enabled)
				continue;

			float baseMult = std::isfinite(catSettings->multiplier) ? std::max(0.0f, catSettings->multiplier) : 1.0f;
			float levelMult = cfg->enableLevelScaling ? GetCategoryLevelMultiplier(catSettings) : 1.0f;
			float inflMult = cfg->enableInflation ? GetCategoryInflationMultiplier(catSettings, daysPassed, cfg->maxInflationCap) : 1.0f;

			StaticCategoryBase baseInfo;
			baseInfo.dynBuy = catSettings->dynamicScaling ? buyRate : 1.0f;
			baseInfo.dynSell = catSettings->dynamicScaling ? sellRate : 1.0f;
			baseInfo.baseEngineMult = std::min(cfg->maxMultCap, baseMult * inflMult * levelMult);
			baseInfo.isValid = true;

			g_staticCategoryBaseCache[catIndex] = baseInfo;
		}
	}

	static void UpdateCategoryExternalRates(std::uint16_t catIndex)
	{
		if (catIndex >= g_staticCategoryBaseCache.size())
			return;
		const auto& baseInfo = g_staticCategoryBaseCache[catIndex];
		if (!baseInfo.isValid)
			return;

		auto cfg = g_settings.load();
		if (!cfg)
			return;

		const std::string& catID = g_idToCategoryMap[catIndex];
		const float        maxCap = (std::isfinite(cfg->maxMultCap) && cfg->maxMultCap >= 0.0f) ? cfg->maxMultCap : 100000.0f;
		constexpr float    overflowGuard = 1.0e13f;

		float extProdStack = 1.0f;
		float extSumStack = 0.0f;
		float extFlatStack = 0.0f;
		float extAlienStack = 0.0f;

		auto extRange = DialogueGlobalRegistry::g_cachedExternalGlobalCategoryMap.equal_range(catID);
		for (auto it = extRange.first; it != extRange.second; ++it) {
			const auto& entry = it->second;
			if (auto* global = entry.global) {
				float rawVal = global->value;
				if (!std::isfinite(rawVal))
					continue;

				switch (entry.op) {
				case DialogueGlobalRegistry::ExtGlobalOp::Multiply:
					rawVal *= entry.scale;
					extProdStack = std::min(overflowGuard, extProdStack * (rawVal >= 0.0f ? rawVal : 1.0f));
					break;
				case DialogueGlobalRegistry::ExtGlobalOp::AddPercent:
					extSumStack += (rawVal * entry.scale);
					break;
				case DialogueGlobalRegistry::ExtGlobalOp::SubPercent:
					extSumStack -= (rawVal * entry.scale);
					break;
				case DialogueGlobalRegistry::ExtGlobalOp::FlatOffset:
					extFlatStack += (rawVal * entry.scale);
					break;
				case DialogueGlobalRegistry::ExtGlobalOp::Alien:
					extAlienStack += (rawVal * entry.scale);
					break;
				}
			}
		}

		float effectiveSumMult = std::max(0.0f, 1.0f + extSumStack);
		float combinedExtMult = std::max(0.0f, extProdStack * effectiveSumMult);

		const auto* catSettings = cfg->GetCategory(catID);

		ComputedCategoryRates rates;
		rates.externalGlobalMult = std::clamp(combinedExtMult, 0.0f, maxCap);
		rates.combinedBuyMult = std::clamp((baseInfo.baseEngineMult * baseInfo.dynBuy * combinedExtMult) + extFlatStack, 0.0f, maxCap);
		rates.combinedSellMult = std::clamp((baseInfo.baseEngineMult * baseInfo.dynSell * combinedExtMult) + extFlatStack, 0.0f, maxCap);
		rates.combinedItemMult = std::clamp((baseInfo.baseEngineMult * combinedExtMult) + extFlatStack, 0.0f, maxCap);
		rates.flatAlienOffset = extAlienStack;
		rates.dynamicScaling = catSettings ? catSettings->dynamicScaling : false;
		rates.usesSellBarterMult = catSettings ? catSettings->usesSellBarterMult : false;
		rates.enabled = catSettings ? catSettings->enabled : true;
		rates.isValid = true;

		if (catIndex < g_categoryRateCache.size()) {
			g_categoryRateCache[catIndex] = rates;
		}
	}

	static void UpdateCategoryExternalRates(const std::string& catID)
	{
		std::uint16_t catIndex = GetCategoryID(catID);
		if (catIndex != invCID) {
			UpdateCategoryExternalRates(catIndex);
		}
	}

	void CacheSpeakerModifiers(RE::Actor* merchant = nullptr)
	{
		g_activeBuyRate.store(CalculateSpeakerCostRate(true, merchant, true));
		g_activeSellRate.store(CalculateSpeakerCostRate(false, merchant, true));
	}

	bool CanActorTalkToPlayer(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}

		if (actor->IsDead() || actor->IsDisabled() || actor->IsDeleted()) {
			return false;
		}

		return true;
	}

	namespace DialoguePriceTransformer
	{

		struct GlobalBaselineEntry
		{
			float         originalValue{ 0.0f };
			std::uint16_t catIndex{ invCID };
			std::string   edid{};
		};

		struct LeveledItemBaselineEntry
		{
			std::vector<std::uint16_t> originalCounts;
			std::int8_t                originalChanceNone{ 0 };
			std::uint16_t              catIndex{ invCID };
			std::string                edid{};
		};

		struct ItemBaselineEntry
		{
			std::int32_t  originalValue{ 0 };
			std::uint16_t catIndex{ invCID };
			std::string   edid{};
			std::int32_t* valuePtr{ nullptr };
		};

		struct FactionCrimeBaselineEntry
		{
			float         originalValue{ 0.0f };
			std::uint16_t catIndex{ invCID };
		};

		std::unordered_map<RE::Setting*, GlobalBaselineEntry>          g_baselineSettGmstMap;
		std::unordered_map<RE::TESGlobal*, GlobalBaselineEntry>        g_baselineSettGlobalsMap;
		std::unordered_map<RE::TESForm*, ItemBaselineEntry>            g_baselineItemsMap;
		std::unordered_map<RE::TESLevItem*, LeveledItemBaselineEntry>  g_baselinePersistLevItemsMap;
		std::unordered_map<RE::TESLevItem*, LeveledItemBaselineEntry>  g_baselineDialogueLevItemsMap;
		std::unordered_map<RE::TESGlobal*, GlobalBaselineEntry>        g_baselineGlobalsMap;
		std::unordered_map<RE::TESGlobal*, float>                      g_activeScaledGlobalValues;
		std::unordered_map<RE::TESGlobal*, std::vector<RE::TESQuest*>> g_globalToQuestMap;
		std::vector<FactionCrimeBaselineEntry>                         g_baselineFactionCrimeEntries;

		static void UpdateCurrentInstanceGlobal(RE::TESQuest* a_quest, RE::TESGlobal* a_global)
		{
			using func_t = decltype(&UpdateCurrentInstanceGlobal);
			static REL::Relocation<func_t> func{ RELOCATION_ID(24509, 25037) };
			return func(a_quest, a_global);
		}

		static void PokeQuestsScaled()
		{
			auto executePoke = [](RE::TESQuest* quest, RE::TESGlobal* global, float scaledVal = -1.0f) {
				if (!quest || !global || !quest->IsRunning())
					return;

				if (spdlog::should_log(spdlog::level::trace)) {
					const char* globalEdid = global->GetFormEditorID();
					const char* questEdid = quest->GetFormEditorID();
					std::string globalID = globalEdid ? globalEdid : std::format("0x{:08X}", global->GetFormID());
					std::string questID = questEdid ? questEdid : std::format("0x{:08X}", quest->GetFormID());
					SKSE::log::trace("[PKQT] Quest '{}' | Global: '{}' | Value: {:.2f}", questID, globalID, scaledVal);
				}

				UpdateCurrentInstanceGlobal(quest, global);
			};

			SKSE::log::trace("[PKQT] Updating CIG...");
			g_pendingPokeGlobals.clear();
			for (const auto& [global, scaledVal] : g_activeScaledGlobalValues) {
				if (!global)
					continue;

				g_pendingPokeGlobals.push_back(global);

				auto it = g_globalToQuestMap.find(global);
				if (it == g_globalToQuestMap.end() || it->second.empty())
					continue;

				for (auto* quest : it->second) {
					executePoke(quest, global, scaledVal);
				}
			}
		}

		void PokeQuestsRefresh()
		{
			auto executePoke = [](RE::TESQuest* quest, RE::TESGlobal* global) {
				if (!quest || !global || !quest->IsRunning())
					return;

				if (spdlog::should_log(spdlog::level::trace)) {
					const char* globalEdid = global->GetFormEditorID();
					const char* questEdid = quest->GetFormEditorID();
					std::string globalID = globalEdid ? globalEdid : std::format("0x{:08X}", global->GetFormID());
					std::string questID = questEdid ? questEdid : std::format("0x{:08X}", quest->GetFormID());
					SKSE::log::trace("[PKQT] Refresh Text Display Globals - Quest '{}' | Global: '{}'", questID, globalID);
				}

				UpdateCurrentInstanceGlobal(quest, global);
			};

			SKSE::log::trace("[PKQT] Updating CIG...");

			for (auto* global : g_pendingPokeGlobals) {
				if (!global)
					continue;

				auto it = g_globalToQuestMap.find(global);
				if (it == g_globalToQuestMap.end() || it->second.empty())
					continue;

				for (auto* quest : it->second) {
					executePoke(quest, global);
				}
			}
		}

		struct CachedActiveGlobal
		{
			RE::TESGlobal* global{ nullptr };
			float*         valuePtr{ nullptr };
			float          originalValue{ 0.0f };
			std::uint16_t  catIndex{ invCID };
		};

		struct CachedLeveledEntry
		{
			RE::TESLevItem*            levItem{ nullptr };
			std::uint16_t*             countPtr{ nullptr };
			std::int8_t*               chanceNonePtr{ nullptr };
			std::vector<std::uint16_t> originalCounts;
			std::int8_t                originalChanceNone{ 0 };
			std::uint16_t              catIndex{ invCID };
		};
		std::vector<CachedActiveGlobal> g_cachedActiveGlobals;
		std::vector<CachedLeveledEntry> g_cachedPersistLevItems;
		std::vector<CachedLeveledEntry> g_cachedDialogueLevItems;

		struct CategoryItemState
		{
			float lastMult{ -987.0f };
			float lastOffset{ -987.0f };
			bool  isPatched{ false };
		};

		struct RNItemBucketEntry
		{
			std::int32_t* valuePtr{ nullptr };
			std::int32_t  originalValue{ 0 };
		};
		std::vector<CategoryItemState>              g_categoryItemStates;
		std::vector<std::vector<RNItemBucketEntry>> g_categoryItemBuckets;

		struct RNLeveledBucketEntry
		{
			RE::TESLevItem*            levItem{ nullptr };
			std::vector<std::uint16_t> originalCounts;
			std::int8_t                originalChanceNone{ 0 };
			bool                       isStackableList{ false };
		};
		std::vector<std::vector<RNLeveledBucketEntry>> g_categoryPersistLevBuckets;
		std::vector<std::vector<RNLeveledBucketEntry>> g_categoryDialogueLevBuckets;

		struct RNGlobalBucketEntry
		{
			RE::TESGlobal* global{ nullptr };
			float          originalValue{ 0.0f };
		};
		std::vector<std::vector<RNGlobalBucketEntry>> g_categoryGlobalBuckets;

		void InitializeItemBuckets()
		{
			const size_t totalCats = g_idToCategoryMap.size();
			g_categoryItemBuckets.assign(totalCats, {});
			g_categoryItemStates.assign(totalCats, {});

			for (const auto& [form, cached] : DialogueGlobalRegistry::g_cachedItemCategoryMap) {
				if (form && cached.valuePtr) {
					std::uint16_t catIndex = GetCategoryID(cached.categoryID);
					if (catIndex == invCID) {
						catIndex = GetOrRegisterCategoryID(cached.categoryID);
					}

					if (catIndex >= g_categoryItemBuckets.size()) {
						g_categoryItemBuckets.resize(catIndex + 1);
						g_categoryItemStates.resize(catIndex + 1);
					}
					g_categoryItemBuckets[catIndex].push_back({ cached.valuePtr, cached.originalValue });
				}
			}
			g_itemBucketsInitialized.store(true);
		}

		void InitializeLeveledBuckets()
		{
			const size_t totalCats = g_idToCategoryMap.size();
			g_categoryPersistLevBuckets.assign(totalCats, {});
			g_categoryDialogueLevBuckets.assign(totalCats, {});

			auto cfg = g_settings.load();
			for (const auto& [levItem, category] : DialogueGlobalRegistry::g_cachedLeveledListCategoryMap) {
				if (!levItem)
					continue;
				const auto* catSettings = cfg ? cfg->GetCategory(category) : nullptr;
				bool        isPersist = catSettings ? catSettings->persistentLeveledLists : false;

				std::uint16_t catIndex = GetCategoryID(category);
				if (catIndex == invCID) {
					catIndex = GetOrRegisterCategoryID(category);
				}

				auto& targetVector = isPersist ? g_categoryPersistLevBuckets : g_categoryDialogueLevBuckets;
				if (catIndex >= targetVector.size()) {
					targetVector.resize(catIndex + 1);
				}

				RNLeveledBucketEntry lEntry{};
				lEntry.levItem = levItem;
				lEntry.originalChanceNone = levItem->chanceNone;
				lEntry.originalCounts.reserve(levItem->entries.size());
				for (const auto& levObj : levItem->entries) {
					lEntry.originalCounts.push_back(levObj.count);
					if (levObj.count > 1)
						lEntry.isStackableList = true;
				}
				targetVector[catIndex].push_back(lEntry);
			}
			g_leveledBucketsInitialized.store(true);
		}

		void InitializeGlobalBuckets()
		{
			const size_t totalCats = g_idToCategoryMap.size();
			g_categoryGlobalBuckets.assign(totalCats, {});
			for (const auto& [global, entry] : DialogueGlobalRegistry::g_cachedCategoryMap) {
				if (!global)
					continue;

				std::uint16_t catIndex = GetCategoryID(entry.categoryID);
				if (catIndex == invCID) {
					catIndex = GetOrRegisterCategoryID(entry.categoryID);
				}

				if (catIndex >= g_categoryGlobalBuckets.size()) {
					g_categoryGlobalBuckets.resize(catIndex + 1);
				}

				g_categoryGlobalBuckets[catIndex].push_back({ global, global->value });
			}
			g_globalBucketsInitialized.store(true);
		}

		void ResetPersistentLevListsToBaseline()
		{
			if (!g_baselinePersistLevItemsMap.empty()) {
				for (auto& [levItem, entry] : g_baselinePersistLevItemsMap) {
					if (!levItem)
						continue;
					for (size_t i = 0; i < levItem->entries.size() && i < entry.originalCounts.size(); ++i) {
						levItem->entries[i].count = entry.originalCounts[i];
					}
					levItem->chanceNone = entry.originalChanceNone;
				}
				g_baselinePersistLevItemsMap.clear();
			}
		}

		void ResetDialogueLevListsToBaseline()
		{
			if (!g_baselineDialogueLevItemsMap.empty()) {
				for (auto& [levItem, entry] : g_baselineDialogueLevItemsMap) {
					if (!levItem)
						continue;
					for (size_t i = 0; i < levItem->entries.size() && i < entry.originalCounts.size(); ++i) {
						levItem->entries[i].count = entry.originalCounts[i];
					}
					levItem->chanceNone = entry.originalChanceNone;
				}
				g_baselineDialogueLevItemsMap.clear();
			}
		}

		void ResetGlobalsToBaseline()
		{
			if (!g_isDialogueScaled.load() && g_baselineGlobalsMap.empty()) {
				return;
			}

			for (auto& [global, entry] : g_baselineGlobalsMap) {
				if (global) {
					global->value = entry.originalValue;
				}
			}
			g_baselineGlobalsMap.clear();
			g_activeScaledGlobalValues.clear();
			g_isDialogueScaled.store(false);
			g_targetSpeakerHandle.reset();

			ResetDialogueLevListsToBaseline();
		}

		void ResetSettGlobalsToBaseline()
		{
			if (!g_baselineSettGlobalsMap.empty()) {
				for (auto& [global, entry] : g_baselineSettGlobalsMap) {
					if (global)
						global->value = entry.originalValue;
				}
				g_baselineSettGlobalsMap.clear();
			}

			if (!g_baselineSettGmstMap.empty()) {
				for (auto& [gmst, entry] : g_baselineSettGmstMap) {
					if (gmst) {
						if (gmst->GetType() == RE::Setting::Type::kFloat) {
							gmst->SetFloat(entry.originalValue);
						} else {
							gmst->SetInteger(static_cast<std::int32_t>(entry.originalValue));
						}
					}
				}
				g_baselineSettGmstMap.clear();
			}
		}

		void ApplySettModifiers()
		{
			auto cfg = g_settings.load();
			if (!cfg || !cfg->scaleCatValues)
				return;

			BENCHMARK_SCOPE_IF("ApplySettModifiers", spdlog::should_log(spdlog::level::debug));

			g_categoryRateCache.assign(g_idToCategoryMap.size(), {});
			InitializeCategoryBaseRateCache();
			std::vector<int> categoryLastId(g_idToCategoryMap.size(), -987);
			for (const auto& entry : DialogueGlobalRegistry::g_cachedSettEntries) {
				if (!entry.global && !entry.gmst)
					continue;

				std::uint16_t catIndex = GetCategoryID(entry.categoryID);
				if (catIndex == invCID) {
					catIndex = GetOrRegisterCategoryID(entry.categoryID);
				}

				const auto* catSettings = cfg->GetCategory(entry.categoryID);
				if (!catSettings || !catSettings->enabled)
					continue;

				if (catIndex >= categoryLastId.size()) {
					categoryLastId.resize(catIndex + 1, -987);
				}

				if (!g_categoryRateCache[catIndex].isValid) {
					UpdateCategoryExternalRates(catIndex);
					categoryLastId[catIndex] = entry.idNumber;
				} else if (entry.idNumber != 0 && entry.idNumber != categoryLastId[catIndex]) {
					UpdateCategoryExternalRates(catIndex);
					categoryLastId[catIndex] = entry.idNumber;
				}

				if (catIndex >= g_categoryRateCache.size() || !g_categoryRateCache[catIndex].isValid)
					continue;
				const auto& rate = g_categoryRateCache[catIndex];

				if (entry.global) {
					g_baselineSettGlobalsMap.try_emplace(entry.global, GlobalBaselineEntry{ entry.global->value, catIndex, "" });
				} else if (entry.gmst) {
					bool  isFloat = (entry.gmst->GetType() == RE::Setting::Type::kFloat);
					float origVal = isFloat ? entry.gmst->GetFloat() : static_cast<float>(entry.gmst->GetInteger());
					g_baselineSettGmstMap.try_emplace(entry.gmst, GlobalBaselineEntry{ origVal, catIndex, "" });
				}

				float combinedMult = rate.dynamicScaling ? (rate.usesSellBarterMult ? rate.combinedSellMult : rate.combinedBuyMult) : rate.combinedItemMult;

				if (entry.global) {
					const auto& baseline = g_baselineSettGlobalsMap[entry.global];
					float       scaledVal = (baseline.originalValue * combinedMult) + rate.flatAlienOffset;
					float       endVal = std::clamp(scaledVal, 0.0f, 2147481600.0f);
					entry.global->value = endVal;
					g_activeScaledGlobalValues[entry.global] = endVal;
				} else if (entry.gmst) {
					const auto& baseline = g_baselineSettGmstMap[entry.gmst];
					float       scaledVal = (baseline.originalValue * combinedMult) + rate.flatAlienOffset;
					float       endVal = std::clamp(scaledVal, 0.0f, 2147481600.0f);
					if (entry.gmst->GetType() == RE::Setting::Type::kFloat) {
						entry.gmst->data.f = endVal;
					} else {
						entry.gmst->data.i = static_cast<std::int32_t>(std::round(endVal));
					}
				}
			}

			const size_t totalCats = g_idToCategoryMap.size();
			for (intptr_t i = 0; i < static_cast<intptr_t>(totalCats); ++i) {
				if (static_cast<size_t>(i) >= g_categoryRateCache.size()) {
					g_categoryRateCache.resize(i + 1, {});
				}
				if (!g_categoryRateCache[i].isValid) {
					UpdateCategoryExternalRates(static_cast<std::uint16_t>(i));
				}
			}
		}

		void ApplyLeveledListModifiers(bool isPersistentContext)
		{
			auto cfg = g_settings.load();
			if (!cfg || !cfg->scaleCatValues)
				return;

			if (!isPersistentContext) {
				ApplyLeveledListModifiers(true);
				ResetDialogueLevListsToBaseline();
			}

			BENCHMARK_SCOPE_IF("ApplyLeveledListModifiers", spdlog::should_log(spdlog::level::debug));

			if (!g_leveledBucketsInitialized.load()) {
				InitializeLeveledBuckets();
			}

			const auto&  lkfcSource = isPersistentContext ? g_categoryPersistLevBuckets : g_categoryDialogueLevBuckets;
			const size_t lkfcCount = lkfcSource.size();
			const size_t rateCacheSize = g_categoryRateCache.size();

			for (std::uint16_t catIndex = 0; catIndex < lkfcCount; ++catIndex) {
				const auto& levList = lkfcSource[catIndex];
				if (levList.empty())
					continue;

				if (catIndex >= rateCacheSize)
					continue;
				const auto& rate = g_categoryRateCache[catIndex];
				if (!rate.isValid || !rate.enabled)
					continue;

				const float combinedMult = rate.dynamicScaling ? (rate.usesSellBarterMult ? rate.combinedSellMult : rate.combinedBuyMult) : rate.combinedItemMult;
				const float flatOffset = rate.flatAlienOffset;

				for (const auto& entry : levList) {
					auto* levItem = entry.levItem;
					if (!levItem)
						continue;

					if (isPersistentContext) {
						g_baselinePersistLevItemsMap.try_emplace(levItem, LeveledItemBaselineEntry{ entry.originalCounts, entry.originalChanceNone, catIndex, "" });
					} else {
						g_baselineDialogueLevItemsMap.try_emplace(levItem, LeveledItemBaselineEntry{ entry.originalCounts, entry.originalChanceNone, catIndex, "" });
					}

					if (entry.isStackableList) {
						const size_t subentryCount = std::min(levItem->entries.size(), entry.originalCounts.size());
						if (subentryCount == 0)
							continue;

						constexpr size_t StripS = 128;
						std::uint16_t*   stripDstPtrs[StripS];
						float            stripOrigVals[StripS];
						float            stripScaledVals[StripS];

						int stripCount = 0;
						for (size_t i = 0; i < subentryCount; ++i) {
							stripDstPtrs[stripCount] = &levItem->entries[i].count;
							stripOrigVals[stripCount] = static_cast<float>(entry.originalCounts[i]);
							stripCount++;

							if (stripCount == StripS || i == subentryCount - 1) {
								const float* __restrict srcVals = stripOrigVals;
								float* __restrict dstVals = stripScaledVals;

								const int count = stripCount;

								VECTORIZE for (int k = 0; k < count; ++k)
								{
									const float orig = srcVals[k];
									const float scaledVal = (orig * combinedMult) + flatOffset;

									const float minAllowed = (orig > 0.0f) ? 1.0f : 0.0f;
									const float clampedVal = (scaledVal > minAllowed) ? scaledVal : minAllowed;

									dstVals[k] = (clampedVal < 65535.0f) ? clampedVal : 65535.0f;
								}

								for (int k = 0; k < stripCount; ++k) {
									*stripDstPtrs[k] = static_cast<std::uint16_t>(dstVals[k] + 0.5f);
								}
								stripCount = 0;
							}
						}
					} else {
						const float baselineSuccessProb = 100.0f - static_cast<float>(entry.originalChanceNone);
						float       scaledSuccessProb = (baselineSuccessProb * combinedMult) + flatOffset;
						scaledSuccessProb = scaledSuccessProb < 0.0f ? 0.0f : (scaledSuccessProb > 100.0f ? 100.0f : scaledSuccessProb);
						levItem->chanceNone = static_cast<std::int8_t>(100.0f - scaledSuccessProb + 0.5f);
					}
				}
			}
		}

		void ResetFactionCrimeToBaseline()
		{
			for (const auto& factEntry : DialogueGlobalRegistry::g_cachedFactionCrimeEntries) {
				if (!factEntry.faction)
					continue;

				auto& values = factEntry.faction->crimeData.crimevalues;
				switch (factEntry.fieldType) {
				case DialogueGlobalRegistry::FactionCrimeField::kStealMult:
					values.stealCrimeGoldMult = factEntry.originalValue;
					break;
				case DialogueGlobalRegistry::FactionCrimeField::kMurder:
					values.murderCrimeGold = static_cast<std::uint16_t>(factEntry.originalValue);
					break;
				case DialogueGlobalRegistry::FactionCrimeField::kAssault:
					values.assaultCrimeGold = static_cast<std::uint16_t>(factEntry.originalValue);
					break;
				case DialogueGlobalRegistry::FactionCrimeField::kTrespass:
					values.trespassCrimeGold = static_cast<std::uint16_t>(factEntry.originalValue);
					break;
				case DialogueGlobalRegistry::FactionCrimeField::kPickpocket:
					values.pickpocketCrimeGold = static_cast<std::uint16_t>(factEntry.originalValue);
					break;
				case DialogueGlobalRegistry::FactionCrimeField::kEscape:
					values.escapeCrimeGold = static_cast<std::uint16_t>(factEntry.originalValue);
					break;
				case DialogueGlobalRegistry::FactionCrimeField::kWerewolf:
					values.werewolfCrimeGold = static_cast<std::uint16_t>(factEntry.originalValue);
					break;
				}
			}
		}

		void ApplyFactionCrimeModifiers()
		{
			auto cfg = g_settings.load();
			if (!cfg || !cfg->scaleCatValues)
				return;

			BENCHMARK_SCOPE_IF("ApplyFactionCrimeModifiers", spdlog::should_log(spdlog::level::debug));

			const size_t rateCacheSize = g_categoryRateCache.size();

			for (const auto& factEntry : DialogueGlobalRegistry::g_cachedFactionCrimeEntries) {
				if (!factEntry.faction)
					continue;

				std::uint16_t catIndex = GetCategoryID(factEntry.categoryID);
				if (catIndex == invCID) {
					catIndex = GetOrRegisterCategoryID(factEntry.categoryID);
				}

				if (catIndex >= rateCacheSize)
					continue;

				const auto& rates = g_categoryRateCache[catIndex];
				if (!rates.isValid || !rates.enabled)
					continue;

				const float combinedMult = rates.dynamicScaling ? (rates.usesSellBarterMult ? rates.combinedSellMult : rates.combinedBuyMult) : rates.combinedItemMult;
				const float flatOffset = rates.flatAlienOffset;

				float scaledVal = (factEntry.originalValue * combinedMult) + flatOffset;
				auto& values = factEntry.faction->crimeData.crimevalues;

				if (factEntry.fieldType == DialogueGlobalRegistry::FactionCrimeField::kStealMult) {
					scaledVal = std::clamp(scaledVal, 0.0f, 2147481600.0f);
					values.stealCrimeGoldMult = scaledVal;
				} else {
					scaledVal = std::clamp(scaledVal, 0.0f, 65535.0f);
					std::uint16_t smolVal = static_cast<std::uint16_t>(scaledVal + 0.5f);

					switch (factEntry.fieldType) {
					case DialogueGlobalRegistry::FactionCrimeField::kMurder:
						values.murderCrimeGold = smolVal;
						break;
					case DialogueGlobalRegistry::FactionCrimeField::kAssault:
						values.assaultCrimeGold = smolVal;
						break;
					case DialogueGlobalRegistry::FactionCrimeField::kTrespass:
						values.trespassCrimeGold = smolVal;
						break;
					case DialogueGlobalRegistry::FactionCrimeField::kPickpocket:
						values.pickpocketCrimeGold = smolVal;
						break;
					case DialogueGlobalRegistry::FactionCrimeField::kEscape:
						values.escapeCrimeGold = smolVal;
						break;
					case DialogueGlobalRegistry::FactionCrimeField::kWerewolf:
						values.werewolfCrimeGold = smolVal;
						break;
					default:
						break;
					}
				}
			}
		}

		void ApplyActiveGlobalModifiers()
		{
			auto cfg = g_settings.load();
			if (!cfg || !cfg->scaleCatValues) {
				ResetGlobalsToBaseline();
				return;
			}

			BENCHMARK_SCOPE_IF("ApplyActiveGlobalModifiers", spdlog::should_log(spdlog::level::debug));

			std::unique_lock<std::shared_mutex> lock(g_modifierMutex);

			if (g_isDialogueScaled.load()) {
				SKSE::log::warn("[TDBG] Dialogue already scaled; skipping ApplyActiveGlobalModifiers.");
				return;
			}

			if (!g_globalBucketsInitialized.load()) {
				InitializeGlobalBuckets();
			}

			g_isDialogueScaled.store(true);

			const size_t gkfcCount = g_categoryGlobalBuckets.size();
			for (std::uint16_t catIndex = 0; catIndex < gkfcCount; ++catIndex) {
				const auto& globalList = g_categoryGlobalBuckets[catIndex];
				if (globalList.empty())
					continue;

				if (catIndex >= g_categoryRateCache.size() || !g_categoryRateCache[catIndex].isValid)
					continue;

				const auto& cache = g_categoryRateCache[catIndex];
				if (!cache.enabled)
					continue;

				const float combinedMult = cache.dynamicScaling ? (cache.usesSellBarterMult ? cache.combinedSellMult : cache.combinedBuyMult) : cache.combinedItemMult;
				const float flatOffset = cache.flatAlienOffset;

				const size_t globalCount = globalList.size();
				const RNGlobalBucketEntry* __restrict entries = globalList.data();

				constexpr size_t StripS = 128;
				RE::TESGlobal*   stripGlobals[StripS];
				float            stripOrigVals[StripS];
				float            stripScaledVals[StripS];

				size_t stripCount = 0;
				for (size_t i = 0; i < globalCount; ++i) {
					auto* global = entries[i].global;
					if (global && !g_baselineSettGlobalsMap.contains(global)) {
						stripGlobals[stripCount] = global;
						stripOrigVals[stripCount] = entries[i].originalValue;
						stripCount++;

						if (stripCount == StripS || i == globalCount - 1) {
							const float* __restrict srcValues = stripOrigVals;
							float* __restrict dstValues = stripScaledVals;

							VECTORIZE for (size_t k = 0; k < stripCount; ++k)
							{
								float val = (srcValues[k] * combinedMult) + flatOffset;
								dstValues[k] = val < 0.0f ? 0.0f : (val > 2147481600.0f ? 2147481600.0f : val);
							}

							for (size_t k = 0; k < stripCount; ++k) {
								auto* g = stripGlobals[k];
								float rawValue = dstValues[k];

								bool  isQuestGlobal = DialoguePriceTransformer::g_globalToQuestMap.contains(g);
								float finalValue = isQuestGlobal ? std::round(rawValue) : rawValue;

								g->value = finalValue;
								g_activeScaledGlobalValues[g] = finalValue;
								g_baselineGlobalsMap.try_emplace(g, GlobalBaselineEntry{ srcValues[k], catIndex, "" });
							}
							stripCount = 0;
						}
					}
				}
			}
			PokeQuestsScaled();
			ApplyLeveledListModifiers(false);
			ApplyItemModifiers();
			ApplyFactionCrimeModifiers();
		}

		void ResetItemsToBaseline()
		{
			const size_t ikfcCount = g_categoryItemBuckets.size();
			for (size_t catIndex = 0; catIndex < ikfcCount; ++catIndex) {
				auto&        itemList = g_categoryItemBuckets[catIndex];
				const size_t itemCount = itemList.size();

				if (itemCount == 0)
					continue;

				const RNItemBucketEntry* __restrict items = itemList.data();

				for (size_t i = 0; i < itemCount; ++i) {
					if (!items[i].valuePtr)
						continue;
					*items[i].valuePtr = items[i].originalValue;
				}
			}

			for (auto& state : g_categoryItemStates) {
				state.isPatched = false;
				state.lastMult = -987.0f;
				state.lastOffset = -987.0f;
			}
			g_itemsArePatched.store(false);
		}

		void ResetPersistentToBaseline()
		{
			std::unique_lock<std::shared_mutex> lock(g_modifierMutex);
			ResetSettGlobalsToBaseline();
			ResetItemsToBaseline();
			ResetPersistentLevListsToBaseline();
			ResetFactionCrimeToBaseline();
			SKSE::log::trace("[TDBG] Persistent patches reset to baseline.");
		}

		void ApplyItemModifiers()
		{
			auto cfg = g_settings.load();
			if (!cfg || !cfg->scaleCatValues)
				return;

			BENCHMARK_SCOPE_IF("ApplyItemModifiers", spdlog::should_log(spdlog::level::debug));

			if (!g_itemBucketsInitialized.load()) {
				InitializeItemBuckets();
			}

			const size_t ikfcCount = g_categoryItemBuckets.size();
			const size_t rateCacheSize = g_categoryRateCache.size();
			const size_t stateCacheSize = g_categoryItemStates.size();

			for (std::uint16_t catIndex = 0; catIndex < ikfcCount; ++catIndex) {
				auto& itemList = g_categoryItemBuckets[catIndex];
				if (itemList.empty())
					continue;
				if (catIndex >= rateCacheSize || catIndex >= stateCacheSize)
					continue;
				const auto& rates = g_categoryRateCache[catIndex];
				if (!rates.isValid)
					continue;

				auto& state = g_categoryItemStates[catIndex];

				if (!rates.enabled) {
					if (state.isPatched) {
						for (const auto& entry : itemList) {
							if (entry.valuePtr) {
								*entry.valuePtr = entry.originalValue;
							}
						}
						state.isPatched = false;
						state.lastMult = -987.0f;
						state.lastOffset = -987.0f;
					}
					continue;
				}

				const float currentMult = rates.combinedItemMult;
				const float currentOffset = rates.flatAlienOffset;

				if (state.isPatched && state.lastMult == currentMult && state.lastOffset == currentOffset) {
					continue;
				}

				const size_t itemCount = itemList.size();
				const RNItemBucketEntry* __restrict items = itemList.data();

				constexpr size_t StripS = 128;
				std::int32_t*    stripPtrs[StripS];
				float            stripOrigVals[StripS];
				std::int32_t     stripScaledVals[StripS];
				size_t           stripCount = 0;
				for (size_t i = 0; i < itemCount; ++i) {
					if (items[i].valuePtr) {
						stripPtrs[stripCount] = items[i].valuePtr;
						stripOrigVals[stripCount] = static_cast<float>(items[i].originalValue);
						stripCount++;
					}

					if (stripCount == StripS || (i == itemCount - 1 && stripCount > 0)) {
						const float* __restrict srcVals = stripOrigVals;
						std::int32_t* __restrict dstVals = stripScaledVals;

						VECTORIZE for (size_t k = 0; k < stripCount; ++k)
						{
							float scaledVal = (srcVals[k] * currentMult) + currentOffset;
							scaledVal = scaledVal < 0.0f ? 0.0f : (scaledVal > 2147481600.0f ? 2147481600.0f : scaledVal);
							dstVals[k] = static_cast<std::int32_t>(scaledVal + 0.5f);
						}

						for (size_t k = 0; k < stripCount; ++k) {
							*stripPtrs[k] = dstVals[k];
						}
						stripCount = 0;
					}
				}

				state.lastMult = currentMult;
				state.lastOffset = currentOffset;
				state.isPatched = true;
			}
		}
	}

	void CheckAndApplyPersistentPatches(bool forceUpdate)
	{
		DialoguePriceTransformer::ResetPersistentToBaseline();
		auto cfg = g_settings.load();
		if (!cfg || !cfg->scaleCatValues)
			return;

		BENCHMARK_SCOPE_IF("CheckAndApplyPersistentPatches", spdlog::should_log(spdlog::level::info));

		bool expected = false;
		if (!g_isItemPatchingInProgress.compare_exchange_strong(expected, true)) {
			return;
		}

		struct ScopeGuard
		{
			~ScopeGuard() { g_isItemPatchingInProgress.store(false); }
		} guard;

		float currentDay = 0.0f;
		if (auto daysGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("GameDaysPassed")) {
			currentDay = std::max(0.0f, daysGlobal->value);
			g_cachedDaysPassed = currentDay;
		}

		std::uint16_t currentLevel = 1;
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			currentLevel = player->GetLevel();
		}

		bool dayChanged = (g_lastPatchedDay < 0.0f) || (currentDay >= g_lastPatchedDay + 1.0f);
		bool levelChanged = (g_lastPatchedLevel != currentLevel);
		bool needsReapplication = !g_itemsArePatched.load();

		if (forceUpdate || dayChanged || levelChanged || needsReapplication) {
			g_lastPatchedDay = currentDay;
			g_lastPatchedLevel = currentLevel;

			std::unique_lock<std::shared_mutex> lock(g_modifierMutex);
			DialoguePriceTransformer::ApplySettModifiers();
			DialoguePriceTransformer::ApplyLeveledListModifiers(true);
			DialoguePriceTransformer::ApplyItemModifiers();
			DialoguePriceTransformer::ApplyFactionCrimeModifiers();

			g_itemsArePatched.store(true);
		}
	}

	void PreCacheQuestGlobals()
	{
		DialoguePriceTransformer::g_globalToQuestMap.clear();

		auto dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			SKSE::log::error("[PCQG] TESDataHandler returned null!");
			return;
		}

		const auto& quests = dataHandler->GetFormArray<RE::TESQuest>();
		SKSE::log::debug("[PCQG] DH scan with {} quests...", quests.size());

		for (auto* quest : quests) {
			if (!quest || !quest->textGlobals)
				continue;

			const char* qEdid = quest->GetFormEditorID();
			std::string questName = (qEdid && qEdid[0] != '\0') ? qEdid : std::format("0x{:08X}", quest->GetFormID());

			for (auto* global : *quest->textGlobals) {
				if (!global)
					continue;

				if (DialogueGlobalRegistry::g_dialogueStepGlobals.contains(global)) {
					const char* gEdid = global->GetFormEditorID();
					std::string globalName = (gEdid && gEdid[0] != '\0') ? gEdid : std::format("0x{:08X}", global->GetFormID());

					auto& questVec = DialoguePriceTransformer::g_globalToQuestMap[global];
					if (std::find(questVec.begin(), questVec.end(), quest) == questVec.end()) {
						questVec.push_back(quest);
						SKSE::log::trace("[PCQG] Global '{}' (0x{:08X}) <-> Quest '{}' (0x{:08X})", globalName, global->GetFormID(), questName, quest->GetFormID());
					}
				}
			}
		}
		SKSE::log::info("[PCQG] Found {} text display globals for updates.", DialoguePriceTransformer::g_globalToQuestMap.size());
	}

	namespace ConsoleCommands
	{
		static void AuditConsoleCommandHooks(bool firstRun)
		{
			static const std::vector<const char*> kSacrificialCommands = {
				"StartAllQuests",
				"CompleteAllQuestStages",
				"ToggleFogOfWar",
				"ToggleWireframe",
				"ToggleWaterSystem",
				"ToggleSky",
				"ToggleTrees",
				"CenterOnWorld"
			};

			SKSE::log::warn("[CCMD] Console Command Hook ({})", firstRun ? "Before" : "After");

			for (const char* sacrificialName : kSacrificialCommands) {
				if (auto cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(sacrificialName)) {
					SKSE::log::debug("[CCMD] Found {:<25} Address: 0x{:X}", sacrificialName, reinterpret_cast<std::uintptr_t>(cmd));
				} else {
					SKSE::log::warn("[CCMD] Not found: {}", sacrificialName);
				}
			}
		}

		static void WriteConsoleMessage(const std::string& msg)
		{
			if (auto console = RE::ConsoleLog::GetSingleton()) {
				console->Print("%s", msg.c_str());
			}
		}

		static bool DispatchCommand(
			const RE::SCRIPT_PARAMETER* /*paramInfo*/,
			RE::SCRIPT_FUNCTION::ScriptData* scriptData,
			RE::TESObjectREFR* /*thisObj*/,
			RE::TESObjectREFR* /*containingObj*/,
			RE::Script* /*scriptObj*/,
			RE::ScriptLocals* /*locals*/,
			double& result,
			std::uint32_t& /*opcodeOffsetPtr*/)
		{
			result = 0.0;
			std::string input;

			if (scriptData && scriptData->numParams > 0) {
				if (auto strChunk = scriptData->GetStringChunk()) {
					input = strChunk->GetString();
				}
			}

			if (auto fstNoSpace = input.find_first_not_of(" \t"); fstNoSpace != std::string::npos) {
				input.erase(0, fstNoSpace);
			}
			auto fstYeSpace = input.find_first_of(" \t");
			auto fstBar = input.find('-');
			if (fstBar != std::string::npos && (fstYeSpace == std::string::npos || fstBar < fstYeSpace)) {
				input[fstBar] = ' ';
			}

			std::istringstream iss(input);
			std::string        action;
			iss >> action;

			std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			if (action == "hardreset") {
				if (MCPddcMenu::IsDialogueMenuOpen()) {
					WriteConsoleMessage("DynamicDialogueCosts: Cannot reload NG INI config while Dialogue Menu is open!");
					result = 0.0;
					return true;
				}
				DialoguePriceTransformer::ResetPersistentToBaseline();
				DialoguePriceTransformer::ResetGlobalsToBaseline();
				Serialization::ResetState();
				DialogueGlobalRegistry::LoadGlobalPatches();
				RebuildCategoryRegistry();
				DialoguePriceTransformer::g_itemBucketsInitialized.store(false);
				DialoguePriceTransformer::g_globalBucketsInitialized.store(false);
				DialoguePriceTransformer::g_leveledBucketsInitialized.store(false);
				MCPddcMenu::RefreshPresets();
				MCPddcMenu::InvalidateCategoryCache();

				CheckAndApplyPersistentPatches(true);
				auto cfg = g_settings.load();
				if (cfg && cfg->useQuestWatcher) {
					QuestOrderManager::EvaluateAndApplyProfile("$FULL");
				}

				DialoguePriceTransformer::PokeQuestsRefresh();
				WriteConsoleMessage("DynamicDialogueCosts: hard reset complete.");
				result = 1.0;
			} else if (action == "reloadcats") {
				if (MCPddcMenu::IsDialogueMenuOpen()) {
					WriteConsoleMessage("DynamicDialogueCosts: Cannot reload cats while in Dialogue!");
					result = 0.0;
					return true;
				}
				auto cfg = g_settings.load();
				if (cfg) {
					Settings menuSettings = *cfg;
					MCPddcMenu::SanitizeSettings(menuSettings);

					if (menuSettings.hideCategories) {
						MCPddcMenu::SetCategoryHiding(true, false);
					}

					DialoguePriceTransformer::ResetPersistentToBaseline();
					g_settings.store(std::make_shared<Settings>(menuSettings));
					DialogueGlobalRegistry::LoadGlobalPatches();
					RebuildCategoryRegistry();
					DialoguePriceTransformer::g_itemBucketsInitialized.store(false);
					DialoguePriceTransformer::g_leveledBucketsInitialized.store(false);
					DialoguePriceTransformer::g_globalBucketsInitialized.store(false);
					MCPddcMenu::RefreshPresets();
					MCPddcMenu::InvalidateCategoryCache();

					CheckAndApplyPersistentPatches(true);
					if (cfg->useQuestWatcher) {
						QuestOrderManager::EvaluateAndApplyProfile("$FULL");
					}

					DialoguePriceTransformer::PokeQuestsRefresh();
					WriteConsoleMessage("DynamicDialogueCosts: Cats reloaded and current settings applied.");
					result = 1.0;
				} else {
					WriteConsoleMessage("DynamicDialogueCosts: Failed to retrieve current settings!");
					SKSE::log::error("[CCMD] Failed to retrieve current settings: reloadcats.");
					result = 0.0;
				}
			} else if (action == "savetoini") {
				if (auto cfg = g_settings.load()) {
					SaveSettings(*cfg);
					WriteConsoleMessage("DynamicDialogueCosts: Current loaded settings exported to DynamicDialogueCosts.ini");
					result = 1.0;
				} else {
					WriteConsoleMessage("DynamicDialogueCosts: Failed to retrieve current settings!");
					SKSE::log::error("[CCMD] Failed to retrieve current settings: savetoini.");
					result = 0.0;
				}
			} else if (action == "scancats") {
				std::shared_lock<std::shared_mutex> lock(g_modifierMutex);
				auto                                cfg = g_settings.load();
				if (!cfg) {
					WriteConsoleMessage("DynamicDialogueCosts: Failed to retrieve current settings!!!");
					SKSE::log::error("[CCMD] Failed to retrieve current settings: scancats.");
					result = 0.0;
					return true;
				}

				int oldLevel = cfg->debugLevel;
				if (oldLevel < 3) {
					SetLogLevel(3);
				}
				struct LogGuard
				{
					int prev;
					~LogGuard() { SetLogLevel(prev); }
				} logGuard{ oldLevel };

				BENCHMARK_SCOPE_IF("Console::scancats", cfg->enableDebug);
				if (!MCPddcMenu::s_bootStateInitialized) {
					MCPddcMenu::ResetUIBufferFromMemory();
					MCPddcMenu::s_bootStateInitialized = true;
					LogMenuFrameworkStatus("Mb started (scancats).");
				}
				std::string targetCatID;
				if (iss >> targetCatID) {
					std::transform(targetCatID.begin(), targetCatID.end(), targetCatID.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				}

				bool enableFileLogging = cfg->enableDebug;
				auto PrintOutput = [enableFileLogging](const std::string& msg) {
					if (auto console = RE::ConsoleLog::GetSingleton()) {
						console->Print("%s", msg.c_str());
					}
					if (enableFileLogging) {
						if (auto log = spdlog::default_logger()) {
							log->info("{}", msg);
						}
					}
				};

				if (!targetCatID.empty()) {
					if (!cfg->categories.contains(targetCatID)) {
						WriteConsoleMessage(std::format("DynamicDialogueCosts: Category '{}' not found.", targetCatID));
						result = 0.0;
						return true;
					}
					PrintOutput(std::format("DynamicDialogueCosts: Category Scan Start for '{}':", targetCatID));
				} else {
					PrintOutput("DynamicDialogueCosts: Category Scan Start: ");
				}

				const auto& activeCategories = MCPddcMenu::GetActiveCategories();
				for (const auto& catID : activeCategories) {
					if (!targetCatID.empty() && catID != targetCatID) {
						continue;
					}

					const auto* catSettings = cfg->GetCategory(catID);
					bool        isEnabled = catSettings ? catSettings->enabled : true;
					PrintOutput(std::format("< Category - {} | Enabled: {} >", catID, isEnabled));
					std::vector<std::string> categoryEntries;
					size_t                   itemCount = 0;
					auto                     FormatVal = [](float val) {
						std::string s = std::format("{:.3f}", val);
						if (s.find('.') != std::string::npos) {
							s.erase(s.find_last_not_of('0') + 1);
							if (s.back() == '.') {
								s.pop_back();
							}
						}
						return s;
					};

					for (const auto& [global, registeredCat] : DialogueGlobalRegistry::g_cachedCategoryMap) {
						if (registeredCat.categoryID == catID && global) {
							float       val = global->value;
							const char* edid = global->GetFormEditorID();
							std::string name = (edid && edid[0] != '\0') ? edid : std::format("0x{:08X}", global->GetFormID());
							categoryEntries.push_back(std::format("{}: {}", name, FormatVal(val)));
						}
					}

					for (const auto& settEntry : DialogueGlobalRegistry::g_cachedSettEntries) {
						if (settEntry.categoryID == catID) {
							if (settEntry.global) {
								float       val = settEntry.global->value;
								const char* edid = settEntry.global->GetFormEditorID();
								std::string name = (edid && edid[0] != '\0') ? edid : std::format("0x{:08X}", settEntry.global->GetFormID());
								categoryEntries.push_back(std::format("[SETT:{}] {}: {}", settEntry.idNumber, name, FormatVal(val)));
							} else if (settEntry.gmst) {
								float       val = (settEntry.gmst->GetType() == RE::Setting::Type::kFloat) ? settEntry.gmst->GetFloat() : static_cast<float>(settEntry.gmst->GetInteger());
								const char* nameStr = settEntry.gmst->GetName();
								std::string name = (nameStr && nameStr[0] != '\0') ? nameStr : settEntry.editorID;
								categoryEntries.push_back(std::format("[GMST:{}] {}: {}", settEntry.idNumber, name, FormatVal(val)));
							}
						}
					}

					auto extRange = DialogueGlobalRegistry::g_cachedExternalGlobalCategoryMap.equal_range(catID);
					for (auto it = extRange.first; it != extRange.second; ++it) {
						const auto& entry = it->second;
						if (entry.global) {
							float       val = entry.global->value;
							const char* edid = entry.global->GetFormEditorID();
							std::string name = (edid && edid[0] != '\0') ? edid : std::format("0x{:08X}", entry.global->GetFormID());
							categoryEntries.push_back(std::format("[EXTG] {}: {}", name, FormatVal(val)));
						}
					}

					for (const auto& factEntry : DialogueGlobalRegistry::g_cachedFactionCrimeEntries) {
						if (factEntry.categoryID == catID && factEntry.faction) {
							std::string name = factEntry.editorID;
							std::string fieldName;
							float       currentVal = 0.0f;

							switch (factEntry.fieldType) {
							case DialogueGlobalRegistry::FactionCrimeField::kStealMult:
								fieldName = "stealmult";
								currentVal = factEntry.faction->crimeData.crimevalues.stealCrimeGoldMult;
								break;
							case DialogueGlobalRegistry::FactionCrimeField::kMurder:
								fieldName = "murder";
								currentVal = static_cast<float>(factEntry.faction->crimeData.crimevalues.murderCrimeGold);
								break;
							case DialogueGlobalRegistry::FactionCrimeField::kAssault:
								fieldName = "assault";
								currentVal = static_cast<float>(factEntry.faction->crimeData.crimevalues.assaultCrimeGold);
								break;
							case DialogueGlobalRegistry::FactionCrimeField::kTrespass:
								fieldName = "trespass";
								currentVal = static_cast<float>(factEntry.faction->crimeData.crimevalues.trespassCrimeGold);
								break;
							case DialogueGlobalRegistry::FactionCrimeField::kPickpocket:
								fieldName = "pickpocket";
								currentVal = static_cast<float>(factEntry.faction->crimeData.crimevalues.pickpocketCrimeGold);
								break;
							case DialogueGlobalRegistry::FactionCrimeField::kEscape:
								fieldName = "jailbreak";
								currentVal = static_cast<float>(factEntry.faction->crimeData.crimevalues.escapeCrimeGold);
								break;
							case DialogueGlobalRegistry::FactionCrimeField::kWerewolf:
								fieldName = "shapeshift";
								currentVal = static_cast<float>(factEntry.faction->crimeData.crimevalues.werewolfCrimeGold);
								break;
							}

							categoryEntries.push_back(std::format("[FACT:{}] {}: {}", name, fieldName, FormatVal(currentVal)));
							//SKSE::log::trace("[CCMD] Cat: {} | Faction: {} | Field: {} | Val: {} | OgVal: {}", catID, name, fieldName, FormatVal(currentVal), FormatVal(factEntry.originalValue));
						}
					}

					for (const auto& [levItem, registeredCat] : DialogueGlobalRegistry::g_cachedLeveledListCategoryMap) {
						if (registeredCat == catID && levItem) {
							const char* edid = levItem->GetFormEditorID();
							std::string name = (edid && edid[0] != '\0') ? edid : std::format("0x{:08X}", levItem->GetFormID());
							std::string rangeStr;
							for (size_t j = 0; j < levItem->entries.size(); ++j) {
								const auto& entry = levItem->entries[j];
								if (j > 0)
									rangeStr += ";";
								rangeStr += std::format("{}@L{}", entry.count, entry.level);
							}
							categoryEntries.push_back(std::format("{}: [{}]", name, rangeStr));
						}
					}

					for (const auto& [form, registeredItem] : DialogueGlobalRegistry::g_cachedItemCategoryMap) {
						if (registeredItem.categoryID == catID && form) {
							itemCount++;
							if (spdlog::should_log(spdlog::level::trace)) {
								const char* edid = form->GetFormEditorID();
								auto        debugIt = DialogueGlobalRegistry::g_debugItemEditorIDs.find(form);
								std::string itemName = (debugIt != DialogueGlobalRegistry::g_debugItemEditorIDs.end()) ? debugIt->second :
								                       (edid && edid[0] != '\0')                                       ? edid :
								                                                                                         std::format("0x{:08X}", form->GetFormID());

								std::int32_t currentValue = registeredItem.valuePtr ? *registeredItem.valuePtr : registeredItem.originalValue;

								SKSE::log::trace("[CCMD] Cat: {} | Item: {} | Val: {} | OgVal: {} | FormID: 0x{:08X}",
									catID, itemName, currentValue, registeredItem.originalValue, form->GetFormID());
							}
						}
					}

					if (itemCount > 0) {
						categoryEntries.push_back(std::format("ITEM entries: {}", itemCount));
					}

					if (categoryEntries.empty()) {
						PrintOutput("  (No registered entries)");
						continue;
					}

					std::string currentLine;
					int         lineCount = 0;
					for (size_t k = 0; k < categoryEntries.size(); ++k) {
						if (lineCount > 0)
							currentLine += ", ";
						currentLine += categoryEntries[k];
						lineCount++;

						if (lineCount == 6 || k == categoryEntries.size() - 1) {
							PrintOutput(currentLine);
							currentLine.clear();
							lineCount = 0;
						}
					}
				}
				result = 1.0;
			} else if (action == "loadinipreset") {
				if (MCPddcMenu::IsDialogueMenuOpen()) {
					WriteConsoleMessage("DynamicDialogueCosts: Cannot load INI preset while in Dialogue!");
					result = 0.0;
					return true;
				}

				std::string targetPresetID;
				if (!(iss >> targetPresetID) || targetPresetID.empty()) {
					WriteConsoleMessage("Usage: ddc loadinipreset-<name> or ddc \"loadinipreset <name>\"");
					auto presets = DialogueGlobalRegistry::GetCachedPresets();
					if (presets.empty()) {
						WriteConsoleMessage("  Available INI Presets: none");
					} else {
						WriteConsoleMessage("  Available INI Presets [<filename>/<displayname>]:");
						for (const auto& preset : presets) {
							if (!preset.displayName.empty() && preset.displayName != preset.filename) {
								WriteConsoleMessage(std::format("    [{}/{}]", preset.filename, preset.displayName));
							} else {
								WriteConsoleMessage(std::format("   [{}/-]", preset.filename));
							}
						}
					}
					result = 1.0;
					return true;
				}

				std::string lowerTarget = targetPresetID;
				std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

				std::string matchedFilename;
				std::string matchedDisplayName;

				auto presets = DialogueGlobalRegistry::GetCachedPresets();
				for (const auto& preset : presets) {
					std::string fnLower = preset.filename;
					std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					std::string fnNoExt = fnLower;
					if (fnNoExt.ends_with(".ini")) {
						fnNoExt.resize(fnNoExt.length() - 4);
					}

					std::string dnLower = preset.displayName;
					std::transform(dnLower.begin(), dnLower.end(), dnLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

					if (fnLower == lowerTarget || fnNoExt == lowerTarget || dnLower == lowerTarget) {
						matchedFilename = preset.filename;
						matchedDisplayName = preset.displayName;
						break;
					}
				}

				if (matchedFilename.empty()) {
					WriteConsoleMessage(std::format("DynamicDialogueCosts: INI Profile '{}' not found.", targetPresetID));
					result = 0.0;
					return true;
				}
				DialoguePriceTransformer::ResetPersistentToBaseline();
				MCPddcMenu::SetSelectedPresetByFilename(matchedFilename);
				MCPddcMenu::RestoreFromIni(matchedFilename, 0, true);

				MCPddcMenu::SanitizeSettings(MCPddcMenu::s_menuSettings);
				if (MCPddcMenu::s_menuSettings.hideCategories) {
					MCPddcMenu::SetCategoryHiding(true, false);
				}

				g_settings.store(std::make_shared<Settings>(MCPddcMenu::s_menuSettings));
				SaveSettings(MCPddcMenu::s_menuSettings);
				DialogueGlobalRegistry::LoadGlobalPatches();
				RebuildCategoryRegistry();
				DialoguePriceTransformer::g_itemBucketsInitialized.store(false);
				DialoguePriceTransformer::g_leveledBucketsInitialized.store(false);
				DialoguePriceTransformer::g_globalBucketsInitialized.store(false);
				MCPddcMenu::InvalidateCategoryCache();

				CheckAndApplyPersistentPatches(true);
				auto cfg = g_settings.load();
				if (cfg && cfg->useQuestWatcher) {
					QuestOrderManager::EvaluateAndApplyProfile("$FULL");
				}
				MCPddcMenu::s_isDirty = false;

				DialoguePriceTransformer::PokeQuestsRefresh();
				WriteConsoleMessage(std::format("DynamicDialogueCosts: Loaded INI Profile '{}' ({}) and applied settings.", matchedDisplayName, matchedFilename));
				result = 1.0;
			} else if (action == "loadqcsprofile") {
				if (MCPddcMenu::IsDialogueMenuOpen()) {
					WriteConsoleMessage("DynamicDialogueCosts: Cannot load QCS profile while in Dialogue!");
					result = 0.0;
					return true;
				}
				auto        loadedQcsProfiles = QuestOrderManager::GetLoadedProfiles();
				std::string targetQcsID;
				if (!(iss >> targetQcsID) || targetQcsID.empty()) {
					WriteConsoleMessage("Usage: ddc loadqcsprofile-<id> or ddc \"loadqcsprofile <id>\"");

					if (loadedQcsProfiles.empty()) {
						WriteConsoleMessage("  Available QCS Profiles: none");
					} else {
						WriteConsoleMessage("  Available QCS Profiles [<id>/name]:");
						for (const auto& profile : loadedQcsProfiles) {
							if (!profile.profileName.empty() && profile.profileName != profile.profileID) {
								WriteConsoleMessage(std::format("    [{}/{}]", profile.profileID, profile.profileName));
							} else {
								WriteConsoleMessage(std::format("   [{}/{}]", profile.profileID, profile.filename));
							}
						}
					}
					result = 1.0;
					return true;
				}

				std::string lowerTarget = targetQcsID;
				std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

				std::string matchedEffectiveID;
				std::string matchedDisplayName;

				for (const auto& profile : loadedQcsProfiles) {
					std::string fnLower = profile.filename;
					std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

					std::string fnNoExt = fnLower;
					if (auto dotPos = fnNoExt.rfind('.'); dotPos != std::string::npos) {
						fnNoExt.resize(dotPos);
					}

					std::string idLower = profile.profileID;
					std::transform(idLower.begin(), idLower.end(), idLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

					std::string nameLower = profile.profileName;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (idLower == lowerTarget || fnLower == lowerTarget || fnNoExt == lowerTarget || nameLower == lowerTarget) {
						matchedEffectiveID = !profile.profileID.empty() ? profile.profileID : profile.filename;
						matchedDisplayName = profile.profileName.empty() ? profile.filename : profile.profileName;
						break;
					}
				}

				if (matchedEffectiveID.empty()) {
					WriteConsoleMessage(std::format("DynamicDialogueCosts: QCS Profile '{}' not found.", targetQcsID));
					result = 0.0;
					return true;
				}

				MCPddcMenu::s_menuSettings.questOrderProfile = matchedEffectiveID;
				MCPddcMenu::s_menuSettings.useQuestWatcher = true;

				MCPddcMenu::SanitizeSettings(MCPddcMenu::s_menuSettings);
				g_settings.store(std::make_shared<Settings>(MCPddcMenu::s_menuSettings));
				SaveSettings(MCPddcMenu::s_menuSettings);
				QuestOrderManager::EvaluateAndApplyProfile("$FULL");
				MCPddcMenu::s_isDirty = false;
				MCPddcMenu::ResetUIBufferFromMemory();

				DialoguePriceTransformer::PokeQuestsRefresh();
				WriteConsoleMessage(std::format("DynamicDialogueCosts: Loaded QCS Profile '{}' ({}) and applied.", matchedDisplayName, matchedEffectiveID));
				result = 1.0;
			} else if (action == "unlock") {
				MCPddcMenu::SetCategoryHiding(false, true);

				if (auto cfg = g_settings.load()) {
					auto updated = std::make_shared<Settings>(*cfg);
					updated->hideCategories = false;
					g_settings.store(updated);
				}

				WriteConsoleMessage("DynamicDialogueCosts: Unlocked Menu.");
				result = 1.0;
			} else if (action == "debug") {
				auto cfg = g_settings.load();
				if (!cfg) {
					WriteConsoleMessage("DynamicDialogueCosts: Failed to retrieve current settings!");
					SKSE::log::error("[CCMD] Failed to retrieve current settings: debug.");
					result = 0.0;
					return true;
				}

				std::string param;
				if (iss >> param) {
					std::transform(param.begin(), param.end(), param.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

					auto updated = std::make_shared<Settings>(*cfg);

					if (param == "on") {
						updated->enableDebug = true;
						g_settings.store(updated);
						WriteConsoleMessage("DynamicDialogueCosts: Debug mode ENABLED.");
					} else if (param == "off") {
						updated->enableDebug = false;
						g_settings.store(updated);
						WriteConsoleMessage("DynamicDialogueCosts: Debug mode DISABLED.");
					} else {
						try {
							int level = std::stoi(param);
							if (level >= 0 && level <= 5) {
								updated->debugLevel = level;
								g_settings.store(updated);
								WriteConsoleMessage(std::format("DynamicDialogueCosts: debug level set to {}.", level));
							} else {
								WriteConsoleMessage("DynamicDialogueCosts: Invalid debugmode param. Use: ddc debug [on | off | 0-5]");
								result = 0.0;
								return true;
							}
						} catch (...) {
							WriteConsoleMessage("DynamicDialogueCosts: Invalid debug param. Use: ddc debug [on | off | 0-5]");
							result = 0.0;
							return true;
						}
					}
					if (auto activeCfg = g_settings.load()) {
						int activeLevel = activeCfg->enableDebug ? activeCfg->debugLevel : 2;
						SetLogLevel(activeLevel);
					}
					result = 1.0;
				} else {
					WriteConsoleMessage(std::format("DynamicDialogueCosts: Debug State: {} | Level: {}",
						cfg->enableDebug ? "ENABLED" : "DISABLED", cfg->debugLevel));
					WriteConsoleMessage("Usage: ddc debug [on | off | 0-5]");
					result = 1.0;
				}
			} else if (action.empty()) {
				WriteConsoleMessage("DynamicDialogueCosts: [NOTICE] No subcommand specified.");
				WriteConsoleMessage("Usage: ddc [hardreset | reloadcats | savetoini | scancats | loadinipreset | loadqcsprofile | unlock | debug]");
			} else {
				WriteConsoleMessage(std::format("DynamicDialogueCosts: [NOTICE] Unknown subcommand '{}'.", action));
				WriteConsoleMessage("Valid subcommands: hardreset, reloadcats, savetoini, scancats, loadinipreset, loadqcsprofile, unlock, debug [on | off | 0-5]");
			}

			return true;
		}

		static void Register()
		{
			static RE::SCRIPT_PARAMETER kCommandParam[] = {
				{ "Arguments", RE::SCRIPT_PARAM_TYPE::kChar, 1 }
			};

			static constexpr std::array kSacrificialCommands = {
				"StartAllQuests",
				"CompleteAllQuestStages",
				"ToggleFogOfWar",
				"ToggleWireframe",
				"ToggleWaterSystem",
				"ToggleSky",
				"ToggleTrees",
				"CenterOnWorld"
			};

			for (const char* sacrificialName : kSacrificialCommands) {
				if (auto cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(sacrificialName)) {
					cmd->functionName = "dyndg-costs";
					cmd->shortName = "ddc";
					cmd->helpString = "DynamicDialogueCosts Command. Syntax: [ddc | dyndg-costs] [hardreset | reloadcats | savetoini | scancats | loadinipreset | loadqcsprofile | unlock | debug]";
					cmd->referenceFunction = false;

					cmd->executeFunction = &DispatchCommand;
					cmd->numParams = 1;
					cmd->params = kCommandParam;
					return;
				}
			}
		}
	}

	namespace
	{
		class StateDrivenItemWatcher :
		    public RE::BSTEventSink<RE::TESSleepStopEvent>,
		    public RE::BSTEventSink<RE::TESWaitStopEvent>,
		    public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
		{
		public:
			static StateDrivenItemWatcher* GetSingleton()
			{
				static StateDrivenItemWatcher singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
			{
				SKSE::log::trace("[Watcher] Item patches triggered by sleep event.");
				SKSE::GetTaskInterface()->AddTask([]() { CheckAndApplyPersistentPatches(true); });
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESWaitStopEvent*, RE::BSTEventSource<RE::TESWaitStopEvent>*) override
			{
				SKSE::log::trace("[Watcher] Item patches triggered by wait event.");
				SKSE::GetTaskInterface()->AddTask([]() { CheckAndApplyPersistentPatches(true); });
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* event, RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
			{
				if (event && event->actor && event->actor->IsPlayerRef()) {
					SKSE::log::trace("[Watcher] Item patches triggered by location change event.");
					SKSE::GetTaskInterface()->AddTask([]() { CheckAndApplyPersistentPatches(false); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class ActorActivationWatcher : public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			static ActorActivationWatcher* GetSingleton()
			{
				static ActorActivationWatcher singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (!event) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto cfg = g_settings.load();
				if (!cfg || !cfg->scaleCatValues || !cfg->useActivateHandler) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto actionRef = event->actionRef.get();
				auto targetRef = event->objectActivated.get();

				if (actionRef && actionRef->IsPlayerRef() && targetRef) {
					if (auto actor = targetRef->As<RE::Actor>()) {
						if (!CanActorTalkToPlayer(actor)) {
							return RE::BSEventNotifyControl::kContinue;
						}

						if (g_isDialogueScaled.load()) {
							DialoguePriceTransformer::ResetGlobalsToBaseline();
						}

						g_targetSpeakerHandle = targetRef->CreateRefHandle();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class QuestStageWatcher : public RE::BSTEventSink<RE::TESQuestStageEvent>
		{
		private:
			static inline std::atomic<bool>               s_evaluationPending{ false };
			static inline std::unordered_set<std::string> s_deferredQuestEdids;
			static inline std::mutex                      s_deferredMutex;

		public:
			static void RegisterDeferredQuest(const std::string& edid)
			{
				std::lock_guard lock(s_deferredMutex);
				s_deferredQuestEdids.insert(edid);
			}

			static std::unordered_set<std::string> PopDeferredQuests()
			{
				std::lock_guard                 lock(s_deferredMutex);
				std::unordered_set<std::string> deferred = std::move(s_deferredQuestEdids);
				s_deferredQuestEdids.clear();
				return deferred;
			}

			static QuestStageWatcher* GetSingleton()
			{
				static QuestStageWatcher singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
			{
				if (!event) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto cfg = g_settings.load();
				if (!cfg || !cfg->useQuestWatcher) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (!QuestOrderManager::IsQuestWatched(event->formID)) {
					return RE::BSEventNotifyControl::kContinue;
				}

				std::string   questEdid = "UNKNOWN";
				RE::TESQuest* quest = RE::TESForm::LookupByID<RE::TESQuest>(event->formID);
				if (quest) {
					const char* edid = quest->GetFormEditorID();
					if (edid && edid[0] != '\0') {
						questEdid = edid;
					}
				}

				if (questEdid != "UNKNOWN") {
					std::string lowerEdid = questEdid;
					std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);

					auto it = g_trackedQuests.find(lowerEdid);
					if (it != g_trackedQuests.end()) {
						for (const auto& rule : it->second) {
							bool stageMatches = (rule.completionStage < 0) || (event->stage == static_cast<std::uint16_t>(rule.completionStage));
							if (stageMatches) {
								std::string hitKey = lowerEdid + ";" + std::to_string(rule.completionStage);
								g_trackedQuestStageHits[hitKey]++;
								SKSE::log::debug("[QWES] Tracked quest '{}' hit target stage {} (Hits: {}/{})",
									questEdid, event->stage, g_trackedQuestStageHits[hitKey], rule.requiredHits);

								if (g_trackedQuestStageHits[hitKey] >= rule.requiredHits) {
									g_trackedQuestStageHits[hitKey] = 0;
									QuestOrderManager::IncrementQuestCompletion(questEdid, rule.completionStage);
								}
							}
						}
					} else if (quest && quest->IsCompleted()) {
						QuestOrderManager::IncrementQuestCompletion(questEdid);
					}
				}

				QuestOrderManager::InvalidateConditionCacheForQuest(questEdid);
				SKSE::log::debug("[QWES] Event triggered by tracked quest: {} (FormID: 0x{:08X}, Stage: {})",
					questEdid, event->formID, event->stage);

				auto ui = RE::UI::GetSingleton();
				bool isMenuBusy = ui && (ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen(RE::BarterMenu::MENU_NAME));

				if (isMenuBusy) {
					SKSE::log::debug("[QWES] Tracked Quest triggered during Dialogue/Barter Menu! Deferring check for: {}", questEdid);
					RegisterDeferredQuest(questEdid);
					g_questWatcherSOS.store(true);
				} else if (!s_evaluationPending.exchange(true)) {
					SKSE::GetTaskInterface()->AddTask([questEdid, stage = event->stage]() {
						s_evaluationPending.store(false);
						SKSE::log::debug("[QWES] Evaluating profile due to stage change on quest {} (Stage {})", questEdid, stage);
						QuestOrderManager::EvaluateAndApplyProfile(questEdid);
						MCPddcMenu::ResetUIBufferFromMemory();
					});
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	static void ProcessDeferredQuestUpdate()
	{
		SKSE::GetTaskInterface()->AddTask([]() {
			auto pendingEdids = QuestStageWatcher::PopDeferredQuests();
			if (pendingEdids.empty()) {
				SKSE::log::debug("[QWES] Evaluating delayed QCS check (No EDID?)");
				QuestOrderManager::EvaluateAndApplyProfile("$FULL");
			} else {
				for (const auto& edid : pendingEdids) {
					SKSE::log::debug("[QWES] Evaluating delayed QCS check for quest: {}", edid);
					QuestOrderManager::EvaluateAndApplyProfile(edid);
				}
			}
			MCPddcMenu::ResetUIBufferFromMemory();
		});
	}

	class DialogueMenuStateMonitor : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static DialogueMenuStateMonitor* GetSingleton()
		{
			static DialogueMenuStateMonitor singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			if (event->menuName == RE::DialogueMenu::MENU_NAME) {
				if (!event->opening) {
					SKSE::GetTaskInterface()->AddTask([]() {
						DialoguePriceTransformer::ResetGlobalsToBaseline();
					});
					if (g_questWatcherSOS.exchange(false)) {
						SKSE::log::info("[DMES] Processing deferred QWES call at dialogue's exit.");
						ProcessDeferredQuestUpdate();
					}
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	static void InitializeDataLoaded()
	{
		ConsoleCommands::AuditConsoleCommandHooks(true);
		ConsoleCommands::Register();
		ConsoleCommands::AuditConsoleCommandHooks(false);

		MCPddcMenu::Register();
		g_ecoCache.Refresh();
		DialogueGlobalRegistry::LoadGlobalPatches();

		DialoguePriceTransformer::g_itemBucketsInitialized.store(false);
		DialoguePriceTransformer::g_globalBucketsInitialized.store(false);
		DialoguePriceTransformer::g_leveledBucketsInitialized.store(false);

		if (auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink(DialogueMenuStateMonitor::GetSingleton());
			SKSE::log::info("[INIT] Registered DialogueMenuStateMonitor Event Sink.");
		} else {
			SKSE::log::error("[INIT] Failed to register DialogueMenuStateMonitor Event Sink!");
		}

		if (auto scriptHolder = RE::ScriptEventSourceHolder::GetSingleton()) {
			scriptHolder->AddEventSink<RE::TESQuestStageEvent>(QuestStageWatcher::GetSingleton());
			SKSE::log::info("[INIT] Registered QuestStageWatcher Event Sink.");

			scriptHolder->AddEventSink<RE::TESSleepStopEvent>(StateDrivenItemWatcher::GetSingleton());
			scriptHolder->AddEventSink<RE::TESWaitStopEvent>(StateDrivenItemWatcher::GetSingleton());
			scriptHolder->AddEventSink<RE::TESActorLocationChangeEvent>(StateDrivenItemWatcher::GetSingleton());
			SKSE::log::info("[INIT] Registered StateDrivenItemWatcher Event Sinks.");
		} else {
			SKSE::log::error("[INIT] Failed to register StateDrivenItemWatcher Event Sinks.");
		}

		auto cfg = g_settings.load();
		if (cfg && cfg->useActivateHandler) {
			if (auto scriptHolder = RE::ScriptEventSourceHolder::GetSingleton()) {
				scriptHolder->AddEventSink<RE::TESActivateEvent>(ActorActivationWatcher::GetSingleton());
				SKSE::log::info("[INIT] Registered ActorActivationWatcher Event Sink.");
			} else {
				SKSE::log::error("[INIT] Failed to register ActorActivationWatcher Event Sink.");
			}
		}
		ReadPluginConfig();
	}

	static void ResetSessionState(bool isNewGame)
	{
		DialoguePriceTransformer::ResetGlobalsToBaseline();

		RebuildCategoryRegistry();
		DialoguePriceTransformer::g_itemBucketsInitialized.store(false);
		DialoguePriceTransformer::g_leveledBucketsInitialized.store(false);
		DialoguePriceTransformer::g_globalBucketsInitialized.store(false);
		MCPddcMenu::InvalidateCategoryCache();

		g_lastPatchedDay = -1.0f;
		g_lastPatchedLevel = 0;

		g_isDialogueScaled.store(false);
		g_targetSpeakerHandle.reset();

		if (isNewGame) {
			g_activeBuyRate.store(1.0f);
			g_activeSellRate.store(1.0f);
			ReadPluginConfig();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
	SKSE::Init(skse);
	SetupLog();
	DynamicDialogueCosts::LoadMenuTexts();
	DynamicDialogueCosts::Serialization::Register();
	DynamicDialogueCosts::Hooks::Install();

	SKSE::GetMessagingInterface()->RegisterListener("SKSE", [](SKSE::MessagingInterface::Message* msg) {
		switch (msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			{
				BENCHMARK_SCOPE("kDataLoaded");
				DynamicDialogueCosts::InitializeDataLoaded();
				break;
			}
		case SKSE::MessagingInterface::kSaveGame:
			{
				SKSE::log::trace("[SAVE] Running kSaveGame");
				BENCHMARK_SCOPE_IF("kSaveGame: Scaling removal", spdlog::should_log(spdlog::level::info));
				bool hadActiveScaling = DynamicDialogueCosts::g_isDialogueScaled.load();
				auto savedSpeaker = DynamicDialogueCosts::g_targetSpeakerHandle;

				float savedBuyRate = DynamicDialogueCosts::g_activeBuyRate.load();
				float savedSellRate = DynamicDialogueCosts::g_activeSellRate.load();

				DynamicDialogueCosts::DialoguePriceTransformer::ResetPersistentToBaseline();
				DynamicDialogueCosts::DialoguePriceTransformer::ResetGlobalsToBaseline();
				DynamicDialogueCosts::DialoguePriceTransformer::PokeQuestsRefresh();

				SKSE::GetTaskInterface()->AddTask([savedSpeaker, savedBuyRate, savedSellRate, hadActiveScaling]() {
					BENCHMARK_SCOPE_IF("kSaveGame: Scaling recovery", spdlog::should_log(spdlog::level::info));
					DynamicDialogueCosts::g_targetSpeakerHandle = savedSpeaker;
					DynamicDialogueCosts::g_activeBuyRate.store(savedBuyRate);
					DynamicDialogueCosts::g_activeSellRate.store(savedSellRate);
					DynamicDialogueCosts::CheckAndApplyPersistentPatches(false);
					if (hadActiveScaling) {
						SKSE::log::error("[SAVE] Unexpected: Dialogue Scaling Step flag was on when saving.");
					}
				});
				break;
			}
		case SKSE::MessagingInterface::kPostLoadGame:
			{
				BENCHMARK_SCOPE("kPostLoadGame");
				DynamicDialogueCosts::ResetSessionState(false);
				SKSE::log::trace("[LOAD] Running kPostLoadGame");
				DynamicDialogueCosts::CheckAndApplyPersistentPatches(true);
				auto cfg = DynamicDialogueCosts::g_settings.load();
				if (cfg && cfg->useQuestWatcher) {
					DynamicDialogueCosts::QuestOrderManager::EvaluateAndApplyProfile("$FULL");
				}
				break;
			}
		case SKSE::MessagingInterface::kNewGame:
			{
				BENCHMARK_SCOPE("kNewGame");
				DynamicDialogueCosts::ResetSessionState(true);
				SKSE::log::trace("[NEWG] Running kNewGame");

				DynamicDialogueCosts::CheckAndApplyPersistentPatches(true);
				auto cfg = DynamicDialogueCosts::g_settings.load();
				if (cfg && cfg->useQuestWatcher) {
					DynamicDialogueCosts::QuestOrderManager::EvaluateAndApplyProfile("$FULL");
				}
				break;
			}
		default:
			break;
		}
	});

	return true;
}