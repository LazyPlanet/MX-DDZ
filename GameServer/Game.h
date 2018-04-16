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

#define CARDS_COUNT 54
	
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
	int64_t _banker_player_id = 0; //庄家//起手叫地主
	int64_t _dizhu_player_id = 0; //地主//叫地主之后产生
	int64_t _room_id = 0;
	int32_t _game_id = 0;

	bool _real_started = false; //开局

	Asset::PaiOperationCache _last_oper; //上次操作缓存
	std::shared_ptr<Player> _players[MAX_PLAYER_COUNT]; //玩家数据：按照进房间的顺序，0->1->2->3...主要用于控制发牌和出牌顺序

	std::vector<Asset::PaiElement> _cards_pool; //牌池，玩家已经打的牌缓存

	Asset::PlayBack _playback; //回放数据
	
	int32_t _beilv = 1; //抢地主倍率
	int32_t _rob_dizhu_count = 0; //是否抢地主操作次数
	std::unordered_map<int64_t, int32_t> _rob_dizhu_bl; //倍率表//缓存玩家叫分
	std::vector<int64_t> _rob_dizhus; //叫地主的玩家
public:
	virtual void Init(std::shared_ptr<Room> room); //初始化
	virtual bool Start(std::vector<std::shared_ptr<Player>> players, int64_t room_id = 0, int32_t game_id = 0); //开始游戏
	virtual void OnStart(); //开始游戏回调
	virtual void OnStarted(std::shared_ptr<Player> dizhu_ptr); //开始游戏,玩家已经进入游戏且抢完地主
	
	bool CanStart(); //是否可以开局

	virtual bool OnGameOver(int64_t player_id); //游戏结束
	void ClearState(); //清理状态

	virtual std::vector<int32_t> FaPai(size_t card_count); //发牌
	//virtual std::vector<int32_t> TailPai(size_t card_count); //后楼发牌
	//void FaPaiAndCommonCheck();

	virtual void Add2CardsPool(Asset::PaiElement pai);
	virtual void Add2CardsPool(Asset::CARD_TYPE card_type, int32_t card_value);
	
	bool CanPaiOperate(std::shared_ptr<Player> player, Asset::PaiOperation* pai_operate = nullptr); //检查玩家是否具有操作权限
	void OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message); //玩家牌操作响应

	void OnPlayerReEnter(std::shared_ptr<Player> player);

	void OnOperateTimeOut();
	const Asset::PaiOperationCache& GetOperCache() { return _last_oper; }

	std::shared_ptr<Player> GetNextPlayer(int64_t player_id); //获取下家
	std::shared_ptr<Player> GetPlayer(int64_t player_id); //获取玩家
	std::shared_ptr<Player> GetPlayerByOrder(int32_t player_index);

	int32_t GetPlayerOrder(int32_t player_id); //获取玩家的顺序

	void SetRoom(std::shared_ptr<Room> room) {	_room = room; } //设置房间
	int64_t GetID() { return _game_id; } //局数

	bool IsBanker(int64_t player_id); //是否庄家
	bool IsStarted() { return _real_started; } //是否真正开局

	void BroadCast(pb::Message* message, int64_t exclude_player_id = 0);
	void BroadCast(pb::Message& message, int64_t exclude_player_id = 0);

	void Calculate(std::shared_ptr<Player> player);
	void PaiPushDown();

	void SetCurrPlayerIndex(int64_t curr_player_index) { _curr_player_index = curr_player_index; } //设置当前可操作的玩家
	void SetCurrPlayerIndexByPlayer(int64_t player_id) { _curr_player_index = GetPlayerOrder(player_id); } //设置当前玩家索引//主要用于玩家发牌后操作
	int32_t GetCurrPlayerIndex() { return _curr_player_index; }

	void SavePlayBack(); //回放存储
	void AddPlayerOperation(const Asset::PaiOperation& pai_operate) { _playback.mutable_oper_list()->Add()->CopyFrom(pai_operate); } //回放记录
	
	//斗地主
	bool RandomDiZhu(); //随机地主
	void IncreaseBeiLv(int32_t beilv = 2) { if (beilv <= 0) beilv = 2; _beilv *= beilv; } //加倍
	int32_t GetBeiLv() { return _beilv; } //获取倍率

	int32_t GetDiZhuPlayerCount() { return _rob_dizhu_bl.size(); } //获取抢地主玩家数量//叫分
	int64_t GetDiZhu() { return _dizhu_player_id; } //地主
	void SetDiZhu(int64_t player_id);

	void OnRobDiZhu(int64_t player_id, int32_t beilv) { _rob_dizhu_bl[player_id] = beilv; } //叫分抢地主
	void OnRobDiZhu(int64_t player_id, bool is_rob); //加倍抢地主
	int32_t GetRobDiZhuCount() { return _rob_dizhus.size(); }
	int32_t GetDiZhuOperCount() { return _rob_dizhu_count; }
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
		{Asset::CARD_TYPE_HEITAO, 1}, 
		{Asset::CARD_TYPE_HONGTAO, 1}, 
		{Asset::CARD_TYPE_MEIHUA, 1}, 
		{Asset::CARD_TYPE_FANGPIAN, 1}, 
		{Asset::CARD_TYPE_KINGS, 2}, 
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
	const std::vector<std::vector<int32_t>>& GetCombine() { return _combines; } //排列组合序列
	int32_t GetCardWeight(const Asset::PaiElement& card);
	bool ComparePai(const Asset::PaiElement& p1, const Asset::PaiElement& p2);
};

#define GameInstance GameManager::Instance()

}
