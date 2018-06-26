#include <vector>
#include <algorithm>

#include <boost/asio.hpp>
#include <cpp_redis/cpp_redis>

#include "Room.h"
#include "Clan.h"
#include "Game.h"
#include "MXLog.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "Timer.h"
#include "Activity.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

//
//房间
//

Asset::ERROR_CODE Room::TryEnter(std::shared_ptr<Player> player)
{
	std::lock_guard<std::mutex> lock(_mutex); //房间状态锁

	if (!player) return Asset::ERROR_INNER;

	if (player->GetLocalRoomID() == GetID()) return Asset::ERROR_SUCCESS; //重入

	if (HasPlayer(player)) 
	{
		return Asset::ERROR_ROOM_HAS_BEEN_IN; //已经在房间
	}
	
	if (IsFull()) 
	{
		return Asset::ERROR_ROOM_IS_FULL; //房间已满
	}
	else if (HasBeenOver()) 
	{
		return Asset::ERROR_ROOM_BEEN_OVER; //战局结束
	}
	else if (HasDisMiss()) 
	{
		return Asset::ERROR_ROOM_BEEN_DISMISS; //房间已经解散
	}

	DEBUG("玩家:{} 进入房间:{} 成功", player->GetID(), GetID());

	return Asset::ERROR_SUCCESS;
}

//
//玩家退出，位置需要为后面加入的玩家空置出来
//
//防止玩家进入和退出，位置不一致的情况
//
bool Room::IsFull() 
{ 
	std::lock_guard<std::mutex> lock(_player_mutex);

	if (_players.size() < (size_t)MAX_PLAYER_COUNT) return false;

	for (auto player : _players)
	{
		if (!player) return false;
	}

	return true;
} 
	
bool Room::IsEmpty()
{
	std::lock_guard<std::mutex> lock(_player_mutex);

	if (_players.size() == 0) return true;
	
	for (auto player : _players)
	{
		if (player) return false;
	}

	return true;
}
	
bool Room::HasPlayer(std::shared_ptr<Player> player_ptr)
{
	if (!player_ptr) return false;

	return HasPlayer(player_ptr->GetID());
}
	
bool Room::HasPlayer(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_player_mutex);

	for (auto player : _players)
	{
		if (!player) continue;

		if (player->GetID() == player_id) return true;
	}

	return false;
}

bool Room::Enter(std::shared_ptr<Player> player)
{
	if (!player) return false;
	
	auto enter_status = TryEnter(player);
	
	//std::lock_guard<std::mutex> lock(_player_mutex); //TryEnter已经加锁检查，此处不必加锁，也防止OnReEnter中死锁

	if (enter_status != Asset::ERROR_SUCCESS && enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN) 
	{
		LOG(ERROR, "玩家:{} 无法进入房间:{} 原因:{}", player->GetID(), GetID(), enter_status);
		return false; //进入房间之前都需要做此检查，理论上不会出现
	}

	if (MAX_PLAYER_COUNT == _players.size())
	{
		for (size_t i = 0; i < _players.size(); ++i)
		{
			auto& player_in = _players[i];

			if (!player_in && enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN) //当前玩家尚未在房间内，则选择新位置
			{
				player_in = player;
				player->SetPosition((Asset::POSITION_TYPE)(i + 1)); //设置位置
				break;
			}
			else if (player_in && player_in->GetID() == player->GetID()) //当前还在房间内
			{
				OnReEnter(player);

				return true;
			}
		}
	}
	else if (enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN)
	{
		_players.push_back(player); //进入房间
		player->SetPosition((Asset::POSITION_TYPE)_players.size()); //设置位置
	}
	
	UpdateClanStatus(); //茶馆房间状态同步
	
	return true;
}
	
void Room::OnReEnter(std::shared_ptr<Player> op_player)
{
	if (!op_player) return;
	
	op_player->SetOffline(false); //回到房间内

	//
	//同步玩家信息
	//
	SyncRoom();

	if (!HasStarted() || HasBeenOver()/*单纯记录局数不能判定对局已经结束*/) return; //尚未开局或者已经对局结束

	//
	//房间内玩家数据推送
	//
	Asset::RoomAll message;
	message.set_current_rounds(_games.size());
	message.set_zhuang_position(Asset::POSITION_TYPE(_banker_index + 1));

	for (const auto& rob_element : _rob_dizhu) //抢地主状态
	{
		auto element_ptr = message.mutable_rob_list()->Add();
		element_ptr->CopyFrom(rob_element);
	}
	
	for (const auto& record : _history.list()) //历史分数
	{	
		auto hist_record = message.mutable_list()->Add();
		hist_record->CopyFrom(record); //分数

		for (int32_t i = 0; i < hist_record->list().size(); ++i)
		{
			if (message.player_brief_list().size() < MAX_PLAYER_COUNT)
			{
				auto player_brief = message.mutable_player_brief_list()->Add();
				player_brief->set_player_id(hist_record->list(i).player_id());
				player_brief->set_nickname(hist_record->list(i).nickname());
				player_brief->set_headimgurl(hist_record->list(i).headimgurl());
			}

			hist_record->mutable_list(i)->clear_nickname();
			hist_record->mutable_list(i)->clear_headimgurl();
			hist_record->mutable_list(i)->mutable_details()->Clear();
		}
	}

	if (!_game) //不在进行中
	{
		op_player->SendProtocol(message);
		return;
	}

	//
	//牌局通用信息
	//
	int32_t dizhu_position = GetPlayerOrder(_game->GetDiZhu());
	if (Asset::POSITION_TYPE_IsValid(dizhu_position)) message.set_dizhu_position(Asset::POSITION_TYPE(dizhu_position + 1));

	message.set_curr_operator_position(Asset::POSITION_TYPE(_game->GetCurrPlayerIndex() + 1));
	message.mutable_pai_oper()->CopyFrom(_game->GetOperCache().pai_oper()); //上家打牌
	message.set_beishu(_game->GetBeiLv());

	for (const auto dipai : _game->GetDiPai())
	{
		auto pai = message.mutable_dipai()->Add();
		pai->CopyFrom(dipai);
	}

	//
	//牌局相关数据推送
	//
	auto players = GetPlayers();
	for (const auto player : players)
	{
		if (!player) continue;

		auto player_list = message.mutable_player_list()->Add();
		player_list->set_player_id(player->GetID());
		player_list->set_position(player->GetPosition());
		player_list->set_pai_count_inhand(player->GetCardsCountInhand()); //手牌数量

		if (op_player->GetID() == player->GetID())
		{
			const auto& cards_inhand = player->GetCardsInhand();
			for (const auto& card : cards_inhand)
			{
				auto pai_ptr = player_list->mutable_cards_inhand()->Add(); //自己手牌数据
				pai_ptr->CopyFrom(card);
			}
		}
	}

	op_player->SendProtocol(message);
}

void Room::OnPlayerLeave(int64_t player_id)
{
	UpdateClanStatus(); //茶馆房间状态同步

	SyncRoom(); //同步当前房间内玩家数据
}

std::shared_ptr<Player> Room::GetHoster()
{
	return _hoster;
}

bool Room::IsHoster(int64_t player_id)
{
	if (_stuff.clan_id()) //茶馆房，老板可以作为房主逻辑
	{
		Asset::Clan clan;
		bool has_clan = ClanInstance.GetCache(_stuff.clan_id(), clan);
		if (has_clan && clan.hoster_id() == player_id) return true;
	}

	auto host = GetHoster();
	if (!host) return false;

	return host->GetID() == player_id;
}

std::shared_ptr<Player> Room::GetPlayer(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_player_mutex);
	
	for (auto player : _players)
	{
		if (player->GetID() == player_id) return player;
	}

	return nullptr;
}

void Room::OnPlayerOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	//std::lock_guard<std::mutex> lock(_mutex); //此处不必加锁，在各个操作的里面进行加锁

	if (!player || !message) return;

	auto game_operate = dynamic_cast<Asset::GameOperation*>(message);
	if (!game_operate) return;
	
	if (game_operate->oper_type() != Asset::GAME_OPER_TYPE_JIAOZHUANG && game_operate->oper_type() != Asset::GAME_OPER_TYPE_JIABEI) BroadCast(game_operate); //广播玩家操作

	switch(game_operate->oper_type())
	{
		case Asset::GAME_OPER_TYPE_START: //开始游戏：准备//扑克押注
		{
			if (!CanStarGame()) return;

			_game = std::make_shared<Game>();

			_game->Init(shared_from_this()); //洗牌

			_games.push_back(_game); //游戏
			
			_game->Start(_players, _stuff.room_id(), _games.size()); //开始游戏

			OnGameStart();
		}
		break;

		case Asset::GAME_OPER_TYPE_LEAVE: //离开游戏
		{
			if ((IsFriend() || IsClanMatch()) && !HasDisMiss() && HasStarted() && !HasBeenOver()) return; //好友房没有解散，且没有对战完，则不允许退出
			
			if (IsMatch() && !IsGaming()) return; //非好友房结束可以直接退出

			if (IsHoster(player->GetID()) && !IsClan()) //非茶馆房间
			{
				if (_games.size() == 0) //尚未开局
				{
					KickOutPlayer(); //如果房主离开房间，且此时尚未开局，则直接解散
				}
				else
				{
					Remove(player->GetID(), Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
				}
			}
			else if (IsEmpty())
			{
				player->OnLeaveRoom(Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
			}
			else
			{
				Remove(player->GetID(), Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
			}
		}
		break;
		
		case Asset::GAME_OPER_TYPE_KICKOUT: //踢人
		{
			if (!IsHoster(player->GetID()))
			{
				player->AlertMessage(Asset::ERROR_ROOM_NO_PERMISSION);
				return;
			}

			Remove(game_operate->destination_player_id(), Asset::GAME_OPER_TYPE_KICKOUT); //玩家退出房间
		}
		break;

		case Asset::GAME_OPER_TYPE_DISMISS_AGREE: //解散
		{
			OnDisMiss(player->GetID(), game_operate);

			//
			//如果房主发起解散房间，且此时尚未开局，则直接解散
			//
			if (IsHoster(player->GetID()) && _games.size() == 0) //GMT开房没有房主
			{
				KickOutPlayer();
			}
			else if (IsGmtOpened() && (!HasStarted() || HasBeenOver()))
			{
				Remove(player->GetID(), Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
			}
			else if (IsEmpty())
			{
				player->OnLeaveRoom(Asset::GAME_OPER_TYPE_DISMISS_AGREE); //玩家退出房间
			}
			else if (CanDisMiss()) 
			{
				DoDisMiss();
			}
		}
		break;

		case Asset::GAME_OPER_TYPE_DISMISS_DISAGREE: //不解散
		{
			OnDisMiss(player->GetID(), game_operate);

			ClearDisMiss();
		}
		break;

		case Asset::GAME_OPER_TYPE_JIAOZHUANG: //叫庄
		{
			if (!_game) break; //尚未开局

			if (!OnJiaoZhuang(player->GetID(), game_operate->beilv())) 
			{
				LOG(ERROR, "玩家:{} 叫庄失败，错误数据:{}", player->GetID(), game_operate->ShortDebugString());
				return;
			}
	
			if (MAX_PLAYER_COUNT == _no_robed_count) //都不叫地主
			{
				BroadCast(game_operate);

				ResetGame(); //刷新下一局

				break;
			}
			
			BroadCast(game_operate);

			if (!_game->CanStart()) break; //是否可以开局//检查是否都叫完地主

			//auto dizhu_ptr = GetPlayer(_game->GetDiZhu());
			//if (!dizhu_ptr) break;

			_game->OnStarted(); //直接开始
		}
		break;

		case Asset::GAME_OPER_TYPE_JIABEI:
		{
			if (!_game) break; //尚未开局

			if (!OnJiaBei(player, game_operate->beilv())) 
			{
				LOG(ERROR, "玩家:{} 加倍失败，错误数据:{}", player->GetID(), game_operate->ShortDebugString());
				return;
			}
			
			BroadCast(game_operate);
		}
		break;
		
		default:
		{
			return;
		}
		break;
	}
}
	
int32_t Room::GetRemainCount() 
{ 
	return _stuff.options().open_rands() - _games.size(); 
}
	
bool Room::HasBeenOver() 
{ 
	//if (!IsFriend() && !IsClanMatch()) return false;
	if (IsMatch()) return false;

	return !_game && GetRemainCount() <= 0; 
}

bool Room::Remove(int64_t player_id, Asset::GAME_OPER_TYPE reason)
{
	std::lock_guard<std::mutex> lock(_player_mutex);

	for (size_t i = 0; i < _players.size(); ++i)
	{
		auto& player = _players[i];
		if (!player) continue;

		if (player->GetID() != player_id) continue;
			
		player->OnLeaveRoom(reason); //玩家退出房间
		player.reset();

		OnPlayerLeave(player_id); //玩家离开房间
		
		return true;
	}

	return false;
}
	
void Room::OnPlayerStateChanged()
{
	Asset::RoomInformation message;
	message.set_sync_type(Asset::ROOM_SYNC_TYPE_STATE_CHANGED);
			
	auto players = GetPlayers();	
	for (const auto player : players)
	{
		if (!player) continue;

		auto p = message.mutable_player_list()->Add();
		p->set_player_id(player->GetID());
		p->set_position(player->GetPosition());
		p->set_oper_type(player->GetOperState());
	}

	BroadCast(message);
}

//
//斗地主开局
//
//开局各发17张牌，剩余3张牌待叫完地主后进行发牌
//
void Room::OnGameStart()
{
	if (!_game) return;

	std::lock_guard<std::mutex> lock(_mutex);

	Asset::GameStart game_start;
	game_start.set_total_rounds(_stuff.options().open_rands());
	game_start.set_current_rounds(_games.size());

	BroadCast(game_start); //广播牌局信息

	auto players = GetPlayers();
	for (auto player : players)
	{
		if (!player) continue;
		player->SetOperState(Asset::GAME_OPER_TYPE_ONLINE); //设置各个玩家的开局状态
	}

	DEBUG("房间:{} 开局:{}", _stuff.room_id(), game_start.ShortDebugString());
}
	
void Room::ResetGame(std::shared_ptr<Game> game)
{
	if (_game) _game.reset();

	_game = game;

	_rob_dizhu.clear();
	_no_robed_count = 0;

	if (!game) _game = std::make_shared<Game>();

	_game->Init(shared_from_this()); //洗牌
	_game->Start(_players, _stuff.room_id(), _games.size()); //开始游戏//防止回放数据不正确

	DEBUG("房间:{} 刷新局数:{}", _stuff.room_id(), _game->GetID());
}
	
/*
void Room::AddHupai(int64_t player_id) 
{ 
	if (player_id <= 0) return;

	++_hupai_players[player_id]; 

	auto player = GetPlayer(player_id);
	if (!player) return;

	player->AddTotalWinRounds();
	player->SetStreakWins(1);
}
*/

void Room::OnClanOver()
{
	if (!IsClan()) return;
	
	//RedisInstance.SaveRoomHistory(_stuff.room_id(), _history); //茶馆房间，存储战绩信息
	//if (_history.list().size() == 0) return;
	
	Asset::ClanRoomStatusChanged proto;
	proto.set_created_time(_created_time);
	proto.mutable_room()->CopyFrom(_stuff);
	proto.set_status(Asset::CLAN_ROOM_STATUS_TYPE_OVER);
	proto.mutable_player_list()->CopyFrom(_history.player_brief_list());
	proto.set_games_count(GetGamesCount()); //开局数量

	WorldInstance.BroadCast2CenterServer(proto); //通知茶馆房间结束
}

void Room::OnGameOver(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_mutex); //状态锁

	_no_robed_count = 0;

	_rob_dizhu.clear();

	if (_game) _game.reset();

	//if (!IsFriend() && !IsClanMatch()) return; //匹配房没有总结算
	if (IsMatch()) return;
	
	UpdateClanStatus(); //茶馆房间状态同步
	
	auto player = GetPlayer(player_id);

	if (player) 
	{ 
		player->AddTotalWinRounds(); //赢牌总数
		player->SetStreakWins(1); //初始化
	} 

	//连胜
	//
	if (_banker != player_id) 
	{
		auto banker = GetPlayer(_banker);

		if (banker) 
		{ 
			banker->SetStreakWins(_lian_shengs[_banker]); //最高连胜

			_lian_shengs[_banker] = 0; //连胜中断 
		} 
	}
	else if (player_id != 0) //不是解散
	{
		++_lian_shengs[player_id]; //连胜
	}
		
	_banker_index = GetPlayerOrder(player_id); //先走的玩家下一局坐庄

	if (!HasBeenOver() && !HasDisMiss()) return; //没有对局结束，且没有解散房间

	if (_games.size() == 0) 
	{
		if (IsClanMatch()) { LOG(ERROR, "房间:{} 数据:{} 尚未开始比赛，没有进行牌局", _stuff.room_id(), _stuff.ShortDebugString()); }
		else { return; } //没有对局
	}
	
	std::lock_guard<std::mutex> plock(_player_mutex); //玩家锁

	for (auto player : _players)
	{
		if (!player) continue;

		if (_history.list().size()) player->AddRoomRecord(GetID());
	}

	//
	//总结算界面弹出
	//
	Asset::RoomCalculate message;
	message.set_calculte_type(Asset::CALCULATE_TYPE_FULL);
	if (HasDisMiss()) message.set_calculte_type(Asset::CALCULATE_TYPE_DISMISS);

	for (auto player : _players)
	{
		if (!player) continue;

		auto player_id = player->GetID();

		auto record = message.mutable_record()->Add();
		record->set_player_id(player_id);
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());

		record->set_pk_count(_games.size()); //对局数
		record->set_dizhu_count(_dizhu_rounds[player_id]); //地主局数
		record->set_win_count(_winner_rounds[player_id]); //胜利局数
		record->set_zhadan_count(_zhadan_count[player_id]); //炸弹数量

		for(int i = 0; i < _history.list().size(); ++i)
			for (int j = 0; j < _history.list(i).list().size(); ++j)
				if (player_id == _history.list(i).list(j).player_id())
					record->set_score(record->score() + _history.list(i).list(j).score());

		auto player_brief = _history.mutable_player_brief_list()->Add();
		player_brief->set_player_id(player_id);
		player_brief->set_nickname(player->GetNickName());
		player_brief->set_headimgurl(player->GetHeadImag());
		player_brief->set_score(record->score());
		//player_brief->set_hupai_count(record->win_count());
		//player_brief->set_dianpao_count(record->dianpao_count());

		player->AddRoomScore(record->score()); //总积分
	}

	LOG(INFO, "房间:{} 整局结算，房间局数:{} 实际局数:{} 结算数据:{}", 
			_stuff.room_id(), _stuff.options().open_rands(), _games.size(), message.ShortDebugString());

	for (auto player : _players)
	{
		if (!player/* || player->IsOffline()*/) continue;
		player->SendProtocol(message);
	}
	
	OnClanOver(); //茶馆房间数据同步
	
	_history.Clear();
	_winner_rounds.clear();
	_dizhu_rounds.clear();
	_zhadan_count.clear();
}

void Room::AddGameRecord(const Asset::GameRecord& record)
{
	_history.mutable_list()->Add()->CopyFrom(record);
	RedisInstance.SaveRoomHistory(_stuff.room_id(), _history);

	LOG(INFO, "存储房间:{} 历史战绩:{}", _stuff.room_id(), _history.ShortDebugString());
}

void Room::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	if (!message) return;
			
	std::lock_guard<std::mutex> plock(_player_mutex); //玩家锁

	for (auto player : _players)
	{
		if (!player) continue; //可能已经释放//或者退出房间

		if (exclude_player_id == player->GetID()) continue;

		player->SendProtocol(message);
	}
}
	
void Room::BroadCast(pb::Message& message, int64_t exclude_player_id)
{
	BroadCast(&message, exclude_player_id);
}
	
void Room::OnRemove()
{
	std::lock_guard<std::mutex> lock(_mutex);

	WARN("房间:{} 删除成功", _stuff.room_id());

	auto players = GetPlayers();
	for (auto player : players)
	{
		if (!player) continue;

		player->OnRoomRemoved(_stuff.room_id());
		player.reset();
	}
	
	/*
	Asset::ClanRoomStatusChanged proto;
	proto.mutable_room()->CopyFrom(_stuff);
	proto.set_created_time(_created_time);
	proto.set_status(Asset::CLAN_ROOM_STATUS_TYPE_OVER);
	WorldInstance.BroadCast2CenterServer(proto); //通知茶馆房间结束
	*/

	OnClanOver();
}

void Room::OnDisMiss(int64_t player_id, pb::Message* message)
{
	if (!message) return;

	if ((IsGmtOpened() || IsClan()) && (!HasStarted() || HasBeenOver())) return; //代开房//茶馆房没开局不允许解散

	if (_dismiss_time == 0) 
	{
		_dismiss_time = CommonTimerInstance.GetTime() + g_const->room_dismiss_timeout();
		_dismiss_cooldown = CommonTimerInstance.GetTime() + g_const->room_dismiss_cooldown();
	}

	Asset::RoomDisMiss proto;

	for (auto player : _players)
	{
		if (!player) continue;

		if (Asset::GAME_OPER_TYPE_DISMISS_AGREE != player->GetOperState() && 
				Asset::GAME_OPER_TYPE_DISMISS_DISAGREE != player->GetOperState()) continue;

		auto list = proto.mutable_list()->Add();
		list->set_player_id(player->GetID());
		list->set_position(player->GetPosition());
		list->set_oper_type(player->GetOperState());
	}

	DEBUG("玩家:{} 解散房间:{} 操作:{}", player_id, _stuff.room_id(), message->ShortDebugString());

	BroadCast(proto); //投票状态
}

void Room::DoDisMiss()
{
	DEBUG("房间:{} 解散成功", _stuff.room_id());

	_is_dismiss = true;
					
	if (_game) { _game->OnGameOver(0); } //当前局数结束
	else { OnGameOver(); }
}

bool Room::OnJiaBei(std::shared_ptr<Player> player, int32_t beilv) 
{
	if (!player || !_game) return false;
	
	if (!IsJiaoFenMode()) return false; //非叫分模式不能加倍
	
	auto player_id = player->GetID();
	auto dizhu_id = _game->GetDiZhu();

	Asset::RobElement rob_element;
	rob_element.set_player_id(player_id);
	rob_element.set_beilv(beilv);
	rob_element.set_oper_type(Asset::GAME_OPER_TYPE_JIABEI);
	
	DEBUG("玩家:{} 房间:{} 局数:{} 地主:{} 加倍:{}", player_id, _stuff.room_id(), _game->GetID(), dizhu_id, rob_element.ShortDebugString());
	
	if (beilv <= 0)
	{
		_rob_dizhu.push_back(rob_element); //加倍状态缓存
	
		//if (player_id == dizhu_id) return true; //地主不加倍则直接开始
		return true;
	}
	
	//1.地主加倍限制
	//
	//地主必须农民加倍后才可以加倍，地主加倍后方是正式开局
	//
	if (player_id == dizhu_id) 
	{
		int32_t count = std::count_if(_rob_dizhu.begin(), _rob_dizhu.end(), [](const Asset::RobElement& rob_element){
					return rob_element.oper_type() == Asset::GAME_OPER_TYPE_JIABEI;
				});
		if (count == 0) return false; //地主只有在农民加倍的时候可以反加倍，农民不加倍则直接退出

		for (auto member : _players)
		{
			if (!member) return false;

			int64_t member_id = member->GetID();

			if (member_id == dizhu_id) continue;

			auto it = std::find_if(_rob_dizhu.begin(), _rob_dizhu.end(), [member_id](const Asset::RobElement& rob_element){
						return rob_element.oper_type() == Asset::GAME_OPER_TYPE_JIABEI && member_id == rob_element.player_id() && rob_element.beilv() > 0;
					});
			if (it == _rob_dizhu.end()) continue; //农民是否加倍，没加过倍则过

			member->OnJiaBei(); //加倍
		}
	
		//_game->OnRealStarted(); //正式开局
	}
	//2.农民叫分限制
	//
	//农民必须是还没有机会叫地主才可以加倍，比如，A直接叫3分，B、C牌型较好则可以加倍
	//
	else
	{
		for (const auto& element : _rob_dizhu)
		{
			if (player_id == dizhu_id) continue; 

			if (element.player_id() == player_id && element.beilv() > 0) return false; //已经加倍或者叫地主了，不能加倍
		}
	
		player->OnJiaBei(); //加倍
	}
	
	_rob_dizhu.push_back(rob_element); //加倍状态缓存

	return true;
}

bool Room::OnJiaoZhuang(int64_t player_id, int32_t beilv)
{
	if (!_game || _game->IsStarted()) return false; //尚未开局或已经开局不能继续叫地主

	if (player_id <= 0) return false;
	
	if (_rob_dizhu.size() > 0 && _rob_dizhu[_rob_dizhu.size() - 1].player_id() == player_id) 
	{
		ERROR("玩家:{} 可能由于弱网络条件下多次点击, 此时倍率:{} 已经操作玩家数量:{}", player_id, beilv, _rob_dizhu.size());

		return false; //防止弱网络条件下玩家多次点击
	}

	Asset::RobElement rob_element;
	rob_element.set_player_id(player_id);
	rob_element.set_beilv(beilv);
	rob_element.set_oper_type(Asset::GAME_OPER_TYPE_JIAOZHUANG);

	if (beilv <= 0) 
	{
		auto it = std::find_if(_rob_dizhu.begin(), _rob_dizhu.end(), [player_id](const Asset::RobElement& element){
					return element.player_id() == player_id && element.oper_type() == Asset::GAME_OPER_TYPE_JIAOZHUANG && element.beilv() <= 0;
				});
		if (it != _rob_dizhu.end()) 
		{
			ERROR("玩家:{} 可能由于弱网络条件下多次点击，操作数据:{}", player_id, rob_element.ShortDebugString());

			return false; //防止玩家多次连击
		}

		++_no_robed_count; //不叫地主
	}
	
	_rob_dizhu.push_back(rob_element); //叫地主状态缓存
	
	if (MAX_PLAYER_COUNT == _no_robed_count) return true; //都不叫地主
			
	/*
	if (MAX_PLAYER_COUNT == _no_robed_count) //都不叫地主
	{
		ResetGame(); //刷新下一局
		return true;
	}
	*/

	if (_stuff.options().zhuang_type() == Asset::ZHUANG_TYPE_JIAOFEN)
	{
		_game->OnRobDiZhu(player_id, beilv);
	}
	else if (_stuff.options().zhuang_type() == Asset::ZHUANG_TYPE_QIANGDIZHU)
	{
		_game->OnRobDiZhu(player_id, beilv > 0); //是否叫地主
			
		//if (!_game->CanStart()) return false; //是否可以开局//检查是否都叫完地主
	}
	else
	{
		ERROR("玩家:{} 叫庄错误:{}", player_id, _stuff.ShortDebugString());
		return false;
	}
		
	/*
	if (!_game->CanStart()) return true; //是否可以开局//检查是否都叫完地主

	auto dizhu_ptr = GetPlayer(_game->GetDiZhu());
	if (!dizhu_ptr) return false;

	_game->OnStarted(dizhu_ptr); //直接开始
	*/

	//DEBUG("房间:{} 开局:{} 地主:{}", _stuff.room_id(), _game->GetID(), _game->GetDiZhu());

	return true;
}

void Room::KickOutPlayer(int64_t player_id)
{
	if (player_id > 0)
	{
		Remove(player_id, Asset::GAME_OPER_TYPE_HOSTER_DISMISS); //加锁踢人
	}
	else
	{
		const auto players = GetPlayers();	
		for (const auto player : players)
		{
			if (!player) continue;

			Remove(player->GetID(), Asset::GAME_OPER_TYPE_HOSTER_DISMISS); //加锁踢人
		}
	}

	_is_dismiss = true;
}
	
void Room::SyncRoom()
{
	Asset::RoomInformation message;
	message.set_sync_type(Asset::ROOM_SYNC_TYPE_NORMAL);
			
	//auto players = GetPlayers();	
	for (const auto player : _players)
	{
		if (!player) continue;

		auto p = message.mutable_player_list()->Add();
		p->set_position(player->GetPosition());
		p->set_player_id(player->GetID());
		p->set_oper_type(player->GetOperState());
		p->mutable_common_prop()->CopyFrom(player->CommonProp());
		p->mutable_wechat()->CopyFrom(player->GetWechat());
		p->set_ip_address(player->GetIpAddress());
		p->set_voice_member_id(player->GetVoiceMemberID());
		/*
		for (auto dis_player : _players)
		{
			if (!dis_player || dis_player->GetID() == player->GetID()) continue;

			auto dis_element = p->mutable_dis_list()->Add();
			dis_element->set_position(dis_player->GetPosition());

			auto distance = RedisInstance.GetDistance(dis_player->GetID(), player->GetID());
			dis_element->set_distance(distance);

			//DEBUG("获取玩家{}和玩家{}之间的距离:{}", dis_player->GetID(), player->GetID(), distance);
		}
		*/
	}

	DEBUG("同步房间:{} 数据:{}", _stuff.room_id(), message.ShortDebugString());

	BroadCast(message);
}

void Room::OnCreated(std::shared_ptr<Player> hoster) 
{ 
	_hoster = hoster;
	if (hoster) _hoster_id = hoster->GetID();

	_created_time = CommonTimerInstance.GetTime(); //创建时间

	if (IsClanMatch()) //比赛房间时间限制
	{
		SetExpiredTime(_created_time + g_const->match_room_last_time());
	}
	else
	{
		SetExpiredTime(_created_time + g_const->room_last_time());
	}
	
	_history.set_room_id(GetID());
	_history.set_create_time(CommonTimerInstance.GetTime()); //创建时间
	_history.mutable_options()->CopyFrom(GetOptions());
	
	if (Asset::ROOM_TYPE_CLAN_MATCH == _stuff.room_type()) //比赛房间
	{
		Asset::Clan clan;
		if (!ClanInstance.GetCache(_stuff.clan_id(), clan)) return;
				
		_ticket_count = clan.match_history().open_match().ticket_count(); //门票数量
	}
	
	UpdateClanStatus(); //茶馆房间状态同步

	LOG(INFO, "玩家:{} 创建房间:{} 玩法:{}成功", _hoster_id, _stuff.room_id(), _stuff.ShortDebugString());
}
	
bool Room::CanStarGame()
{
	if (IsFriend() && !_hoster && !_gmt_opened) return false; //好友房检查

	std::lock_guard<std::mutex> lock(_player_mutex); //玩家锁

	if (_players.size() != MAX_PLAYER_COUNT) return false; //玩家数量检查

	for (auto player : _players)
	{
		if (!player) return false;
		if (!player->IsReady()) return false; //玩家都是准备状态
	}
	
	auto room_type = _stuff.room_type();
	
	//
	//开始游戏
	//
	//消耗房卡//欢乐豆
	//
	if (Asset::ROOM_TYPE_FRIEND == room_type) //好友房
	{
		if (GetRemainCount() <= 0) 
		{
			LOG(ERROR, "房间:{}牌局结束，不能继续进行游戏，总局数:{} 当前局数:{}", _stuff.room_id(), _stuff.options().open_rands(), _games.size());
			return false;
		}

		auto activity_id = g_const->room_card_limit_free_activity_id();
		if (ActivityInstance.IsOpen(activity_id)) return true;

		if (IsGmtOpened()) 
		{
			LOG(INFO, "GMT开房，不消耗房卡数据:{}", _stuff.ShortDebugString());
			return true;
		}
			
		const auto room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
		if (!room_card || room_card->rounds() <= 0) return false;

		auto consume_count = GetOptions().open_rands() / room_card->rounds(); //待消耗房卡数

		if (IsClan()) //茶馆：开局消耗，到中心服务器消耗
		{
			if (_games.size()) return true; //已经开局，不再进行房卡检查

			Asset::Clan clan;
			auto has_record = ClanInstance.GetClan(_stuff.clan_id(), clan);
			if (!has_record || clan.room_card_count() < consume_count) return false;

			if (_games.size() == 0) OnClanCreated(); //茶馆房
			return true;
		}

		if (!_hoster) return false; //没有房主，正常消耗

		if (_hoster && _games.size() == 0) //开局消耗
		{
			auto pay_type = GetOptions().pay_type(); //付费方式
		
			switch (pay_type)
			{
				case Asset::ROOM_PAY_TYPE_HOSTER: //房主付卡
				{
					if (!_hoster->CheckRoomCard(consume_count)) 
					{
						_hoster->AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
						return false;
					}
					_hoster->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_OPEN_ROOM, consume_count); //消耗
				}
				break;
				
				case Asset::ROOM_PAY_TYPE_AA: //AA付卡
				{
					consume_count = consume_count / MAX_PLAYER_COUNT; //单人付卡数量

					for (auto player : _players) //房卡检查
					{
						if (!player) return false;

						if (!player->CheckRoomCard(consume_count)) 
						{
							player->AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //理论上一定会过，玩家进入AA付卡已经前置检查
							return false;
						}
					}
					
					for (auto player : _players) //房卡消耗
					{
						if (!player) return false;
						player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_OPEN_ROOM, consume_count); 
					}
				}
				break;

				default:
				{
					return false;
				}
				break;
			}
		}
		else if (_games.size() == 0)
		{
			LOG(ERROR, "房间:{}尚未消耗房卡进行开房, 房主:{}", _stuff.room_id(), _hoster->GetID()); //记录
			return false;
		}
	}
	else if (Asset::ROOM_TYPE_CLAN_MATCH == room_type) //比赛房间
	{
		if (GetRemainCount() <= 0) return false; //本房结束
			
		if (_games.size()) return true; //已经开局，不再进行房卡检查

		int32_t consume_count = _ticket_count;
		if (consume_count <= 0) return false; //没有设置消耗数量

		for (auto player : _players) //房卡检查
		{
			if (!player) return false;

			if (!player->CheckRoomCard(consume_count)) 
			{
				player->AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //理论上一定会过，玩家进入比赛之前已经检查
				return false;
			}
		}
		
		for (auto player : _players) //房卡消耗
		{
			if (!player) return false;
			player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_START_CLAN_MATCH, consume_count); 
		}
	}
	else
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return false;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return false;

		//
		//欢乐豆检查
		//
		for (auto player : _players)
		{
			if (!player) 
			{
				DEBUG_ASSERT(false && "开局失败，未能找到玩家");
				return false;
			}
		
			auto beans_count = player->GetHuanledou();
			if (beans_count < room_limit->cost_count()) return false;
		}
		
		//
		//欢乐豆消耗
		//
		for (auto player : _players)
		{
			if (!player) 
			{
				DEBUG_ASSERT(false && "开局失败，未能找到玩家");
				return false;
			}
			
			auto real_cost = player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_ROOM_TICKET, room_limit->cost_count());
			if (room_limit->cost_count() != real_cost) return false;
		}
	}
	
	return true;
}

void Room::OnClanCreated()
{
	if (!IsClan()) return;

	Asset::ClanRoomStatusChanged message;
	message.set_status(Asset::CLAN_ROOM_STATUS_TYPE_START);
	message.mutable_room()->CopyFrom(_stuff);

	WorldInstance.BroadCast2CenterServer(message);
}

bool Room::CanDisMiss()
{
	std::lock_guard<std::mutex> lock(_player_mutex); //玩家锁

	for (auto player : _players)
	{
		if (!player) return false;

		if (!player->AgreeDisMiss()) return false; 
	}

	return true;
}

void Room::ClearDisMiss()
{
	_dismiss_time = 0;
	_dismiss_cooldown = 0;
	
	std::lock_guard<std::mutex> lock(_player_mutex); //玩家锁

	for (auto player : _players)
	{
		if (!player) continue;

		if (player->AgreeDisMiss() || player->DisAgreeDisMiss()) player->ClearDisMiss(); //必须重新投票
	}

	if (!_game) SyncRoom();
}
	
bool Room::IsExpired()
{
	if (_gmt_opened) return false; //代开房不解散

	auto curr_time = CommonTimerInstance.GetTime();
	return _expired_time < curr_time;
}

void Room::Update()
{
	auto curr_time = CommonTimerInstance.GetTime();

	if (!_is_dismiss && _dismiss_time > 0 && _dismiss_time <= curr_time) //非已确认解散，系统才自动解散
	{
		DoDisMiss(); //解散
	}
}

void Room::UpdateClanStatus()
{
	if (!IsFriend()) return; //非好友房没有状态同步

	if (!IsClan()) return; //非茶馆房间不同步
	
	Asset::ClanRoomSync message;

	Asset::RoomInformation room_information;
	room_information.set_sync_type(Asset::ROOM_SYNC_TYPE_QUERY); //外服查询房间信息
			
	//std::lock_guard<std::mutex> lock(_player_mutex); //玩家锁

	for (const auto player : _players)
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

	Asset::RoomQueryResult room_info;
	room_info.set_room_id(GetID());
	room_info.set_clan_id(GetClan());
	room_info.set_create_time(GetCreateTime());
	room_info.mutable_options()->CopyFrom(GetOptions());
	room_info.mutable_information()->CopyFrom(room_information);
	room_info.set_curr_count(GetGamesCount());

	message.set_room_status(room_info.SerializeAsString());

	DEBUG("逻辑服务器:{} 向中心服务器广播茶馆房间信息:{}", g_server_id, room_info.ShortDebugString());

	WorldInstance.BroadCast2CenterServer(message);
}

int32_t Room::GetPlayerOrder(int32_t player_id)
{
	std::lock_guard<std::mutex> lock(_player_mutex); //玩家锁

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) continue;

		if (player->GetID() == player_id) return i; //序号
	}

	return -1;
}
	
/////////////////////////////////////////////////////
//房间通用管理类
/////////////////////////////////////////////////////
RoomManager::RoomManager()
{
	_server_id = ConfigInstance.GetInt("ServerID", 1); 
}

std::shared_ptr<Room> RoomManager::Get(int64_t room_id)
{
	std::lock_guard<std::mutex> lock(_room_lock);

	auto it = _rooms.find(room_id);
	if (it == _rooms.end()) 
	{
		ERROR("获取房间:{} 数据失败：未找到房间", room_id);
		return nullptr;
	}
	
	return it->second;
}
	
bool RoomManager::CheckPassword(int64_t room_id, std::string password)
{
	auto room = Get(room_id);
	if (!room) return false;

	return true;
}

int64_t RoomManager::AllocRoom()
{
	return RedisInstance.CreateRoom();
}
	
std::shared_ptr<Room> RoomManager::CreateRoom(const Asset::Room& room)
{
	auto room_id = room.room_id();

	if (room_id <= 0) room_id = AllocRoom(); //如果没有房间号，则创建
	if (room_id <= 0) return nullptr;

	auto locate_room = std::make_shared<Room>(room);
	locate_room->SetID(room_id);
	locate_room->OnCreated();

	auto success = OnCreateRoom(locate_room); //成功创建房间
	if (!success) return nullptr; 

	return locate_room;
}

bool RoomManager::OnCreateRoom(std::shared_ptr<Room> room)
{
	if (!room) return false;

	auto room_id = room->GetID();

	std::lock_guard<std::mutex> lock(_room_lock);

	if (_rooms.find(room_id) != _rooms.end()) return false;
	_rooms.emplace(room_id, room);

	return true;
}

std::shared_ptr<Room> RoomManager::GetMatchingRoom(Asset::ROOM_TYPE room_type)
{
	std::lock_guard<std::mutex> lock(_match_mutex);

	auto& rooms = _matching_rooms[room_type];

	for (auto it = rooms.begin(); it != rooms.end(); ++it)
	{
		if (it->second->IsFull()) continue;
	
		return it->second; //尚未满房间
	}
			
	auto room_id = RoomInstance.AllocRoom();
	if (room_id <= 0) return nullptr; //创建

	Asset::Room room;
	room.set_room_id(room_id);
	room.set_room_type(room_type);

	auto room_ptr = std::make_shared<Room>(room);
	room_ptr->SetID(room_id);
	room_ptr->OnCreated();

	OnCreateRoom(room_ptr);
	rooms.emplace(room_id, room_ptr);

	return room_ptr;
}
	
void RoomManager::Update(int32_t diff)
{
	++_heart_count;
	
	std::lock_guard<std::mutex> lock(_room_lock);

	if (_heart_count % 1200 == 0) { DEBUG("服务器:{} 进行房间数量:{}", _server_id, _rooms.size()); } //1分钟
	if (_heart_count % 12000 == 0) UpdateMatching(); //10分钟

	if (_heart_count % 100 == 0) //5秒
	{
		for (auto it = _rooms.begin(); it != _rooms.end(); )
		{
			it->second->Update();

			if ((it->second->IsFriend() && it->second->IsExpired() && it->second->IsEmpty()) || it->second->HasDisMiss() || it->second->HasBeenOver())
			{
				it->second->OnRemove();
				it = _rooms.erase(it); //删除房间
			}
			else if (it->second->IsClanMatch() && it->second->IsExpired())
			{
				WARN("比赛房间:{} 超时强制解散", it->first);

				it->second->DoDisMiss(); //比赛房间超时强制删除
			}
			else
			{
				++it;
			}
		}
	}
}

void RoomManager::UpdateMatching()
{
	std::lock_guard<std::mutex> lock(_match_mutex);

	Asset::MatchStatistics stats;
	RedisInstance.GetMatching(stats);

	Asset::MatchStatistics_MatchingRoom* match_stats = nullptr;

	if (stats.server_list().size() == 0)
	{
		match_stats = stats.mutable_server_list()->Add();
		match_stats->set_server_id(_server_id);
	}
	else
	{
		for (int32_t i = 0; i < stats.server_list().size(); ++i)
		{
			if (_server_id == stats.server_list(i).server_id())	
			{
				match_stats = stats.mutable_server_list(i);
				break;
			}
		}
	}

	if (!match_stats) return;

	match_stats->mutable_room_list()->Clear();

	for (auto room : _matching_rooms)
	{
		auto room_matching = match_stats->mutable_room_list()->Add();
		room_matching->set_room_type((Asset::ROOM_TYPE)room.first);
		room_matching->set_player_count(room.second.size());
	}

	RedisInstance.SaveMatching(stats); //存盘
}
	
void RoomManager::Remove(int64_t room_id)
{
	std::lock_guard<std::mutex> lock(_room_lock);

	auto it = _rooms.find(room_id);
	if (it == _rooms.end()) return;

	it->second->OnRemove();
	_rooms.erase(it);
}


}
