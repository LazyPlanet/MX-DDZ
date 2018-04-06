#include <iostream>

#include <spdlog/spdlog.h>
#include <pbjson.hpp>
#include <cpp_redis/cpp_redis>

#include "Player.h"
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
	AddHandler(Asset::META_TYPE_SHARE_BUY_SOMETHING, std::bind(&Player::CmdBuySomething, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SIGN, std::bind(&Player::CmdSign, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_COMMON_PROPERTY, std::bind(&Player::CmdGetCommonProperty, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SAY_HI, std::bind(&Player::CmdSayHi, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_GAME_SETTING, std::bind(&Player::CmdGameSetting, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_ROOM_HISTORY, std::bind(&Player::CmdGetBattleHistory, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_RECHARGE, std::bind(&Player::CmdRecharge, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_PLAY_BACK, std::bind(&Player::CmdPlayBack, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_PLAY_BACK, std::bind(&Player::CmdGetMatchStatistics, this, std::placeholders::_1));

	AddHandler(Asset::META_TYPE_C2S_GET_REWARD, std::bind(&Player::CmdGetReward, this, std::placeholders::_1));
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

	/*
	cpp_redis::future_client client;
	client.connect(ConfigInstance.GetString("Redis_ServerIP", "127.0.0.1"), ConfigInstance.GetInt("Redis_ServerPort", 6379));
	if (!client.is_connected()) return 2;
	
	auto has_auth = client.auth(ConfigInstance.GetString("Redis_Password", "!QAZ%TGB&UJM9ol."));
	if (has_auth.get().ko()) return 3;

	auto get = client.get("player:" + std::to_string(_player_id));
	cpp_redis::reply reply = get.get();
	client.commit();

	if (!reply.is_string()) return 4;

	auto success = _stuff.ParseFromString(reply.as_string());
	if (!success) return 5;
	*/

	auto key = "player:" + std::to_string(_player_id);
	auto loaded = RedisInstance.Get(key, _stuff);
	if (!loaded) return 2;

	_loaded = true;

	DEBUG("玩家:{}加载数据成功，内容:{}", _player_id, _stuff.ShortDebugString());

	return 0;
}

//
//数据存储，如果玩家当前在中心服则以中心服为准
//
//如果玩家已经不在中心服，则去游戏逻辑服进行存储
//
//以免数据覆盖
//
int32_t Player::Save(bool force)
{
	LOG_BI("player", _stuff);	

	if (!force && !IsDirty()) return 1;

	if (!IsCenterServer()) return 2; 

	//auto redis = make_unique<Redis>();
	auto success = RedisInstance.SavePlayer(_player_id, _stuff); 
	if (!success) return 3;

	_dirty = false;
	
	DEBUG("玩家:{}保存数据成功，内容:{}", _player_id, _stuff.ShortDebugString());
		
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

	if (!IsCenterServer()) 
	{
		ERROR("玩家:{}游戏进行中，服务器:{}，房间:{} 不能从大厅退出", _player_id, _stuff.server_id(), _stuff.room_id());
		//WorldSessionInstance.RemovePlayer(_player_id); //网络会话数据
		//return 1; //玩家在游戏进行中，不能退出//多网关模式,直接退出
	}

	_stuff.set_login_time(0);
	_stuff.set_logout_time(CommonTimerInstance.GetTime());
	
	Save(true);	//存档数据库

	WorldSessionInstance.RemovePlayer(_player_id); //网络会话数据
	PlayerInstance.Remove(_player_id); //玩家管理

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

	if (is_login) SendPlayer(); //发送数据给玩家
	
	_stuff.set_login_time(CommonTimerInstance.GetTime());
	_stuff.set_logout_time(0);

	Save(true); //存盘

	LOG_BI("player", _stuff);
	
	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理

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
			
	SetLocalServer(ConfigInstance.GetInt("ServerID", 1));

	//Save(true); //存盘
	
	DEBUG("玩家:{}退出游戏逻辑服务器进入游戏大厅，数据内容:{}", _player_id, _stuff.ShortDebugString());

	auto session = WorldSessionInstance.GetPlayerSession(_player_id);
	if (!session || !session->IsConnect()) OnLogout(); //玩家管理//分享界面退出

	return 0;
}
	
int32_t Player::OnLogin(bool is_login)
{
	ActivityInstance.OnPlayerLogin(shared_from_this()); //活动数据

	if (is_login) BattleHistory(); //历史对战表
	if (is_login) MultiplyRoomCard(); //房卡翻倍

	return 0;
}

void Player::SetLocalServer(int32_t server_id) 
{ 
	if (server_id == _stuff.server_id()) return;

	_stuff.set_server_id(server_id); 

	Save(true); //必须强制存盘，否则会覆盖数据
}
	
bool Player::IsCenterServer() 
{ 
	int32_t curr_server_id = ConfigInstance.GetInt("ServerID", 1);

	return _stuff.server_id() == 0 || _stuff.server_id() == curr_server_id;
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

	//调试
	//const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	//if (!field) return;

	//const pb::EnumValueDescriptor* enum_value = message.GetReflection()->GetEnum(message, field);
	//if (!enum_value) return;

	//auto debug_string = message.ShortDebugString();
	//DEBUG("send protocol to player_id:{} protocol_name:{} content:{}", _player_id, enum_value->name().c_str(), debug_string);
}
	
void Player::SendMeta(const Asset::Meta& meta)
{
	/*
	if (!Connected()) 
	{
		LOG(ERROR, "玩家:{}未能找到合适逻辑服务器，当前服务器:{}", _player_id, _stuff.server_id());
		return;
	}
	*/
	
	auto session = WorldSessionInstance.GetPlayerSession(_player_id);
	if (!session || !session->IsConnect()) return;
	
	//DEBUG("玩家:{}发送协议:{}到游戏逻辑服务器", _player_id, meta.ShortDebugString());

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

	DEBUG("玩家:{}发送到游戏逻辑服务器:{}，内容:{}", _player_id, _stuff.server_id(), debug_string);

	_gs_session->SendMeta(meta); 

	return true;
}

bool Player::SendProtocol2GameServer(const pb::Message* message)
{
	if (!message) return false;

	//auto _gs_session = WorldSessionInstance.GetServerSession(GetLocalServer());
	//if (!_gs_session || !message) return false;

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

Asset::ERROR_CODE Player::CommonCheck(int32_t type_t)
{
	switch (type_t)
	{
		case Asset::META_TYPE_SHARE_CREATE_ROOM: //创建房间
		{
			if (g_const->guest_forbid_friend_room() && (_account_type == 0 || Asset::ACCOUNT_TYPE_GUEST == _account_type)) 
			{
				LOG(ERROR, "玩家:{} 创建房间失败:{} 账号信息:{}", _player_id, _account_type, _stuff.account());
				return Asset::ERROR_ROOM_FRIEND_NOT_FORBID; //游客禁止进入好友房
			}
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

bool Player::HandleProtocol(int32_t type_t, pb::Message* message)
{
	if (!message) return false;

	DEBUG("当前玩家{}所在服务器:{} 接收协议数据:{}", _player_id, _stuff.server_id(), message->ShortDebugString());
	//
	//如果中心服务器没有协议处理回调，则发往游戏服务器进行处理
	//
	//如果玩家已经在游戏逻辑服务器，则直接发往游戏逻辑服务器，防止数据覆盖
	//
	auto result = CommonCheck(type_t); //通用限制检查
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

void Player::AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type/*= Asset::ERROR_TYPE_NORMAL*/, 
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
	
int32_t Player::CmdGetBattleHistory(pb::Message* message)
{
	auto battle_history = dynamic_cast<const Asset::BattleHistory*>(message);
	if (!battle_history) return 1;

	int32_t start_index = battle_history->start_index();
	int32_t end_index = battle_history->end_index();

	BattleHistory(start_index, end_index);

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
	
void Player::MultiplyRoomCard()
{
	if (_stuff.card_count_changed()) return;
	_stuff.set_card_count_changed(true); //倍率

	auto curr_count = GetRoomCard();
	GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_FANBEI, curr_count * (g_const->room_card_beishu() - 1));
}

void Player::BattleHistory(int32_t start_index, int32_t end_index)
{
	Asset::BattleHistory message;
	message.set_start_index(start_index);
	message.set_end_index(end_index);

	if (start_index > end_index || start_index < 0 || end_index < 0) return;

	int32_t historty_count = std::min(_stuff.room_history().size(), 5); //最多显示5条记录
	if (historty_count <= 0) return;
	
	if (end_index - start_index > historty_count) return;

	if (end_index == 0) end_index = historty_count; //_stuff.room_history().size();
	if (start_index == 0) start_index = 1; //end_index - historty_count;
	
	std::set<int64_t> room_list; //历史记录

	if (_stuff.room_history().size() > 10) //历史战绩最多保留10条
	{
		std::vector<int32_t> room_history;

		for (int32_t i = 0; i < _stuff.room_history().size(); ++i) 
		{
			auto room_id = _stuff.room_history(_stuff.room_history().size() - 1 - i);

			//auto it = std::find(room_history.begin(), room_history.end(), room_id);
			//if (it != room_history.end()) continue;

			room_history.push_back(room_id);
			if (room_history.size() >= 10) break; 
		}

		_stuff.mutable_room_history()->Clear();
		for (auto it = room_history.rbegin(); it != room_history.rend(); ++it) _stuff.mutable_room_history()->Add(*it); 

		SetDirty();
	}

	room_list.clear(); 

	/*
	cpp_redis::future_client client;
	client.connect(ConfigInstance.GetString("Redis_ServerIP", "127.0.0.1"), ConfigInstance.GetInt("Redis_ServerPort", 6379));
	if (!client.is_connected()) return;
	
	auto has_auth = client.auth(ConfigInstance.GetString("Redis_Password", "!QAZ%TGB&UJM9ol."));
	if (has_auth.get().ko()) return;
	*/

	for (int32_t i = start_index - 1; i < end_index; ++i)
	{
		if (i < 0 || i >= _stuff.room_history().size()) continue; //安全检查

		Asset::RoomHistory history;
		auto room_id = _stuff.room_history(i);
		/*
		auto get = client.get("room_history:" + std::to_string(room_id));
		cpp_redis::reply reply = get.get();
		client.commit();

		if (!reply.is_string()) continue;

		auto success = history.ParseFromString(reply.as_string());
		if (!success) continue;
		*/

		//auto redis_cli = make_unique<Redis>();
		if (!RedisInstance.GetRoomHistory(room_id, history)) 
		{
			auto record = message.mutable_history_list()->Add();
			record->set_room_id(room_id); //尚未存盘成功的战绩，只发房间ID
			continue;
		}

		if (room_list.find(room_id) != room_list.end()) continue; //防止历史战绩冗余
		room_list.insert(room_id);

		for (int32_t j = 0; j < history.list().size(); ++j)
		{	
			for (int32_t k = 0; k < history.list(j).list().size(); ++k)
			{
				if (history.player_brief_list().size() < MAX_PLAYER_COUNT)
				{
					auto player_brief = history.mutable_player_brief_list()->Add();
					player_brief->set_player_id(history.list(j).list(k).player_id());
					player_brief->set_nickname(history.list(j).list(k).nickname());
					player_brief->set_headimgurl(history.list(j).list(k).headimgurl());
				}

				history.mutable_list(j)->mutable_list(k)->clear_nickname();
				history.mutable_list(j)->mutable_list(k)->clear_headimgurl();
				history.mutable_list(j)->mutable_list(k)->mutable_details()->Clear();
			}
		}

		auto record = message.mutable_history_list()->Add();
		record->CopyFrom(history);
	}

	//DEBUG("获取玩家:{}历史战绩，索引区间:{}~{} 数据:{}", _player_id, start_index, end_index, message.ShortDebugString());

	if (message.history_list().size() == 0) return;
	if (message.history_list().size()) SendProtocol(message);
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

	for (auto it = _players.begin(); it != _players.end(); )
	{
		if (!it->second)
		{
			it = _players.erase(it);
		}
		else
		{
			++it;
		}
	}

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

}
