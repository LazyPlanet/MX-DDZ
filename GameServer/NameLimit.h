#pragma once

#include <memory>
#include <functional>
#include <fstream>
#include <vector>
#include <string>

#include <boost/algorithm/searching/boyer_moore.hpp>
#include <boost/algorithm/string.hpp>

#include "P_Header.h"
#include "CommonUtil.h"
#include "MXLog.h"

namespace Adoter
{
using namespace boost::algorithm;

class NameLimit : public std::enable_shared_from_this<NameLimit>   
{
	std::vector<std::string> _names;
public:
	static NameLimit& Instance()
	{
		static NameLimit _instance;
		return _instance;
	}

	bool Load()
	{
		std::ifstream fi("PingBi.txt", std::ifstream::in);

		string name;  
	    while (fi >> name) _names.push_back(name);

		DEBUG("加载屏蔽字库:{}", _names.size());
		
		return true;
	}

	bool IsValid(std::string name)
	{
		boost::trim(name); 

		boyer_moore<std::string::const_iterator> search(name.begin(), name.end());

		for (const auto& n : _names)
		{
			auto it = search(n.begin(), n.end()).first;

			if (it != n.end())
			{
				ERROR("名字:{} 非法", name);

				return false;
			}
		}

		return true; 
	}
};

#define NameLimitInstance NameLimit::Instance()

}
