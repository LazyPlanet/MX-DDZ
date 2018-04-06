#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <iostream>
#include <deque>

#include <boost/asio.hpp>

#include "ClientSocket.h"
#include "P_Header.h"

namespace Adoter
{

using namespace google::protobuf;
namespace pb = google::protobuf;

class GmtSession : public ClientSocket
{
public:
	explicit GmtSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint);

	GmtSession(GmtSession const& right) = delete;    
	GmtSession& operator = (GmtSession const& right) = delete;
	
	const boost::asio::ip::tcp::endpoint GetRemotePoint() { return _remote_endpoint; }

	Asset::COMMAND_ERROR_CODE OnCommandProcess(const Asset::Command& command);
	Asset::COMMAND_ERROR_CODE OnSendMail(const Asset::SendMail& command);
	Asset::COMMAND_ERROR_CODE OnSystemBroadcast(const Asset::SystemBroadcast& command);
	Asset::COMMAND_ERROR_CODE OnActivityControl(const Asset::ActivityControl& command);
    
	virtual void OnConnected(); //连接上服务器
	bool OnInnerProcess(const Asset::InnerMeta& meta); //内部协议处理
	
	void SendProtocol(pb::Message& message);
	void SendProtocol(pb::Message* message);

	void SendInnerMeta(const Asset::InnerMeta& meta);

    virtual bool StartReceive();
    virtual bool StartSend();
    
    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred);

	virtual void AsyncSendMessage(std::string message);
    virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred);  

	virtual bool Update() override;
private:
	std::deque<std::string> _send_list;
	std::deque<Asset::InnerMeta> _receive_list;
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::string _ip_address;
	std::mutex _gmt_lock; 
	std::mutex _data_lock; 
	int64_t _session_id = 0; //操作会话
};

}
