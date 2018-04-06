#pragma once

#include "P_Header.h"
#include "PriorityQueue.h"
#include "CommonUtil.h"

namespace Adoter
{

using namespace std::chrono;

class PriorityQueueItem
{
private:
	Asset::MsgItem _item;
public:

	PriorityQueueItem() { }
	explicit PriorityQueueItem(Asset::MsgItem& item)
	{
		this->_item.CopyFrom(item);
	}

	bool operator < (const PriorityQueueItem& other) const
	{
		return _item.priority() > other._item.priority();	//最小值优先，优先级级别：1~10，默认为10
	}

	const Asset::MsgItem& Get() { return _item; }
	const Asset::MsgItem& Get() const { return _item; }
};

class DispatcherWorker : public std::enable_shared_from_this<DispatcherWorker>
{
private:
	std::shared_ptr<std::thread> _thread;
	PriorityQueue<PriorityQueueItem> _messages;
public:
	~DispatcherWorker() { _thread->join(); }
	virtual void Start() { _thread = std::make_shared<std::thread>(std::bind(&DispatcherWorker::Dispatcher, this)); }
public:
	bool Empty(); //队列是否为空
	bool Size(); //队列大小
	void Dispatcher(); //分发消息
	void SendMessage(Asset::MsgItem msg); //发消息
	bool Discharge(int64_t receiver, const Asset::MsgItem& message); //发送给接收者
};

class MessageDispatcher 
{
private:
	int32_t _worker_count = 2;
	std::vector<std::shared_ptr<DispatcherWorker>> _workers;
public:

	MessageDispatcher()
	{
		//初始化工作线程
		for (int i = 0; i < _worker_count; ++i)
		{
			auto worker = std::make_shared<DispatcherWorker>();
			_workers.push_back(worker);
		}
	}

	void StartWork()
	{
		for (auto worker : _workers)
		{
			worker->Start();
		}
	}

	//消息队列存盘(一般游戏逻辑不需做此操作)
	void Save() 
	{
		//Asset::MsgItems items;	//用于存盘
	}	

	//消息队列加载(一般游戏逻辑不需做此操作)
	void Load() 
	{
		//Asset::MsgItems items;	//用于加载
	}		
	
	void SendMessage(Asset::MsgItem msg) //发消息
	{
		int32_t selected = CommonUtil::Random(0, _worker_count-1);
		auto worker = _workers[selected]; //随机选择一个线程发送
		if (!worker) return;

		worker->SendMessage(msg);
	}

	static MessageDispatcher& Instance()
	{
		static MessageDispatcher _instance;
		return _instance;
	}
};

#define DispatcherInstance MessageDispatcher::Instance()

}
