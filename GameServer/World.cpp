#include "World.h"
#include "Protocol.h"
#include "Room.h"
#include "Game.h"
#include "PlayerMatch.h"
#include "PlayerName.h"
#include "MXLog.h"
#include "Activity.h"
#include "CenterSession.h"
#include "NameLimit.h"

namespace Adoter
{

int32_t g_server_id = 0;
Asset::GAME_TYPE _game_type = Asset::GAME_TYPE_DOUDIZHU;
const Asset::CommonConst* g_const = nullptr;

bool World::Load()
{
	//
	//协议初始化：必须最先初始化
	//
	if (!ProtocolInstance.Load()) 
	{
		ERROR("协议加载失败");
		return false;
	}

	//
	//数据初始化：必须最先初始化
	//
	if (!AssetInstance.Load()) 
	{
		ERROR("资源加载失败");
		return false;
	}
	
	if (!NameLimitInstance.Load()) 
	{
		ERROR("屏蔽字库加载失败.");
		return false;
	}

	//
	//不依赖顺序的数据初始化
	//
	
	if (!GameInstance.Load()) 
	{
		ERROR("GameInstance load error.");
		return false;
	}

	if (!NameInstance.Load())
	{
		ERROR("NameInstance load error.");
		return false;
	}

	//
	//游戏内初始化
	//

	pb::Message* message = AssetInstance.Get(458753); //特殊ID定义表
	g_const = dynamic_cast<const Asset::CommonConst*>(message); 
	if (!g_const) return false; //如果没有起不来

	MatchInstance.DoMatch(); //玩家匹配
	
	if (!ActivityInstance.Load())
	{
		ERROR("ActivityInstance load error.");
		return false;
	}

	return true;
}

//
//世界中所有刷新都在此(比如刷怪，拍卖行更新...)
//
//当前周期为50MS.
//
void World::Update(int32_t diff)
{
	++_heart_count;

	MatchInstance.Update(diff);

	RoomInstance.Update(diff);
	
	ActivityInstance.Update(diff);
	
	//g_center_session->Update();
	for (auto session : _sessions) session->Update();
}
	
void World::BroadCast2CenterServer(const pb::Message& message, int except_server_id)
{
	for (auto session : _sessions)
	{
		if (!session) continue;

		auto center_server_id = session->GetCenterServerID();
		if (center_server_id == except_server_id) continue;

		session->SendProtocol(message);
	}
}

void World::BroadCast2CenterServer(const pb::Message* message, int except_server_id) 
{
	if (!message) return;

	BroadCast2CenterServer(*message, except_server_id);
}
	
bool World::SendProtocol2CenterServer(const pb::Message& message, int server_id)
{
	for (auto session : _sessions)
	{
		if (!session) continue;
		
		auto center_server_id = session->GetCenterServerID();
		if (center_server_id == server_id) 
		{
			session->SendProtocol(message);
			return true;;
		}
	}

	return false;
}

bool World::SendProtocol2CenterServer(const pb::Message* message, int server_id) 
{
	if (!message) return false;

	return SendProtocol2CenterServer(*message, server_id);
}

}
