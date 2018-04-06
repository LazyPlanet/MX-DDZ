#pragma once

#include <memory>
#include <functional>
#include <mutex>

#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/date_time/posix_time/conversion.hpp"
#include "boost/date_time/gregorian/gregorian.hpp"

#include "Timer.h"
#include "P_Header.h"
#include "Asset.h"
#include "Player.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Activity : public std::enable_shared_from_this<Activity> 
{
private:
	int32_t _heart_count = 0;
	std::mutex _mutex;
	std::unordered_map<int64_t/*global_id*/, Asset::Activity*> _activity; //可以通过GMT命令进行修改
	std::unordered_map<int64_t/*global_id*/, bool/*opened*/> _state;
public:

	static Activity& Instance()
	{
		static Activity _instance;
		return _instance;
	}

	bool Load()
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ACTIVITY);

		for (auto it = messages.begin(); it != messages.end(); ++it)
		{
			auto activity = dynamic_cast<Asset::Activity*>(*it);
			if (!activity) return false;

			_activity.emplace(activity->common_prop().global_id(), activity);
		}

		return true;
	}
	
	bool IsOpen(int64_t global_id)
	{
		std::lock_guard<std::mutex> lock(_mutex);

		auto it = _state.find(global_id);
		if (it == _state.end()) return true; //如果没有，则认为不限制，是开启状态

		return it->second;
	}
	
	void Update(int32_t diff)
	{
		std::lock_guard<std::mutex> lock(_mutex);

		++_heart_count;

		bool has_changed = false;

		for (auto it = _activity.begin(); it != _activity.end(); ++it)
		{
			switch (it->second->activity_type())
			{
				//
				//每日活动，每天固定时间内满足条件
				//
				//先进行日期检查，再进行时间检查
				//
				//如果有变化，才重新推给Client做更新
				//
				case Asset::ACTIVITY_CYCLE_TYPE_DIALY: //每日活动
				{
					boost::posix_time::ptime start_date(boost::posix_time::time_from_string(it->second->start_date() + " 00::00::00"));
					boost::posix_time::ptime stop_date(boost::posix_time::time_from_string(it->second->stop_date() + " 23::59::59"));
						
					boost::posix_time::time_duration start_time_duration(boost::posix_time::duration_from_string(it->second->start_time()));
					boost::posix_time::time_duration stop_time_duration(boost::posix_time::duration_from_string(it->second->stop_time()));
					
					auto curr_time = CommonTimerInstance.GetPTime();
					//boost::posix_time::ptime curr_time = boost::posix_time::from_time_t(cur_t);
					auto curr_time_duration = curr_time.time_of_day()/* + boost::posix_time::time_duration(-8, 0, 0)*/; //19:00:00

					//DEBUG("start_date:{} stop_date:{} curr_time:{}", boost::posix_time::to_simple_string(start_date), 
					//		boost::posix_time::to_simple_string(stop_date), boost::posix_time::to_simple_string(curr_time));

					if (curr_time.date() >= start_date.date() && curr_time.date() <= stop_date.date()) //日期满足
					{
						//DEBUG("start_time_duration:{} stop_time_duration:{} curr_time_duration:{}", boost::posix_time::to_simple_string(start_time_duration), 
						//		boost::posix_time::to_simple_string(stop_time_duration), boost::posix_time::to_simple_string(curr_time_duration));

						if (curr_time_duration >= start_time_duration && curr_time_duration <= stop_time_duration) //时间满足
						{
							if (!_state[it->first])
							{
								has_changed = true; //状态变化
								_state[it->first] = true;
							}
						}
						else
						{
							if (_state[it->first])
							{
								has_changed = true; //状态变化
								_state[it->first] = false;
							}
						}
					}
					else
					{
						if (_state[it->first])
						{
							has_changed = true; //状态变化
							_state[it->first] = false;
						}
					}
				}
				break;
				
				//
				//时间段活动
				//
				//在某个时间内，进行活动检查，比如 2017-07-07 10:00:00 ~ 2017-08-07 10:00:00 
				//
				//直接判断当前时间是否在该时间段内即可
				//
				case Asset::ACTIVITY_CYCLE_TYPE_DURATION: //时间段
				{
					boost::posix_time::ptime start_time(boost::posix_time::time_from_string(it->second->start_date() + " " + it->second->start_time()));
					//auto start_time_t = boost::posix_time::to_time_t(start_time);

					boost::posix_time::ptime stop_time(boost::posix_time::time_from_string(it->second->start_date() + " " + it->second->stop_time()));
					//auto stop_time_t = boost::posix_time::to_time_t(stop_time);
					
					auto curr_time = TimerInstance.GetPTime();

					if (curr_time < start_time || curr_time > stop_time) //不在时间段内
					{
						if (_state[it->first])
						{
							has_changed = true; //状态变化
							_state[it->first] = false;
						}
					}
					else
					{
						if (!_state[it->first])
						{
							has_changed = true; //状态变化
							_state[it->first] = true;
						}
					}
				}
				break;

				default:
				{

				}
				break;
			}
		}

		//
		//如果有一个任务状态发生了变化，则更新所有任务数据
		//
		if (has_changed)
		{
			Asset::SyncActivity message; //数据变化

			for (auto it = _state.begin(); it != _state.end(); ++it)
			{
				auto activity = message.mutable_activity_list()->Add();
				activity->set_activity_id(it->first);
				activity->set_open(it->second);
			}

			PlayerInstance.BroadCast(message); //广播所有玩家
		}
	}

	void OnPlayerLogin(std::shared_ptr<Player> player)
	{
		std::lock_guard<std::mutex> lock(_mutex);

		if (!player) return;

		Asset::SyncActivity message; //数据变化

		for (auto it = _state.begin(); it != _state.end(); ++it)
		{
			auto activity = message.mutable_activity_list()->Add();
			activity->set_activity_id(it->first);
			activity->set_open(it->second);
		}
		player->SendProtocol(message);
	}
	
	//
	//GMT命令控制活动的开启时间和结束时间
	//
	Asset::COMMAND_ERROR_CODE OnActivityControl(const Asset::ActivityControl& command)
	{
		std::lock_guard<std::mutex> lock(_mutex);

		auto it = _activity.find(command.activity_id());
		if (it == _activity.end()) return Asset::COMMAND_ERROR_CODE_PARA;

		try {

			boost::posix_time::ptime start_date(boost::posix_time::time_from_string(command.start_time()));
			boost::posix_time::ptime stop_date(boost::posix_time::time_from_string(command.stop_time()));

			it->second->set_start_date(boost::gregorian::to_simple_string(start_date.date()));
			it->second->set_start_time(boost::posix_time::to_simple_string(start_date.time_of_day()));
			
			it->second->set_stop_date(boost::gregorian::to_simple_string(stop_date.date()));
			it->second->set_stop_time(boost::posix_time::to_simple_string(stop_date.time_of_day()));
		}
		catch (std::exception& e)
		{
			LOG(ERROR, "GMT修改活动:{}时间:{}失败", command.activity_id(), command.ShortDebugString());
			return Asset::COMMAND_ERROR_CODE_PARA;
		}

		return Asset::COMMAND_ERROR_CODE_SUCCESS;
	}

};

#define ActivityInstance Activity::Instance()

}

