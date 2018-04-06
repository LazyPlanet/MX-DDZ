#include "GmtSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Player.h"
#include "Config.h"
#include "Timer.h"
#include "Activity.h"

//
//GMT Client 程序
//
//连接GMT服务器，SendProtocol为直接对GMT服务器进行的协议返回
//
//如果要向逻辑服务器进行数据转发，需要直接执行玩家SendGmtProtocol发送指令
//

namespace Adoter
{
#define RETURN(x) { \
	auto response = command; \
	response.set_error_code(x); \
	auto debug_string = command.ShortDebugString(); \
	if (x) { \
		LOG(ERR, "执行指令失败:{} 指令:{}", Asset::COMMAND_ERROR_CODE_Name(x), command.ShortDebugString()); \
	} else { \
		LOG(TRACE, "执行指令成功，指令:{}", command.ShortDebugString()); \
	} \
	SendProtocol(response); \
	return x; } \

GmtSession::GmtSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
	ClientSocket(io_service, endpoint)
{
	_remote_endpoint = endpoint;
	_ip_address = endpoint.address().to_string();
}
	
void GmtSession::OnConnected()
{
	ClientSocket::OnConnected();

	Asset::Register message;
	message.set_server_type(Asset::SERVER_TYPE_CENTER);
	message.set_server_id(ConfigInstance.GetInt("ServerID", 1)); //服务器ID，全球唯一

	SendProtocol(message); //注册服务器角色
}

bool GmtSession::OnInnerProcess(const Asset::InnerMeta& meta)
{
	std::lock_guard<std::mutex> lock(_gmt_lock);
	_session_id = meta.session_id();

	DEBUG("中心服务器接收GMT服务器:{} 协议:{}", _ip_address, meta.ShortDebugString());

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_REGISTER: //注册服务器成功
		{
			DEBUG("逻辑服务器注册到GMT服务器成功.");
		}
		break;

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

			int64_t server_id = WorldSessionInstance.RandomServer();

			auto session = WorldSessionInstance.GetServerSession(server_id);
			if (!session) return false;

			Asset::GmtInnerMeta inner_meta;
			inner_meta.set_inner_meta(meta.SerializeAsString());
			session->SendProtocol(inner_meta); //游戏逻辑均在逻辑服务器上进行
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
		break;

		case Asset::INNER_TYPE_ACTIVITY_CONTROL: //活动控制
		{
			Asset::ActivityControl message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnActivityControl(message);
		}
		break;
		
		case Asset::INNER_TYPE_BIND_PLAYER: //绑定玩家
		{
			Asset::BindPlayer command;
			auto result = command.ParseFromString(meta.stuff());
			if (!result) return false;
		
			auto player_ptr = PlayerInstance.Get(command.player_id());
			if (!player_ptr) 
			{
				RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_OFFLINE); //玩家目前不在线
				return false;
			}

			player_ptr->SendGmtProtocol(command, _session_id); //发给逻辑服务器处理
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
	
Asset::COMMAND_ERROR_CODE GmtSession::OnActivityControl(const Asset::ActivityControl& command)
{
	auto ret = ActivityInstance.OnActivityControl(command);
	RETURN(ret)
}

Asset::COMMAND_ERROR_CODE GmtSession::OnSendMail(const Asset::SendMail& command)
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

		if (!player_ptr->IsCenterServer()) //当前不在中心服务器，则到逻辑服务器进行处理
		{
			player_ptr->SendGmtProtocol(command, _session_id); //转发
			RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
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
			
Asset::COMMAND_ERROR_CODE GmtSession::OnCommandProcess(const Asset::Command& command)
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
			
	if (!player_ptr->IsCenterServer()) //当前不在中心服务器，则到逻辑服务器进行处理
	{
		player_ptr->SendGmtProtocol(command, _session_id); //转发
		//RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
		return Asset::COMMAND_ERROR_CODE_SUCCESS; //不返回协议
	}

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

Asset::COMMAND_ERROR_CODE GmtSession::OnSystemBroadcast(const Asset::SystemBroadcast& command)
{
	if (!IsConnected()) return Asset::COMMAND_ERROR_CODE_PARA;

	Asset::SystemBroadcasting message;
	message.set_broad_cast_type(Asset::SYSTEM_BROADCAST_TYPE_SCROLL);
	message.set_content(command.content());

	WorldSessionInstance.BroadCast(message);

	return Asset::COMMAND_ERROR_CODE_SUCCESS; //直接返回，不用回复给GMT服务器
}
	
void GmtSession::SendInnerMeta(const Asset::InnerMeta& meta)
{
	if (!IsConnected()) return;

	std::string content = meta.SerializeAsString();
	if (content.empty()) return;

	DEBUG("发送协议数据到GMT服务器:{} 协议数据:{}", _ip_address, meta.ShortDebugString());
	AsyncSendMessage(content);
}

void GmtSession::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void GmtSession::SendProtocol(pb::Message& message)
{
	if (!IsConnected()) return;

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::INNER_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	auto inner_meta_string = message.SerializeAsString();

	Asset::InnerMeta meta;
	meta.set_type_t((Asset::INNER_TYPE)type_t);
	meta.set_session_id(_session_id);
	meta.set_stuff(inner_meta_string);
	
	std::string content = meta.SerializeAsString();

	if (content.empty()) 
	{
		ERROR("server:{} send nothing, message:{}", _ip_address, meta.ShortDebugString());
		return;
	}

	DEBUG("发送协议数据到GMT服务器:{} 协议数据:{} 内容:{}", _ip_address, meta.ShortDebugString(), message.ShortDebugString());
	AsyncSendMessage(content);
}

bool GmtSession::StartSend()
{
	bool started = false;

	std::deque<std::string> send_list;
	send_list.swap(_send_list);

	while (IsConnected() && send_list.size())
	{
		auto& message = send_list.front();
		AsyncWriteSome(message.c_str(), message.size());
		send_list.pop_front();

		started = true;
	}
	return started;
}


void GmtSession::AsyncSendMessage(std::string message)
{
	if (IsClosed()) return;

	std::lock_guard<std::mutex> lock(_data_lock);
	_send_list.push_back(message);

	StartSend();
}

void GmtSession::OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;

	if (error)
	{
		Close(error.message());
		CRITICAL("server:{} send success, bytes_transferred:{}, error:{}", _ip_address, bytes_transferred, error.message());
		return;
	}

	DEBUG("server:{} send success, bytes_transferred:{}, error:{}", _ip_address, bytes_transferred, error.message());
}

bool GmtSession::StartReceive()
{
	if (!IsConnected()) return false;

	AsynyReadSome();
	return true;
}

void GmtSession::OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;
		
	if (error)
	{
		Close(error.message());
		return;
	}
	
	Asset::InnerMeta meta;
	auto result = meta.ParseFromArray(_buffer.data(), bytes_transferred);
	if (!result)
	{
		LOG(ERROR, "Receive message error from server:{} cannot parse from data.", _ip_address);
		return;
	}

	_receive_list.push_back(meta);

	std::deque<Asset::InnerMeta> received_messages;
	received_messages.swap(_receive_list);

	//数据处理
	while (!IsClosed() && !received_messages.empty())
	{
		const auto& message = received_messages.front();
		OnInnerProcess(message);
		received_messages.pop_front();
	}
	
	AsynyReadSome(); //继续下一次数据接收
}
	
bool GmtSession::Update() 
{
	ClientSocket::Update();

	return true;
}

#undef RETURN
}
