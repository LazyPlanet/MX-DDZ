#pragma once

#include <list>
#include <memory>
#include <vector>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "P_Header.h"
#include "Player.h"
#include "Room.h"

namespace Adoter
{

#define CARDS_COUNT 136
	
extern const int32_t MAX_PLAYER_COUNT; //玩家总数：有些地方不是4人麻将

/////////////////////////////////////////////////////
//一场游戏: 牛牛
/////////////////////////////////////////////////////
class Game : public std::enable_shared_from_this<Game>
{
	std::shared_ptr<Room> _room = nullptr; //游戏在哪个房间开启

private:
	
	std::list<int32_t> _cards; //随机牌,每次开局更新,索引为GameManager牌中索引

	int32_t _curr_player_index = 0; //当前在操作的玩家索引
	int64_t _banker_player_id = 0; //庄家
	std::vector<int64_t> _ting_players; //听牌玩家
	int64_t _room_id = 0;
	int32_t _game_id = 0;

	Asset::PaiElement _baopai; //宝牌
	int32_t _random_result = 0; //宝牌随机：1~6
	std::unordered_set<int32_t> _random_result_list; //宝牌历史随机结果
	std::vector<int32_t> _saizi_random_result; //开局股子缓存

	Asset::PaiElement _huipai; //会儿牌
	
	Asset::PaiOperationCache _oper_cache; //牌操作缓存
	std::vector<Asset::PaiOperationCache> _oper_list; //可操作列表

	std::shared_ptr<Player> _players[MAX_PLAYER_COUNT]; //玩家数据：按照进房间的顺序，0->1->2->3...主要用于控制发牌和出牌顺序

	bool _liuju = false; //是否流局
	std::vector<Asset::PaiElement> _cards_pool; //牌池，玩家已经打的牌缓存

	Asset::PlayBack _playback; //回放数据
public:
	virtual void Init(std::shared_ptr<Room> room); //初始化
	virtual bool Start(std::vector<std::shared_ptr<Player>> players, int64_t room_id = 0, int32_t game_id = 0); //开始游戏
	virtual void OnStart(); //开始游戏回调
	virtual void OnStarted(std::shared_ptr<Player> banker); //开始游戏,玩家已经进入游戏

	virtual bool OnGameOver(int64_t player_id); //游戏结束
	void ClearState(); //清理状态

	virtual std::vector<int32_t> FaPai(size_t card_count); //发牌
	virtual std::vector<int32_t> TailPai(size_t card_count); //后楼发牌
	void FaPaiAndCommonCheck();

	virtual void Add2CardsPool(Asset::PaiElement pai);
	virtual void Add2CardsPool(Asset::CARD_TYPE card_type, int32_t card_value);
	
	void OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message); //玩家牌操作响应
	bool CanPaiOperate(std::shared_ptr<Player> player); //检查玩家是否具有操作权限
	void OnPlayerReEnter(std::shared_ptr<Player> player);

	void OnOperateTimeOut();
	void ClearOperation();
	void SetPaiOperation(const Asset::PaiOperationCache& oper) { _oper_cache = oper; } //牌操作限制
	const Asset::PaiOperationCache& GetOperCache() { return _oper_cache; }
	//void SetPaiOperation(int64_t player_id, int64_t source_player_id, Asset::PaiElement pai, Asset::PAI_OPER_TYPE oper_type = Asset::PAI_OPER_TYPE_HUPAI);
	//void SetZiMoCache(std::shared_ptr<Player> player, Asset::PaiElement pai);

	//bool SendCheckRtn(); //发送当前可以操作的牌数据
	//bool CheckPai(const Asset::PaiElement& pai, int64_t source_player_id); //检查玩家可以进行的牌操作,包括胡//杠//碰//吃
	//bool CheckQiangGang(const Asset::PaiElement& pai, int64_t source_player_id); //抢杠胡
	//bool IsQiangGang(int64_t player_id); //是否抢杠操作
	
	std::shared_ptr<Player> GetNextPlayer(int64_t player_id); //获取下家
	std::shared_ptr<Player> GetPlayer(int64_t player_id); //获取玩家
	std::shared_ptr<Player> GetPlayerByOrder(int32_t player_index);
	int32_t GetPlayerOrder(int32_t player_id); //获取玩家的顺序
	void SetRoom(std::shared_ptr<Room> room) {	_room = room; } //设置房间
	int64_t GetID() { return _game_id; } //局数
	bool IsBanker(int64_t player_id); //是否庄家

	void BroadCast(pb::Message* message, int64_t exclude_player_id = 0);
	void BroadCast(pb::Message& message, int64_t exclude_player_id = 0);

	int32_t GetMultiple(int32_t fan_type);
	//void CalculateGangScore(Asset::GameCalculate& game_calculate);
	//void Calculate(int64_t hupai_player_id/*胡牌玩家*/, int64_t dianpao_player_id/*点炮玩家*/, std::unordered_set<int32_t>& fan_list/*基础分*/);
	void Calculate(std::shared_ptr<Player> banker);
	void PaiPushDown();

	void AddTingPlayer(int64_t player_id) {	_ting_players.push_back(player_id);	} //增加听牌玩家
	void OnTingPai(std::shared_ptr<Player> player); //玩家听牌

	/*
	Asset::PaiElement GetBaoPai(int32_t tail_index); //随机宝牌
	void SetBaoPai(const Asset::PaiElement& pai) { _baopai = pai; } //设置宝牌
	const Asset::PaiElement& GetBaoPai() { return _baopai; } //获取当前宝牌
	bool IsBaopai(const Asset::PaiElement& pai) {  //是否是宝牌
		if (_baopai.card_type() == 0 || _baopai.card_value() == 0) return false;
		return pai.card_type() == _baopai.card_type() && pai.card_value() == _baopai.card_value(); 
	} 
	*/

	/*
	bool HasBaopai() { return _baopai.card_type() != 0 && _baopai.card_value() != 0; } //当前是否含有宝牌
	void OnRefreshBaopai(int64_t player_id, int32_t random_result); //刷新宝牌
	void OnPlayerLookAtBaopai(int64_t player_id, Asset::RandomSaizi proto);
	int32_t GetRemainBaopai(); //剩余宝牌数量
	*/

	void SetRandResult(int32_t random_result) { _random_result = random_result; }
	int32_t GetRandResult() { return _random_result; }
	const std::vector<int32_t>& GetSaizi() { return _saizi_random_result; }

	/*
	const Asset::PaiElement& GetHuiPai() { return _huipai; } //获取会儿牌
	bool IsHuiPai(const Asset::PaiElement& pai) {  //是否是会儿牌
		if (_huipai.card_type() == 0 || _huipai.card_value() == 0) return false;
		return pai.card_type() == _huipai.card_type() && pai.card_value() == _huipai.card_value(); 
	} 
	bool IsHuiPai(int32_t card_type, int32_t card_value) {  //是否是会儿牌
		if (_huipai.card_type() == 0 || _huipai.card_value() == 0) return false;
		return card_type == _huipai.card_type() && card_value == _huipai.card_value();
	}
	*/

	/*
	bool CheckLiuJu(); //流局检查
	bool IsLiuJu() { return _liuju; } //是否流局
	void OnLiuJu(); //流局 
	*/

	int32_t GetRemainCount() { return _cards.size() - _random_result_list.size(); } //当前剩余牌数量
	/*
	bool SanJiaBi(int64_t hupai_player_id); //三家闭门
	*/

	void SetCurrPlayerIndex(int64_t curr_player_index) { _curr_player_index = curr_player_index; } //设置当前可操作的玩家
	void SetCurrPlayerIndexByPlayer(int64_t player_id) { _curr_player_index = GetPlayerOrder(player_id); } //设置当前玩家索引//主要用于玩家发牌后操作
	int32_t GetCurrPlayerIndex() { return _curr_player_index; }

	void SavePlayBack(); //回放存储
	void AddPlayerOperation(const Asset::PaiOperation& pai_operate) { _playback.mutable_oper_list()->Add()->CopyFrom(pai_operate); } //回放记录
};

/////////////////////////////////////////////////////
//游戏通用管理类
/////////////////////////////////////////////////////
class GameManager
{
private:
	std::unordered_map<int32_t/*牌索引*/, Asset::PaiElement/*牌值*/> _cards;
	std::vector<Asset::PaiElement> _pais; //牌值
	std::vector<std::shared_ptr<Game>> _games;
	std::vector<std::vector<int32_t>> _combines; //排列组合序列
	std::unordered_map<int32_t, int32_t> _card_tye_weight = 
	{
		{Asset::CARD_TYPE_HEITAO, 4}, 
		{Asset::CARD_TYPE_HONGTAO, 3}, 
		{Asset::CARD_TYPE_MEIHUA, 2}, 
		{Asset::CARD_TYPE_FANGPIAN, 1}, 
	};
public:
	static GameManager& Instance()
	{
		static GameManager _instance;
		return _instance;
	}

	bool Load(); //加载麻将牌数据

	Asset::PaiElement GetCard(int32_t card_index) 
	{
		auto it = _cards.find(card_index);
		if (it != _cards.end()) return it->second; 
		return {};
	}
	
	const std::unordered_map<int32_t, Asset::PaiElement>& GetAllCards() { return _cards; }
	const std::vector<Asset::PaiElement>& GetPais() { return _pais; } 
	
	void OnCreateGame(std::shared_ptr<Game> game);
	void DoCombine();
	const std::vector<std::vector<int32_t>>& GetCombine() { return _combines; } //排列组合序列
	int32_t GetCardWeight(int32_t card_type);
	bool ComparePai(const Asset::PaiElement& p1, const Asset::PaiElement& p2);
};

#define GameInstance GameManager::Instance()

}
