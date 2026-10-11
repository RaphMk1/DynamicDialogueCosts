#pragma once
#include "settings.h"
#include <SimpleIni.h>
#include <shared_mutex>

namespace DynamicDialogueCosts
{
	struct GlobalCategoryEntry
	{
		RE::TESGlobal* global{ nullptr };
		std::string    categoryID;
		std::string    editorID;
	};

	struct CachedPreset
	{
		std::string                  filename;
		std::filesystem::path        filepath;
		std::string                  displayName;
		int                          priority{ 0 };
		std::shared_ptr<CSimpleIniA> iniData{ std::make_shared<CSimpleIniA>() };
	};

	class DialogueGlobalRegistry
	{
	public:
		enum class ExtGlobalOp : std::uint8_t
		{
			Multiply = 0,
			AddPercent,
			SubPercent,
			FlatOffset,
			Alien
		};

		struct ExtGlobalEntry
		{
			RE::TESGlobal* global{ nullptr };
			ExtGlobalOp    op{ ExtGlobalOp::Multiply };
			float          scale{ 1.0f };
		};

		struct SettEntry
		{
			RE::TESGlobal* global{ nullptr };
			RE::Setting*   gmst{ nullptr };
			int            idNumber{ 0 };
			std::string    categoryID;
			std::string    editorID;

			bool operator<(const SettEntry& a_other) const
			{
				if (idNumber != a_other.idNumber) {
					return idNumber > a_other.idNumber;
				}
				if (global != a_other.global) {
					return global < a_other.global;
				}
				return gmst < a_other.gmst;
			}
		};

		struct CachedItemData
		{
			std::string   categoryID;
			std::int32_t  originalValue{ 0 };
			std::int32_t* valuePtr{ nullptr };
		};

		enum class FactionCrimeField : std::uint8_t
		{
			kStealMult = 0,
			kMurder,
			kAssault,
			kTrespass,
			kPickpocket,
			kEscape,
			kWerewolf
		};

		struct CachedFactionCrimeData
		{
			std::string       categoryID;
			RE::TESFaction*   faction{ nullptr };
			FactionCrimeField fieldType;
			float             originalValue{ 0.0f };
			std::string       editorID;
		};

		inline static std::unordered_map<RE::TESForm*, std::string> g_debugItemEditorIDs;

		inline static std::unordered_map<RE::TESForm*, CachedItemData>        g_cachedItemCategoryMap;
		inline static std::unordered_map<RE::TESGlobal*, GlobalCategoryEntry> g_cachedCategoryMap;
		inline static std::unordered_map<RE::TESLevItem*, std::string>        g_cachedLeveledListCategoryMap;
		inline static std::unordered_map<std::string, RE::TESGlobal*>         g_dialogueStepGlobalEdids;
		inline static std::unordered_map<RE::TESGlobal*, GlobalCategoryEntry> g_dialogueStepGlobals;
		inline static std::vector<SettEntry>                                  g_cachedSettEntries;
		inline static std::vector<CachedFactionCrimeData>                     g_cachedFactionCrimeEntries;
		inline static std::unordered_multimap<std::string, ExtGlobalEntry>    g_cachedExternalGlobalCategoryMap;

		inline static std::unordered_map<std::string, std::string>      g_categoryDisplayNames;
		inline static std::unordered_map<std::string, CategorySettings> g_categoryDiskDefaults;

		struct RawCategoryData
		{
			std::string      catID;
			std::string      displayName;
			CategorySettings diskDefaults;

			std::vector<std::string>                                 globEditorIDs;
			std::vector<std::string>                                 lvliEditorIDs;
			std::vector<std::string>                                 itemEditorIDs;
			std::vector<std::pair<std::string, int>>                 settEntries;
			std::vector<std::pair<std::string, std::string>>         factEntries;
			std::vector<std::tuple<std::string, ExtGlobalOp, float>> extgEntries;
		};

		static RawCategoryData ParseCategoryDataTask(const std::filesystem::path& a_folderPath);

		static void           IndexCatRegistry();
		static void           LoadGlobalPatches();
		static RE::TESGlobal* GetDialoguePriceGlobalByEdid(std::string_view a_edid);

		static void                        LoadPresetsCache();
		static std::vector<CachedPreset>   GetCachedPresets();
		static std::optional<CachedPreset> GetCachedPreset(const std::string& a_filename);

	private:
		static inline std::shared_mutex                             g_presetCacheMutex;
		static inline std::vector<CachedPreset>                     g_cachedPresets;
		static inline std::unordered_map<std::string, CachedPreset> g_cachedPresetsMap;
		static void                                                 UpdateCategoryMultipliers();
	};
}