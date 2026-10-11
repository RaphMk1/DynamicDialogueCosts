#include "QOManager.h"
#include "MCPddcMenu.h"
#include "PCH.h"
#include "dialogueRegistry.h"
#include "settings.h"

#include <SimpleIni.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>

namespace DynamicDialogueCosts
{
	static bool AreCategorySettingsEqual(const CategorySettings& a, const CategorySettings& b)
	{
		return a.categoryID == b.categoryID &&
		       a.priority == b.priority &&
		       a.displayName == b.displayName &&
		       a.persistentLeveledLists == b.persistentLeveledLists &&
		       a.usesSellBarterMult == b.usesSellBarterMult &&
		       a.hideCatInMenu == b.hideCatInMenu &&
		       a.enabled == b.enabled &&
		       a.multiplier == b.multiplier &&
		       a.dynamicScaling == b.dynamicScaling &&
		       a.enableInflation == b.enableInflation &&
		       a.invertInflation == b.invertInflation &&
		       a.inflationRate == b.inflationRate &&
		       a.inflationResetDay == b.inflationResetDay &&
		       a.levelScale == b.levelScale &&
		       a.invertLevelMult == b.invertLevelMult &&
		       a.levelMult == b.levelMult;
	}

	static bool AreSettingsEqual(const Settings& a, const Settings& b)
	{
		if (a.scaleCatValues != b.scaleCatValues ||
			a.enableDynamicScaling != b.enableDynamicScaling ||
			a.includePowerModifiers != b.includePowerModifiers ||
			a.includePerkModifiers != b.includePerkModifiers ||
			a.enableInflation != b.enableInflation ||
			a.maxInflationCap != b.maxInflationCap ||
			a.enableLevelScaling != b.enableLevelScaling ||
			a.maxMultCap != b.maxMultCap ||
			a.enableDebug != b.enableDebug ||
			a.debugLevel != b.debugLevel ||
			a.hideCategories != b.hideCategories ||
			a.useActivateHandler != b.useActivateHandler ||
			a.useQuestWatcher != b.useQuestWatcher ||
			a.questOrderProfile != b.questOrderProfile) {
			return false;
		}

		if (a.categories.size() != b.categories.size()) {
			return false;
		}

		for (const auto& [key, catA] : a.categories) {
			auto it = b.categories.find(key);
			if (it == b.categories.end()) {
				return false;
			}
			if (!AreCategorySettingsEqual(catA, it->second)) {
				return false;
			}
		}

		return true;
	}

	static bool EqualsIgnoreCase(std::string_view a, std::string_view b)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char ca, char cb) {
			return ::tolower(static_cast<unsigned char>(ca)) == ::tolower(static_cast<unsigned char>(cb));
		});
	}

	static void LoadPresetIntoSettings(Settings& a_settings, const std::string& a_presetFilename)
	{
		auto cachedPresetOpt = DialogueGlobalRegistry::GetCachedPreset(a_presetFilename);
		if (!cachedPresetOpt || !cachedPresetOpt->iniData) {
			SKSE::log::warn("[QCS] Preset not found or uninitialized in cache: {}", a_presetFilename);
			return;
		}

		const auto& presetIni = *cachedPresetOpt->iniData;

		a_settings.scaleCatValues = presetIni.GetBoolValue("Settings", "bScaleValues", a_settings.scaleCatValues);
		a_settings.enableDynamicScaling = presetIni.GetBoolValue("Settings", "bEnableBarterMult", a_settings.enableDynamicScaling);
		a_settings.includePowerModifiers = presetIni.GetBoolValue("Settings", "bIncludePowerModifiers", a_settings.includePowerModifiers);
		a_settings.includePerkModifiers = presetIni.GetBoolValue("Settings", "bIncludePerkModifiers", a_settings.includePerkModifiers);
		a_settings.enableInflation = presetIni.GetBoolValue("Settings", "bEnableInflation", a_settings.enableInflation);
		a_settings.maxInflationCap = std::clamp(static_cast<float>(presetIni.GetDoubleValue("Settings", "fMaxInflationCap", a_settings.maxInflationCap)), 1.0f, 100.0f);
		a_settings.enableLevelScaling = presetIni.GetBoolValue("Settings", "bEnableLevelScaling", a_settings.enableLevelScaling);
		a_settings.maxMultCap = std::clamp(static_cast<float>(presetIni.GetDoubleValue("Settings", "fMaxMultCap", a_settings.maxMultCap)), 0.0f, 100000.0f);
		a_settings.enableDebug = presetIni.GetBoolValue("Settings", "bEnableDebugMode", a_settings.enableDebug);

		// This can be used by a preset to lock menu when it is loaded, but it can't unlock menu if the QCS profile is set to always hide.
		a_settings.hideCategories = presetIni.GetBoolValue("Settings", "bHideCategories", a_settings.hideCategories);

		// This can be used to chain different QCS profiles through preset INIs.
		a_settings.questOrderProfile = presetIni.GetValue("Settings", "sQuestOrderProfile", a_settings.questOrderProfile.c_str());

		// This can be used by a preset to disable the QCS, but the Menu will stay locked if the QCS profile is set to always hide, instead of using bHideCategories in presets to hide.
		a_settings.useQuestWatcher = presetIni.GetBoolValue("Settings", "bUseQuestWatcher", a_settings.useQuestWatcher);

		CSimpleIniA::TNamesDepend sections;
		presetIni.GetAllSections(sections);

		for (const auto& sec : sections) {
			std::string secName = sec.pItem;
			std::string lowerSec = secName;
			std::transform(lowerSec.begin(), lowerSec.end(), lowerSec.begin(), ::tolower);

			if (lowerSec == "settings") {
				continue;
			}

			std::string catID = lowerSec;
			if (lowerSec.starts_with("category_")) {
				catID = lowerSec.substr(9);
			}

			if (a_settings.GetCategory(catID) == nullptr) {
				continue;
			}

			auto& cat = a_settings.GetOrCreateCategory(catID);
			cat.enabled = presetIni.GetBoolValue(sec.pItem, "bScaleValues", cat.enabled);
			cat.multiplier = std::clamp(static_cast<float>(presetIni.GetDoubleValue(sec.pItem, "fStaticMultiplier", cat.multiplier)), 0.0f, 1000.0f);
			cat.dynamicScaling = presetIni.GetBoolValue(sec.pItem, "bEnableBarterMult", cat.dynamicScaling);
			cat.enableInflation = presetIni.GetBoolValue(sec.pItem, "bEnableInflation", cat.enableInflation);
			cat.invertInflation = presetIni.GetBoolValue(sec.pItem, "bInvertInflation", cat.invertInflation);
			cat.inflationRate = std::clamp(static_cast<float>(presetIni.GetDoubleValue(sec.pItem, "fInflationRate", cat.inflationRate)), 0.0f, 1.0f);
			cat.inflationResetDay = static_cast<float>(presetIni.GetDoubleValue(sec.pItem, "fInflationResetDay", cat.inflationResetDay));
			cat.levelScale = presetIni.GetBoolValue(sec.pItem, "bLevelScale", cat.levelScale);
			cat.invertLevelMult = presetIni.GetBoolValue(sec.pItem, "bInvertLevelMult", cat.invertLevelMult);
			cat.levelMult = std::clamp(static_cast<float>(presetIni.GetDoubleValue(sec.pItem, "fLevelMult", cat.levelMult)), 0.0f, 0.1f);
		}
	}

	static void ApplySettingModifier(Settings& a_settings, const std::string& a_section, const std::string& a_key, QuestActionType a_action, float a_value)
	{
		auto UpdateValue = [&](float& a_field, float a_min, float a_max) {
			float current = a_field;
			float target = current;

			switch (a_action) {
			case QuestActionType::kModifySetting:
				target = a_value;
				break;
			case QuestActionType::kAddSetting:
				target = current + a_value;
				break;
			case QuestActionType::kSubSetting:
				target = current - a_value;
				break;
			case QuestActionType::kMultSetting:
				target = current * a_value;
				break;
			default:
				return;
			}

			a_field = std::clamp(target, a_min, a_max);
		};

		auto UpdateBool = [&](bool& a_field) {
			if (a_action == QuestActionType::kModifySetting) {
				a_field = (a_value != 0.0f);
			}
		};

		std::string lowerSec = a_section;
		std::transform(lowerSec.begin(), lowerSec.end(), lowerSec.begin(), ::tolower);

		if (lowerSec == "settings") {
			if (EqualsIgnoreCase(a_key, "bHideCategories")) {
				UpdateBool(a_settings.hideCategories);
				return;
			}
			if (EqualsIgnoreCase(a_key, "fMaxInflationCap")) {
				UpdateValue(a_settings.maxInflationCap, 1.0f, 100.0f);
				return;
			}
			if (EqualsIgnoreCase(a_key, "fMaxMultCap")) {
				UpdateValue(a_settings.maxMultCap, 0.0f, 100000.0f);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bScaleValues")) {
				UpdateBool(a_settings.scaleCatValues);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bEnableBarterMult")) {
				UpdateBool(a_settings.enableDynamicScaling);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bIncludePowerModifiers")) {
				UpdateBool(a_settings.includePowerModifiers);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bIncludePerkModifiers")) {
				UpdateBool(a_settings.includePerkModifiers);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bEnableInflation")) {
				UpdateBool(a_settings.enableInflation);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bEnableLevelScaling")) {
				UpdateBool(a_settings.enableLevelScaling);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bEnableDebugMode")) {
				UpdateBool(a_settings.enableDebug);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bUseActivateHandler")) {
				UpdateBool(a_settings.useActivateHandler);
				return;
			}
			if (EqualsIgnoreCase(a_key, "bUseQuestWatcher")) {
				UpdateBool(a_settings.useQuestWatcher);
				return;
			}
			return;
		}

		std::string categoryID = lowerSec;
		if (categoryID.starts_with("category_")) {
			categoryID = categoryID.substr(9);
		}

		if (!categoryID.empty() && a_settings.GetCategory(categoryID) != nullptr) {
			auto& cat = a_settings.GetOrCreateCategory(categoryID);

			if (EqualsIgnoreCase(a_key, "fStaticMultiplier")) {
				UpdateValue(cat.multiplier, 0.0f, 1000.0f);
			} else if (EqualsIgnoreCase(a_key, "fInflationRate")) {
				UpdateValue(cat.inflationRate, 0.0f, 1.0f);
			} else if (EqualsIgnoreCase(a_key, "fLevelMult")) {
				UpdateValue(cat.levelMult, 0.0f, 0.1f);
			} else if (EqualsIgnoreCase(a_key, "fInflationResetDay")) {
				UpdateValue(cat.inflationResetDay, 0.0f, 1000000.0f);
			} else if (EqualsIgnoreCase(a_key, "bScaleValues")) {
				UpdateBool(cat.enabled);
			} else if (EqualsIgnoreCase(a_key, "bEnableBarterMult")) {
				UpdateBool(cat.dynamicScaling);
			} else if (EqualsIgnoreCase(a_key, "bEnableInflation")) {
				UpdateBool(cat.enableInflation);
			} else if (EqualsIgnoreCase(a_key, "bInvertInflation")) {
				UpdateBool(cat.invertInflation);
			} else if (EqualsIgnoreCase(a_key, "bLevelScale")) {
				UpdateBool(cat.levelScale);
			} else if (EqualsIgnoreCase(a_key, "bInvertLevelMult")) {
				UpdateBool(cat.invertLevelMult);
			}
		}
	}

	void QuestOrderManager::PreloadProfiles()
	{
		LoadTrackedQuests();
		std::unique_lock lock(s_profileCacheMutex);
		s_profileCache.clear();

		std::filesystem::path dirPath = "Data/SKSE/Plugins/DynamicDialogueCosts/QCSProfiles";
		if (!std::filesystem::exists(dirPath)) {
			SKSE::log::warn("[QCS] Profile directory missing: {}", dirPath.string());
			return;
		}

		for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
			if (entry.is_directory()) {
				std::string folderName = entry.path().filename().string();
				s_profileCache[folderName] = ParseProfileFolder(folderName);
				SKSE::log::info("[QCS] Preloaded profile folder '{}' [Display: '{}'] ({} rules, lockSettings: {})",
					folderName, s_profileCache[folderName].profileName, s_profileCache[folderName].rules.size(), s_profileCache[folderName].hideAndLockSettings);
			}
		}
	}

	QuestProfileData QuestOrderManager::GetProfileData(const std::string& a_profileFolder)
	{
		{
			std::shared_lock lock(s_profileCacheMutex);
			auto             it = s_profileCache.find(a_profileFolder);
			if (it != s_profileCache.end()) {
				return it->second;
			}
			for (const auto& [folder, data] : s_profileCache) {
				if ((!data.profileID.empty() && _stricmp(data.profileID.c_str(), a_profileFolder.c_str()) == 0) ||
					(!data.profileName.empty() && _stricmp(data.profileName.c_str(), a_profileFolder.c_str()) == 0) ||
					_stricmp(folder.c_str(), a_profileFolder.c_str()) == 0) {
					return data;
				}
			}
		}

		std::unique_lock lock(s_profileCacheMutex);
		auto             data = ParseProfileFolder(a_profileFolder);
		if (!data.rules.empty() || !data.profileName.empty()) {
			s_profileCache[a_profileFolder] = data;
		}
		return data;
	}

	std::vector<QuestRule> QuestOrderManager::GetProfileRules(const std::string& a_profileFolder)
	{
		return GetProfileData(a_profileFolder).rules;
	}

	std::string QuestOrderManager::GetProfileName(const std::string& a_profileFolder)
	{
		auto data = GetProfileData(a_profileFolder);
		return data.profileName.empty() ? a_profileFolder : data.profileName;
	}

	std::vector<QuestOrderProfileInfo> QuestOrderManager::GetLoadedProfiles()
	{
		{
			std::shared_lock lock(s_profileCacheMutex);
			if (!s_profileCache.empty()) {
				std::vector<QuestOrderProfileInfo> profiles;
				profiles.reserve(s_profileCache.size());
				for (const auto& [folderName, data] : s_profileCache) {
					profiles.push_back({ folderName, data.profileName, data.profileID, data.orderingPriority });
				}
				std::stable_sort(profiles.begin(), profiles.end(), [](const QuestOrderProfileInfo& a, const QuestOrderProfileInfo& b) {
					return a.orderingPriority > b.orderingPriority;
				});

				return profiles;
			}
		}

		PreloadProfiles();

		std::shared_lock                   lock(s_profileCacheMutex);
		std::vector<QuestOrderProfileInfo> profiles;
		profiles.reserve(s_profileCache.size());

		for (const auto& [folderName, data] : s_profileCache) {
			profiles.push_back({ folderName, data.profileName, data.profileID, data.orderingPriority });
		}
		std::stable_sort(profiles.begin(), profiles.end(), [](const QuestOrderProfileInfo& a, const QuestOrderProfileInfo& b) {
			return a.orderingPriority > b.orderingPriority;
		});

		return profiles;
	}

	std::unordered_map<std::string, std::vector<TrackedQuestRule>> g_trackedQuests;
	std::unordered_map<std::string, std::uint32_t>                 g_trackedQuestStageHits;
	std::unordered_map<std::string, std::uint32_t>                 g_questCompletionCounts;
	std::unordered_map<std::string, bool>                          g_conditionTruthCache;
	std::shared_mutex                                              g_conditionCacheMutex;

	void QuestOrderManager::LoadTrackedQuests()
	{
		g_trackedQuests.clear();
		std::filesystem::path dirPath = "Data/SKSE/Plugins/DynamicDialogueCosts/QCSProfiles";
		if (!std::filesystem::exists(dirPath)) {
			SKSE::log::warn("[QCS] Profile directory missing: {}", dirPath.string());
			return;
		}

		for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".ini") {
				continue;
			}

			std::ifstream file(entry.path());
			if (!file.is_open()) {
				SKSE::log::warn("[QCS] Failed to open tracked quests INI: {}", entry.path().string());
				continue;
			}

			std::string line;
			while (std::getline(file, line)) {
				if (auto pos = line.find("//"); pos != std::string::npos)
					line = line.substr(0, pos);

				std::istringstream iss(line);
				std::string        edid;
				if (!(iss >> edid) || edid.empty() || edid.front() == '[')
					continue;

				std::int32_t  stage = -1;
				std::uint32_t count = 1;

				if (iss >> stage) {
					if (!(iss >> count) || count == 0) {
						count = 1;
					}
				}

				std::string lowerEdid = edid;
				std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);
				auto& existingRules = g_trackedQuests[lowerEdid];
				bool  duplicateFound = std::any_of(existingRules.begin(), existingRules.end(), [stage, count](const TrackedQuestRule& r) {
					return r.completionStage == stage && r.requiredHits == count;
				});

				if (!duplicateFound) {
					existingRules.push_back({ stage, count });
					SKSE::log::debug("[QCS] Registered tracked quest rule: {} (Completion Stage: {}, Target Count: {}) from {}", edid, stage, count, entry.path().filename().string());
				} else {
					// more flexibility with bad patches with duplicate completion rules, note that these won't be logged on boot as debug/trace isn't enabled there
					SKSE::log::trace("[QCS] Skipped duplicate tracked quest rule for {} (Stage: {}, Count: {}) in {}", edid, stage, count, entry.path().filename().string());
				}
			}
		}

		std::unique_lock lock(s_watchedQuestsMutex);
		s_trackedQuestFormIDs.clear();
		for (const auto& [lowerEdid, rules] : g_trackedQuests) {
			if (auto quest = RE::TESForm::LookupByEditorID<RE::TESQuest>(lowerEdid)) {
				s_trackedQuestFormIDs.insert(quest->GetFormID());
				SKSE::log::trace("[QCS] Added tracked quest form ID: {}", quest->GetFormID());
			}
		}
	}

	void QuestCondition::BuildCacheKey()
	{
		if (isDefault) {
			cacheKey = "DEFAULT";
			return;
		}

		std::string lowerEdid = questEdid;
		std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);

		std::string bKey;
		if (checkCompletionCount) {
			if (targetCompletionStage >= 0) {
				bKey = lowerEdid + "@" + std::to_string(targetCompletionStage) + "#" + std::to_string(minCompletions);
			} else {
				bKey = lowerEdid + "#" + std::to_string(minCompletions);
			}
		} else {
			bKey = lowerEdid + ":" + std::to_string(minStage);
		}

		cacheKey = (isInverted ? "!" : "") + bKey;
	}

	void QuestOrderManager::InvalidateConditionCacheKey(const std::string& a_cacheKey)
	{
		std::unique_lock lock(g_conditionCacheMutex);
		g_conditionTruthCache.erase(a_cacheKey);
		SKSE::log::debug("[QCS] Invalidated condition cache key: {}", a_cacheKey);
	}

	void QuestOrderManager::InvalidateConditionCacheForQuest(const std::string& a_edid)
	{
		std::string lowerEdid = a_edid;
		std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);

		std::unique_lock lock(g_conditionCacheMutex);
		for (auto it = g_conditionTruthCache.begin(); it != g_conditionTruthCache.end();) {
			if (it->first.starts_with(lowerEdid + ":") || it->first.starts_with(lowerEdid + "#") || it->first.starts_with(lowerEdid + "@")) {
				it = g_conditionTruthCache.erase(it);
			} else {
				++it;
			}
		}
		SKSE::log::debug("[QCS] Invalidated condition cache entries for quest EDID: {}", a_edid);
	}

	bool QuestOrderManager::EvaluateCondition(const QuestCondition& a_cond)
	{
		if (a_cond.isDefault) {
			SKSE::log::debug("[QCS] Condition [DEFAULT] -> PASS");
			return true;
		}

		const std::string& cacheKey = a_cond.cacheKey;
		{
			std::shared_lock lock(g_conditionCacheMutex);
			if (auto it = g_conditionTruthCache.find(cacheKey); it != g_conditionTruthCache.end()) {
				bool rawPassed = it->second;
				bool result = a_cond.isInverted ? !rawPassed : rawPassed;
				SKSE::log::debug("[QCS] Condition [{}] (CACHED) -> Result: {}", cacheKey, result ? "PASS" : "FAIL");
				return result;
			}
		}
		bool rawPassed = false;

		if (a_cond.checkCompletionCount) {
			std::string lowerEdid = a_cond.questEdid;
			std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);

			std::string key = (a_cond.targetCompletionStage >= 0) ? (lowerEdid + "@" + std::to_string(a_cond.targetCompletionStage)) : lowerEdid;

			std::uint32_t count = 0;
			if (auto it = g_questCompletionCounts.find(key); it != g_questCompletionCounts.end()) {
				count = it->second;
			}
			rawPassed = (count >= a_cond.minCompletions);
		} else {
			if (!a_cond.questPtr && !a_cond.questEdid.empty()) {
				a_cond.questPtr = RE::TESForm::LookupByEditorID<RE::TESQuest>(a_cond.questEdid);
			}

			auto quest = a_cond.questPtr;
			if (!quest) {
				bool result = a_cond.isInverted;
				SKSE::log::debug("[QCS] Condition [{}{}:{}] -> Quest not found in load order. Result: {}",
					a_cond.isInverted ? "!" : "", a_cond.questEdid, a_cond.minStage, result ? "PASS" : "FAIL");
				return result;
			}

			bool          isCompleted = quest->IsCompleted();
			std::uint16_t currentStage = quest->GetCurrentStageID();
			rawPassed = isCompleted || (currentStage >= a_cond.minStage);
		}

		if (rawPassed) {
			std::unique_lock lock(g_conditionCacheMutex);
			g_conditionTruthCache[cacheKey] = true;
			SKSE::log::debug("[QCS] Condition [{}] state stored to Confluence Cache.", cacheKey);
		}

		bool result = a_cond.isInverted ? !rawPassed : rawPassed;
		SKSE::log::debug("[QCS] Condition [{}] -> Result: {}", cacheKey, result ? "PASS" : "FAIL");
		return result;
	}

	bool QuestOrderManager::IsQuestWatched(RE::FormID a_formID)
	{
		if (a_formID == 0)
			return false;
		std::shared_lock lock(s_watchedQuestsMutex);
		return s_watchedQuestFormIDs.contains(a_formID) || s_trackedQuestFormIDs.contains(a_formID);
	}

	void QuestOrderManager::IncrementQuestCompletion(const std::string& a_edid, std::int32_t a_completionStage)
	{
		if (a_edid.empty() || a_edid == "UNKNOWN")
			return;

		std::string lowerEdid = a_edid;
		std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);

		g_questCompletionCounts[lowerEdid]++;
		if (a_completionStage >= 0) {
			std::string pathKey = lowerEdid + "@" + std::to_string(a_completionStage);
			g_questCompletionCounts[pathKey]++;
			SKSE::log::info("[QCS] Quest '{}' stage {} completion count incremented to {}", a_edid, a_completionStage, g_questCompletionCounts[pathKey]);
		}

		InvalidateConditionCacheForQuest(a_edid);
		SKSE::log::info("[QCS] Quest '{}' total completion count incremented to {}", a_edid, g_questCompletionCounts[lowerEdid]);
	}

	QuestProfileData QuestOrderManager::ParseProfileFolder(const std::string& a_profileFolder)
	{
		QuestProfileData      profileData;
		std::filesystem::path folderPath = std::filesystem::path("Data/SKSE/Plugins/DynamicDialogueCosts/QCSProfiles") / a_profileFolder;

		if (!std::filesystem::exists(folderPath) || !std::filesystem::is_directory(folderPath)) {
			SKSE::log::warn(" [QuestControl] Profile directory missing or invalid: {}", folderPath.string());
			return profileData;
		}

		std::filesystem::path controlPath = folderPath / "settings.ini";
		if (std::filesystem::exists(controlPath)) {
			CSimpleIniA controlIni;
			controlIni.SetUnicode();
			if (controlIni.LoadFile(controlPath.c_str()) >= 0) {
				const char* nameVal = controlIni.GetValue("Settings", "sProfileName", nullptr);
				if (!nameVal) {
					nameVal = controlIni.GetValue("", "sProfileName", nullptr);
				}
				if (nameVal) {
					profileData.profileName = nameVal;
				}

				const char* idVal = controlIni.GetValue("Settings", "sProfileID", nullptr);
				if (!idVal) {
					idVal = controlIni.GetValue("Settings", "sQuestOrderProfile", nullptr);
				}
				if (!idVal) {
					idVal = controlIni.GetValue("", "sProfileID", nullptr);
				}
				if (idVal) {
					profileData.profileID = idVal;
				}
				profileData.orderingPriority = static_cast<int>(controlIni.GetLongValue("Settings", "iOrderingPriority", 0));
				profileData.hideAndLockSettings = controlIni.GetBoolValue("Settings", "bHideAndLockSettings", false);
			}
		}

		if (profileData.profileName.empty()) {
			profileData.profileName = a_profileFolder;
		}

		struct LineFileEntry
		{
			int                    id{ 0 };
			std::vector<QuestRule> rules;
		};
		std::vector<LineFileEntry> fileEntries;

		std::filesystem::path rulesPath = folderPath / "rules";
		if (std::filesystem::exists(rulesPath) && std::filesystem::is_directory(rulesPath)) {
			for (const auto& entry : std::filesystem::directory_iterator(rulesPath)) {
				if (!entry.is_regular_file() || entry.path().extension() != ".txt")
					continue;

				std::ifstream file(entry.path(), std::ios::in | std::ios::binary | std::ios::ate);
				if (!file.is_open())
					continue;

				auto fileSize = file.tellg();
				if (fileSize <= 0)
					continue;

				std::string buffer(fileSize, '\0');
				file.seekg(0);
				file.read(buffer.data(), fileSize);

				LineFileEntry    fileEntry;
				std::string_view view(buffer);
				size_t           start = 0;

				while (start < view.size()) {
					size_t           end = view.find_first_of("\r\n", start);
					std::string_view line = (end == std::string_view::npos) ? view.substr(start) : view.substr(start, end - start);

					if (end == std::string_view::npos)
						start = view.size();
					else
						start = view.find_first_not_of("\r\n", end);

					if (auto slashPos = line.find("//"); slashPos != std::string_view::npos) {
						line = line.substr(0, slashPos);
					}

					auto first = line.find_first_not_of(" \t");
					if (first == std::string_view::npos)
						continue;
					auto last = line.find_last_not_of(" \t");
					line = line.substr(first, (last - first + 1));

					std::string lowerLine(line);
					std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);

					auto idPos = lowerLine.find("id");
					if (idPos != std::string::npos) {
						auto eqPos = lowerLine.find_first_of("=", idPos);
						if (eqPos != std::string::npos) {
							std::string_view valStr = line.substr(eqPos + 1);
							auto             valStart = valStr.find_first_not_of(" \t");
							if (valStart != std::string_view::npos) {
								std::from_chars(valStr.data() + valStart, valStr.data() + valStr.size(), fileEntry.id);
							}
							continue;
						}
					}

					std::vector<std::string_view> tokens;
					size_t                        pos = 0;
					while (pos < line.size()) {
						pos = line.find_first_not_of(" \t", pos);
						if (pos == std::string_view::npos)
							break;
						size_t tokEnd = line.find_first_of(" \t", pos);
						if (tokEnd == std::string_view::npos)
							tokEnd = line.size();
						tokens.push_back(line.substr(pos, tokEnd - pos));
						pos = tokEnd;
					}

					if (tokens.empty())
						continue;

					QuestRule rule;
					size_t    actionIdx = tokens.size();

					for (size_t i = 0; i < tokens.size(); ++i) {
						std::string upperToken(tokens[i]);
						std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(), ::toupper);

						if (upperToken == "PRESET" || upperToken == "SET" || upperToken == "ADD" || upperToken == "SUB" || upperToken == "MULT") {
							actionIdx = i;
							break;
						}
					}

					if (actionIdx == tokens.size())
						continue;

					for (size_t i = 0; i < actionIdx; ++i) {
						QuestCondition cond;
						std::string    condStr(tokens[i]);

						std::string upperCond = condStr;
						std::transform(upperCond.begin(), upperCond.end(), upperCond.begin(), ::toupper);

						if (upperCond == "DEFAULT") {
							cond.isDefault = true;
						} else {
							if (condStr.starts_with('!')) {
								cond.isInverted = true;
								condStr = condStr.substr(1);
							}

							auto atPos = condStr.find('@');
							auto hashPos = condStr.find('#');
							auto colonPos = condStr.find(':');

							if (hashPos != std::string::npos) {
								cond.checkCompletionCount = true;
								if (atPos != std::string::npos && atPos < hashPos) {
									cond.questEdid = condStr.substr(0, atPos);
									try {
										cond.targetCompletionStage = std::stoi(condStr.substr(atPos + 1, hashPos - (atPos + 1)));
									} catch (...) {
										cond.targetCompletionStage = -1;
									}
								} else {
									cond.questEdid = condStr.substr(0, hashPos);
								}

								try {
									cond.minCompletions = static_cast<std::uint32_t>(std::stoul(condStr.substr(hashPos + 1)));
								} catch (...) {
									cond.minCompletions = 1;
								}

								if (cond.targetCompletionStage >= 0) {
									std::string lowerEdid = cond.questEdid;
									std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);
									auto& existing = g_trackedQuests[lowerEdid];
									bool  exists = std::any_of(existing.begin(), existing.end(), [&](const TrackedQuestRule& r) {
										return r.completionStage == cond.targetCompletionStage;
									});
									if (!exists) {
										existing.push_back({ cond.targetCompletionStage, 1 });
										if (auto q = RE::TESForm::LookupByEditorID<RE::TESQuest>(lowerEdid)) {
											std::unique_lock lock(s_watchedQuestsMutex);
											s_trackedQuestFormIDs.insert(q->GetFormID());
										}
									}
								}
							} else if (colonPos != std::string::npos) {
								cond.questEdid = condStr.substr(0, colonPos);
								try {
									cond.minStage = static_cast<std::uint16_t>(std::stoi(condStr.substr(colonPos + 1)));
								} catch (...) {
									cond.minStage = 1;
								}
							} else {
								cond.questEdid = condStr;
								cond.minStage = 1;
							}
						}
						cond.BuildCacheKey();
						rule.conditions.push_back(cond);
					}

					size_t currIdx = actionIdx;
					while (currIdx < tokens.size()) {
						std::string upperToken(tokens[currIdx]);
						std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(), ::toupper);

						QuestAction action;
						if (upperToken == "PRESET") {
							action.actionType = QuestActionType::kLoadPreset;
							if (currIdx + 1 < tokens.size()) {
								action.presetFilename = std::string(tokens[currIdx + 1]);
								currIdx += 2;
							} else
								break;
						} else {
							if (upperToken == "SET")
								action.actionType = QuestActionType::kModifySetting;
							else if (upperToken == "ADD")
								action.actionType = QuestActionType::kAddSetting;
							else if (upperToken == "SUB")
								action.actionType = QuestActionType::kSubSetting;
							else if (upperToken == "MULT")
								action.actionType = QuestActionType::kMultSetting;

							if (currIdx + 3 < tokens.size()) {
								action.section = std::string(tokens[currIdx + 1]);
								action.key = std::string(tokens[currIdx + 2]);
								action.value = std::string(tokens[currIdx + 3]);
								try {
									action.parsedValue = std::stof(action.value);
								} catch (...) {
									action.parsedValue = 0.0f;
								}
								currIdx += 4;
							} else
								break;
						}

						rule.actions.push_back(action);

						if (currIdx < tokens.size() && tokens[currIdx] == "|") {
							currIdx++;
						} else {
							break;
						}
					}

					if (!rule.actions.empty()) {
						fileEntry.rules.push_back(rule);
					}
				}

				if (!fileEntry.rules.empty()) {
					fileEntries.push_back(std::move(fileEntry));
				}
			}
		}

		std::stable_sort(fileEntries.begin(), fileEntries.end(), [](const LineFileEntry& a, const LineFileEntry& b) {
			return a.id > b.id;
		});

		for (auto& fe : fileEntries) {
			profileData.rules.insert(profileData.rules.end(), fe.rules.begin(), fe.rules.end());
		}

		for (auto& rule : profileData.rules) {
			for (auto& cond : rule.conditions) {
				if (!cond.isDefault && !cond.questEdid.empty()) {
					cond.questPtr = RE::TESForm::LookupByEditorID<RE::TESQuest>(cond.questEdid);
				}
			}
		}

		return profileData;
	}

	bool QuestOrderManager::EvaluateRule(const QuestRule& a_rule)
	{
		for (const auto& cond : a_rule.conditions) {
			if (!EvaluateCondition(cond)) {
				return false;
			}
		}
		return true;
	}

	void QuestOrderManager::EvaluateAndApplyProfile(const std::string& a_triggeredQuestEdid)
	{
		auto cfg = g_settings.load();
		if (!cfg)
			return;
		BENCHMARK_SCOPE_IF("QCS:EvaluateAndApplyProfile", spdlog::should_log(spdlog::level::info));

		if (!cfg->useQuestWatcher) {
			MCPddcMenu::SetCategoryHiding(cfg->hideCategories, false);
			return;
		}

		SKSE::log::trace("[QCS] Starting Profile Evaluation: {} by: {}",
			cfg->questOrderProfile, a_triggeredQuestEdid.empty() ? "$load" : a_triggeredQuestEdid);

		auto        profileData = GetProfileData(cfg->questOrderProfile);
		const auto& rules = profileData.rules;
		if (rules.empty()) {
			SKSE::log::warn("[QCS] Active profile is empty or missing: {}", cfg->questOrderProfile);
			return;
		}

		std::unique_lock stateLock(s_ruleStateMutex);

		bool profileChanged = (s_currentActiveProfile != cfg->questOrderProfile) || (s_activeRuleStates.size() != rules.size());
		if (profileChanged) {
			s_currentActiveProfile = cfg->questOrderProfile;
			s_activeRuleStates.assign(rules.size(), false);
			s_activeQuestToRulesMap.clear();

			std::unique_lock watchedLock(s_watchedQuestsMutex);
			s_watchedQuestFormIDs.clear();

			for (size_t i = 0; i < rules.size(); ++i) {
				const auto& rule = rules[i];
				for (const auto& cond : rule.conditions) {
					if (!cond.isDefault && !cond.questEdid.empty()) {
						if (!cond.questPtr) {
							cond.questPtr = RE::TESForm::LookupByEditorID<RE::TESQuest>(cond.questEdid);
						}
						if (auto quest = cond.questPtr) {
							s_watchedQuestFormIDs.insert(quest->GetFormID());
						}
						std::string lowerEdid = cond.questEdid;
						std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);
						s_activeQuestToRulesMap[lowerEdid].push_back(i);
					}
				}
			}
		}

		if (a_triggeredQuestEdid.empty() && !profileChanged) {
			SKSE::log::trace("[QCS] Empty evaluation call. Applying current rule states without re-evaluation.");
		} else if (profileChanged || a_triggeredQuestEdid == "$FULL") {
			SKSE::log::trace("[QCS] Performing full evaluation of all {} rules (Reason: {}).",
				rules.size(), profileChanged ? "Profile Changed" : "Explicit Full Trigger ($FULL)");
			for (size_t i = 0; i < rules.size(); ++i) {
				s_activeRuleStates[i] = EvaluateRule(rules[i]);
			}
		} else {
			std::string lowerTriggered = a_triggeredQuestEdid;
			std::transform(lowerTriggered.begin(), lowerTriggered.end(), lowerTriggered.begin(), ::tolower);

			auto it = s_activeQuestToRulesMap.find(lowerTriggered);
			if (it != s_activeQuestToRulesMap.end()) {
				SKSE::log::trace("[QCS] Re-evaluating {} rule(s) associated with quest: {}", it->second.size(), a_triggeredQuestEdid);
				for (size_t ruleIdx : it->second) {
					s_activeRuleStates[ruleIdx] = EvaluateRule(rules[ruleIdx]);
				}
			} else {
				SKSE::log::trace("[QCS] No rules associated with triggered quest EDID: {}", a_triggeredQuestEdid);
			}
		}

		Settings    workingSettings = *cfg;
		std::string lastLoadedPreset = "";

		struct MatchedAction
		{
			QuestActionType actionType;
			std::string     presetFilename;
			std::string     section;
			std::string     key;
			float           value;
		};

		std::vector<MatchedAction> matchedActions;
		matchedActions.reserve(rules.size());

		for (size_t i = 0; i < rules.size(); ++i) {
			if (s_activeRuleStates[i]) {
				const auto& rule = rules[i];
				SKSE::log::trace("[QCS] Rule #{} is ACTIVE - Executing action(s): {}.", i + 1, rule.actions.size());
				for (const auto& act : rule.actions) {
					if (act.actionType == QuestActionType::kLoadPreset) {
						lastLoadedPreset = act.presetFilename;
						matchedActions.push_back({ act.actionType, act.presetFilename, {}, {}, 0.0f });
					} else {
						matchedActions.push_back({ act.actionType, {}, act.section, act.key, act.parsedValue });
					}
				}
			}
		}

		stateLock.unlock();

		for (const auto& action : matchedActions) {
			if (action.actionType == QuestActionType::kLoadPreset) {
				LoadPresetIntoSettings(workingSettings, action.presetFilename);
			} else {
				ApplySettingModifier(workingSettings, action.section, action.key, action.actionType, action.value);
			}
		}

		workingSettings.hideCategories = profileData.hideAndLockSettings || workingSettings.hideCategories;
		bool settingsChanged = !AreSettingsEqual(workingSettings, *cfg);

		if (settingsChanged) {
			g_settings.store(std::make_shared<Settings>(workingSettings));
			CheckAndApplyPersistentPatches(true);
			DialoguePriceTransformer::PokeQuestsRefresh();
			SKSE::log::trace("[QCS] Profile Evaluation Complete (Settings Updated)");
		} else {
			SKSE::log::trace("[QCS] Profile Evaluation Complete (No Changes)");
		}

		MCPddcMenu::SetCategoryHiding(workingSettings.hideCategories, false);

		if (!lastLoadedPreset.empty()) {
			MCPddcMenu::SetSelectedPresetByFilename(lastLoadedPreset);
		}
	}
}