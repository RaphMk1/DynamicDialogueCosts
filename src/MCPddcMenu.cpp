#include "MCPddcMenu.h"
#include "PCH.h"
#include "QOManager.h"
#include "dialogueRegistry.h"
#include "log.h"
#include "settings.h"
#include <SimpleIni.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <set>
#include <unordered_map>

namespace DynamicDialogueCosts
{
	std::atomic<std::shared_ptr<const MenuTextsMCP>> g_menuTexts{ std::make_shared<MenuTextsMCP>() };

	bool        MCPddcMenu::s_isDirty{ false };
	Settings    MCPddcMenu::s_menuSettings{};
	bool        MCPddcMenu::s_bootStateInitialized{ false };
	std::string MCPddcMenu::s_statusMessage{};
	float       MCPddcMenu::s_statusTimer{ 0.0f };
	bool        MCPddcMenu::s_statusIsError{ false };
	int         MCPddcMenu::s_activeTab{ 0 };
	bool        MCPddcMenu::s_confirmRestoreTab{ false };

	std::vector<PresetEntry> MCPddcMenu::s_presets{};
	int                      MCPddcMenu::s_selectedPresetIndex{ -1 };
	char                     MCPddcMenu::s_presetFilter[64]{ "" };
	char                     MCPddcMenu::s_categoryFilter[64]{ "" };
	bool                     MCPddcMenu::s_confirmApplyPresetAll{ false };

	std::vector<std::string> MCPddcMenu::s_cachedActiveCategories{};
	bool                     MCPddcMenu::s_categoryCacheValid{ false };

	bool MCPddcMenu::IsInMainMenu()
	{
		if (auto ui = RE::UI::GetSingleton()) {
			if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
				return true;
			}
		}
		auto player = RE::PlayerCharacter::GetSingleton();
		return !player || !player->Is3DLoaded();
	}

	bool MCPddcMenu::IsDialogueMenuOpen()
	{
		if (auto ui = RE::UI::GetSingleton()) {
			return ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}
		return false;
	}

	static float GetCurrentInGameDays()
	{
		if (auto daysGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("GameDaysPassed")) {
			return std::max(0.0f, daysGlobal->value);
		}
		return 0.0f;
	}

	static float CalculateCategoryCurInfMultiplier(bool a_isAffected, float a_rate, float a_resetDay, float a_maxCap, bool a_invert = false)
	{
		if (!a_isAffected) {
			return 1.0f;
		}

		float currentDays = GetCurrentInGameDays();
		float effectiveDays = std::max(0.0f, currentDays - a_resetDay);
		float rawMultiplier = 1.0f + (effectiveDays * a_rate);

		float cappedMax = std::clamp(a_maxCap, 1.0f, 100.0f);
		float finalMult = a_invert ? (1.0f / rawMultiplier) : rawMultiplier;
		return std::min(finalMult, cappedMax);
	}

	static float GetCurrentPlayerLevel()
	{
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			return static_cast<float>(player->GetLevel());
		}
		return 1.0f;
	}

	static float CalculateCategoryCurLevelMultiplier(bool a_isAffected, float a_rate, bool a_invert = false)
	{
		if (!a_isAffected) {
			return 1.0f;
		}
		float level = GetCurrentPlayerLevel();
		if (level <= 1.0f) {
			return 1.0f;
		}
		float levelFactor = 1.0f + ((level - 1.0f) * a_rate);
		float finalMult = a_invert ? (1.0f / levelFactor) : levelFactor;
		return std::max(0.0f, finalMult);
	}

	void MCPddcMenu::InvalidateCategoryCache()
	{
		s_categoryCacheValid = false;
	}

	const std::vector<std::string>& MCPddcMenu::GetActiveCategories()
	{
		if (s_categoryCacheValid && !s_cachedActiveCategories.empty()) {
			return s_cachedActiveCategories;
		}

		std::set<std::string> rawCategories;

		for (const auto& [global, entry] : DialogueGlobalRegistry::g_cachedCategoryMap) {
			rawCategories.insert(entry.categoryID);
		}
		for (const auto& [levItem, catID] : DialogueGlobalRegistry::g_cachedLeveledListCategoryMap) {
			rawCategories.insert(catID);
		}
		for (const auto& [form, itemData] : DialogueGlobalRegistry::g_cachedItemCategoryMap) {
			rawCategories.insert(itemData.categoryID);
		}
		for (const auto& entry : DialogueGlobalRegistry::g_cachedSettEntries) {
			rawCategories.insert(entry.categoryID);
		}
		for (const auto& [catID, entry] : DialogueGlobalRegistry::g_cachedExternalGlobalCategoryMap) {
			rawCategories.insert(catID);
		}
		for (const auto& [catID, config] : s_menuSettings.categories) {
			rawCategories.insert(catID);
		}

		std::erase_if(rawCategories, [](const std::string& catID) {
			std::string lowerCat = catID;
			std::transform(lowerCat.begin(), lowerCat.end(), lowerCat.begin(), ::tolower);

			auto it = DialogueGlobalRegistry::g_categoryDiskDefaults.find(lowerCat);
			return it != DialogueGlobalRegistry::g_categoryDiskDefaults.end() && it->second.hideCatInMenu;
		});

		struct SortEntry
		{
			std::string id;
			std::string lowerId;
			int         priority;
		};

		std::vector<SortEntry> sortEntries;
		sortEntries.reserve(rawCategories.size());

		for (const auto& catID : rawCategories) {
			std::string lowerCat = catID;
			std::transform(lowerCat.begin(), lowerCat.end(), lowerCat.begin(), ::tolower);

			int priority = 0;
			if (const auto* cat = s_menuSettings.GetCategory(lowerCat)) {
				if (cat->priority != 0)
					priority = cat->priority;
			}
			if (priority == 0) {
				if (auto it = DialogueGlobalRegistry::g_categoryDiskDefaults.find(lowerCat); it != DialogueGlobalRegistry::g_categoryDiskDefaults.end()) {
					priority = it->second.priority;
				}
			}

			sortEntries.push_back({ catID, std::move(lowerCat), priority });
		}

		std::stable_sort(sortEntries.begin(), sortEntries.end(), [](const SortEntry& a, const SortEntry& b) {
			if (a.priority != b.priority) {
				return a.priority > b.priority;
			}
			return a.lowerId < b.lowerId;
		});

		s_cachedActiveCategories.clear();
		s_cachedActiveCategories.reserve(sortEntries.size());
		for (auto& entry : sortEntries) {
			s_cachedActiveCategories.push_back(std::move(entry.id));
		}

		s_categoryCacheValid = true;
		return s_cachedActiveCategories;
	}

	std::string MCPddcMenu::GetCategoryDisplayName(const std::string& a_categoryID, const MenuTextsMCP* a_texts)
	{
		std::string lower = a_categoryID;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

		std::string baseName = a_categoryID;

		auto it = DialogueGlobalRegistry::g_categoryDisplayNames.find(lower);
		if (it != DialogueGlobalRegistry::g_categoryDisplayNames.end()) {
			if (!it->second.empty()) {
				baseName = it->second;
			}
		} else {
			if (!baseName.empty()) {
				baseName[0] = static_cast<char>(std::toupper(baseName[0]));
			}
			baseName += (a_texts ? a_texts->missingCategoryFolder : " [MISSING Category Folder]");
		}

		if (const auto* cat = s_menuSettings.GetCategory(a_categoryID)) {
			if (!cat->displayName.empty()) {
				std::string lowerDisp = cat->displayName;
				std::transform(lowerDisp.begin(), lowerDisp.end(), lowerDisp.begin(), ::tolower);
				if (lowerDisp != lower) {
					baseName = cat->displayName;
				}
			}
		}

		return baseName;
	}

	void MCPddcMenu::SetStatus(const std::string& a_msg, bool a_isError)
	{
		s_statusMessage = a_msg;
		s_statusIsError = a_isError;
		s_statusTimer = 3.0f;
	}

	static float RoundToDecimals(float a_val, int a_decimals)
	{
		float scale = std::pow(10.0f, static_cast<float>(a_decimals));
		return std::round(a_val * scale) / scale;
	}

	static void SetCleanFloatValue(CSimpleIniA& a_ini, const char* a_section, const char* a_key, float a_val, int a_dcdigits = 4)
	{
		std::string formatted = std::format("{:.{}f}", a_val, a_dcdigits);
		if (formatted.find('.') != std::string::npos) {
			formatted.erase(formatted.find_last_not_of('0') + 1);
			if (formatted.back() == '.') {
				formatted.pop_back();
			}
		}
		a_ini.SetValue(a_section, a_key, formatted.c_str());
	}

	void MCPddcMenu::SanitizeSettings(Settings& a_settings)
	{
		a_settings.maxInflationCap = RoundToDecimals(std::clamp(a_settings.maxInflationCap, 1.0f, 100.0f), 2);
		a_settings.maxMultCap = RoundToDecimals(std::clamp(a_settings.maxMultCap, 0.0f, 100000.0f), 2);
		a_settings.debugLevel = std::clamp(a_settings.debugLevel, 0, 5);

		for (auto& [id, cat] : a_settings.categories) {
			cat.multiplier = RoundToDecimals(std::clamp(cat.multiplier, 0.0f, 1000.0f), 2);
			cat.inflationRate = RoundToDecimals(std::clamp(cat.inflationRate, 0.0f, 1.0f), 4);
			cat.inflationResetDay = RoundToDecimals(std::max(0.0f, cat.inflationResetDay), 4);
			cat.levelMult = RoundToDecimals(std::clamp(cat.levelMult, 0.0f, 0.1f), 4);
		}
	}

	void MCPddcMenu::SetSelectedPresetByFilename(const std::string& a_filename)
	{
		if (a_filename.empty()) {
			s_selectedPresetIndex = -1;
			return;
		}

		if (s_presets.empty()) {
			RefreshPresets();
		}

		for (int i = 0; i < static_cast<int>(s_presets.size()); ++i) {
			if (_stricmp(s_presets[i].filename.c_str(), a_filename.c_str()) == 0) {
				s_selectedPresetIndex = i;
				return;
			}
		}
		s_selectedPresetIndex = -1;
	}

	static bool ContainsIgnoreCase(std::string_view a_haystack, std::string_view a_needle)
	{
		if (a_needle.empty())
			return true;
		auto it = std::search(a_haystack.begin(), a_haystack.end(), a_needle.begin(), a_needle.end(),
			[](char ch1, char ch2) {
				return ::tolower(static_cast<unsigned char>(ch1)) == ::tolower(static_cast<unsigned char>(ch2));
			});
		return it != a_haystack.end();
	}

	static bool RenderCategorySliderWithTiers(const char* a_id, float* a_val)
	{
		using ImGuiID = std::uint32_t;
		static std::unordered_map<ImGuiID, float> s_sliderCaps;
		ImGuiID                                   key = ImGuiMCP::GetID(a_id);

		auto it = s_sliderCaps.find(key);
		if (it == s_sliderCaps.end()) {
			it = s_sliderCaps.emplace(key, (*a_val > 100.0f) ? 1000.0f : ((*a_val > 10.0f) ? 100.0f : 10.0f)).first;
		}

		float& cap = it->second;

		if (*a_val > cap) {
			cap = (*a_val > 100.0f) ? 1000.0f : 100.0f;
		} else if (*a_val < cap / 10.0f && cap > 10.0f) {
			cap = (cap >= 1000.0f && *a_val > 10.0f) ? 100.0f : 10.0f;
		}

		float       step = (cap >= 1000.0f) ? 10.0f : ((cap >= 100.0f) ? 1.0f : 0.1f);
		const char* fmt = (step < 1.0f) ? "%.1fx" : "%.0fx";

		bool        changed = false;
		std::string sliderId = std::string("##") + a_id;

		if (ImGuiMCP::SliderFloat(sliderId.c_str(), a_val, 0.0f, cap, fmt)) {
			*a_val = std::round(*a_val / step) * step;
			*a_val = std::clamp(*a_val, 0.0f, 1000.0f);
			changed = true;
		}

		ImGuiMCP::SameLine();
		std::string btn10 = std::string("1x##") + a_id;
		if (ImGuiMCP::Button(btn10.c_str())) {
			cap = 10.0f;
			*a_val = 1.0f;
			changed = true;
		}

		ImGuiMCP::SameLine();
		std::string btn100 = std::string("10x##") + a_id;
		if (ImGuiMCP::Button(btn100.c_str())) {
			cap = 100.0f;
			*a_val = 10.0f;
			changed = true;
		}

		ImGuiMCP::SameLine();
		std::string btn1000 = std::string("100x##") + a_id;
		if (ImGuiMCP::Button(btn1000.c_str())) {
			cap = 1000.0f;
			*a_val = 100.0f;
			changed = true;
		}
		return changed;
	}

	template <typename Func>
	static bool BindUI(Func&& a_imguiFunc)
	{
		if (a_imguiFunc()) {
			MCPddcMenu::s_isDirty = true;
			return true;
		}
		return false;
	}

	static void RenderInflationCategoryRow(
		const std::string& a_categoryBaseName,
		const char*        a_sliderId,
		const char*        a_resetBtnId,
		const char*        a_invertCheckboxId,
		bool*              a_inflateToggle,
		bool*              a_invertToggle,
		float*             a_rateVal,
		float*             a_resetDayVal,
		float              a_maxCap)
	{
		float       currentMult = CalculateCategoryCurInfMultiplier(*a_inflateToggle, *a_rateVal, *a_resetDayVal, a_maxCap, *a_invertToggle);
		std::string displayName = std::format("{} ({:.2f}x)", a_categoryBaseName, currentMult);

		BindUI([&] { return ImGuiMCP::Checkbox(displayName.c_str(), a_inflateToggle); });

		if (*a_inflateToggle) {
			ImGuiMCP::Indent();

			BindUI([&] {
				bool slChanged = ImGuiMCP::SliderFloat(a_sliderId, a_rateVal, 0.0001f, 0.05f, "%.4f/day");
				*a_rateVal = std::clamp(*a_rateVal, 0.0f, 1.0f);
				return slChanged;
			});

			ImGuiMCP::SameLine();

			if (ImGuiMCP::Button(a_resetBtnId)) {
				*a_resetDayVal = GetCurrentInGameDays();
				MCPddcMenu::s_isDirty = true;
			}

			ImGuiMCP::SameLine();
			BindUI([&] { return ImGuiMCP::Checkbox(a_invertCheckboxId, a_invertToggle); });

			ImGuiMCP::Unindent();
		}
	}

	static void RenderLevelCategoryRow(
		const std::string& a_categoryBaseName,
		const char*        a_sliderId,
		const char*        a_invertCheckboxId,
		bool*              a_levelToggle,
		bool*              a_invertToggle,
		float*             a_rateVal)
	{
		float       currentMult = CalculateCategoryCurLevelMultiplier(*a_levelToggle, *a_rateVal, *a_invertToggle);
		std::string displayName = std::format("{} ({:.2f}x)", a_categoryBaseName, currentMult);

		BindUI([&] { return ImGuiMCP::Checkbox(displayName.c_str(), a_levelToggle); });

		if (*a_levelToggle) {
			ImGuiMCP::Indent();
			BindUI([&] {
				bool slChanged = ImGuiMCP::SliderFloat(a_sliderId, a_rateVal, 0.0001f, 0.1f, "%.4f/level");
				*a_rateVal = std::clamp(*a_rateVal, 0.0f, 0.1f);
				return slChanged;
			});
			ImGuiMCP::SameLine();
			BindUI([&] { return ImGuiMCP::Checkbox(a_invertCheckboxId, a_invertToggle); });
			ImGuiMCP::Unindent();
		}
	}

	void LoadMenuTexts()
	{
		CSimpleIniA ini;
		ini.SetUnicode();

		if (ini.LoadFile(kMenuTextsIniPath) >= 0) {
			auto newTexts = std::make_shared<MenuTextsMCP>();

			newTexts->menuConfiguration = ini.GetValue("MenuTexts", "sMenuConfiguration", "Config");
			newTexts->tabStatic = ini.GetValue("MenuTexts", "sTabStatic", "Static Mult");
			newTexts->tabBarter = ini.GetValue("MenuTexts", "sTabBarter", "Barter Mult");
			newTexts->tabInflation = ini.GetValue("MenuTexts", "sTabInflation", "Inflation Mult");
			newTexts->tabLevel = ini.GetValue("MenuTexts", "sTabLevel", "Level Mult");
			newTexts->tabDebug = ini.GetValue("MenuTexts", "sTabDebug", "Misc.");

			newTexts->activePreset = ini.GetValue("MenuTexts", "sActivePreset", "Active Preset:");
			newTexts->dummyPreset = ini.GetValue("MenuTexts", "sDummyPreset", "None");
			newTexts->searchPreset = ini.GetValue("MenuTexts", "sSearchPreset", "Search");
			newTexts->searchCategories = ini.GetValue("MenuTexts", "sSearchCategories", "Search Categories");
			newTexts->maxInflationCapHeader = ini.GetValue("MenuTexts", "sMaxInflationCapHeader", "Inflation Mult Cap:");
			newTexts->totalFinalCapHeader = ini.GetValue("MenuTexts", "sTotalFinalCapHeader", "Combined Mult Cap:");
			newTexts->btnYesLoadAll = ini.GetValue("MenuTexts", "sBtnYesLoadAll", "Yes, Load All Tabs");
			newTexts->selectProfile = ini.GetValue("MenuTexts", "sSelectProfile", "Select Profile...");
			newTexts->searchProfile = ini.GetValue("MenuTexts", "sSearchProfile", "Search");

			newTexts->scaleCatValues = ini.GetValue("MenuTexts", "sScaleCatValues", "Enable Value Scaling");
			newTexts->categoriesHeader = ini.GetValue("MenuTexts", "sCategoriesHeader", "Select affected categories and set their base static multipliers:");

			newTexts->enableDynamicScaling = ini.GetValue("MenuTexts", "sEnableDynamicScaling", "Enable Barter Rates Mult");
			newTexts->includePowerModifiers = ini.GetValue("MenuTexts", "sIncludePowerModifiers", "Include Gear|Potion|Blessings");
			newTexts->includePerkModifiers = ini.GetValue("MenuTexts", "sIncludePerkModifiers", "Include Perk Entries");
			newTexts->barterCategoriesHeader = ini.GetValue("MenuTexts", "sBarterCategoriesHeader", "Select categories affected by buy|sell barter rates:");

			newTexts->enableInflation = ini.GetValue("MenuTexts", "sEnableInflation", "Enable Inflation");
			newTexts->inflationCategoriesHeader = ini.GetValue("MenuTexts", "sInflationCategoriesHeader", "Select affected categories, then set their daily inflation rates:");

			newTexts->enableLevelScaling = ini.GetValue("MenuTexts", "sEnableLevelScaling", "Enable Level Scaling");
			newTexts->levelCategoriesHeader = ini.GetValue("MenuTexts", "sLevelCategoriesHeader", "Select affected categories and set their scaling rates per level:");
			newTexts->invertLevelMult = ini.GetValue("MenuTexts", "sInvertLevelMult", "Invert");

			newTexts->useQuestWatcher = ini.GetValue("MenuTexts", "sUseQuestWatcher", "Enable Quest Control System");
			newTexts->activeQuestOrderHeader = ini.GetValue("MenuTexts", "sActiveQuestOrderHeader", "Active Quest Control Profile:");

			newTexts->enableDebug = ini.GetValue("MenuTexts", "sEnableDebug", "Enable Debug Mode");
			newTexts->logLevel = ini.GetValue("MenuTexts", "sLogLevel", "Log Level");
			newTexts->hideCategories = ini.GetValue("MenuTexts", "sHideCategories", "Hide and lock menu");
			newTexts->btnReloadCategories = ini.GetValue("MenuTexts", "sBtnReloadCategories", "Reload Category Files and Apply Current Settings");
			newTexts->btnSaveDefaults = ini.GetValue("MenuTexts", "sBtnSaveDefaults", "Save as NG Defaults");

			newTexts->unsavedPending = ini.GetValue("MenuTexts", "sUnsavedPending", "* Unsaved Changes");
			newTexts->btnApply = ini.GetValue("MenuTexts", "sBtnApply", "Apply");
			newTexts->btnDiscard = ini.GetValue("MenuTexts", "sBtnDiscard", "Discard");
			newTexts->btnRestore = ini.GetValue("MenuTexts", "sBtnRestore", "Restore Tab Preset");
			newTexts->btnReset = ini.GetValue("MenuTexts", "sBtnReset", "Reset");
			newTexts->confirmRestore = ini.GetValue("MenuTexts", "sConfirmRestore", "Are you sure?");
			newTexts->btnYesRestore = ini.GetValue("MenuTexts", "sBtnYesRestore", "Yes");
			newTexts->btnCancel = ini.GetValue("MenuTexts", "sBtnCancel", "Cancel");
			newTexts->savedSuccess = ini.GetValue("MenuTexts", "sSavedSuccess", "Saved!");

			newTexts->missingCategoryFolder = ini.GetValue("MenuTexts", "sMissingCategoryFolder", " [MISSING Category Folder]");

			newTexts->statusCategoryHidingLocked = ini.GetValue("MenuTexts", "sStatusCategoryHidingLocked", "Category hiding locked!");
			newTexts->statusCategoryHidingUnlocked = ini.GetValue("MenuTexts", "sStatusCategoryHidingUnlocked", "Category hiding unlocked!");
			newTexts->statusFailedLoadPreset = ini.GetValue("MenuTexts", "sStatusFailedLoadPreset", "Failed to load preset INI!");
			newTexts->confirmApplyPreset = ini.GetValue("MenuTexts", "sConfirmApplyPreset", "Apply selected preset to all tabs?");
			newTexts->statusLoadedPresetAll = ini.GetValue("MenuTexts", "sStatusLoadedPresetAll", "Loaded preset to all tabs!");
			newTexts->statusAppliedQuestOrder = ini.GetValue("MenuTexts", "sStatusAppliedQuestOrder", "Applied Quest Order profile to active session!");
			newTexts->statusLoadedQuestOrder = ini.GetValue("MenuTexts", "sStatusLoadedQuestOrder", "Loaded Quest Order:");
			newTexts->statusErrDialogueOpenReload = ini.GetValue("MenuTexts", "sStatusErrDialogueOpenReload", "Cannot reload category files while in Dialogue!");
			newTexts->statusErrDialogueOpenApply = ini.GetValue("MenuTexts", "sStatusErrDialogueOpenApply", "Cannot apply settings while in Dialogue!");

			g_menuTexts.store(newTexts);
		}
	}

	void LogMenuFrameworkStatus(const std::string& a_message, bool a_isError)
	{
		if (a_isError) {
			SKSE::log::error("[Menu] {}", a_message);
		} else {
			SKSE::log::info("[Menu] {}", a_message);
		}
	}

	void EnsureBackupIniExists()
	{
		namespace fs = std::filesystem;
		try {
			if (!fs::exists(kPresetsDirPath)) {
				fs::create_directories(kPresetsDirPath);
			}
			if (!fs::exists(kVanillaPresetPath)) {
				if (fs::exists(kIniPath)) {
					fs::copy_file(kIniPath, kVanillaPresetPath, fs::copy_options::overwrite_existing);
					LogMenuFrameworkStatus("Created default vanilla backup preset INI in Presets folder.");
				}
			}
		} catch (const std::exception& e) {
			LogMenuFrameworkStatus(std::format("Failed to create vanilla backup INI copy: {}", e.what()), true);
		}
	}

	void SaveSettings(const Settings& cfg)
	{
		EnsureBackupIniExists();

		Settings sanitizedCfg = cfg;
		MCPddcMenu::SanitizeSettings(sanitizedCfg);

		for (const auto& catID : MCPddcMenu::GetActiveCategories()) {
			sanitizedCfg.GetOrCreateCategory(catID);
		}

		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(kIniPath);

		ini.SetBoolValue("Settings", "bScaleValues", sanitizedCfg.scaleCatValues);
		ini.SetBoolValue("Settings", "bEnableBarterMult", sanitizedCfg.enableDynamicScaling);
		ini.SetBoolValue("Settings", "bIncludePowerModifiers", sanitizedCfg.includePowerModifiers);
		ini.SetBoolValue("Settings", "bIncludePerkModifiers", sanitizedCfg.includePerkModifiers);
		ini.SetBoolValue("Settings", "bEnableInflation", sanitizedCfg.enableInflation);
		SetCleanFloatValue(ini, "Settings", "fMaxInflationCap", sanitizedCfg.maxInflationCap, 2);
		ini.SetBoolValue("Settings", "bEnableLevelScaling", sanitizedCfg.enableLevelScaling);
		ini.SetBoolValue("Settings", "bUseQuestWatcher", sanitizedCfg.useQuestWatcher);
		ini.SetValue("Settings", "sQuestOrderProfile", sanitizedCfg.questOrderProfile.c_str());
		SetCleanFloatValue(ini, "Settings", "fMaxMultCap", sanitizedCfg.maxMultCap, 2);

		ini.SetBoolValue("Settings", "bHideCategories", sanitizedCfg.hideCategories);
		ini.SetBoolValue("Settings", "bEnableDebugMode", sanitizedCfg.enableDebug);
		ini.SetLongValue("Settings", "iDebugLevel", static_cast<long>(sanitizedCfg.debugLevel));
		ini.SetBoolValue("Settings", "bUseActivateHandler", sanitizedCfg.useActivateHandler);

		for (const auto& [catID, catConfig] : sanitizedCfg.categories) {
			std::string sec = "Category_" + catID;

			if (!catConfig.displayName.empty()) {
				ini.SetValue(sec.c_str(), "sDisplayName", catConfig.displayName.c_str());
			}
			ini.SetBoolValue(sec.c_str(), "bScaleValues", catConfig.enabled);
			SetCleanFloatValue(ini, sec.c_str(), "fStaticMultiplier", catConfig.multiplier, 2);
			ini.SetBoolValue(sec.c_str(), "bEnableBarterMult", catConfig.dynamicScaling);
			ini.SetBoolValue(sec.c_str(), "bPersistLeveledLists", catConfig.persistentLeveledLists);
			ini.SetBoolValue(sec.c_str(), "bUsesSellBarterMult", catConfig.usesSellBarterMult);
			ini.SetBoolValue(sec.c_str(), "bHideCatInMenu", catConfig.hideCatInMenu);
			ini.SetBoolValue(sec.c_str(), "bEnableInflation", catConfig.enableInflation);
			ini.SetBoolValue(sec.c_str(), "bInvertInflation", catConfig.invertInflation);
			SetCleanFloatValue(ini, sec.c_str(), "fInflationRate", catConfig.inflationRate, 4);
			SetCleanFloatValue(ini, sec.c_str(), "fInflationResetDay", catConfig.inflationResetDay, 4);
			ini.SetBoolValue(sec.c_str(), "bLevelScale", catConfig.levelScale);
			ini.SetBoolValue(sec.c_str(), "bInvertLevelMult", catConfig.invertLevelMult);
			SetCleanFloatValue(ini, sec.c_str(), "fLevelMult", catConfig.levelMult, 4);
		}

		ini.SaveFile(kIniPath);
	}

	bool MCPddcMenu::Register()
	{
		EnsureBackupIniExists();

		if (!SKSEMenuFramework::IsInstalled()) {
			if (auto console = RE::ConsoleLog::GetSingleton()) {
				console->Print("DynamicDialogueCosts: SKSE Menu Framework not found, in-game menu disabled.");
			}
			LogMenuFrameworkStatus("SKSE Menu Framework was not found. Menu Registration skipped.");
			return false;
		}

		SKSEMenuFramework::SetSection("Dynamic Dialogue Costs");

		auto               txt = g_menuTexts.load();
		static std::string s_menuLabel = txt ? txt->menuConfiguration : "Config";

		SKSEMenuFramework::AddSectionItem(s_menuLabel.c_str(), RenderMenu);
		SKSEMenuFramework::AddEvent(OnMenuOpen, 0.0f);
		LogMenuFrameworkStatus("Dynamic Dialogue Costs Menu successfully registered with SKSE Menu Framework.");
		return true;
	}

	bool MCPddcMenu::IsCategoryHidingLocked()
	{
		auto cfg = g_settings.load();
		return s_hideCategoriesLocked.load() || (cfg && cfg->hideCategories);
	}

	void MCPddcMenu::SetCategoryHiding(bool a_hideAndLock, bool a_syncToGlobalSettings)
	{
		s_hideCategoriesLocked.store(a_hideAndLock);
		s_menuSettings.hideCategories = a_hideAndLock;

		if (a_syncToGlobalSettings) {
			if (auto cfg = g_settings.load()) {
				auto updatedSettings = std::make_shared<Settings>(*cfg);
				updatedSettings->hideCategories = a_hideAndLock;
				g_settings.store(updatedSettings);
			}
		}
	}

	std::vector<QuestOrderEntry> MCPddcMenu::s_questOrders{};
	int                          MCPddcMenu::s_selectedQuestOrderIndex{ 0 };
	char                         MCPddcMenu::s_questOrderFilter[64]{ "" };

	void MCPddcMenu::RefreshQuestOrders()
	{
		s_questOrders.clear();
		auto cachedProfiles = QuestOrderManager::GetLoadedProfiles();

		for (const auto& profile : cachedProfiles) {
			QuestOrderEntry qe;
			qe.filename = profile.filename;
			qe.displayName = profile.profileName.empty() ? profile.filename : profile.profileName;
			qe.profileID = profile.profileID;
			s_questOrders.push_back(qe);
		}

		s_selectedQuestOrderIndex = 0;
		bool foundMatch = false;

		for (int i = 0; i < static_cast<int>(s_questOrders.size()); ++i) {
			auto        profileData = QuestOrderManager::GetProfileData(s_questOrders[i].filename);
			std::string effectiveID = !s_questOrders[i].profileID.empty() ? s_questOrders[i].profileID : s_questOrders[i].filename;

			if (_stricmp(s_questOrders[i].filename.c_str(), s_menuSettings.questOrderProfile.c_str()) == 0 ||
				_stricmp(s_questOrders[i].displayName.c_str(), s_menuSettings.questOrderProfile.c_str()) == 0 ||
				(!profileData.profileID.empty() && _stricmp(profileData.profileID.c_str(), s_menuSettings.questOrderProfile.c_str()) == 0) ||
				_stricmp(effectiveID.c_str(), s_menuSettings.questOrderProfile.c_str()) == 0) {
				s_selectedQuestOrderIndex = i;
				s_menuSettings.questOrderProfile = effectiveID;
				foundMatch = true;
				break;
			}
		}

		if (!foundMatch && !s_questOrders.empty()) {
			s_menuSettings.questOrderProfile = !s_questOrders[0].profileID.empty() ? s_questOrders[0].profileID : s_questOrders[0].filename;
		}
	}

	void __stdcall MCPddcMenu::OnMenuOpen(SKSEMenuFramework::Model::EventType a_event)
	{
		if (a_event == SKSEMenuFramework::Model::EventType::kOpenMenu) {
			//InvalidateCategoryCache();
			ResetUIBufferFromMemory();
		}
	}

	void MCPddcMenu::ResetUIBufferFromMemory()
	{
		InvalidateCategoryCache();

		if (auto current = g_settings.load()) {
			s_menuSettings = *current;
			SanitizeSettings(s_menuSettings);
		}

		for (const auto& catID : GetActiveCategories()) {
			s_menuSettings.GetOrCreateCategory(catID);
		}

		s_isDirty = false;
		s_confirmApplyPresetAll = false;
		RefreshQuestOrders();
		RefreshPresets();
	}

	bool MCPddcMenu::IsDirty()
	{
		return s_isDirty;
	}

	void MCPddcMenu::RefreshPresets()
	{
		s_presets.clear();
		auto cachedPresets = DialogueGlobalRegistry::GetCachedPresets();

		for (const auto& cached : cachedPresets) {
			PresetEntry p;
			p.filename = cached.filename;
			p.filepath = cached.filepath.wstring();
			p.displayName = cached.displayName;
			p.priority = cached.priority;
			s_presets.push_back(p);
		}

		if (s_selectedPresetIndex < -1 || s_selectedPresetIndex >= static_cast<int>(s_presets.size())) {
			s_selectedPresetIndex = -1;
		}
	}

	void MCPddcMenu::RestoreFromIni(const std::string& a_presetFilename, int a_activeTab, bool a_allTabs)
	{
		auto txt = g_menuTexts.load();

		auto cachedOpt = DialogueGlobalRegistry::GetCachedPreset(a_presetFilename);
		if (!cachedOpt || !cachedOpt->iniData) {
			SetStatus(txt ? txt->statusFailedLoadPreset : "Failed to load preset INI!", true);
			return;
		}

		const auto& presetIni = cachedOpt->iniData;

		InvalidateCategoryCache();
		const auto& activeCats = GetActiveCategories();

		auto GetCatSection = [&](const std::string& catID) -> std::string {
			std::string prefixed = "Category_" + catID;
			if (presetIni->GetSection(prefixed.c_str())) {
				return prefixed;
			}
			return catID;
		};

		auto restoreStatic = [&]() {
			s_menuSettings.scaleCatValues = presetIni->GetBoolValue("Settings", "bScaleValues", true);
			for (const auto& catID : activeCats) {
				auto&       cat = s_menuSettings.GetOrCreateCategory(catID);
				std::string sec = GetCatSection(catID);
				cat.enabled = presetIni->GetBoolValue(sec.c_str(), "bScaleValues", cat.enabled);
				cat.multiplier = std::clamp(static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fStaticMultiplier", cat.multiplier)), 0.0f, 1000.0f);
				cat.dynamicScaling = presetIni->GetBoolValue(sec.c_str(), "bEnableBarterMult", cat.dynamicScaling);
				cat.enableInflation = presetIni->GetBoolValue(sec.c_str(), "bEnableInflation", cat.enableInflation);
				cat.inflationRate = std::clamp(static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fInflationRate", cat.inflationRate)), 0.0f, 1.0f);
				cat.inflationResetDay = static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fInflationResetDay", cat.inflationResetDay));
				cat.levelScale = presetIni->GetBoolValue(sec.c_str(), "bLevelScale", cat.levelScale);
				cat.invertLevelMult = presetIni->GetBoolValue(sec.c_str(), "bInvertLevelMult", cat.invertLevelMult);
				cat.levelMult = std::clamp(static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fLevelMult", cat.levelMult)), 0.0f, 0.1f);
			}
		};

		auto restoreBarter = [&]() {
			s_menuSettings.enableDynamicScaling = presetIni->GetBoolValue("Settings", "bEnableBarterMult", true);
			s_menuSettings.includePerkModifiers = presetIni->GetBoolValue("Settings", "bIncludePerkModifiers", true);
			s_menuSettings.includePowerModifiers = presetIni->GetBoolValue("Settings", "bIncludePowerModifiers", true);
			for (const auto& catID : activeCats) {
				auto&       cat = s_menuSettings.GetOrCreateCategory(catID);
				std::string sec = GetCatSection(catID);
				cat.dynamicScaling = presetIni->GetBoolValue(sec.c_str(), "bEnableBarterMult", cat.dynamicScaling);
			}
		};

		auto restoreInflation = [&]() {
			s_menuSettings.enableInflation = presetIni->GetBoolValue("Settings", "bEnableInflation", false);
			s_menuSettings.maxInflationCap = std::clamp(static_cast<float>(presetIni->GetDoubleValue("Settings", "fMaxInflationCap", 100.0)), 1.0f, 100.0f);
			for (const auto& catID : activeCats) {
				auto&       cat = s_menuSettings.GetOrCreateCategory(catID);
				std::string sec = GetCatSection(catID);
				cat.enableInflation = presetIni->GetBoolValue(sec.c_str(), "bEnableInflation", cat.enableInflation);
				cat.inflationRate = std::clamp(static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fInflationRate", cat.inflationRate)), 0.0f, 1.0f);
				cat.inflationResetDay = static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fInflationResetDay", cat.inflationResetDay));
			}
		};

		auto restoreLevel = [&]() {
			s_menuSettings.enableLevelScaling = presetIni->GetBoolValue("Settings", "bEnableLevelScaling", true);
			for (const auto& catID : activeCats) {
				auto&       cat = s_menuSettings.GetOrCreateCategory(catID);
				std::string sec = GetCatSection(catID);
				cat.levelScale = presetIni->GetBoolValue(sec.c_str(), "bLevelScale", cat.levelScale);
				cat.invertLevelMult = presetIni->GetBoolValue(sec.c_str(), "bInvertLevelMult", cat.invertLevelMult);
				cat.levelMult = std::clamp(static_cast<float>(presetIni->GetDoubleValue(sec.c_str(), "fLevelMult", cat.levelMult)), 0.0f, 0.1f);
			}
		};

		auto restoreDebug = [&]() {
			s_menuSettings.maxMultCap = std::clamp(static_cast<float>(presetIni->GetDoubleValue("Settings", "fMaxMultCap", 10000.0)), 0.0f, 100000.0f);
			s_menuSettings.enableDebug = presetIni->GetBoolValue("Settings", "bEnableDebugMode", true);
			s_menuSettings.debugLevel = std::clamp(static_cast<int>(presetIni->GetLongValue("Settings", "iDebugLevel", 2)), 0, 5);
		};

		if (a_allTabs) {
			restoreStatic();
			restoreBarter();
			restoreInflation();
			restoreLevel();
			restoreDebug();
		} else {
			if (a_activeTab == 0)
				restoreStatic();
			else if (a_activeTab == 1)
				restoreBarter();
			else if (a_activeTab == 2)
				restoreInflation();
			else if (a_activeTab == 3)
				restoreLevel();
			else if (a_activeTab == 4)
				restoreDebug();
		}

		SanitizeSettings(s_menuSettings);
		s_isDirty = true;
	}

	void MCPddcMenu::RenderMenu()
	{
		// Redundant, but only runs once
		if (!s_bootStateInitialized) {
			ResetUIBufferFromMemory();
			s_bootStateInitialized = true;
			LogMenuFrameworkStatus("Mb started (first menu open).");
		}

		auto txt = g_menuTexts.load();
		if (!txt) {
			txt = std::make_shared<MenuTextsMCP>();
		}

		if (s_presets.empty()) {
			RefreshPresets();
		}

		const bool inMainMenu = IsInMainMenu();
		const bool effectiveHideCategories = IsCategoryHidingLocked() && !inMainMenu;

		if (!effectiveHideCategories) {
			static int      s_previousPresetIndex = -1;
			static Settings s_presetSnapshotSettings{};
			static bool     s_previousIsDirty = false;

			ImGuiMCP::Text(txt->activePreset.c_str());
			ImGuiMCP::SameLine();

			std::string currentLabel = (s_selectedPresetIndex >= 0 && s_selectedPresetIndex < static_cast<int>(s_presets.size())) ? s_presets[s_selectedPresetIndex].displayName : txt->dummyPreset;

			if (ImGuiMCP::BeginCombo("##PresetCombo", currentLabel.c_str())) {
				std::string searchPresetLabel = txt->searchPreset + "##PresetFilter";
				ImGuiMCP::InputText(searchPresetLabel.c_str(), s_presetFilter, sizeof(s_presetFilter));
				ImGuiMCP::Separator();

				bool showDummyInList = (s_selectedPresetIndex < 0) || (s_confirmApplyPresetAll && s_previousPresetIndex < 0);
				if (showDummyInList) {
					bool isDummySelected = (s_selectedPresetIndex < 0);
					if (ImGuiMCP::Selectable(txt->dummyPreset.c_str(), isDummySelected)) {
						s_selectedPresetIndex = -1;
						s_confirmApplyPresetAll = false;
					}
					if (isDummySelected) {
						ImGuiMCP::SetItemDefaultFocus();
					}
				}

				std::string filterStr = s_presetFilter;
				std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

				for (int i = 0; i < static_cast<int>(s_presets.size()); ++i) {
					std::string itemLabel = s_presets[i].displayName;
					std::string itemLower = itemLabel;
					std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), ::tolower);

					if (!filterStr.empty() && itemLower.find(filterStr) == std::string::npos) {
						continue;
					}

					bool isSelected = (s_selectedPresetIndex == i);
					if (ImGuiMCP::Selectable(itemLabel.c_str(), isSelected)) {
						if (s_selectedPresetIndex != i) {
							if (!s_confirmApplyPresetAll) {
								s_previousPresetIndex = s_selectedPresetIndex;
								s_presetSnapshotSettings = s_menuSettings;
								s_previousIsDirty = s_isDirty;
							}
							s_selectedPresetIndex = i;
							s_confirmApplyPresetAll = true;
							RestoreFromIni(s_presets[s_selectedPresetIndex].filename, 0, true);
						}
					}
					if (isSelected) {
						ImGuiMCP::SetItemDefaultFocus();
					}
				}
				ImGuiMCP::EndCombo();
			}

			if (s_confirmApplyPresetAll && s_selectedPresetIndex >= 0) {
				ImGuiMCP::Spacing();
				ImGuiMCP::TextColored({ 1.0f, 0.6f, 0.2f, 1.0f }, txt->confirmApplyPreset.c_str());
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button(txt->btnYesLoadAll.c_str())) {
					s_confirmApplyPresetAll = false;
					SetStatus(txt->statusLoadedPresetAll, false);
				}
				ImGuiMCP::SameLine();
				std::string cancelPresetAllLabel = txt->btnCancel + "##PresetAll";
				if (ImGuiMCP::Button(cancelPresetAllLabel.c_str())) {
					s_selectedPresetIndex = s_previousPresetIndex;
					s_menuSettings = s_presetSnapshotSettings;
					s_isDirty = s_previousIsDirty;
					s_confirmApplyPresetAll = false;
				}
			}

			ImGuiMCP::Spacing();
			ImGuiMCP::Separator();
			ImGuiMCP::Spacing();
		}

		const auto& activeCategories = GetActiveCategories();

		if (ImGuiMCP::BeginTabBar("DDC-TabBar")) {
			if (!effectiveHideCategories && ImGuiMCP::BeginTabItem(txt->tabStatic.c_str())) {
				s_activeTab = 0;
				ImGuiMCP::Spacing();

				BindUI([&] { return ImGuiMCP::Checkbox(txt->scaleCatValues.c_str(), &s_menuSettings.scaleCatValues); });

				if (s_menuSettings.scaleCatValues) {
					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					std::string searchCatStaticLabel = txt->searchCategories + "##CatFilter";
					ImGuiMCP::InputText(searchCatStaticLabel.c_str(), s_categoryFilter, sizeof(s_categoryFilter));
					ImGuiMCP::Spacing();
					ImGuiMCP::Text(txt->categoriesHeader.c_str());
					ImGuiMCP::Spacing();

					std::string filterStr = s_categoryFilter;
					std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

					for (const auto& catID : activeCategories) {
						auto&       catConfig = s_menuSettings.GetOrCreateCategory(catID);
						std::string label = GetCategoryDisplayName(catID, txt.get());

						std::string labelLower = label;
						std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);

						if (!filterStr.empty() && labelLower.find(filterStr) == std::string::npos) {
							continue;
						}

						BindUI([&] { return ImGuiMCP::Checkbox(label.c_str(), &catConfig.enabled); });
						if (catConfig.enabled) {
							ImGuiMCP::Indent();
							BindUI([&] { return RenderCategorySliderWithTiers(catID.c_str(), &catConfig.multiplier); });
							ImGuiMCP::Unindent();
						}
					}
				}

				ImGuiMCP::EndTabItem();
			}

			if (!effectiveHideCategories && ImGuiMCP::BeginTabItem(txt->tabBarter.c_str())) {
				s_activeTab = 1;
				ImGuiMCP::Spacing();

				BindUI([&] { return ImGuiMCP::Checkbox(txt->enableDynamicScaling.c_str(), &s_menuSettings.enableDynamicScaling); });

				if (s_menuSettings.enableDynamicScaling) {
					ImGuiMCP::Indent();
					BindUI([&] { return ImGuiMCP::Checkbox(txt->includePerkModifiers.c_str(), &s_menuSettings.includePerkModifiers); });
					BindUI([&] { return ImGuiMCP::Checkbox(txt->includePowerModifiers.c_str(), &s_menuSettings.includePowerModifiers); });
					ImGuiMCP::Unindent();

					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					std::string searchCatBarterLabel = txt->searchCategories + "##CatFilterBarter";
					ImGuiMCP::InputText(searchCatBarterLabel.c_str(), s_categoryFilter, sizeof(s_categoryFilter));
					ImGuiMCP::Spacing();
					ImGuiMCP::Text(txt->barterCategoriesHeader.c_str());
					ImGuiMCP::Spacing();

					std::string filterStr = s_categoryFilter;
					std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

					for (const auto& catID : activeCategories) {
						auto& catConfig = s_menuSettings.GetOrCreateCategory(catID);
						if (!catConfig.enabled)
							continue;
						std::string label = GetCategoryDisplayName(catID, txt.get());

						std::string labelLower = label;
						std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);

						if (!filterStr.empty() && labelLower.find(filterStr) == std::string::npos) {
							continue;
						}

						BindUI([&] { return ImGuiMCP::Checkbox(label.c_str(), &catConfig.dynamicScaling); });
					}
				}

				ImGuiMCP::EndTabItem();
			}

			if (!effectiveHideCategories && ImGuiMCP::BeginTabItem(txt->tabInflation.c_str())) {
				s_activeTab = 2;
				ImGuiMCP::Spacing();

				BindUI([&] { return ImGuiMCP::Checkbox(txt->enableInflation.c_str(), &s_menuSettings.enableInflation); });

				if (s_menuSettings.enableInflation) {
					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					ImGuiMCP::Text(txt->maxInflationCapHeader.c_str());
					ImGuiMCP::SameLine();
					BindUI([&] {
						bool changed = ImGuiMCP::SliderFloat("##MaxInflationCap", &s_menuSettings.maxInflationCap, 1.0f, 100.0f, "%.1fx");
						s_menuSettings.maxInflationCap = std::clamp(s_menuSettings.maxInflationCap, 1.0f, 100.0f);
						return changed;
					});

					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					std::string searchCatInfLabel = txt->searchCategories + "##CatFilterInf";
					ImGuiMCP::InputText(searchCatInfLabel.c_str(), s_categoryFilter, sizeof(s_categoryFilter));
					ImGuiMCP::Spacing();
					ImGuiMCP::Text(txt->inflationCategoriesHeader.c_str());
					ImGuiMCP::Spacing();

					std::string filterStr = s_categoryFilter;
					std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

					for (const auto& catID : activeCategories) {
						auto& catConfig = s_menuSettings.GetOrCreateCategory(catID);
						if (!catConfig.enabled)
							continue;
						std::string label = GetCategoryDisplayName(catID, txt.get());

						std::string labelLower = label;
						std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);

						if (!filterStr.empty() && labelLower.find(filterStr) == std::string::npos) {
							continue;
						}

						std::string rateId = "##InfRate_" + catID;
						std::string resetId = txt->btnReset + "##Inf_" + catID;
						std::string invertId = txt->invertLevelMult + "##InfInvert_" + catID;

						RenderInflationCategoryRow(
							label,
							rateId.c_str(),
							resetId.c_str(),
							invertId.c_str(),
							&catConfig.enableInflation,
							&catConfig.invertInflation,
							&catConfig.inflationRate,
							&catConfig.inflationResetDay,
							s_menuSettings.maxInflationCap);
					}
				}

				ImGuiMCP::EndTabItem();
			}

			if (!effectiveHideCategories && ImGuiMCP::BeginTabItem(txt->tabLevel.c_str())) {
				s_activeTab = 3;
				ImGuiMCP::Spacing();

				BindUI([&] { return ImGuiMCP::Checkbox(txt->enableLevelScaling.c_str(), &s_menuSettings.enableLevelScaling); });

				if (s_menuSettings.enableLevelScaling) {
					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					std::string searchCatLvlLabel = txt->searchCategories + "##CatFilterLvl";
					ImGuiMCP::InputText(searchCatLvlLabel.c_str(), s_categoryFilter, sizeof(s_categoryFilter));
					ImGuiMCP::Spacing();
					ImGuiMCP::Text(txt->levelCategoriesHeader.c_str());
					ImGuiMCP::Spacing();

					std::string filterStr = s_categoryFilter;
					std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

					for (const auto& catID : activeCategories) {
						auto& catConfig = s_menuSettings.GetOrCreateCategory(catID);
						if (!catConfig.enabled)
							continue;
						std::string label = GetCategoryDisplayName(catID, txt.get());

						std::string labelLower = label;
						std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);

						if (!filterStr.empty() && labelLower.find(filterStr) == std::string::npos) {
							continue;
						}

						std::string rateId = "##LvlRate_" + catID;
						std::string invertId = txt->invertLevelMult + "##LvlInvert_" + catID;

						RenderLevelCategoryRow(
							label,
							rateId.c_str(),
							invertId.c_str(),
							&catConfig.levelScale,
							&catConfig.invertLevelMult,
							&catConfig.levelMult);
					}
				}

				ImGuiMCP::EndTabItem();
			}

			if (ImGuiMCP::BeginTabItem(txt->tabDebug.c_str())) {
				s_activeTab = 4;
				ImGuiMCP::Spacing();

				if (!effectiveHideCategories) {
					BindUI([&] { return ImGuiMCP::Checkbox(txt->useQuestWatcher.c_str(), &s_menuSettings.useQuestWatcher); });
					if (s_menuSettings.useQuestWatcher) {
						ImGuiMCP::Indent();
						ImGuiMCP::Text(txt->activeQuestOrderHeader.c_str());
						ImGuiMCP::SameLine();

						std::string currentQoLabel = (s_selectedQuestOrderIndex >= 0 && s_selectedQuestOrderIndex < static_cast<int>(s_questOrders.size())) ? s_questOrders[s_selectedQuestOrderIndex].displayName : txt->selectProfile;

						if (ImGuiMCP::BeginCombo("##QuestOrderCombo", currentQoLabel.c_str())) {
							std::string searchQoLabel = txt->searchProfile + "##QOFilter";
							ImGuiMCP::InputText(searchQoLabel.c_str(), s_questOrderFilter, sizeof(s_questOrderFilter));
							ImGuiMCP::Separator();

							std::string filterStr = s_questOrderFilter;
							std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

							for (int i = 0; i < static_cast<int>(s_questOrders.size()); ++i) {
								std::string itemLabel = s_questOrders[i].displayName;
								std::string itemLower = itemLabel;
								std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), ::tolower);

								if (!filterStr.empty() && itemLower.find(filterStr) == std::string::npos) {
									continue;
								}

								bool isSelected = (s_selectedQuestOrderIndex == i);
								if (ImGuiMCP::Selectable(itemLabel.c_str(), isSelected)) {
									std::string effectiveID = !s_questOrders[i].profileID.empty() ? s_questOrders[i].profileID : s_questOrders[i].filename;
									if (s_selectedQuestOrderIndex != i || s_menuSettings.questOrderProfile != effectiveID) {
										s_selectedQuestOrderIndex = i;
										s_menuSettings.questOrderProfile = effectiveID;
										s_isDirty = true;
									}
								}
								if (isSelected) {
									ImGuiMCP::SetItemDefaultFocus();
								}
							}
							ImGuiMCP::EndCombo();
						}
						ImGuiMCP::Unindent();
					}

					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					ImGuiMCP::Text(txt->totalFinalCapHeader.c_str());
					ImGuiMCP::SameLine();
					BindUI([&] {
						return ImGuiMCP::SliderFloat("##MaxMultCap", &s_menuSettings.maxMultCap, 0.0f, 10000.0f, "%.0fx");
					});

					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();
				}

				BindUI([&] { return ImGuiMCP::Checkbox(txt->enableDebug.c_str(), &s_menuSettings.enableDebug); });

				if (s_menuSettings.enableDebug) {
					ImGuiMCP::Indent();
					static const char* logLevelLabels[] = { "0 - OFF", "1 - ERROR", "2 - WARNING", "3 - INFO", "4 - DEBUG", "5 - TRACE" };
					s_menuSettings.debugLevel = std::clamp(s_menuSettings.debugLevel, 0, 5);
					BindUI([&] {
						std::string logLevelLbText = txt->logLevel + "##DebugLevel";
						bool        changed = ImGuiMCP::SliderInt(logLevelLbText.c_str(), &s_menuSettings.debugLevel, 0, 5, logLevelLabels[s_menuSettings.debugLevel]);
						s_menuSettings.debugLevel = std::clamp(s_menuSettings.debugLevel, 0, 5);
						return changed;
					});
					ImGuiMCP::Unindent();
				}

				if (!inMainMenu) {
					ImGuiMCP::Spacing();
					ImGuiMCP::Separator();
					ImGuiMCP::Spacing();

					ImGuiMCP::BeginDisabled(s_hideCategoriesLocked);

					BindUI([&] { return ImGuiMCP::Checkbox(txt->hideCategories.c_str(), &s_menuSettings.hideCategories); });

					ImGuiMCP::EndDisabled();
				}
				ImGuiMCP::Spacing();
				ImGuiMCP::Separator();
				ImGuiMCP::Spacing();

				if (ImGuiMCP::Button(txt->btnReloadCategories.c_str())) {
					if (IsDialogueMenuOpen()) {
						SetStatus(txt->statusErrDialogueOpenReload, true);
					} else {
						SanitizeSettings(s_menuSettings);
						if (s_menuSettings.hideCategories) {
							MCPddcMenu::SetCategoryHiding(true, false);
						}
						DialoguePriceTransformer::ResetPersistentToBaseline();
						g_settings.store(std::make_shared<Settings>(s_menuSettings));

						DialogueGlobalRegistry::LoadGlobalPatches();
						RebuildCategoryRegistry();
						DialoguePriceTransformer::g_itemBucketsInitialized.store(false);
						DialoguePriceTransformer::g_leveledBucketsInitialized.store(false);
						DialoguePriceTransformer::g_globalBucketsInitialized.store(false);
						auto cfg = g_settings.load();
						if (cfg && cfg->useQuestWatcher) {
							QuestOrderManager::EvaluateAndApplyProfile("$FULL");
							ResetUIBufferFromMemory();
						} else {
							CheckAndApplyPersistentPatches(true);
						}
						DialoguePriceTransformer::PokeQuestsRefresh();
						RefreshPresets();
						InvalidateCategoryCache();
						s_isDirty = false;
						SetStatus(txt->savedSuccess, false);
					}
				}

				ImGuiMCP::EndTabItem();
			}

			ImGuiMCP::EndTabBar();
		}

		ImGuiMCP::Spacing();
		ImGuiMCP::Separator();
		ImGuiMCP::Spacing();

		if (IsDirty()) {
			ImGuiMCP::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, txt->unsavedPending.c_str());
			ImGuiMCP::Spacing();
		}

		std::string applyBtnText = inMainMenu ? txt->btnSaveDefaults : txt->btnApply;
		if (ImGuiMCP::Button(applyBtnText.c_str())) {
			if (IsDialogueMenuOpen()) {
				SetStatus(txt->statusErrDialogueOpenApply, true);
			} else {
				SanitizeSettings(s_menuSettings);
				MCPddcMenu::SetCategoryHiding(s_menuSettings.hideCategories, false);

				if (inMainMenu) {
					SaveSettings(s_menuSettings);
					LoadMenuTexts();
				}

				g_settings.store(std::make_shared<Settings>(s_menuSettings));

				int activeLevel = s_menuSettings.enableDebug ? s_menuSettings.debugLevel : 2;
				SetLogLevel(activeLevel);
				SKSE::log::info("[Menu] Applied settings. Log Level: {}", activeLevel);

				CheckAndApplyPersistentPatches(true);
				auto cfg = g_settings.load();
				if (cfg && cfg->useQuestWatcher) {
					QuestOrderManager::EvaluateAndApplyProfile("$FULL");
				}
				DialoguePriceTransformer::PokeQuestsRefresh();
				InvalidateCategoryCache();
				s_isDirty = false;
				s_confirmRestoreTab = false;
				SetStatus(txt->savedSuccess, false);
			}
		}
		bool hasValidPreset = (s_selectedPresetIndex >= 0 && s_selectedPresetIndex < static_cast<int>(s_presets.size()));
		ImGuiMCP::SameLine();

		if (ImGuiMCP::Button(txt->btnDiscard.c_str())) {
			ResetUIBufferFromMemory();
			s_confirmRestoreTab = false;
		}

		ImGuiMCP::SameLine();
		ImGuiMCP::BeginDisabled(!hasValidPreset);

		if (!s_confirmRestoreTab) {
			if (ImGuiMCP::Button(txt->btnRestore.c_str())) {
				s_confirmRestoreTab = true;
			}
		} else {
			ImGuiMCP::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, txt->confirmRestore.c_str());
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(txt->btnYesRestore.c_str())) {
				if (hasValidPreset) {
					RestoreFromIni(s_presets[s_selectedPresetIndex].filename, s_activeTab, false);
				}
				s_isDirty = true;
				s_confirmRestoreTab = false;
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(txt->btnCancel.c_str())) {
				s_confirmRestoreTab = false;
			}
		}

		ImGuiMCP::EndDisabled();

		if (s_statusTimer > 0.0f) {
			ImGuiMCP::SameLine();
			if (s_statusIsError) {
				ImGuiMCP::TextColored({ 1.0f, 0.3f, 0.3f, 1.0f }, s_statusMessage.c_str());
			} else {
				ImGuiMCP::TextColored({ 0.0f, 1.0f, 0.0f, 1.0f }, s_statusMessage.c_str());
			}
			s_statusTimer -= ImGuiMCP::GetIO()->DeltaTime;
		}
	}
}