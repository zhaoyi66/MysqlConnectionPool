#include "CommonConnectionPool.h"
#include "public.h"

// 线程安全的懒汉单例函数接口
ConnectionPool* ConnectionPool::getConnectionPool()
{
	static ConnectionPool pool;//C++11后静态变量初始化是线程安全
	return &pool;
}

// 从配置文件中加载配置项
bool ConnectionPool::loadConfigFile()
{
	FILE* pf = fopen("mysql.ini", "r");
	if (pf == nullptr)
	{
		LOG("mysql.ini file is not exist!");
		return false;
	}

	while (!feof(pf))
	{
		char line[128] = { 0 };
		fgets(line, 128, pf);
		string str = line;
		//key=value\n
		int idx = str.find('=', 0);//返回index
		if (idx == -1) // 无效的配置项
		{
			continue;
		}

		int endidx = str.find('\n', idx);
		string key = str.substr(0, idx);
		string value = str.substr(idx + 1, endidx - idx - 1);

		if (key == "ip")
		{
			_ip = value;
		}
		else if (key == "port")
		{
			_port = atoi(value.c_str());
		}
		else if (key == "username")
		{
			_username = value;
		}
		else if (key == "password")
		{
			_password = value;
		}
		else if (key == "dbname")
		{
			_dbname = value;
		}
		else if (key == "initSize")
		{
			_initSize = atoi(value.c_str());
		}
		else if (key == "maxSize")
		{
			_maxSize = atoi(value.c_str());
		}
		else if (key == "maxIdleTime")
		{
			_maxIdleTime = atoi(value.c_str());
		}
		else if (key == "connectionTimeOut")
		{
			_connectionTimeout = atoi(value.c_str());
		}
	}
	return true;
}

// 连接池的构造
ConnectionPool::ConnectionPool()
{
	// 加载配置项了
	if (!loadConfigFile())
	{
		return;
	}
	// 创建初始数量的连接
	for (int i = 0; i < _initSize; ++i)
	{
		Connection* p = new Connection();
		p->connect(_ip, _port, _username, _password, _dbname);
		p->refreshAliveTime();// 刷新一下开始空闲的起始时间
		_connectionQue.push(p);
		_connectionCnt++;
	}

	// 启动一个新的线程，作为连接的生产者
	thread produce(std::bind(&ConnectionPool::produceConnectionTask, this));
	produce.detach();//当主线程结束时，由运行时库负责清理与子线程相关的资源

	// 启动一个新的定时线程，扫描超过maxIdleTime时间的空闲连接，进行对于的连接回收
	thread scanner(std::bind(&ConnectionPool::scannerConnectionTask, this));
	scanner.detach();
}

// 运行在独立的线程中，专门负责生产新连接
void ConnectionPool::produceConnectionTask()
{
	while (true)
	{
		unique_lock<mutex> lock(_queueMutex);
		while (!_connectionQue.empty())
		{
			//队列不空，此处生产线程进入等待状态
			cv.wait(lock);//解锁 线程阻塞直到被唤醒 再加锁处理临界资源
		}

		// 连接数量没有到达上限，继续创建新的连接
		if (_connectionCnt < _maxSize)
		{
			Connection* p = new Connection();
			p->connect(_ip, _port, _username, _password, _dbname);
			p->refreshAliveTime();
			_connectionQue.push(p);
			_connectionCnt++;
		}

		// 通知消费者线程，可以消费连接了
		cv.notify_all();
	}
}


// 给外部提供接口，从连接池中获取一个可用的空闲连接
shared_ptr<Connection> ConnectionPool::getConnection()
{
	//访问临界资源加锁
	unique_lock<mutex> lock(_queueMutex);
	while (_connectionQue.empty())
	{
		/*
		wait_for返回no_timeout,timeout
		当返回no_timeout被其他唤醒，可能由于线程调度导致队列又为空，所以需要再次访问队列是否为空
		*/
		if (cv_status::timeout == cv.wait_for(lock, chrono::milliseconds(_connectionTimeout)))
		{
			if (_connectionQue.empty())
			{
				LOG("获取空闲连接超时了...获取连接失败!");
				return nullptr;
			}
		}
	}

	/*
	shared_ptr智能指针析构时，会把connection资源直接delete掉，相当于
	调用connection的析构函数，connection就被close掉了。
	这里需要自定义shared_ptr的释放资源的方式，把connection直接归还到queue当中
	*/
	shared_ptr<Connection> sp(_connectionQue.front(),
		[&](Connection* pcon)
	{
		unique_lock<mutex> lock(_queueMutex);
		pcon->refreshAliveTime(); // 刷新一下开始空闲的起始时间
		_connectionQue.push(pcon);
	});

	_connectionQue.pop();
	if (_connectionQue.empty())
	{
		cv.notify_all();
	}
	return sp;
}

// 扫描超过maxIdleTime时间的空闲连接，进行连接回收
void ConnectionPool::scannerConnectionTask()
{
	while (true)
	{
		// 通过sleep模拟定时效果
		this_thread::sleep_for(chrono::milliseconds(_maxIdleTime));//ms

		// 扫描整个队列，释放多余的连接
		unique_lock<mutex> lock(_queueMutex);
		while (_connectionCnt > _initSize)
		{
			Connection* p = _connectionQue.front();
			if (p->getAliveeTime() >= (_maxIdleTime * 1000))
			{
				_connectionQue.pop();
				_connectionCnt--;
				delete p; // 调用~Connection()释放连接
			}
			else
			{
				// 队头的连接没有超过_maxIdleTime，其它连接肯定没有
				break;
			}
		}
	}
}












