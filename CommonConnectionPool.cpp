#include "CommonConnectionPool.h"
#include "public.h"

// �̰߳�ȫ���������������ӿ�
ConnectionPool* ConnectionPool::getConnectionPool()
{
	static ConnectionPool pool;//C++11��̬������ʼ�����̰߳�ȫ
	return &pool;
}

// �������ļ��м���������
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
		int idx = str.find('=', 0);//����index
		if (idx == -1) // ��Ч��������
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

// ���ӳصĹ���
ConnectionPool::ConnectionPool()
{
	// ������������
	if (!loadConfigFile())
	{
		return;
	}
	// ������ʼ����������
	for (int i = 0; i < _initSize; ++i)
	{
		Connection* p = new Connection();
		p->connect(_ip, _port, _username, _password, _dbname);
		p->refreshAliveTime();// ˢ��һ�¿�ʼ���е���ʼʱ��
		_connectionQue.push(p);
		_connectionCnt++;
	}

	// ����һ���µ��̣߳���Ϊ���ӵ�������
	thread produce(std::bind(&ConnectionPool::produceConnectionTask, this));
	produce.detach();//�����߳̽���ʱ��������ʱ�⸺�����������߳���ص���Դ

	// ����һ���µĶ�ʱ�̣߳�ɨ�賬��maxIdleTimeʱ��Ŀ������ӣ����ж��ڵ����ӻ���
	thread scanner(std::bind(&ConnectionPool::scannerConnectionTask, this));
	scanner.detach();
}

// �����ڶ������߳��У�ר�Ÿ�������������
void ConnectionPool::produceConnectionTask()
{
	while (true)
	{
		unique_lock<mutex> lock(_queueMutex);
		while (!_connectionQue.empty())
		{
			//���в��գ��˴������߳̽���ȴ�״̬
			cv.wait(lock);//���� �߳�����ֱ�������� �ټ��������ٽ���Դ
		}

		// ��������û�е������ޣ����������µ�����
		if (_connectionCnt < _maxSize)
		{
			Connection* p = new Connection();
			p->connect(_ip, _port, _username, _password, _dbname);
			p->refreshAliveTime();
			_connectionQue.push(p);
			_connectionCnt++;
		}

		// ֪ͨ�������̣߳���������������
		cv.notify_all();
	}
}


// ���ⲿ�ṩ�ӿڣ������ӳ��л�ȡһ�����õĿ�������
shared_ptr<Connection> ConnectionPool::getConnection()
{
	//�����ٽ���Դ����
	unique_lock<mutex> lock(_queueMutex);
	while (_connectionQue.empty())
	{
		/*
		wait_for����no_timeout,timeout
		������no_timeout���������ѣ����������̵߳��ȵ��¶�����Ϊ�գ�������Ҫ�ٴη��ʶ����Ƿ�Ϊ��
		*/
		if (cv_status::timeout == cv.wait_for(lock, chrono::milliseconds(_connectionTimeout)))
		{
			if (_connectionQue.empty())
			{
				LOG("��ȡ�������ӳ�ʱ��...��ȡ����ʧ��!");
				return nullptr;
			}
		}
	}

	/*
	shared_ptr����ָ������ʱ�����connection��Դֱ��delete�����൱��
	����connection������������connection�ͱ�close���ˡ�
	������Ҫ�Զ���shared_ptr���ͷ���Դ�ķ�ʽ����connectionֱ�ӹ黹��queue����
	*/
	shared_ptr<Connection> sp(_connectionQue.front(),
		[&](Connection* pcon)
	{
		unique_lock<mutex> lock(_queueMutex);
		pcon->refreshAliveTime(); // ˢ��һ�¿�ʼ���е���ʼʱ��
		_connectionQue.push(pcon);
	});

	_connectionQue.pop();
	if (_connectionQue.empty())
	{
		cv.notify_all();
	}
	return sp;
}

// ɨ�賬��maxIdleTimeʱ��Ŀ������ӣ��������ӻ���
void ConnectionPool::scannerConnectionTask()
{
	while (true)
	{
		// ͨ��sleepģ�ⶨʱЧ��
		this_thread::sleep_for(chrono::milliseconds(_maxIdleTime));//ms

		// ɨ���������У��ͷŶ��������
		unique_lock<mutex> lock(_queueMutex);
		while (_connectionCnt > _initSize)
		{
			Connection* p = _connectionQue.front();
			if (p->getAliveeTime() >= (_maxIdleTime * 1000))
			{
				_connectionQue.pop();
				_connectionCnt--;
				delete p; // ����~Connection()�ͷ�����
			}
			else
			{
				// ��ͷ������û�г���_maxIdleTime���������ӿ϶�û��
				break;
			}
		}
	}
}












