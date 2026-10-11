#include "dialogueRegistry.h"
#include "PCH.h"
#include "QOManager.h"
#include "settings.h"
#include <SimpleIni.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>

namespace DynamicDialogueCosts
{
	RE::TESGlobal* DialogueGlobalRegistry::GetDialoguePriceGlobalByEdid(std::string_view a_edid)
	{
		std::string lowerEdid(a_edid);
		std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);
		auto it = g_dialogueStepGlobalEdids.find(lowerEdid);
		return (it != g_dialogueStepGlobalEdids.end()) ? it->second : nullptr;
	}

	DialogueGlobalRegistry::RawCategoryData DialogueGlobalRegistry::ParseCategoryDataTask(const std::filesystem::path& a_folderPath)
	{
		RawCategoryData data{};
		std::string     catID = a_folderPath.filename().string();
		std::string     displayName = catID;

		CategorySettings diskDefaults{};
		diskDefaults.categoryID = catID;

		std::filesystem::path iniPath = a_folderPath / "cat.ini";
		if (!std::filesystem::exists(iniPath)) {
			iniPath = a_folderPath / "category.ini";
		}

		if (std::filesystem::exists(iniPath)) {
			CSimpleIniA ini;
			ini.SetUnicode();
			if (ini.LoadFile(iniPath.c_str()) >= 0) {
				const char* idVal = ini.GetValue("Category", "sCategoryID", nullptr);
				if (idVal && idVal[0] != '\0')
					catID = idVal;
				diskDefaults.priority = static_cast<int>(ini.GetLongValue("Category", "iOrderingPriority", ini.GetLongValue("Category", "iPriority", diskDefaults.priority)));
				const char* dispNameVal = ini.GetValue("Category", "sDisplayName", nullptr);
				if (dispNameVal && dispNameVal[0] != '\0')
					displayName = dispNameVal;
				diskDefaults.persistentLeveledLists = ini.GetBoolValue("Category", "bPersistLeveledLists", diskDefaults.persistentLeveledLists);
				diskDefaults.usesSellBarterMult = ini.GetBoolValue("Category", "bUsesSellBarterMult", diskDefaults.usesSellBarterMult);
				diskDefaults.hideCatInMenu = ini.GetBoolValue("Category", "bHideCatInMenu", diskDefaults.hideCatInMenu);

				diskDefaults.enabled = ini.GetBoolValue("Category", "bScaleValues", diskDefaults.enabled);
				diskDefaults.multiplier = static_cast<float>(ini.GetDoubleValue("Category", "fStaticMultiplier", diskDefaults.multiplier));
				diskDefaults.dynamicScaling = ini.GetBoolValue("Category", "bEnableBarterMult", diskDefaults.dynamicScaling);
				diskDefaults.enableInflation = ini.GetBoolValue("Category", "bEnableInflation", diskDefaults.enableInflation);
				diskDefaults.invertInflation = ini.GetBoolValue("Category", "bInvertInflation", diskDefaults.invertInflation);
				diskDefaults.inflationRate = static_cast<float>(ini.GetDoubleValue("Category", "fInflationRate", diskDefaults.inflationRate));
				diskDefaults.inflationResetDay = static_cast<float>(ini.GetDoubleValue("Category", "fInflationResetDay", diskDefaults.inflationResetDay));
				diskDefaults.levelScale = ini.GetBoolValue("Category", "bLevelScale", diskDefaults.levelScale);
				diskDefaults.invertLevelMult = ini.GetBoolValue("Category", "bInvertLevelMult", diskDefaults.invertLevelMult);
				diskDefaults.levelMult = static_cast<float>(ini.GetDoubleValue("Category", "fLevelMult", diskDefaults.levelMult));
			}
		}

		std::string lowerCatID = catID;
		std::transform(lowerCatID.begin(), lowerCatID.end(), lowerCatID.begin(), ::tolower);
		diskDefaults.categoryID = lowerCatID;
		diskDefaults.displayName = displayName;

		data.catID = lowerCatID;
		data.displayName = displayName;
		data.diskDefaults = diskDefaults;

		auto parseTextLines = [](const std::filesystem::path& folderPath, auto&& lineCallback) {
			if (!std::filesystem::exists(folderPath) || !std::filesystem::is_directory(folderPath))
				return;
			for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
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

				std::string_view view(buffer);
				size_t           start = 0;
				while (start < view.size()) {
					size_t           end = view.find_first_of("\r\n", start);
					std::string_view line = (end == std::string_view::npos) ? view.substr(start) : view.substr(start, end - start);

					if (auto pos = line.find_first_of(";"); pos != std::string_view::npos)
						line = line.substr(0, pos);
					if (auto pos = line.find("//"); pos != std::string_view::npos)
						line = line.substr(0, pos);

					auto first = line.find_first_not_of(" \t");
					if (first != std::string_view::npos) {
						auto last = line.find_last_not_of(" \t");
						lineCallback(line.substr(first, (last - first + 1)));
					}

					if (end == std::string_view::npos)
						break;
					start = view.find_first_not_of("\r\n", end);
				}
			}
		};

		parseTextLines(a_folderPath / "GLOB", [&](std::string_view line) { data.globEditorIDs.emplace_back(line); });

		parseTextLines(a_folderPath / "LVLI", [&](std::string_view line) { data.lvliEditorIDs.emplace_back(line); });

		parseTextLines(a_folderPath / "ITEM", [&](std::string_view line) { data.itemEditorIDs.emplace_back(line); });

		parseTextLines(a_folderPath / "SETT", [&](std::string_view line) {
            size_t pos = 0;
            auto getNextToken = [&]() -> std::string_view {
                pos = line.find_first_not_of(" ,\t", pos);
                if (pos == std::string_view::npos) return {};
                size_t end = line.find_first_of(" ,\t", pos);
                std::string_view token = (end == std::string_view::npos) ? line.substr(pos) : line.substr(pos, end - pos);
                pos = end;
                return token;
                };

            auto edidT = getNextToken();
            if (edidT.empty()) return;

            int idNb = 0;
            if (auto idT = getNextToken(); !idT.empty()) {
                std::from_chars(idT.data(), idT.data() + idT.size(), idNb);
            }
            data.settEntries.emplace_back(std::string(edidT), idNb); });

		parseTextLines(a_folderPath / "FACT", [&](std::string_view line) {
			size_t pos = 0;
			auto   getNextToken = [&]() -> std::string_view {
				pos = line.find_first_not_of(" ,\t", pos);
				if (pos == std::string_view::npos)
					return {};
				size_t           end = line.find_first_of(" ,\t", pos);
				std::string_view token = (end == std::string_view::npos) ? line.substr(pos) : line.substr(pos, end - pos);
				pos = end;
				return token;
			};

			auto edidT = getNextToken();
			auto fieldT = getNextToken();
			if (!edidT.empty() && !fieldT.empty()) {
				data.factEntries.emplace_back(std::string(edidT), std::string(fieldT));
			}
		});

		parseTextLines(a_folderPath / "EXTG", [&](std::string_view line) {
            size_t pos = 0;
            auto getNextToken = [&]() -> std::string_view {
                pos = line.find_first_not_of(" ,\t", pos);
                if (pos == std::string_view::npos) return {};
                size_t end = line.find_first_of(" ,\t", pos);
                std::string_view token = (end == std::string_view::npos) ? line.substr(pos) : line.substr(pos, end - pos);
                pos = end;
                return token;
                };

            auto edidT = getNextToken();
            auto opidT = getNextToken();
            if (edidT.empty() || opidT.empty()) return;

            std::string opStr(opidT);
            std::transform(opStr.begin(), opStr.end(), opStr.begin(), ::tolower);

            ExtGlobalOp op;
            if (opStr == "mult" || opStr == "multstack") op = ExtGlobalOp::Multiply;
            else if (opStr == "add" || opStr == "addstack") op = ExtGlobalOp::AddPercent;
            else if (opStr == "sub" || opStr == "substack") op = ExtGlobalOp::SubPercent;
            else if (opStr == "flat" || opStr == "flatstack") op = ExtGlobalOp::FlatOffset;
            else if (opStr == "alien" || opStr == "alienstack") op = ExtGlobalOp::Alien;
            else return;

            float scale = 1.0f;
            if (auto scaleTok = getNextToken(); !scaleTok.empty()) {
                std::from_chars(scaleTok.data(), scaleTok.data() + scaleTok.size(), scale);
            }
            data.extgEntries.emplace_back(std::string(edidT), op, scale); });

		return data;
	}

	void DialogueGlobalRegistry::IndexCatRegistry()
	{
		BENCHMARK_SCOPE("IndexCatRegistry");
		g_cachedCategoryMap.clear();
		g_cachedLeveledListCategoryMap.clear();
		g_cachedItemCategoryMap.clear();
		g_cachedSettEntries.clear();
		g_cachedExternalGlobalCategoryMap.clear();
		g_cachedFactionCrimeEntries.clear();
		g_categoryDisplayNames.clear();
		g_categoryDiskDefaults.clear();

		const std::filesystem::path baseFolder = "Data/SKSE/Plugins/DynamicDialogueCosts/CatRegistry";
		if (!std::filesystem::is_directory(baseFolder))
			return;

		std::vector<std::filesystem::path> categoryDirs;
		for (const auto& entry : std::filesystem::directory_iterator(baseFolder)) {
			if (entry.is_directory()) {
				categoryDirs.push_back(entry.path());
			}
		}

		if (categoryDirs.empty())
			return;

		std::vector<std::future<RawCategoryData>> futures;
		futures.reserve(categoryDirs.size());

		for (const auto& dirPath : categoryDirs) {
			futures.push_back(std::async(std::launch::async, ParseCategoryDataTask, dirPath));
		}

		auto gmstCollection = RE::GameSettingCollection::GetSingleton();

		for (auto& fut : futures) {
			auto catData = fut.get();

			g_categoryDisplayNames[catData.catID] = catData.displayName;
			g_categoryDiskDefaults[catData.catID] = catData.diskDefaults;

			for (const auto& edid : catData.globEditorIDs) {
				if (auto global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(edid)) {
					std::string lowerLine = edid;
					std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);
					g_cachedCategoryMap[global] = GlobalCategoryEntry{ global, catData.catID, lowerLine };
				}
			}

			for (const auto& edid : catData.lvliEditorIDs) {
				if (auto levItem = RE::TESForm::LookupByEditorID<RE::TESLevItem>(edid)) {
					g_cachedLeveledListCategoryMap[levItem] = catData.catID;
				}
			}

			for (const auto& edid : catData.itemEditorIDs) {
				if (auto form = RE::TESForm::LookupByEditorID<RE::TESForm>(edid)) {
					std::int32_t* valPtr = nullptr;
					std::int32_t  origVal = 0;

					if (auto valueForm = form->As<RE::TESValueForm>()) {
						valPtr = &valueForm->value;
						origVal = valueForm->value;
					} else if (auto alch = form->As<RE::AlchemyItem>()) {
						valPtr = &alch->data.costOverride;
						origVal = alch->data.costOverride;
					}

					if (valPtr) {
						g_cachedItemCategoryMap[form] = CachedItemData{ catData.catID, origVal, valPtr };
						if (!edid.empty()) {
							g_debugItemEditorIDs[form] = edid;
						}
					}
				}
			}

			for (const auto& [edid, idNb] : catData.settEntries) {
				auto         global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(edid);
				RE::Setting* gmst = nullptr;

				if (!global && gmstCollection) {
					gmst = gmstCollection->GetSetting(edid.c_str());
				}
				if (!global && !gmst)
					continue;

				std::string lowerEdid = edid;
				std::transform(lowerEdid.begin(), lowerEdid.end(), lowerEdid.begin(), ::tolower);
				g_cachedSettEntries.push_back(SettEntry{ global, gmst, idNb, catData.catID, lowerEdid });
			}

			for (const auto& [edid, fieldName] : catData.factEntries) {
				if (auto faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(edid)) {
					std::string lowerField = fieldName;
					std::transform(lowerField.begin(), lowerField.end(), lowerField.begin(), ::tolower);

					FactionCrimeField fieldType;
					float             origVal = 0.0f;

					if (lowerField == "stealmult") {
						fieldType = FactionCrimeField::kStealMult;
						origVal = faction->crimeData.crimevalues.stealCrimeGoldMult;
					} else if (lowerField == "murder") {
						fieldType = FactionCrimeField::kMurder;
						origVal = static_cast<float>(faction->crimeData.crimevalues.murderCrimeGold);
					} else if (lowerField == "assault") {
						fieldType = FactionCrimeField::kAssault;
						origVal = static_cast<float>(faction->crimeData.crimevalues.assaultCrimeGold);
					} else if (lowerField == "trespass") {
						fieldType = FactionCrimeField::kTrespass;
						origVal = static_cast<float>(faction->crimeData.crimevalues.trespassCrimeGold);
					} else if (lowerField == "pickpocket") {
						fieldType = FactionCrimeField::kPickpocket;
						origVal = static_cast<float>(faction->crimeData.crimevalues.pickpocketCrimeGold);
					} else if (lowerField == "jailbreak") {
						fieldType = FactionCrimeField::kEscape;
						origVal = static_cast<float>(faction->crimeData.crimevalues.escapeCrimeGold);
					} else if (lowerField == "shapeshift") {
						fieldType = FactionCrimeField::kWerewolf;
						origVal = static_cast<float>(faction->crimeData.crimevalues.werewolfCrimeGold);
					} else {
						continue;
					}

					g_cachedFactionCrimeEntries.push_back({ catData.catID, faction, fieldType, origVal, edid });
					SKSE::log::trace("[TEMP] Registered faction crime entry: <{}> {}: {}", catData.catID, edid, lowerField);
				}
			}

			for (const auto& [edid, op, scale] : catData.extgEntries) {
				if (auto global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(edid)) {
					g_cachedExternalGlobalCategoryMap.emplace(catData.catID, ExtGlobalEntry{ global, op, scale });
				}
			}
		}

		std::sort(g_cachedSettEntries.begin(), g_cachedSettEntries.end());

		if (auto baseGoldGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("Hireling_BaseGold")) {
			if (auto hirelingGoldGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("hirelinggold")) {
				auto it = g_cachedCategoryMap.find(hirelingGoldGlobal);
				if (it != g_cachedCategoryMap.end()) {
					GlobalCategoryEntry entry = it->second;
					entry.global = baseGoldGlobal;
					entry.editorID = "Hireling_BaseGold";
					g_cachedCategoryMap.erase(it);
					if (!g_cachedCategoryMap.contains(baseGoldGlobal)) {
						g_cachedCategoryMap[baseGoldGlobal] = entry;
					}
				}
			}
		}
		UpdateCategoryMultipliers();
	}

	void DialogueGlobalRegistry::UpdateCategoryMultipliers()
	{
		g_dialogueStepGlobals.clear();
		g_dialogueStepGlobalEdids.clear();

		for (const auto& [global, entry] : g_cachedCategoryMap) {
			if (!global)
				continue;
			g_dialogueStepGlobals[global] = entry;
			g_dialogueStepGlobalEdids[entry.editorID] = global;
		}

		for (const auto& settEntry : g_cachedSettEntries) {
			if (!settEntry.global)
				continue;

			GlobalCategoryEntry entry{ settEntry.global, settEntry.categoryID, settEntry.editorID };
			g_dialogueStepGlobals[settEntry.global] = entry;
			g_dialogueStepGlobalEdids[settEntry.editorID] = settEntry.global;
		}
	}

	void DialogueGlobalRegistry::LoadPresetsCache()
	{
		std::unique_lock lock(g_presetCacheMutex);
		g_cachedPresets.clear();
		g_cachedPresetsMap.clear();

		const std::filesystem::path presetsDir = "Data/SKSE/Plugins/DynamicDialogueCosts/Presets";
		if (std::filesystem::exists(presetsDir)) {
			for (const auto& entry : std::filesystem::directory_iterator(presetsDir)) {
				if (!entry.is_regular_file() || entry.path().extension() != ".ini")
					continue;

				CachedPreset preset;
				preset.filename = entry.path().filename().string();
				preset.filepath = entry.path();
				preset.displayName = preset.filename;
				preset.priority = 0;

				preset.iniData->SetUnicode();
				if (preset.iniData->LoadFile(entry.path().c_str()) >= 0) {
					const char* nameVal = preset.iniData->GetValue("Settings", "sPresetName", nullptr);
					if (nameVal && nameVal[0] != '\0') {
						preset.displayName = nameVal;
					}
					preset.priority = static_cast<int>(preset.iniData->GetLongValue("Settings", "iOrderingPriority", 0));
				}

				std::string lowerName = preset.filename;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

				g_cachedPresetsMap[lowerName] = preset;
				g_cachedPresets.push_back(preset);
			}
		}

		if (g_cachedPresets.empty()) {
			std::filesystem::path fallbackPath = "Data/SKSE/Plugins/DynamicDialogueCosts.ini";
			if (std::filesystem::exists(fallbackPath)) {
				CachedPreset fallback;
				fallback.filename = "DynamicDialogueCosts.ini";
				fallback.filepath = fallbackPath;
				fallback.displayName = "NG INI";
				fallback.priority = 0;

				fallback.iniData->SetUnicode();
				fallback.iniData->LoadFile(fallbackPath.c_str());

				std::string lowerName = fallback.filename;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				g_cachedPresetsMap[lowerName] = fallback;
				g_cachedPresets.push_back(fallback);
			}
		}
		std::stable_sort(g_cachedPresets.begin(), g_cachedPresets.end(), [](const CachedPreset& a, const CachedPreset& b) { return a.priority > b.priority; });

		SKSE::log::info("[LPCH] Cached {} presets in memory", g_cachedPresets.size());
	}

	std::vector<CachedPreset> DialogueGlobalRegistry::GetCachedPresets()
	{
		std::shared_lock lock(g_presetCacheMutex);
		return g_cachedPresets;
	}

	std::optional<CachedPreset> DialogueGlobalRegistry::GetCachedPreset(const std::string& a_filename)
	{
		std::shared_lock lock(g_presetCacheMutex);
		std::string      lowerName = a_filename;
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

		auto it = g_cachedPresetsMap.find(lowerName);
		if (it != g_cachedPresetsMap.end()) {
			return it->second;
		}

		for (const auto& preset : g_cachedPresets) {
			std::string presetFileLower = preset.filename;
			std::transform(presetFileLower.begin(), presetFileLower.end(), presetFileLower.begin(), ::tolower);

			std::string presetNameLower = preset.displayName;
			std::transform(presetNameLower.begin(), presetNameLower.end(), presetNameLower.begin(), ::tolower);

			if (presetFileLower == lowerName || presetNameLower == lowerName) {
				return preset;
			}
		}

		return std::nullopt;
	}

	void DialogueGlobalRegistry::LoadGlobalPatches()
	{
		IndexCatRegistry();
		PreCacheQuestGlobals();

		LoadPresetsCache();
		QuestOrderManager::PreloadProfiles();
	}
}