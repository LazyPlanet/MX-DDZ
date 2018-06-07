#include <string>

#include "Clan.h"
#include "Timer.h"
#include "Player.h"
#include "RedisManager.h"
#include "NameLimit.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

/*
void Clan::Update()
{
	if (_dirty) Save();
}

int32_t Clan::OnApply(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return Asset::ERROR_INNER;
	
	auto player_id = player->GetID();
	auto oper_type = message->oper_type();

	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [player_id](const Asset::SystemMessage& message){
				return player_id == message.player_id();	
			});

	if (it == _stuff.mutable_message_list()->end())
	{
		auto system_message = _stuff.mutable_message_list()->Add();
		system_message->set_player_id(player_id);
		system_message->set_name(player->GetName());
		system_message->set_oper_time(TimerInstance.GetTime());
		system_message->set_oper_type(oper_type);
	}
	else
	{
		it->set_oper_time(TimerInstance.GetTime());
		it->set_oper_type(oper_type);
	}

	message->set_oper_result(Asset::ERROR_SUCCESS);
	player->SendProtocol(message);

	_dirty = true;
	return 0;
}


int32_t Clan::OnRecharge(std::shared_ptr<Player> player, int32_t count)
{
	if (!player || count <= 0) return Asset::ERROR_INNER;
			
	if (player->GetRoomCard() < count) return Asset::ERROR_CLAN_ROOM_CARD_NOT_ENOUGH; //房卡不足

	player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_RECHARGE_CLAN, count); //扣除馆长房卡
	return 0;
}

int32_t Clan::OnAgree(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return Asset::ERROR_INNER;
	
	auto member_id = message->dest_player_id();
	auto oper_type = message->oper_type();

	//
	//申请列表状态更新
	//
	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [member_id](const Asset::SystemMessage& message){
				return member_id == message.player_id();	
			});

	if (it == _stuff.mutable_message_list()->end()) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录
	
	if (oper_type == it->oper_type()) return Asset::ERROR_SUCCESS; //状态一致

	it->set_oper_time(TimerInstance.GetTime());
	it->set_oper_type(oper_type);

	//
	//成员列表更新
	//
	auto it_ = std::find_if(_stuff.mutable_member_list()->begin(), _stuff.mutable_member_list()->end(), [member_id](const Asset::Clan_Member& member){
				return member_id == member.player_id();
			});

	if (it_ != _stuff.mutable_member_list()->end()) return Asset::ERROR_CLAN_HAS_JOINED; //已经是成员

	auto member_ptr = _stuff.mutable_member_list()->Add();
	member_ptr->set_player_id(it->player_id());
	member_ptr->set_name(it->name());
	member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_AVAILABLE);

	message->set_oper_result(Asset::ERROR_SUCCESS);
	player->SendProtocol(message);

	_dirty = true;
	return 0;
}
	
int32_t Clan::OnDisAgree(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return Asset::ERROR_INNER;
	
	auto member_id = message->dest_player_id();
	auto oper_type = message->oper_type();

	//
	//申请列表状态更新
	//
	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [member_id](const Asset::SystemMessage& message){
				return member_id == message.player_id();	
			});

	if (it == _stuff.mutable_message_list()->end()) return Asset::ERROR_CLAN_NO_RECORD; //尚未申请记录
	
	if (oper_type == it->oper_type()) return Asset::ERROR_SUCCESS; //状态一致

	it->set_oper_time(TimerInstance.GetTime());
	it->set_oper_type(oper_type);

	_dirty = true;
	return 0;
}

int32_t Clan::OnChangedInformation(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return Asset::ERROR_INNER;

	auto hoster_id = _stuff.hoster_id();

	if (hoster_id != player->GetID()) return Asset::ERROR_CLAN_NO_PERMISSION;

	auto name = message->name();
	auto announcement = message->announcement();

	if (name.size()) _stuff.set_name(name);
	if (announcement.size()) _stuff.set_name(announcement);

	_dirty = true;
	return 0;
}

void Clan::OnQueryMemberStatus(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;

	for (int32_t i = 0; i < _stuff.member_list().size(); ++i)
	{
		auto member_ptr = _stuff.mutable_member_list(i);
		if (!member_ptr) continue;

		member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_AVAILABLE);
	
		Asset::Player stuff;

		auto loaded = RedisInstance.Get("player:" + std::to_string(member_ptr->player_id()), stuff);
		if (!loaded) continue;

		if (stuff.login_time()) //在线
		{
			if (stuff.room_id()) member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_GAMING); //游戏中
		}
		else if (stuff.logout_time())
		{
			member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_OFFLINE); //离线
		}
	}

	message->mutable_clan()->CopyFrom(_stuff);

	_dirty = true;
}
*/

//
//茶馆当前牌局列表查询
//
//Client根据列表查询详细数据
//
/*
void Clan::OnQueryRoomList(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;

	auto room_query_start_index = message->room_query_start_index();
	if (room_query_start_index < 0 || room_query_start_index >= _stuff.room_list().size()) return;

	auto room_query_end_index = message->room_query_end_index();
	if (room_query_end_index < 0 || room_query_end_index >= _stuff.room_list().size()) return;

	for (int32_t i = room_query_start_index; i < room_query_end_index; ++i)
	{
		auto it = _rooms.find(_stuff.room_list(i));
		if (it == _rooms.end()) continue;
	
		auto room_battle = message->mutable_room_list()->Add();
		room_battle->CopyFrom(it->second);
	}
}
*/

/*
void Clan::Save(bool force)
{
	if (!force && !_dirty) return;
		
	RedisInstance.Save("clan:" + std::to_string(_clan_id), _stuff);

	_dirty = false;
}

void Clan::OnDisMiss()
{
	_stuff.set_dismiss(true); //解散

	_dirty = true;
}

void Clan::RemoveMember(int64_t player_id)
{
	for (int32_t i = 0; i < _stuff.member_list().size(); ++i)
	{
		if (player_id != _stuff.member_list(i).player_id()) continue;

		_stuff.mutable_member_list()->SwapElements(i, _stuff.member_list().size() - 1);
		_stuff.mutable_member_list()->RemoveLast(); //删除玩家
		break;
	}

	_dirty = true;
}

void Clan::BroadCast(const pb::Message* message)
{
	if (!message) return;

	BroadCast(*message);
}

void Clan::BroadCast(const pb::Message& message)
{
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
	if (count >= 0) return;

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
				LOG(ERROR, "茶馆房间消耗房卡失败,然而已经开局,数据:{}", message->ShortDebugString());
				return;
			}

			ConsumeRoomCard(consume_count);
		}
		break;
		
		case Asset::CLAN_ROOM_STATUS_TYPE_OVER:
		{
			_rooms.erase(message->room().room_id());

			for (int32_t i = 0; i < _stuff.room_list().size(); ++i)
			{
				auto room_id = _stuff.room_list(i);
				if (room_id != message->room().room_id()) continue;

				_stuff.mutable_room_list()->SwapElements(i, _stuff.room_list().size() - 1);
				_stuff.mutable_room_list()->RemoveLast();
				break;
			}
		}
		break;
	}

	_dirty = true;
}

void Clan::OnRoomSync(const Asset::RoomQueryResult& room_query)
{
	auto room_id = room_query.room_id();
	if (room_id <= 0) return;

	_rooms[room_id] = room_query;

	auto it = std::find(_stuff.room_list().begin(), _stuff.room_list().end(), room_id);
	if (it == _stuff.room_list().end()) _stuff.mutable_room_list()->Add(room_id);

	_dirty = true;
}

void ClanManager::Update(int32_t diff)
{
	if (_heart_count % 60 != 0) return;  //3秒

	++_heart_count;

	std::lock_guard<std::mutex> lock(_mutex);
	
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

}
	
void ClanManager::Load()
{
	if (_loaded) return;

	std::vector<std::string> clan_list;
	bool has_record = RedisInstance.GetArray("clan:*", clan_list);	
	if (!has_record)
	{
		ERROR("加载茶馆数据失败，加载成功数量:{}", clan_list.size());
		return;
	}

	for (const auto& value : clan_list)
	{
		Asset::Clan clan;
		auto success = clan.ParseFromString(value);
		if (!success) continue;

		auto clan_ptr = std::make_shared<Clan>(clan);
		if (!clan_ptr) return;

		Emplace(clan.clan_id(), clan_ptr);
	}

	_loaded = true;
}

void ClanManager::Remove(int64_t clan_id)
{
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

	_clans[clan_id] = clan;

	DEBUG("添加茶馆:{} 成功，当前茶馆数量:{}", clan_id, _clans.size());
}

std::shared_ptr<Clan> ClanManager::GetClan(int64_t clan_id)
{
	auto it = _clans.find(clan_id);
	if (it == _clans.end()) return nullptr;

	return it->second;
}

std::shared_ptr<Clan> ClanManager::Get(int64_t clan_id)
{
	return GetClan(clan_id);
}
*/
	
void Clan::OnJoinMatch(std::shared_ptr<Player> player, Asset::JoinMatch* message)
{

}

void ClanManager::OnOperate(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!message || !player) return;

	DEBUG("服务器:{} 接收来自中心服务器的玩家:{} 茶馆操作结果:{}", g_server_id, player->GetID(), message->ShortDebugString());
		
	static std::set<int32_t> _valid_operation = { Asset::CLAN_OPER_TYPE_CREATE, Asset::CLAN_OPER_TYPE_MEMEBER_AGEE, Asset::CLAN_OPER_TYPE_CLAN_LIST_QUERY }; //合法
	
	defer {
		if (!message) return;

		if (_valid_operation.find(message->oper_type()) != _valid_operation.end()) player->SendProtocol(message); //返回结果

		//OnResult(message); //执行成功：广播执行结果
	};
			
	/*
	std::shared_ptr<Clan> clan = nullptr;
	
	if (message->oper_type() != Asset::CLAN_OPER_TYPE_CREATE) 
	{
		clan = ClanInstance.Get(message->clan_id());

		if (!clan) //只有创建茶馆无需检查
		{
			message->set_oper_result(Asset::ERROR_CLAN_NOT_FOUND); //没找到茶馆
			return 2;
		}
	}
	*/

	switch (message->oper_type())
	{
		case Asset::CLAN_OPER_TYPE_CREATE: //创建
		{
			auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
			if (!clan_limit) return;

			const auto& trim_name = message->name();
			
			/*
			boost::trim(trim_name);

			if (trim_name.size() != message->name().size())
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

			if (clan_limit->create_daili_limit() && !player->IsDaili())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION); //非代理不能创建茶馆
				return;
			}

			if (player->GetHosterCount() > clan_limit->create_upper_limit())
			{
				message->set_oper_result(Asset::ERROR_CLAN_HOSTER_UPPER);
				return;
			}
	
			if (player->GetRoomCard() < clan_limit->room_card_limit())
			{
				message->set_oper_result(Asset::ERROR_CLAN_ROOM_CARD_NOT_ENOUGH); //房卡不足
				return;
			}
			
			int64_t clan_id = RedisInstance.CreateClan();
			if (clan_id == 0)
			{
				message->set_oper_result(Asset::ERROR_CLAN_CREATE_INNER);
				return;
			}

			clan_id = (message->server_id() << 16) + clan_id;

			player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_CREATE_CLAN, clan_limit->room_card_limit()); //扣除馆长房卡

			message->set_clan_id(clan_id);
			message->set_oper_result(Asset::ERROR_SUCCESS);

			Asset::Clan clan;
			clan.set_clan_id(clan_id);
			clan.set_name(trim_name);
			clan.set_created_time(CommonTimerInstance.GetTime());
			clan.set_hoster_id(player->GetID());
			clan.set_hoster_name(player->GetNickName());
			clan.set_room_card_count(clan_limit->room_card_limit());

			message->mutable_clan()->CopyFrom(clan); //回传Client

			OnResult(message); //执行成功：广播执行结果

			player->OnClanCreated(clan_id);
			player->SetCurrClan(clan_id);
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_JOIN: //申请加入：跨服操作
		{
			auto server_id = ClanInstance.GetRegisterServerID(message->clan_id());

			Asset::ClanOperationSync proto;
			proto.set_player_id(player->GetID());
			proto.mutable_operation()->CopyFrom(*message);

			WorldInstance.SendProtocol2CenterServer(proto, server_id);
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_EDIT: //修改基本信息
		{
			//auto result = clan->OnChangedInformation(player, message);
			//message->set_oper_result(result); 
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_DISMISS: //解散
		{
			player->GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_DISMISS_CLAN, message->recharge_count());
			
			OnResult(message); //执行成功：广播执行结果
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_AGEE: //同意加入
		{
			if (player->GetID() != message->dest_player_id()) return; //馆长操作，不进行处理

			if (message->oper_result() == 0) //加入成功
			{
				auto des_player = PlayerInstance.Get(message->dest_player_id());
				if (!des_player) return;

				auto result = des_player->OnClanJoin(message->clan_id());
				if (result) des_player->SendProtocol(message);
			}
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_DISAGEE: //拒绝加入
		{
			/*
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return 10;
			}
			
			auto result = clan->OnDisAgree(player, message);
			message->set_oper_result(result); 
			*/
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_DELETE: //删除成员
		case Asset::CLAN_OPER_TYPE_MEMEBER_QUIT: //主动退出
		{
			player->OnQuitClan(message->clan_id());
			OnResult(message);  //执行成功：广播执行结果
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_RECHARGE: //充值
		{
			if (message->recharge_count() <= 0 || player->GetRoomCard() < message->recharge_count()) 
			{
				message->set_oper_result(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
				player->SendProtocol(message);

				return;
			}

			player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_RECHARGE_CLAN, message->recharge_count()); //扣除馆长房卡
			OnResult(message);  //执行成功：广播执行结果
		}

		case Asset::CLAN_OPER_TYPE_MEMEBER_QUERY: //成员状态查询
		{
			//clan->OnQueryMemberStatus(player, message);
			player->SetCurrClan(message->clan_id());
		}
		break;

		/*
		case Asset::CLAN_OPER_TYPE_ROOM_LIST_QUERY:
		{
			clan->OnQueryRoomList(player, message);
		}
		break;
		*/
		
		case Asset::CLAN_OPER_TYPE_CLAN_LIST_QUERY:
		{
			OnQueryClanList(player, message);
		}
		break;

		default:
		{
			ERROR("玩家:{} 茶馆操作尚未处理:{}", player->GetID(), message->ShortDebugString());
			return;
		}
		break;
	}
			
	//OnResult(message); //执行成功：广播执行结果
}
	
void ClanManager::OnResult(const Asset::ClanOperation* message)
{
	if (!message) return;

	Asset::ClanOperationSync proto;
	proto.mutable_operation()->CopyFrom(*message);

	WorldInstance.BroadCast2CenterServer(proto);
}

bool ClanManager::GetClan(int64_t clan_id, Asset::Clan& clan)
{
	return RedisInstance.Get("ddz_clan:" + std::to_string(clan_id), clan);
}

bool ClanManager::GetCache(int64_t clan_id, Asset::Clan& clan)
{
	return RedisInstance.Get("ddz_clan:" + std::to_string(clan_id), clan);
}

void ClanManager::OnQueryClanList(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;

	const auto& _stuff = player->Get();
	
	std::vector<int64_t> clan_list;
				
	for (auto clan_id : _stuff.clan_hosters()) clan_list.push_back(clan_id);
	for (auto clan_id : _stuff.clan_joiners()) clan_list.push_back(clan_id);


	for (const auto& clan_id : clan_list)
	{
		if (clan_id <= 0) continue;

		Asset::Clan clan;
		bool has_clan = GetClan(clan_id, clan);
		if (!has_clan) continue;

		if (clan.dismiss()) 
		{
			player->OnQuitClan(clan.clan_id());
			continue; //已解散
		}

		auto brief = message->mutable_clan_list()->Add();
		brief->set_clan_id(clan.clan_id());
		brief->set_name(clan.name());
		brief->set_created_time(clan.created_time());
		brief->set_hoster_id(clan.hoster_id());
		brief->set_hoster_name(clan.hoster_name());
		brief->set_mem_count(clan.member_list().size());
		brief->set_online_mem_count(clan.online_mem_count());
	}

	DEBUG("玩家:{} 查询茶馆列表:{}", player->GetID(), message->ShortDebugString());
}

}

