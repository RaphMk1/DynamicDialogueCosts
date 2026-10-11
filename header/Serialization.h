#pragma once

#include <SKSE/SKSE.h>

namespace DynamicDialogueCosts::Serialization
{
	inline constexpr std::uint32_t kSerializationID = 'C?4t';
	inline constexpr std::uint32_t kSettingsRecord = 'STRD';
	inline constexpr std::uint32_t kQuestCountsRecord = 'QCRD';
	inline constexpr std::uint32_t kConditionCacheRecord = 'CCRD';
	inline constexpr std::uint32_t kActiveRatesRecord = 'ARRD';

	inline constexpr std::uint32_t kCurrentVersion = 1;

	void SaveCallback(SKSE::SerializationInterface* a_intfc);
	void LoadCallback(SKSE::SerializationInterface* a_intfc);
	void ResetState();
	void RevertCallback(SKSE::SerializationInterface* a_intfc);
	void Register();
}