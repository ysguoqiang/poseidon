// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2015, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "exception.hpp"
#include "../log.hpp"

namespace Poseidon {

namespace {
	const OptionalMap EMPTY_HEADERS;

	SharedNts replaceWithDefault(SharedNts message){
		if(message[0] == (char)0xFF){
			message = sslit("\xFF     <Use default HTML page>");
		}
		return STD_MOVE(message);
	}
}

namespace Http {
	Exception::Exception(const char *file, std::size_t line, StatusCode statusCode, OptionalMap headers, SharedNts message)
		: ProtocolException(file, line, replaceWithDefault(STD_MOVE(message)), static_cast<long>(statusCode))
	{
		if(!headers.empty()){
			m_headers = boost::make_shared<OptionalMap>(STD_MOVE(headers));
		}

		LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO,
			"Http::Exception: statusCode = ", statusCode, ", what = ", what());
	}
	Exception::Exception(const char *file, std::size_t line, StatusCode statusCode, SharedNts message)
		: ProtocolException(file, line, replaceWithDefault(STD_MOVE(message)), static_cast<long>(statusCode))
	{
		LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO,
			"Http::Exception: statusCode = ", statusCode, ", what = ", what());
	}
	Exception::~Exception() NOEXCEPT {
	}

	const OptionalMap &Exception::headers() const NOEXCEPT {
		return m_headers ? *m_headers : EMPTY_HEADERS;
	}
}

}
