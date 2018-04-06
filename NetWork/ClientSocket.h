#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <sstream>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include "MXLog.h"

namespace Adoter 
{

enum CONNECTION_STATUS {

	CONNECTION_STATUS_NIL = 1,
	CONNECTION_STATUS_CONNECTING = 2,
	CONNECTION_STATUS_CONNECTED = 3
};

class ClientSocket : public std::enable_shared_from_this<ClientSocket>
{
public:
	ClientSocket(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
		_timer(io_service), _remote_endpoint(endpoint), _closed(false), _closing(false)
	{
		_io_service = &io_service;
		_ip_address = _remote_endpoint.address().to_string();
		_port = _remote_endpoint.port();
	}
	
	virtual bool Update() 
	{
		if (_closed) return false;
		std::lock_guard<std::mutex> lock(_send_lock);

		if (_is_writing_async || (_write_queue.empty() && !_closing)) return true; //发送可以放到消息队列里面处理

		for (; HandleQueue(); ) {}

		return true;
	}

    virtual void AsyncConnect()
    {
		_conn_status = CONNECTION_STATUS_CONNECTING;
		_socket.reset(new boost::asio::ip::tcp::socket(*_io_service));
        _socket->async_connect(_remote_endpoint, std::bind(&ClientSocket::OnConnect, shared_from_this(), std::placeholders::_1));

        if (_connect_timeout > 0) 
        {
            _timer.expires_from_now(boost::posix_time::seconds(_connect_timeout));
            _timer.async_wait(std::bind(&ClientSocket::OnConnectTimeOut, shared_from_this(), std::placeholders::_1));
        }
    }
    
	virtual void OnConnectTimeOut(const boost::system::error_code& error) 
    {
		//DEBUG("超时检查...连接服务器:{}，端口:{} 状态:{}", _ip_address, _port, _conn_status);

        if (error == boost::asio::error::operation_aborted) return;
		if (error) ERROR("服务器内部连接超时失败，必须处理解决，错误码:{}", error.message());
            
        if (_connect_timeout > 0) 
        {
			_timer.expires_from_now(boost::posix_time::seconds(_connect_timeout));
			_timer.async_wait(std::bind(&ClientSocket::OnConnectTimeOut, shared_from_this(), std::placeholders::_1));
		}

		if (CONNECTION_STATUS_CONNECTED != _conn_status) 
		{
			WARN("网络正在连接...连接服务器:{}，端口:{} 状态:{}", _ip_address, _port, _conn_status);

			_conn_status = CONNECTION_STATUS_CONNECTING;
			_socket.reset(new boost::asio::ip::tcp::socket(*_io_service));
			_socket->async_connect(_remote_endpoint, std::bind(&ClientSocket::OnConnect, shared_from_this(), std::placeholders::_1));
		}
    }
    
    void Close(const std::string& reason)
    {
		if (_closed.exchange(true)) return;

		boost::system::error_code error;
		_socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
		_socket->close(error);
		_socket.reset();

		ERROR("服务器关闭网络连接，错误码:{} 原因:{}", error.message(), reason);

		OnClose();
    }

    void AsynyReadSome()
    {
        _socket->async_read_some(boost::asio::buffer(_buffer), std::bind(&ClientSocket::OnReadSome, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

	void EnterQueue(std::string&& meta)
	{
		std::lock_guard<std::mutex> lock(_send_lock);

		auto content = std::move(meta);
		//
		//数据包头
		//
		unsigned short body_size = content.size();
		if (body_size >= MAX_DATA_SIZE) 
		{
			LOG(ERROR, "协议已经超过最大限制，包长:{}", body_size);
			return;
		}

		unsigned char header[2] = { 0 };

		header[0] = (body_size >> 8) & 0xff;
		header[1] = body_size & 0xff;

		//
		//包数据体
		//
		auto body = content.c_str(); 

		//数据整理发送
		char buffer[MAX_DATA_SIZE] = { 0 }; //发送数据缓存
		for (int i = 0; i < 2; ++i) buffer[i] = header[i];
		for (int i = 0; i < body_size; ++i) buffer[i + 2] = body[i];

		_write_queue.push(std::string(buffer, body_size + 2));
	}
	
	void AsyncSendMessage(std::string meta)
	{
		EnterQueue(std::move(meta));
	}

    void AsyncWriteSome(const char* data, size_t size)
    {
		std::string meta(data, size);
		EnterQueue(std::move(meta));
    }
	
	bool AsyncProcessQueue()    
	{
		if (_is_writing_async) return false;
		_is_writing_async = true;

		_socket->async_write_some(boost::asio::null_buffers(), std::bind(&ClientSocket::OnWriteSome, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
		return false;
	}

	bool HandleQueue()
	{
		if (!IsConnected()) 
		{
			ERROR("网络已断开连接.");
			return false;
		}
		if (_write_queue.empty()) return false;

		std::string& meta = _write_queue.front();  //其实是META数据

		std::size_t bytes_to_send = meta.size();

		boost::system::error_code error;
		std::size_t bytes_sent = _socket->write_some(boost::asio::buffer(meta.c_str(), bytes_to_send), error);

		if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
		{
			ERROR("待发送数据长度:{} 实际发送数据长度:{}", bytes_to_send, bytes_sent);
			return AsyncProcessQueue();

			_write_queue.pop();
			if (_closing && _write_queue.empty()) Close("关闭");

			return false;
		}
		else if (bytes_sent == 0)
		{
			ERROR("待发送数据长度:{} 实际发送数据长度:{} 错误码:{}", bytes_to_send, bytes_sent, error.message());

			_write_queue.pop();
			if (_closing && _write_queue.empty()) Close("关闭");

			return false;
		}
		else if (bytes_sent < bytes_to_send) //一般不会出现这个情况，重新发送，记个ERROR
		{
			ERROR("待发送数据长度:{} 实际发送数据长度:{}", bytes_to_send, bytes_sent);
			return AsyncProcessQueue();
		}

		//DEBUG("待发送数据长度:{} 实际发送数据长度:{} 错误码:{}", bytes_to_send, bytes_sent, error.message());
		_write_queue.pop();

		if (_closing && _write_queue.empty()) Close("关闭");
		return !_write_queue.empty();
	}

	virtual void DelayedClose() { _closing = true; } //发送队列为空时再进行关闭
	virtual bool IsConnected() { return _socket && _socket->is_open(); }
	virtual bool IsOpen() const { return !_closed && !_closing; }
	virtual bool IsClosed() const { return _closed || _closing; }

    virtual void OnClose() { 
		_conn_status = CONNECTION_STATUS_NIL;
	}
    virtual void OnConnected() { 
		_closed = _closing = false;
		_conn_status = CONNECTION_STATUS_CONNECTED;
	}

    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred) { }
	virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred) { }

protected:
	boost::asio::io_service* _io_service = nullptr;
	boost::asio::deadline_timer _timer;
	std::shared_ptr<boost::asio::ip::tcp::socket> _socket; 
	boost::asio::ip::tcp::endpoint _local_endpoint;
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::mutex _send_lock;

	std::string _ip_address;
	int32_t _port = 0;

    volatile int64_t _ticks = 0;
    volatile int64_t _last_rw_ticks = 0;
    int64_t _connect_timeout = 5;
	
	//接收缓存
	std::array<unsigned char, MAX_DATA_SIZE> _buffer;
    
	virtual void OnConnect(const boost::system::error_code& error)
    {
		DEBUG("网络连接中，当前状态:{}，错误信息:{}...", _conn_status, error.message());

		if (_conn_status != CONNECTION_STATUS_CONNECTING) return;

        if (error)
        {
            Close("连接失败，错误信息: " + error.message());
            return;
        }

        boost::system::error_code ec;
        _socket->set_option(boost::asio::ip::tcp::no_delay(true), ec);

        if (ec)
        {
            Close("连接失败，错误信息: " + ec.message());
            return;
        }

        _local_endpoint = _socket->local_endpoint(ec);

        if (ec)
        {
            Close("连接失败，错误信息: " + ec.message());
            return;
        }

		OnConnected(); //连接成功

        StartReceive(); //开始接收数据
        StartSend(); //开始发送数据
    }
    
	virtual bool StartSend() { return true; }
	virtual bool StartReceive() { return true; }
private:
	std::atomic<bool> _closed;    
	std::atomic<bool> _closing;
	bool _is_writing_async = false;
	std::queue<std::string> _write_queue;
	CONNECTION_STATUS _conn_status = CONNECTION_STATUS_NIL;
};	

}
