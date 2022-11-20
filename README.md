 C++11 mysql连接池


## 背景

为了处理用户的一次CURD请求，服务器端与MySql Server之间要必须经历 **TCP三次握手建立连接**，**MySQL Server连接认证**、**MySQL Server关闭连接回收资源**和**TCP四次挥手断开连接**

为了提高MySQL数据库（基于C/S设计）的访问瓶颈，建立连接池能减少这一部分的性能损耗



## 连接池功能点

基于C++语言实现的连接池，连接池一般包含了数据库连接所用的ip地址、port端口号、用户名和密码以及其它的性能参数，例如初始连接量，最大连接量，最大空闲时间、连接超时时间等



1.连接池只需要一个实例，所以ConnectionPool单例模式进行设计

2.对于建立连接所用的ip地址等参数，应该从文件中读取数据初始化这个唯一的ConnectionPool对象，方便创建新的连接Connection



3.空闲连接Connection全部维护在一个线程安全的queue队列中，使用mutex互斥锁保证队列的线程安全



4.如果Connection队列为空，还需要再获取连接，此时需要由生产者动态创建连接，上限数量是maxSize



5.队列中空闲连接时间超过maxIdleTime的就要被释放掉，只保留初始的initSize个连接就可以了，这个

功能点需要放在定时的线程中去做



6.如果Connection队列为空，而此时连接的数量已达上限maxSize，那么等待connectionTimeout时间

如果还获取不到空闲的连接，那么获取连接失败，此处从Connection队列获取空闲连接，可以使用带

超时时间的mutex互斥锁来实现连接超时时间



7.用户获取的连接用shared_ptr智能指针来管理，用lambda表达式定义智能指针的删除器（不真正释放

连接，而是把连接归还到连接池中）



8.连接的生产和连接的消费采用生产者-消费者线程模型来设计，使用了线程间的同步通信机制条件变量

和互斥锁



## 连接类

Connection.h和Connection.cpp

Connection.h提供mysql数据库初始化，建立连接，释放连接，执行CURD，该连接存活时间的接口

~~~c++
#pragma once
#include <mysql.h>
#include <string>
#include <ctime>
using namespace std;
/*
实现MySQL数据库的操作
*/
class Connection
{
public:
	// 初始化数据库连接
	Connection();
	// 释放数据库连接资源
	~Connection();
	// 连接数据库
	bool connect(string ip, 
		unsigned short port, 
		string user, 
		string password,
		string dbname);
	// 更新操作 insert、delete、update
	bool update(string sql);
	// 查询操作 select
	MYSQL_RES* query(string sql);

	//刷新一下连接的起始的空闲时间点
	void refreshAliveTime() { _alivetime = clock(); }
	//返回存活的时间
	clock_t getAliveeTime()const { return clock() - _alivetime; }
private:
	MYSQL *_conn; // 表示和MySQL Server的一条连接
	clock_t _alivetime;//记录进入空闲状态后的起始存活时间
};
~~~



Connection.cpp使用第三方API实现

~~~c++
#include "Connection.h"
#include"public.h"
#include <iostream>

using namespace std;

Connection::Connection()
{
	// 初始化数据库连接
	_conn = mysql_init(nullptr);
}

Connection::~Connection()
{
	// 释放数据库连接资源
	if (_conn != nullptr)
		mysql_close(_conn);
}

bool Connection::connect(string ip, unsigned short port, 
	string username, string password, string dbname)
{
	// 连接数据库
	MYSQL *p = mysql_real_connect(_conn, ip.c_str(), username.c_str(),
		password.c_str(), dbname.c_str(), port, nullptr, 0);
	return p != nullptr;
}

bool Connection::update(string sql)
{
	// 更新操作 insert、delete、update
	if (mysql_query(_conn, sql.c_str()))
	{
		LOG("更新失败:" + sql);
		return false;
	}
	return true;
}

MYSQL_RES* Connection::query(string sql)
{
	// 查询操作 select
	if (mysql_query(_conn, sql.c_str()))
	{
		LOG("查询失败:" + sql);
		return nullptr;
	}
	return mysql_use_result(_conn);
}
~~~





## 连接池类

CommonConnectionPool.h和CommonConnectionPool.cpp

CommonConnectionPool对外提供连接池的唯一实例，获取可用连接的接口

内部提供读取文件的接口，生产者线程函数，定时线程函数，并在构造函数里定义相关线程类

~~~c++
#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <iostream>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <memory>
#include <functional>
using namespace std;
#include "Connection.h"

/*
连接池只需要一个实例，所以ConnectionPool以单例模式进行设计
*/
class ConnectionPool
{
public:
	//获取连接池实例
	static ConnectionPool* getConnectionPool();

	// 给外部提供接口，从连接池中获取一个可用的空闲连接
	shared_ptr<Connection> getConnection();//消费者线程
private:
	// 从配置文件中加载配置项
	bool loadConfigFile();

	// 运行在独立的线程中，专门负责生产新连接
	void produceConnectionTask();//生产者线程

	// 扫描超过maxIdleTime时间的空闲连接，进行对于的连接回收
	void scannerConnectionTask();//定时线程

	ConnectionPool();//派生类和外界无法实例化

	string _ip; // mysql的ip地址
	unsigned short _port; // mysql的端口号 3306
	string _username; // mysql登录用户名
	string _password; // mysql登录密码
	string _dbname; // 连接的数据库名称
	int _initSize; // 连接池的初始连接量
	int _maxSize; // 连接池的最大连接量
	int _maxIdleTime; // 连接池最大空闲时间
	int _connectionTimeout; // 连接池获取连接的超时时间

	queue<Connection*> _connectionQue; // 存储mysql连接的队列
	mutex _queueMutex; // 维护连接队列的线程安全互斥锁
	atomic_int _connectionCnt; // 记录连接所创建的connection连接的总数量 
	condition_variable cv; // 设置条件变量，用于连接生产线程和连接消费线程的通信
};
~~~



~~~c++
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

	while (!feof(pf))//判断是否到文件尾EOF
	{
        //在一行行读的过程中处理KEY=VALUES
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
	// 加载配置项
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
			cv.wait(lock);//解锁 线程进入等待态直到条件满足变阻塞 再抢锁进入就绪态
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
	cv.notify_all();////多生产，少消费
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

~~~



























