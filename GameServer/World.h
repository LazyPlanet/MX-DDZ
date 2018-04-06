#pragma once

#include "Asset.h"
#include "CenterSession.h"

#include <memory>

namespace Adoter 
{

/*
 * 类说明：
 *
 * 大世界
 *
 * 游戏中玩家公共空间，区别副本.
 *
 * 说明：有的游戏中称为场景(World)，场景分为大世界和副本.
 *
 * */

class World : public std::enable_shared_from_this<World>
{
private:
	bool _stopped = false;
	int64_t _heart_count = 0; //心跳记次
	std::vector<std::shared_ptr<CenterSession>> _sessions;
public:
	static World& Instance()
	{
		static World _instance;
		return _instance;
	}
	
	bool Load(); //加载所有

	bool IsStopped() { return _stopped; }
	void EmplaceSession(std::shared_ptr<CenterSession> session) { _sessions.push_back(session); }

	void Update(int32_t diff); //世界中所有刷新都在此(比如刷怪，拍卖行更新...)，当前周期为50MS.
	virtual void BroadCast2CenterServer(const pb::Message& message, int except_server_id = 0); //向中心服务器广播数据
	virtual void BroadCast2CenterServer(const pb::Message* message, int except_server_id = 0); 
	virtual bool SendProtocol2CenterServer(const pb::Message& message, int server_id); //发协议到指定中心服务器
	virtual bool SendProtocol2CenterServer(const pb::Message* message, int server_id); 
};

#define WorldInstance World::Instance()

}
