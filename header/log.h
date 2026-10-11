#pragma once

#include <algorithm>
#include <chrono>
#include <format>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <string_view>

inline void SetLogLevel(int a_userLevel)
{
	spdlog::level::level_enum spdlogLevel;
	switch (a_userLevel) {
	case 0:
		spdlogLevel = spdlog::level::off;
		break;
	case 1:
		spdlogLevel = spdlog::level::err;
		break;
	case 2:
		spdlogLevel = spdlog::level::warn;
		break;
	case 3:
		spdlogLevel = spdlog::level::info;
		break;
	case 4:
		spdlogLevel = spdlog::level::debug;
		break;
	case 5:
		spdlogLevel = spdlog::level::trace;
		break;
	default:
		spdlogLevel = spdlog::level::off;
		break;
	}

	spdlog::set_level(spdlogLevel);
	spdlog::flush_on(spdlogLevel);
}

inline void SetupLog(int a_initialLevel = 2)
{
	auto logsFolder = SKSE::log::log_directory();
	if (!logsFolder)
		SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
	std::string pluginName = std::string(SKSE::PluginDeclaration::GetSingleton()->GetName());
	auto        logFilePath = *logsFolder / std::format("{}.log", pluginName);

	auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
	auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
	loggerPtr->set_pattern("[%H:%M:%S.%e] %v");
	spdlog::set_default_logger(std::move(loggerPtr));

	spdlog::info("--------------------------- Dynamic Dialogue Costs v1.0.0 --------------------");
	spdlog::info("Created by RaphNode. Licensed under GNU GPL-3.0 with Modding Exception.");
	spdlog::info("Source Code: https://github.com/RaphMk1/DynamicDialogueCosts");
	spdlog::info("------------------------------------------------------------------------------");

	SetLogLevel(a_initialLevel);
}

struct ScopedTimer
{
	std::string                                    name;
	std::chrono::high_resolution_clock::time_point start;
	bool                                           enabled{ true };

	explicit ScopedTimer(std::string_view a_name, bool a_enabled = true) : name(a_name), start(std::chrono::high_resolution_clock::now()), enabled(a_enabled)
	{
	}

	~ScopedTimer()
	{
		if (!enabled)
			return;

		auto end = std::chrono::high_resolution_clock::now();
		auto durationNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

		double durationUs = static_cast<double>(durationNs) / 1000.0;
		double durationMs = durationUs / 1000.0;

		spdlog::error("[PERF] '{}' completed in {:.1f} ms ({:.1f} µs)", name, durationMs, durationUs);
	}
};

#define BENCHMARK_SCOPE(name) ::ScopedTimer timer_##__LINE__(name)
#define BENCHMARK_SCOPE_IF(name, cond) ::ScopedTimer timer_##__LINE__(name, static_cast<bool>(cond))