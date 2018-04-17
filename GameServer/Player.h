#pragma once

#include <map>
#include <mutex>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <functional>
#include <unordered_set>

#include "P_Header.h"
#include "Item.h"
#include "Asset.h"
#include "MessageDispatcher.h"
#include "World.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Room;
class Game;

struct Card_t {

	int32_t _type; //类型
	int32_t _value; //值

public:
	bool operator == (const Card_t& card)
	{
		return _type == card._type && _value == card._value;
	}

	Card_t operator + (int32_t value)
	{
		return Card_t(_type, _value + value);
	}

	Card_t(int32_t type, int32_t value)
	{
		_type = type;
		_value = value;
	}
};

extern int32_t g_server_id;
extern const Asset::CommonConst* g_const;

class Player : public std::enable_shared_from_this<Player>
{
	typedef std::function<int32_t(pb::Message*)> CallBack;
	unordered_map<int32_t, CallBack>  _callbacks;	//每个协议的回调函数，不要传入引用
	std::shared_ptr<CenterSession> _session = nullptr;
private:
	int64_t _player_id = 0; //玩家全局唯一标识
	Asset::Player _stuff; //玩家数据，存盘数据
	Asset::PlayerProp _player_prop; //玩家临时状态，不进行存盘
	Asset::User _user; //账号信息

	int64_t _heart_count = 0; //心跳次数
	int32_t _hi_time = 0; 
	int32_t _pings_count = 0;
	bool _dirty = false; //脏数据
	CallBack _method; //协议处理回调函数
	int32_t _chupai_count = 0; //出牌次数
public:
	Player();
	Player(int64_t player_id);
	//Player(int64_t player_id, std::shared_ptr<WorldSession> session);
	
	//const std::shared_ptr<WorldSession> GetSession() { return _session;	}
	//bool Connected() { if (!_session) return false; return _session->IsConnect(); }

	const std::shared_ptr<CenterSession> GetSession() { return _session; }
	void SetSession(std::shared_ptr<CenterSession> session) { _session = session; }
	bool Connected() { if (!_session) return false; return _session->IsConnected(); }

	int32_t DefaultMethod(pb::Message*); //协议处理默认调用函数
	
	void AddHandler(Asset::META_TYPE message_type, CallBack callback)
	{
		int32_t type_t = message_type;

		if (_callbacks.find(type_t) != _callbacks.end()) return;

		_callbacks.emplace(type_t, callback);
	}

	CallBack& GetMethod(int32_t message_type)
	{
		auto it = _callbacks.find(message_type);
		if (it == _callbacks.end()) return _method;
		return it->second;
	}

	Asset::Player& Get() { return _stuff; } //获取玩家数据
	bool HasClan(int64_t clan_id);

	//获取基础属性
	const Asset::CommonProp& CommonProp() { return _stuff.common_prop(); }
	const Asset::CommonProp& GetCommonProp() { return _stuff.common_prop(); }
	Asset::CommonProp* MutableCommonProp() { return _stuff.mutable_common_prop(); }

	//微信数据
	const Asset::WechatUnion GetWechat();
	const std::string GetNickName();
	const std::string GetHeadImag();
	const std::string GetIpAddress();

	void OnBind(std::string account);

	int32_t GetLocalServer() { return _stuff.server_id(); } //玩家当前所在服务器
	void SetLocalServer(int32_t server_id) { return _stuff.set_server_id(server_id); }

	virtual int64_t GetID() { return _stuff.common_prop().player_id(); } //获取ID
	virtual void SetID(int64_t player_id) { 
		_player_id = player_id; //缓存
		_stuff.mutable_common_prop()->set_player_id(player_id); 
	} 
	//获取名字
	virtual std::string GetName() { return _stuff.common_prop().name(); }
	virtual void SetName(std::string name) { _stuff.mutable_common_prop()->set_name(name); } 
	//获取级别
	virtual int32_t GetLevel() { return _stuff.common_prop().level(); }
	//获取性别
	virtual int32_t GetGender() { return _stuff.common_prop().gender(); }
	//消息处理
	virtual bool HandleMessage(const Asset::MsgItem& item); 
	virtual void SendMessage(const Asset::MsgItem& item);
	virtual void SendMessage(int64_t receiver, const pb::Message& message);
	virtual void SendMessage(int64_t receiver, const pb::Message* message);
	virtual void BroadCastCommonProp(Asset::MSG_TYPE type); //向房间里的玩家发送公共数据       
	//协议处理(Protocol Buffer)
	virtual bool HandleProtocol(int32_t type_t, pb::Message* message);
	virtual void SendProtocol(const pb::Message& message);
	virtual void SendProtocol(const pb::Message* message);
	virtual void Send2Roomers(pb::Message& message, int64_t exclude_player_id = 0); //向房间里玩家发送协议数据，发送到Client
	virtual void Send2Roomers(pb::Message* message, int64_t exclude_player_id = 0); //向房间里玩家发送协议数据，发送到Client
	virtual void BroadCast(Asset::MsgItem& item);
	//virtual void OnCreatePlayer(int64_t player_id);
	//进入游戏
	//virtual int32_t CmdEnterGame(pb::Message* message);
	virtual int32_t OnEnterGame();
	//创建房间
	virtual int32_t CmdCreateRoom(pb::Message* message);
	virtual int32_t CreateRoom(pb::Message* message);
	virtual void OnCreateRoom(Asset::CreateRoom* create_room);
	virtual void OnRoomRemoved();
	//进入房间
	virtual int32_t CmdEnterRoom(pb::Message* message);
	virtual int32_t EnterRoom(pb::Message* message);
	virtual void OnEnterSuccess(int64_t room_id = 0); //成功回调

	bool HasMatching(Asset::ROOM_TYPE room_type) { 
		if (_stuff.matching_room_type() == room_type) return false; 
		return _stuff.matching_room_type() == Asset::ROOM_TYPE_XINSHOU || _stuff.matching_room_type() == Asset::ROOM_TYPE_GAOSHOU || _stuff.matching_room_type() == Asset::ROOM_TYPE_DASHI; }
	void SetMatchingRoom(Asset::ROOM_TYPE room_type) { _stuff.set_matching_room_type(room_type); }
	void ClearMatching() { _stuff.clear_matching_room_type(); }

	//玩家登录
	void OnLogin();
	//玩家登出
	virtual int32_t Logout(pb::Message* message = nullptr);
	virtual int32_t OnLogout(Asset::KICK_OUT_REASON reason = Asset::KICK_OUT_REASON_LOGOUT);
	//离开房间
	virtual int32_t CmdLeaveRoom(pb::Message* message);
	virtual void OnLeaveRoom(Asset::GAME_OPER_TYPE reason = Asset::GAME_OPER_TYPE_LEAVE);
	//加载数据	
	virtual int32_t Load();
	//保存数据
	virtual int32_t Save(bool force = false);
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
	bool IsOnline() { return _stuff.login_time() != 0; }
	//是否离线//房间内离线状态处理
	bool IsOffline() { return _player_prop.offline(); }
	void SetOffline(bool offline = true);
	//签到
	virtual int32_t CmdSign(pb::Message* message);
	//获取玩家基础属性
	virtual int32_t CmdGetCommonProperty(pb::Message* message);
	void SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE reason = Asset::SyncCommonProperty_SYNC_REASON_TYPE_SYNC_REASON_TYPE_SELF);
	//离线状态实时监测
	virtual int32_t CmdSayHi(pb::Message* message);
	void OnSayHi();
	void OnlineCheck();
	//游戏设置
	virtual int32_t CmdGameSetting(pb::Message* message);
	//系统聊天
	virtual int32_t CmdSystemChat(pb::Message* message);
	//茶馆
	virtual int32_t CmdClanOperate(pb::Message* message);

	//踢下线
	virtual int32_t OnKickOut(pb::Message* message);
	//玩家状态变化
	virtual int32_t OnPlayerStateChanged(pb::Message* message);
	//获取房间语音成员ID
	int64_t GetVoiceMemberID() { return _player_prop.voice_member_id(); }
public:
	//获取所有包裹
	const Asset::Inventory& GetInventory() { return _stuff.inventory();	}

	//获取指定包裹
	const Asset::Inventory_Element& GetInventory(Asset::INVENTORY_TYPE type) { return _stuff.inventory().inventory(type); }	
	Asset::Inventory_Element* GetMutableInventory(Asset::INVENTORY_TYPE type) { return _stuff.mutable_inventory()->mutable_inventory(type);	}	
	
	//获取物品
	bool GainItem(Item* item, int32_t count = 1);
	bool GainItem(int64_t global_item_id, int32_t count = 1);
	bool PushBackItem(Asset::INVENTORY_TYPE inventory_type, Item* item); //存放物品

	//通用错误码提示
	void AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type = Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE error_show_type = Asset::ERROR_SHOW_TYPE_NORMAL);

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
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
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	//通用限制
	const Asset::PlayerCommonLimit& GetCommonLimit() { return _stuff.common_limit(); }
	Asset::PlayerCommonLimit* GetMutableCommonLimit() {	return _stuff.mutable_common_limit(); }
	bool AddCommonLimit(int64_t global_id);
	bool IsCommonLimit(int64_t global_id);
	bool CommonLimitUpdate();
	void SyncCommonLimit();
	//通用奖励
	Asset::ERROR_CODE DeliverReward(int64_t global_id);
	void SyncCommonReward(int64_t common_reward_id);

	//获胜局信息
	void AddTotalRounds() { _stuff.mutable_common_prop()->set_total_rounds(_stuff.common_prop().total_rounds() + 1); _dirty = true; }
	void AddFriendRoomRounds() { _stuff.mutable_common_prop()->set_room_card_rounds(_stuff.common_prop().room_card_rounds() + 1); _dirty = true; }
	void AddTotalWinRounds() { _stuff.mutable_common_prop()->set_total_win_rounds(_stuff.common_prop().total_win_rounds() + 1); _dirty = true; }
	void SetStreakWins(int32_t count); //最高连胜
	void AddStreakWins() { _stuff.mutable_common_prop()->set_streak_wins(_stuff.common_prop().streak_wins() + 1); _dirty = true; }; //最高连胜
///////游戏逻辑定义
private:
	std::mutex _card_lock;
	std::shared_ptr<Room> _room = nullptr; //实体所在房间
	std::shared_ptr<Game> _game = nullptr; //当前游戏
	bool _debug_model = false;

	//玩家牌数据
	std::list<Asset::PaiElement> _cards_pool; //牌池//玩家已经打的牌缓存
	std::vector<Asset::PaiElement> _cards_inhand; //玩家手里的牌

	bool _tuoguan_server = false; //服务器托管

	std::vector<Asset::PAI_OPER_TYPE> _xf_gang; //旋风杠所有操作
	std::vector<std::tuple<bool, bool, bool>> _hu_result;
	Asset::PAI_OPER_TYPE _oper_type; //玩家当前操作类型
	Asset::PAI_OPER_TYPE _last_oper_type; //玩家上次操作类型

public:
	//
	//玩家操作
	//
	virtual int32_t CmdGameOperate(pb::Message* message); //游戏操作
	virtual int32_t CmdPaiOperate(pb::Message* message); //牌操作
	virtual int32_t CmdGetReward(pb::Message* message); //领取奖励
	virtual int32_t CmdLoadScene(pb::Message* message); //加载场景
	virtual int32_t CmdLuckyPlate(pb::Message* message); //幸运转盘
	virtual int32_t CmdSaizi(pb::Message* message); //打股子
	virtual int32_t CmdGetRoomData(pb::Message* message); //获取房间数据
	virtual int32_t CmdUpdateRoom(pb::Message* message); //更新房间数据
	virtual int32_t CmdRecharge(pb::Message* message); //充值

	void OnEnterScene(bool is_reenter); //进入房间回调
	virtual std::shared_ptr<Room> GetRoom() { return _room; }	//获取当前房间

	virtual void SetRoomID(int64_t room_id) { _player_prop.set_room_id(room_id); }	//设置房间ID
	virtual int32_t GetLocalRoomID(); //获取当前房间ID

	virtual int32_t GetRoomID() { return _player_prop.room_id(); }
	virtual bool HasRoom() { return _room != nullptr; }
	virtual void SetRoom(std::shared_ptr<Room> room) { _room = room; }
	virtual void ResetRoom();

	void SetGame(std::shared_ptr<Game> game) { _game = game; }
	bool IsInGame() { return _game != nullptr; }

	virtual int32_t OnFaPai(std::vector<int32_t>& cards); //发牌
	bool RemovePai(const Asset::PaiElement& pai); //删除手里的牌，返回是否删除成功

	void OnGameStart(); //开局

	bool IsReady() { return _player_prop.game_oper_state() == Asset::GAME_OPER_TYPE_START; } //是否已经在准备状态 
	bool AgreeDisMiss() { return _player_prop.game_oper_state() == Asset::GAME_OPER_TYPE_DISMISS_AGREE; } //是否同意解散 
	bool DisAgreeDisMiss() { return _player_prop.game_oper_state() == Asset::GAME_OPER_TYPE_DISMISS_DISAGREE; } //是否拒绝解散 
	void ClearDisMiss() { _player_prop.clear_game_oper_state(); }

	Asset::GAME_OPER_TYPE GetOperState();
	void SetOperState(Asset::GAME_OPER_TYPE oper_type) { return _player_prop.set_game_oper_state(oper_type); }

	//获取//设置玩家座次
	Asset::POSITION_TYPE GetPosition() { return _player_prop.position(); }
	void SetPosition(Asset::POSITION_TYPE position) { _player_prop.set_position(position); }
	
	void PrintPai(); //打印牌玩家当前牌

	const std::vector<Asset::PaiElement>& GetCardsInhand() { return _cards_inhand; } 
	int32_t GetCardsCountInhand() { return _cards_inhand.size(); }

	void Add2CardsPool(Asset::PaiElement pai) { _cards_pool.push_back(pai); }
	bool PaiXingCheck(Asset::PaiOperation* pai_operate);

	void ClearCards(); //删除玩家牌(包括手里牌、墙外牌)
	void OnGameOver(); //游戏结束

	//是否//设置服务器托管状态
	bool HasTuoGuan();
	void SetTuoGuan() { _tuoguan_server = true; }

	bool AddRoomRecord(int64_t room_id);
	void SendRoomState(); //房间状态
	void AddRoomScore(int32_t score); //胜率
	
	int32_t GetHosterCount() { return _stuff.clan_hosters().size(); } //拥有茶馆数量
	int32_t GetMemberCount() { return _stuff.clan_joiners().size(); } //加入茶馆数量
	bool IsHoster(int64_t clan_id); //是否是该茶馆的老板

	void OnClanCreated(int64_t clan_id); //成功创建茶馆//俱乐部
	bool OnClanJoin(int64_t clan_id); //成功加入茶馆//俱乐部
	void OnQuitClan(int64_t clan_id); //成功退出茶馆//俱乐部

	void SetCurrClan(int64_t clan_id); //设置当前所在俱乐部
	void OnClanCheck(); //通用检查
	
	bool IsDaili() { return _stuff.agent_account().size() > 0;} //是否是代理账号
	int32_t GetChuPaiCount() { return _chupai_count; } //出牌次数
};

/////////////////////////////////////////////////////
//玩家通用管理类
/////////////////////////////////////////////////////
class PlayerManager : public std::enable_shared_from_this<PlayerManager>
{
private:
	std::mutex _player_lock;
	std::unordered_map<int64_t, std::shared_ptr<Player>> _players; //实体为智能指针，不要传入引用
	int32_t _heart_count = 0;
public:
	static PlayerManager& Instance()
	{
		static PlayerManager _instance;
		return _instance;
	}

	void Update(int32_t diff);

	void Remove(int64_t player_id);
	void Remove(std::shared_ptr<Player> player);

	void Emplace(int64_t player_id, std::shared_ptr<Player> player);

	bool Has(int64_t player_id);

	std::shared_ptr<Player> GetPlayer(int64_t player_id);
	std::shared_ptr<Player> Get(int64_t player_id);
	
	virtual void BroadCast(const pb::Message& message);
};

#define PlayerInstance PlayerManager::Instance()

}
