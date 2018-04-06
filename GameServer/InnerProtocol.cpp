#include <spdlog/spdlog.h>

#include "CenterSession.h"
#include "MXLog.h"
#include "Player.h"
#include "GmtSession.h"
#include "Protocol.h"

namespace Adoter
{

namespace spd = spdlog;

bool CenterSession::OnInnerProcess(const Asset::Meta& meta)
{
	pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
	if (!msg) return false;

	auto message = msg->New();

	defer {
		delete message;
		message = nullptr;
	};

	auto result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
	if (!result) return false; 

	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器成功
		{
			auto register_server = dynamic_cast<Asset::RegisterServer*>(message);
			if (!register_server) return false;

			_center_server_id = register_server->global_id(); //中心服务器返回

			DEBUG("游戏逻辑服务器:{} 注册到中心服:{} 成功.", g_server_id, _center_server_id);
		}
		break;

		case Asset::META_TYPE_S2S_GMT_INNER_META: //GMT命令
		{
			const auto gmt_inner_meta = dynamic_cast<const Asset::GmtInnerMeta*>(message);
			if (!gmt_inner_meta) return false;

			GmtInstance.SetSession(std::dynamic_pointer_cast<CenterSession>(shared_from_this()));

			Asset::InnerMeta inner_meta;
			inner_meta.ParseFromString(gmt_inner_meta->inner_meta());
			GmtInstance.OnInnerProcess(inner_meta);
		}
		break;
		
		case Asset::META_TYPE_S2S_KICKOUT_PLAYER: //防止玩家退出后收到踢出继续初始化
		{
			const auto kick_player = dynamic_cast<const Asset::KickOutPlayer*>(message);
			if (!kick_player) return false;

			auto player = GetPlayer(meta.player_id());
			if (!player) return false;
			
			WARN("玩家:{} 被踢出服务器:{} 原因:{}", meta.player_id(), g_server_id, message->ShortDebugString());

			if (kick_player->reason() == Asset::KICK_OUT_REASON_CHANGE_SERVER) player->OnLogout(kick_player->reason()); //强制踢出
			else player->Logout(message);
		}
		break;

		default:
		{
			ERROR("尚未存在协议处理回调:{} 协议:{}", meta.type_t(), message->ShortDebugString());
		}
		break;
	}

	return true;
}

}
