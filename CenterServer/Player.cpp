#include <iostream>

#include <spdlog/spdlog.h>
#include <pbjson.hpp>
#include <cpp_redis/cpp_redis>

#include "Player.h"
#include "Clan.h"
#include "Timer.h"
#include "Mall.h"
#include "Protocol.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "PlayerName.h"
#include "PlayerCommonReward.h"
#include "PlayerCommonLimit.h"
#include "PlayerCoolDown.h"

#define MAX_PLAYER_COUNT 4

namespace Adoter
{

namespace spd = spdlog;
extern int32_t g_server_id;
extern const Asset::CommonConst* g_const;

using namespace std::chrono;

Player::~Player()
{
	//WorldSessionInstance.RemovePlayer(_player_id); //网络会话数据
}

Player::Player()
{
	//协议默认处理函数
	_method = std::bind(&Player::DefaultMethod, this, std::placeholders::_1);

	//
	//协议处理回调初始化
	//
	//如果没有在此注册，则认为到游戏逻辑服务器进行处理
	//
	//中心服务器只处理查询逻辑，修改数据逻辑需要到逻辑服务器进行处理
	//
	//AddHandler(Asset::META_TYPE_SHARE_BUY_SOMETHING, std::bind(&Player::CmdBuySomething, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_SHARE_SIGN, std::bind(&Player::CmdSign, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_SHARE_COMMON_PROPERTY, std::bind(&Player::CmdGetCommonProperty, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SAY_HI, std::bind(&Player::CmdSayHi, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_SHARE_GAME_SETTING, std::bind(&Player::CmdGameSetting, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_SHARE_ROOM_HISTORY, std::bind(&Player::CmdGetBattleHistory, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_SHARE_RECHARGE, std::bind(&Player::CmdRecharge, this, std::placeholders::_1));//逻辑服务器处理
	AddHandler(Asset::META_TYPE_SHARE_PLAY_BACK, std::bind(&Player::CmdPlayBack, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_MATCHING_STATS, std::bind(&Player::CmdGetMatchStatistics, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_CLAN_OPERATION, std::bind(&Player::CmdClanOperate, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_OPEN_MATCH, std::bind(&Player::CmdOpenMatch, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_JOIN_MATCH, std::bind(&Player::CmdJoinMatch, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_CLAN_MATCH_HISTORY, std::bind(&Player::CmdMatchHistory, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_CLAN_MATCH_DISMISS, std::bind(&Player::CmdDismissMatch, this, std::placeholders::_1));

	//AddHandler(Asset::META_TYPE_C2S_GET_REWARD, std::bind(&Player::CmdGetReward, this, std::placeholders::_1)); //逻辑服务器处理
}

Player::Player(int64_t player_id/*, std::shared_ptr<WorldSession> session*/) : Player()
{
	SetID(player_id);	
	//_session = session; //地址拷贝
}

bool Player::Connected() 
{ 
	//if (!_session) return false; 
	//return _session->IsConnect(); 
	return false;
}

int32_t Player::Load()
{
	if (_player_id == 0) return 1;

	auto key = "player:" + std::to_string(_player_id);
	auto loaded = RedisInstance.Get(key, _stuff);
	if (!loaded) return 2;

	_loaded = true;
	_dirty = false;

	DEBUG("玩家:{} 加载数据成功，内容:{}", _player_id, _stuff.ShortDebugString());

	return 0;
}

//
//数据存储，如果玩家当前在中心服则以中心服为准
//
//如果玩家已经不在中心服，则去游戏逻辑服进行存储，以免数据覆盖
//
//尽量减少中心服的数据存储
//
int32_t Player::Save(bool force)
{
	LOG_BI("player", _stuff);	

	//if (!force && !IsDirty()) return 1;
	//if (!force && !IsCenterServer()) return 2; 
	
	if (!force && !IsDirty()) return 1;
	if (!IsCenterServer()) return 2; //不能强制存盘，防止数据覆盖
	
	auto success = RedisInstance.SavePlayer(_player_id, _stuff); 
	if (!success) return 3;

	_dirty = false;
	
	DEBUG("玩家:{} 是否强制存盘:{} 服务器:{} 保存数据成功，内容:{}", _player_id, force, g_server_id, _stuff.ShortDebugString());
		
	return 0;
}

bool Player::IsExpire()
{
	//if (_expire_time == 0) return false;

	//return _expire_time < CommonTimerInstance.GetTime();
	return false;
}
	
int32_t Player::Logout(pb::Message* message)
{
	OnLogout();
	
	return 0;
}
	
int32_t Player::OnLogout()
{
	//_expire_time = CommonTimerInstance.GetTime() + 300; //30分钟之内没有上线，则删除

	if (IsInRoom()) 
	{
		ERROR("玩家:{} 游戏进行中，服务器:{}，房间:{} 不能从大厅退出", _player_id, _stuff.server_id(), _stuff.room_id());

		WorldSessionInstance.RemovePlayer(_player_id); //网络会话数据
		PlayerInstance.Remove(_player_id); //玩家管理
		return 1; //玩家在游戏进行中，不能退出//多网关模式,直接退出
	}

	_stuff.set_login_time(0);
	_stuff.set_logout_time(CommonTimerInstance.GetTime());
	
	Save(true);	//存档数据库

	WorldSessionInstance.RemovePlayer(_player_id); //网络会话数据
	PlayerInstance.Remove(_player_id); //玩家管理
			
	Asset::KickOutPlayer kickout_player; //通知游戏逻辑服务器退出
	kickout_player.set_player_id(_player_id);
	kickout_player.set_reason(Asset::KICK_OUT_REASON_LOGOUT);
	SendProtocol2GameServer(kickout_player); 

	DEBUG("玩家:{} 数据:{} 从大厅成功退出", _player_id, _stuff.ShortDebugString());

	return 0;
}

int32_t Player::OnEnterGame(bool is_login) 
{
	DEBUG("玩家:{}进入游戏，是否登陆:{} 是否已经加载数据:{}", _player_id, is_login, _loaded);

	if (!_loaded)
	{
		if (Load())
		{
			LOG(ERROR, "加载玩家{}数据失败", _player_id);
			return 1;
		}
	}

	_stuff.set_login_time(CommonTimerInstance.GetTime());
	_stuff.set_logout_time(0);

	Save(true); //存盘

	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理
	
	Asset::EnterGame enter_game;
	enter_game.set_player_id(_player_id);
	SendProtocol2GameServer(enter_game); //通知逻辑服务器

	OnLogin(is_login);

	return 0;
}

int32_t Player::OnEnterCenter() 
{
	if (Load()) 
	{
		ERROR("玩家:{}加载数据失败", _player_id);
		return 1;
	}
	
	_stuff.set_login_time(CommonTimerInstance.GetTime());
	_stuff.set_logout_time(0);

	_stuff.clear_room_id();
			
	//SetLocalServer(ConfigInstance.GetInt("ServerID", 1)); //架构调整

	DEBUG("玩家:{} 退出游戏逻辑服务器进入游戏大厅，数据内容:{}", _player_id, _stuff.ShortDebugString());

	auto session = WorldSessionInstance.GetPlayerSession(_player_id);
	if (!session || !session->IsConnect()) OnLogout(); //玩家管理//分享界面退出

	return 0;
}
	
int32_t Player::OnLogin(bool is_login)
{
	ActivityInstance.OnPlayerLogin(shared_from_this()); //活动数据

	//if (is_login) BattleHistory(); //历史对战表
	//if (is_login) MultiplyRoomCard(); //房卡翻倍//营口不再翻倍

	return 0;
}

void Player::SetLocalServer(int32_t server_id) 
{ 
	if (server_id <= 0 || server_id == _stuff.server_id()) return;
	
	//
	//通知当前游戏逻辑服务器下线
	//
	if (server_id && _stuff.server_id())
	{
		Asset::KickOutPlayer kickout_player; 
		kickout_player.set_player_id(_player_id);
		kickout_player.set_reason(Asset::KICK_OUT_REASON_CHANGE_SERVER);
		//SendProtocol2GameServer(kickout_player); 
	
		Asset::Meta meta;
		meta.set_type_t(Asset::META_TYPE_S2S_KICKOUT_PLAYER);
		meta.set_stuff(kickout_player.SerializeAsString());
		meta.set_player_id(_player_id); 

		auto gs_session = WorldSessionInstance.GetServerSession(_stuff.server_id());
		if (gs_session) gs_session->SendMeta(meta);  //通知逻辑服务器
		
		WARN("玩家:{} 通知当前游戏逻辑服务器下线，即将退出服务器:{} 进入服务器:{} 协议数据:{}", 
				_player_id, _stuff.server_id(), server_id, kickout_player.ShortDebugString());
	}
	
	//
	//切换逻辑服务器
	//
	_stuff.set_server_id(server_id); 
	_dirty = true;
	
	Save(true); //必须强制存盘，否则会覆盖数据
	
	//登陆//进入逻辑服务器
	//
	Asset::EnterGame enter_game;
	enter_game.set_player_id(_player_id);
	SendProtocol2GameServer(enter_game); //登陆逻辑服务器
}
	
bool Player::IsCenterServer() 
{ 
	bool is_center = _stuff.server_id() == 0 || _stuff.server_id() == g_server_id;

	if (!is_center) 
	{
		auto gs_session = WorldSessionInstance.GetServerSession(GetLocalServer());

		if (!gs_session) 
		{
			//WARN("玩家:{} 是否在中心服:{}", _player_id, is_center);
			return true; //如果逻辑服务器尚未在线，则认为玩家还在中心服务器
		}

		//WARN("玩家:{} 是否在中心服:{}", _player_id, is_center);
	}

	return is_center;
}
	
int64_t Player::ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (_stuff.common_prop().room_card_count() - count > 0)
	{
		_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() - count);
	}
	else
	{
		_stuff.mutable_common_prop()->set_room_card_count(0);
	}

	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}消耗房卡，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

int64_t Player::GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count) 
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}获得房卡，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

bool Player::CheckRoomCard(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().room_card_count();
	return curr_count >= count;
}

int64_t Player::GetRoomCard()
{
	return _stuff.common_prop().room_card_count();
}

int64_t Player::ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (_stuff.common_prop().huanledou() - count > 0)
	{
		_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() - count);
	}
	else
	{
		_stuff.mutable_common_prop()->set_huanledou(0);
	}

	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}消耗欢乐豆，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

int64_t Player::GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}获得欢乐豆，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

bool Player::CheckHuanledou(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().huanledou();
	return curr_count >= count;
}

int64_t Player::GetHuanledou() 
{ 
	return _stuff.common_prop().huanledou(); 
}

int64_t Player::GetDiamond() 
{ 
	return _stuff.common_prop().diamond(); 
}

int64_t Player::ConsumeDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (_stuff.common_prop().diamond() - count > 0)
	{
		_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() - count);
	}
	else
	{
		_stuff.mutable_common_prop()->set_diamond(0);
	}

	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}消耗钻石:{}原因:{}", _player_id, count, changed_type);
	return count;
}

int64_t Player::GainDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() + count);
	_dirty = true;

	SyncCommonProperty();

	LOG(INFO, "玩家:{}获取钻石:{}原因:{}", _player_id, count, changed_type);
	return count;
}

bool Player::CheckDiamond(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().diamond();
	return curr_count >= count;
}

int32_t Player::CmdSign(pb::Message* message)
{
	Asset::Sign* sign = dynamic_cast<Asset::Sign*>(message);
	if (!sign) return 1; 

	auto curr_t = CommonTimerInstance.GetTime(); //当前时间

	auto it = std::find_if(_stuff.sign_time().rbegin(), _stuff.sign_time().rend(), [curr_t](const int32_t& time) {
			return CommonTimerInstance.IsSameDay(curr_t, time); 
	});

	if (it == _stuff.sign_time().rend()) 
	{
		_stuff.mutable_sign_time()->Add(curr_t); //记录签到时间
		sign->set_success(true); //默认失败
	}

	//发奖
	auto asset_message = AssetInstance.Get(g_const->daily_sign_id());
	if (!asset_message) return 2;

	auto asset_sign = dynamic_cast<Asset::DailySign*>(asset_message);
	if (!asset_sign) return 3;

	auto common_limit_id = asset_sign->common_limit_id();
	if (!IsCommonLimit(common_limit_id)) DeliverReward(asset_sign->common_reward_id()); //正式发奖
	AddCommonLimit(common_limit_id); //今日已领取

	SendProtocol(sign);
	return 0;
}

int32_t Player::CmdGetCommonProperty(pb::Message* message)
{
	Asset::SyncCommonProperty* common_prop = dynamic_cast<Asset::SyncCommonProperty*>(message);
	if (!common_prop) return 1; 

	SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE_SYNC_REASON_TYPE_GET);
	return 0;
}
	
void Player::SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE reason)
{
	Asset::SyncCommonProperty common_prop;
	
	common_prop.set_reason_type(reason);
	common_prop.set_player_id(_player_id);
	common_prop.mutable_common_prop()->CopyFrom(_stuff.common_prop());

	SendProtocol(common_prop);
}

void Player::SendProtocol(const pb::Message* message)
{
	SendProtocol(*message);
}

void Player::SendProtocol(const pb::Message& message)
{
	//if (!Connected()) return;

	auto session = WorldSessionInstance.GetPlayerSession(_player_id);
	if (!session || !session->IsConnect()) return;

	session->SendProtocol(message);
	
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (Asset::META_TYPE_SHARE_CLAN_OPERATION == type_t) return; //日志过滤

	DEBUG("玩家:{} 发送协议:{}", _player_id, message.ShortDebugString());
}
	
void Player::SendMeta(const Asset::Meta& meta)
{
	auto session = WorldSessionInstance.GetPlayerSession(_player_id);
	if (!session || !session->IsConnect()) return;
	
	session->SendMeta(meta);
}
	
bool Player::SendProtocol2GameServer(const pb::Message& message)
{
	auto _gs_session = WorldSessionInstance.GetServerSession(GetLocalServer());
	if (!_gs_session) 
	{
		int64_t server_id = WorldSessionInstance.RandomServer(); //随机一个逻辑服务器
		if (server_id == 0) return false;
			
		SetLocalServer(server_id);
		_gs_session = WorldSessionInstance.GetServerSession(GetLocalServer());
	}
		
	auto debug_string = message.ShortDebugString();
	
	if (!_gs_session) 
	{
		LOG(ERROR, "玩家:{}未能找到合适发逻辑服务器，当前服务器:{}，协议内容:{}", _player_id, _stuff.server_id(), debug_string);
		return false;
	}

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return false;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return false;	//如果不合法，不检查会宕线

	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());
	meta.set_player_id(_player_id); 

	//DEBUG("玩家:{} 发送到游戏逻辑服务器:{} 协议类型:{} 内容:{}", _player_id, _stuff.server_id(), Asset::META_TYPE_Name(meta.type_t()), debug_string);

	_gs_session->SendMeta(meta); 

	return true;
}

bool Player::SendProtocol2GameServer(const pb::Message* message)
{
	if (!message) return false;

	SendProtocol2GameServer(*message); 
	return true;
}

void Player::SendGmtProtocol(const pb::Message* message, int64_t session_id)
{
	SendGmtProtocol(*message, session_id);
}

void Player::SendGmtProtocol(const pb::Message& message, int64_t session_id)
{
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::INNER_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	auto stuff = message.SerializeAsString(); //复制，防止析构
	
	Asset::InnerMeta meta;
	meta.set_type_t((Asset::INNER_TYPE)type_t);
	meta.set_session_id(session_id);
	meta.set_stuff(stuff);

	auto inner_meta = meta.SerializeAsString();
	Asset::GmtInnerMeta gmt_meta;
	gmt_meta.set_inner_meta(inner_meta);

	SendProtocol2GameServer(gmt_meta);
}

//
//玩家心跳周期为50ms
//
//如果该函数返回FALSE则表示掉线
//
bool Player::Update()
{
	++_heart_count; //心跳
	
	if (_heart_count % 20 == 0) //1s
	{
		if (_dirty) Save(); //触发存盘

		CommonLimitUpdate(); //通用限制,定时更新
	}
	
	//
	//大厅玩家(中心服务器上玩家)心跳时间5s，游戏逻辑服务器上玩家心跳3s
	//
	//if ((_heart_count % 100 == 0 && IsCenterServer()) || (_heart_count % 60 == 0 && !IsCenterServer())) SayHi();

	return true;
}
	
int32_t Player::DefaultMethod(pb::Message* message)
{
	if (!message) return 0;

	const pb::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName("type_t");
	if (!field) return 1;

	const pb::EnumValueDescriptor* enum_value = message->GetReflection()->GetEnum(*message, field);
	if (!enum_value) return 2;

	const std::string& enum_name = enum_value->name();
	ERROR("尚未找到玩家:{} 处理协议:{}", _player_id, enum_name);

	return 0;
}

int32_t Player::CommonCheck(int32_t type_t, pb::Message* message)
{
	if (!message) return 0;

	switch (type_t)
	{
		case Asset::META_TYPE_SHARE_CREATE_ROOM: //创建房间
		{
			if (g_const->guest_forbid_friend_room() && (_account_type == 0 || Asset::ACCOUNT_TYPE_GUEST == _account_type)) 
			{
				LOG(ERROR, "玩家:{} 创建房间失败:{} 账号信息:{}", _player_id, _account_type, _stuff.account());
				return Asset::ERROR_ROOM_FRIEND_NOT_FORBID; //游客禁止进入好友房
			}

			auto result = CheckCreateRoom(message);
			if (result) return result;
		
			int64_t server_id = 0;

			if (!IsInRoom()) server_id = WorldSessionInstance.RandomServer(); //随机一个逻辑服务器//防止茶馆老板房间内还创建房间

			/*
			if (server_id != 0 && server_id != GetLocalServer())
			{
				Asset::KickOutPlayer kickout_player; //通知当前游戏逻辑服务器下线
				kickout_player.set_player_id(_player_id);
				kickout_player.set_reason(Asset::KICK_OUT_REASON_CHANGE_SERVER);

				WARN("玩家:{} 创建房间，随机服务器:{} 当前所在服务器:{} 踢出当前服务器", _player_id, server_id, GetLocalServer());
				
				SendProtocol2GameServer(kickout_player); 
			}
			*/

			if (server_id > 0) SetLocalServer(server_id); //开房随机

			//WARN("玩家:{} 当前所在服务器:{} 开房随机服务器:{}", _player_id, _stuff.server_id(), server_id);
		}
		break;

		default:
		{
			return Asset::ERROR_SUCCESS; //无限制
		}
		break;
	}

	return Asset::ERROR_SUCCESS;
}
	
int32_t Player::CheckCreateRoom(pb::Message* message)
{
	if (!message) return Asset::ERROR_INNER;

	auto create_room = dynamic_cast<Asset::CreateRoom*>(message);
	if (!create_room) return Asset::ERROR_INNER;
	
	if (ActivityInstance.IsOpen(g_const->room_card_limit_free_activity_id())) return 0; //限免开启

	auto clan_id = create_room->room().clan_id();
	if (clan_id == 0) return 0; //茶馆

	auto clan = ClanInstance.Get(clan_id);
	if (!clan) return Asset::ERROR_CLAN_NOT_FOUND; //尚未存在茶馆 

	auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
	if (!clan_limit) return Asset::ERROR_CLAN_NOT_FOUND;

	auto room_gaming_count = clan->GetRoomOpenedCount();
	if (clan_limit->create_room_limit() < room_gaming_count) return Asset::ERROR_CLAN_ROOM_COUNT_LIMIT; //茶馆房间上限限制
	
	const Asset::Item_RoomCard* room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
	if (!room_card || room_card->rounds() <= 0) return Asset::ERROR_INNER; //消耗卡数据

	auto open_rands = create_room->room().options().open_rands(); //本次局数
	int32_t total_consume_room_card_count = open_rands / room_card->rounds(); //本次待消耗房卡数量
	
	const auto& rooms = clan->GetRooms();
	for (const auto& room : rooms)
	{
		if (room.second.curr_count()) continue; //已经开局

		int32_t open_rands = room.second.options().open_rands(); //局数
		int32_t consume_count = open_rands / room_card->rounds(); //待消耗房卡数量

		total_consume_room_card_count += consume_count; //待消耗房卡总数
	}

	if (clan->GetRoomCard() < total_consume_room_card_count) return Asset::ERROR_CLAN_ROOM_CARD_NOT_ENOUGH;  //房卡不足

	return 0;
}

bool Player::HandleProtocol(int32_t type_t, pb::Message* message)
{
	if (!message) return false;
	
	if (!Asset::META_TYPE_IsValid(type_t)) return false; //如果不合法，不检查会宕线

	DEBUG("当前玩家:{} 所在服务器:{} 协议类型:{} 接收协议数据:{}", _player_id, _stuff.server_id(), Asset::META_TYPE_Name((Asset::META_TYPE)type_t), message->ShortDebugString());
	//
	//如果中心服务器没有协议处理回调，则发往游戏服务器进行处理
	//
	//如果玩家已经在游戏逻辑服务器，则直接发往游戏逻辑服务器，防止数据覆盖
	//
	auto result = CommonCheck(type_t, message); //通用限制检查
	if (result)
	{
		AlertMessage(result, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //通用错误码
		return false;
	}
	
	auto it = _callbacks.find(type_t);
	if (it != _callbacks.end()) //中心服有处理逻辑，则直接在中心服处理
	{
		if (type_t == Asset::META_TYPE_SHARE_SAY_HI)
		{
			if (IsCenterServer()) { CmdSayHi(message); }
			else { SendProtocol2GameServer(message); } //中心服没有处理逻辑，转发给游戏逻辑服务器进行处理
		}
		else
		{
			CallBack& callback = GetMethod(type_t); 
			callback(std::forward<pb::Message*>(message));	
		}
	}
	else
	{
		SendProtocol2GameServer(message); //转发给游戏逻辑服务器进行处理
	}

	return true;
}

void Player::AlertMessage(int32_t error_code, Asset::ERROR_TYPE error_type/*= Asset::ERROR_TYPE_NORMAL*/, 
		Asset::ERROR_SHOW_TYPE error_show_type/* = Asset::ERROR_SHOW_TYPE_NORMAL*/)
{
	Asset::AlertMessage message;
	message.set_error_type(error_type);
	message.set_error_show_type(error_show_type);
	message.set_error_code(error_code);

	SendProtocol(message);
}

bool Player::AddCommonLimit(int64_t global_id)
{
	if (global_id <= 0) return false;
	return CommonLimitInstance.AddCommonLimit(shared_from_this(), global_id);
}
	
bool Player::IsCommonLimit(int64_t global_id)
{
	if (global_id <= 0) return false;
	return CommonLimitInstance.IsCommonLimit(shared_from_this(), global_id);
}

bool Player::CommonLimitUpdate()
{
	bool updated = CommonLimitInstance.Update(shared_from_this());
	if (updated) SyncCommonLimit();

	return updated;
}
	
bool Player::AddCoolDown(int64_t global_id)
{
	if (global_id <= 0) return false;
	return CoolDownInstance.AddCoolDown(shared_from_this(), global_id);
}

bool Player::IsCoolDown(int64_t global_id)
{
	if (global_id <= 0) return false;
	return CoolDownInstance.IsCoolDown(shared_from_this(), global_id);
}

void Player::SendPlayer()
{
	Asset::PlayerInformation player_info;
	player_info.mutable_player()->CopyFrom(this->_stuff);

	SendProtocol(player_info);
}

void Player::SyncCommonLimit()
{
	Asset::SyncCommonLimit proto;
	proto.mutable_common_limit()->CopyFrom(_stuff.common_limit());

	SendProtocol(proto);
}

Asset::ERROR_CODE Player::DeliverReward(int64_t global_id)
{
	auto ret_code = CommonRewardInstance.DeliverReward(shared_from_this(), global_id);
	if (ret_code != Asset::ERROR_SUCCESS) AlertMessage(ret_code);
		
	SyncCommonReward(global_id, ret_code);
	return ret_code;
}

void Player::SyncCommonReward(int64_t common_reward_id, int32_t error_code)
{
	Asset::SyncCommonReward proto;
	proto.set_common_reward_id(common_reward_id);
	proto.set_error_code(error_code);

	SendProtocol(proto);
}

int32_t Player::CmdGetReward(pb::Message* message)
{
	Asset::GetReward* get_reward = dynamic_cast<Asset::GetReward*>(message);
	if (!get_reward) return 1;

	int64_t reward_id = get_reward->reward_id();
	if (reward_id <= 0) return 2;

	switch (get_reward->reason())
	{
		case Asset::GetReward_GET_REWARD_REASON_GET_REWARD_REASON_DAILY_BONUS: //每日登陆奖励
		{
			int64_t daily_bonus_id = g_const->daily_bonus_id();
			if (reward_id != daily_bonus_id) return 3; //Client和Server看到的数据不一致
			
			auto message = AssetInstance.Get(daily_bonus_id);
			if (!message) return 4;

			auto bonus = dynamic_cast<Asset::DailyBonus*>(message);
			if (!bonus) return 5;

			auto ret = DeliverReward(bonus->common_reward_id()); //发奖
			AlertMessage(ret);
		}
		break;
		
		case Asset::GetReward_GET_REWARD_REASON_GET_REWARD_REASON_DAILY_ALLOWANCE: //每日补助奖励
		{
			int64_t daily_allowance_id = g_const->daily_allowance_id();
			if (reward_id != daily_allowance_id) return 3; //Client和Server看到的数据不一致
			
			auto message = AssetInstance.Get(daily_allowance_id);
			if (!message) return 4;

			auto allowance = dynamic_cast<Asset::DailyAllowance*>(message);
			if (!allowance) return 5;

			int32_t huanledou_below = allowance->huanledou_below(); 
			if (huanledou_below > 0 && huanledou_below < GetHuanledou())
			{
				AlertMessage(Asset::ERROR_HUANLEDOU_LIMIT); //欢乐豆数量不满足
				return 8;
			}

			auto ret = DeliverReward(allowance->common_reward_id()); //发奖
			AlertMessage(ret);
		}
		break;

		default:
		{
			DeliverReward(reward_id);
		}
		break;
	}

	return 0;
}
	
int32_t Player::CmdBuySomething(pb::Message* message)
{
	auto some_thing = dynamic_cast<Asset::BuySomething*>(message);
	if (!some_thing) return 1;

	int64_t mall_id = some_thing->mall_id();
	if (mall_id <= 0) return 2;

	auto ret = MallInstance.BuySomething(shared_from_this(), mall_id);
	some_thing->set_result(ret);
	SendProtocol(some_thing); //返回给Client

	if (ret) AlertMessage(ret, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX);
	
	LOG(INFO, "玩家:{} 购买商品:{} 结果:{}", _player_id, some_thing->ShortDebugString(), ret);
	return 0;
}

int32_t Player::CmdLuckyPlate(pb::Message* message)
{
	auto lucky_plate = dynamic_cast<Asset::PlayerLuckyPlate*>(message);
	if (!lucky_plate) return 1;

	auto asset_message = AssetInstance.Get(lucky_plate->plate_id());
	if (!asset_message) return 2;
	
	auto asset_lucky_plate = dynamic_cast<Asset::LuckyPlate*>(asset_message);
	if (!asset_lucky_plate) return 3;

	auto index = CommonUtil::RandomByWeight(asset_lucky_plate->plates().begin(), asset_lucky_plate->plates().end(), 
		[](const Asset::LuckyPlate_Plate& ele){
			return ele.weight();
	});

	if (index < 0 || index >= asset_lucky_plate->plates().size() || index >= asset_lucky_plate->plates_count()) return 4;

	auto common_reward_id = asset_lucky_plate->plates(index).common_reward_id();
	DeliverReward(common_reward_id); //发奖

	lucky_plate->set_result(index + 1); //Client从1开始
	SendProtocol(lucky_plate);

	return 0;
}

int32_t Player::CmdSayHi(pb::Message* message)
{
	auto say_hi = dynamic_cast<const Asset::SayHi*>(message);
	if (!say_hi) return 1;

	SayHi(); //回复心跳
    
	_pings_count = 0;
	_hi_time = CommonTimerInstance.GetTime(); 

	return 0;
}
	
void Player::SayHi()
{
	/*
	auto curr_time = CommonTimerInstance.GetTime();
	auto duration_pass = curr_time - _hi_time;

	if (duration_pass > 10)
	{
		++_pings_count;
		
		static int32_t max_allowed = 3;

		if (max_allowed && _pings_count > max_allowed) 
		{
			//SetOffline(); //玩家离线
		}
	}
	else
	{
		//SetOffline(false); //玩家上线
		
		_pings_count = 0;
	}
	*/

	Asset::SayHi message;
	message.set_heart_count(_heart_count);
	SendProtocol(message);

	DEBUG("玩家:{} 发送心跳:{}", _player_id, _hi_time);
}
	
int32_t Player::CmdGameSetting(pb::Message* message)
{
	auto game_setting = dynamic_cast<const Asset::GameSetting*>(message);
	if (!game_setting) return 1;

	_stuff.mutable_game_setting()->CopyFrom(game_setting->game_setting());
	
	SendProtocol(game_setting); //设置成功
	
	SetDirty();

	return 0;
}
	
int32_t Player::CmdRecharge(pb::Message* message)
{
	auto user_recharge = dynamic_cast<const Asset::UserRecharge*>(message);
	if (!user_recharge) return 1;
		
	const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_RECHARGE);

	for (auto it = messages.begin(); it != messages.end(); ++it)
	{
		auto recharge = dynamic_cast<Asset::Recharge*>(*it);
		if (!recharge) continue;

		if (user_recharge->product_id() != recharge->product_id()) continue;

		//if (recharge->price_show() != user_recharge->price()) continue; //价格不一致
		GainDiamond(Asset::DIAMOND_CHANGED_TYPE_MALL, recharge->gain_diamond());
		break;
	}

	return 0;
}

int32_t Player::CmdPlayBack(pb::Message* message)
{
	auto play_back = dynamic_cast<const Asset::PlayBack*>(message);
	if (!play_back) return 1;
	
	std::string key = "playback:" + std::to_string(play_back->room_id()) + "_" + std::to_string(play_back->game_index());
	
	Asset::PlayBack playback;
	auto has_record = RedisInstance.Get(key, playback);
	if (!has_record)
	{
		AlertMessage(Asset::ERROR_ROOM_PLAYBACK_NO_RECORD, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX);
		return 2;
	}

	SendProtocol(playback);
	
	return 0;
}

int32_t Player::CmdGetMatchStatistics(pb::Message* message)
{
	auto match_stats = dynamic_cast<Asset::MatchStats*>(message);
	if (!match_stats) return 1;

	std::unordered_map<int32_t, int32_t> room_list;
	
	Asset::MatchStatistics stats;
	RedisInstance.GetMatching(stats);

	for (const auto& server_element : stats.server_list())
	{
		//auto server_id = server_element.server_id();
		for (const auto& room_element : server_element.room_list())
		{
			auto room_type = room_element.room_type();
			auto player_count = room_element.player_count();

			room_list[room_type] = room_list[room_type] + player_count;
		}
	}

	for (auto room_match : room_list)
	{
		auto random_count = CommonUtil::Random(1, g_const->player_matching_random_count());

		auto room = match_stats->mutable_room_list()->Add();
		room->set_room_type((Asset::ROOM_TYPE)room_match.first);
		room->set_player_count(room_match.second + g_const->player_matching_base_count() + random_count);
	}

	SendProtocol(match_stats);

	return 0;
}

int32_t Player::CmdClanOperate(pb::Message* message)
{
	auto clan_oper = dynamic_cast<Asset::ClanOperation*>(message);
	if (!clan_oper) return 1;
	
	ClanInstance.OnOperate(shared_from_this(), clan_oper);
	return 0;
}

int32_t Player::CmdOpenMatch(pb::Message* message)
{
	auto open_match = dynamic_cast<Asset::OpenMatch*>(message);
	if (!open_match) return 1;

	auto clan_ptr = ClanInstance.Get(open_match->clan_id());
	if (!clan_ptr) return 2;
	
	clan_ptr->OnMatchOpen(shared_from_this(), open_match); //俱乐部部长开启比赛//此时并未真正开始比赛

	return 0;
}

int32_t Player::CmdJoinMatch(pb::Message* message)
{
	auto match = dynamic_cast<Asset::JoinMatch*>(message);
	if (!match) return 1;

	auto clan_ptr = ClanInstance.Get(match->clan_id());
	if (!clan_ptr) return 2;
	
	clan_ptr->OnJoinMatch(shared_from_this(), match);

	return 0;
}

int32_t Player::CmdMatchHistory(pb::Message* message)
{
	auto match = dynamic_cast<Asset::ClanMatchHistory*>(message);
	if (!match) return 1;

	auto clan_ptr = ClanInstance.Get(match->clan_id());
	if (!clan_ptr) return 2;
	
	clan_ptr->OnMatchHistory(shared_from_this(), match);

	return 0;
}

int32_t Player::CmdDismissMatch(pb::Message* message)
{
	auto match_dismiss = dynamic_cast<Asset::ClanMatchDismiss*>(message);
	if (!match_dismiss) return 1;
	
	auto clan_ptr = ClanInstance.Get(match_dismiss->clan_id());
	if (!clan_ptr) return 2;
	
	clan_ptr->OnMatchDismiss(shared_from_this(), match_dismiss);

	return 0;
}
	
void Player::OnKickOut(Asset::KICK_OUT_REASON reason)
{
	switch (reason)
	{
		case Asset::KICK_OUT_REASON_DISCONNECT: //玩家杀进程退出
		{
			if (IsCenterServer()) 
			{
				DEBUG("玩家:{}在中心服务器，尚不能发往游戏逻辑服:{}", _player_id, _stuff.server_id());
				break; //中心服没必要发往逻辑服务器//绝对不能
			}

			Asset::KickOutPlayer kickout_player; //通知游戏逻辑服务器退出
			kickout_player.set_player_id(_player_id);
			kickout_player.set_reason(reason);
			SendProtocol2GameServer(kickout_player); 
		}
		break;

		default:
		{
			if (!IsCenterServer()) return; 
		}
		break;
	}

	//
	//如果玩家主动退出，数据发送失败
	//
	//如果由于顶号，会优先发给在线玩家
	//
	Asset::KickOut kickout; //提示Client
	kickout.set_player_id(_player_id);
	kickout.set_reason(reason);
	SendProtocol(kickout); 

	Logout(nullptr);
}

void Player::SetOffline(bool offline)
{
	//
	//状态不变，则不进行推送
	//
	if (offline && _player_state == Asset::GAME_OPER_TYPE_OFFLINE) return;
	else if (_player_state == Asset::GAME_OPER_TYPE_ONLINE) return;

	if (offline)
	{
		_player_state = Asset::GAME_OPER_TYPE_OFFLINE;

		//ERROR("玩家:{}离线", _player_id);
	}
	else
	{
		_player_state = Asset::GAME_OPER_TYPE_ONLINE;

		//WARN("玩家:{}上线", _player_id);
	}
				
	Asset::PlayerState state;
	state.set_oper_type(_player_state);
	SendProtocol2GameServer(state);
}
	
bool Player::IsInRoom(int64_t room_id)
{
	if (room_id <= 0 || _stuff.room_id() <= 0) return false;

	return room_id == _stuff.room_id();
}
	
void PlayerManager::Emplace(int64_t player_id, std::shared_ptr<Player> player)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!player) return;

	_players[player_id] = player;
	
	DEBUG("插入玩家:{}成功，当前在线玩家数量:{}", player_id, _players.size());
}

std::shared_ptr<Player> PlayerManager::GetPlayer(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_mutex);

	auto it = _players.find(player_id);
	if (it == _players.end()) return nullptr;

	return it->second;
}

std::shared_ptr<Player> PlayerManager::Get(int64_t player_id)
{
	return GetPlayer(player_id);
}
	
bool PlayerManager::Has(int64_t player_id)
{
	auto player = GetPlayer(player_id);

	return player != nullptr;
}

void PlayerManager::Remove(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (player_id <= 0) return;

	auto it = _players.find(player_id);
	if (it == _players.end()) return;
		
	if (it->second) it->second.reset();

	_players.erase(it);
	
	DEBUG("删除玩家:{}成功，当前在线玩家数量:{}", player_id, _players.size());
}

void PlayerManager::Remove(std::shared_ptr<Player> player)
{
	if (!player) return;

	Remove(player->GetID());
}
	
void PlayerManager::BroadCast(const pb::Message& message)
{
	for (auto it = _players.begin(); it != _players.end(); ++it)
	{
		auto player = it->second;
		if (!player) continue;

		player->SendProtocol(message); //发送给Client
	}
}
	
//
//玩家心跳周期为50ms
//
void PlayerManager::Update(int32_t diff)
{
	++_heart_count;

	std::lock_guard<std::mutex> lock(_mutex);
	
	if (_heart_count % (20 * 60 * 30) == 0) 
	{
		int32_t player_count = WorldSessionInstance.GetOnlinePlayerCount();

		LOG(INFO, "在线玩家数量:{} 网络连接数量:{}", _players.size(), player_count); //30分钟查询一次
	}

	for (auto it = _players.begin(); it != _players.end();)
	{
		if (!it->second)
		{
			it = _players.erase(it);
			continue; 
		}
		else
		{
			it->second->Update();
			++it;
		}
	}
}
	
bool PlayerManager::BeenMaxPlayer()
{
	std::lock_guard<std::mutex> lock(_mutex);

	int32_t max_online_player = ConfigInstance.GetInt("MaxOnlinePlayer", 600);
	int32_t online_player = _players.size();

	return max_online_player < online_player; //在线玩家数量超过最大限制
}
	
int32_t PlayerManager::GetOnlinePlayerCount()
{
	std::lock_guard<std::mutex> lock(_mutex);

	return  _players.size();    
}
	
bool PlayerManager::GetCache(int64_t player_id, Asset::Player& player)
{
	return RedisInstance.Get("player:" + std::to_string(player_id), player);
}

bool PlayerManager::Save(int64_t player_id, Asset::Player& player)
{
	return RedisInstance.Save("player:" + std::to_string(player_id), player);
}
	
bool PlayerManager::IsLocal(int64_t player_id)
{
	int64_t server_id = player_id >> 20;
	return server_id == g_server_id;
}
	
bool PlayerManager::SendProtocol2GameServer(int64_t player_id, const pb::Message& message)
{
	//本地角色
	//
	if (IsLocal(player_id)) 
	{
		auto player = Get(player_id);
		if (!player) return false;
	
		player->SendProtocol2GameServer(message);
		return true; //本服角色在线直接发送数据
	}

	//跨服角色
	//
	Asset::Player stuff;

	auto loaded = PlayerInstance.GetCache(player_id, stuff);
	if (!loaded) return false;

	if (stuff.login_time() == 0) return false; //离线玩家不进行协议发送

	auto _gs_session = WorldSessionInstance.GetServerSession(stuff.server_id());
	if (!_gs_session) return false; //逻辑服务器
		
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return false;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return false;	//如果不合法，不检查会宕线

	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());
	meta.set_player_id(player_id); 

	DEBUG("玩家:{} 发送到游戏逻辑服务器:{}，协议类型:{} 内容:{}", player_id, stuff.server_id(), Asset::META_TYPE_Name(meta.type_t()), message.ShortDebugString());

	_gs_session->SendMeta(meta); 

	return true;
}

bool PlayerManager::SendProtocol2GameServer(int64_t player_id, const pb::Message* message)
{
	if (!message) return false;

	return SendProtocol2GameServer(player_id, *message);
}

}
