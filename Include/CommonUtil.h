#pragma once

#include <random>
#include <string>

#include "Config.h"

namespace Adoter
{
	template<typename T, typename ...Args>
	std::unique_ptr<T> make_unique(Args&& ...args)
	{
		return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
	}

#define DEBUG_ASSERT(expr) \
	{ \
		auto debug = ConfigInstance.GetBool("DebugModel", true); \
		if (debug) { \
			assert(expr); \
		} else if (!(expr)) { \
			LOG(ERROR, "command: {}", #expr); \
		} \
	}

}

#define CONCAT_INTERNAL(x,y) x##y
#define CONCAT(x,y) CONCAT_INTERNAL(x,y)

template<typename T> struct ExitScope
{
	T lambda;
	ExitScope(T lambda):lambda(lambda){}
	~ExitScope(){lambda();}
	ExitScope(const ExitScope&);
private:
	ExitScope& operator =(const ExitScope&);
};

class ExitScopeHelp
{
public:
	template<typename T>
	ExitScope<T> operator+(T t){ return t;}
};

#define defer const auto& CONCAT(defer__, __LINE__) = ExitScopeHelp() + [&]()

class CommonUtil
{
public:
	static int32_t Random(int32_t min, int32_t max) //[min, max]
	{
		/*
		static std::default_random_engine _generator;
		std::uniform_int_distribution<int> distribution(min, max);
		return distribution(_generator);
		*/
		static std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(min, max);
		return dis(gen);
	}

	template<class IT, class WEIGHT>
	static int RandomByWeight(const IT& begin, const IT& end, WEIGHT w)
	{
		int sum = 0;
		for(auto it = begin; it != end; ++it) sum += w(*it);
		if(sum <= 0) return -1;
		int rnd = Random(0, sum - 1);
		size_t index = 0;
		int weight = 0;
		for(auto it = begin; it != end; ++it, ++index)
		{
			weight = w(*it);
			if(rnd < weight)
				return index;
			rnd -= weight;
		}
		return -1;
	}
	
	template<class IT, class WEIGHT>
	static IT RandomItorByWeight(const IT& begin, const IT& end, WEIGHT w)
	{
		int sum = 0;
		for(auto it = begin; it != end; ++it) sum += w(*it);
		if(sum <= 0) return end;
		int rnd = Random(0, sum - 1);
		int weight = 0;
		for(auto it = begin; it != end; ++it)
		{
			weight = w(*it);
			if(rnd < weight)
				return it;
			rnd -= weight;
		}
		return end;
	}

	//
	//排列组合,带重复元素
	//
	static bool CombinationWithRepeated(unsigned int Set, unsigned int Comb, std::vector<unsigned int> &vi)
	{
		if (Set == 0 || Comb == 0) return false;
		
		bool reach_end = false;
		for( int x = Comb - 1; x >= 0 ; --x )
		{
			if (x == 0 && vi[x] == Set - 1) return false;

			if (reach_end)
			{
				if (vi[x] != Set - 1)
				{
					unsigned int level = vi[x] + 1;
					for(unsigned int y = x; y < Comb; ++y) vi[y] = level;
						
					return true;
				}		
			}
			
			// At the end of the Set
			if (vi[x] == Set - 1)
			{
				reach_end = true;
			}
			else if (vi[x] < Set - 1)
			{
				(vi[x])++;
				return true;		
			}
		}

		return true;		
	}

	static void DeleteAllMark(std::string &s, const std::string &mark)  
	{  
	    size_t size = mark.size();  
	    while(1)  
	    {  
	        size_t pos = s.find(mark);  
	        if(pos == std::string::npos) return;  
	  
	        s.erase(pos, size);  
		}  
	}  

	static void Trim(std::string &s)
	{
		DeleteAllMark(s, " ");
	}
	  
};

