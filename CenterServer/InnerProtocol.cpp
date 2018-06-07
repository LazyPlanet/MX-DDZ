#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "MXLog.h"
#include "Player.h"
#include "GmtSession.h"
#include "Protocol.h"
#include "Clan.h"

namespace Adoter
{

namespace spd = spdlog;

extern int32_t g_server_id;
extern std::shared_ptr<GmtSession> g_gmt_client;

bool WorldSession::OnInnerProcess(const Asset::Meta& meta)
{
	pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
	if (!msg) return false;

	auto message = msg->New();

	defer {
		delete message;
		message = nullptr;
	};

	//if (meta.stuff().size() == 0) return true; //确实存在只传输类型没内容的情况
	
	auto result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
	if (!result) return false; 

	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器
		{
			auto register_server = dynamic_cast<Asset::RegisterServer*>(message);
			if (!register_server) return false;

			SetRoleType(Asset::ROLE_TYPE_GAME_SERVER, register_server->global_id());
			WorldSessionInstance.AddServer(register_server->global_id(), shared_from_this());
					
			DEBUG("逻辑服务器:{} 注册在中心服务器:{} 成功", register_server->global_id(), g_server_id);

			register_server->set_global_id(g_server_id);

			SendProtocol(message);
		}
		break;
		
		case Asset::META_TYPE_S2S_GMT_INNER_META: //GMT命令
		{
			//Asset::GmtInnerMeta message;
			//auto result = message.ParseFromString(meta.stuff());
			//if (!result) return false;
			
			const auto gmt_inner_meta = dynamic_cast<const Asset::GmtInnerMeta*>(message);
			if (!gmt_inner_meta) return false;

			if (!g_gmt_client) return false;

			Asset::InnerMeta inner_meta;
			inner_meta.ParseFromString(gmt_inner_meta->inner_meta());
			g_gmt_client->SendInnerMeta(inner_meta);
		}
		break;
		
		case Asset::META_TYPE_S2S_KICKOUT_PLAYER: //退出游戏逻辑服务器
		{
			//Asset::KickOutPlayer message;
			//auto result = message.ParseFromString(meta.stuff());
			//if (!result) return false;
			
			const auto kick_out = dynamic_cast<const Asset::KickOutPlayer*>(message);
			if (!kick_out) return false;

			if (kick_out->player_id() != meta.player_id()) return false;
			
			auto player = PlayerInstance.Get(meta.player_id());
			if (!player) return false;

			if (kick_out->reason() == Asset::KICK_OUT_REASON_LEAVE_ROOM) player->OnEnterCenter(); //进入游戏，初始化数据//正常退出逻辑服务器
		}
		break;

		case Asset::META_TYPE_S2S_CLAN_ROOM_START_OR_OVER:
		{
			const auto clan_room = dynamic_cast<const Asset::ClanRoomStatusChanged*>(message);
			if (!clan_room) return false;

			auto clan = ClanInstance.Get(clan_room->room().clan_id());
			if (!clan) return false;

			clan->OnRoomChanged(clan_room);
		}
		break;
		
		case Asset::META_TYPE_S2S_CLAN_ROOM_SYNC:
		{
			const auto room_sync = dynamic_cast<const Asset::ClanRoomSync*>(message);
			if (!room_sync) return false;

			Asset::RoomQueryResult room_query;
			room_query.ParseFromString(room_sync->room_status());

			auto clan = ClanInstance.Get(room_query.clan_id());
			if (!clan) return false;

			clan->OnRoomSync(room_query);
		}
		break;
		
		case Asset::META_TYPE_S2S_CLAN_OPERATION:
		{
			const auto oper_sync = dynamic_cast<const Asset::ClanOperationSync*>(message);
			if (!oper_sync) return false;

			ClanInstance.OnGameServerBack(*oper_sync);
		}
		break;
		
		/*
		case Asset::META_TYPE_S2S_COMMON_PROP_SYNC:
		{
			auto player = PlayerInstance.Get(meta.player_id());
			if (!player) return false;

			DEBUG("玩家:{} 接收逻辑服务器数据变化协议，重新加载数据", player->GetID());

			player->Load(); //加载数据
		}
		break;
		*/
		
		case Asset::META_TYPE_S2S_CLAN_MATCH:
		{
			const auto match_sync = dynamic_cast<const Asset::ClanMatchSync*>(message);
			if (!match_sync) return false;

			ClanInstance.OnGameServerBack(*match_sync);
		}
		break;

		default:
		{
			auto player = WorldSessionInstance.GetPlayerSession(meta.player_id());

			if (!player) 
			{
				ERROR("未能找到玩家:{}网络连接 不能处理协议:{}", meta.player_id(), Asset::META_TYPE_Name(meta.type_t()));
				return false;
			}
			player->SendMeta(meta);
		}
		break;
	}
	return true;
}

}
