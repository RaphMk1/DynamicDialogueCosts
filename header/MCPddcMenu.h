#pragma once

#include "SKSEMenuFramework.h"
#include "dialogueRegistry.h"
#include "settings.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace DynamicDialogueCosts
{
	inline constexpr auto kVanillaPresetPath = L"Data/SKSE/Plugins/DynamicDialogueCosts/Presets/Vanilla.ini";
	inline constexpr auto kMenuTextsIniPath = L"Data/SKSE/Plugins/DynamicDialogueCosts/SKSEMenu/MenuTextsMCP.ini";

	struct QuestOrderEntry
	{
		std::string  filename;
		std::wstring filepath;
		std::string  displayName;
		std::string  profileID;
	};

	struct PresetEntry
	{
		std::string  filename;
		std::wstring filepath;
		std::string  displayName;
		int          priority{ 0 };
	};

	struct MenuTextsMCP
	{
		std::string menuConfiguration{ "Config" };
		std::string tabStatic{ "Static Mult" };
		std::string tabBarter{ "Barter Mult" };
		std::string tabInflation{ "Inflation Mult" };
		std::string tabLevel{ "Level Mult" };
		std::string tabDebug{ "Misc." };

		std::string activePreset{ "Active Preset:" };
		std::string dummyPreset{ "None" };
		std::string searchPreset{ "Search" };
		std::string searchCategories{ "Search Categories" };
		std::string maxInflationCapHeader{ "Inflation Mult Cap:" };
		std::string totalFinalCapHeader{ "Combined Mult Cap:" };
		std::string btnYesLoadAll{ "Yes, Load All Tabs" };
		std::string selectProfile{ "Select Profile..." };
		std::string searchProfile{ "Search" };

		std::string scaleCatValues{ "Enable Value Scaling" };
		std::string categoriesHeader{ "Select affected categories and set their base static multipliers:" };

		std::string enableDynamicScaling{ "Enable Barter Rates Mult" };
		std::string includePowerModifiers{ "Include Gear|Potions|Blessings" };
		std::string includePerkModifiers{ "Include Perk Entries" };
		std::string barterCategoriesHeader{ "Select categories affected by buy|sell barter rates:" };

		std::string enableInflation{ "Enable Inflation" };
		std::string inflationCategoriesHeader{ "Select affected categories, then set their daily inflation rates:" };

		std::string enableLevelScaling{ "Enable Level Scaling" };
		std::string levelCategoriesHeader{ "Select affected categories and set their scaling rates per level:" };
		std::string invertLevelMult{ "Invert" };

		std::string useQuestWatcher{ "Enable Quest Control System" };
		std::string activeQuestOrderHeader{ "Active Quest Control Profile:" };
		std::string enableDebug{ "Enable Debug Mode" };
		std::string logLevel{ "Log Level" };
		std::string hideCategories{ "Hide and lock menu" };
		std::string btnReloadCategories{ "Reload Category Files and Apply Current Settings" };
		std::string btnSaveDefaults{ "Save as NG Defaults" };

		std::string unsavedPending{ "* Unsaved Changes" };
		std::string btnApply{ "Apply" };
		std::string btnDiscard{ "Discard" };
		std::string btnRestore{ "Restore Tab Preset" };
		std::string btnReset{ "Reset" };
		std::string confirmRestore{ "Are you sure?" };
		std::string btnYesRestore{ "Yes" };
		std::string btnCancel{ "Cancel" };
		std::string savedSuccess{ "Saved!" };

		std::string missingCategoryFolder{ " [MISSING Category Folder]" };

		std::string statusCategoryHidingLocked{ "Category hiding locked!" };
		std::string statusCategoryHidingUnlocked{ "Category hiding unlocked!" };
		std::string statusFailedLoadPreset{ "Failed to load preset INI!" };
		std::string confirmApplyPreset{ "Apply selected preset to all tabs?" };
		std::string statusLoadedPresetAll{ "Loaded preset to all tabs!" };
		std::string statusAppliedQuestOrder{ "Applied Quest Order profile to active session!" };
		std::string statusLoadedQuestOrder{ "Loaded Quest Order:" };
		std::string statusErrDialogueOpenReload{ "Cannot reload category files while in Dialogue!" };
		std::string statusErrDialogueOpenApply{ "Cannot apply settings while in Dialogue!" };
	};

	extern std::atomic<std::shared_ptr<const MenuTextsMCP>> g_menuTexts;

	void LoadMenuTexts();
	void LogMenuFrameworkStatus(const std::string& a_message, bool a_isError = false);
	void EnsureBackupIniExists();
	void SaveSettings(const Settings& cfg);

	class MCPddcMenu
	{
	public:
		static bool Register();
		static bool IsDialogueMenuOpen();
		static void SetStatus(const std::string& a_msg, bool a_isError);

		static bool     s_isDirty;
		static Settings s_menuSettings;
		static bool     IsCategoryHidingLocked();
		static void     SetCategoryHiding(bool a_hideAndLock, bool a_syncToGlobalSettings = true);

		static void                         RefreshQuestOrders();
		static std::vector<QuestOrderEntry> s_questOrders;
		static int                          s_selectedQuestOrderIndex;
		static char                         s_questOrderFilter[64];
		static char                         s_categoryFilter[64];
		static void                         ResetUIBufferFromMemory();
		static void                         SanitizeSettings(Settings& a_settings);
		static void                         SetSelectedPresetByFilename(const std::string& a_filename);
		static bool                         IsInMainMenu();
		static bool                         s_bootStateInitialized;

		static const std::vector<std::string>& GetActiveCategories();
		static void                            InvalidateCategoryCache();
		static void                            RefreshPresets();

		static void RestoreFromIni(const std::string& a_presetFilename, int a_activeTab, bool a_allTabs = false);

	private:
		static void __stdcall OnMenuOpen(SKSEMenuFramework::Model::EventType a_event);
		static bool           IsDirty();
		static void           RenderMenu();

		static std::string              GetCategoryDisplayName(const std::string& a_categoryID, const MenuTextsMCP* a_txt);
		static inline std::atomic<bool> s_hideCategoriesLocked{ false };

		static std::string s_statusMessage;
		static float       s_statusTimer;
		static bool        s_statusIsError;

		static int  s_activeTab;
		static bool s_confirmRestoreTab;

		static std::vector<PresetEntry> s_presets;
		static int                      s_selectedPresetIndex;
		static char                     s_presetFilter[64];
		static bool                     s_confirmApplyPresetAll;

		static std::vector<std::string> s_cachedActiveCategories;
		static bool                     s_categoryCacheValid;
	};
}