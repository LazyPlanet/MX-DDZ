/*
 * C++版本：C++11
 *
 * 说明：倾向于用标准类库中接口
 *
 */

#include <thread>
#include <vector>
#include <memory>
#include <functional>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <cpp_redis/cpp_redis>

#include "Timer.h"
#include "World.h"
#include "MXLog.h"
#include "Config.h"
#include "CenterSession.h"
#include "MessageDispatcher.h"

const int const_world_sleep = 50;

/*
 * 游戏逻辑处理接口
 *
 * */

using namespace Adoter;

boost::asio::io_service _io_service;
std::shared_ptr<boost::asio::io_service::work> _io_service_work = nullptr;

void SignalHandler(const boost::system::error_code& error, int)
{    
	//if (!error) World::StopNow(SHUTDOWN_EXIT_CODE);
}

void WorldUpdateLoop()
{
	int32_t curr_time = 0, prev_sleep_time = 0;
	
	int32_t prev_time = CommonTimerInstance.GetStartTime();

	while (!WorldInstance.IsStopped())
	{
		curr_time = CommonTimerInstance.GetStartTime();
		
		int32_t diff = CommonTimerInstance.GetTimeDiff(prev_time, curr_time);
		
		WorldInstance.Update(diff);        
		
		prev_time = curr_time;
		
		if (diff <= const_world_sleep + prev_sleep_time) //50MS
		{            
			prev_sleep_time = const_world_sleep + prev_sleep_time - diff;            
			std::this_thread::sleep_for(std::chrono::milliseconds(prev_sleep_time));        
		}        
		else    
		{	
			prev_sleep_time = 0;
		}
	}
}

void ShutdownThreadPool(std::vector<std::shared_ptr<std::thread>>& threads)
{    
	_io_service.stop();    
	
	for (auto& thread : threads) thread->join();    
}

/*
 *
 * 函数入口
 *
 * */
int main(int argc, const char* argv[])
{
	if (argc != 2) return 2; //参数不对

	try 
	{
		std::srand(std::time(0)); //random_shuffle不是真随机：http://stackoverflow.com/questions/13459953/random-shuffle-not-really-random
	
		//
		//系统配置读取
		//
		if (!ConfigInstance.LoadInitial(argv[1]))
		{
			std::cout << "Load file error, please check the file, name:" << argv[1] << std::endl;; //控制台的日志可以直接用该函数
			return 3;
		}
	
		//
		//日志系统配置
		//
		MXLogInstance.Load();
	
		//
		//世界初始化，涵盖所有
		//
		//游戏内相关逻辑均在此初始化
		//
		if (!WorldInstance.Load()) return 1;

		//网络初始化
		_io_service_work = std::make_shared<boost::asio::io_service::work>(_io_service);

		int _thread_nums = 5;
		std::vector<std::shared_ptr<std::thread>> _threads;	
		for (int i = 0; i < _thread_nums; ++i)
		{
			std::shared_ptr<std::thread> pthread(new std::thread(boost::bind(&boost::asio::io_service::run, &_io_service)));	//IO <-> Multi Threads
			_threads.push_back(pthread);
		}

		//boost::asio::signal_set signals(_io_service, SIGINT, SIGTERM);
		//signals.async_wait(SignalHandler);
		//

		/*
		std::string server_ip = ConfigInstance.GetString("ServerIP", "0.0.0.0");
		if (server_ip.empty()) return 4;
		
		int32_t server_port = ConfigInstance.GetInt("ServerPort", 50001);
		if (server_port <= 0 || server_port > 0xffff) return 5;
		*/

		int32_t thread_count = ConfigInstance.GetInt("ThreadCount", 5);
		if (thread_count <= 0) return 6;

		int32_t redis_work_count = ConfigInstance.GetInt("Redis_WorkCount", 5);
		tacopie::get_default_io_service()->set_nb_workers(redis_work_count);

		//
		//连接中心服
		//
		std::string center_server_address = ConfigInstance.GetString("Center_ServerIP", "0.0.0.0");

		int32_t center_server_port1 = ConfigInstance.GetInt("Center_ServerPort1", 49999); 
		int32_t center_server_port2 = ConfigInstance.GetInt("Center_ServerPort2", 50000); 

		boost::asio::ip::tcp::endpoint center_endpoint1(boost::asio::ip::address::from_string(center_server_address), center_server_port1);
		boost::asio::ip::tcp::endpoint center_endpoint2(boost::asio::ip::address::from_string(center_server_address), center_server_port2);

		auto session1 = std::make_shared<CenterSession>(_io_service, center_endpoint1);
		session1->AsyncConnect();
		WorldInstance.EmplaceSession(session1);
		
		auto session2 = std::make_shared<CenterSession>(_io_service, center_endpoint2);
		session2->AsyncConnect();
		WorldInstance.EmplaceSession(session2);
		
		DispatcherInstance.StartWork(); //消息处理机制

		//世界循环
		WorldUpdateLoop();
	
		ShutdownThreadPool(_threads);
	}
	catch (const std::exception& e)
	{
		ERROR("Exception:{}", e.what());
	}
	
	DEBUG("Service stopped.");
	return 0;
}
