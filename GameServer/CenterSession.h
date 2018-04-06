#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <iostream>
#include <deque>
#include <unordered_map>

#include <boost/asio.hpp>

#include "ClientSocket.h"
#include "P_Header.h"

namespace Adoter
{

using namespace google::protobuf;
namespace pb = google::protobuf;

class Player;

extern int32_t g_server_id;

class CenterSession : public ClientSocket
{
public:
	explicit CenterSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint);

	CenterSession(CenterSession const& right) = delete;    
	CenterSession& operator = (CenterSession const& right) = delete;
	
	const boost::asio::ip::tcp::endpoint GetRemotePoint() { return _remote_endpoint; }
	Asset::COMMAND_ERROR_CODE OnCommandProcess(const Asset::Command& command);
    
	virtual void OnConnected(); //连接上服务器
	bool OnMessageProcess(const Asset::Meta& meta); //内部协议处理：包括玩家发往逻辑服务器的协议
	bool OnInnerProcess(const Asset::Meta& meta); //服务器间协议处理
	
	void SendProtocol(const pb::Message& message);
	void SendProtocol(const pb::Message* message);

    virtual bool StartReceive();
    virtual bool StartSend();
    
    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred);

	//void AsyncSendMessage(std::string message);
    virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred);  

	virtual bool Update() override;
	void SayHi();

	void RemovePlayer(int64_t player_id);
	void AddPlayer(int64_t player_id, std::shared_ptr<Player>);
	std::shared_ptr<Player> GetPlayer(int64_t player_id);

	int32_t ServerID() { return _server_id; }
	int32_t GetCenterServerID() { return _center_server_id; }
private:
	std::deque<std::string> _send_list;
	std::deque<Asset::Meta> _receive_list;
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::string _ip_address;
	std::unordered_map<int64_t, std::shared_ptr<Player>> _players; //实体为智能指针，不要传入引用
	std::mutex _player_lock;
	
	int64_t _heart_count = 0; //心跳次数
	int32_t _server_id = 0;
	int32_t _center_server_id = 0; //连接的中心服务器
};

}
