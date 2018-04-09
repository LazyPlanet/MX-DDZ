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

	if (!_room) return false;

	_game_id = game_id + 1;
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
			auto pai_list = player_element->mutable_pai_list()->Add();
			pai_list->add_cards(card.card_value());
		}
	}

	return true;
}
	
void Game::OnStart()
{
	if (!_room) return;

	_room->SetBanker(_banker_player_id); //设置庄家//庄家开始叫地主

	Asset::GameInformation info; //游戏数据广播
	info.set_banker_player_id(_banker_player_id);
	BroadCast(info);
	
	/*
	Asset::RandomSaizi saizi; //开局股子广播
	saizi.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_START);

	for (int i = 0; i < 2; ++i)
	{
		int32_t result = CommonUtil::Random(1, 6);
		saizi.mutable_random_result()->Add(result);
		
		_saizi_random_result.push_back(result);
	}

	BroadCast(saizi);
	*/
}

//开局后，庄家//地主补3张底牌
//
void Game::OnStarted(std::shared_ptr<Player> dizhu_ptr)
{
	if (!dizhu_ptr) return;
	
	_real_started = true;

	auto cards = FaPai(3);
	dizhu_ptr->OnFaPai(cards);  
}

bool Game::OnGameOver(int64_t player_id)
{
	if (!_room) return false;

	SavePlayBack(); //回放

	for (auto player : _players)
	{
		if (!player) continue;

		player->OnGameOver();
	}
	
	ClearState();

	_room->OnGameOver(player_id); //胡牌

	return true;
}

void Game::SavePlayBack()
{
	if (!_room || _room->IsFriend()) return; //非好友房不存回放

	std::string key = "playback:" + std::to_string(_room_id) + "_" + std::to_string(_game_id);
	RedisInstance.Save(key, _playback);
}
	
void Game::ClearState()
{
	_last_oper.Clear();
	_cards_pool.clear();
	
	_beilv = 1; //倍率
	_rob_dizhu_count = 0; 
	_rob_dizhu_bl.clear(); //倍率表
	_rob_dizhus.clear(); //倍率表
	
	_real_started = false;
	_dizhu_player_id = 0;
	_curr_player_index = 0;
}

bool Game::CanPaiOperate(std::shared_ptr<Player> player, Asset::PaiOperation* pai_operate)
{
	if (!player) return false;

	//开局
	//
	if (!_real_started) return false;
	
	if (!_last_oper.has_pai_oper() && _dizhu_player_id == player->GetID()) return true; 
	
	auto curr_player = GetPlayerByOrder(_curr_player_index);
	if (!curr_player) return false;

	//轮到玩家打牌
	//
	if (player != curr_player) return false; 

	if (_last_oper.player_id() == player->GetID()) return true; //没人能要的起，都点了过

	//下家管上家的牌
	//
	//必须满足条件:
	//
	//1.牌型一致;
	//
	//2.最大数量的后出的牌值大于前者;
	//
	if (!pai_operate) return false;

	if (pai_operate->paixing() == Asset::PAIXING_TYPE_ZHADAN && _last_oper.pai_oper().paixing() != Asset::PAIXING_TYPE_ZHADAN)
	{
		return true; //炸弹可以管上任何非炸弹
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
	if (!player || !message || !_room) return;
	
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return; //牌已排序

	AddPlayerOperation(*pai_operate);  //回放记录
	
	if (Asset::PAI_OPER_TYPE_DAPAI == pai_operate->oper_type() && !CanPaiOperate(player, pai_operate)) 
	{
		player->AlertMessage(Asset::ERROR_PAI_UNSATISFIED); //没到玩家操作，或者管不上上家出牌，防止外挂
					
		LOG(ERROR, "玩家:{} 可操作玩家索引:{} 在房间:{}/{}局中无法出牌，牌数据:{}", 
				player->GetID(), _curr_player_index, GetID(), _room->GetID(), pai_operate->ShortDebugString());
		return; //不允许操作
	}

	if (Asset::PAI_OPER_TYPE_DAPAI == pai_operate->oper_type()) 
	{
		_last_oper.set_player_id(player->GetID());
		_last_oper.mutable_pai_oper()->CopyFrom(*pai_operate); //缓存上次牌数据
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
			
			if (player->GetCardsCountInhand() == 0) Calculate(player); //有一家出完所有牌则进入本局结算
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GIVEUP: //放弃//不要
		{
			//_curr_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //如果有玩家放弃操作，则继续下个玩家
		}
		break;
		
		default:
		{
			return; //直接退出
		}
		break;
	}
			
	_curr_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //继续下个玩家
	
	BroadCast(message); //广播玩家操作
}

void Game::PaiPushDown()
{
	Asset::PaiPushDown proto;

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) 
		{
			ERROR("player_index:{} has not found, maybe it has disconneced.", i);
			continue;
		}

		auto player_info = proto.mutable_player_list()->Add();
		player_info->set_player_id(player->GetID());
		player_info->set_position(player->GetPosition());

		const auto& cards_inhand = player->GetCardsInhand();
		for (const auto& card : cards_inhand)
		{
			auto pai = player_info->mutable_pai_list()->Add();
			pai->mutable_cards()->Add(card.card_value()); //牌值
		}
	}

	BroadCast(proto);
}
	
void Game::Calculate(std::shared_ptr<Player> player_ptr)
{
	if (!_room || !player_ptr) return;

	//1.推到牌
	//
	PaiPushDown();

	//2.结算
	//
	Asset::GameCalculate message;

	//(1)农民输或赢牌积分
	//
	int32_t top_mutiple = _room->MaxFan(); //封顶番数
	int32_t base_score = 1;

	for (auto player : _players)
	{
		if (!player) continue;
		
		auto player_id = player->GetID();

		auto record = message.mutable_record()->mutable_list()->Add(); //包括地主和农民
		record->set_player_id(player_id);
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());
		
		if (player_id == _dizhu_player_id) continue;

		auto score = base_score;
		if (player_ptr->GetID() == _dizhu_player_id) score = -base_score; //庄家先走

		score *= GetBeiLv(); //总分数

		//牌型基础分值计算
		//
		auto detail = record->mutable_details()->Add();
		//detail->set_fan_type((Asset::FAN_TYPE)fan);
		detail->set_score(-score); //负数代表输分
	
		//输牌玩家番数上限封底
		//
		if (top_mutiple > 0) score = std::min(top_mutiple, score); //封顶

		record->set_score(-score); //玩家总积分
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

	//好友房//匹配房记录消耗
	//
	auto room_type = _room->GetType();

	if (Asset::ROOM_TYPE_FRIEND == room_type)   
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
			
			auto total_score = record->score();
			auto consume_count = total_score * room_limit->base_count();
			
			auto beans_count = player->GetHuanledou();
			if (beans_count < consume_count) consume_count = beans_count; //如果玩家欢乐豆不足，则扣成0

			auto consume_real = player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GAME, consume_count); //消耗
			if (consume_count != consume_real) continue;
		}
	}
	
	BroadCast(message);
	OnGameOver(0); //结算之后才是真正结束
}
	
void Game::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	if (!_room) return;

	_room->BroadCast(message, exclude_player_id);
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
	if (!_room) return nullptr;

	int32_t order = GetPlayerOrder(player_id);
	if (order == -1) return nullptr;

	return GetPlayerByOrder((order + 1) % MAX_PLAYER_COUNT);
}

int32_t Game::GetPlayerOrder(int32_t player_id)
{
	if (!_room) return -1;

	return _room->GetPlayerOrder(player_id);
}

std::shared_ptr<Player> Game::GetPlayerByOrder(int32_t player_index)
{
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
	if (!_room) return false;
	return _room->IsBanker(player_id); 
}

bool Game::RandomDiZhu()
{
	if (!_room) return false;
		
	if (GetDiZhuPlayerCount() < MAX_PLAYER_COUNT) return false; //有玩家尚未叫分

	int32_t max_score = 0; //最大分数
	std::vector<int64_t> bankers;

	for (auto zhuang : _rob_dizhu_bl)
	{
		if (zhuang.second >= max_score) 
		{
			max_score = zhuang.second;
			bankers.push_back(zhuang.first); //最后从分数高的玩家中随机选择庄家
		}
	}

	if (bankers.size() == 0) return false; //尚未地主可选

	std::random_shuffle(bankers.begin(), bankers.end());
	SetDiZhu(bankers[0]); //随机产生一个地主

	return true;
}
	
//加倍抢地主
//
//庄家可以抢两次
void Game::OnRobDiZhu(int64_t player_id, bool is_rob) 
{ 
	DEBUG("玩家:{} 叫地主数量:{}，是否抢地主:{} 此时庄家:{}", player_id, _rob_dizhu_count, is_rob, _banker_player_id);

	++_rob_dizhu_count;

	if (_rob_dizhu_count > MAX_PLAYER_COUNT + 1) return; //不能再抢

	if (_rob_dizhus.find(player_id) != _rob_dizhus.end())
	{
		if (player_id != _banker_player_id) return; //非庄家不能多次叫地主
	}

	if (is_rob) 
	{ 
		++_rob_dizhus[player_id]; 
	}
	else 
	{ 
		_rob_dizhus[player_id] = 0; 
	}
} 
	
void Game::SetDiZhu(int64_t player_id)
{
	if (player_id <= 0) return;

	_dizhu_player_id = player_id;

	SetCurrPlayerIndexByPlayer(_dizhu_player_id); //指针转向地主        

	DEBUG("房间:{} 牌局:{} 产生地主:{}", _room_id, _game_id, _dizhu_player_id);
}
	
//是否可以开局
//
//斗地主的开局在发牌之后进行叫地主操作
//
//每次游戏可以必须进行4次是否抢地主操作
bool Game::CanStart()
{
	if (_rob_dizhus.size() < MAX_PLAYER_COUNT) return false;

	if (_rob_dizhus.size() == MAX_PLAYER_COUNT) //都已经叫(或不叫)了地主
	{
		int32_t rob_dizhu_count = 0; //抢地主玩家数量
		for (const auto player : _rob_dizhus)
		{
			if (player.second) 
			{
				SetDiZhu(player.first);

				++rob_dizhu_count;
			}
		}

		if (rob_dizhu_count == 1) //只有一个抢地主
		{
			return true; //直接开始
		}
	}


	//有人抢地主，可以再叫
	//
	for (const auto player : _rob_dizhus)
	{
		if (player.second >= 1) SetDiZhu(player.first);
		
		if (player.second == 2) 
		{
			SetDiZhu(player.first);

			return true; //直接开始
		}
	}

	return false;
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
	if (GetCardWeight(p1) > GetCardWeight(p2)) return true; //除了大小王，其他的根据牌值进行判断

	int32_t p1_value = p1.card_value();
	int32_t p2_value = p2.card_value();

	if (p1_value == 1 || p1_value == 2) p1_value += 13;
	if (p2_value == 1 || p2_value == 2) p2_value += 13;

	return p1_value > p2_value;
}

}
