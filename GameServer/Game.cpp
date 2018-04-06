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

#define NIUNIU_FAPAI 5
#define NIUNIU_XUANPAI 3

extern const Asset::CommonConst* g_const;

/////////////////////////////////////////////////////
//一场游戏
/////////////////////////////////////////////////////
void Game::Init(std::shared_ptr<Room> room)
{
	_cards.clear();
	_cards.resize(136);

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

		int32_t card_count = 5; //正常开启，普通玩家牌数量

		if (banker_index % MAX_PLAYER_COUNT == i) _curr_player_index = i; //当前操作玩家

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
		for (const auto& crds : cards_inhand)
		{
			auto pai_list = player_element->mutable_pai_list()->Add();
			pai_list->set_card_type((Asset::CARD_TYPE)crds.first); //牌类型
			for (auto card_value : crds.second) pai_list->mutable_cards()->Add(card_value); //牌值
		}
	}

	OnStarted(player_banker); //开局成功

	return true;
}
	
void Game::OnStart()
{
	if (!_room) return;

	_room->SetBanker(_banker_player_id); //设置庄家

	Asset::GameInformation info; //游戏数据广播
	info.set_banker_player_id(_banker_player_id);
	BroadCast(info);
	
	Asset::RandomSaizi saizi; //开局股子广播
	saizi.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_START);

	for (int i = 0; i < 2; ++i)
	{
		int32_t result = CommonUtil::Random(1, 6);
		saizi.mutable_random_result()->Add(result);
		
		_saizi_random_result.push_back(result);
	}

	BroadCast(saizi);
}

//
//开局后,直接进行比较大小
//
void Game::OnStarted(std::shared_ptr<Player> banker)
{
	Calculate(banker); //直接结算
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
	_baopai.Clear();
	_huipai.Clear();

	_oper_cache.Clear();

	_oper_list.clear();

	_cards_pool.clear();
	
	_liuju = false;
}

bool Game::CanPaiOperate(std::shared_ptr<Player> player)
{
	if (!player) return false;

	if (/*_oper_cache.time_out() < CommonTimerInstance.GetTime() && 超时检查*/_oper_cache.player_id() == player->GetID()) 
	{
		return true; //玩家操作：碰、杠、胡牌
	}

	auto player_index = GetPlayerOrder(player->GetID());
	if (player_index < 0) return false;

	if (_curr_player_index == player_index) 
	{
		return true; //轮到该玩家
	}
	
	LOG(ERROR, "房间:{} 局数:{} 当前缓存玩家索引:{} 当前操作玩家索引:{} 当前操作玩家:{} 服务器缓存数据:{}", 
			_room_id, _game_id, _curr_player_index, player_index, player->GetID(), _oper_cache.ShortDebugString());
	return false;
}
void Game::OnPlayerReEnter(std::shared_ptr<Player> player)
{
	if (!player) return;
	
}

void Game::OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	if (!player || !message || !_room) return;
	
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return; 

	AddPlayerOperation(*pai_operate);  //回放记录
	
	auto player_index = GetPlayerOrder(player->GetID());

	auto curr_player_id = player->GetID();
	auto pai_operate_string = pai_operate->ShortDebugString();
	auto oper_limit_string = _oper_cache.ShortDebugString();

	DEBUG("房间:{} 当前牌局:{} 当前操作玩家ID:{} 位置索引:{} 进行的操作:{} 服务器记录的当前可操作玩家索引:{} 服务器缓存玩家操作:{}", _room_id, _game_id, curr_player_id, player_index, pai_operate_string, _curr_player_index, oper_limit_string);

	if (!CanPaiOperate(player)) 
	{
		player->AlertMessage(Asset::ERROR_GAME_NO_PERMISSION); //没有权限，没到玩家操作，防止外挂

		ERROR("尚未轮到该玩家:{} 操作:{}", curr_player_id, pai_operate_string);
		return; //不允许操作
	}
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

		const auto& cards = player->GetCardsInhand();

		for (auto it = cards.begin(); it != cards.end(); ++it)
		{
			auto pai = player_info->mutable_pai_list()->Add();

			pai->set_card_type((Asset::CARD_TYPE)it->first); //牌类型

			for (auto card_value : it->second)
			{
				pai->mutable_cards()->Add(card_value); //牌值
			}
		}
	}

	BroadCast(proto);
}
	
void Game::Calculate(std::shared_ptr<Player> banker)
{
	if (!_room || !banker) return;
	
	//
	//1.推到牌
	//
	PaiPushDown();

	//
	//2.结算
	//
	
	Asset::GameCalculate message;

	//
	//(1)各个玩家输牌积分
	//
	//std::unordered_map<int64_t, int32_t> player_score; //非庄家输赢分数
	auto banker_niu = banker->GetNiu();
	int32_t top_mutiple = _room->MaxFan(); //封顶番数
	int32_t base_score = 1;

	for (auto player : _players)
	{
		if (!player) continue;
		
		auto record = message.mutable_record()->mutable_list()->Add();
		record->set_player_id(player->GetID());
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());
		
		if (player == banker) continue;

		auto niu = player->GetNiu();
		auto fan = _room->GetMultiple(niu);
		auto score = base_score * fan;

		if (banker_niu != 0 && niu > banker_niu)
		{
			DEBUG("玩家:{} 牛:{} 大于庄家:{} 牛:{} 分数:{}", player->GetID(), niu, banker->GetID(), banker_niu, score);
		}
		else if (niu != 0 && niu < banker_niu)
		{
			score = -_room->GetMultiple(banker_niu);
			DEBUG("玩家:{} 牛:{} 小于庄家:{} 牛:{} 分数:{}", player->GetID(), niu, banker->GetID(), score);
		}
		else //都牛牛或者都非牛牛
		{
			bool banker_win = GameInstance.ComparePai(banker->GetMaxPai(), player->GetMaxPai());
			if (banker_win) 
			{
				score = -score;
				DEBUG("玩家:{} 牛:{} 大于庄家:{} 牛:{} 分数:{}", player->GetID(), niu, banker->GetID(), banker_niu, score);
			}
			else
			{
				DEBUG("玩家:{} 牛:{} 小于庄家:{} 牛:{} 分数:{}", player->GetID(), niu, banker->GetID(), banker_niu, score);
			}
		}
			
		
		//player_score.emplace(player->GetID(), score);

		//
		//牌型基础分值计算
		//
		auto detail = record->mutable_details()->Add();
		detail->set_fan_type((Asset::FAN_TYPE)fan);
		detail->set_score(-score);
	
		//
		//输牌玩家番数上限封底
		//
		if (top_mutiple > 0) score = std::min(top_mutiple, score); //封顶

		record->set_score(-score); //玩家所输积分
	}

	//
	//(2)庄家积分
	//

	auto get_record = [&](int64_t player_id)->google::protobuf::internal::RepeatedPtrIterator<Adoter::Asset::GameRecord_GameElement> { 
		auto it = std::find_if(message.mutable_record()->mutable_list()->begin(), message.mutable_record()->mutable_list()->end(), 
				[player_id](const Asset::GameRecord_GameElement& ele){
					return player_id == ele.player_id();
			});
		return it;
	};
	
	auto record = get_record(banker->GetID()); 
	if (record == message.mutable_record()->mutable_list()->end()) return;
	//
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
	
	auto room_id = _room->GetID();
	auto curr_count = _room->GetGamesCount();
	auto open_rands = _room->GetOpenRands();
	auto message_string = message.ShortDebugString();

	//LOG(INFO, "房间:{}第:{}/{}局结束，胡牌玩家:{} 点炮玩家:{}, 胡牌结算:{}", room_id, curr_count, open_rands, hupai_player_id, dianpao_player_id, message_string);
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

int32_t Game::GetMultiple(int32_t fan_type)
{
	if (!_room) return 0;
	
	return _room->GetMultiple(fan_type);
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

		static std::set<int32_t> _valid_cards = { Asset::CARD_TYPE_HONGTAO, Asset::CARD_TYPE_FANGPIAN, Asset::CARD_TYPE_HEITAO, Asset::CARD_TYPE_MEIHUA }; //扑克

		auto it = _valid_cards.find(asset_card->card_type());
		if (it == _valid_cards.end()) continue;
		
		for (int k = 0; k < asset_card->group_count(); ++k) //4组，麻将每张牌有4张
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

	//if (_cards.size() != CARDS_COUNT) return false;
	
	DoCombine(); //生成排列组合序列

	return true;
}

void GameManager::OnCreateGame(std::shared_ptr<Game> game)
{
	_games.push_back(game);
}

void GameManager::DoCombine()
{
	std::vector<bool> v(NIUNIU_FAPAI);
	std::fill(v.begin(), v.begin() + NIUNIU_XUANPAI, true);

	do 
	{
		std::vector<int32_t> combine;

		for (int i = 0; i < NIUNIU_FAPAI; ++i) 
		{
			if (v[i]) 
			{
				DEBUG("生成牛牛组合序列，索引:{}", i);
				combine.push_back(i);
			}
		}

		_combines.push_back(combine);

	} while (std::prev_permutation(v.begin(), v.end()));
}

int32_t GameManager::GetCardWeight(int32_t card_type)
{
	auto it = _card_tye_weight.find(card_type);
	if (it == _card_tye_weight.end()) return 0;

	return it->second;
}
	
bool GameManager::ComparePai(const Asset::PaiElement& p1, const Asset::PaiElement& p2)
{
	if (p1.card_value() > p2.card_value()) return true;
	if (p1.card_value() < p2.card_value()) return false;

	return GetCardWeight(p1.card_type()) > GetCardWeight(p2.card_type());
}

}
