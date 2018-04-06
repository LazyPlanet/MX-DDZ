#pragma once

#include <memory>
#include <functional>

#include "P_Header.h"
#include "Timer.h"

namespace Adoter
{
namespace pb = google::protobuf;

//
//冷却包括两种:
//
//1.系统冷却,服务器单端限制,自行定义,从1开始
//
//2.通用冷却,S<->C 同时维护,Server变化通知Client
//
//冷却时间单位:秒(s)
//

std::unordered_map<int32_t, int32_t> _cooldown_list = {
	{Asset::SYSTEM_COOLDOWN_TYPE_MATCHING, 3}, //匹配冷却3秒
};

class PlayerCoolDown : public std::enable_shared_from_this<PlayerCoolDown> 
{
public:

	static PlayerCoolDown& Instance()
	{
		static PlayerCoolDown _instance;
		return _instance;
	}

	bool AddCoolDown(std::shared_ptr<Player> player, int64_t global_id)
	{
		if (!player || global_id <= 0) return false; 

		auto cool_down = player->GetMutableCoolDown(); //玩家所有通用限制数据

		auto it = std::find_if(cool_down->elements().begin(), cool_down->elements().end(), [global_id](const Asset::PlayerCoolDown_Element& cool){
					return global_id == cool.cool_down_id();
				});

		if (it != cool_down->elements().end()) 
		{
			auto mutable_it = const_cast<Asset::PlayerCoolDown_Element*>(&(*it));
			mutable_it->set_time_stamp(CommonTimerInstance.GetTime() + _cooldown_list[global_id]);
		}
		else
		{
			auto element = cool_down->mutable_elements()->Add();
			element->set_cool_down_id(global_id);
			element->set_time_stamp(CommonTimerInstance.GetTime() + _cooldown_list[global_id]);
		}

		//player->SyncCommonLimit(); //同步数据

		return true;
	}

	//是否冷却中
	//
	//返回true,则还在冷却中,不能进行操作
	//返回false,则冷却消除,可以进行操作
	//
	bool IsCoolDown(std::shared_ptr<Player> player, int64_t global_id)
	{
		if (!player || global_id <= 0) return false; //没有限制
		
		auto cool_down = player->GetMutableCoolDown(); //玩家所有通用限制数据

		int32_t current_time = CommonTimerInstance.GetTime();

		for (int32_t i = 0; i < cool_down->elements().size(); ++i)
		{
			const auto& cooldown_element = cool_down->elements(i);

			if (global_id != cooldown_element.cool_down_id()) continue;

			if (current_time < cooldown_element.time_stamp()) return true; //尚未在周期内

			cool_down->mutable_elements()->SwapElements(i, cool_down->elements().size() - 1);
			cool_down->mutable_elements()->RemoveLast();
			return false;
		}
			
		return false;
	}
	
	//返回是否更新
	bool Update(std::shared_ptr<Player> player)
	{
		if (!player) return false; 

		return true;
	}

};

#define CoolDownInstance PlayerCoolDown::Instance()

}
