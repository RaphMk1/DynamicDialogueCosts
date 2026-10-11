#include "Serialization.h"
#include "MCPddcMenu.h"
#include "PCH.h"
#include "QOManager.h"
#include "log.h"
#include "settings.h"

namespace DynamicDialogueCosts::Serialization
{
	void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_str)
	{
		std::uint32_t len = static_cast<std::uint32_t>(a_str.size());
		a_intfc->WriteRecordData(len);
		if (len > 0) {
			a_intfc->WriteRecordData(a_str.data(), len);
		}
	}

	std::string ReadString(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t len = 0;
		a_intfc->ReadRecordData(len);
		if (len == 0)
			return "";
		std::string str(len, '\0');
		a_intfc->ReadRecordData(str.data(), len);
		return str;
	}

	void SaveCallback(SKSE::SerializationInterface* a_intfc)
	{
		BENCHMARK_SCOPE("Cosave_Save");
		auto cfg = g_settings.load();
		if (!cfg)
			return;

		if (a_intfc->OpenRecord(kSettingsRecord, kCurrentVersion)) {
			a_intfc->WriteRecordData(cfg->scaleCatValues);
			a_intfc->WriteRecordData(cfg->enableDynamicScaling);
			a_intfc->WriteRecordData(cfg->includePowerModifiers);
			a_intfc->WriteRecordData(cfg->includePerkModifiers);
			a_intfc->WriteRecordData(cfg->enableInflation);
			a_intfc->WriteRecordData(cfg->maxInflationCap);
			a_intfc->WriteRecordData(cfg->enableLevelScaling);
			a_intfc->WriteRecordData(cfg->maxMultCap);
			a_intfc->WriteRecordData(cfg->hideCategories);
			a_intfc->WriteRecordData(cfg->useQuestWatcher);
			a_intfc->WriteRecordData(cfg->enableDebug);
			a_intfc->WriteRecordData(cfg->useActivateHandler);

			WriteString(a_intfc, cfg->questOrderProfile);

			std::uint32_t catCount = static_cast<std::uint32_t>(cfg->categories.size());
			a_intfc->WriteRecordData(catCount);

			for (const auto& [catKey, cat] : cfg->categories) {
				WriteString(a_intfc, catKey);
				WriteString(a_intfc, cat.categoryID);
				WriteString(a_intfc, cat.displayName);

				a_intfc->WriteRecordData(cat.persistentLeveledLists);
				a_intfc->WriteRecordData(cat.usesSellBarterMult);
				a_intfc->WriteRecordData(cat.hideCatInMenu);
				a_intfc->WriteRecordData(cat.enabled);
				a_intfc->WriteRecordData(cat.multiplier);
				a_intfc->WriteRecordData(cat.dynamicScaling);
				a_intfc->WriteRecordData(cat.enableInflation);
				a_intfc->WriteRecordData(cat.invertInflation);
				a_intfc->WriteRecordData(cat.inflationRate);
				a_intfc->WriteRecordData(cat.inflationResetDay);
				a_intfc->WriteRecordData(cat.levelScale);
				a_intfc->WriteRecordData(cat.invertLevelMult);
				a_intfc->WriteRecordData(cat.levelMult);
			}
		} else {
			SKSE::log::error("[COSV] Failed to open settings record for writing.");
			return;
		}

		if (a_intfc->OpenRecord(kActiveRatesRecord, kCurrentVersion)) {
			float buyRate = g_activeBuyRate.load();
			float sellRate = g_activeSellRate.load();
			bool  isScaled = g_isDialogueScaled.load();

			a_intfc->WriteRecordData(buyRate);
			a_intfc->WriteRecordData(sellRate);
			a_intfc->WriteRecordData(isScaled);

			SKSE::log::debug("[COSV] Saved active rates - Buy: {}, Sell: {}, Scaled: {}", buyRate, sellRate, isScaled);
		}

		if (a_intfc->OpenRecord(kQuestCountsRecord, kCurrentVersion)) {
			std::uint32_t count = static_cast<std::uint32_t>(g_questCompletionCounts.size());
			a_intfc->WriteRecordData(count);
			for (const auto& [edid, num] : g_questCompletionCounts) {
				WriteString(a_intfc, edid);
				a_intfc->WriteRecordData(num);
			}

			std::uint32_t progressCount = static_cast<std::uint32_t>(g_trackedQuestStageHits.size());
			a_intfc->WriteRecordData(progressCount);
			for (const auto& [edid, num] : g_trackedQuestStageHits) {
				WriteString(a_intfc, edid);
				a_intfc->WriteRecordData(num);
			}

			SKSE::log::trace("[COSV] Saved {} quest completion records and {} tracked stage hit records.", count, progressCount);
		}

		if (a_intfc->OpenRecord(kConditionCacheRecord, kCurrentVersion)) {
			std::shared_lock lock(g_conditionCacheMutex);
			std::uint32_t    count = static_cast<std::uint32_t>(g_conditionTruthCache.size());
			a_intfc->WriteRecordData(count);

			for (const auto& [cacheKey, state] : g_conditionTruthCache) {
				WriteString(a_intfc, cacheKey);
				a_intfc->WriteRecordData(state);
			}
			SKSE::log::debug("[COSV] Saved {} condition cache entries to cosave.", count);
		}

		SKSE::log::debug("[COSV] Successfully saved configuration to SKSE cosave.");
	}

	void LoadCallback(SKSE::SerializationInterface* a_intfc)
	{
		BENCHMARK_SCOPE("Cosave_Load");
		std::uint32_t type;
		std::uint32_t version;
		std::uint32_t length;

		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == kQuestCountsRecord) {
				std::uint32_t count = 0;
				a_intfc->ReadRecordData(count);
				g_questCompletionCounts.clear();
				for (std::uint32_t i = 0; i < count; ++i) {
					std::string   edid = ReadString(a_intfc);
					std::uint32_t num = 0;
					a_intfc->ReadRecordData(num);
					g_questCompletionCounts[edid] = num;
				}

				g_trackedQuestStageHits.clear();
				std::uint32_t progressCount = 0;
				if (a_intfc->ReadRecordData(progressCount)) {
					for (std::uint32_t i = 0; i < progressCount; ++i) {
						std::string   edid = ReadString(a_intfc);
						std::uint32_t num = 0;
						a_intfc->ReadRecordData(num);
						g_trackedQuestStageHits[edid] = num;
					}
				}
				SKSE::log::info("[COSV] Loaded {} quest completion counts and {} tracked stage hit counts from cosave.", count, progressCount);
			} else if (type == kConditionCacheRecord) {
				std::uint32_t count = 0;
				a_intfc->ReadRecordData(count);

				std::unique_lock lock(g_conditionCacheMutex);
				g_conditionTruthCache.clear();

				for (std::uint32_t i = 0; i < count; ++i) {
					std::string cacheKey = ReadString(a_intfc);
					bool        state = false;
					a_intfc->ReadRecordData(state);
					g_conditionTruthCache[cacheKey] = state;
				}
				SKSE::log::info("[COSV] Loaded {} condition cache entries from cosave.", count);
			} else if (type == kSettingsRecord) {
				if (version > kCurrentVersion) {
					SKSE::log::warn("[COSV] Record version ({}) exceeds current supported version ({}).", version, kCurrentVersion);
					continue;
				}

				auto currentIniSettings = g_settings.load();
				auto loadedSettings = currentIniSettings ? std::make_shared<Settings>(*currentIniSettings) : std::make_shared<Settings>();

				switch (version) {
				case 1:
					{
						a_intfc->ReadRecordData(loadedSettings->scaleCatValues);
						a_intfc->ReadRecordData(loadedSettings->enableDynamicScaling);
						a_intfc->ReadRecordData(loadedSettings->includePowerModifiers);
						a_intfc->ReadRecordData(loadedSettings->includePerkModifiers);
						a_intfc->ReadRecordData(loadedSettings->enableInflation);
						a_intfc->ReadRecordData(loadedSettings->maxInflationCap);
						a_intfc->ReadRecordData(loadedSettings->enableLevelScaling);
						a_intfc->ReadRecordData(loadedSettings->maxMultCap);
						a_intfc->ReadRecordData(loadedSettings->hideCategories);
						a_intfc->ReadRecordData(loadedSettings->useQuestWatcher);
						a_intfc->ReadRecordData(loadedSettings->enableDebug);
						a_intfc->ReadRecordData(loadedSettings->useActivateHandler);

						loadedSettings->questOrderProfile = ReadString(a_intfc);

						std::uint32_t catCount = 0;
						a_intfc->ReadRecordData(catCount);

						std::unordered_map<std::string, CategorySettings> savedCategories;
						for (std::uint32_t i = 0; i < catCount; ++i) {
							std::string      catKey = ReadString(a_intfc);
							CategorySettings cat;
							cat.categoryID = ReadString(a_intfc);
							cat.displayName = ReadString(a_intfc);

							a_intfc->ReadRecordData(cat.persistentLeveledLists);
							a_intfc->ReadRecordData(cat.usesSellBarterMult);
							a_intfc->ReadRecordData(cat.hideCatInMenu);
							a_intfc->ReadRecordData(cat.enabled);
							a_intfc->ReadRecordData(cat.multiplier);
							a_intfc->ReadRecordData(cat.dynamicScaling);
							a_intfc->ReadRecordData(cat.enableInflation);
							a_intfc->ReadRecordData(cat.invertInflation);
							a_intfc->ReadRecordData(cat.inflationRate);
							a_intfc->ReadRecordData(cat.inflationResetDay);
							a_intfc->ReadRecordData(cat.levelScale);
							a_intfc->ReadRecordData(cat.invertLevelMult);
							a_intfc->ReadRecordData(cat.levelMult);

							savedCategories[catKey] = cat;
						}

						if (currentIniSettings) {
							for (auto& [catKey, iniCat] : loadedSettings->categories) {
								auto savedIt = savedCategories.find(catKey);
								if (savedIt != savedCategories.end()) {
									const auto& savedCat = savedIt->second;
									iniCat.categoryID = savedCat.categoryID;
									iniCat.displayName = savedCat.displayName;
									iniCat.persistentLeveledLists = savedCat.persistentLeveledLists;
									iniCat.usesSellBarterMult = savedCat.usesSellBarterMult;
									iniCat.hideCatInMenu = savedCat.hideCatInMenu;
									iniCat.enabled = savedCat.enabled;
									iniCat.multiplier = savedCat.multiplier;
									iniCat.dynamicScaling = savedCat.dynamicScaling;
									iniCat.enableInflation = savedCat.enableInflation;
									iniCat.invertInflation = savedCat.invertInflation;
									iniCat.inflationRate = savedCat.inflationRate;
									iniCat.inflationResetDay = savedCat.inflationResetDay;
									iniCat.levelScale = savedCat.levelScale;
									iniCat.invertLevelMult = savedCat.invertLevelMult;
									iniCat.levelMult = savedCat.levelMult;
								}
							}

							for (auto& [savedKey, savedCat] : savedCategories) {
								if (!loadedSettings->categories.contains(savedKey)) {
									loadedSettings->categories[savedKey] = savedCat;
								}
							}
						} else {
							loadedSettings->categories = std::move(savedCategories);
						}

						g_settings.store(loadedSettings);
						SKSE::log::info("[COSV] Successfully restored settings: Version {}.", version);
					}
					break;

				default:
					SKSE::log::warn("[COSV] Unknown serialization version: {}", version);
					break;
				}
			} else if (type == kActiveRatesRecord) {
				float buyRate = 1.0f;
				float sellRate = 1.0f;
				bool  isScaled = false;

				a_intfc->ReadRecordData(buyRate);
				a_intfc->ReadRecordData(sellRate);
				a_intfc->ReadRecordData(isScaled);

				g_activeBuyRate.store(buyRate);
				g_activeSellRate.store(sellRate);
				g_isDialogueScaled.store(isScaled);

				SKSE::log::info("[COSV] Loaded active rates - Buy: {}, Sell: {}, Scaled: {}", buyRate, sellRate, isScaled);
			}
		}
	}

	void ResetState()
	{
		ReadPluginConfig();
		MCPddcMenu::InvalidateCategoryCache();
		auto cfg = g_settings.load();
		if (cfg && cfg->questOrderProfile.empty()) {
			auto profiles = QuestOrderManager::GetLoadedProfiles();
			if (!profiles.empty()) {
				auto mutableCfg = std::make_shared<Settings>(*cfg);
				mutableCfg->questOrderProfile = !profiles[0].profileID.empty() ? profiles[0].profileID : profiles[0].filename;
				g_settings.store(mutableCfg);
			}
		}
	}

	void RevertCallback(SKSE::SerializationInterface* /*itf*/)
	{
		g_questCompletionCounts.clear();
		g_trackedQuestStageHits.clear();
		{
			std::unique_lock lock(g_conditionCacheMutex);
			g_conditionTruthCache.clear();
		}

		g_activeBuyRate.store(1.0f);
		g_activeSellRate.store(1.0f);
		g_isDialogueScaled.store(false);

		ResetState();
		SKSE::log::debug("[COSV] RevertCallback executed - state reset to defaults.");
	}

	void Register()
	{
		auto serialization = SKSE::GetSerializationInterface();
		serialization->SetUniqueID(kSerializationID);
		serialization->SetSaveCallback(SaveCallback);
		serialization->SetLoadCallback(LoadCallback);
		serialization->SetRevertCallback(RevertCallback);
	}
}