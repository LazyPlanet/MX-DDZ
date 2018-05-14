#include <iostream>

//#include <hiredis.h>

#include <spdlog/spdlog.h>
#include <pbjson.hpp>

#include "Player.h"
#include "Game.h"
#include "Timer.h"
#include "Mall.h"
#include "Protocol.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "PlayerCommonReward.h"
#include "PlayerCommonLimit.h"
#include "PlayerMatch.h"
#include "Clan.h"

namespace Adoter
{

namespace spd = spdlog;

Player::Player()
{
	//协议默认处理函数
	_method = std::bind(&Player::DefaultMethod, this, std::placeholders::_1);

	//协议处理回调初始化
	AddHandler(Asset::META_TYPE_SHARE_CREATE_ROOM, std::bind(&Player::CmdCreateRoom, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_GAME_OPERATION, std::bind(&Player::CmdGameOperate, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_PAI_OPERATION, std::bind(&Player::CmdPaiOperate, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_BUY_SOMETHING, std::bind(&Player::CmdBuySomething, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_ENTER_ROOM, std::bind(&Player::CmdEnterRoom, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SIGN, std::bind(&Player::CmdSign, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_RANDOM_SAIZI, std::bind(&Player::CmdSaizi, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_COMMON_PROPERTY, std::bind(&Player::CmdGetCommonProperty, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SAY_HI, std::bind(&Player::CmdSayHi, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_GAME_SETTING, std::bind(&Player::CmdGameSetting, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SYSTEM_CHAT, std::bind(&Player::CmdSystemChat, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_RECHARGE, std::bind(&Player::CmdRecharge, this, std::placeholders::_1));

	//AddHandler(Asset::META_TYPE_C2S_LOGIN, std::bind(&Player::CmdLogin, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_C2S_ENTER_GAME, std::bind(&Player::CmdEnterGame, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_GET_REWARD, std::bind(&Player::CmdGetReward, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_LOAD_SCENE, std::bind(&Player::CmdLoadScene, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_GET_ROOM_DATA, std::bind(&Player::CmdGetRoomData, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_UPDATE_ROOM, std::bind(&Player::CmdUpdateRoom, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_CLAN_OPERATION, std::bind(&Player::CmdClanOperate, this, std::placeholders::_1));
	
	//中心服务器协议处理
	AddHandler(Asset::META_TYPE_S2S_KICKOUT_PLAYER, std::bind(&Player::OnKickOut, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_S2S_PLAYER_STATE, std::bind(&Player::OnPlayerStateChanged, this, std::placeholders::_1));
}
	
Player::Player(int64_t player_id) : Player()/*委派构造函数*/
{
	SetID(player_id);	
}

/*
Player::Player(int64_t player_id, std::shared_ptr<WorldSession> session) : Player()
{
	SetID(player_id);	
	//_session = session; //地址拷贝
}
*/

int32_t Player::Load()
{
	//加载数据库
	auto success = RedisInstance.GetPlayer(_player_id, _stuff);
	if (!success) return 1;
		
	//初始化包裹
	//
	//创建角色或者增加包裹会调用一次
	//
	do {
		const pb::EnumDescriptor* enum_desc = Asset::INVENTORY_TYPE_descriptor();
		if (!enum_desc) return 0;

		int32_t curr_inventories_size = _stuff.inventory().inventory_size(); 
		if (curr_inventories_size == enum_desc->value_count() - 1) break; 

		for (int inventory_index = curr_inventories_size; inventory_index < enum_desc->value_count() - 1; ++inventory_index)
		{
			auto inventory = _stuff.mutable_inventory()->mutable_inventory()->Add(); //增加新包裹，且初始化数据
			inventory->set_inventory_type((Asset::INVENTORY_TYPE)(inventory_index + 1));

			const pb::EnumValueDescriptor *enum_value = enum_desc->value(inventory_index);
			if (!enum_value) break;
		}
	} while(false);

	OnClanCheck();
	
	return 0;
}

int32_t Player::Save(bool force)
{
	LOG_BI("player", _stuff);

	if (!force && !IsDirty()) return 1;

	auto success = RedisInstance.SavePlayer(_player_id, _stuff);
	if (!success) return 2;
	
	_dirty = false;

	if (force) SyncCommonProperty();

	return 0;
}
	
void Player::OnLogin()
{
}

int32_t Player::Logout(pb::Message* message)
{
	const auto kick_out = dynamic_cast<const Asset::KickOutPlayer*>(message);
	if (!kick_out) return 1;
	//
	//如果玩家正在进行玩牌，则不允许立即退出
	//
	//(1) 处理非操作玩家退出的状态;
	//
	//(2) 处理操作玩家退出的状态，即刚好轮到该玩家进行操作的时候，玩家逃跑;
	//
	if (_room) 
	{
		if (_game || (_room->HasStarted() && !_room->HasBeenOver() && !_room->HasDisMiss())) //游戏中，或已经开局且尚未对局完成且不是解散，则不让退出房间
		{
			SetOffline(); //玩家状态

			ERROR("玩家:{} 从房间且牌局内退出游戏:{},服务器进行托管", _player_id, _room->GetID()); //玩家逃跑

			_tuoguan_server = true; //服务器托管

			if (HasTuoGuan() && _game->CanPaiOperate(shared_from_this())) //轮到该玩家操作
			{
				Asset::PaiElement pai;

				/*
				for (auto it = _cards_inhand.begin(); it != _cards_inhand.end(); ++it)
				{
					if (it->second.size())
					{
						pai.set_card_type((Asset::CARD_TYPE)it->first);
						pai.set_card_value(it->second[0]); //随便选取一个
						break;
					}
				}
				*/

				Asset::PaiOperation pai_operation; 
				pai_operation.set_oper_type(Asset::PAI_OPER_TYPE_DAPAI);
				pai_operation.set_position(GetPosition());
				pai_operation.mutable_pai()->CopyFrom(pai);

				CmdPaiOperate(&pai_operation);
			}

			return 2; //不能退出游戏
		}
		else
		{
			//
			//房主在尚未开局状态，不能因为离线而解散或者退出房间
			//
			if (_room->IsHoster(_player_id) && !_room->HasBeenOver() && !_room->HasDisMiss())
			{
				SetOffline(); //玩家状态

				//_room->KickOutPlayer(); //不做踢人处理

				return 3;
			}
			else
			{
				_room->Remove(_player_id); //退出房间，回调会调用OnLogout接口，从而退出整个游戏逻辑服务器

				return 4;
			}
		}
	}

	OnLogout(Asset::KICK_OUT_REASON_LOGOUT); //否则房主不会退出//直接通知中心服务器退出
	
	return 0;
}
	
int32_t Player::OnLogout(Asset::KICK_OUT_REASON reason)
{
	if (!_game && _room && (!_room->HasStarted() || _room->HasBeenOver() || _room->HasDisMiss())) 
	{
		ResetRoom();
	}
	else if (!_room && _stuff.room_id()) //进入房间后加载场景失败
	{
		auto room = RoomInstance.Get(_stuff.room_id());
		if (room) room->Remove(_player_id);
	}
	
	//
	//避免中心服务器先进行退出，然后逻辑服务器退出，返回给中心服务器退出成功后，中心服务器没有找到玩家的情况
	//
	//处理问题：https://www.teambition.com/project/592ad0fd66e46f0dd7503429/tasks/scrum/592ad0fe4e2b442b01e8753d/task/5aba0214bc23685b35b6a896
	//https://www.teambition.com/project/592ad0fd66e46f0dd7503429/tasks/scrum/592ad0fe4e2b442b01e8753d/task/5ab9e9a11adffc7755dc4df4
	//
	//因此，次序必须保证
	if (reason == Asset::KICK_OUT_REASON_LOGOUT)
	{
		_stuff.set_login_time(0); //不在逻辑服务器进行在线或离线状态更新//真正离线
		_stuff.set_logout_time(CommonTimerInstance.GetTime());

		PlayerInstance.Remove(_player_id); //删除玩家
	}
	else if (reason == Asset::KICK_OUT_REASON_CHANGE_SERVER) //切换服务器直接退出当前服务器，玩家状态还是在线
	{
		PlayerInstance.Remove(_player_id); //删除玩家
	}
	
	Save(true);	//存档数据库

	Asset::KickOutPlayer kickout_player; //通知中心服务器退出
	kickout_player.set_player_id(_player_id);
	kickout_player.set_reason(reason);
	SendProtocol(kickout_player);
		
	WARN("玩家:{} 服务器:{} 数据:{} 当前所在服务器:{} 成功退出逻辑服务器:{} 原因:{}", 
			_player_id, g_server_id, _stuff.ShortDebugString(), _stuff.server_id(), g_server_id, kickout_player.ShortDebugString());
	
	return 0;
}

bool Player::HasTuoGuan()
{
	if (!_game || !_room) return false;

	if (_room->IsFriend()) return false; //好友房不托管

	return _tuoguan_server; 
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
	
	LOG(INFO, "玩家:{} 消耗房卡，原因:{} 数量:{} 成功，此时房卡数量:{}", 
			_player_id, Asset::ROOM_CARD_CHANGED_TYPE_Name(changed_type), count, _stuff.common_prop().room_card_count());
	return count;
}

int64_t Player::GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count) 
{
	if (count <= 0) return 0;
	
	_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() + count);
	_dirty = true;
	
	SyncCommonProperty();

	LOG(INFO, "玩家:{} 获得房卡，原因:{} 数量:{} 成功，此时房卡数量:{}", 
			_player_id, Asset::ROOM_CARD_CHANGED_TYPE_Name(changed_type), count, _stuff.common_prop().room_card_count());
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
	
	LOG(INFO, "玩家:{} 消耗欢乐豆，原因:{} 数量:{} 成功", _player_id, Asset::HUANLEDOU_CHANGED_TYPE_Name(changed_type), count);
	return count;
}

int64_t Player::GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{} 获得欢乐豆，原因:{} 数量:{} 成功", _player_id, Asset::HUANLEDOU_CHANGED_TYPE_Name(changed_type), count);
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
	
	LOG(INFO, "玩家:{} 消耗钻石:{} 原因:{} 成功", _player_id, count, Asset::DIAMOND_CHANGED_TYPE_Name(changed_type));
	return count;
}

int64_t Player::GainDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() + count);
	_dirty = true;

	SyncCommonProperty();
	
	LOG(INFO, "玩家:{} 获得钻石:{} 原因:{}", _player_id, count, Asset::DIAMOND_CHANGED_TYPE_Name(changed_type));
	return count;
}

bool Player::CheckDiamond(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().diamond();
	return curr_count >= count;
}
	
bool Player::HasClan(int64_t clan_id)
{
	if (clan_id <= 0) return true;

	auto it = std::find(_stuff.clan_hosters().begin(), _stuff.clan_hosters().end(), clan_id);
	if (it != _stuff.clan_hosters().end()) return true;

	it = std::find(_stuff.clan_joiners().begin(), _stuff.clan_joiners().end(), clan_id);
	if (it != _stuff.clan_joiners().end()) return true;

	return false;
}
	
int32_t Player::OnEnterGame() 
{
	DEBUG("玩家:{} 进入逻辑服务器:{}", _player_id, g_server_id);

	if (Load()) 
	{
		LOG(ERROR, "玩家:{}加载数据失败", _player_id);
		return 1;
	}
	
	//
	//设置玩家所在服务器，每次进入场景均调用此
	//
	//对于MMORPG游戏，可以是任意一个场景或副本ID，此处记录为解决全球唯一服，通过Redis进行进程间通信，获取玩家所在服务器ID.
	//
	SetLocalServer(ConfigInstance.GetInt("ServerID", 1));

	SendPlayer(); //发送数据给玩家
	
	_stuff.set_login_time(CommonTimerInstance.GetTime());
	_stuff.set_logout_time(0);

	SetDirty(); //存盘

	LOG_BI("player", _stuff);

	//WorldSessionInstance.Emplace(_player_id, _session); //网络会话数据
	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理

	OnLogin(); //进入逻辑服务器后，登陆成功

	return 0;
}

int32_t Player::CmdLeaveRoom(pb::Message* message)
{
	if (!message) return 1;

	OnLeaveRoom(); //房间处理

	return 0;
}

void Player::SendPlayer()
{
	Asset::PlayerInformation player_info;
	player_info.mutable_player()->CopyFrom(this->_stuff);

	SendProtocol(player_info);
}

int32_t Player::CmdCreateRoom(pb::Message* message)
{
	int32_t result = CreateRoom(message);
	if (result) OnLogout(Asset::KICK_OUT_REASON_LEAVE_ROOM); //创建失败

	return result;
}

int32_t Player::CreateRoom(pb::Message* message)
{
	Asset::CreateRoom* create_room = dynamic_cast<Asset::CreateRoom*>(message);
	if (!create_room) return 1;
	
	if (_room && create_room->room().clan_id() == 0) 
	{
		auto room = RoomInstance.Get(_room->GetID());

		if (room && room->GetRemainCount() > 0) //房间尚未解散
		{
			SendRoomState();

			return 2;
		}
	}

	auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
	if (!clan_limit) return 12;

	if (clan_limit->hoster_create_room() && !IsHoster(create_room->room().clan_id())) return 11; //非茶馆老板，不能创建茶馆房
	
	//
	//检查是否活动限免房卡
	//
	//否则，检查房卡是否满足要求
	//
	if (ActivityInstance.IsOpen(g_const->room_card_limit_free_activity_id()))
	{
		WARN("限免活动开启中，玩家ID:{} 正常开房...", _player_id);
	}
	else if (create_room->room().clan_id() == 0) //非茶馆房间
	{
		auto open_rands = create_room->room().options().open_rands(); //局数
		auto pay_type = create_room->room().options().pay_type(); //付费方式

		const Asset::Item_RoomCard* room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
		if (!room_card || room_card->rounds() <= 0) return 3;

		int32_t consume_count = open_rands / room_card->rounds(); //待消耗房卡数量

		switch (pay_type)
		{
			case Asset::ROOM_PAY_TYPE_HOSTER:
			{
				if (!CheckRoomCard(consume_count)) 
				{
					AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足

					LOG(ERROR, "玩家:{}开房房卡不足，当前房卡数量:{}需要消耗房卡数量:{}，开局数量:{}，单个房卡可以开房数量:{}", 
							_player_id, _stuff.common_prop().room_card_count(), consume_count, open_rands, room_card->rounds());
					return 5;
				}
			}
			break;
			
			case Asset::ROOM_PAY_TYPE_AA:
			{
				consume_count = consume_count / MAX_PLAYER_COUNT; //单人付卡数量

				if (!CheckRoomCard(consume_count)) 
				{
					AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
					return 6;
				}
			}
			break;

			default:
			{
				ERROR("玩家:{} 创建房间错误:{}", _player_id, message->ShortDebugString());
				return 7;
			}
			break;
		}
	}
	
	if (!HasClan(create_room->room().clan_id())) return 10; //茶馆检查

	int64_t room_id = RoomInstance.AllocRoom();
	if (!room_id) return 9;

	create_room->mutable_room()->set_room_id(room_id);
	create_room->mutable_room()->set_room_type(Asset::ROOM_TYPE_FRIEND); //创建房间，其实是好友房
	
	SendProtocol(create_room); 
	
	OnCreateRoom(create_room); //创建房间成功

	LOG(INFO, "玩家:{} 创建房间:{} 成功", _player_id, room_id);

	return 0;
}
	
void Player::OnRoomRemoved()
{
	ResetRoom(); //房间非法
}

void Player::OnCreateRoom(Asset::CreateRoom* create_room)
{
	if (!create_room) return; //理论不会如此

	_stuff.mutable_last_room_options()->CopyFrom(create_room->room().options()); //存储开房玩法
	_dirty = true;

	Asset::Room asset_room;
	asset_room.CopyFrom(create_room->room());

	auto room = std::make_shared<Room>(asset_room);
	room->OnCreated(shared_from_this());

	RoomInstance.OnCreateRoom(room); //房间管理
}

int32_t Player::CmdGameOperate(pb::Message* message)
{
	auto game_operate = dynamic_cast<Asset::GameOperation*>(message);
	if (!game_operate) return 1;
	
	game_operate->set_source_player_id(_player_id); //设置当前操作玩家

	switch(game_operate->oper_type())
	{
		case Asset::GAME_OPER_TYPE_NULL: 
		case Asset::GAME_OPER_TYPE_START: //开始游戏：其实是个准备//扑克押注
		{
			_player_prop.set_game_oper_state(game_operate->oper_type());
		}
		break;

		case Asset::GAME_OPER_TYPE_LEAVE: //离开游戏：相当于退出房间
		{
			if (_game) return 0;

			if (!_room) 
			{
				OnLeaveRoom();
				return 0; //如果玩家不在房间，也不存在后面的逻辑
			}
		}
		break;

		case Asset::GAME_OPER_TYPE_DISMISS_AGREE: //解散
		case Asset::GAME_OPER_TYPE_DISMISS_DISAGREE: //不解散
		{
			_player_prop.set_game_oper_state(game_operate->oper_type());

			if (!_room || _room->HasBeenOver()) 
			{
				OnLeaveRoom(); //防止玩家不在房间内进行解散操作,出现这种情况原因是C<->S状态不一致
				return 0;
			}

			if (_room && _room->IsMatch()) return 5; //匹配房不允许解散
		}
		break;

		case Asset::GAME_OPER_TYPE_KICKOUT: //踢人
		{
			if (!_room->IsHoster(_player_id)) //不是房主，不能踢人
			{
				AlertMessage(Asset::ERROR_ROOM_NO_PERMISSION); //没有权限
				return 3;
			}
		}
		break;

		default:
		{
			 //_player_prop.clear_game_oper_state(); //错误状态//交给房间逻辑处理
		}
		break;
	}

	if (!_room) return 4;

	_room->OnPlayerOperate(shared_from_this(), message); //广播给其他玩家

	return 0;
}
	
void Player::SetStreakWins(int32_t count) 
{ 
	int32_t curr_count = _stuff.common_prop().streak_wins();
	if (count <= curr_count) return;

	_stuff.mutable_common_prop()->set_streak_wins(count); 
	_dirty = true; 
}
	
void Player::OnGameStart()
{
	AddTotalRounds(); //总对战局数

	if (_room && _room->IsFriend()) AddFriendRoomRounds(); //好友房对战局数

	ClearCards();  //游戏数据
}

int32_t Player::CmdClanOperate(pb::Message* message)
{
	auto clan_oper = dynamic_cast<Asset::ClanOperation*>(message);
	if (!clan_oper) return 1;
	
	ClanInstance.OnOperate(shared_from_this(), clan_oper);
	return 0;
}

//
//对玩家打牌分析
//
bool Player::PaiXingCheck(Asset::PaiOperation* pai_operate)
{
	if (!_room || !_game) return false;

	if (!pai_operate) return false;

	std::sort(pai_operate->mutable_pais()->begin(), pai_operate->mutable_pais()->end(), [](const Asset::PaiElement& x, const Asset::PaiElement& y){ 
			return x.card_value() < y.card_value(); //由小到大
		}); 


	auto chupai_count = pai_operate->pais().size(); //出牌数量
	if (chupai_count <= 0) return false;

	//王炸
	//
	if (chupai_count == 2 && pai_operate->pais(0).card_type() == Asset::CARD_TYPE_KINGS && pai_operate->pais(1).card_type() == Asset::CARD_TYPE_KINGS)
	{
		_game->IncreaseBeiLv(); //翻倍

		pai_operate->mutable_pai()->set_card_type(Asset::CARD_TYPE_KINGS); //最大牌值
		pai_operate->mutable_pai()->set_card_value(2); 

		pai_operate->set_paixing(Asset::PAIXING_TYPE_ZHADAN);
		return true;
	}
	
	//单张王
	if (chupai_count == 1 && pai_operate->pais(0).card_type() == Asset::CARD_TYPE_KINGS)
	{
		pai_operate->mutable_pai()->CopyFrom(pai_operate->pais(0)); //最大牌值

		pai_operate->set_paixing(Asset::PAIXING_TYPE_DANZHANG);
		return true;
	}

	//正常出牌，不带王
	//
	std::map<int32_t, int32_t> cards_value; //各个牌值数量

	for (const auto& card : pai_operate->pais())
	{
		if (card.card_type() == Asset::CARD_TYPE_KINGS) return false; //王牌不能进行任何组合

		int32_t card_value = card.card_value();
		if (card_value == 1 || card_value == 2) card_value += 13;

		++cards_value[card_value];
	}

	auto max_value = 0, max_value_count = 0; //最大牌值和最大牌值数量
	for (const auto& card : cards_value)
	{
		if (card.second >= max_value_count)
		{
			max_value = card.first;
			max_value_count = card.second;	
		}
	}

	const auto max_pai = pai_operate->pai(); //Clinet最大数量牌值
	const auto pai_xing = pai_operate->paixing(); //Client最大牌型
	
	for (const auto& card : pai_operate->pais())
	{
		int32_t card_value = card.card_value();
		if (card_value == 1 || card_value == 2) card_value += 13;

		if (max_value == card_value)
		{
			pai_operate->mutable_pai()->CopyFrom(card); //缓存最大牌值
			break;
		}
	}

	//根据牌面相同的最大数量判断类型
	//
	switch (max_value_count)
	{
		case 4:
		{
			if (chupai_count == 4) //炸弹
			{
				_game->IncreaseBeiLv(); //翻倍

				pai_operate->set_paixing(Asset::PAIXING_TYPE_ZHADAN);
				return true; 
			}
			else if (chupai_count == 6) //四带两张
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_42);
				return true; 
			}
			else if (chupai_count == 8) //四带两对或者四个拆开做飞机
			{
				int32_t begin = 0, n = 0;

				for (const auto& mem : cards_value) //判断连续的3张牌面的最大数量
				{
					if (mem.second >= 3)
					{
						if (!begin || begin == mem.first) ++n;

						if (!begin) begin = mem.first;
						
						if (begin != mem.first && n == 1)
						{
							n = 1;
							begin = mem.first;
						}
						
						++begin;
					}
				}
		  
				if (chupai_count == 4 * n) //飞机带单张的翅膀
				{
					pai_operate->set_paixing(Asset::PAIXING_TYPE_FEIJI_1);
					return true; 
				}
				else
				{
					for (const auto& mem : cards_value)
					{
						if (mem.second != 2 && mem.second != 4) return false; //不符合规则
					}

					pai_operate->set_paixing(Asset::PAIXING_TYPE_42);
					return true; 
				}
			}
		}
		break;

		case 3:
		{
			if (chupai_count == 3) //三条
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_3);
				return true; 
			}
			else if (chupai_count == 4) //三带一张
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_31);
				return true; 
			}
			else if (chupai_count == 5) //三带一对
			{
				for (const auto& mem : cards_value)
				{
					if (mem.second != 3 && mem.second != 2) return false; //不符合规则
				}
				
				pai_operate->set_paixing(Asset::PAIXING_TYPE_32);
				return true;
			}

			int32_t begin = 0, n = 0;

			for (const auto& mem : cards_value) //判断连续的3张牌面的最大数量
			{
				if (mem.second == 3)
				{
					if (!begin || begin == mem.first) ++n;

					if (!begin) begin = mem.first;
					
					if (begin != mem.first && n == 1)
					{
						n = 1;
						begin = mem.first;
					}
					
					++begin;
				}
			}
		  
			if (chupai_count == 3 * n) //三顺
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_SHUN_3);
				return true; 
			}
			else if (chupai_count == 4 * n) //飞机带单张的翅膀
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_FEIJI_1);
				return true; 
			}
			else if (chupai_count == 5 * n) //飞机带对子翅膀
			{
				for (auto mem : cards_value)
				{
					if (mem.second != 2 && mem.second != 3) return false; //牌不合规
				}

				pai_operate->set_paixing(Asset::PAIXING_TYPE_FEIJI_2);
				return true; 
			}
		}
		break;

		case 2:
		{
			if (chupai_count == 2) //一对
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_DUIZI);
				return true;
			}
			else if (chupai_count >= 6 && !(chupai_count % 2)) //连对
			{
				int begin = 0;
				for (auto mem : cards_value) //确定牌是连续的，并且都是成对的
				{
					if (!begin)	begin = mem.first;
					if (begin++ != mem.first || mem.second != 2 || mem.first >= 15) return false; //牌不符合规定//2不允许排对子
				}

				pai_operate->set_paixing(Asset::PAIXING_TYPE_LIANDUI);
				return true;
			}
		}
		break;

		case 1:
		{
			if (chupai_count == 1) //单张
			{
				pai_operate->set_paixing(Asset::PAIXING_TYPE_DANZHANG);
				return true;
			}
			else if (chupai_count >= 5) //判断是否为顺子
			{
				int32_t begin = 0;
				for (auto mem : cards_value)
				{
					if (!begin) begin = mem.first;
					if (begin++ != mem.first || mem.first >= 15) return false; //牌不是连续的或者带了2及以上的牌
				}

				pai_operate->set_paixing(Asset::PAIXING_TYPE_DANSHUN);
				return true;
			}
		}
		break;

		default: //不符合规定，不让出牌
		{
			return false;
		}
		break;
	}

	WARN("玩家:{} 出牌:{} 不符合条件，最大牌:{} 牌型:{}", _player_id, pai_operate->ShortDebugString(), max_value, Asset::PAIXING_TYPE_Name(pai_xing));
		
	return false; //牌型不满足
}

int32_t Player::CmdPaiOperate(pb::Message* message)
{
	std::lock_guard<std::mutex> lock(_card_lock);
	
	if (!_room || !_game) return 1; //还没加入房间或者还没开始游戏

	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return 2; 

	if (!pai_operate->position()) pai_operate->set_position(GetPosition()); //设置玩家座位
			
	//进行操作
	//
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		{
			if (!PaiXingCheck(pai_operate)) //牌型检查
			{
				AlertMessage(Asset::ERROR_PAI_UNSATISFIED); //不满足牌型
				return 3;
			}

			++_chupai_count; //出牌次数
		}
		break;

		case Asset::PAI_OPER_TYPE_GIVEUP: //不要//过
		{

		}
		break;
		
		default:
		{
			WARN("玩家:{} 在房间:{}/{}局操作错误，牌数据:{}", _player_id, _game->GetID(), _room->GetID(), pai_operate->ShortDebugString());

			return 4; //非法操作，直接退出
		}
		break;
	}

	//牌内操作
	//
	_game->OnPaiOperate(shared_from_this(), message);

	return 0;
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

int32_t Player::CmdEnterRoom(pb::Message* message) 
{
	int32_t result = EnterRoom(message);
	if (result) OnLogout(Asset::KICK_OUT_REASON_LEAVE_ROOM); //进入失败

	return result;
}

int32_t Player::EnterRoom(pb::Message* message) 
{
	Asset::EnterRoom* enter_room = dynamic_cast<Asset::EnterRoom*>(message);
	if (!enter_room) return Asset::ERROR_INNER; 

	//
	//房间重入检查
	//
	do 
	{
		if (_room) 
		{
			auto room_id = _room->GetID();

			auto client_room_id = enter_room->room().room_id();
			if (room_id != client_room_id)
			{
				LOG(ERROR, "玩家:{}重入房间错误，客户端记录:{}和服务器记录:{}不是一个，以当前客户端为准", _player_id, client_room_id, room_id);
				room_id = client_room_id;
			}
				
			auto locate_room = RoomInstance.Get(room_id);
			if (!locate_room)
			{
				enter_room->set_error_code(Asset::ERROR_ROOM_NOT_FOUNT); //是否可以进入场景//房间
				SendProtocol(message);
				
				AlertMessage(Asset::ERROR_ROOM_NOT_FOUNT, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //不能加入，错误提示

				ResetRoom(); //房间非法
				SetOffline(false); //恢复在线状态

				return Asset::ERROR_ROOM_NOT_FOUNT;
			}

			if (_room->GetID() == enter_room->room().room_id()) //重入房间
			{
				enter_room->mutable_room()->CopyFrom(_room->Get());
				enter_room->set_error_code(Asset::ERROR_SUCCESS); //是否可以进入场景//房间
				SendProtocol(message);

				return Asset::ERROR_SUCCESS;
			}
			else
			{
				locate_room = RoomInstance.Get(_room->GetID());
				if (!locate_room || locate_room->HasDisMiss() || locate_room->HasBeenOver()) //已经结束或者解散
				{
					ResetRoom(); //房间非法
					break; //房间已经不存在
				}

				SendRoomState();

				//return Asset::ERROR_ROOM_HAS_BEEN_IN;
				return Asset::ERROR_SUCCESS;
			}
		}
	} while (false);

	//
	//房间正常进入
	//

	ClearCards();

	Asset::ROOM_TYPE room_type = enter_room->room().room_type();

	auto check = [this, room_type]()->Asset::ERROR_CODE {

		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return Asset::ERROR_ROOM_TYPE_NOT_FOUND;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return Asset::ERROR_ROOM_TYPE_NOT_FOUND;

		int64_t beans_count = GetHuanledou();

		int32_t min_limit = room_limit->min_limit();
		if (min_limit >= 0 && beans_count < min_limit) return Asset::ERROR_ROOM_BEANS_MIN_LIMIT;

		int32_t max_limit = room_limit->max_limit();
		if (max_limit >= 0 && beans_count > max_limit) return Asset::ERROR_ROOM_BEANS_MAX_LIMIT;

		return Asset::ERROR_SUCCESS;
	};

	switch (room_type)
	{
		case Asset::ROOM_TYPE_FRIEND: //好友房
		{
			auto room_id = enter_room->room().room_id(); 

			auto locate_room = RoomInstance.Get(room_id);

			if (!locate_room) 
			{
				enter_room->set_error_code(Asset::ERROR_ROOM_NOT_FOUNT); //是否可以进入场景//房间
			}
			else
			{
				if (!HasClan(locate_room->GetClan())) 
				{
					enter_room->set_error_code(Asset::ERROR_CLAN_ROOM_ENTER_NO_CLAN); 
					SendProtocol(enter_room);
					return Asset::ERROR_CLAN_ROOM_ENTER_NO_CLAN; //非茶馆成员，不能加入
				}

				if (locate_room->GetOptions().pay_type() == Asset::ROOM_PAY_TYPE_AA && 
						!ActivityInstance.IsOpen(g_const->room_card_limit_free_activity_id())) //AA付卡
				{
					const Asset::Item_RoomCard* room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
					if (!room_card || room_card->rounds() <= 0) return Asset::ERROR_INNER;

					int32_t consume_count = locate_room->GetOpenRands() / room_card->rounds() / MAX_PLAYER_COUNT;
					if (consume_count <= 0 || !CheckRoomCard(consume_count))
					{
						//AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足

						enter_room->set_error_code(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
						SendProtocol(enter_room);
						return Asset::ERROR_ROOM_CARD_NOT_ENOUGH;
					}
				}

				auto enter_status = locate_room->TryEnter(shared_from_this()); //玩家进入房间
				enter_room->mutable_room()->CopyFrom(locate_room->Get());
				enter_room->set_error_code(enter_status); //是否可以进入场景//房间

				if (enter_status == Asset::ERROR_SUCCESS || enter_status == Asset::ERROR_ROOM_HAS_BEEN_IN) 
				{
					enter_room->set_error_code(Asset::ERROR_SUCCESS);
					bool success = locate_room->Enter(shared_from_this()); //玩家进入房间

					if (success) OnEnterSuccess(room_id);
				}
			}
			
			if (enter_room->error_code() != Asset::ERROR_SUCCESS) 
				AlertMessage(enter_room->error_code(), Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //不能加入，错误提示

			SendProtocol(enter_room);
		}
		break;

		case Asset::ROOM_TYPE_XINSHOU:
		case Asset::ROOM_TYPE_GAOSHOU:
		case Asset::ROOM_TYPE_DASHI:
		{
			if (_room || _game) return 1; //已经在房间内

			if (HasMatching(room_type)) 
			{
				ERROR("玩家:{}已经匹配中,历史匹配房间类型:{},本次匹配房间类型:{} 无法再次匹配.", _player_id, _stuff.matching_room_type(), room_type);
				return 2; //匹配中,防止多次点击匹配
			}

			auto result = check();

			if (result != Asset::ERROR_SUCCESS) //不允许进入
			{
				AlertMessage(result);
				return result;
			}

			SetMatchingRoom(room_type);

			WARN("玩家:{}进入匹配, 匹配数据:{}", _player_id, message->ShortDebugString());

			//进入匹配
			MatchInstance.Join(shared_from_this(), message);
		}
		break;

		default:
		{
			return Asset::ERROR_INNER; //非法
		}
		break;
	}

	return 0;
}
	
int32_t Player::GetLocalRoomID() 
{ 
	if (!_room) return 0; 

	return _room->GetID();
}

void Player::OnEnterSuccess(int64_t room_id)
{
	_stuff.set_room_id(room_id); //避免玩家进入房间后尚未加载场景，掉线
	_stuff.clear_matching_room_type(); //匹配
	
	SetDirty();
}

bool Player::HandleMessage(const Asset::MsgItem& item)
{
	pb::Message* msg = ProtocolInstance.GetMessage(item.type_t());
	if (!msg) return false;

	auto message = msg->New();
	
	defer {
		delete message;
		message = nullptr;
	};

	auto result = message->ParseFromString(item.content());

	if (!result) return false;      //非法协议
	
	DEBUG("玩家:{} 处理消息:{}", _player_id, message->ShortDebugString());

	switch (item.type_t())
	{
		case Asset::META_TYPE_SHARE_PAI_OPERATION:
		{
			CmdPaiOperate(message);
		}
		break;

		default:
		{
			WARN("玩家:{} 尚未消息:{}处理回调", _player_id, item.ShortDebugString());
		}
		break;
	}
		
	delete message; //防止内存泄漏
	message = nullptr;

	return true;
}
	
void Player::SendMessage(const Asset::MsgItem& item)
{
	DispatcherInstance.SendMessage(item);
}	
	
void Player::SendMessage(int64_t receiver, const pb::Message* message)
{
	if (!message) return;
	SendMessage(receiver, *message);
}
	
void Player::SendMessage(int64_t receiver, const pb::Message& message)
{
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::MsgItem msg;
	msg.set_receiver(receiver);
	msg.set_type_t((Asset::META_TYPE)type_t);
	msg.set_content(message.SerializeAsString());

	SendMessage(msg);
}

void Player::SendProtocol(const pb::Message* message)
{
	if (!message) return;
	SendProtocol(*message);
}

void Player::SendProtocol(const pb::Message& message)
{
	if (!Connected()) return; //尚未建立网络连接

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());
	meta.set_player_id(_player_id);

	std::string content = meta.SerializeAsString();
	if (content.empty()) return;

	_session->AsyncSendMessage(content);

	int64_t room_id = 0, game_id = 0;

	if (_room) room_id = _room->GetID();
	if (_game) game_id = _game->GetID();

	DEBUG("玩家:{} 发送协议，类型:{} 房间:{} 局数:{} 内容:{}", 
			_player_id, Asset::META_TYPE_Name(meta.type_t()), room_id, game_id, message.ShortDebugString());
}

void Player::Send2Roomers(pb::Message& message, int64_t exclude_player_id) 
{
	if (!_room) return;
	_room->BroadCast(message, exclude_player_id);
}

void Player::Send2Roomers(pb::Message* message, int64_t exclude_player_id)
{
	if (!_room) return;
	_room->BroadCast(message, exclude_player_id);
}

//
//玩家心跳周期为1s
//
//如果该函数返回FALSE则表示掉线
//
bool Player::Update()
{
	++_heart_count; //心跳
	
	if (_heart_count % 3 == 0) //3s
	{
		CommonLimitUpdate(); //通用限制,定时更新
	
		if (_dirty) Save(); //触发存盘
	}
	
	if (_heart_count % 5 == 0) //5s
	{
		OnlineCheck(); //逻辑服务器不进行心跳检查，只进行断线逻辑检查
	}

	//if (_heart_count % 60 == 0) //1min
	return true;
}
	
int32_t Player::DefaultMethod(pb::Message* message)
{
	if (!message) return 0;

	const pb::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName("type_t");
	if (!field) 
	{
		ERROR("玩家:{}尚未存在协议:{}的协议类型", _player_id, message->ShortDebugString());
		return 1;
	}
		
	WARN("玩家:{}尚未存在协议:{}的处理回调", _player_id, message->ShortDebugString());

	return 0;
}

bool Player::HandleProtocol(int32_t type_t, pb::Message* message)
{
	if (!message) return false;

	SetOffline(false); //玩家在线

	_pings_count = 0;
	_hi_time = CommonTimerInstance.GetTime();

	CallBack& callback = GetMethod(type_t); 
	callback(std::forward<pb::Message*>(message));	
	return true;
}

bool Player::GainItem(int64_t global_item_id, int32_t count)
{
	/*
	pb::Message* asset_item = AssetInstance.Get(global_item_id); //此处取出来的必然为合法ITEM.
	if (!asset_item) return false;

	Item* item = new Item(asset_item);
	GainItem(item, count);
	*/
	const auto message = AssetInstance.Get(global_item_id); //例如：Asset::Item_Potion
	if (!message) return false;

	auto message_item = message->New(); 
	if (!message_item) return false; //内存不足

	message_item->CopyFrom(*message);
	
	const pb::FieldDescriptor* prop_field = message_item->GetDescriptor()->FindFieldByName("item_common_prop"); //物品公共属性变量
	if (!prop_field) return false; //不是物品

	try {
		const pb::Message& const_item_common_prop = message_item->GetReflection()->GetMessage(*message_item, prop_field);

		pb::Message& item_common_prop = const_cast<pb::Message&>(const_item_common_prop);
		auto& common_prop = dynamic_cast<Asset::Item_CommonProp&>(item_common_prop);

		auto inventory_type = common_prop.inventory(); //物品默认进包

		auto it_inventory = std::find_if(_stuff.mutable_inventory()->mutable_inventory()->begin(), _stuff.mutable_inventory()->mutable_inventory()->end(), 
				[inventory_type](const Asset::Inventory_Element& element){
			return inventory_type == element.inventory_type();		
		});

		if (it_inventory == _stuff.mutable_inventory()->mutable_inventory()->end()) return false; //数据错误

		auto inventory_items = it_inventory->mutable_items(); //包裹中物品数据

		if (!inventory_items) return false; //数据错误

		const pb::FieldDescriptor* type_field = message_item->GetDescriptor()->FindFieldByName("type_t");
		if (!type_field) return false; //数据错误

		auto type_t = type_field->default_value_enum()->number();

		auto it_item = inventory_items->begin(); //查找包裹中该物品数据
		for ( ; it_item != inventory_items->end(); ++it_item)
		{
			if (type_t == it_item->type_t())
			{
				auto item = message_item->New();
				item->ParseFromString(it_item->stuff()); //解析存盘数据

				const FieldDescriptor* item_prop_field = item->GetDescriptor()->FindFieldByName("item_common_prop");
				if (!item_prop_field) continue;

				const Message& item_prop_message = item->GetReflection()->GetMessage(*item, item_prop_field);
				prop_field = item_prop_message.GetDescriptor()->FindFieldByName("common_prop");
				if (!prop_field) continue;

				const Message& prop_message = item_prop_message.GetReflection()->GetMessage(item_prop_message, prop_field);
				const FieldDescriptor* global_id_field = prop_message.GetDescriptor()->FindFieldByName("global_id");
				if (!global_id_field) continue;

				auto global_id = prop_message.GetReflection()->GetInt64(prop_message, global_id_field);
				if (global_id == global_item_id) break; //TODO:不限制堆叠
			}
		}
		
		if (it_item == inventory_items->end()) //没有该类型物品
		{
			auto message_string = message_item->SerializeAsString();

			auto item_toadd = inventory_items->Add();
			item_toadd->set_type_t((Adoter::Asset::ASSET_TYPE)type_t);
			common_prop.set_count(count); //Asset::Item_CommonProp
			item_toadd->set_stuff(message_string);
		}
		else
		{
			auto message_string = message_item->SerializeAsString();

			common_prop.set_count(common_prop.count() + count); //Asset::Item_CommonProp
			it_item->set_stuff(message_string);
		}
	}
	catch (std::exception& e)
	{
		LOG(ERR, "const_cast or dynamic_cast exception:{}", e.what());	
		return false;
	}
	return true;
}

bool Player::GainItem(Item* item, int32_t count)
{
	if (!item || count <= 0) return false;

	Asset::Item_CommonProp& common_prop = item->GetCommonProp(); 
	common_prop.set_count(common_prop.count() + count); //数量

	if (!PushBackItem(common_prop.inventory(), item)) return false;
	return true;
}
	
bool Player::PushBackItem(Asset::INVENTORY_TYPE inventory_type, Item* item)
{
	if (!item) return false;

	const pb::EnumDescriptor* enum_desc = Asset::INVENTORY_TYPE_descriptor();
	if (!enum_desc) return false;

	Asset::Inventory_Element* inventory = _stuff.mutable_inventory()->mutable_inventory(inventory_type); 
	if (!inventory) return false;

	auto item_toadd = inventory->mutable_items()->Add();
	item_toadd->CopyFrom(item->GetCommonProp()); //Asset::Item_CommonProp数据

	return true;
}

void Player::BroadCastCommonProp(Asset::MSG_TYPE type)       
{
	Asset::MsgItem item; //消息数据
	item.set_type_t(type);
	item.set_sender(_player_id);
	this->BroadCast(item); //通知给房间玩家
}

void Player::OnLeaveRoom(Asset::GAME_OPER_TYPE reason)
{
	//
	//房间数据初始化
	//
	ResetRoom();
	
	//
	//游戏数据
	//
	ClearCards();  

	//
	//逻辑服务器的退出房间，则退出
	//
	OnLogout(Asset::KICK_OUT_REASON_LEAVE_ROOM); //架构调整：退出房间不进行逻辑服务器退出//退出房间

	//
	//房间状态同步
	//
	Asset::RoomState room_state;
	room_state.set_room_id(0);
	room_state.set_oper_type(reason);
	SendProtocol(room_state);
}
	
void Player::BroadCast(Asset::MsgItem& item) 
{
	if (!_room) return;
	
}	
	
void Player::ResetRoom() 
{ 
	if (_room) _room.reset(); //刷新房间信息

	_stuff.clear_room_id(); //状态初始化
	_player_prop.clear_voice_member_id(); //房间语音数据
	_dirty = true;
}

void Player::AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type/*= Asset::ERROR_TYPE_NORMAL*/, 
		Asset::ERROR_SHOW_TYPE error_show_type/* = Asset::ERROR_SHOW_TYPE_NORMAL*/)
{
	Asset::AlertMessage message;
	message.set_error_type(error_type);
	message.set_error_show_type(error_show_type);
	message.set_error_code(error_code);

	ERROR("玩家:{} 消息错误:{}", _player_id, message.ShortDebugString());

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

void Player::SyncCommonLimit()
{
	Asset::SyncCommonLimit proto;
	proto.mutable_common_limit()->CopyFrom(_stuff.common_limit());

	SendProtocol(proto);
}

Asset::ERROR_CODE Player::DeliverReward(int64_t global_id)
{
	auto delivered = CommonRewardInstance.DeliverReward(shared_from_this(), global_id);
	if (delivered == Asset::ERROR_SUCCESS) SyncCommonReward(global_id);
	
	return delivered;
}

void Player::SyncCommonReward(int64_t common_reward_id)
{
	Asset::SyncCommonReward proto;
	proto.set_common_reward_id(common_reward_id);

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

	return 0;
}
	
int32_t Player::CmdGetRoomData(pb::Message* message)
{
	auto get_data = dynamic_cast<Asset::GetRoomData*>(message);
	if (!get_data) return 1;

	if (get_data->reason() == Asset::ROOM_SYNC_TYPE_QUERY)
	{
		auto room = RoomInstance.Get(get_data->room_id());
		if (!room)
		{
			AlertMessage(Asset::ERROR_ROOM_QUERY_NOT_FORBID);
			
			//OnLogout(Asset::KICK_OUT_REASON_LOGOUT); //查询之后退出，否则会残留在此服务器//架构调整：直接驻留在本服务器内
			return 2;
		}

		Asset::RoomInformation room_information;
		room_information.set_sync_type(Asset::ROOM_SYNC_TYPE_QUERY); //外服查询房间信息
				
		const auto players = room->GetPlayers();
		for (const auto player : players)
		{
			if (!player) continue;

			auto p = room_information.mutable_player_list()->Add();
			p->set_position(player->GetPosition());
			p->set_player_id(player->GetID());
			p->set_oper_type(player->GetOperState());
			p->mutable_common_prop()->CopyFrom(player->CommonProp());
			p->mutable_wechat()->CopyFrom(player->GetWechat());
			p->set_ip_address(player->GetIpAddress());
			p->set_voice_member_id(player->GetVoiceMemberID());
		}

		Asset::RoomQueryResult proto;
		proto.set_room_id(room->GetID());
		proto.set_clan_id(room->GetClan());
		proto.set_create_time(room->GetCreateTime());
		proto.mutable_options()->CopyFrom(room->GetOptions());
		proto.mutable_information()->CopyFrom(room_information);

		SendProtocol(proto);
	
		//OnLogout(Asset::KICK_OUT_REASON_LOGOUT); //查询之后退出//架构调整：直接驻留在本服务器内
	}
	else
	{
		DEBUG("玩家:{}由于房间:{}内断线，获取数据:{}", _player_id, message->ShortDebugString(), _stuff.ShortDebugString())

		if (!_room || _room->HasDisMiss() || _room->GetID() != get_data->room_id() || _stuff.room_id() == 0) { SendRoomState(); } //估计房间已经解散
		else { _room->OnReEnter(shared_from_this()); } //再次进入
	}

	return 0;
}

int32_t Player::CmdUpdateRoom(pb::Message* message)
{
	auto update_data = dynamic_cast<Asset::UpdateRoom*>(message);
	if (!update_data) return 1;
	
	if (!_room || !_room->IsVoiceOpen()) return 2;

	if (update_data->voice_member_id() == GetVoiceMemberID()) return 3; //尚未发生变化

	_player_prop.set_voice_member_id(update_data->voice_member_id());

	_room->SyncRoom();
	
	return 0;
}

int32_t Player::CmdLoadScene(pb::Message* message)
{
	Asset::LoadScene* load_scene = dynamic_cast<Asset::LoadScene*>(message);
	if (!load_scene) return 1;

	switch (load_scene->load_type())
	{
		case Asset::LOAD_SCENE_TYPE_START: //加载开始
		{
			_player_prop.set_load_type(Asset::LOAD_SCENE_TYPE_START);
			_player_prop.set_room_id(load_scene->scene_id()); //进入房间ID
		}
		break;
		
		case Asset::LOAD_SCENE_TYPE_SUCCESS: //加载成功
		{
			if (_player_prop.load_type() != Asset::LOAD_SCENE_TYPE_START) return 2;

			auto room_id = _player_prop.room_id();
			
			auto locate_room = RoomInstance.Get(room_id);
			if (!locate_room) return 3; //非法的房间 

			bool is_reenter = (_room == nullptr ? false : room_id == _room->GetID());
			
			SetRoom(locate_room);
				
			_player_prop.clear_load_type(); 
			_player_prop.clear_room_id(); 
	
			if (_stuff.room_id() > 0 && _stuff.room_id() != room_id)
			{
				LOG(ERROR, "玩家:{}加载房间:{}和保存的房间:{}不一致", _player_id, room_id, _stuff.room_id());
			
				_stuff.set_room_id(room_id); 
						
				SetDirty();
			}
				
			OnEnterScene(is_reenter); //进入房间//场景回调
			
			DEBUG("玩家:{} 进入房间:{}成功.", _player_id, room_id);
		}
		break;
		
		default:
		{

		}
		break;
	}

	return 0;
}

void Player::OnEnterScene(bool is_reenter)
{
	SendPlayer(); //发送数据给Client
	
	if (!is_reenter) ClearCards(); //第一次进房间初始化牌局状态

	if (_room) 
	{
		_room->SyncRoom(); //同步当前房间内玩家数据

		if (is_reenter) _room->OnReEnter(shared_from_this()); //房间重入
	}

	DEBUG("玩家:{} 进入房间, 是否重入:{}", _player_id, is_reenter);
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

int32_t Player::CmdSaizi(pb::Message* message)
{
	auto saizi = dynamic_cast<Asset::RandomSaizi*>(message);
	if (!saizi) return 1;

	int32_t result = CommonUtil::Random(1, 6);
	saizi->mutable_random_result()->Add(result);

	SendProtocol(saizi);
	return 0;
}
	
/*
bool Player::AddGameRecord(const Asset::GameRecord& record)
{
	if (!_room) return false;

	int64_t room_id = _room->GetID();

	Asset::RoomHistory* room_history = nullptr;

	for (int32_t i = _stuff.room_history().size() - 1; i >= 0; --i)
	{
		if (_stuff.room_history(i).room_id() == room_id)
		{
			room_history = _stuff.mutable_room_history(i);
			break;
		}
	}

	if (!room_history)
	{
		room_history = _stuff.mutable_room_history()->Add();
		room_history->set_room_id(_room->GetID());
		room_history->set_create_time(CommonTimerInstance.GetTime()); //创建时间
		room_history->mutable_options()->CopyFrom(_room->GetOptions());
	}

	auto list = room_history->mutable_list()->Add();
	list->CopyFrom(record);

	SetDirty();

	DEBUG("player_id:{} add game record:{}", _player_id, record.ShortDebugString());

	return true;
}
*/
	
bool Player::AddRoomRecord(int64_t room_id) 
{ 
	Asset::BattleList message;
	for (auto id : _stuff.room_history()) message.mutable_room_list()->Add(id);
	message.mutable_room_list()->Add(room_id);
	SendProtocol(message);

	auto it = std::find(_stuff.room_history().begin(), _stuff.room_history().end(), room_id);
	if (it == _stuff.room_history().end()) 
	{
		_stuff.mutable_room_history()->Add(room_id); 
		_dirty = true; 
		
		DEBUG("玩家:{}增加房间:{}历史战绩", _player_id, room_id);
	}
	
	OnGameOver(); 

	return true;
}

const std::string Player::GetNickName()
{
	auto wechat = GetWechat();

	auto name = wechat.nickname();

	//
	//如果微信名为空，则用基本属性数据
	//
	if (name.empty()) name = GetName();

	return name;
}

const std::string Player::GetHeadImag()
{
	auto wechat = GetWechat();

	return wechat.headimgurl();
}
	
const std::string Player::GetIpAddress()
{
	//if (!_user.has_client_info() || !_user.client_info().has_client_ip())
	//{
		//auto redis = make_unique<Redis>();
		RedisInstance.GetUser(_stuff.account(), _user);
	//}

	return _user.client_info().ip_address();
}

/////////////////////////////////////////////////////
//游戏逻辑定义
/////////////////////////////////////////////////////
/*
std::vector<Asset::PAI_OPER_TYPE> Player::CheckPai(const Asset::PaiElement& pai, int64_t source_player_id)
{
	std::lock_guard<std::mutex> lock(_card_lock);

	std::vector<Asset::PAI_OPER_TYPE> rtn_check;

	if (CheckHuPai(pai, false, false)) 
	{
		DEBUG("玩家:{} 可以胡来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_HUPAI);
	}
	if (CheckHuiHu(pai, false, false)) 
	{
		DEBUG("玩家:{} 可以会胡来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_HUPAI);
	}
	if (CheckGangPai(pai, source_player_id)) 
	{
		DEBUG("玩家:{} 可以杠来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_GANGPAI);
	}
	if (CheckPengPai(pai)) 
	{
		DEBUG("玩家:{} 可以碰来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_PENGPAI);
	}
	if (CheckChiPai(pai)) 
	{
		DEBUG("玩家:{} 可以吃来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_CHIPAI);
	}
		
	return rtn_check;
}
*/

/*
bool Player::HasPai(const Asset::PaiElement& pai)
{
	if (pai.card_type() <= 0 || pai.card_value() == 0) return false;

	auto type_it = _cards_inhand.find(pai.card_type());
	if (type_it == _cards_inhand.end()) return false;

	auto value_it = std::find(type_it->second.begin(), type_it->second.end(), pai.card_value());
	if (value_it == type_it->second.end()) return false;

	return true;
}
*/
	
bool Player::RemovePai(const Asset::PaiElement& pai)
{
	if (pai.card_type() <= 0 || pai.card_value() == 0) return false;

	auto it = std::find_if(_cards_inhand.begin(), _cards_inhand.end(), [pai](const Asset::PaiElement& card){
				return card.card_type() == pai.card_type() && card.card_value() == pai.card_value();
			});

	if (it == _cards_inhand.end()) return false;

	_cards_inhand.erase(it);
	return true;
}
	
int32_t Player::OnFaPai(std::vector<int32_t>& cards)
{
	//std::lock_guard<std::mutex> lock(_card_lock);

	if (!_room || !_game) return 1;
	
	Asset::PaiNotify notify; //玩家牌数据发给Client
	notify.set_player_id(_player_id); //目标玩家
	
	notify.set_data_type(Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_START); //操作类型：开局

	if (_cards_inhand.size()) //已经有牌，则证明本次发牌是底牌
		notify.set_data_type(Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_DIPAI); //底牌

	for (auto card_index : cards) //发牌到玩家手里
	{
		const auto& card = GameInstance.GetCard(card_index);

		auto pai = notify.mutable_cards()->Add();
		pai->CopyFrom(card);

		_cards_inhand.push_back(card); //插入玩家手牌
	}

	SendProtocol(notify); //发牌给玩家

	if (Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_START == notify.data_type()) 
		notify.mutable_cards()->Clear(); //开局牌数据不通知其他玩家

	Send2Roomers(notify, _player_id); //通知其他玩家底牌

	return 0;
}
	
void Player::PrintPai()
{
	if (!_room || !_game) return;

	std::stringstream card_value_list;

	for (const auto& pai : _cards_inhand)
	{
		auto pai_str = pai.ShortDebugString();
		card_value_list << pai_str;
	}
		
	auto room_id = _room->GetID();
	auto curr_count = _room->GetGamesCount();
	auto open_rands = _room->GetOpenRands();

	LOG(INFO, "玩家:{} 在房间{}，第{}/{}局牌数据:{}", _player_id, room_id, curr_count, open_rands, card_value_list.str());
}
	
Asset::GAME_OPER_TYPE Player::GetOperState() 
{ 
	if (_player_prop.offline()) return Asset::GAME_OPER_TYPE_OFFLINE;

	return _player_prop.game_oper_state(); 
}
	
void Player::SetOffline(bool offline)
{ 
	if (!_room/* || !_game*/) return; //房间状态

	if (offline == _player_prop.offline()) return; //状态尚未发生变化
	
	DEBUG("玩家:{}状态变化:{} 是否离线:{}", _player_id, _player_prop.game_oper_state(), offline);

	_player_prop.set_offline(offline); 

	_room->OnPlayerStateChanged();
}

void Player::ClearCards() 
{
	_chupai_count = 0;

	_cards_inhand.clear(); //清理手里牌
	_cards_pool.clear(); //牌池
 
	_tuoguan_server = false;
	_last_oper_type = _oper_type = Asset::PAI_OPER_TYPE_BEGIN; //初始化操作
	_player_prop.clear_game_oper_state(); //准备//离开

	if (_game) _game.reset();
}

void Player::OnGameOver()
{
	ClearCards();
	
	if (HasTuoGuan()) OnLogout(Asset::KICK_OUT_REASON_LOGOUT);
}

int32_t Player::CmdSayHi(pb::Message* message)
{
	auto say_hi = dynamic_cast<const Asset::SayHi*>(message);
	if (!say_hi) return 1;

	_hi_time = TimerInstance.GetTime();
	
	OnSayHi(); //心跳回复

	return 0;
}

void Player::OnlineCheck()
{
	auto curr_time = TimerInstance.GetTime();
	auto duration_pass = curr_time - _hi_time;

	if (duration_pass <= 0) 
	{
		_pings_count = 0;
		return;
	}

	if (duration_pass > 5)
	{
		++_pings_count;
		
		static int32_t max_allowed = 2;

		if (max_allowed && _pings_count >= max_allowed) 
		{
			SetOffline(); //玩家离线
		}
	}
	else
	{
		SetOffline(false); //玩家上线
		
		_pings_count = 0;
	}
}
	
void Player::OnSayHi()
{
	Asset::SayHi message;
	message.set_heart_count(_heart_count);
	SendProtocol(message);
	
	DEBUG("玩家:{}收到心跳发送心跳", _player_id);
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
	
int32_t Player::CmdSystemChat(pb::Message* message)
{
	auto chat = dynamic_cast<Asset::SystemChat*>(message);
	if (!chat) return 1;

	if (!_room) return 2;

	chat->set_position(GetPosition());

	_room->BroadCast(message);

	return 0;
}
	
int32_t Player::OnKickOut(pb::Message* message)
{
	const auto kick_out = dynamic_cast<const Asset::KickOutPlayer*>(message);
	if (!kick_out) return 1;

	DEBUG("玩家:{} 被踢出逻辑服务器:{} 踢出内容:{}", _player_id, _stuff.server_id(), kick_out->ShortDebugString());
	
	Logout(message);

	return 0;
}
	
int32_t Player::OnPlayerStateChanged(pb::Message* message)
{
	const auto state = dynamic_cast<const Asset::PlayerState*>(message);
	if (!state) return 1;

	switch (state->oper_type())
	{
		case Asset::GAME_OPER_TYPE_OFFLINE:
		{
			SetOffline(); //玩家离线
		}
		break;

		default:
		{
		}
		break;
	}

	return 0;
}
	
const Asset::WechatUnion Player::GetWechat() 
{ 
	if (!_user.has_wechat()) RedisInstance.GetUser(_stuff.account(), _user);

	return _user.wechat();
}
	
void Player::SendRoomState()
{
	Asset::RoomState proto;

	if (_room && !_room->HasDisMiss()) 
	{
		proto.set_room_id(_room->GetID());
	}
	else if (_stuff.room_id() && (_room && !_room->HasDisMiss()))
	{
		auto room = RoomInstance.Get(_stuff.room_id());
		if (room) proto.set_room_id(_stuff.room_id());
	}
	else 
	{
		proto.set_oper_type(Asset::GAME_OPER_TYPE_LEAVE);
	}

	SendProtocol(proto);
}
	
void Player::AddRoomScore(int32_t score)
{
	_stuff.mutable_common_prop()->set_score(_stuff.common_prop().score() + score); 

	if (score > 0)
	{
		_stuff.mutable_common_prop()->set_score_win_rounds(_stuff.common_prop().score_win_rounds() + 1); //获胜
	}
	else
	{
		_stuff.mutable_common_prop()->set_score_win_rounds(_stuff.common_prop().score_win_rounds() - 1);

		if (_stuff.common_prop().score_win_rounds() < 0) _stuff.mutable_common_prop()->set_score_win_rounds(0); //失败
	}

	_dirty = true;
}

//
//当前周期为50MS.
//
//CenterSession::Update()  调用
//
/*
void PlayerManager::Update(int32_t diff)
{
	std::lock_guard<std::mutex> lock(_player_lock);

	++_heart_count;

	if (_heart_count % 20 == 0) //1s
	{
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
	
	if (_heart_count % 3600 == 0) //30mins
	{
		int32_t server_id = ConfigInstance.GetInt("ServerID", 1);
		DEBUG("游戏逻辑服务器:{}在线玩家数量:{}", server_id, _players.size());
	}
}
*/

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

//
//玩家管理
//
void PlayerManager::Emplace(int64_t player_id, std::shared_ptr<Player> player)
{
	if (!player) return;

	std::lock_guard<std::mutex> lock(_player_lock);

	_players[player_id] = player;
}

std::shared_ptr<Player> PlayerManager::GetPlayer(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_player_lock);

	return _players[player_id];
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
	std::lock_guard<std::mutex> lock(_player_lock);

	auto it = _players.find(player_id);
	if (it == _players.end()) return;

	if (!it->second)
	{
		_players.erase(it);
		return;
	}

	auto session = it->second->GetSession();
	if (session) session->RemovePlayer(player_id);

	if (it->second) it->second.reset();
	_players.erase(it);
}

void PlayerManager::Remove(std::shared_ptr<Player> player)
{
	if (!player) return;

	Remove(player->GetID());
}
	
void PlayerManager::BroadCast(const pb::Message& message)
{
	std::lock_guard<std::mutex> lock(_player_lock);

	for (auto it = _players.begin(); it != _players.end(); ++it)
	{
		auto player = it->second;
		if (!player) continue;

		player->SendProtocol(message);
	}
}

void Player::OnBind(std::string account)
{
	_stuff.set_agent_account(account);

	SendPlayer(); //同步角色数据

	_dirty = true;
}
	
void Player::OnQuitClan(int64_t clan_id)
{
	for (int32_t i = 0; i < _stuff.clan_hosters().size(); ++i) //茶馆老板
	{
		if (clan_id != _stuff.clan_hosters(i)) continue;

		_stuff.mutable_clan_hosters()->SwapElements(i, _stuff.clan_hosters().size() - 1);
		_stuff.mutable_clan_hosters()->RemoveLast();
	}
	
	for (int32_t i = 0; i < _stuff.clan_joiners().size(); ++i) //茶馆成员
	{
		if (clan_id != _stuff.clan_joiners(i)) continue;

		_stuff.mutable_clan_joiners()->SwapElements(i, _stuff.clan_joiners().size() - 1);
		_stuff.mutable_clan_joiners()->RemoveLast();
	}
	
	if (_stuff.selected_clan_id() == clan_id) _stuff.set_selected_clan_id(0);

	Asset::ClanOperation message;
	message.set_oper_type(Asset::CLAN_OPER_TYPE_CLAN_LIST_QUERY);
	ClanInstance.OnQueryClanList(shared_from_this(), &message);

	DEBUG("玩家:{} 退出茶馆:{} 成功", _player_id, clan_id);

	_dirty = true;
}
	
void Player::OnClanCreated(int64_t clan_id) 
{ 
	auto it = std::find(_stuff.clan_hosters().begin(), _stuff.clan_hosters().end(), clan_id);
	if (it != _stuff.clan_hosters().end()) return;
	
	DEBUG("玩家:{} 成功创建茶馆:{}", _player_id, clan_id);

	_stuff.mutable_clan_hosters()->Add(clan_id); 
	_dirty = true; 
}
	
bool Player::OnClanJoin(int64_t clan_id) 
{ 
	auto it = std::find(_stuff.clan_joiners().begin(), _stuff.clan_joiners().end(), clan_id);
	if (it != _stuff.clan_joiners().end()) return false;

	DEBUG("玩家:{} 成功加入茶馆:{}", _player_id, clan_id);
 
	_stuff.mutable_clan_joiners()->Add(clan_id); 
	_dirty = true; 

	return true;
}

void Player::SetCurrClan(int64_t clan_id) 
{ 
	if (clan_id == _stuff.selected_clan_id()) return; 
	_stuff.set_selected_clan_id(clan_id); 
	_dirty = true; 
} 

void Player::OnClanCheck()
{
	std::vector<int64_t> clan_list;

	for (int32_t i = 0; i < _stuff.clan_hosters().size(); ++i) //茶馆老板
	{
		auto clan_id = _stuff.clan_hosters(i);

		Asset::Clan clan;
		bool has_clan = ClanInstance.GetClan(clan_id, clan);

		if (!has_clan || clan.dismiss()) continue;

		clan_list.push_back(clan_id);
	}

	_stuff.mutable_clan_hosters()->Clear(); //删除茶馆
	for (auto clan_id : clan_list) _stuff.mutable_clan_hosters()->Add(clan_id);
	
	clan_list.clear();

	for (int32_t i = 0; i < _stuff.clan_joiners().size(); ++i) //茶馆成员
	{
		auto clan_id = _stuff.clan_joiners(i);
		
		Asset::Clan clan;
		bool has_clan = ClanInstance.GetClan(clan_id, clan);

		if (!has_clan || clan.dismiss()) continue;

		clan_list.push_back(clan_id);
	}
	
	_stuff.mutable_clan_joiners()->Clear(); //删除茶馆
	for (auto clan_id : clan_list) _stuff.mutable_clan_joiners()->Add(clan_id);

	if (_stuff.clan_hosters().size() == 0 && _stuff.clan_joiners().size() == 0) _stuff.set_selected_clan_id(0); //如果尚未存在，则直接删除
}

bool Player::IsHoster(int64_t clan_id)
{
	if (clan_id <= 0) return true;

	for (int32_t i = 0; i < _stuff.clan_hosters().size(); ++i) //茶馆老板
	{
		if (clan_id == _stuff.clan_hosters(i)) return true;
	}

	return false;
}

}
