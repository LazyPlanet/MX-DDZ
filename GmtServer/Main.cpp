#include <thread>
#include <vector>
#include <memory>
#include <functional>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <cpp_redis/cpp_redis>

#include "Timer.h"
#include "Config.h"
#include "Asset.h"
#include "Protocol.h"
#include "MXLog.h"
#include "ServerSession.h"

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
		//系统配置读取
		if (!ConfigInstance.LoadInitial(argv[1]))
		{
			printf("Load %s error, please check the file.", argv[1]); //控制台的日志可以直接用该函数
			return 3;
		}
		
		//日志系统配置
		MXLogInstance.Load();
	
		//协议初始化：必须最先初始化
		if (!ProtocolInstance.Load()) 
		{
			ERROR("协议加载失败.");
			return false;
		}

		//数据初始化
		if (!AssetInstance.Load())
		{
			printf("Load asset error, please check the file."); //控制台的日志可以直接用该函数
			return 4;
		}
	
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
		
		int32_t server_port = ConfigInstance.GetInt("ServerPort", 50003);
		if (server_port <= 0 || server_port > 0xffff) return 5;
		
		int32_t thread_count = ConfigInstance.GetInt("ThreadCount", 5);
		if (thread_count <= 0) return 6;

		int32_t redis_work_count = ConfigInstance.GetInt("Redis_WorkCount", 5);
		tacopie::get_default_io_service()->set_nb_workers(redis_work_count);

		ServerSessionInstance.StartNetwork(_io_service, server_ip, server_port, thread_count);

		_io_service.run();

		std::cout << "Service stop..." << std::endl;

		ShutdownThreadPool(_threads);
	}
	catch (std::exception& e)
	{
		std::cerr << __func__ << ":Exception: " << e.what() << std::endl;
	}
	
	std::cout << "Service stoped." << std::endl;
	return 0;
}
