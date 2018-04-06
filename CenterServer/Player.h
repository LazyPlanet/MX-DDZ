#pragma once

#include <map>
#include <mutex>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <functional>

#include "P_Header.h"
#include "Asset.h"
#include "WorldSession.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Player : public std::enable_shared_from_this<Player>
{
	typedef std::function<int32_t(pb::Message*)> CallBack;
	std::unordered_map<int32_t, CallBack>  _callbacks;	//每个协议的回调函数，不要传入引用
private:
	int64_t _player_id = 0; //玩家ID
	Asset::Player _stuff; //玩家数据，存盘数据
	Asset::GAME_OPER_TYPE _player_state; //玩家状态//在线//离线
	Asset::ACCOUNT_TYPE _account_type;

	int64_t _heart_count = 0; //心跳次数
	std::time_t _hi_time = 0;
	int32_t _expire_time = 0;
	int32_t _pings_count = 0;
	bool _dirty = false; //脏数据
	bool _loaded = false; //数据是否加载

	CallBack _method; //协议处理回调函数
	//std::shared_ptr<WorldSession> _session = nullptr; //Client网络连接//循环引用
	//std::shared_ptr<WorldSession> _gs_session = nullptr; //游戏逻辑服务器网络连接

public:
	~Player();
	Player();
	Player(int64_t player_id/*, std::shared_ptr<WorldSession> session*/);
	
	//void SetGameServer(std::shared_ptr<WorldSession> gs_session) { _gs_session = gs_session; }
	//std::shared_ptr<WorldSession> GetGameServer() { return _gs_session; }

	//const std::shared_ptr<WorldSession> GetSession() { return _session;	}
	//void SetSession(std::shared_ptr<WorldSession> session) { _session = session; }
	bool Connected(); //网络是否连接
	bool IsExpire();

	int32_t DefaultMethod(pb::Message*); //协议处理默认调用函数
	
	void AddHandler(Asset::META_TYPE message_type, CallBack callback)
	{
		if (_callbacks.find(message_type) != _callbacks.end()) return;

		_callbacks.emplace(message_type, callback);
	}

	CallBack& GetMethod(int32_t message_type)
	{
		auto it = _callbacks.find(message_type);
		if (it == _callbacks.end()) return _method;
		return it->second;
	}

	void ResetLoad() { _loaded = false; }

	Asset::Player& Get() { return _stuff; } //获取玩家数据

	const Asset::CommonProp& CommonProp() { return _stuff.common_prop(); } //获取基础属性
	const Asset::CommonProp& GetCommonProp() { return _stuff.common_prop(); }
	
	bool IsCenterServer(); //是否在中心服务器
	void SetLocalServer(int32_t server_id);
	int32_t GetLocalServer() { return _stuff.server_id(); } //玩家当前所在服务器

	virtual int64_t GetID() { return _stuff.common_prop().player_id(); } //获取ID
	virtual void SetID(int64_t player_id) { 
		_player_id = player_id; //缓存
		_stuff.mutable_common_prop()->set_player_id(player_id); 
	} 

	//获取名字
	virtual std::string GetName() { return _stuff.common_prop().name(); }
	virtual void SetName(std::string name) { _stuff.mutable_common_prop()->set_name(name); } 
	//账号
	virtual std::string GetAccount() { return _stuff.account(); }
	virtual void SetAccount(std::string account, Asset::ACCOUNT_TYPE account_type) { 
		_stuff.set_account(account); 
		_account_type = account_type; //账号类型
	} 
	//获取级别
	virtual int32_t GetLevel() { return _stuff.common_prop().level(); }
	//获取性别
	virtual int32_t GetGender() { return _stuff.common_prop().gender(); }
	//协议处理(Protocol Buffer)
	virtual bool HandleProtocol(int32_t type_t, pb::Message* message);
	virtual void SendProtocol(const pb::Message& message);
	virtual void SendProtocol(const pb::Message* message);
	virtual void SendMeta(const Asset::Meta& meta);
	
	virtual bool SendProtocol2GameServer(const pb::Message& message);
	virtual bool SendProtocol2GameServer(const pb::Message* message);

	virtual void SendGmtProtocol(const pb::Message* message, int64_t session_id);
	virtual void SendGmtProtocol(const pb::Message& message, int64_t session_id);

	Asset::ERROR_CODE CommonCheck(int32_t type_t);
	//玩家登出
	virtual int32_t Logout(pb::Message* message);
	virtual int32_t OnLogout();
	virtual int32_t OnLogin(bool is_login = true);
	//加载数据	
	virtual int32_t Load();
	//保存数据
	virtual int32_t Save(bool force = false);
	//进入游戏
	virtual int32_t OnEnterGame(bool is_login = true);
	virtual int32_t OnEnterCenter();
	//是否脏数据
	virtual bool IsDirty() { return _dirty; }
	virtual void SetDirty(bool dirty = true) { _dirty = dirty; }
	//同步玩家数据
	virtual void SendPlayer();
	//玩家心跳周期为10MS，如果该函数返回FALSE则表示掉线
	virtual bool Update();
	//购买商品
	virtual int32_t CmdBuySomething(pb::Message* message);
	//是否在线
	bool IsOnline() { return _stuff.login_time() != 0 || _player_state == Asset::GAME_OPER_TYPE_ONLINE; }
	//签到
	virtual int32_t CmdSign(pb::Message* message);
	//获取玩家基础属性
	virtual int32_t CmdGetCommonProperty(pb::Message* message);
	void SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE reason = Asset::SyncCommonProperty_SYNC_REASON_TYPE_SYNC_REASON_TYPE_SELF);
	//离线状态实时监测
	virtual int32_t CmdSayHi(pb::Message* message);
	void SayHi();
	//游戏设置
	virtual int32_t CmdGameSetting(pb::Message* message);
	//领取奖励
	int32_t CmdGetReward(pb::Message* message);
	//幸运转盘
	int32_t CmdLuckyPlate(pb::Message* message);
	//历史战绩
	virtual int32_t CmdGetBattleHistory(pb::Message* message);
	//充值
	virtual int32_t CmdRecharge(pb::Message* message);
	//回放
	virtual int32_t CmdPlayBack(pb::Message* message);
	//匹配信息
	virtual int32_t CmdGetMatchStatistics(pb::Message* message);
public:
	//获取所有包裹
	const Asset::Inventory& GetInventory() { return _stuff.inventory();	}
	//获取指定包裹
	const Asset::Inventory_Element& GetInventory(Asset::INVENTORY_TYPE type) { return _stuff.inventory().inventory(type); }	
	Asset::Inventory_Element* GetMutableInventory(Asset::INVENTORY_TYPE type) { return _stuff.mutable_inventory()->mutable_inventory(type);	}	
	//通用错误码提示
	void AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type = Asset::ERROR_TYPE_NORMAL, 
			Asset::ERROR_SHOW_TYPE error_show_type = Asset::ERROR_SHOW_TYPE_NORMAL);

	//
	// 欢乐豆相关
	//
	int64_t ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count); //消费欢乐豆(返回实际消耗的欢乐豆数)
	int64_t GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count); //增加欢乐豆
	bool CheckHuanledou(int64_t count); //欢乐豆是否足够
	int64_t GetHuanledou(); //获取欢乐豆数量

	//
	// 钻石相关
	//
	int64_t ConsumeDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count); //消费钻石(返回实际消耗的钻石数)
	int64_t GainDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count); //增加钻石
	bool CheckDiamond(int64_t count); //钻石是否足够
	int64_t GetDiamond(); //获取钻石数量
	
	//
	// 房卡相关
	//
	int64_t ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count); //消费房卡(返回实际消耗的房卡数)
	int64_t GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count); //增加房卡
	bool CheckRoomCard(int64_t count); //房卡是否足够
	int64_t GetRoomCard(); //获取房卡数量

	//
	//游戏常规逻辑定义
	//

	//通用限制
	const Asset::PlayerCommonLimit& GetCommonLimit() { return _stuff.common_limit(); }
	Asset::PlayerCommonLimit* GetMutableCommonLimit() {	return _stuff.mutable_common_limit(); }
	bool AddCommonLimit(int64_t global_id);
	bool IsCommonLimit(int64_t global_id);
	bool CommonLimitUpdate();
	void SyncCommonLimit();
	//冷却
	const Asset::PlayerCoolDown& GetCoolDown() { return _stuff.cool_down(); }
	Asset::PlayerCoolDown* GetMutableCoolDown() {	return _stuff.mutable_cool_down(); }
	
	bool AddCoolDown(int64_t global_id);
	bool IsCoolDown(int64_t global_id);
	//通用奖励
	Asset::ERROR_CODE DeliverReward(int64_t global_id);
	void SyncCommonReward(int64_t common_reward_id, int32_t error_code);
	//历史战绩
	void BattleHistory(int32_t start_index = 0, int32_t end_index = 5);
	//踢人
	void OnKickOut(Asset::KICK_OUT_REASON reason);
	//玩家离线
	void SetOffline(bool offline = true);

	void SetAccountType(Asset::ACCOUNT_TYPE account_type) { _account_type = account_type; }
	Asset::ACCOUNT_TYPE GetAccountType() { return _account_type; } //账号类型

	void MultiplyRoomCard();
};

class PlayerManager : public std::enable_shared_from_this<PlayerManager>
{
private:
	std::mutex _mutex;
	std::unordered_map<int64_t, std::shared_ptr<Player>> _players; //实体为智能指针，不要传入引用
	int64_t _heart_count = 0;
public:
	static PlayerManager& Instance()
	{
		static PlayerManager _instance;
		return _instance;
	}

	void Update(int32_t diff);
	bool BeenMaxPlayer(); //最大玩家上限

	void Remove(int64_t player_id);
	void Remove(std::shared_ptr<Player> player);
	void Emplace(int64_t player_id, std::shared_ptr<Player> player);
	bool Has(int64_t player_id);
	std::shared_ptr<Player> GetPlayer(int64_t player_id);
	std::shared_ptr<Player> Get(int64_t player_id);
	int32_t GetOnlinePlayerCount(); //获取在线玩家数量//带缓存
	
	virtual void BroadCast(const pb::Message& message);
};

#define PlayerInstance PlayerManager::Instance()

}
