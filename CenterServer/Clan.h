#pragma once

#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#include "P_Header.h"
#include "Player.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Clan : public std::enable_shared_from_this<Clan>
{
private:
	std::mutex _mutex, _member_mutex;
	std::mutex _joiners_mutex, _applicants_mutex;
	Asset::Clan _stuff;
	bool _dirty = false;
	int64_t _clan_id = 0;
	int64_t _heart_count = 0;

	std::unordered_map<int64_t, Asset::RoomQueryResult> _rooms; 
	std::vector<int64_t> _gaming_room_list;

	std::unordered_set<int64_t> _applicants; //比赛报名玩家
	std::unordered_set<int64_t> _joiners; //比赛开始之后，参加比赛的玩家

	bool _match_opened = false; //比赛是否可以报名
	int32_t _match_server_id = 0; //比赛模式茶馆开房逻辑服务器
	int32_t _curr_rounds = 0; //当前比赛轮次

	//std::atomic<bool> _matching_start; //比赛已经开始，玩家可以比赛
	bool _room_created = false; //房间是否创建完毕

	std::unordered_set<int64_t/*房间ID*/> _room_list; //房间列表：根据报名玩家数量创建的最大房间数量，所有房间缓存

	std::unordered_map<int64_t/*房间ID*/, std::vector<int64_t>/*比赛玩家列表*/> _room_players; //开局后，每个房间内的所有玩家
	std::unordered_map<int64_t/*玩家ID*/, int64_t/*房间ID*/> _player_room; //方便查找玩家所在房间
	
	std::unordered_map<int32_t/*轮次*/, std::vector<Asset::PlayerBrief>> _player_details; //轮次各个玩家分数
	std::unordered_map<int64_t/*玩家ID*/, Asset::PlayerBrief> _player_score; //玩家分数累积

	Asset::Room _room; //俱乐部老板比赛房间设置
	Asset::MatchHistory _history; //当轮战绩数据缓存
public:
	Clan(const Asset::Clan& clan) 
	{ 
		_clan_id = clan.clan_id(); 

		_stuff = clan; 

		//_matching_start = false;
	}

	const int64_t GetID() { return _stuff.clan_id(); }
	const Asset::Clan& Get() { return _stuff; }

	bool Load(); //加载数据
	void OnLoaded(); //加载成功

	int64_t GetHoster() { return _stuff.hoster_id(); }
	bool IsHoster(int64_t player_id) { return player_id == _stuff.hoster_id(); }

	void Update();
	void Save(bool force = true);
	void OnDisMiss();
	bool HasDismiss() { return _stuff.dismiss(); }
	int32_t RemoveMember(int64_t player_id, Asset::ClanOperation* message = nullptr);
	void BroadCast(const pb::Message* message, int64_t except_player_id = 0);
	void BroadCast(const pb::Message& message, int64_t except_player_id = 0);

	bool CheckRoomCard(int32_t count);
	void ConsumeRoomCard(int32_t count);
	void AddRoomCard(int32_t count);
	int32_t GetRoomCard() { return _stuff.room_card_count(); }

	void OnRoomChanged(const Asset::ClanRoomStatusChanged* message);
	void OnRoomSync(const Asset::RoomQueryResult& room_query);
	void OnRoomOver(const Asset::ClanRoomStatusChanged* message);

	bool HasGaming() { return _gaming_room_list.size() > 0; } //牌局进行中
	int32_t GetRoomOpenedCount() { return _gaming_room_list.size(); } //总开房房间数量
	int32_t GetRoomGamingCount(); //开局房间在玩的房间数量
	int32_t GetRoomNoPlayedCount(); //开房没有玩的房间数量
	const std::unordered_map<int64_t, Asset::RoomQueryResult>& GetRooms();

	int32_t OnApply(int64_t player_id, Asset::ClanOperation* message);
	int32_t OnChangedInformation(std::shared_ptr<Player> player, Asset::ClanOperation* message);
	int32_t OnAgree(Asset::ClanOperation* message);
	int32_t OnDisAgree(std::shared_ptr<Player> player, Asset::ClanOperation* message);
	int32_t OnRecharge(int32_t count);
	void OnQueryMemberStatus(Asset::ClanOperation* message = nullptr);
	void OnQueryRoomList(std::shared_ptr<Player> player, Asset::ClanOperation* message);
	void OnQueryGamingList(Asset::ClanOperation* message);
	void OnSetUpdateTime();

	//比赛相关
	void OnMatchOpen(std::shared_ptr<Player> player, Asset::OpenMatch* message);
	const Asset::OpenMatch& GetMatchSetting() { return _stuff.match_history().open_match(); }
	bool IsMatchOpen(); //比赛是否开启
	void OnMatchUpdate(); //比赛定时维护
	void OnJoinMatch(std::shared_ptr<Player> player, Asset::JoinMatch* message); //参加比赛
	void AddApplicant(int64_t player_id); //报名
	bool HasApplicant(int64_t player_id); //是否报名过
	void AddJoiner(std::shared_ptr<Player> player); //参加比赛
	void OnCreateRoom(const Asset::ClanCreateRoom* message);
	void OnPlayerMatch(); //进行匹配
	bool GetPlayers(std::vector<int64_t>& players);
	const Asset::Room& GetRoom() { return _room; } //俱乐部部长比赛房间设置
	bool CanJoinMatch(int64_t player_id); //是否可以参加比赛
	void OnMatchRoomOver(const Asset::ClanRoomStatusChanged* message);
	void OnRoundsCalculate();
	bool IsMatchOver() { return _curr_rounds >= _stuff.match_history().open_match().lunci_count(); } //比赛是否结束
	int32_t GetRemainRounds(){ return _stuff.match_history().open_match().lunci_count() - _curr_rounds; } //剩余轮次
	void OnMatchOver();
	void SaveMatchHistory();
	int32_t GetBattleTime() { return _stuff.match_history().open_match().start_time(); }
	int32_t GetMatchRoomCount() { return _room_players.size(); } //获取当前正在比赛的房间数量

	void AddMember(int64_t player_id); //增加成员列表
	bool HasMember(int64_t player_id); //是否含有成员
};

class ClanManager : public std::enable_shared_from_this<ClanManager>
{
private:
	std::mutex _mutex;
	std::unordered_map<int64_t, std::shared_ptr<Clan>> _clans; 
	int64_t _heart_count = 0;
	bool _loaded = false;
public:
	static ClanManager& Instance()
	{
		static ClanManager _instance;
		return _instance;
	}
	
	int32_t IsNameValid(std::string name, std::string trim_name);

	void Update(int32_t diff);
	void Load();

	void Remove(int64_t Clan_id);
	void Remove(std::shared_ptr<Clan> clan);
	void Emplace(int64_t Clan_id, std::shared_ptr<Clan> clan);

	std::shared_ptr<Clan> GetClan(int64_t clan_id);
	std::shared_ptr<Clan> Get(int64_t clan_id);

	void OnOperate(std::shared_ptr<Player> player, Asset::ClanOperation* message);
	void OnCreated(int64_t clan_id, std::shared_ptr<Clan> clan); //创建成功
	void OnGameServerBack(const Asset::ClanOperationSync& message);
	void OnGameServerBack(const Asset::ClanMatchSync& message);
	bool IsLocal(int64_t clan_id);
	bool GetClan(int64_t clan_id, Asset::Clan& clan);
	void OnCreateRoom(const Asset::ClanCreateRoom* message);
};

#define ClanInstance ClanManager::Instance()
}
