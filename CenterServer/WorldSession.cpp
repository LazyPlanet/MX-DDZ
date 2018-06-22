#include <spdlog/spdlog.h>
//#include <CkHttp.h>
//#include <cpp_redis/cpp_redis>

#include "WorldSession.h"
#include "RedisManager.h"
#include "CommonUtil.h"
#include "MXLog.h"
#include "Protocol.h"
#include "Player.h"
#include "PlayerName.h"
#include "Timer.h"
#include "bin2ascii.h"
#include "WhiteBlackManager.h"

namespace Adoter
{

namespace spd = spdlog;
extern int32_t g_server_id;
extern const Asset::CommonConst* g_const;
	
WorldSession::~WorldSession()
{
	Close(); //关闭网络
}

WorldSession::WorldSession(boost::asio::ip::tcp::socket&& socket) : Socket(std::move(socket))
{
	_remote_endpoint = _socket.remote_endpoint();
	_ip_address = _remote_endpoint.address().to_string();
			
	_hi_time = CommonTimerInstance.GetTime(); 
	DEBUG("地址:{} 端口:{} 连接成功", _ip_address, _remote_endpoint.port());
}

void WorldSession::InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred)
{
	try
	{
		if (error)
		{
			ERROR("地址:{} 端口:{} 玩家:{} 断开连接，错误码:{} 错误描述:{}", _ip_address, _remote_endpoint.port(), _player ? _player->GetID() : 0, error.value(), error.message());
			
			Close();
			return;
		}
		else
		{
			for (size_t index = 0; index < bytes_transferred;)
			{
				unsigned short body_size = _buffer[index] * 256 + _buffer[1 + index];
					
				if (body_size > MAX_DATA_SIZE)
				{
					LOG(ERROR, "接收来自地址:{} 端口:{} 玩家:{} 太大的包长:{} 丢弃.", _ip_address, _remote_endpoint.port(), _player ? _player->GetID() : 0, body_size)
					AsyncReceiveWithCallback(&WorldSession::InitializeHandler); //递归持续接收	
					return;
				}

				char buffer[MAX_DATA_SIZE] = {0}; //数据缓存  
				for (size_t i = 0; i < body_size; ++i) buffer[i] = _buffer[i + index + 2];  

				Asset::Meta meta;
				bool result = meta.ParseFromArray(buffer, body_size);

				if (!result) 
				{
					LOG(ERROR, "会话类型:{} 会话全局ID:{} 来自地址:{} 端口:{} 玩家:{} 转换Protobuff数据失败，数据长度:{}", 
							_role_type, _global_id, _ip_address, _remote_endpoint.port(), _player ? _player->GetID() : 0, body_size);

					AsyncReceiveWithCallback(&WorldSession::InitializeHandler); //递归持续接收	
					return; //非法协议
				}

				OnProcessMessage(meta);

				index += (body_size + 2); //下个包的起始位置
			}
		}
	}
	catch (std::exception& e)
	{
		ERROR("地址:{} 端口:{} 玩家:{} 断开连接，错误码:{}", _ip_address, _remote_endpoint.port(), _player ? _player->GetID() : 0, e.what());
		Close();

		KickOutPlayer(Asset::KICK_OUT_REASON_DISCONNECT);
		return;
	}

	AsyncReceiveWithCallback(&WorldSession::InitializeHandler); //递归持续接收	
}
			
void WorldSession::OnProcessMessage(const Asset::Meta& meta)
{
	if (meta.type_t() == Asset::META_TYPE_SHARE_BEGIN) return;

	pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
	if (!msg)
	{
		ERROR("会话类型:{} 会话全局ID:{} 尚未找到协议处理回调，协议类型:{} 对方IP地址:{}", _role_type, _global_id, meta.type_t(), _ip_address);
		return;		//非法协议
	}

	auto message = msg->New();

	defer {
		delete message;
		message = nullptr;
	};

	//if (meta.stuff().size() == 0) return; //可以发只包含type_t的协议
	
	auto result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
	if (!result) 
	{
		LOG(ERROR, "会话类型:{} 会话全局ID:{} 转换协议失败，协议类型:{} 对方IP地址:{}", _role_type, _global_id, meta.type_t(), _ip_address);
		return;		//非法协议
	}

	//显示协议内容
	const pb::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;

	const pb::EnumValueDescriptor* enum_value = message->GetReflection()->GetEnum(*message, field);
	if (!enum_value) return;

	if (Asset::META_TYPE_SHARE_SAY_HI != meta.type_t())
		DEBUG("中心服务器接收数据, 玩家:{} 全局角色ID:{} 全局角色类型:{} 协议名称:{} 内容:{}", meta.player_id(), _global_id, _role_type, enum_value->name(), message->ShortDebugString());

	//
	// C2S协议可能存在两种情况：
	//
	// 1.Client发送上来的数据;
	//
	// 2.转发到游戏逻辑服务器，经过逻辑服务器处理的结果返回;
	//
	//
	
	if (Asset::META_TYPE_C2S_COUNT <= meta.type_t()) 
	{
		//
		//游戏服务器内部协议处理
		//
		OnInnerProcess(meta); //内部处理

		//DEBUG("1.中心服务器接收游戏服务器内部协议:{}", meta.type_t());
	}
	else if (meta.player_id() > 0)
	{
		//DEBUG("2.中心服务器接收游戏服务器数据, 玩家:{} 协议:{}", meta.player_id(), meta.type_t());

		auto player = PlayerInstance.Get(meta.player_id());
		if (!player) 
		{
			ERROR("未能找到玩家:{} 协议数据:{}", meta.player_id(), message->ShortDebugString());
			return;
		}
		player->SendProtocol(message); //直接进行转发
	}
	else //if (meta.player_id() == 0)
	{
		//
		//游戏逻辑处理流程
		//
		//特别注意：不能传入player_id在Meta数据当中
		//
		//来自Client协议均在此处理，逻辑程序员请勿在此后添加代码
		//
		
		//DEBUG("3.中心服务器接收来自Client的协议:{}", meta.type_t());

		_pings_count = 0;
		_expire_time = 0;
		_hi_time = CommonTimerInstance.GetTime(); 
		
		if (Asset::META_TYPE_C2S_LOGIN == meta.type_t()) //账号登陆：MMO选人界面
		{
			Asset::Login* login = dynamic_cast<Asset::Login*>(message);
			if (!login) return; 

			//
			//1.黑白名单检查
			//
			if (WBInstance.EnabledWhite() && !WBInstance.IsWhite(_ip_address))
			{
				LOG(ERROR, "只能白名单进行登录，当前IP地址:{}", _ip_address);
				return;
			}
			
			if (WBInstance.EnabledBlack() && WBInstance.IsBlack(_ip_address))
			{
				LOG(ERROR, "黑名单不允许进行登录，当前IP地址:{}", _ip_address);
				return;
			}

			//
			//2.Client版本号检查
			//
			int32_t limit_version = g_const->limit_version();
			if (limit_version > 0 && _user.client_info().version() < limit_version)
			{
				AlertMessage(Asset::ERROR_VERSION_LIMIT, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //版本低
				LOG(ERROR, "Client版本不满足条件，最低版本要求:{}，Client版本:{}", limit_version, _user.client_info().version());
				return;
			}

			//
			//3.同时在线玩家限制
			//
			if (PlayerInstance.BeenMaxPlayer())
			{
				AlertMessage(Asset::ERROR_ONLINE_PLAYERS_LIMIT, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); 
				LOG(ERROR, "玩家达到最大在线数量，不能继续登陆");

				Close(); //关闭网络
				return;
			}
			
			_account.CopyFrom(login->account()); //账号信息
			auto success = RedisInstance.GetUser(login->account().username(), _user);
			if (!success) return;

			_user.mutable_client_info()->set_ip_address(_ip_address); //Client数据

			Asset::UpdateClientData client_data;
			client_data.mutable_client_info()->CopyFrom(_user.client_info());
			SendProtocol(client_data);

			//
			//如果账号下没有角色，则创建一个
			//
			//对于需要玩家自定义角色数据的游戏，此处要单独处理，比如需要选择性别
			//
			if (_user.player_list().size() == 0)
			{
				int64_t player_id = RedisInstance.CreatePlayer(); //如果账号下没有角色，创建一个给Client
				if (player_id == 0) return;
				
				_player = std::make_shared<Player>(player_id);
				
				WorldSessionInstance.AddPlayer(player_id, shared_from_this()); //在线玩家
				
				SetRoleType(Asset::ROLE_TYPE_PLAYER, _player->GetID());

				std::string player_name = NameInstance.Get();
				_player->SetName(player_name);
				_player->SetAccount(login->account().username(), _account.account_type());
				
				_player->SetRegisterServer(g_server_id);
				//_player->SetLocalServer(g_server_id); //触发服务器切换，中心服务器->逻辑服务器

				_player->Save(true); //存盘，防止数据库无数据//此时尚未加载到场景，因此无法通过心跳进行存盘
				_user.mutable_player_list()->Add(player_id);
				
				LOG(INFO, "账号:{} 下尚未创建角色，创建角色:{} 账号数据:{}", login->account().username(), player_id, _user.ShortDebugString());
			}

			if (_player_list.size()) _player_list.clear(); 
			for (auto player_id : _user.player_list()) _player_list.emplace(player_id); //玩家数据
			
			//
			//账号数据存储
			//
			RedisInstance.SaveUser(login->account().username(), _user);

			LOG_BI("account", _user); //账号查询
			
			//
			//发送当前的角色信息
			//
			Asset::PlayerList player_list; 
			player_list.mutable_player_list()->CopyFrom(_user.player_list());
			player_list.mutable_wechat()->CopyFrom(_user.wechat());
			SendProtocol(player_list); 
		}
		else if (Asset::META_TYPE_C2S_LOGOUT == meta.type_t()) //账号登出
		{
			Asset::Logout* logout = dynamic_cast<Asset::Logout*>(message);
			if (!logout) return; 

			OnLogout();
		}
		else if (Asset::META_TYPE_C2S_WECHAT_LOGIN == meta.type_t()) //微信登陆
		{
			Asset::WechatLogin* login = dynamic_cast<Asset::WechatLogin*>(message);
			if (!login) return; 

			OnWechatLogin(message); //初始化账号信息
		}
		else if (Asset::META_TYPE_SHARE_GUEST_LOGIN == meta.type_t()) //游客登陆
		{
			Asset::GuestLogin* login = dynamic_cast<Asset::GuestLogin*>(message);
			if (!login) return; 
			
			std::string account;

			if (_user.client_info().has_imei() && !_user.client_info().imei().empty()) //直接获取IEMI对应的游客账号，防止刷卡
			{
				auto has_key = RedisInstance.Get(_user.client_info().imei(), account);
				if (!has_key) 
				{
					LOG(ERROR, "玩家账号游客登录错误，手机唯一识别码:{} 已经存在游客账号,不能再次游客登录，防止刷卡.", _user.client_info().imei());
					return;
				}
			}
			else
			{
				if (RedisInstance.GetGuestAccount(account)) 
				{
					login->set_account(account);

					_account.set_account_type(Asset::ACCOUNT_TYPE_GUEST); 
					_account.set_username(login->account()); //账号信息
				}
				
				RedisInstance.Save(_user.client_info().imei(), account); //存储IMEI和账号对应关系
			}

			_user.mutable_account()->CopyFrom(_account);
			_user.set_created_time(CommonTimerInstance.GetTime()); //创建时间

			RedisInstance.SaveUser(account, _user); //存盘

			SendProtocol(login); 
		}
		else if (Asset::META_TYPE_C2S_RECONNECT == meta.type_t()) //断线重连
		{
			Asset::ReConnect* connect = dynamic_cast<Asset::ReConnect*>(message);
			if (!connect) return; 

			_player = PlayerInstance.Get(connect->player_id());

			if (!_player) 
			{
				_player = std::make_shared<Player>(connect->player_id()); //服务器已经没有缓存

				auto load_success = _player->Load();
				if (load_success) return;
			}
			else
			{
				if (!_player->IsCenterServer()) _player->Load();  
			}
			
			if (connect->has_account())
			{
				_account.CopyFrom(connect->account()); //账号信息
				_player->SetAccount(_account.username(), _account.account_type());
			}
				
			auto success = RedisInstance.GetUser(_account.username(), _user);
			if (!success) 
			{
				AlertMessage(Asset::ERROR_LONGTIME_NO_OPERATION, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); 
				return;
			}
				
			SetRoleType(Asset::ROLE_TYPE_PLAYER, _player->GetID());
			WorldSessionInstance.AddPlayer(connect->player_id(), shared_from_this()); //在线玩家，获取网络会话

			//_player->SetLocalServer(ConfigInstance.GetInt("ServerID", 1)); //架构调整，玩家直接在逻辑服务器处理逻辑
			_player->OnEnterGame(false);
		}
		else if (Asset::META_TYPE_C2S_GET_ROOM_DATA == meta.type_t()) //获取房间数据
		{
			auto get_data = dynamic_cast<Asset::GetRoomData*>(message);
			if (!get_data) return;
			
			int64_t server_id = RedisInstance.GetServer(get_data->room_id());

			auto game_server = WorldSessionInstance.GetServerSession(server_id);
			if (!game_server || !_player) return;

			_player->SetLocalServer(server_id); //设置玩家当前所在服务器
			_player->SendProtocol2GameServer(get_data); 
		}
		else if (Asset::META_TYPE_C2S_ENTER_GAME == meta.type_t()) //进入游戏：MMO进入场景
		{
			const Asset::EnterGame* enter_game = dynamic_cast<Asset::EnterGame*>(message);
			if (!enter_game) return; 

			if (enter_game->player_id() == 0) return;

			if (_player_list.find(enter_game->player_id()) == _player_list.end()) return; //账号下没有该角色数据

			_player = PlayerInstance.Get(enter_game->player_id());

			if (!_player) 
			{
				_player = std::make_shared<Player>(enter_game->player_id());
				WorldSessionInstance.AddPlayer(enter_game->player_id(), shared_from_this()); //在线玩家
			}

			/*
			if (_player->Load())
			{
				LOG(ERROR, "玩家进入游戏，角色ID:{} 加载数据失败", enter_game->player_id());
				return; //数据加载失败必须终止
			}
			*/

			SetRoleType(Asset::ROLE_TYPE_PLAYER, _player->GetID());
			
			//
			//必须放在角色初始化之后，后面很多操作都依赖此处，比如存盘
			//
			//设置玩家所在服务器，每次进入场景均调用此
			//
			//对于MMORPG游戏，可以是任意一个场景或副本ID，此处记录为解决全球唯一服，通过Redis进行进程间通信，获取玩家所在服务器ID.
			//
			//_player->SetLocalServer(ConfigInstance.GetInt("ServerID", 1)); //架构调整，玩家直接在逻辑服务器处理逻辑
			
			//
			//账号未能存储成功在角色数据，则重新存储
			//
			//if (_player->GetAccount().empty()) _player->Save(true); //存盘，防止数据库无数据
				
			_player->SetAccount(_account.username(), _account.account_type());

			//
			//已经在线玩家检查
			//
			//对于已经进入游戏内操作的玩家进行托管
			//
			WorldSessionInstance.AddPlayer(_player->GetID(), shared_from_this()); //在线玩家
			
			//
			//此时才可以真正进入游戏大厅
			//
			//加载数据
			//
			if (_player->OnEnterGame()) //理论上不会出现
			{
				_player->AlertMessage(Asset::ERROR_DATABASE);
				return;
			}
			
			PlayerInstance.UpdateUser(_player->GetID(), _user);
		}
		else if (Asset::META_TYPE_C2S_SWITCH_ACCOUNT == meta.type_t()) //切换账号
		{
			Asset::SwitchAccount* switch_account = dynamic_cast<Asset::SwitchAccount*>(message);
			if (!switch_account) return; 

			if (switch_account->account_name() != _account.username()) return;

			if (!_player || _player->GetID() != switch_account->player_id()) return;

			if (_user.player_list().size()) RedisInstance.SaveUser(_account.username(), _user); //存盘退出

			//_player->Save(true);
	
			_player->Logout(nullptr);
			_player.reset();
			
			_user.Clear();
			_account.Clear();
	
			if (_player_list.size()) _player_list.clear(); 
		}
		else if (Asset::META_TYPE_SHARE_ENTER_ROOM == meta.type_t()) //进入房间：进入游戏逻辑服务器的入口
		{
			//
			//进入房间根据房间号选择房间所在逻辑服务器进行会话选择
			//
			if (!_player) return;

			Asset::EnterRoom* enter_room = dynamic_cast<Asset::EnterRoom*>(message);
			if (!enter_room) return; 

			int64_t room_id = enter_room->room().room_id();
			/*
			if (_player->IsInRoom(room_id)) 
			{
				LOG(ERROR, "玩家:{} 已经在房间:{} 本次加入房间:{} 协议内容:{}", 
						_player->GetID(), _player->GetRoom(), room_id, enter_room->ShortDebugString());
				return; //已经在另一个房间内
			}
			*/

			int64_t game_type = RedisInstance.GetGameType(room_id);
			int64_t server_id = RedisInstance.GetServer(room_id);

			auto room_type = enter_room->room().room_type();

			if (Asset::ROOM_TYPE_FRIEND == room_type)
			{
				if (g_const->guest_forbid_friend_room() && 
						(_account.account_type() == 0 || Asset::ACCOUNT_TYPE_GUEST == _account.account_type())) //游客禁止进入好友房
				{
					_player->AlertMessage(Asset::ERROR_ROOM_FRIEND_NOT_FORBID, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //通用错误码
					return;
				}

				auto game_server = WorldSessionInstance.GetServerSession(server_id);
				if (!game_server) 
				{
					enter_room->set_error_code(Asset::ERROR_ROOM_NOT_FOUNT);
					SendProtocol(message);

					_player->AlertMessage(Asset::ERROR_ROOM_NOT_FOUNT, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //通用错误码
					return;
				}
			}
			else
			{
				if (_player->IsCoolDown(Asset::SYSTEM_COOLDOWN_TYPE_MATCHING))
				{
					ERROR("玩家:{} 冷却中...", _player->GetID());
					_player->AlertMessage(Asset::ERROR_COOLDOWN_MATCHING);
					return;
				}

					/*
				do 
				{
					if (!_player->IsCenterServer()) 
					{
						WARN("玩家:{} 已经在逻辑服务器上,直接进行匹配.", _player->GetID());
						break;
					}
					*/

					if (server_id == 0) server_id = WorldSessionInstance.RandomServer(); //随机一个逻辑服务器
					
					WARN("玩家:{} 逻辑服务器:{} 进入房间:{}", _player->GetID(), server_id, enter_room->ShortDebugString());

					if (server_id == 0) 
					{
						ERROR("玩家:{} 进入匹配房,所在服务器:{} 玩家传入房间错误,错误信息:{}", 
								_player->GetID(), _player->GetLocalServer(), enter_room->ShortDebugString());
						return;
					}

				//} while(false);
				
				_player->AddCoolDown(Asset::SYSTEM_COOLDOWN_TYPE_MATCHING);
			}
				
			/*
			if (!_player->IsCenterServer() && server_id > 0 && _player->GetLocalServer() != server_id)
			{
				Asset::KickOutPlayer kickout_player; //通知当前游戏逻辑服务器下线
				kickout_player.set_player_id(_player->GetID());
				kickout_player.set_reason(Asset::KICK_OUT_REASON_CHANGE_SERVER);

				WARN("玩家:{} 进入服务器:{} 房间:{}和当前缓存服务器:{}，房间:{}不同，发往原服踢出:{}", 
						_player->GetID(), server_id, _player->GetRoom(), room_id, _player->GetLocalServer(), kickout_player.ShortDebugString());

				_player->SendProtocol2GameServer(kickout_player); 
			}
			*/
	
			WARN("玩家:{} 当前所在服务器:{} 即将进入逻辑服务器:{} 游戏类型:{} 进入房间:{}", 
					_player->GetID(), _player->GetLocalServer(), server_id, game_type, enter_room->ShortDebugString());

			if (room_id) _player->SetRoom(room_id); //设置房间
			if (server_id) _player->SetLocalServer(server_id); //设置玩家当前所在服务器
			_player->SendProtocol2GameServer(enter_room); //转发
		}
		else if (Asset::META_TYPE_SHARE_UPDATE_CLIENT_DATA == meta.type_t()) //Client参数数据
		{
			auto client_data = dynamic_cast<Asset::UpdateClientData*>(message);
			if (!client_data) return;

			if (!_player) return;

			_user.mutable_client_info()->set_ip_address(_ip_address); //服务器存储
			_user.mutable_client_info()->mutable_location()->CopyFrom(client_data->client_info().location());
				
			//auto redis = make_unique<Redis>();
			RedisInstance.SetLocation(_player->GetID(), client_data->client_info().location()); //位置信息

			client_data->mutable_client_info()->CopyFrom(_user.client_info());
			SendProtocol(message);
		}
		else if (Asset::META_TYPE_SHARE_SAY_HI == meta.type_t() && _role_type == Asset::ROLE_TYPE_GAME_SERVER) //心跳
		{
			SendProtocol(message);
		}
		else
		{
			if (!_player && meta.type_t() != Asset::META_TYPE_SHARE_SAY_HI) 
			{
				LOG(ERROR, "玩家尚未初始化，收到协议:{}", meta.type_t());
				return; 
			}

			if (_player) _player->HandleProtocol(meta.type_t(), message); //协议处理
		}
	}
}

void WorldSession::KickOutPlayer(Asset::KICK_OUT_REASON reason)
{
	DEBUG("角色类型:{} 全局ID:{} 被踢下线,原因:{}", _role_type, _global_id, reason);

	if (_global_id == 0 || _role_type == Asset::ROLE_TYPE_NULL) return;
	
	if (_role_type == Asset::ROLE_TYPE_GAME_SERVER) //逻辑服务器
	{
	    WorldSessionInstance.RemoveServer(_global_id);
	}
	else if (_role_type == Asset::ROLE_TYPE_PLAYER) //玩家
	{
		if (!_player) return;

		auto session = WorldSessionInstance.GetPlayerSession(_player->GetID());

		if (session && session->GetRemotePoint() != GetRemotePoint()) 
		{
			ERROR("服务器主动断开连接，角色类型:{}全局ID:{}会话失效,原因:玩家连接冗余，删除废弃连接", _role_type, _global_id);
			return;
		}

		_player->OnKickOut(reason); //玩家退出登陆
	}
}
	
void WorldSession::OnLogout()
{
	if (!_player) return;
	
	_player->Logout(nullptr);
	//_online = false;
}
	
int32_t WorldSession::OnWechatLogin(const pb::Message* message)
{
	const Asset::WechatLogin* login = dynamic_cast<const Asset::WechatLogin*>(message);
	if (!login) return 1; 

	auto key = "user:" + login->wechat().openid();
	auto success = RedisInstance.Get(key, _user);

	if (!success) _user.set_created_time(CommonTimerInstance.GetTime()); //创建时间

	if (!login->wechat().openid().empty()) 
	{
		_user.mutable_wechat()->CopyFrom(login->wechat()); //微信数据
		RedisInstance.SaveUser(_user.wechat().openid(), _user); //账号数据存盘//每次都更新防止玩家修改了个人信息
	}

	//DEBUG("微信登陆数据:{} 缓存数据:{}", login->ShortDebugString(), _user.ShortDebugString());

	return 0;
}

void WorldSession::Start()
{
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler);
}
	
//
//周期10MS
//
bool WorldSession::Update() 
{ 
	++_heart_count;

	if (!Socket::Update()) return false;

	if (_heart_count % 60 == 0) OnHeartBeat1s();
	if (_heart_count % 3600 == 0) OnHeartBeat1m();

	return true;
}

void WorldSession::OnHeartBeat1s()
{
	auto duration_pass = TimerInstance.GetTime() - _hi_time;

	if (duration_pass > 30 && _expire_time == 0) 
	{
		_expire_time = TimerInstance.GetTime() + 60 * 3; //3mins之内没有上线(进行网络收发)，则删除
	}
}

void WorldSession::OnHeartBeat1m()
{
	if (IsExpire()) 
	{
		WARN("角色类型:{} 全局ID:{} 地址:{} 由于长时间未进行操关闭网络连接", _role_type, _global_id, _ip_address);
		Close(); //关闭网络连接
	}
}

bool WorldSession::IsExpire()
{
	if (_role_type == Asset::ROLE_TYPE_GAME_SERVER) return false;

	if (_expire_time == 0) return false;

	return _expire_time < TimerInstance.GetTime();
}

void WorldSession::OnClose()
{
	Socket::OnClose();
				
	KickOutPlayer(Asset::KICK_OUT_REASON_DISCONNECT);
	
	//WorldSessionInstance.RemovePlayer(_global_id); //网络会话数据

	//OnLogout();
	
	DEBUG("角色类型:{} 全局ID:{} 关闭网络连接", _role_type, _global_id);
}

void WorldSession::SendProtocol(const pb::Message* message)
{
	SendProtocol(*message);
}

void WorldSession::SendProtocol(const pb::Message& message)
{
	if (!IsConnect()) return; //网络已经断开

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线

	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());
	
	std::string content = meta.SerializeAsString();
	if (content.empty()) return;

	EnterQueue(std::move(content));
}

void WorldSession::SendMeta(const Asset::Meta& meta)
{
	if (!IsConnect()) return; //网络已经断开

	std::string content = meta.SerializeAsString();
	if (content.empty()) return;

	EnterQueue(std::move(content));
}

void WorldSession::AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type/*= Asset::ERROR_TYPE_NORMAL*/, Asset::ERROR_SHOW_TYPE error_show_type/* = Asset::ERROR_SHOW_TYPE_NORMAL*/)
{
	Asset::AlertMessage message;
	message.set_error_type(error_type);
	message.set_error_show_type(error_show_type);
	message.set_error_code(error_code);

	SendProtocol(message);
}

void WorldSessionManager::BroadCast2GameServer(const pb::Message* message)
{
	if (!message) return;
	BroadCast2GameServer(*message);
}

void WorldSessionManager::BroadCast2GameServer(const pb::Message& message)
{
	std::lock_guard<std::mutex> lock(_server_mutex);

	for (auto session : _server_list)
	{
		if (!session.second) continue;
		session.second->SendProtocol(message);
	}
}
	
void WorldSessionManager::SendProtocol2GameServer(int32_t server_id, const pb::Message& message)
{
	auto session = GetServerSession(server_id);
	if (!session) return;

	session->SendProtocol(message);
}

void WorldSessionManager::SendProtocol2GameServer(int32_t server_id, const pb::Message* message)
{
	if (server_id <= 0 || !message) return;

	SendProtocol2GameServer(server_id, *message);
}
	
void WorldSessionManager::BroadCast(const pb::Message& message)
{
	std::lock_guard<std::mutex> lock(_client_mutex);

	for (auto session : _client_list)
	{
		if (!session.second) continue;
		session.second->SendProtocol(message);
	}
}

void WorldSessionManager::BroadCast(const pb::Message* message)
{
	if (!message) return;
	BroadCast(*message);
}

int64_t WorldSessionManager::RandomServer()
{
	std::lock_guard<std::mutex> lock(_server_mutex);

	if (_server_list.size() == 0) return 0;

	std::vector<int32_t> server_list;

	for (auto it = _server_list.begin(); it != _server_list.end(); ) 
	{
		if (it->first == 0 || !it->second)
		{
			it = _server_list.erase(it);
		}
		else
		{
			server_list.push_back(it->first);

			++it;
		}
	}
	
	if (server_list.size() == 0) return 0;

	std::random_shuffle(server_list.begin(), server_list.end()); //随机

	return server_list[0];
}

NetworkThread<WorldSession>* WorldSessionManager::CreateThreads() const
{    
	return new NetworkThread<WorldSession>[GetNetworkThreadCount()];
}

void WorldSessionManager::OnSocketAccept(tcp::socket&& socket, int32_t thread_index)
{    
	WorldSessionInstance.OnSocketOpen(std::forward<tcp::socket>(socket), thread_index);
}

bool WorldSessionManager::StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count)
{
	if (!SuperSocketManager::StartNetwork(io_service, bind_ip, port, thread_count)) return false;
	_acceptor->SetSocketFactory(std::bind(&SuperSocketManager::GetSocketForAccept, this));    
	_acceptor->AsyncAcceptWithCallback<&OnSocketAccept>();    
	return true;
}
	
void WorldSessionManager::AddPlayer(int64_t player_id, std::shared_ptr<WorldSession> session) 
{ 
	if (player_id <= 0 || !session) return;

	std::lock_guard<std::mutex> lock(_client_mutex);

	auto it = _client_list.find(player_id);
	if (it != _client_list.end() && it->second/* && session != it->second*/) 
	{
		//it->second->Close(); //断线后进行连接，导致之前网络连接释放KickOutPlayer被调用删除玩家
		it->second.reset();
	}

	_client_list[player_id] = session; 
}

void WorldSessionManager::RemovePlayer(int64_t player_id) 
{ 
	std::lock_guard<std::mutex> lock(_client_mutex);

	auto it = _client_list.find(player_id);
	if (it == _client_list.end()) return;
	
	if (it->second) 
	{
		//it->second->Close(); //创建账号后会导致Player析构调用删除，从而玩家会有一次掉线
		it->second.reset();
	}

	_client_list.erase(it); 
}
	
std::shared_ptr<WorldSession> WorldSessionManager::GetPlayerSession(int64_t player_id) 
{ 
	std::lock_guard<std::mutex> lock(_client_mutex);

	auto it = _client_list.find(player_id);
	if (it == _client_list.end()) return nullptr;

	return it->second;
}

int32_t WorldSessionManager::GetOnlinePlayerCount()
{
	std::lock_guard<std::mutex> lock(_client_mutex);

	return _client_list.size();
}

void WorldSessionManager::AddServer(int64_t server_id, std::shared_ptr<WorldSession> session) 
{	
	if (server_id <= 0 || !session) return;

	std::lock_guard<std::mutex> lock(_server_mutex);

	auto it = _server_list.find(server_id);
	if (it != _server_list.end() && it->second) it->second.reset();

	_server_list[server_id] = session;
}	

void WorldSessionManager::RemoveServer(int64_t server_id) 
{ 
	std::lock_guard<std::mutex> lock(_server_mutex);
 
	auto it = _server_list.find(server_id);
	if (it == _server_list.end()) return;
 
	_server_list.erase(it); 
}

std::shared_ptr<WorldSession> WorldSessionManager::GetServerSession(int64_t server_id) 
{ 
	std::lock_guard<std::mutex> lock(_server_mutex);
	
	auto it = _server_list.find(server_id);
	if (it == _server_list.end()) return nullptr;

	return it->second; 
}

}
