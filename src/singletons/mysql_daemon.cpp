// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2015, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "mysql_daemon.hpp"
#include "main_config.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>
#include "../mysql/object_base.hpp"
#include "../mysql/exception.hpp"
#include "../mysql/connection.hpp"
#include "../mysql/thread_context.hpp"
#include "../thread.hpp"
#include "../mutex.hpp"
#include "../condition_variable.hpp"
#include "../atomic.hpp"
#include "../exception.hpp"
#include "../log.hpp"
#include "../raii.hpp"
#include "../multi_index_map.hpp"
#include "../job_base.hpp"
#include "../profiler.hpp"
#include "../time.hpp"
#include "../errno.hpp"

namespace Poseidon {

namespace {
	std::string		g_serverAddr		= "localhost";
	unsigned		g_serverPort		= 3306;
	std::string		g_username			= "root";
	std::string		g_password			= "root";
	std::string		g_schema			= "poseidon";
	bool			g_useSsl			= false;
	std::string		g_charset			= "utf8";

	std::string		g_dumpDir			= "";
	std::size_t		g_maxThreads		= 3;
	boost::uint64_t	g_saveDelay			= 5000;
	boost::uint64_t	g_reconnDelay		= 10000;
	std::size_t		g_maxRetryCount		= 3;
	boost::uint64_t	g_retryInitDelay	= 1000;

	// 对于日志文件的写操作应当互斥。
	Mutex g_dumpMutex;

	// 回调函数任务。
	class SaveCallbackJob : public JobBase {
	private:
		const MySql::AsyncSaveCallback m_callback;
		const bool m_succeeded;
		const boost::uint64_t m_autoId;
		const MySql::ExceptionCallback m_except;

	public:
		SaveCallbackJob(MySql::AsyncSaveCallback callback, bool succeeded, boost::uint64_t autoId,
			MySql::ExceptionCallback except)
			: m_callback(STD_MOVE_IDN(callback)), m_succeeded(succeeded), m_autoId(autoId)
			, m_except(STD_MOVE_IDN(except))
		{
		}

	private:
		boost::weak_ptr<const void> getCategory() const OVERRIDE {
			return VAL_INIT;
		}
		void perform() const OVERRIDE {
			PROFILE_ME;

			try {
				m_callback(m_succeeded, m_autoId);
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class LoadCallbackJob : public JobBase {
	private:
		const MySql::AsyncLoadCallback m_callback;
		const bool m_found;
		const MySql::ExceptionCallback m_except;

	public:
		LoadCallbackJob(MySql::AsyncLoadCallback callback, bool found,
			MySql::ExceptionCallback except)
			: m_callback(STD_MOVE_IDN(callback)), m_found(found)
			, m_except(STD_MOVE_IDN(except))
		{
		}

	private:
		boost::weak_ptr<const void> getCategory() const OVERRIDE {
			return VAL_INIT;
		}
		void perform() const OVERRIDE {
			PROFILE_ME;

			try {
				m_callback(m_found);
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class BatchLoadCallbackJob : public JobBase {
	private:
		const MySql::BatchAsyncLoadCallback m_callback;
		const std::vector<boost::shared_ptr<MySql::ObjectBase> > m_objects;
		const MySql::ExceptionCallback m_except;

	public:
		BatchLoadCallbackJob(MySql::BatchAsyncLoadCallback callback,
			std::vector<boost::shared_ptr<MySql::ObjectBase> > objects,
			MySql::ExceptionCallback except)
			: m_callback(STD_MOVE_IDN(callback)), m_objects(STD_MOVE(objects))
			, m_except(STD_MOVE_IDN(except))
		{
		}

	private:
		boost::weak_ptr<const void> getCategory() const OVERRIDE {
			return VAL_INIT;
		}
		void perform() const OVERRIDE {
			PROFILE_ME;

			try {
				m_callback(m_objects);
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	// 数据库线程操作。
	class OperationBase : NONCOPYABLE {
	public:
		virtual ~OperationBase(){
		}

	public:
		// 写入合并。
		virtual boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const = 0;
		virtual const char *getTableName() const = 0;
		virtual void execute(std::string &query, MySql::Connection &conn) = 0;
	};

	class SaveOperation : public OperationBase {
	private:
		const boost::shared_ptr<const MySql::ObjectBase> m_object;
		const bool m_toReplace;

		MySql::AsyncSaveCallback m_callback;
		const MySql::ExceptionCallback m_except;

	public:
		SaveOperation(boost::shared_ptr<const MySql::ObjectBase> object, bool toReplace,
			MySql::AsyncSaveCallback callback, MySql::ExceptionCallback except)
			: m_object(STD_MOVE(object)), m_toReplace(toReplace)
			, m_callback(STD_MOVE_IDN(callback)), m_except(STD_MOVE_IDN(except))
		{
		}

	public:
		boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const OVERRIDE {
			return m_object;
		}
		const char *getTableName() const OVERRIDE {
			return m_object->getTableName();
		}
		void execute(std::string &query, MySql::Connection &conn){
			PROFILE_ME;

			try {
				m_object->syncGenerateSql(query, m_toReplace);
				bool succeeded = false;
				try {
					LOG_POSEIDON_DEBUG("Executing SQL in ", m_object->getTableName(), ": query = ", query);
					conn.executeSql(query);
					succeeded = true;
				} catch(MySql::Exception &e){
					LOG_POSEIDON_DEBUG("MySql::Exception: errorCode = ", e.errorCode(), ", message = ", e.what());
					if(!m_callback || (e.errorCode() != ER_DUP_ENTRY)){
						throw;
					}
				}

				if(m_callback){
					enqueueJob(boost::make_shared<SaveCallbackJob>(STD_MOVE(m_callback), succeeded, conn.getInsertId(), m_except));
				}
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class LoadOperation : public OperationBase {
	private:
		const boost::shared_ptr<MySql::ObjectBase> m_object;
		const std::string m_query;

		MySql::AsyncLoadCallback m_callback;
		const MySql::ExceptionCallback m_except;

	public:
		LoadOperation(boost::shared_ptr<MySql::ObjectBase> object, std::string query,
			MySql::AsyncLoadCallback callback, MySql::ExceptionCallback except)
			: m_object(STD_MOVE(object)), m_query(STD_MOVE(query))
			, m_callback(STD_MOVE_IDN(callback)), m_except(STD_MOVE_IDN(except))
		{
		}

	public:
		boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const OVERRIDE {
			return VAL_INIT; // 不能合并。
		}
		const char *getTableName() const OVERRIDE {
			return m_object->getTableName();
		}
		void execute(std::string &query, MySql::Connection &conn){
			PROFILE_ME;

			try {
				query = m_query;

				LOG_POSEIDON_INFO("MySQL load: table = ", m_object->getTableName(), ", query = ", query);
				conn.executeSql(query);
				const bool found = conn.fetchRow();
				if(found){
					m_object->syncFetch(conn);
					m_object->enableAutoSaving();
				}

				if(m_callback){
					enqueueJob(boost::make_shared<LoadCallbackJob>(STD_MOVE(m_callback), found, m_except));
				}
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	class BatchLoadOperation : public OperationBase {
	private:
		const char *const m_tableHint;
		boost::shared_ptr<MySql::ObjectBase> (*const m_factory)();
		const std::string m_query;

		MySql::BatchAsyncLoadCallback m_callback;
		const MySql::ExceptionCallback m_except;

	public:
		BatchLoadOperation(const char *tableHint, boost::shared_ptr<MySql::ObjectBase> (*factory)(), std::string query,
			MySql::BatchAsyncLoadCallback callback, MySql::ExceptionCallback except)
			: m_tableHint(tableHint), m_factory(factory), m_query(STD_MOVE(query))
			, m_callback(STD_MOVE_IDN(callback)), m_except(STD_MOVE_IDN(except))
		{
		}

	public:
		boost::shared_ptr<const MySql::ObjectBase> getCombinableObject() const OVERRIDE {
			return VAL_INIT; // 不能合并。
		}
		const char *getTableName() const OVERRIDE {
			return m_tableHint;
		}
		void execute(std::string &query, MySql::Connection &conn){
			PROFILE_ME;

			try {
				query = m_query;

				LOG_POSEIDON_INFO("MySQL batch load: tableHint = ", m_tableHint, ", query = ", query);
				conn.executeSql(query);

				std::vector<boost::shared_ptr<MySql::ObjectBase> > objects;
				while(conn.fetchRow()){
					AUTO(object, (*m_factory)());
					object->syncFetch(conn);
					object->enableAutoSaving();
					objects.push_back(STD_MOVE(object));
				}

				if(m_callback){
					enqueueJob(boost::make_shared<BatchLoadCallbackJob>(STD_MOVE(m_callback), STD_MOVE(objects), m_except));
				}
			} catch(...){
				if(m_except){
					m_except();
				}
				throw;
			}
		}
	};

	struct OperationElement {
		boost::uint64_t dueTime;
		boost::shared_ptr<const MySql::ObjectBase> combinableObject;

		boost::shared_ptr<OperationBase> operation;

		mutable std::size_t retryCount;

		OperationElement(boost::uint64_t dueTime_, boost::shared_ptr<OperationBase> operation_)
			: dueTime(dueTime_), combinableObject(operation_->getCombinableObject())
			, operation(STD_MOVE(operation_))
			, retryCount(0)
		{
		}
	};

	MULTI_INDEX_MAP(OperationMap, OperationElement,
		MULTI_MEMBER_INDEX(dueTime)
		MULTI_MEMBER_INDEX(combinableObject)
	)

	class MySqlThread : public Thread {
	private:
		class WorkingTimeAccumulator : NONCOPYABLE {
		private:
			MySqlThread *const m_owner;
			const char *const m_table;

		public:
			WorkingTimeAccumulator(MySqlThread *owner, const char *table)
				: m_owner(owner), m_table(table)
			{
				m_owner->accumulateTimeForTable("");
			}
			~WorkingTimeAccumulator(){
				m_owner->accumulateTimeForTable(m_table);
			}
		};

		struct TableNameComparator {
			bool operator()(const char *lhs, const char *rhs) const {
				return std::strcmp(lhs, rhs) < 0;
			}
		};

	private:
		const std::size_t m_index;

		volatile bool m_running;

		mutable Mutex m_mutex;
		mutable ConditionVariable m_newOperation;
		volatile bool m_urgent; // 无视延迟写入，一次性处理队列中所有操作。
		OperationMap m_operationMap;

		// 性能统计。
		mutable Mutex m_profileMutex;
		double m_profileFlushedTime;
		std::map<const char *, double, TableNameComparator> m_profile;

	public:
		explicit MySqlThread(std::size_t index)
			: m_index(index)
			, m_running(true)
			, m_urgent(false)
			, m_profileFlushedTime(getHiResMonoClock())
		{
			Thread(boost::bind(&MySqlThread::daemonLoop, this), " D  ").swap(*this);
		}

	private:
		void accumulateTimeForTable(const char *table) NOEXCEPT {
			const AUTO(now, getHiResMonoClock());
			try {
				const Mutex::UniqueLock lock(m_profileMutex);
				m_profile[table] += now - m_profileFlushedTime;
			} catch(...){
			}
			m_profileFlushedTime = now;
		}

		bool pumpOneOperation(MySql::Connection &conn) NOEXCEPT {
			PROFILE_ME;

			typedef OperationMap::delegated_container::nth_index<0>::type::iterator OperationIterator;

			const AUTO(now, getFastMonoClock());

			OperationIterator operationIt;
			{
				const Mutex::UniqueLock lock(m_mutex);
				operationIt = m_operationMap.begin<0>();
				if(operationIt == m_operationMap.end<0>()){
					atomicStore(m_urgent, false, ATOMIC_RELEASE);
					return false;
				}
				if(atomicLoad(m_running, ATOMIC_CONSUME) && (now < operationIt->dueTime) && !atomicLoad(m_urgent, ATOMIC_CONSUME)){
					return false;
				}
			}

			boost::uint64_t newDueTime = 0;
			try {
				unsigned errorCode = 0;
				std::string errorMsg;
				std::string query;

				try {
					const WorkingTimeAccumulator profiler(this, operationIt->operation->getTableName());
					operationIt->operation->execute(query, conn);
				} catch(MySql::Exception &e){
					LOG_POSEIDON_ERROR("MySql::Exception thrown in MySQL operation: errorCode = ", e.errorCode(), ", what = ", e.what());

					errorCode = e.errorCode();
					errorMsg = e.what();
				} catch(std::exception &e){
					LOG_POSEIDON_ERROR("std::exception thrown in MySQL operation: what = ", e.what());

					errorCode = 99999;
					errorMsg = e.what();
				} catch(...){
					LOG_POSEIDON_ERROR("Unknown exception thrown in MySQL operation.");

					errorCode = 99999;
					errorMsg = "Unknown exception";
				}

				if(errorCode != 0){
					LOG_POSEIDON_ERROR("Going to retry MySQL operation: retryCount = ", operationIt->retryCount,
						", errorCode = ", errorCode, ", errorMsg = ", errorMsg);

					if(errorCode == CR_SERVER_LOST){
						newDueTime = now + g_retryInitDelay;
					} else if(operationIt->retryCount < g_maxRetryCount){
						newDueTime = now + (g_retryInitDelay << operationIt->retryCount);
						++operationIt->retryCount;
					} else {
						LOG_POSEIDON_ERROR("Max retry count exceeded.");

						if(g_dumpDir.empty()){
							LOG_POSEIDON_WARNING("MySQL dump is disabled.");
						} else {
							const AUTO(dt, breakDownTime(getLocalTime()));
							char temp[256];
							unsigned len = (unsigned)std::sprintf(temp, "%04u-%02u-%02u %05u", dt.yr, dt.mon, dt.day, (unsigned)::getpid());
							std::string dumpPath;
							dumpPath.assign(g_dumpDir);
							dumpPath.push_back('/');
							dumpPath.append(temp, len);
							dumpPath.append(".log");

							LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Creating SQL dump file: ", dumpPath);
							UniqueFile dumpFile;
							if(!dumpFile.reset(::open(dumpPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644))){
								const int errCode = errno;
								LOG_POSEIDON_FATAL("Error creating SQL dump file: dumpPath = ", dumpPath,
									", errno = ", errCode, ", desc = ", getErrorDesc(errCode));
								std::abort();
							}

							LOG_POSEIDON_INFO("Writing MySQL dump...");
							len = (unsigned)std::sprintf(temp, "%5u", errorCode);
							std::string dump;
							dump.reserve(1024);
							dump.assign("-- Error code = ");
							dump.append(temp, len);
							dump.append(", Description = ");
							dump.append(errorMsg);
							dump.append("\n");
							dump.append(query);
							dump.append(";\n\n");

							{
								const Mutex::UniqueLock lock(g_dumpMutex);

								std::size_t total = 0;
								do {
									::ssize_t written = ::write(dumpFile.get(), dump.data() + total, dump.size() - total);
									if(written <= 0){
										break;
									}
									total += static_cast<std::size_t>(written);
								} while(total < dump.size());
							}
						}

						DEBUG_THROW(Exception, sslit("Max retry count exceeded"));
					}
				}
			} catch(std::exception &e){
				LOG_POSEIDON_ERROR("std::exception thrown in MySQL operation: what = ", e.what());
			} catch(...){
				LOG_POSEIDON_ERROR("Unknown exception thrown in MySQL operation.");
			}
			if(newDueTime != 0){
				const Mutex::UniqueLock lock(m_mutex);
				const AUTO(range, m_operationMap.equalRange<1>(operationIt->combinableObject));
				for(AUTO(it, range.first); it != range.second; ++it){
					m_operationMap.setKey<1, 0>(it, newDueTime);
					++newDueTime;
				}
			} else {
				const Mutex::UniqueLock lock(m_mutex);
				m_operationMap.erase<0>(operationIt);
			}

			return true;
		}

		void daemonLoop(){
			PROFILE_ME;
			LOG_POSEIDON_INFO("MySQL thread ", m_index, " started.");

			const MySql::ThreadContext threadContext;
			boost::shared_ptr<MySql::Connection> conn;

			for(;;){
				accumulateTimeForTable("");

				while(!conn){
					LOG_POSEIDON_INFO("Connecting to MySQL server...");

					try {
						conn = MySqlDaemon::createConnection();
						LOG_POSEIDON_INFO("Successfully connected to MySQL server.");
					} catch(std::exception &e){
						LOG_POSEIDON_ERROR("std::exception thrown while connecting to MySQL server: what = ", e.what());

						const AUTO(ns, g_reconnDelay * 1000 * 1000);
						::timespec req;
						req.tv_sec = (::time_t)(ns / (1000 * 1000 * 1000));
						req.tv_nsec = (long)(ns % (1000 * 1000 * 1000));
						::nanosleep(&req, NULLPTR);
					}
				}

				while(pumpOneOperation(*conn)){
					// noop
				}

				if(!atomicLoad(m_running, ATOMIC_CONSUME)){
					break;
				}

				Mutex::UniqueLock lock(m_mutex);
				m_newOperation.timedWait(lock, 100);
			}

			LOG_POSEIDON_INFO("MySQL thread ", m_index, " stopped.");
		}

	public:
		void shutdown(){
			atomicStore(m_running, false, ATOMIC_RELEASE);
		}
		void join(){
			shutdown();
			waitTillIdle();
			Thread::join();
		}

		std::size_t getProfile(std::vector<MySqlDaemon::SnapshotElement> &ret, unsigned thread) const {
			const Mutex::UniqueLock lock(m_profileMutex);
			const std::size_t count = m_profile.size();
			ret.reserve(ret.size() + count);
			for(AUTO(it, m_profile.begin()); it != m_profile.end(); ++it){
				MySqlDaemon::SnapshotElement item;
				item.thread = thread;
				item.table = it->first;
				item.usTotal = static_cast<boost::uint64_t>(it->second * 1.0e3);
				ret.push_back(item);
			}
			return count;
		}

		void addOperation(boost::shared_ptr<OperationBase> operation, bool urgent){
			if(!atomicLoad(m_running, ATOMIC_CONSUME)){
				LOG_POSEIDON_ERROR("MySQL thread ", m_index, " is being shut down.");
				DEBUG_THROW(Exception, sslit("MySQL thread is being shut down"));
			}

			const AUTO(combinableObject, operation->getCombinableObject());
			AUTO(dueTime, getFastMonoClock());
			// 有紧急操作时无视写入延迟，这个逻辑不在这里处理。
			// if(combinableObject && !urgent){ // 这个看似合理但是实际是错的。
			if(combinableObject || urgent){ // 确保紧急操作排在其他所有操作之后。
				dueTime += g_saveDelay;
			}

			const Mutex::UniqueLock lock(m_mutex);
			if(combinableObject){
				AUTO(it, m_operationMap.find<1>(combinableObject));
				if(it != m_operationMap.end<1>()){
					m_operationMap.setKey<1, 0>(it, dueTime);
					goto _inserted;
				}
			}
			m_operationMap.insert(OperationElement(dueTime, STD_MOVE(operation)));
		_inserted:
			if(urgent){
				atomicStore(m_urgent, true, ATOMIC_RELEASE);
			}
			m_newOperation.signal();
		}
		void waitTillIdle(){
			for(;;){
				std::size_t pendingObjects;
				{
					const Mutex::UniqueLock lock(m_mutex);
					pendingObjects = m_operationMap.size();
					if(pendingObjects == 0){
						break;
					}
					atomicStore(m_urgent, true, ATOMIC_RELEASE);
					m_newOperation.signal();
				}
				LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "There are ", pendingObjects, " object(s) in my queue.");

				::timespec req;
				req.tv_sec = 0;
				req.tv_nsec = 500 * 1000 * 1000;
				::nanosleep(&req, NULLPTR);
			}
		}
	};

	volatile bool g_running = false;
	std::vector<boost::shared_ptr<MySqlThread> > g_threads;

	void commitOperation(const char *table, boost::shared_ptr<OperationBase> operation, bool urgent){
		if(g_threads.empty()){
			DEBUG_THROW(Exception, sslit("No MySQL thread is running"));
		}

		// http://www.isthe.com/chongo/tech/comp/fnv/
		std::size_t hash;
		if(sizeof(std::size_t) < 8){
			hash = 2166136261u;
			const char *p = table;
			while(*p){
				hash ^= static_cast<unsigned char>(*p);
				hash *= 16777619u;
				++p;
			}
		} else {
			hash = 14695981039346656037u;
			const char *p = table;
			while(*p){
				hash ^= static_cast<unsigned char>(*p);
				hash *= 1099511628211u;
				++p;
			}
		}
		const AUTO(threadIndex, hash % g_threads.size());
		LOG_POSEIDON_DEBUG("Assigning MySQL table `", table, "` to thread ", threadIndex);
		g_threads.at(threadIndex)->addOperation(STD_MOVE(operation), urgent);
	}
}

void MySqlDaemon::start(){
	if(atomicExchange(g_running, true, ATOMIC_ACQ_REL) != false){
		LOG_POSEIDON_FATAL("Only one daemon is allowed at the same time.");
		std::abort();
	}
	LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Starting MySQL daemon...");

	MainConfig::get(g_serverAddr, "mysql_server_addr");
	LOG_POSEIDON_DEBUG("MySQL server addr = ", g_serverAddr);

	MainConfig::get(g_serverPort, "mysql_server_port");
	LOG_POSEIDON_DEBUG("MySQL server port = ", g_serverPort);

	MainConfig::get(g_username, "mysql_username");
	LOG_POSEIDON_DEBUG("MySQL username = ", g_username);

	MainConfig::get(g_password, "mysql_password");
	LOG_POSEIDON_DEBUG("MySQL password = ", g_password);

	MainConfig::get(g_schema, "mysql_schema");
	LOG_POSEIDON_DEBUG("MySQL schema = ", g_schema);

	MainConfig::get(g_useSsl, "mysql_use_ssl");
	LOG_POSEIDON_DEBUG("MySQL use ssl = ", g_useSsl);

	MainConfig::get(g_charset, "mysql_charset");
	LOG_POSEIDON_DEBUG("MySQL charset = ", g_charset);

	MainConfig::get(g_dumpDir, "mysql_dump_dir");
	LOG_POSEIDON_DEBUG("MySQL dump dir = ", g_dumpDir);

	MainConfig::get(g_maxThreads, "mysql_max_threads");
	LOG_POSEIDON_DEBUG("MySQL max threads = ", g_maxThreads);

	MainConfig::get(g_saveDelay, "mysql_save_delay");
	LOG_POSEIDON_DEBUG("MySQL save delay = ", g_saveDelay);

	MainConfig::get(g_reconnDelay, "mysql_reconn_delay");
	LOG_POSEIDON_DEBUG("MySQL reconnect delay = ", g_reconnDelay);

	MainConfig::get(g_maxRetryCount, "mysql_max_retry_count");
	LOG_POSEIDON_DEBUG("MySQL max retry count = ", g_maxRetryCount);

	MainConfig::get(g_retryInitDelay, "mysql_retry_init_delay");
	LOG_POSEIDON_DEBUG("MySQL retry init delay = ", g_retryInitDelay);

	g_threads.resize(std::max<std::size_t>(g_maxThreads, 1));
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		LOG_POSEIDON_INFO("Creating MySQL thread ", i);
		g_threads[i] = boost::make_shared<MySqlThread>(i);
	}

	if(!g_dumpDir.empty()){
		const AUTO(placeholderPath, g_dumpDir + "/placeholder");
		LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO,
			"Checking whether MySQL dump directory is writeable: testPath = ", placeholderPath);
		UniqueFile dumpFile;
		if(!dumpFile.reset(::open(placeholderPath.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644))){
			const int errCode = errno;
			LOG_POSEIDON_FATAL("Could not create placeholder file: placeholderPath = ", placeholderPath, ", errno = ", errCode);
			std::abort();
		}
	}
}
void MySqlDaemon::stop(){
	if(atomicExchange(g_running, false, ATOMIC_ACQ_REL) == false){
		return;
	}
	LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Stopping MySQL daemon...");

	for(std::size_t i = 0; i < g_threads.size(); ++i){
		g_threads[i]->shutdown();
	}
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		LOG_POSEIDON_INFO("Stopping MySQL thread ", i);
		if(g_threads[i]->joinable()){
			g_threads[i]->join();
		}
	}
	g_threads.clear();
}

boost::shared_ptr<MySql::Connection> MySqlDaemon::createConnection(){
	return MySql::Connection::create(g_serverAddr, g_serverPort, g_username, g_password, g_schema, g_useSsl, g_charset);
}

std::vector<MySqlDaemon::SnapshotElement> MySqlDaemon::snapshot(){
	std::vector<SnapshotElement> ret;
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		g_threads[i]->getProfile(ret, i);
	}
	return ret;
}

void MySqlDaemon::waitForAllAsyncOperations(){
	for(std::size_t i = 0; i < g_threads.size(); ++i){
		g_threads[i]->waitTillIdle();
	}
}

void MySqlDaemon::enqueueForSaving(boost::shared_ptr<const MySql::ObjectBase> object, bool toReplace, bool urgent,
	MySql::AsyncSaveCallback callback, MySql::ExceptionCallback except)
{
	const AUTO(tableName, object->getTableName());
	const bool reallyUrgent = urgent || callback;
	commitOperation(tableName,
		boost::make_shared<SaveOperation>(
			STD_MOVE(object), toReplace, STD_MOVE(callback), STD_MOVE(except)),
		reallyUrgent);
}
void MySqlDaemon::enqueueForLoading(boost::shared_ptr<MySql::ObjectBase> object, std::string query,
	MySql::AsyncLoadCallback callback, MySql::ExceptionCallback except)
{
	const AUTO(tableName, object->getTableName());
	commitOperation(tableName,
		boost::make_shared<LoadOperation>(
			STD_MOVE(object), STD_MOVE(query), STD_MOVE(callback), STD_MOVE(except)),
		true);
}
void MySqlDaemon::enqueueForBatchLoading(boost::shared_ptr<MySql::ObjectBase> (*factory)(),
	const char *tableHint, std::string query,
	MySql::BatchAsyncLoadCallback callback, MySql::ExceptionCallback except)
{
	commitOperation(tableHint,
		boost::make_shared<BatchLoadOperation>(
			tableHint, factory, STD_MOVE(query), STD_MOVE(callback), STD_MOVE(except)),
		true);
}

}
