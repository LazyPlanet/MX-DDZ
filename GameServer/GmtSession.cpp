#include "GmtSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Player.h"
#include "Activity.h"
#include "Room.h"

namespace Adoter
{
#define RETURN(x) \
	auto response = command; \
	response.set_error_code(x); \
	auto debug_string = command.ShortDebugString(); \
	if (x) { \
		LOG(ERR, "执行指令失败:{} 指令:{}", x, command.ShortDebugString()); \
	} else { \
		LOG(TRACE, "执行指令成功:{} 指令:{}", x, command.ShortDebugString()); \
	} \
	SendProtocol(response); \
	return x; \


bool GmtManager::OnInnerProcess(const Asset::InnerMeta& meta)
{
	std::lock_guard<std::mutex> lock(_gmt_lock);
	_session_id = meta.session_id();
	
	DEBUG("游戏逻辑服务器接收会话:{} GMT指令:{}", _session_id, meta.ShortDebugString());

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_COMMAND: //发放钻石//房卡//欢乐豆
		{
			Asset::Command message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;
			
			OnCommandProcess(message);
		}
		break;

		case Asset::INNER_TYPE_OPEN_ROOM: //代开房
		{
			Asset::OpenRoom message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			//房间设置
			Asset::Room room;
			room.set_room_type(Asset::ROOM_TYPE_FRIEND);
			room.mutable_options()->ParseFromString(message.options());
			room.mutable_options()->set_gmt_opened(true); //代开房

			auto room_ptr = RoomInstance.CreateRoom(room);
			if (!room_ptr) 
			{
				LOG(ERROR, "GMT开房失败，开房信息:{}", room.ShortDebugString());
				return false; //未能创建成功房间，理论不会出现
			}

			room_ptr->SetGmtOpened(); //设置GMT开房

			message.set_room_id(room_ptr->GetID());
			message.set_error_code(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功创建
			message.set_server_id(ConfigInstance.GetInt("ServerID", 1));

 			SendProtocol(message); //发送给GTM服务器
			
			DEBUG("GMT开房成功，房间ID:{} 会话:{} 玩法:{}", message.room_id(), meta.session_id(), room.ShortDebugString());
		}	   
		break;

		case Asset::INNER_TYPE_SEND_MAIL: //发送邮件
		{
			Asset::SendMail message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnSendMail(message);
		}
		break;
		
		case Asset::INNER_TYPE_SYSTEM_BROADCAST: //系统广播
		{
			Asset::SystemBroadcast message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnSystemBroadcast(message);
		}

		case Asset::INNER_TYPE_ACTIVITY_CONTROL: //活动控制
		{
			Asset::ActivityControl message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnActivityControl(message);
		}
		
		case Asset::INNER_TYPE_BIND_PLAYER: //绑定玩家
		{
			Asset::BindPlayer message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnBindPlayer(message);
		}
		break;

		default:
		{
			WARN("接收GMT指令:{} 尚未含有处理回调，协议数据:{}", meta.type_t(), meta.ShortDebugString());
		}
		break;
	}
	return true;
}
	
Asset::COMMAND_ERROR_CODE GmtManager::OnActivityControl(const Asset::ActivityControl& command)
{
	auto ret = ActivityInstance.OnActivityControl(command);
	RETURN(ret)
}

Asset::COMMAND_ERROR_CODE GmtManager::OnBindPlayer(const Asset::BindPlayer& command)
{
	auto player_ptr = PlayerInstance.Get(command.player_id());
	if (!player_ptr)
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_OFFLINE); //玩家目前不在线
	}

	player_ptr->OnBind(command.agent_account());
	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
}

Asset::COMMAND_ERROR_CODE GmtManager::OnSendMail(const Asset::SendMail& command)
{
	const auto player_id = command.player_id(); 

	if (player_id != 0) //玩家定向邮件
	{
		auto player_ptr = PlayerInstance.Get(player_id);
		//
		//理论上玩家应该在线，但是没有查到该玩家，原因
		//
		//1.玩家已经下线; 2.玩家在其他服务器上;
		//
		if (!player_ptr) 
		{
			RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_OFFLINE); //玩家目前不在线
		}

		auto& player = player_ptr->Get();

		auto mail_id = command.mail_id();

		if (mail_id > 0)
		{
			player.mutable_mail_list_system()->Add(mail_id);
		}
		else
		{
			auto mail = player.mutable_mail_list_customized()->Add();
			mail->set_title(command.title());
			mail->set_content(command.content());
			mail->set_send_time(CommonTimerInstance.GetTime());

			//钻石
			auto attachment = mail->mutable_attachments()->Add();
			attachment->set_attachment_type(Asset::ATTACHMENT_TYPE_DIAMOND);
			attachment->set_count(command.diamond_count());
			
			//欢乐豆
			attachment = mail->mutable_attachments()->Add();
			attachment->set_attachment_type(Asset::ATTACHMENT_TYPE_HUANLEDOU);
			attachment->set_count(command.huanledou_count());
			
			//房卡
			attachment = mail->mutable_attachments()->Add();
			attachment->set_attachment_type(Asset::ATTACHMENT_TYPE_ROOM_CARD);
			attachment->set_count(command.room_card_count());
		}

		//存盘
		player_ptr->SetDirty();
	}
	else //全服邮件
	{
		WARN("收到了全服邮件数据，目前不支持.");
	}
	
	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
}
			
Asset::COMMAND_ERROR_CODE GmtManager::OnCommandProcess(const Asset::Command& command)
{
	auto player_id = command.player_id();
	if (player_id <= 0) //玩家角色校验
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}
	
	/*
	if (command.count() <= 0) //不应该是负数
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}
	*/
	
	auto player_ptr = PlayerInstance.Get(player_id);
	//
	//理论上玩家应该在线，但是没有查到该玩家，原因
	//
	//1.玩家已经下线; 2.玩家在其他服务器上;
	//
	if (!player_ptr) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_OFFLINE); //玩家目前不在线
	}
			
	/*
	if (!player_ptr->IsCenterServer()) //当前不在中心服务器，则到逻辑服务器进行处理
	{
		player_ptr->SendGmtProtocol(command); //转发
		//RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
		return Asset::COMMAND_ERROR_CODE_SUCCESS; //不返回协议
	}
	*/

	switch(command.command_type())
	{
		case Asset::COMMAND_TYPE_RECHARGE:
		{
			player_ptr->GainDiamond(Asset::DIAMOND_CHANGED_TYPE_GMT, command.count());
		}
		break;
		
		case Asset::COMMAND_TYPE_ROOM_CARD:
		{
			if (command.count() >= 0)
			{
				player_ptr->GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_GMT, command.count());   
			}
			else
			{
				player_ptr->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_GMT, -command.count());
			}
		}
		break;
		
		case Asset::COMMAND_TYPE_HUANLEDOU:
		{
			player_ptr->GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GMT, command.count());
		}
		break;
		
		default:
		{
		}
		break;
	}

	//存盘
	player_ptr->SetDirty();

	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //执行成功
}

Asset::COMMAND_ERROR_CODE GmtManager::OnSystemBroadcast(const Asset::SystemBroadcast& command)
{
	Asset::SystemBroadcasting message;
	message.set_content(command.content());

	//WorldSessionInstance.BroadCast(message);

	return Asset::COMMAND_ERROR_CODE_SUCCESS; //直接返回，不用回复给GMT服务器
}
	
void GmtManager::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void GmtManager::SendProtocol(pb::Message& message)
{
	if (!Connected())
    {
        ERROR("尚未连接中心服服务器");
        return;
    }

    const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
    if (!field) return;

    int type_t = field->default_value_enum()->number();
    if (!Asset::INNER_TYPE_IsValid(type_t)) return; //如果不合法，不检查会宕线

    Asset::InnerMeta meta;
    meta.set_type_t((Asset::INNER_TYPE)type_t);
	meta.set_session_id(_session_id);
    meta.set_stuff(message.SerializeAsString());

    Asset::GmtInnerMeta gmt_meta;
    gmt_meta.set_inner_meta(meta.SerializeAsString());

    DEBUG("逻辑服务器处理来自会话:{}GMT指令后发送数据到中心服务器:{}", _session_id, gmt_meta.ShortDebugString());
	if (_session) _session->SendProtocol(gmt_meta);
}

void GmtManager::SendInnerMeta(const Asset::InnerMeta& message)
{
	if (!Connected())
    {
        ERROR("尚未连接中心服服务器");
        return;
    }

    Asset::GmtInnerMeta gmt_meta;
    gmt_meta.set_inner_meta(message.SerializeAsString());

    DEBUG("逻辑服务器处理GMT指令后发送数据到中心服务器:{}", gmt_meta.ShortDebugString());
    _session->SendProtocol(gmt_meta);
}
	
#undef RETURN
}
