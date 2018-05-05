#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>

#include "Asset.h"
#include "Player.h"

namespace Adoter
{

class Player;
class Game;

const int32_t MAX_PLAYER_COUNT = 3;

class Room : public std::enable_shared_from_this<Room>
{
private:
	int32_t _banker_index = 0; //庄家索引
	int64_t _banker = 0; //庄家//玩家ID
	int64_t _hoster_id = 0; //房主ID
	int32_t _expired_time = 0; //过期时间
	bool _gmt_opened = false; //GMT开房
private:
	std::mutex _mutex;
	Asset::Room _stuff; //数据
	std::shared_ptr<Game> _game = nullptr; 
	std::shared_ptr<Player> _hoster = nullptr; 
	std::vector<std::shared_ptr<Game>> _games; //游戏列表
	std::vector<std::shared_ptr<Player>> _players; //房间中的玩家：按照进房间的顺序，东南西北
	Asset::RoomHistory _history; //历史战绩

	//总结算//统计数据
	std::unordered_map<int64_t, int32_t> _winner_rounds; //胜利局数
	std::unordered_map<int64_t, int32_t> _dizhu_rounds; //地主局数
	std::unordered_map<int64_t, int32_t> _zhadan_count; //炸弹数量
	std::unordered_map<int64_t, int32_t> _streak_wins; //最高连胜

	bool _is_dismiss = false; //是否已经解散
	int32_t _dismiss_time = 0; //解散时间
	int32_t _created_time = 0; //创建时间
	int32_t _dismiss_cooldown = 0; //解散冷却时间
	int32_t _no_robed_count = 0; //不叫地主数量
	std::vector<Asset::RobElement> _rob_dizhu; //玩家是否叫地主和叫地主的分数
public:
	explicit Room(Asset::Room room) {  _stuff = room; }

	void SetGmtOpened() { _gmt_opened = true; }
	bool IsGmtOpened() { return _gmt_opened; }

	bool IsClan() { return _stuff.clan_id() != 0; }
	int64_t GetClan() { return _stuff.clan_id(); }

	void OnClanCreated();

	const std::vector<std::shared_ptr<Player>> GetPlayers() { return _players; } 
	int32_t GetCreateTime() { return _created_time; } //创建时间

	virtual int64_t GetID() { return _stuff.room_id(); }
	virtual void SetID(int64_t room_id) { return _stuff.set_room_id(room_id); }

	virtual const Asset::Room& Get() { return _stuff; } //数据
	virtual Asset::ROOM_TYPE GetType() { return _stuff.room_type(); } //数据
	virtual bool IsFriend() { return Asset::ROOM_TYPE_FRIEND == _stuff.room_type(); } //是否是好友房间
	virtual bool IsMatch() { return Asset::ROOM_TYPE_FRIEND != _stuff.room_type(); } //是否是匹配房间
	
	const Asset::RoomOptions& GetOptions() { return _stuff.options(); } //额外番型
	void SetOptions(const Asset::RoomOptions& options) { _stuff.mutable_options()->CopyFrom(options);}

	const Asset::ZHUANG_TYPE GetZhuangType() { return _stuff.options().zhuang_type(); }

	int32_t MaxFan() { return _stuff.options().top_mutiple(); }
	Asset::CITY_TYPE GetCity() { return _stuff.options().city_type(); } //城市玩法

	bool IsVoiceOpen() { return _stuff.options().voice_open(); }

	int32_t GetExpiredTime() { return _expired_time; } //获取过期时间
	void SetExpiredTime(int32_t expired_time) { _expired_time = expired_time; }

	void AddGameRecord(const Asset::GameRecord& record); //记录
	//void AddHupai(int64_t player_id);

	void AddWinner(int64_t player_id) { ++_winner_rounds[player_id]; }
	void AddDiZhu(int64_t player_id) { ++_dizhu_rounds[player_id]; }
	void AddZhaDan(int64_t player_id) { ++_zhadan_count[player_id]; }
public:
	Asset::ERROR_CODE TryEnter(std::shared_ptr<Player> player);
	bool Enter(std::shared_ptr<Player> player); //加入房间
	void OnReEnter(std::shared_ptr<Player> op_player); //再次进入

	void OnPlayerLeave(int64_t player_id);

	void OnCreated(std::shared_ptr<Player> hoster = nullptr); 
	void Update();

	bool IsFull(); //是否已满
	bool IsEmpty(); //是否没人

	bool CanStarGame(); //能否开启游戏

	bool CanDisMiss(); //能否解散
	int32_t GetRemainCount(); //剩余游戏次数
	int32_t GetGamesCount() { return _games.size(); } //当前开局数量
	int32_t GetOpenRands() { return _stuff.options().open_rands(); } //开房总数量

	bool HasStarted() { return _games.size() > 0; } //是否已经开局
	bool HasBeenOver(); //牌局是否结束//总结算

	void ResetGame(std::shared_ptr<Game> game = nullptr);
	std::shared_ptr<Game> GetGame() { return _game; }
	bool IsGaming() { return _game != nullptr; } //牌局进行中，比如上局结束下局尚未开始则不在进行中

	void OnPlayerOperate(std::shared_ptr<Player> player, pb::Message* message);

	void BroadCast(pb::Message* message, int64_t exclude_player_id = 0);
	void BroadCast(pb::Message& message, int64_t exclude_player_id = 0);
	
	void SyncRoom(); //房间数据
	void OnDisMiss(int64_t player_id, pb::Message* message); //解散房间
	void DoDisMiss(); //解散房间

	std::shared_ptr<Player> GetHoster(); //获取房主
	bool IsHoster(int64_t player_id); //是否是房主

	std::shared_ptr<Player> GetPlayer(int64_t player_id); //获取房间中的玩家
	int32_t GetPlayerOrder(int32_t player_id); //获取玩家的顺序
	bool Remove(int64_t player_id, Asset::GAME_OPER_TYPE reason = Asset::GAME_OPER_TYPE_LEAVE); //删除玩家
	void KickOutPlayer(int64_t player_id = 0);

	//游戏开始
	void OnGameStart();
	void OnPlayerStateChanged();
	//游戏结束
	void OnGameOver(int64_t player_id = 0/*胡牌玩家*/);
	void OnClanOver(); //茶馆房间结束
	
	//庄家信息
	void SetBanker(int64_t player_id) { _banker = player_id; } //设置庄家
	int64_t GetBanker() { return _banker; } //获取庄家
	int32_t GetBankerIndex() { return _banker_index; } //庄家索引
	bool IsBanker(int64_t player_id){ return _banker == player_id; } //是否是庄家

	bool IsExpired(); //是否过期
	bool IsTimeOut(); //是否超时
	bool HasDisMiss() { return _is_dismiss; } //是否解散状态
	void ClearDisMiss(); //清除解散状态
	void OnRemove();

	//茶馆信息
	void UpdateClanStatus(); //同步茶馆开房房间数据
	
	//斗地主
	bool OnJiaoZhuang(int64_t player_id, int32_t beilv); //叫庄//倍率或分数//扑克游戏

};

/////////////////////////////////////////////////////
//房间通用管理类
/////////////////////////////////////////////////////
class RoomManager
{
private:
	std::mutex _match_mutex;
	std::mutex _room_lock;

	//好友房//匹配房
	std::unordered_map<int64_t, std::shared_ptr<Room>> _rooms;
	//
	//匹配房间
	//
	//匹配的原理：如果有在匹配的房间，优先选择未满的房间；如果都是已满的房间，则创建一个；
	//
	std::unordered_map<int32_t/*房间类型*/, std::unordered_map<int64_t/*房间ID*/, std::shared_ptr<Room>>> _matching_rooms;
	
	int32_t _heart_count = 0; //心跳
	int32_t _server_id = 0; //服务器ID
public:
	RoomManager();

	static RoomManager& Instance()
	{
		static RoomManager _instance;
		return _instance;
	}
	
	int64_t AllocRoom(); //创建房间：获取房间全局ID
	std::shared_ptr<Room> CreateRoom(const Asset::Room& room); //通过配置创建房间
	bool OnCreateRoom(std::shared_ptr<Room> room); //进入房间回调
	std::shared_ptr<Room> Get(int64_t room_id); //获取房间
	std::shared_ptr<Room> GetMatchingRoom(Asset::ROOM_TYPE room_type); //获取可入房间
	bool CheckPassword(int64_t room_id, std::string password); //密码检查
	void Update(int32_t diff); //心跳
	void UpdateMatching(); //匹配房数据

	void Remove(int64_t room_id);
};

#define RoomInstance RoomManager::Instance()

}
