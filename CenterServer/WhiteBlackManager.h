#pragma once

#include <memory>
#include <functional>
#include <fstream>

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <boost/algorithm/string.hpp>

#include "P_Header.h"

namespace Adoter
{
namespace pb = google::protobuf;

class WhiteBlackManager : public std::enable_shared_from_this<WhiteBlackManager>   
{
	Asset::WhiteList _white_list;
	Asset::BlackList _black_list;
public:
	static WhiteBlackManager& Instance()
	{
		static WhiteBlackManager _instance;
		return _instance;
	}

	bool Load()
	{
		_white_list.set_open(false);
		_white_list.add_ip_address("127.0.0.1");
		DEBUG("例子:{}", _white_list.DebugString());

		std::ifstream wfi("WhiteList", std::ifstream::in);
		pb::io::IstreamInputStream wh_iis(&wfi);
		auto result = pb::TextFormat::Parse(&wh_iis, &_white_list);
		wfi.close();

		if (!result) return false;
		
		std::ifstream bfi("BlackList", std::ifstream::in);
		pb::io::IstreamInputStream bl_iis(&bfi);
		result = pb::TextFormat::Parse(&bl_iis, &_black_list);
		bfi.close();

		if (!result) return false;

		return true;
	}

	bool IsBlack(const std::string& ip_address)
	{
		auto it = std::find(_black_list.ip_address().begin(), _black_list.ip_address().end(), ip_address);
		if (it == _black_list.ip_address().end()) return false;

		return true;
	}
	
	bool IsWhite(const std::string& ip_address)
	{
		auto it = std::find(_white_list.ip_address().begin(), _white_list.ip_address().end(), ip_address);
		if (it == _white_list.ip_address().end()) return false;

		return true;
	}
	
	bool EnabledWhite() { return _white_list.open(); }
	bool EnabledBlack() { return _black_list.open(); }
};

#define WhiteBlackInstance WhiteBlackManager::Instance()
#define WBInstance WhiteBlackManager::Instance()

}
