#include <thread>
#include <vector>
#include <memory>
#include <functional>

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include "World.h"
#include "Timer.h"
#include "Config.h"
#include "Asset.h"
#include "MXLog.h"
#include "WorldSession.h"
#include "GmtSession.h"
#include "RedisManager.h"

const int const_world_sleep = 50;

/*
 * 游戏逻辑处理接口
 *
 * */

using namespace Adoter;

boost::asio::io_service _io_service;
std::shared_ptr<boost::asio::io_service::work> _io_service_work;

void SignalHandler(const boost::system::error_code& error, int)
{    
	//if (!error) World::StopNow(SHUTDOWN_EXIT_CODE);
}

void WorldUpdateLoop()
{
	uint32_t curr_time = 0, prev_sleep_time = 0;
	
	uint32_t prev_time = CommonTimerInstance.GetStartTime();

	while (!WorldInstance.IsStopped())
	{
		curr_time = CommonTimerInstance.GetStartTime();
		
		uint32_t diff = CommonTimerInstance.GetTimeDiff(prev_time, curr_time);
		
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
		//
		//系统配置读取
		//
		if (!ConfigInstance.LoadInitial(argv[1]))
		{
			printf("Load %s error, please check the file.", argv[1]); //控制台的日志可以直接用该函数
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

		/*
		boost::asio::signal_set signals(_io_service, SIGINT, SIGTERM);
		signals.async_wait(SignalHandler);
		*/

		std::string server_ip = ConfigInstance.GetString("ServerIP", "0.0.0.0");
		if (server_ip.empty()) return 4;
		
		int32_t server_port = ConfigInstance.GetInt("ServerPort", 50000);
		if (server_port <= 0 || server_port > 0xffff) return 5;
		
		int32_t thread_count = ConfigInstance.GetInt("ThreadCount", 5);
		if (thread_count <= 0) return 6;

		int32_t redis_work_count = ConfigInstance.GetInt("Redis_WorkCount", 5);
		tacopie::get_default_io_service()->set_nb_workers(redis_work_count);

		WorldSessionInstance.StartNetwork(_io_service, server_ip, server_port, thread_count);

		//
		//连接GMT服务器
		//
		std::string gmt_server_address = ConfigInstance.GetString("GMT_ServerIP", "0.0.0.0");
		int32_t gmt_server_port = ConfigInstance.GetInt("GMT_ServerPort", 50003); 
		boost::asio::ip::tcp::endpoint gmt_endpoint(boost::asio::ip::address::from_string(gmt_server_address), gmt_server_port);
		g_gmt_client = std::make_shared<GmtSession>(_io_service, gmt_endpoint);
		g_gmt_client->AsyncConnect();

		//世界循环
		WorldUpdateLoop();

		ShutdownThreadPool(_threads);
	}
	catch (std::exception& e)
	{
		std::cerr << __func__ << ":Exception: " << e.what() << std::endl;
	}
	
	std::cout << "Service stoped." << std::endl;
	return 0;
}
