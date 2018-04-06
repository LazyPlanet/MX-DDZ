#include "World.h"
#include "Protocol.h"
#include "Room.h"
#include "Game.h"
#include "PlayerMatch.h"
#include "PlayerName.h"
#include "MXLog.h"
#include "Activity.h"
#include "CenterSession.h"

namespace Adoter
{

const Asset::CommonConst* g_const = nullptr;
//std::shared_ptr<CenterSession> g_center_session = nullptr;

bool World::Load()
{
	//
	//协议初始化：必须最先初始化
	//
	if (!ProtocolInstance.Load()) 
	{
		ERROR("ProtocolInstance load error.");
		return false;
	}

	//
	//数据初始化：必须最先初始化
	//
	if (!AssetInstance.Load()) 
	{
		ERROR("AssetInstance load error.");
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
	

}
