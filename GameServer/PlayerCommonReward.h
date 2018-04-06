#pragma once

#include <memory>
#include <functional>

#include "P_Header.h"
#include "MXLog.h"

namespace Adoter
{
namespace pb = google::protobuf;

class PlayerCommonReward : public std::enable_shared_from_this<PlayerCommonReward> 
{
public:
	static PlayerCommonReward& Instance()
	{
		static PlayerCommonReward _instance;
		return _instance;
	}

	Asset::ERROR_CODE DeliverReward(std::shared_ptr<Player> player, int64_t global_id)
	{
		if (!player || global_id <= 0) return Asset::ERROR_COMMON_REWARD_DATA;

		const auto message = AssetInstance.Get(global_id);
		if (!message) return Asset::ERROR_COMMON_REWARD_DATA;

		const auto common_reward = dynamic_cast<const Asset::CommonReward*>(message);
		if (!common_reward) return Asset::ERROR_COMMON_REWARD_DATA;

		int64_t common_limit_id = common_reward->common_limit_id();
		if (common_limit_id && player->IsCommonLimit(common_limit_id)) return Asset::ERROR_COMMON_REWARD_HAS_GOT; //该奖励已经超过领取限制
		
		int64_t activity_id = common_reward->activity_id();
		if (activity_id && !ActivityInstance.IsOpen(activity_id)) return Asset::ERROR_ACTIVITY_NOT_OPEN; //活动尚未开启

		for (const auto& reward : common_reward->rewards())
		{
			int32_t count = reward.count();

			switch (reward.reward_type())
			{
				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_DIAMOND:
				{
					player->GainDiamond(Asset::DIAMOND_CHANGED_TYPE_GENERAL_REWARD, count);
				}
				break;

				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_HUANLEDOU:
				{
					player->GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GENERAL_REWARD, count);
				}
				break;
				
				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_ROOM_CARD:
				{
					player->GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_GENERAL_REWARD, count);
				}
				
				/*
				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_ITEM:
				{
					player->GainItem(reward.item_id(), count);
				}
				break;
				*/

				default:
				{

				}
				break;
			}

			LOG(INFO, "player_id:{} global_id:{} common_limit_id:{} reward_type:{}", player->GetID(), global_id, common_limit_id, reward.reward_type());
		}

		player->AddCommonLimit(common_limit_id); 

		return Asset::ERROR_SUCCESS;
	}

};

#define CommonRewardInstance PlayerCommonReward::Instance()

}
