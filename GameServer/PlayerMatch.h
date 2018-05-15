#pragma once

#include <queue>
#include <memory>
#include <functional>
#include <unordered_map>

#include "P_Header.h"
#include "TaskScheduler.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Player;

class PlayerMatch : public std::enable_shared_from_this<PlayerMatch>   
{
	std::unordered_map<int32_t/*房间类型*/, std::unordered_map<int64_t/*角色ID*/, std::shared_ptr<Player>>> _match_list; 
	std::unordered_map<int32_t/*房间类型*/, const Asset::RoomOptions> _options;

	TaskScheduler _scheduler;
	std::mutex _match_mutex;

public:
	static PlayerMatch& Instance()
	{
		static PlayerMatch _instance;
		return _instance;
	}

	void Update(int32_t diff);

	void Join(std::shared_ptr<Player> player, pb::Message* message);
	void DoMatch();
	void OnStart();
};

#define MatchInstance PlayerMatch::Instance()

}
