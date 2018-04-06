#include "MessageDispatcher.h"
#include "Player.h"

namespace Adoter
{

void DispatcherWorker::SendMessage(Asset::MsgItem msg/*消息COPY*/)
{
	PriorityQueueItem item(msg);
	system_clock::time_point curr_time = system_clock::now();
	msg.set_time(system_clock::to_time_t(curr_time));	//发送时间
	_messages.Emplace(item);
}

bool DispatcherWorker::Empty()
{
	return _messages.Empty();
}

bool DispatcherWorker::Size()
{
	return _messages.Size();
}

void DispatcherWorker::Dispatcher()
{
	while(true)
	{
		PriorityQueueItem item;
		if (!_messages.GetNext(item)) 
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		const Asset::MsgItem& message = item.Get();
		if (message.delay()) 
		{
			int32_t delay = message.delay(); //延时发送
			system_clock::time_point curr_time = system_clock::now();
			if (delay > 0 && system_clock::to_time_t(curr_time) < message.time() + delay) continue;	//还没到发送时间
		}
		int64_t receiver = message.receiver(); //发给接收者
		Discharge(receiver, message);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

bool DispatcherWorker::Discharge(int64_t receiver, const Asset::MsgItem& message)
{
	auto player = PlayerInstance.GetPlayer(receiver); //接收者可能不在线
	if (!player) return false;

	player->HandleMessage(message);		//交给各个接受者处理
	return true;
}

}
