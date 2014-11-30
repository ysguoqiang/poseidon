// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014, LH_Mouse. All wrongs reserved.

#ifndef POSEIDON_MYSQL_CALLBACKS_HPP_
#define POSEIDON_MYSQL_CALLBACKS_HPP_

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <vector>

namespace Poseidon {

class MySqlObjectBase;

typedef boost::shared_ptr<MySqlObjectBase> (*MySqlObjectFactoryProc)();

typedef boost::function<
	void (boost::shared_ptr<MySqlObjectBase> object, bool found)
	> MySqlAsyncLoadCallback;

typedef boost::function<
	void (std::vector<boost::shared_ptr<MySqlObjectBase> > objects)
	> MySqlBatchAsyncLoadCallback;

}

#endif