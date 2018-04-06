#pragma once

#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <string>
#include <stdarg.h>

#include "spdlog/spdlog.h"

#include "Config.h"
#include "CommonUtil.h"
#include "P_Header.h"

namespace Adoter
{

#define MAX_DATA_SIZE 65536  

//////////////////////////////////////////////////////////////////////////
/////各种日志的宏定义/////
//////////////////////////////////////////////////////////////////////////

//调试日志
#define DEBUG(fmt, ...) { \
	auto debug = ConfigInstance.GetBool("ConsoleLog", true); \
	if (debug) { \
		spdlog::get("console")->debug("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
	}\
}\

#define TRACE(fmt, ...) { \
		spdlog::get("console")->trace("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define ERROR(fmt, ...) { \
		spdlog::get("console")->error("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define WARN(fmt, ...) { \
		spdlog::get("console")->warn("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define CRITICAL(fmt, ...) { \
		spdlog::get("console")->critical("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

//主要供BI日志查询
#define LOG_BI(log_name, message) { \
		std::string json; \
		pbjson::pb2json(&message, json); \
		auto log = spdlog::get(log_name); \
		if (log) log->info(json); \
}\

//通用日志
#define LOG_TRACE(fmt, ...) { \
		spdlog::get("common")->trace("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG_DEBUG(fmt, ...) { \
		spdlog::get("common")->debug("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG_INFO(fmt, ...) { \
		spdlog::get("common")->info("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG_ACTION(fmt, ...) { \
		spdlog::get("common")->info("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG_ERROR(fmt, ...) { \
		spdlog::get("common")->error("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
		spdlog::get("console")->error("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG_ERR(fmt, ...) { \
		spdlog::get("common")->error("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
		spdlog::get("console")->error("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG_CRITICAL(fmt, ...) { \
		spdlog::get("common")->critical("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define LOG(level, fmt, ...) LOG_##level(fmt, ##__VA_ARGS__);

namespace pb = google::protobuf;

////////////////////////////////////////////////////////////////////////////////
// 日志管理系统
////////////////////////////////////////////////////////////////////////////////
class MXLog : public std::enable_shared_from_this<MXLog> 
{
	std::string _dir; //存储路径
	int64_t _server_id; //服务器ID
	std::string _server_name; //服务器名称

public:
	~MXLog() { 
		spdlog::drop_all(); 
	}

	MXLog()
	{
		////////////////////日志格式////////////////////
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%f] [logname:%n] [level:%l] [thread:%t] %v");
		spdlog::set_async_mode(4096); //队列大小必须是2的整数倍

		////////////////////日志定义////////////////////
		//
		//控制台日志
		auto console = spdlog::stdout_color_mt("console");
		console->set_level(spdlog::level::trace);
		//控制台日志历史记录
		//auto rotating_logger = spdlog::rotating_logger_mt("console_history", "logs/history", 1048576 * 500, 20); //500MB 20个文件
		//rotating_logger->flush_on(spdlog::level::trace);
		//玩家日志
		auto player = spdlog::daily_logger_st("player", "logs/player");
		player->flush_on(spdlog::level::trace);
		
		//账号数据
		auto account = spdlog::daily_logger_st("account", "logs/account");
		account->flush_on(spdlog::level::trace);
		//通用日志
		auto common = spdlog::daily_logger_st("common", "logs/log");
		common->flush_on(spdlog::level::trace);
	}

	static MXLog& Instance()
	{
		static MXLog _instance;
		return _instance;
	}

	void Load()
	{
		_dir = ConfigInstance.GetString("LogDirectory", "logs");

		if (!_dir.empty())
		{
			if ((_dir.at(_dir.length() - 1) != '/') && (_dir.at(_dir.length() - 1) != '\\'))
			{
				_dir.push_back('/');
			}
		}
	}
};

#define MXLogInstance MXLog::Instance()

}
