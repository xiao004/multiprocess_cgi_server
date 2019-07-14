#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>

#include "processpool.h"

// 处理 http 请求的项目路径
const char *path = "/home/www/helloworld\0";

// 用于处理客户 cgi 请求的类，可以作为 processpool 类的模板参数
class cgi_conn {
public:
	cgi_conn(){}
	~cgi_conn(){}

	// 初始化客户连接，清空读缓冲区
	// 在 processpool 子进程函数中调用（有新用户连接到来时）
	void init(int epollfd, int sockfd, const sockaddr_in &client_addr) {
		m_epollfd = epollfd;
		m_sockfd = sockfd;
		m_address = client_addr;
		memset(m_buf, '\0', BUFFER_SIZE);
		m_read_idx = 0;
	}

	// 处理 cgi 的业务逻辑
	// 在 processpool 子进程函数中调用（连接上有用户请求到来时）
	void process() {
		int idx = 0;
		int ret = -1;

		// 循环读取和分析客户数据
		while(true) {
			idx = m_read_idx;
			ret = recv(m_sockfd, m_buf + idx, BUFFER_SIZE - 1 - idx, 0);

			// 如果操作发生错误，则关闭客户连接。如果暂时无数据可读则退出循环
			if(ret < 0) {
				if(errno != EAGAIN) {
					removefd(m_epollfd, m_sockfd);
				}
				break;

			} else if(ret == 0) {
				// 如果对方关闭连接，则服务器也关闭连接
				removefd(m_epollfd, m_sockfd);
				break;

			} else {
				m_read_idx += ret;
				printf("user content is:\n %s\n", m_buf);

				// 如果遇到 "\r\n"，则开始处理客户请求
				for(; idx < m_read_idx; ++idx) {
					if((idx >= 1) && (m_buf[idx - 1] == '\r') && (m_buf[idx] == '\n')) {
						break;
					}
				}

				// 如果没有遇到 "\r\n" 则需要读取更多客户数据
				if(idx == m_read_idx) {
					continue;
				}

				m_buf[idx - 1] == '\0';

				// url 路径
				char file_name[100];
				memcpy(file_name, path, strlen(path));
				bool start = false;//标志是否遇到了m_buf中的第一个 '/'
				int pos = strlen(path);//表示 file_name 的下标
				file_name[pos] = '\0';

				//printf("%s\n", file_name);
				// printf("%d\n", idx);

				for(int i = 0; i < idx; ++i) {
					if(start == false && m_buf[i] == '/') {
						start = true;
					}

					if(start && (m_buf[i] == '?' || m_buf[i] == ' ')) {
						break;
					}
					
					// printf("%d %d\n", i, start);

					if(start) {
						file_name[pos++] = m_buf[i];
					}
				}
				file_name[pos] = '\0';

				printf("---------------\n");
				printf("%s\n", file_name);

				printf("%s\n", m_buf);

				// 判断客户要运行的 cgi 程序是否存在
				if(access(file_name, F_OK) == -1) {
					removefd(m_epollfd, m_sockfd);
					break;
				}

				// 创建子程序来执行 cgi 程序
				ret = fork();
				if(ret == -1) {
					removefd(m_epollfd, m_sockfd);
					br
				} else if(ret > 0) {
					// 在父进程中只需要关闭连接
					removefd(m_epollfd, m_sockfd);
					break;

				} else {
					// 子进程将标准输出重定向到 m_sockfd 并执行 cgi 程序
					close(STDOUT_FILENO);
					dup(m_sockfd);
					execl(file_name, m_buf, NULL);
					exit(0);
				}
			}
		}
	}

private:
	// 读缓冲区大小
	static const int BUFFER_SIZE = 1024;

	// 当前进程的 epoll 文件描述符，一个进程中的所有用户共享
	static int m_epollfd;

	// 连接 socket
	int m_sockfd;

	// 客户连接地址
	sockaddr_in m_address;

	// 读缓冲区
	char m_buf[BUFFER_SIZE];

	// 标记读缓冲区以及读入的客户端数据的最后一个字节的下一个位置
	int m_read_idx;
};

int cgi_conn::m_epollfd = -1;


int main(int argc, char const *argv[]) {
	if(argc <= 2) {
		printf("usage: %s ip_address port_number\n", basename(argv[0]));
		return 1;
	}

	const char *ip = argv[1];
	int port = atoi(argv[2]);

	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);

	int ret = 0;
	struct sockaddr_in address;
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address.sin_addr);
	address.sin_port = htons(port);

	ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
	assert(ret != -1);

	ret = listen(listenfd, 5);
	assert(ret != -1);

	processpool<cgi_conn>* pool = processpool<cgi_conn>::create(listenfd, 4);
	if(pool) {
		pool->run();
		delete pool;
	}

	// 哪个函数创建的文件描述符就哪个文件关闭
	close(listenfd);

	return 0;
}
