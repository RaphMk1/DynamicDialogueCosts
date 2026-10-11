#pragma once

#include <RE/Skyrim.h>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DynamicDialogueCosts
{
	struct TrackedQuestRule
	{
		std::int32_t  completionStage{ -1 };
		std::uint32_t requiredHits{ 1 };
	};

	extern std::unordered_map<std::string, std::vector<TrackedQuestRule>> g_trackedQuests;
	extern std::unordered_map<std::string, std::uint32_t>                 g_trackedQuestStageHits;
	extern std::unordered_map<std::string, std::uint32_t>                 g_questCompletionCounts;
	extern std::unordered_map<std::string, bool>                          g_conditionTruthCache;
	extern std::shared_mutex                                              g_conditionCacheMutex;

	struct QuestCondition
	{
		std::string           questEdid;
		mutable RE::TESQuest* questPtr{ nullptr };
		std::uint16_t         minStage{ 1 };
		std::uint32_t         minCompletions{ 0 };
		std::int32_t          targetCompletionStage{ -1 };
		bool                  checkCompletionCount{ false };
		bool                  isInverted{ false };
		bool                  isDefault{ false };

		std::string cacheKey;
		void        BuildCacheKey();
	};

	enum class QuestActionType
	{
		kLoadPreset,
		kModifySetting,
		kAddSetting,
		kSubSetting,
		kMultSetting
	};

	struct QuestAction
	{
		QuestActionType actionType{ QuestActionType::kLoadPreset };
		std::string     presetFilename;
		std::string     section;
		std::string     key;
		std::string     value;
		float           parsedValue{ 0.0f };
	};

	struct QuestRule
	{
		std::vector<QuestCondition> conditions;
		std::vector<QuestAction>    actions;
	};

	struct QuestProfileData
	{
		std::string            profileName;
		std::string            profileID;
		int                    orderingPriority{ 0 };
		bool                   hideAndLockSettings{ false };
		std::vector<QuestRule> rules;
	};

	struct QuestOrderProfileInfo
	{
		std::string filename;
		std::string profileName;
		std::string profileID;
		int         orderingPriority{ 0 };
	};

	class QuestOrderManager
	{
	public:
		static void                               LoadTrackedQuests();
		static void                               PreloadProfiles();
		static QuestProfileData                   GetProfileData(const std::string& a_profileFolder);
		static std::vector<QuestRule>             GetProfileRules(const std::string& a_profileFolder);
		static std::vector<QuestOrderProfileInfo> GetLoadedProfiles();
		static std::string                        GetProfileName(const std::string& a_profileFolder);
		static void                               EvaluateAndApplyProfile(const std::string& a_triggeredQuestEdid = "");
		static bool                               IsQuestWatched(RE::FormID a_formID);
		static void                               IncrementQuestCompletion(const std::string& a_edid, std::int32_t a_completionStage = -1);
		static void                               InvalidateConditionCacheKey(const std::string& a_cacheKey);
		static void                               InvalidateConditionCacheForQuest(const std::string& a_edid);

	private:
		static bool                                                        EvaluateRule(const QuestRule& a_rule);
		static inline std::string                                          s_currentActiveProfile;
		static inline std::vector<bool>                                    s_activeRuleStates;
		static inline std::unordered_map<std::string, std::vector<size_t>> s_activeQuestToRulesMap;
		static inline std::shared_mutex                                    s_ruleStateMutex;
		static QuestProfileData                                            ParseProfileFolder(const std::string& a_profileFolder);
		static bool                                                        EvaluateCondition(const QuestCondition& a_cond);

		static inline std::unordered_map<std::string, QuestProfileData> s_profileCache;
		static inline std::shared_mutex                                 s_profileCacheMutex;

		static inline std::unordered_set<RE::FormID> s_watchedQuestFormIDs;
		static inline std::unordered_set<RE::FormID> s_trackedQuestFormIDs;
		static inline std::shared_mutex              s_watchedQuestsMutex;
	};
}