#include <string>

#include "Clan.h"
#include "Timer.h"
#include "Player.h"
#include "RedisManager.h"
#include "NameLimit.h"

namespace Adoter
{

extern int32_t g_server_id;
extern const Asset::CommonConst* g_const;

//3秒一次
void Clan::Update()
{
	if (_dirty) Save();

	if (!ClanInstance.IsLocal(_clan_id)) Load(); //从茶馆加载数据

	OnQueryMemberStatus(); //定期更新茶馆成员状态
}
	
bool Clan::Load()
{
	if (!_dirty) return true;

	ClanInstance.GetClan(_clan_id, _stuff);

	DEBUG("服务器:{} 茶馆:{} 加载数据:{}", g_server_id, _clan_id, _stuff.ShortDebugString());

	_dirty = false;

	return true;
}

void Clan::OnLoaded()
{
	auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
	if (!clan_limit) return;

	auto curr_time = TimerInstance.GetTime();

	std::vector<Asset::SystemMessage> sys_messages;

	for (const auto& sys_message : _stuff.message_list())
	{
		if (curr_time > sys_message.oper_time() + clan_limit->sys_message_limit() * 24 * 3600) continue;

		sys_messages.push_back(sys_message);
	}

	_stuff.mutable_message_list()->Clear();

	for (const auto& sys_message : sys_messages) 
	{
		auto sys_message_ptr = _stuff.mutable_message_list()->Add();
		sys_message_ptr->CopyFrom(sys_message);
	}
}

int32_t Clan::OnApply(int64_t player_id, Asset::ClanOperation* message)
{
	if (!message) return Asset::ERROR_INNER;
	
	const auto& oper_type = message->oper_type();
	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [player_id, oper_type](const Asset::SystemMessage& sys_message){
				return player_id == sys_message.player_id() && oper_type == sys_message.oper_type();	//相同操作
			});

	if (it != _stuff.mutable_message_list()->end()) //已经申请过加入茶馆
	{
		it->set_oper_time(TimerInstance.GetTime());
		it->set_oper_type(oper_type);

		return Asset::ERROR_CLAN_HAS_APPLY;
	}

	//
	//申请加入茶馆
	
	if (HasMember(player_id)) return Asset::ERROR_CLAN_HAS_JOINED; //已经是茶馆成员
	
	Asset::Player player;
	auto loaded = PlayerInstance.GetCache(player_id, player);
	if (!loaded) return Asset::ERROR_CLAN_NO_MEM;
	
	Asset::User user;
	loaded = RedisInstance.GetUser(player.account(), user);
	if (!loaded) return Asset::ERROR_CLAN_NO_MEM; //没有账号数据

	std::string nickname = player.common_prop().name();
	if (user.wechat().nickname().size()) nickname = user.wechat().nickname();

	auto system_message = _stuff.mutable_message_list()->Add();
	system_message->set_player_id(player_id);
	system_message->set_name(nickname);
	system_message->set_oper_time(TimerInstance.GetTime());
	system_message->set_oper_type(message->oper_type());

	_dirty = true;

	return 0;
}

int32_t Clan::OnRecharge(int32_t count)
{
	AddRoomCard(count);

	DEBUG("在服务器:{} 成功给茶馆:{} 充值房卡:{}", g_server_id, _clan_id, count);

	return 0;
}

int32_t Clan::OnAgree(Asset::ClanOperation* message)
{
	if (!message) return Asset::ERROR_INNER;
	
	auto member_id = message->dest_player_id();
	auto dest_sys_message_index = _stuff.message_list().size() - message->dest_sys_message_index();

	if (dest_sys_message_index < 0 || dest_sys_message_index > _stuff.message_list().size()) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录

	//申请列表状态更新
	//
	const auto& sys_message = _stuff.message_list(dest_sys_message_index);
	if (member_id != sys_message.player_id() || Asset::CLAN_OPER_TYPE_JOIN != sys_message.oper_type()) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录

	auto it = _stuff.mutable_message_list(dest_sys_message_index);

	it->set_oper_time(TimerInstance.GetTime());
	it->set_oper_type(message->oper_type());

	OnSetUpdateTime(); //更新操作时间

	//成员列表更新
	//
	AddMember(member_id);

	return 0;
}

bool Clan::HasMember(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_member_mutex);

	auto it = std::find_if(_stuff.member_list().begin(), _stuff.member_list().end(), [player_id](const Asset::Clan_Member& member){
				return player_id == member.player_id();
			});
	if (it != _stuff.member_list().end()) return true;

	return false;
}
	
void Clan::AddMember(int64_t player_id)
{
	if (HasMember(player_id)) return; //已是成员

	Asset::Player player;
	auto loaded = PlayerInstance.GetCache(player_id, player);
	if (!loaded) return;
	
	Asset::User user;
	loaded = RedisInstance.GetUser(player.account(), user);
	if (!loaded) return; //没有账号数据

	std::string nickname = player.common_prop().name();
	if (user.wechat().nickname().size()) nickname = user.wechat().nickname();

	auto member_ptr = _stuff.mutable_member_list()->Add();
	member_ptr->set_player_id(player_id);
	member_ptr->set_name(nickname); //昵称
	member_ptr->set_headimgurl(user.wechat().headimgurl()); //头像
	member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_AVAILABLE); //状态

	if (player.login_time() == 0) //离线
	{
		player.add_clan_joiners(_clan_id);
		PlayerInstance.Save(player_id, player); //直接存盘
	}

	DEBUG("在茶馆:{} 中增加成员:{} 信息:{} 成员数据:{} 成功", _clan_id, player_id, member_ptr->ShortDebugString(), player.ShortDebugString());
	
	_dirty = true;
}
	
int32_t Clan::OnDisAgree(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return Asset::ERROR_INNER;
	
	auto member_id = message->dest_player_id();
	auto dest_sys_message_index = _stuff.message_list().size() - message->dest_sys_message_index();
	
	if (dest_sys_message_index < 0 || dest_sys_message_index > _stuff.message_list().size()) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录

	//
	//申请列表状态更新
	//

	const auto& sys_message = _stuff.message_list(dest_sys_message_index);
	if (member_id != sys_message.player_id() || Asset::CLAN_OPER_TYPE_JOIN != sys_message.oper_type()) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录

	auto it = _stuff.mutable_message_list(dest_sys_message_index);
	if (!it) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录

	it->set_oper_time(TimerInstance.GetTime());
	it->set_oper_type(message->oper_type());
	
	OnSetUpdateTime(); //更新操作时间

	_dirty = true;

	return 0;
}

int32_t Clan::OnChangedInformation(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return Asset::ERROR_INNER;

	if (_stuff.hoster_id() != player->GetID()) return Asset::ERROR_CLAN_NO_PERMISSION;
			

	auto name = message->name();
	
	if (name.size()) 
	{
		auto result = ClanInstance.IsNameValid(message->name(), message->name());
		if (result) return result; //名字检查失败

		CommonUtil::Trim(name);
		_stuff.set_name(name);
	}

	auto announcement = message->announcement();

	if (announcement.size()) 
	{
		CommonUtil::Trim(announcement);

		auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
		if (!clan_limit) return Asset::ERROR_CLAN_ANNOUCEMENT_INVALID;
	
		if ((int32_t)announcement.size() > clan_limit->annoucement_limit()) return Asset::ERROR_CLAN_ANNOUCEMENT_INVALID; //字数限制

		if (!NameLimitInstance.IsValid(announcement)) return Asset::ERROR_CLAN_ANNOUCEMENT_INVALID;

		_stuff.set_announcement(announcement);
	}

	_dirty = true;

	return 0;
}

void Clan::OnQueryMemberStatus(Asset::ClanOperation* message)
{
	std::lock_guard<std::mutex> lock(_member_mutex);

	auto online_mem_count = _stuff.online_mem_count(); //当前在线人数
	_stuff.set_online_mem_count(0); //在线成员数量缓存

	for (int32_t i = 0; i < _stuff.member_list().size(); ++i)
	{
		auto member_ptr = _stuff.mutable_member_list(i);
		if (!member_ptr) continue;

		member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_AVAILABLE);
	
		Asset::Player player;
		auto loaded = PlayerInstance.GetCache(member_ptr->player_id(), player);
		if (!loaded) continue;
		
		Asset::User user;
		loaded = RedisInstance.GetUser(player.account(), user);
		if (!loaded) continue;

		member_ptr->set_headimgurl(user.wechat().headimgurl()); //头像信息

		if (player.login_time()) //在线
		{
			if (player.room_id()) member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_GAMING); //游戏中
		}
		else if (player.logout_time())
		{
			member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_OFFLINE); //离线
		}

		//当玩家加入了两个俱乐部的时候在一个俱乐部玩的情况下另一个俱乐部应该显示他离线 
		//
		//不应该是游戏中离线
		if (player.selected_clan_id() != _clan_id)
			member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_OFFLINE); 

		if (member_ptr->status() == Asset::CLAN_MEM_STATUS_TYPE_AVAILABLE || member_ptr->status() == Asset::CLAN_MEM_STATUS_TYPE_GAMING) 
			_stuff.set_online_mem_count(_stuff.online_mem_count() + 1); //在线成员数量缓存
	}

	if (message) message->mutable_clan()->CopyFrom(_stuff);

	if (online_mem_count != _stuff.online_mem_count()) _dirty = true; //状态更新，减少存盘频率
}
	
const std::unordered_map<int64_t, Asset::RoomQueryResult>& Clan::GetRooms()
{
	std::lock_guard<std::mutex> lock(_mutex);

	return _rooms;
}

int32_t Clan::GetRoomGamingCount()
{
	std::lock_guard<std::mutex> lock(_mutex);

	int32_t gaming_count = 0;

	for (const auto& room : _rooms)
	{
		if (room.second.curr_count()) ++gaming_count; //进行中房间//已开局房间
	}

	return gaming_count;
}

int32_t Clan::GetRoomNoPlayedCount()
{
	std::lock_guard<std::mutex> lock(_mutex);

	int32_t room_no_played_count = 0;

	for (const auto& room : _rooms)
	{
		if (room.second.curr_count() == 0) ++room_no_played_count; //开房尚未开局的房间
	}

	return room_no_played_count;
}

//
//茶馆当前牌局列表查询
//
//Client根据列表查询详细数据
//
void Clan::OnQueryRoomList(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!player || !message) return;

	size_t room_query_start_index = message->query_start_index() - 1;
	if (room_query_start_index < 0 || room_query_start_index >= _gaming_room_list.size()) return;

	size_t room_query_end_index = message->query_end_index() - 1;
	if (room_query_end_index < 0 || room_query_end_index >= _gaming_room_list.size()) return;

	for (size_t i = room_query_start_index; i <= room_query_end_index; ++i)
	{
		auto it = _rooms.find(_gaming_room_list[i]);
		if (it == _rooms.end()) continue;
	
		auto room_battle = message->mutable_room_list()->Add();
		room_battle->CopyFrom(it->second);
	}
}

void Clan::OnQueryGamingList(Asset::ClanOperation* message)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!message) return;

	for (auto room_id : _gaming_room_list) message->add_room_gaming_list(room_id);

	DEBUG("茶馆:{} 房间列表查询:{}", _clan_id, message->ShortDebugString());
}

void Clan::Save(bool force)
{
	if (!force && !_dirty) return;

	if (!ClanInstance.IsLocal(_clan_id)) return; //不是本服俱乐部，不进行存储

	DEBUG("存储茶馆:{} 数据:{} 成功", _clan_id, _stuff.ShortDebugString());
		
	RedisInstance.Save("ddz_clan:" + std::to_string(_clan_id), _stuff);

	_dirty = false;
}

void Clan::OnDisMiss()
{
	Asset::ClanOperation message;
	message.set_clan_id(_clan_id);
	message.set_oper_type(Asset::CLAN_OPER_TYPE_DISMISS);

	BroadCast(message); //解散成功

	_stuff.set_dismiss(true); //解散

	_dirty = true;
}

void Clan::OnSetUpdateTime()
{
	_stuff.set_sys_messsage_update_time(TimerInstance.GetTime());

	_dirty = true;
}

int32_t Clan::RemoveMember(int64_t player_id, Asset::ClanOperation* message)
{
	if (!message) return Asset::ERROR_CLAN_NO_MEM;
	
	std::lock_guard<std::mutex> lock(_member_mutex);

	std::string player_name;

	int32_t curr_mem_count = _stuff.member_list().size();

	for (int32_t i = 0; i < _stuff.member_list().size(); ++i)
	{
		if (player_id != _stuff.member_list(i).player_id()) continue;

		player_name = _stuff.member_list(i).name();

		_stuff.mutable_member_list()->SwapElements(i, _stuff.member_list().size() - 1);
		_stuff.mutable_member_list()->RemoveLast(); //删除玩家

		break; //删除一个即可
	}
	
	if (curr_mem_count == _stuff.member_list().size()) return Asset::ERROR_CLAN_NO_MEM;
	
	//列表状态更新
	//
	if (Asset::CLAN_OPER_TYPE_MEMEBER_QUIT == message->oper_type())
	{
		auto system_message = _stuff.mutable_message_list()->Add();
		system_message->set_player_id(player_id);
		system_message->set_name(player_name);
		system_message->set_oper_time(TimerInstance.GetTime());
		system_message->set_oper_type(message->oper_type());
	}

	Asset::Player player;
	bool loaded = PlayerInstance.GetCache(player_id, player);
	if (!loaded) return Asset::ERROR_CLAN_NO_MEM;

	if (player.login_time() == 0) //离线:直接从茶馆删除
	{
		/*
		for (int32_t i = 0; i < player.clan_hosters().size(); ++i) //茶馆老板
		{
			if (_clan_id != player.clan_hosters(i)) continue;

			player.mutable_clan_hosters()->SwapElements(i, player.clan_hosters().size() - 1);
			player.mutable_clan_hosters()->RemoveLast();
		}
		*/

		for (int32_t i = 0; i < player.clan_joiners().size(); ++i) //茶馆成员
		{
			if (_clan_id == player.clan_joiners(i)) 
			{
				player.mutable_clan_joiners()->SwapElements(i, player.clan_joiners().size() - 1);
				player.mutable_clan_joiners()->RemoveLast();
			
				break;
			}
		}

		if (_clan_id == player.selected_clan_id()) player.set_selected_clan_id(0); //清理当前选择的茶馆
		
		PlayerInstance.Save(player_id, player); //直接存盘
	}
	else //在线 
	{
		PlayerInstance.SendProtocol2GameServer(player_id, message); //发给另一个中心服处理
	}

	message->mutable_clan()->CopyFrom(_stuff); //更新列表
	
	OnSetUpdateTime(); //更新操作时间

	DEBUG("茶馆:{} 删除成员:{} 协议:{} 成功", _clan_id, player_id, message->ShortDebugString());
		
	_dirty = true;

	return 0;
}

void Clan::BroadCast(const pb::Message* message)
{
	if (!message) return;

	BroadCast(*message);
}

void Clan::BroadCast(const pb::Message& message)
{
	std::lock_guard<std::mutex> lock(_member_mutex);

	for (const auto& member : _stuff.member_list())
	{
		auto member_ptr = PlayerInstance.Get(member.player_id());
		if (!member_ptr) continue;

		member_ptr->SendProtocol(message);
	}
}
	
bool Clan::CheckRoomCard(int32_t count)
{
	return _stuff.room_card_count() >= count;
}

void Clan::ConsumeRoomCard(int32_t count)
{
	if (count <= 0) return;

	_stuff.set_room_card_count(_stuff.room_card_count() - count);
	_dirty = true;
}

void Clan::AddRoomCard(int32_t count)
{
	if (count <= 0) return;

	_stuff.set_room_card_count(_stuff.room_card_count() + count);
	_dirty = true;
}
	
void Clan::OnRoomChanged(const Asset::ClanRoomStatusChanged* message)
{
	if (!message) return;

	switch (message->status())
	{
		case Asset::CLAN_ROOM_STATUS_TYPE_START:
		{
			const auto room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
			if (!room_card || room_card->rounds() <= 0) return;

			auto consume_count = message->room().options().open_rands() / room_card->rounds(); //待消耗房卡数
			if (consume_count <= 0) return;

			if (!CheckRoomCard(consume_count)) 
			{
				LOG(ERROR, "茶馆:{} 房间消耗房卡失败,然而已经开局,数据:{}", _clan_id, message->ShortDebugString());
				return;
			}

			ConsumeRoomCard(consume_count);
		}
		break;
		
		case Asset::CLAN_ROOM_STATUS_TYPE_OVER:
		{
			OnRoomOver(message);
		}
		break;
	}

	DEBUG("茶馆:{} 房间变化:{}", _clan_id, message->ShortDebugString());

	_dirty = true;
}
	
void Clan::OnRoomOver(const Asset::ClanRoomStatusChanged* message)
{
	if (!message) return;

	const auto& room = message->room();
	auto room_id = room.room_id();

	std::lock_guard<std::mutex> lock(_mutex);

	_rooms.erase(room_id);

	auto it = std::find(_gaming_room_list.begin(), _gaming_room_list.end(), room_id);
	if (it != _gaming_room_list.end()) _gaming_room_list.erase(it);

	auto it_ = std::find_if(_stuff.battle_history().begin(), _stuff.battle_history().end(), [room_id](const Asset::Clan_RoomHistory& history){
				return room_id == history.room_id();
			});
	if (it_ != _stuff.battle_history().end()) return; //已经存在记录

	if (message->games_count())
	{
		auto history = _stuff.mutable_battle_history()->Add(); //历史战绩
		history->set_room_id(room_id);
		history->set_battle_time(message->created_time());
		history->mutable_player_list()->CopyFrom(message->player_list());
	}
	
	//
	//删除过期的历史战绩
	//
	auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
	if (!clan_limit) return;
	
	auto curr_time = TimerInstance.GetTime();
	std::vector<Asset::Clan_RoomHistory> room_histry;

	for (const auto& history : _stuff.battle_history())
	{
		auto battle_time = history.battle_time();
		if (battle_time + clan_limit->room_history_last_day() * 24 * 3600 < curr_time) continue; //过期

		room_histry.push_back(history);
	}

	_stuff.mutable_battle_history()->Clear();

	for (const auto& history : room_histry)
	{
		auto history_ptr = _stuff.mutable_battle_history()->Add();
		history_ptr->CopyFrom(history);
	}

	DEBUG("茶馆:{} 房间:{} 结束，删除", _clan_id, room_id);
	
	_dirty = true;
}

void Clan::OnRoomSync(const Asset::RoomQueryResult& room_query)
{
	std::lock_guard<std::mutex> lock(_mutex);

	auto room_id = room_query.room_id();
	_rooms[room_id] = room_query;

	auto it = std::find(_gaming_room_list.begin(), _gaming_room_list.end(), room_id);
	if (it == _gaming_room_list.end()) _gaming_room_list.push_back(room_id);

	_dirty = true;
}

void ClanManager::Update(int32_t diff)
{
	++_heart_count; //心跳

	if (_heart_count % 60 != 0) return;  //3秒

	//std::lock_guard<std::mutex> lock(_mutex);
	
	for (auto it = _clans.begin(); it != _clans.end();)
	{
		if (!it->second)
		{
			it = _clans.erase(it);
			continue; 
		}
		else
		{
			it->second->Update();
			++it;
		}
	}

	Load();
}
	
void ClanManager::Load()
{
	if (_loaded) return;

	std::vector<std::string> clan_list;
	bool has_record = RedisInstance.GetArray("clan:*", clan_list);	
	if (!has_record)
	{
		WARN("加载茶馆数据失败，可能是本服尚未创建茶馆，加载成功数量:{}", clan_list.size());
		return;
	}

	for (const auto& value : clan_list)
	{
		Asset::Clan clan;
		auto success = RedisInstance.Get(value, clan);
		if (!success) continue;

		if (clan.dismiss()) continue; //解散

		auto clan_ptr = std::make_shared<Clan>(clan);
		if (!clan_ptr) return;

		clan_ptr->OnLoaded();

		Emplace(clan.clan_id(), clan_ptr);
	}
		
	DEBUG("加载茶馆数据成功，加载成功数量:{}", _clans.size());

	_loaded = true;
}

void ClanManager::Remove(int64_t clan_id)
{
	WARN("删除茶馆:{}", clan_id);

	std::lock_guard<std::mutex> lock(_mutex);

	if (clan_id <= 0) return;

	auto it = _clans.find(clan_id);
	if (it == _clans.end()) return;
		
	if (it->second) 
	{
		it->second->OnDisMiss(); //解散
		it->second->Save();
		it->second.reset();
	}

	_clans.erase(it);
}

void ClanManager::Remove(std::shared_ptr<Clan> clan)
{
	if (!clan) return;

	Remove(clan->GetID());
}

void ClanManager::Emplace(int64_t clan_id, std::shared_ptr<Clan> clan)
{
	if (clan_id <= 0 || !clan) return;

	std::lock_guard<std::mutex> lock(_mutex);

	_clans[clan_id] = clan;

	DEBUG("添加茶馆:{} 成功，当前茶馆数量:{}", clan_id, _clans.size());
}

std::shared_ptr<Clan> ClanManager::GetClan(int64_t clan_id)
{
	std::lock_guard<std::mutex> lock(_mutex);

	auto it = _clans.find(clan_id);
	if (it == _clans.end()) return nullptr;

	if (it->second->HasDismiss()) return nullptr; //已解散

	return it->second;
}

std::shared_ptr<Clan> ClanManager::Get(int64_t clan_id)
{
	return GetClan(clan_id);
}

void ClanManager::OnOperate(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!message || !player) return;
	
	if (message->server_id() == g_server_id) //理论上[server_id]是默认值，不会赋值
	{
		WARN("服务器:{} 收到茶馆操作数据:{}, g_server_id, message->ShortDebugString()"); //收到本服发送的协议数据，不再处理
		return;
	}
	
	static std::set<int32_t> _valid_operation = { Asset::CLAN_OPER_TYPE_CREATE, Asset::CLAN_OPER_TYPE_CLAN_LIST_QUERY, Asset::CLAN_OPER_TYPE_RECHARGE }; //合法
	
	defer 
	{
		if (!message) return;

		if (message->oper_result() == 0 && _valid_operation.find(message->oper_type()) != _valid_operation.end()) return;  //不进行协议转发

		player->SendProtocol(message); //返回结果
	};
			
	std::shared_ptr<Clan> clan = nullptr;
	
	if (message->oper_type() != Asset::CLAN_OPER_TYPE_CREATE && message->oper_type() != Asset::CLAN_OPER_TYPE_CLAN_LIST_QUERY) 
	{
		if (message->clan_id() <= 0) return;

		clan = ClanInstance.Get(message->clan_id());

		if (!clan) //创建茶馆//列表查询无需检查
		{
			message->set_oper_result(Asset::ERROR_CLAN_NOT_FOUND); //没找到茶馆
			return;
		}
	}

	message->set_server_id(g_server_id);

	switch (message->oper_type())
	{
		case Asset::CLAN_OPER_TYPE_CREATE: //创建
		{
			/*
			auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
			if (!clan_limit) return;

			auto trim_name = message->name();
			CommonUtil::Trim(trim_name);

			if (trim_name.size() != message->name().size()) //有空格
			{
				message->set_oper_result(Asset::ERROR_CLAN_NAME_INVALID);
				return;
			}

			if (trim_name.empty()) 
			{
				message->set_oper_result(Asset::ERROR_CLAN_NAME_EMPTY);
				return;
			}
			if ((int32_t)trim_name.size() > clan_limit->name_limit())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NAME_UPPER);
				return;
			}
			if (!NameLimitInstance.IsValid(trim_name))
			{
				message->set_oper_result(Asset::ERROR_CLAN_NAME_INVALID);
				return;
			}
			*/

			auto result = ClanInstance.IsNameValid(message->name(), message->name());
			if (result)
			{
				message->set_oper_result(result);
				return;
			}

			player->SendProtocol2GameServer(message); //到逻辑服务器进行检查
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_JOIN: //申请加入
		{
			auto result = clan->OnApply(player->GetID(), message); 
			message->set_oper_result(result); 

			if (result == 0) //申请成功
			{
				if (!IsLocal(message->clan_id())) //本服玩家申请加入另一个服的茶馆
				{
					player->SendProtocol2GameServer(message); //发给另一个中心服处理
					return; //不是本服
				}

				auto hoster_id = clan->GetHoster();
				auto hoster_ptr = PlayerInstance.Get(hoster_id);
				if (!hoster_ptr) return;

				auto proto = *message;
				proto.mutable_clan()->CopyFrom(clan->Get());
				hoster_ptr->SendProtocol(proto); //通知茶馆老板
			}
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_EDIT: //修改基本信息
		{
			auto result = clan->OnChangedInformation(player, message);
			message->set_oper_result(result); 
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_DISMISS: //解散
		{
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return;
			}

			if (clan->HasGaming()) //牌局进行中，不能解散
			{
				message->set_oper_result(Asset::ERROR_CLAN_DISMISS_GAMING);
				return;
			}

			Remove(message->clan_id());

			message->set_recharge_count(clan->GetRoomCard());
			player->SendProtocol2GameServer(message); //通知逻辑服务器解散
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_AGEE: //同意加入
		{
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return;
			}
			
			auto result = clan->OnAgree(message); //列表更新
			message->set_oper_result(result); 

			if (result != 0) return; //失败
				
			auto des_player = PlayerInstance.Get(message->dest_player_id()); //不在当前中心服务器
			if (!des_player) return;

			message->mutable_clan()->CopyFrom(clan->Get()); //茶馆信息

			//des_player->SendProtocol(message); //通知玩家馆长同意加入茶馆
			des_player->SendProtocol2GameServer(message); //通知逻辑服务器加入茶馆成功
			
			//player->SendProtocol2GameServer(message); //通知逻辑服务器加入成功
		
			/*
			Asset::Player des_player;
			if (!PlayerInstance.GetCache(message->dest_player_id(), des_player)) return; //没有记录

			if (des_player.login_time() == 0) //离线
			{
				des_player.add_clan_joiners(message->clan_id());

				PlayerInstance.Save(message->dest_player_id(), des_player); //直接存盘
			}
			else //在线 
			{
				auto des_player = PlayerInstance.Get(message->dest_player_id()); //不在当前中心服务器
				if (!des_player) return;

				message->mutable_clan()->CopyFrom(clan->Get()); //茶馆信息

				//des_player->OnClanJoin(message->clan_id());
				des_player->SendProtocol(message); //通知玩家馆长同意加入茶馆
				des_player->SendProtocol2GameServer(message); //通知逻辑服务器加入茶馆成功
			}
			*/
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_DISAGEE: //拒绝加入
		{
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return;
			}
			
			auto result = clan->OnDisAgree(player, message);
			message->set_oper_result(result); 

			if (result) return; //非成功操作不进行转发
	
			auto des_player = PlayerInstance.Get(message->dest_player_id());
			if (!des_player) return;
				
			des_player->SendProtocol(message); //通知玩家馆长拒绝加入茶馆
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_DELETE: //删除成员
		{
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION); //权限检查
				return;
			}
			
			auto result = clan->RemoveMember(message->dest_player_id(), message);
			message->set_oper_result(result); 

			if (result) return; //删除失败
			
			auto des_player = PlayerInstance.Get(message->dest_player_id());
			if (!des_player) return;
				
			des_player->SendProtocol(message); //通知玩家馆长删除
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_QUIT: //主动退出
		{
			if (clan->GetHoster() == player->GetID()) //老板不能退出
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION); //权限检查
				return;
			}

			auto result = clan->RemoveMember(player->GetID(), message);
			message->set_dest_player_id(player->GetID());
			message->set_oper_result(result); 

			auto hoster_ptr = PlayerInstance.Get(clan->GetHoster());
			if (!hoster_ptr) return;

			hoster_ptr->SendProtocol(message);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_RECHARGE: //充值
		{
			if (clan->GetHoster() != player->GetID()) //非老板不能充值
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION); //权限检查
				return;
			}

			player->SendProtocol2GameServer(message); //到逻辑服务器进行检查
		}
		break;

		case Asset::CLAN_OPER_TYPE_MEMEBER_QUERY: //成员状态查询
		{
			clan->OnQueryMemberStatus(message);

			if (!clan->HasMember(player->GetID())) //不是成员不能查询
			{
				message->set_oper_result(Asset::ERROR_CLAN_QUERY_NO_CLAN);
				return; //不是成员
			}
	
			player->SendProtocol2GameServer(message); //到逻辑服务器进行同步当前茶馆
		}
		break;

		case Asset::CLAN_OPER_TYPE_ROOM_LIST_QUERY:
		{
			clan->OnQueryRoomList(player, message);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_CLAN_LIST_QUERY:
		{
			player->SendProtocol2GameServer(message);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_ROOM_GAMING_LIST_QUERY:
		{
			clan->OnQueryGamingList(message);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_UPDATE_SYSTEM_MESSAGE:
		{
			if (clan->GetHoster() != player->GetID()) //非老板不能设置
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION); //权限检查
				return;
			}
			clan->OnSetUpdateTime();
		}
		break;

		default:
		{
			ERROR("玩家:{} 茶馆操作尚未处理:{}", player->GetID(), message->ShortDebugString());
			return;
		}
		break;
	}
}
	
int32_t ClanManager::IsNameValid(std::string name, std::string trim_name)
{
	auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
	if (!clan_limit) return Asset::ERROR_CLAN_NAME_INVALID;

	CommonUtil::Trim(trim_name);

	if (trim_name.size() != name.size()) return Asset::ERROR_CLAN_NAME_INVALID;

	if (trim_name.empty()) return Asset::ERROR_CLAN_NAME_EMPTY;

	if ((int32_t)trim_name.size() > clan_limit->name_limit()) return Asset::ERROR_CLAN_NAME_UPPER;

	if (!NameLimitInstance.IsValid(trim_name)) return Asset::ERROR_CLAN_NAME_INVALID;

	return 0;
}
	
void ClanManager::OnCreated(int64_t clan_id, std::shared_ptr<Clan> clan)
{
	if (clan_id <= 0 || !clan) return;

	clan->AddMember(clan->GetHoster()); //成员
		
	Emplace(clan_id, clan);

	DEBUG("创建茶馆:{} 成功", clan_id);
}
	
void ClanManager::OnGameServerBack(const Asset::ClanOperationSync& message)
{
	auto operation = message.operation();
	if (operation.oper_result() != 0) return; //执行失败

	//if (g_server_id == operation.server_id()) return; //本服不再处理

	auto clan_id = operation.clan_id();
	
	switch (operation.oper_type())
	{
		case Asset::CLAN_OPER_TYPE_CREATE: //创建
		{
			auto clan_ptr = std::make_shared<Clan>(operation.clan());
			clan_ptr->Save(true); //存盘

			OnCreated(clan_id, clan_ptr); //创建成功
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_JOIN: //申请加入
		{
			auto clan = Get(clan_id);
			if (!clan) return;
		
			auto oper = message.operation();
			auto result = clan->OnApply(message.player_id(), &oper); 
			
			if (result == 0) //申请成功
			{
				auto hoster_id = clan->GetHoster();
				auto hoster_ptr = PlayerInstance.Get(hoster_id);
				if (!hoster_ptr) return;

				oper.mutable_clan()->CopyFrom(clan->Get());
				hoster_ptr->SendProtocol(oper); //通知茶馆老板
			}
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_RECHARGE: //充值
		{
			auto clan_ptr = Get(clan_id);
			if (!clan_ptr) return;

			clan_ptr->OnRecharge(message.operation().recharge_count());

			auto player = PlayerInstance.Get(clan_ptr->GetHoster());
			if (!player) return;

			operation.set_recharge_count(clan_ptr->GetRoomCard());
			player->SendProtocol(operation);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_DISMISS: //解散
		{
			Remove(clan_id);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_AGEE: //同意加入
		{
			auto clan_ptr = Get(clan_id);
			if (!clan_ptr) return;

			auto result = clan_ptr->OnAgree(&operation); //列表更新
			operation.set_oper_result(result); 

			if (result) return; //不能加入则不通知申请者

			auto des_player = PlayerInstance.Get(operation.dest_player_id()); //不在当前中心服务器
			if (!des_player) return;

			operation.mutable_clan()->CopyFrom(clan_ptr->Get()); //茶馆信息

			//des_player->OnClanJoin(operation.clan_id());
			des_player->SendProtocol(operation); //通知玩家馆长同意加入茶馆
			des_player->SendProtocol2GameServer(operation); //通知逻辑服务器加入茶馆成功
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_QUIT: //主动退出
		{
			auto clan_ptr = Get(clan_id);
			if (!clan_ptr) return;

			clan_ptr->RemoveMember(operation.dest_player_id(), &operation); //复用删除玩家变量^_^
		}
		break;

		default:
		{
			if (ClanInstance.IsLocal(clan_id)) return; //本地数据不再加载

			WARN("茶馆:{} 尚未处理回调接口:{}", clan_id, operation.ShortDebugString());

			auto clan_ptr = Get(clan_id);
			if (!clan_ptr) return;

			clan_ptr->Load();
		}
		break;
	}

	DEBUG("接收逻辑服务器返回茶馆操作数据:{}", operation.ShortDebugString());
}
	
bool ClanManager::IsLocal(int64_t clan_id)
{
	int64_t server_id = clan_id >> 16;
	return server_id == g_server_id;
}

bool ClanManager::GetClan(int64_t clan_id, Asset::Clan& clan)
{
	return RedisInstance.Get("ddz_clan:" + std::to_string(clan_id), clan);
}

}

