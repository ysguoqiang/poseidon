// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2015, LH_Mouse. All wrongs reserved.

#ifndef POSEIDON_THREAD_HPP_
#define POSEIDON_THREAD_HPP_

#include "cxx_util.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>

namespace Poseidon {

class Thread : NONCOPYABLE {
private:
	class Impl;

private:
	boost::shared_ptr<Impl> m_impl;

public:
	Thread() NOEXCEPT;
	explicit Thread(boost::function<void ()> proc);
	~Thread(); // if(joinable()){ std::terminate(); }

public:
	void swap(Thread &rhs) NOEXCEPT;

	bool joinable() const NOEXCEPT;
	void join();
	void detach();
};

inline void swap(Thread &lhs, Thread &rhs) NOEXCEPT {
	lhs.swap(rhs);
}

}

#endif