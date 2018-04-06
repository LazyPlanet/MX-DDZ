#include "World.h"
#include "Protocol.h"
#include "PlayerName.h"
#include "MXLog.h"
#include "Activity.h"
#include "WhiteBlackManager.h"

namespace Adoter
{

const Asset::CommonConst* g_const = nullptr;
std::shared_ptr<GmtSession> g_gmt_client = nullptr;

bool World::Load()
{
	//协议初始化：必须最先初始化
	if (!ProtocolInstance.Load()) 
	{
		ERROR("协议加载失败.");
		return false;
	}
	//数据初始化：必须最先初始化
	if (!AssetInstance.Load()) 
	{
		ERROR("资源加载失败.");
		return false;
	}

	//
	//不依赖顺序的数据初始化
	//
	
	if (!NameInstance.Load())
	{
		ERROR("名字生成加载失败.");
		return false;
	}
	
	if (!WhiteBlackInstance.Load())
	{
		ERROR("黑白名单加载失败.");
		return false;
	}

	//
	//游戏内初始化
	//

	//特殊ID定义表
	pb::Message* message = AssetInstance.Get(458753); 
	g_const = dynamic_cast<const Asset::CommonConst*>(message); 
	if (!g_const) return false; //如果没有起不来

	//活动
	if (!ActivityInstance.Load())
	{
		ERROR("活动加载失败.");
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

	if (_heart_count % 60 == 0) ActivityInstance.Update(diff);

	PlayerInstance.Update(diff);

	g_gmt_client->Update();
}
	
}
