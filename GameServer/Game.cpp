#include <vector>
#include <algorithm>

#include <cpp_redis/cpp_redis>

#include "Game.h"
#include "Timer.h"
#include "Asset.h"
#include "MXLog.h"
#include "CommonUtil.h"
#include "RedisManager.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

/////////////////////////////////////////////////////
//一场游戏
/////////////////////////////////////////////////////
void Game::Init(std::shared_ptr<Room> room)
{
	_cards.clear();
	_cards.resize(CARDS_COUNT);

	std::iota(_cards.begin(), _cards.end(), 1);
	std::vector<int32_t> cards(_cards.begin(), _cards.end());

	std::random_shuffle(cards.begin(), cards.end()); //洗牌

	_cards = std::list<int32_t>(cards.begin(), cards.end());

	_room = room;
}

bool Game::Start(std::vector<std::shared_ptr<Player>> players, int64_t room_id, int32_t game_id)
{
	if (MAX_PLAYER_COUNT != players.size()) return false; //做下检查，是否满足开局条件

	auto _room = GetRoom();
	if (!_room) return false;

	_game_id = game_id; // + 1;
	_room_id = room_id;

	//
	//房间(Room)其实是游戏(Game)的管理类
	//
	//复制房间数据到游戏中
	//
	int32_t player_index = 0;

	for (auto player : players)
	{
		_players[player_index++] = player; 

		player->SetRoom(_room);
		player->SetPosition(Asset::POSITION_TYPE(player_index));
		player->OnGameStart();
	}

	//当前庄家
	auto banker_index = _room->GetBankerIndex();
		
	auto player_banker = GetPlayerByOrder(banker_index);
	if (!player_banker) return false;

	_banker_player_id  = player_banker->GetID();
	
	OnStart(); //同步本次游戏开局数据：此时玩家没有进入游戏

	//
	//设置游戏数据
	//
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return false;

		player->SetGame(shared_from_this());

		int32_t card_count = 17; //正常开启，普通玩家牌数量//斗地主起手每人17张牌

		//if (banker_index % MAX_PLAYER_COUNT == i) _curr_player_index = i; //当前操作玩家

		//玩家发牌
		auto cards = FaPai(card_count);
		player->OnFaPai(cards);  

		//回放缓存：初始牌数据
		_playback.mutable_options()->CopyFrom(_room->GetOptions()); //房间玩法

		auto player_element = _playback.mutable_player_list()->Add();
		player_element->set_player_id(player->GetID());
		player_element->set_position(player->GetPosition());
		player_element->mutable_common_prop()->CopyFrom(player->CommonProp());
		player_element->mutable_wechat()->CopyFrom(player->GetWechat());

		const auto& cards_inhand = player->GetCardsInhand();
		for (const auto& card : cards_inhand)
		{
			auto pai = player_element->mutable_pai_list()->Add();
			pai->CopyFrom(card);
		}
	}

	return true;
}
	
void Game::OnStart()
{
	auto _room = GetRoom();
	if (!_room) return;

	_playback.set_create_time(_room->GetCreateTime()); //回放时间

	_room->SetBanker(_banker_player_id); //设置庄家//庄家开始叫地主

	Asset::GameInformation info; //游戏数据广播
	info.set_banker_player_id(_banker_player_id);

	BroadCast(info);
}

//开局后，庄家//地主补3张底牌
//
void Game::OnStarted()
{
	auto _room = GetRoom();
	if (!_room) return;
	
	//if (!_room->IsJiaoFenMode()) _real_started = true; //叫分模式可以加倍，因此不能立即开始
	
	auto dizhu_ptr = GetPlayer(_dizhu_player_id);
	if (!dizhu_ptr) return;
	
	_real_started = true;

	auto cards = FaPai(3);
	dizhu_ptr->OnFaPai(cards);  

	for (auto card_index : cards)
	{
		const auto& card = GameInstance.GetCard(card_index);
		_dipai.push_back(card); //底牌缓存

		auto dipai = _playback.mutable_dipai()->Add();
		dipai->CopyFrom(card); //回放：底牌
	}

	_playback.set_dizhu_position(dizhu_ptr->GetPosition()); //回放：地主

	DEBUG("房间:{} 开局:{} 地主:{}", _room->GetID(), _game_id, _dizhu_player_id);
}

bool Game::OnGameOver(int64_t player_id)
{
	auto _room = GetRoom();
	if (!_room) return false;

	const auto& list = _room->GetRobDiZhuList();
	for (const auto& oper_element : list)
	{
		auto rob_element = _playback.mutable_rob_list()->Add();
		rob_element->CopyFrom(oper_element); //叫地主回放
	}

	SavePlayBack(); //回放

	for (auto player : _players)
	{
		if (!player) continue;

		player->OnGameOver();
	}
	
	ClearState();

	_room->OnGameOver(player_id); 

	return true;
}

void Game::SavePlayBack()
{
	auto _room = GetRoom();
	if (!_room || _room->IsMatch()) return; //匹配房不存回放

	std::string key = "playback:" + std::to_string(_room_id) + "_" + std::to_string(_game_id);
	RedisInstance.Save(key, _playback);
}
	
void Game::ClearState()
{
	_last_oper.Clear();
	_cards_pool.clear();
	_dipai.clear();
	
	_base_score = _beilv = 1; //底分//倍率
	_rob_dizhu_count = 0; 
	_rob_dizhu_bl.clear(); //倍率表
	_rob_dizhus.clear(); //倍率表
	
	_real_started = false;
	_dizhu_player_id = 0;
	_curr_player_index = -1;
}

bool Game::CanPaiOperate(std::shared_ptr<Player> player, Asset::PaiOperation* pai_operate)
{
	if (!player) return false;

	//
	//是否真的开局
	//
	//斗地主需要发3张牌
	//
	if (!_real_started) return false;
	
	if (!_last_oper.has_pai_oper())
	{
		if (_dizhu_player_id == player->GetID()) { return true; }
		else { return false; }
	}
	
	auto curr_player = GetPlayerByOrder(_curr_player_index);
	if (!curr_player) return false;

	//轮到玩家打牌
	if (player != curr_player) return false; 
	
	//正常牌序
	if (!pai_operate) return false; //必须检查
	
	if (Asset::PAI_OPER_TYPE_GIVEUP == pai_operate->oper_type()) return true; //放弃直接下一个

	if (_last_oper.player_id() == player->GetID()) return true; //没人能要的起，都点了过

	//下家管上家的牌
	//
	//必须满足条件:
	//
	//1.牌型一致;
	//
	//2.最大数量的后出的牌值大于前者;
	//
	if (pai_operate->paixing() == Asset::PAIXING_TYPE_ZHADAN && _last_oper.pai_oper().paixing() != Asset::PAIXING_TYPE_ZHADAN)
	{
		return true; //炸弹可以管上任何非炸弹
	}
	else if (pai_operate->paixing() == Asset::PAIXING_TYPE_ZHADAN && pai_operate->pais().size() == 2)
	{
		return true; //王炸
	}
	else
	{
		if (pai_operate->pais().size() != _last_oper.pai_oper().pais().size()) return false; //出牌数量不一致
		if (_last_oper.pai_oper().paixing() != pai_operate->paixing()) return false; //牌型不一致
		if (!GameInstance.ComparePai(pai_operate->pai(), _last_oper.pai_oper().pai())) return false; //比较大小
	}

	return true;
}

void Game::OnPlayerReEnter(std::shared_ptr<Player> player)
{
	if (!player) return;
}

void Game::OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	auto _room = GetRoom();
	if (!player || !message || !_room) return;
	
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return; //牌已排序

	if (/*Asset::PAI_OPER_TYPE_DAPAI == pai_operate->oper_type() && */!CanPaiOperate(player, pai_operate)) 
	{
		player->AlertMessage(Asset::ERROR_PAI_CANNOT_GUANGSHANG); //没到玩家操作，或者管不上上家出牌，防止外挂
					
		LOG(ERROR, "玩家:{} 可操作玩家索引:{} 在房间:{}/{}局中无法出牌，牌数据:{} 缓存牌数据:{}", 
				player->GetID(), _curr_player_index, GetID(), _room->GetID(), pai_operate->ShortDebugString(), _last_oper.pai_oper().ShortDebugString());
		return; //不允许操作
	}
	
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		{
			for (const auto& pai : pai_operate->pais()) 
			{
				if (!player->RemovePai(pai)) //删除玩家牌
				{
					player->PrintPai(); //打印出牌数据

					LOG(ERROR, "玩家:{} 在房间:{}/{}局中无法删除牌，牌数据:{}", player->GetID(), GetID(), _room->GetID(), pai_operate->ShortDebugString());
					return;
				}
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GIVEUP: //放弃//不要
		{
			/*
			auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //如果有玩家托管

			auto next_player = GetPlayerByOrder(next_player_index);
			if (!next_player) return;
			
			if (next_player->HasTuoGuan())
			{
				WARN("玩家:{} 进行托管", next_player->GetID());

				next_player->OnOperateTimeOut(); //托管
			}
			*/
		}
		break;
		
		default:
		{
			return; //直接退出
		}
		break;
	}
	
	if (Asset::PAI_OPER_TYPE_DAPAI == pai_operate->oper_type()) 
	{
		_last_oper.set_player_id(player->GetID());
		_last_oper.mutable_pai_oper()->CopyFrom(*pai_operate); //缓存上次牌数据
	}
	
	if (pai_operate->paixing() == Asset::PAIXING_TYPE_ZHADAN) 
	{
		IncreaseBeiLv();
		_room->AddZhaDan(player->GetID()); //整局统计
	}
			
	_curr_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //继续下个玩家
	
	AddPlayerOperation(*pai_operate);  //回放记录
	
	BroadCast(message); //玩家操作
	
	//有一家出完所有牌则进入本局结算
	if (pai_operate->oper_type() == Asset::PAI_OPER_TYPE_DAPAI && player->GetCardsCountInhand() == 0) Calculate(player); 
}

void Game::PaiPushDown()
{
	Asset::PaiPushDown proto;

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) continue;

		auto player_info = proto.mutable_player_list()->Add();
		player_info->set_player_id(player->GetID());
		player_info->set_position(player->GetPosition());

		const auto& cards_inhand = player->GetCardsInhand();
		for (const auto& card : cards_inhand)
		{
			auto pai = player_info->mutable_cards()->Add();
			pai->CopyFrom(card);
		}
	}

	BroadCast(proto);
}
	
void Game::Calculate(std::shared_ptr<Player> player_ptr)
{
	auto _room = GetRoom();
	if (!_room || !player_ptr) return;

	//1.推到牌
	//
	PaiPushDown();

	//2.结算
	//
	Asset::GameCalculate message;

	//(1)农民输或赢牌积分
	//
	int32_t chuntian_count = 0;
	int32_t top_mutiple = _room->MaxFan(); //封顶番数

	for (auto player : _players)
	{
		if (!player) continue;
		
		if (player->GetID() == _dizhu_player_id && player->GetChuPaiCount() == 1) 
		{
			message.set_is_chuntian(true); //农民反春天，地主只出了一次牌

			break; //地主和农民只能有一方是春天或反春天
		}
		
		if (player->GetChuPaiCount() == 0) ++chuntian_count; //是否春天
	}

	if (chuntian_count == MAX_PLAYER_COUNT - 1) message.set_is_chuntian(true); //地主春天，2个农民都1张牌没出

	if (message.is_chuntian()) IncreaseBeiLv(); //春天或反春天翻倍

	for (auto player : _players)
	{
		if (!player) continue;
		
		auto player_id = player->GetID();

		auto record = message.mutable_record()->mutable_list()->Add(); //包括地主和农民
		record->set_player_id(player_id);
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());
		
		if (player_id == _dizhu_player_id) continue;

		auto score = _base_score * GetBeiLv(); //总分数
		score *= player->GetBeiLv(); //农民单独倍率，是否加倍

		//输牌玩家番数上限封底
		//
		if (top_mutiple > 0) score = std::min(top_mutiple, score); //封顶

		if (player_ptr->GetID() == _dizhu_player_id) score = -score; //地主先走

		record->set_score(score); //农民总积分

		if (record->score() > 0) _room->AddWinner(player_id); //整局统计
	}

	//(2)地主积分
	//
	auto get_record = [&](int64_t player_id)->google::protobuf::internal::RepeatedPtrIterator<Adoter::Asset::GameRecord_GameElement> { 
		auto it = std::find_if(message.mutable_record()->mutable_list()->begin(), message.mutable_record()->mutable_list()->end(), 
				[player_id](const Asset::GameRecord_GameElement& ele){
					return player_id == ele.player_id();
			});
		return it;
	};
	
	auto record = get_record(_dizhu_player_id); 
	if (record == message.mutable_record()->mutable_list()->end()) return;

	int32_t total_score = 0;
	for (const auto& nongmin_record : message.record().list())
	{
		if (_dizhu_player_id == nongmin_record.player_id()) continue;

		total_score += nongmin_record.score(); //分数
	}

	record->set_score(-total_score); //地主总积分

	if (record->score() > 0) _room->AddWinner(_dizhu_player_id); //整局统计

	//好友房//匹配房记录消耗
	//
	auto room_type = _room->GetType();

	if (Asset::ROOM_TYPE_FRIEND == room_type || Asset::ROOM_TYPE_CLAN_MATCH == room_type)   
	{
		_room->AddGameRecord(message.record()); //本局记录
	}
	else
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);
		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return;

		for (auto player : _players)
		{
			if (!player) continue;

			auto record = get_record(player->GetID()); 
			if (record == message.mutable_record()->mutable_list()->end()) continue;
			
			auto changed_count = record->score() * room_limit->base_count(); //欢乐豆变化数量
			if (changed_count < 0)
			{
				player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GAME, -changed_count); //消耗
			}
			else
			{
				player->GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GAME, changed_count); //获得
			}

			//auto consume_real = player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GAME, consume_count); //消耗
			//if (consume_count != consume_real) continue;
		}
	}
	
	BroadCast(message);

	//真正结束
	//
	//统计数据
	OnGameOver(player_ptr->GetID()); 
	
	/*
	auto room_id = _room->GetID();
	auto curr_count = _room->GetGamesCount();
	auto open_rands = _room->GetOpenRands();
	auto message_string = message.ShortDebugString();

	LOG(INFO, "房间:{} 第:{}/{}局结束，先出玩家:{} 本局结算:{}", room_id, curr_count, open_rands, player_ptr->GetID(), message_string);
	*/
}
	
void Game::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	auto _room = GetRoom();
	if (!_room) return;

	_room->BroadCast(message, exclude_player_id);
}
	
void Game::IncreaseBeiLv(int32_t beilv) 
{ 
	auto _room = GetRoom();
	if (!_room) return;

	if (beilv <= 0) beilv = 2; 
	
	_beilv *= beilv; 

	DEBUG("房间:{} 局数:{} 倍率:{} 当前局数倍率:{}", _room->GetID(), _game_id, beilv, _beilv);
} 

void Game::Add2CardsPool(Asset::PaiElement pai) 
{ 
	_cards_pool.push_back(pai); 
}

void Game::Add2CardsPool(Asset::CARD_TYPE card_type, int32_t card_value) 
{
	Asset::PaiElement pai;
	pai.set_card_type(card_type);
	pai.set_card_value(card_value);

	Add2CardsPool(pai);
}

void Game::BroadCast(pb::Message& message, int64_t exclude_player_id)
{
	auto _room = GetRoom();
	if (!_room) return;

	_room->BroadCast(message, exclude_player_id);
}

/*
std::vector<int32_t> Game::TailPai(size_t card_count)
{
	std::vector<int32_t> tail_cards;
	std::vector<int32_t> cards(_cards.begin(), _cards.end());

	for (size_t i = 0; i < cards.size(); ++i)
	{
		if (_random_result_list.find(i + 1) == _random_result_list.end())  //不是宝牌缓存索引
		{
			_random_result_list.insert(i + 1);

			tail_cards.push_back(cards[cards.size() - 1 - i]);
			if (tail_cards.size() >= card_count) break;
		}
	}
	
	return tail_cards;
}
*/
	
std::vector<int32_t> Game::FaPai(size_t card_count)
{
	std::vector<int32_t> cards;

	if (_cards.size() < card_count) return cards;

	for (size_t i = 0; i < card_count; ++i)
	{
		int32_t value = _cards.front();	
		cards.push_back(value);
		_cards.pop_front();
	}
	
	return cards;
}
	
std::shared_ptr<Player> Game::GetNextPlayer(int64_t player_id)
{
	auto _room = GetRoom();
	if (!_room) return nullptr;

	int32_t order = GetPlayerOrder(player_id);
	if (order == -1) return nullptr;

	return GetPlayerByOrder((order + 1) % MAX_PLAYER_COUNT);
}

int32_t Game::GetPlayerOrder(int32_t player_id)
{
	auto _room = GetRoom();
	if (!_room) return -1;

	return _room->GetPlayerOrder(player_id);
}

std::shared_ptr<Player> Game::GetPlayerByOrder(int32_t player_index)
{
	auto _room = GetRoom();
	if (!_room) return nullptr;

	if (player_index < 0 || player_index >= MAX_PLAYER_COUNT) return nullptr;

	return _players[player_index];
}

std::shared_ptr<Player> Game::GetPlayer(int64_t player_id)
{
	for (auto player : _players)
	{
		if (!player) continue;
		if (player->GetID() == player_id) return player;
	}
	
	return nullptr;
}

bool Game::IsBanker(int64_t player_id) 
{ 
	auto _room = GetRoom();
	if (!_room) return false;
	return _room->IsBanker(player_id); 
}

/*
bool Game::RandomDiZhu()
{
	if (!_room) return false;
		
	if (_rob_dizhu_bl.size() == 0) return false;
	
	if (_rob_dizhu_count < MAX_PLAYER_COUNT) return false; 

	if (_rob_dizhu_bl.size() == 1)
	{
		auto it = _rob_dizhu_bl.begin();
		if (it == _rob_dizhu_bl.end()) return false;

		if (it->second == 3)
		{
			_base_score = 3;

			SetDiZhu(it->first);

			return true;
		}
	}
	
	int64_t dizhu = 0, max_score = 0; //最大分数
	
	for (auto player : _rob_dizhu_bl)
	{
		if (player.second > max_score) 
		{
			dizhu = player.first;
			max_score = player.second;
		}
	}

	_base_score = max_score;

	SetDiZhu(dizhu); //设置地主

	return true;
}
*/

void Game::OnRobDiZhu(int64_t player_id, int32_t beilv) 
{ 
	auto _room = GetRoom();
	if (!_room) return;

	DEBUG("玩家:{} 在房间:{} 局数:{} 叫地主数量:{} 叫分:{} 此时庄家:{}", player_id, _room->GetID(), _game_id, _rob_dizhu_count, beilv, _banker_player_id);

	++_rob_dizhu_count;

	for (auto rob_info : _rob_dizhu_bl)
	{
		if (beilv <= rob_info.second) return; //分数必须比上一个抢地主玩家分数高，否则不能抢地主
	}

	if (_rob_dizhu_bl.find(player_id) != _rob_dizhu_bl.end()) return; //防止多次抢地主操作

	_rob_dizhu_bl[player_id] = beilv; //缓存分数
	
	_rob_dizhus.push_back(player_id); //缓存叫地主的玩家
} 
	
//加倍抢地主
//
//庄家可以抢两次
//
void Game::OnRobDiZhu(int64_t player_id, bool is_rob) 
{ 
	auto _room = GetRoom();
	if (!_room) return;

	DEBUG("玩家:{} 在房间:{} 局数:{} 叫地主数量:{}，是否抢地主:{} 此时庄家:{}", player_id, _room->GetID(), _game_id, _rob_dizhu_count, is_rob, _banker_player_id);

	++_rob_dizhu_count;

	if (_rob_dizhu_count > MAX_PLAYER_COUNT + 1) return; //最多4次，不能再抢

	if (is_rob) 
	{
		if (_rob_dizhus.size()) IncreaseBeiLv(); //抢地主加倍//叫地主不加倍

		_rob_dizhus.push_back(player_id);
	}
} 
	
void Game::SetDiZhu(int64_t player_id)
{
	auto _room = GetRoom();
	if (!_room) return;

	if (player_id <= 0) return;

	_dizhu_player_id = player_id; //地主

	_base_score = _rob_dizhu_bl[_dizhu_player_id]; //底分

	if (_base_score <= 0) _base_score = 1; 

	SetCurrPlayerIndexByPlayer(_dizhu_player_id); //指针转向地主        

	_room->AddDiZhu(player_id);

	DEBUG("房间:{} 牌局:{} 产生地主:{}", _room_id, _game_id, _dizhu_player_id);
}
	
//是否可以开局
//
//斗地主的开局在发牌之后进行叫地主操作
//
//每次游戏可以必须进行4次是否抢地主操作
//
bool Game::CanStart()
{
	auto _room = GetRoom();
	if (!_room) return false;

	//
	//叫分模式，先叫3分直接开始
	//
	for (const auto& dizhu : _rob_dizhu_bl)
	{
		if (dizhu.second == 3)
		{
			SetDiZhu(dizhu.first);
			
			//if (_rob_dizhus.size() == 1) _real_started = true; //不可以加倍，直接开始

			return true;
		}
	}

	//
	//通用检查//叫分//叫庄
	//
	if (_rob_dizhu_count < MAX_PLAYER_COUNT) return false; 

	if (_rob_dizhu_count == MAX_PLAYER_COUNT) //都已经叫(或不叫)了地主
	{
		if (_rob_dizhus.size() == 0) //都不抢地主
		{
			return false;
		}
		else if (_rob_dizhus.size() == 1) //只有一个抢地主
		{
			//_real_started = true; //不可以加倍，直接开始

			SetDiZhu(_rob_dizhus[0]);

			return true;
		}

		if (Asset::ZHUANG_TYPE_QIANGDIZHU == _room->GetZhuangType()) return false; //多人抢地主//给叫庄模式的最后一次机会
	}

	if (_rob_dizhus.size() == 0) return false;

	SetDiZhu(_rob_dizhus[_rob_dizhus.size() - 1]); //最后一个抢地主的是地主

	return true;
}

//
//游戏通用管理类
//
bool GameManager::Load()
{
	std::unordered_set<pb::Message*> messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_MJ_CARD);

	for (auto message : messages)
	{
		Asset::MJCard* asset_card = dynamic_cast<Asset::MJCard*>(message); 
		if (!asset_card) return false;

		//扑克
		//
		static std::set<int32_t> _valid_cards = { Asset::CARD_TYPE_HONGTAO, Asset::CARD_TYPE_FANGPIAN, Asset::CARD_TYPE_HEITAO, Asset::CARD_TYPE_MEIHUA,
			Asset::CARD_TYPE_KINGS}; 

		auto it = _valid_cards.find(asset_card->card_type());
		if (it == _valid_cards.end()) continue;
		
		for (int k = 0; k < asset_card->group_count(); ++k) 
		{
			int32_t cards_count = std::min(asset_card->cards_count(), asset_card->cards_size());

			for (int i = 0; i < cards_count; ++i)
			{
				Asset::PaiElement card;
				card.set_card_type(asset_card->card_type());
				card.set_card_value(asset_card->cards(i).value());

				if (k == 0) _pais.push_back(card); //每张牌存一个
				_cards.emplace(_cards.size() + 1, card); //从1开始的索引
			}
		}
	}

	if (_cards.size() != CARDS_COUNT) 
	{
		ERROR("加载牌数据失败，加载牌数量:{} 期望牌数量:{}", _cards.size(), CARDS_COUNT);
		return false;
	}
	
	return true;
}

void GameManager::OnCreateGame(std::shared_ptr<Game> game)
{
	_games.push_back(game);
}

int32_t GameManager::GetCardWeight(const Asset::PaiElement& card)
{
	auto it = _card_tye_weight.find(card.card_type());
	if (it == _card_tye_weight.end()) return 0;

	return it->second;
}
	
bool GameManager::ComparePai(const Asset::PaiElement& p1, const Asset::PaiElement& p2)
{
	int32_t p1_value = p1.card_value();
	int32_t p2_value = p2.card_value();

	if (p1.card_type() == Asset::CARD_TYPE_KINGS)
	{
		p1_value += 15; 
	}
	else
	{
	    if (p1_value == 1 || p1_value == 2) p1_value += 13;
	}

	if (p2.card_type() == Asset::CARD_TYPE_KINGS)
	{
	    p2_value += 15; 
	}
	else
	{
	    if (p2_value == 1 || p2_value == 2) p2_value += 13;
	}

	return p1_value > p2_value;
}

}
