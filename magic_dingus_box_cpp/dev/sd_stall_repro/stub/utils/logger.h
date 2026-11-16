#pragma once
// Stand-in for src/utils/logger.h (spdlog is fetched by CMake, which this
// one-file build does not run): prints the format string only.
#include <cstdio>
#define MDB_REPRO_LOG_(lvl, fmt, ...) std::fprintf(stderr, "[%s] %s\n", lvl, fmt)
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(fmt, ...) MDB_REPRO_LOG_("info", fmt)
#define LOG_WARN(fmt, ...) MDB_REPRO_LOG_("warn", fmt)
#define LOG_ERROR(fmt, ...) MDB_REPRO_LOG_("error", fmt)
#define LOG_CRITICAL(fmt, ...) MDB_REPRO_LOG_("crit", fmt)
