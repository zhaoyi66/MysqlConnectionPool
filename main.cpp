#include <iostream>
using namespace std;
#include "Connection.h"
#include"CommonConnectionPool.h"
#include"public.h"
int main()
{
	//��ȡ�̳߳ص�Ψһʵ��
	ConnectionPool* cp = ConnectionPool::getConnectionPool();

	//���߳�ʹ�����ӳ�
	for (int i = 0; i < 10; i++)
	{
		thread t([&]() {
			shared_ptr<Connection> sp = cp->getConnection();
			this_thread::sleep_for(chrono::milliseconds(100));
			char sql[1024] = { 0 };
			sprintf(sql, "insert into studentinfo(stuName,stuNo,score) values('%s',%s,'%f')",
				"zhang san", "111", "66");
			if (!sp->update(sql))
			{
				LOG("update error!");
			}
			});
		t.detach();
	}
	this_thread::sleep_for(chrono::seconds(5));
	return 0;
}