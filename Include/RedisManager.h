#pragma once

//#include <hiredis.h>
#include <string>
#include <iostream>

#include <cpp_redis/cpp_redis>  

#include "MXLog.h" 
#include "Config.h" 

/*
 * 数据库管理
 *
 * 所有表名称都要小写
 *
 *
 * */

namespace Adoter
{

class Redis 
{
private:
	std::string _hostname = "127.0.0.1";
	int32_t _port = 6379;
	std::string _password = "!QAZ%TGB&UJM9ol.";

	cpp_redis::client _client;
public:
	
	static Redis& Instance()
	{
		static Redis _instance;
		return _instance;
	}

	Redis() 
	{ 
		std::string hostname = ConfigInstance.GetString("Redis_ServerIP", "127.0.0.1");
		int32_t port = ConfigInstance.GetInt("Redis_ServerPort", 6379); 
		std::string password = ConfigInstance.GetString("Redis_Password", "!QAZ%TGB&UJM9ol.");

		if (port > 0) _port = port;
		if (!hostname.empty()) _hostname = hostname;
		if (!password.empty()) _password = password;

		_client.add_sentinel(_hostname, _port);
	}

	bool Connect()
	{
		try 
		{
			if (_client.is_connected()) return true;

			_client.connect(_hostname, _port, [this](const std::string& host, std::size_t port, cpp_redis::client::connect_state status) {
					if (status == cpp_redis::client::connect_state::dropped) { std::cout << "数据库连接失败..." << std::endl; }
					}, 0, -1, 5000);
			if (!_client.is_connected()) {
				LOG(ERROR, "数据库地址:{}，端口:{}连接失败，原因:未能连接数据库", _hostname, _port);
				return false;
			}

			_client.auth(_password, [](const cpp_redis::reply& reply) {
					if (reply.is_error()) { std::cerr << "数据库认证失败..." << reply.as_string() << std::endl; }
					else { std::cout << "数据库认证成功..." << std::endl; }
			});
		}
		catch (std::exception& e)
		{
			LOG(ERROR, "数据库地址:{}，端口:{}连接失败，原因:{}", _hostname, _port, e.what());
			return false;
		}
		return true;
	}

	bool Get(const std::string& key, std::string& value, bool async = true)
	{
		if (!Connect()) return false;

		auto get = _client.get(key);
		
		if (async) { _client.commit(); } 
		else { _client.sync_commit(std::chrono::milliseconds(100)); }
		
		auto reply = get.get();
		if (!reply.is_string()) {
			LOG(ERROR, "数据库获取:{}数据失败，原因:没有数据", key);
			return false;
		}
	
		value = reply.as_string();
		if (value == "") return false;

		return true;
	}
	
	bool GetArray(const std::string& key, std::vector<std::string>& values, bool async = true)
	{
		if (!Connect()) return false;

		auto get = _client.keys(key);
		
		if (async) {
			_client.commit(); 
		} else {
			_client.sync_commit(std::chrono::milliseconds(100)); 
		}
		
		auto reply = get.get();
		if (!reply.is_array()) return false;

		const auto& reply_list = reply.as_array();
	
		for (const auto& reply : reply_list)
		{
			if (!reply.is_string()) continue;

			values.push_back(reply.as_string());
		}

		return true;
	}
	
	bool Save(const std::string& key, const std::string& value, bool async = true)
	{
		if (!Connect()) return false;

		//auto has_auth = _client.auth(_password);
		auto set = _client.set(key, value);

		if (async) {
			_client.commit(); 
		} else {
			_client.sync_commit(std::chrono::milliseconds(100)); 
		}
		
		/*
		if (has_auth.get().ko()) {
			LOG(ERROR, "数据库获取:{}数据失败，原因:权限不足", key);
			return false;
		}
		*/

		auto get = set.get();
		std::string result = "ERROR";
		if (get.is_string()) result = get.as_string();
		
		LOG(INFO, "存储数据，结果:{} Key:{}", result, key); 
		return true;
	}
	
	bool Get(const std::string& key, google::protobuf::Message& value, bool async = true)
	{
		if (!Connect()) return false;

		//auto has_auth = _client.auth(_password);
		auto get = _client.get(key);
		
		if (async) {
			_client.commit(); 
		} else {
			_client.sync_commit(std::chrono::milliseconds(100)); 
		}
		
		/*
		if (has_auth.get().ko()) {
			LOG(ERROR, "数据库获取:{}数据失败，原因:权限不足", key);
			return false;
		}
		*/

		auto reply = get.get();
		if (!reply.is_string()) {
			LOG(ERROR, "数据库获取:{}数据失败，原因:没有数据", key);
			return false;
		}
	
		auto success = value.ParseFromString(reply.as_string());
		if (!success) 
		{
			LOG(ERROR, "数据库获取:{}数据失败，原因:不能反序列化成protobuff结构", key);
			return false;
		}

		return true;
	}
	
	bool Save(const std::string& key, const google::protobuf::Message& value, bool async = true)
	{
		if (!Connect()) return false;

		//auto has_auth = _client.auth(_password);
		auto set = _client.set(key, value.SerializeAsString());

		if (async) {
			_client.commit(); //异步存储
		} else {
			_client.sync_commit(std::chrono::milliseconds(100)); //同步存储
		}
		
		/*
		if (has_auth.get().ko()) {
			LOG(ERROR, "数据库存储:{}数据失败，原因:权限不足", key);
			return false;
		}
		*/

		auto get = set.get();
		std::string result = "ERROR";
		if (get.is_string()) result = get.as_string();
		
		LOG(INFO, "存储数据，结果:{} Key:{} 数据:{}", result, key, value.ShortDebugString()); 
		return true;
	}

	int64_t CreatePlayer()
	{
		if (!Connect()) return false;

		//auto has_auth = _client.auth(_password);
		auto incrby = _client.incrby("player_counter", 1);
		_client.commit();

		/*
		if (has_auth.get().ko()) {
			LOG(ERROR, "数据库验证失败，原因:权限不足");
			return false;
		}
		*/

		int64_t player_id = 0;

		auto reply = incrby.get();
		if (reply.is_integer()) player_id = reply.as_integer();

		if (player_id == 0) return 0;

		//
		//角色ID带有服务器ID
		//
		int32_t server_id = ConfigInstance.GetInt("ServerID", 1); //服务器ID
		player_id = (server_id << 20) + player_id; 

		return player_id;
	}

	bool GetPlayer(int64_t player_id, Asset::Player& player)
	{
		std::string key = "player:" + std::to_string(player_id);
		auto success = Get(key, player, true);
		return success;
	}
	
	bool SavePlayer(int64_t player_id, const Asset::Player& player)
	{
		std::string key = "player:" + std::to_string(player_id);
		auto success = Save(key, player, true);
		return success;
	}
	
	int64_t CreateRoom()
	{
		if (!Connect()) return false;
		
		auto incrby = _client.incrby("ddz_room_counter", 1);
		_client.commit();

		int64_t room_id = 0;

		auto reply = incrby.get();
		if (reply.is_integer()) room_id = reply.as_integer();

		if (room_id == 0) return 0;

		//
		//房间ID带有服务器ID
		//
		//每个服务器上限65536个房间，玩家通过房间ID即可获取当前房间所在服务器
		//
		room_id = room_id % 65536;

		int32_t server_id = ConfigInstance.GetInt("ServerID", 1); //服务器ID
		room_id = (server_id << 16) + room_id;
		
		//int32_t game_type = ConfigInstance.GetInt("GameType", 1); //游戏类型
		//room_id = (game_type << 24) + room_id;
		
		return room_id;
	}

	int64_t GetGameType(int64_t room_id)
	{
		int64_t game_type = room_id >> 24;

		return game_type;
	}
	
	int64_t GetServer(int64_t room_id)
	{
		//int64_t server_id = (room_id & 0x00FFFFFF) >> 16;
		int64_t server_id = room_id >> 16;

		return server_id;
	}

	bool GetUser(std::string username, Asset::User& user)
	{
		if (username.empty()) return false;

		std::string key = "user:" + username;
		return Get(key, user);
	}
	
	bool SaveUser(std::string username, const Asset::User& user)
	{
		if (username.empty()) return false;

		std::string key = "user:" + username;

		auto success = Save(key, user, true);
		if (!success) return false;

		LOG(INFO, "保存账号数据，username:{} user:{}", username, user.ShortDebugString());
		return true;
	}
	
	bool GetGuestAccount(std::string& account)
	{
		if (!Connect()) return false;
		
		//auto has_auth = _client.auth(_password);
		auto incrby = _client.incrby("guest_counter", 1);
		_client.commit();
		
		/*
		if (has_auth.get().ko()) {
			LOG(ERROR, "数据库验证失败，原因:权限不足");
			return false;
		}
		*/

		int64_t guest_id = 0;

		auto reply = incrby.get();
		if (reply.is_integer()) guest_id = reply.as_integer();

		if (guest_id == 0) return 0;

		account = "guest_" + std::to_string(guest_id);
		return true;
	}

	bool SetLocation(int64_t player_id, Asset::Location location)
	{
		/*
		double longitude = location.longitude();
		double latitude = location.latitude();

		std::string key = "player:" + std::to_string(player_id);
		std::string command = "geoadd players_location " + std::to_string(longitude) + " " + std::to_string(latitude) + " " + key;

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str()); 
		if (!reply) return false;
		*/

		return true;
	}

	bool GetLocation(int64_t player_id, Asset::Location& location)
	{
		/*
		std::string key = "player:" + std::to_string(player_id);
		std::string command = "geopos players_location " + key;

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());
		if (!reply) return false;

		if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 1) 
		{
			freeReplyObject(reply);
			return false;
		}

		if (reply->element[0]->elements != 2) 
		{
			freeReplyObject(reply);
			return false;
		}

		if (!reply->element[0]->element[0]->str || !reply->element[0]->element[1]->str) 
		{
			freeReplyObject(reply);
			return false;
		}

		double longitude = atof(reply->element[0]->element[0]->str);
		double latitude = atof(reply->element[0]->element[1]->str);
		freeReplyObject(reply);

		location.set_longitude(longitude);
		location.set_latitude(latitude);
		*/
		return true;
	}

	double GetDistance(int64_t player_id1, int64_t player_id2)
	{
		/*
		std::string command = "geodist players_location player:" + std::to_string(player_id1) + " player:" + std::to_string(player_id2);

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());
		if (!reply) return -1.0;

		if (reply->type == REDIS_REPLY_NIL || !reply->str) 
		{
			freeReplyObject(reply);
			return -1.0;
		}

		double dist = atof(reply->str);
		freeReplyObject(reply);
		return dist;
		*/
		return 0;
	}
	
	bool GetRoomHistory(int64_t room_id, Asset::RoomHistory& history)
	{
		auto success = Get("room_history:" + std::to_string(room_id), history);
		if (!success) return false;

		return true;
	}
	
	bool SaveRoomHistory(int64_t room_id, const Asset::RoomHistory& history)
	{
		auto success = Save("room_history:" + std::to_string(room_id), history);
		if (!success) return false;

		return true;
	}
	
	bool GetMatching(Asset::MatchStatistics& stats)
	{
		auto success = Get("match_stats:", stats);
		if (!success) return false;

		return true;
	}
	
	bool SaveMatching(const Asset::MatchStatistics& stats)
	{
		auto success = Save("match_stats:", stats);
		if (!success) return false;

		return true;
	}
	
	int64_t CreateClan()
	{
		if (!Connect()) return 0;

		auto incrby = _client.incrby("ddz_clan_counter", 1);
		_client.commit();

		int64_t clan_id = 0;

		auto reply = incrby.get();
		if (reply.is_integer()) clan_id = reply.as_integer();

		/*
		if (clan_id == 0) return 0;

		int64_t server_id = ConfigInstance.GetInt("ServerID", 1); //服务器ID
		clan_id = (server_id << 32) + clan_id; 
		*/

		return clan_id;
	}
	
	//创建比赛，比赛唯一识别
	//
	int64_t CreateMatch()
	{
		if (!Connect()) return 0;

		auto incrby = _client.incrby("clan_match_counter", 1);
		_client.commit();

		int64_t match_id = 0;

		auto reply = incrby.get();
		if (reply.is_integer()) match_id = reply.as_integer();

		return match_id;
	}
	
	bool MGet(const std::vector<std::string>& keys, std::vector<std::string>& values, bool async = true)
	{
		if (!Connect()) return false;

		auto get = _client.mget(keys);
		
		if (async) {
			_client.commit(); 
		} else {
			_client.sync_commit(std::chrono::milliseconds(100)); 
		}
		
		auto reply = get.get();
		if (!reply.is_array()) return false;

		const auto& reply_list = reply.as_array();
	
		for (const auto& reply : reply_list)
		{
			if (!reply.is_string()) continue;

			values.push_back(reply.as_string());
		}

		return true;
	}
};

#define RedisInstance Redis::Instance()

}
