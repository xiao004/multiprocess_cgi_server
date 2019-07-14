//进程池类

#ifndef PROCESSPOLL_H
#define PROCESSPOLL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "process.h"


//进程池类
template <typename T>
class processpool {
private:
	//使用单例模式, 将构造函数定义为私有的
	// listenfd 为监听 socket
	// 默认创建 4 个子进程
	processpool(int listenfd, int process_number = 4);

public:
	//获取实列的函数
	static processpool<T>* create(int listenfd, int process_number = 8) {
		if(!m_instance) {
			m_instance = new processpool<T>(listenfd, process_number);
		}

		return m_instance;
	}

	~processpool() {
		delete [] m_sub_process;
	}

	// 启动进程池
	void run();

private:
	// 统一事件源
	void setup_sig_pipe();
	// 父进程中运行的逻辑代码
	virtual void run_parent();
	// 子进程中运行的逻辑代码
	void run_child();

private:
	// 进程池默认允许的最大子进程数量为 8
	static const int MAX_PROCESS_NUMBER = 8;
	
	// 每个子进程最多能处理的客户数量
	static const int USER_PER_PROCESS = 65536;

	// epoll 最多能处理的事件数量
	static const int MAX_EVENT_NUMBER = 10000;

	// 进程池中的进程总数
	int m_process_number;

	// 子进程在池中的序号, 从 0 开始. 进程中的 m_idx 值为 -1
	int m_idx;

	// 每个进程都有一个 epoll 内核事件表, 用 m_epollfd 标识
	int m_epollfd;

	// 监听 socket
	int m_listenfd;

	// 进程通过 m_stop 来决定是否停止运行
	int m_stop;

	// 保存所有子进程的描述信息
	process *m_sub_process;

	// 进程池的静态实列
	static processpool<T> *m_instance;
};

// 类外初始化进程池类的静态实列变量
template<typename T>
processpool<T>* processpool<T>::m_instance = NULL;


// 用于处理信号的管道, 以实现统一事件源
static int sig_pipefd[2];


// 用 static 修饰的函数，限定在本源码文件中，不能被本源码文件以外的代码文件调用。而普通的函数，默认是 extern 的，也就是说它可以被其它代码文件调用
// 其他文件中可以定义相同名字的函数，不会发生冲突
// 静态函数不能被其他文件所用
// 子进程会复制父进程中的静态数据(还有堆数据, 栈数据)

// 将 fd 设置为非阻塞的
static int setnonblocking(int fd) {
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;

	fcntl(fd, F_SETFL, new_option);

	return old_option;
}


// 在 epollfd 内核事件表中注册 fd 上的 EPOLLIN 事件且开启 ET 模式
static void addfd(int epollfd, int fd) {
	epoll_event event;

	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;

	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

	// epoll 的 et 模式下注册事件所属文件描述符必须为非阻塞的
	setnonblocking(fd);
}


// 从 epollfd 内核事件表中删除 fd 上的所有注册事件
static void removefd(int epollfd, int fd) {
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}


static void sig_handler(int sig) {
	int save_errno = errno;
	int msg = sig;

	send(sig_pipefd[1], (char*)&msg, 1, 0);

	errno = save_errno;
}


static void addsig(int sig, void(hanler)(int), bool restart = true) {
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));

	sa.sa_handler = hanler;

	if(restart) {
		sa.sa_flags |= SA_RESTART;
	}

	sigfillset(&sa.sa_mask);

	assert(sigaction(sig, &sa, NULL) != -1);
}


// 进程池构造函数. 参数 listenfd 是监听 socket, 它必须在创建进程池之前被创建, 否则子进程无法直接引用它
// 参数 process_number 指定进程池中子进程的数量
template <typename T>
processpool<T>::processpool(int listenfd, int process_number) 
	: m_listenfd(listenfd), m_process_number(process_number), m_idx(-1), m_stop(false) {

		assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));

		// 子进程对象数组
		m_sub_process = new process[process_number];
		assert(m_sub_process);

		// 创建 process_number 个子进程, 并建立它们和父进程之间的管道
		for(int i = 0; i < process_number; ++i) {
			// 创建一个 socketpair 双向管道
			int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
			assert(ret == 0);

			m_sub_process[i].m_pid = fork();
			assert(m_sub_process[i].m_pid >= 0);

			// 在父进程中
			if(m_sub_process[i].m_pid > 0) {
 				close(m_sub_process[i].m_pipefd[1]);
 				continue;

			} else {
				// 在子进程中
				close(m_sub_process[i].m_pipefd[0]);
				// 子进程在进程池中的编号
				m_idx = i;
				break;
			}
		}
}


// 统一事件源
template <typename T>
void processpool<T>::setup_sig_pipe() {
	// 每个子进程都有一个 epoll 事件监听表
	m_epollfd = epoll_create(5);
	assert(m_epollfd != -1);

	// 创建信号管道
	int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
	assert(ret != -1);

	setnonblocking(sig_pipefd[1]);

	// 将信号管道 sig_pipefd[0] 端注册到每个子进程 m_epollfd 内核事件表上
	// 使每个子进程都能从 sig_pipefd[0] 接收到付进程从 sig_pipe[1] 发过来的信号
	addfd(m_epollfd, sig_pipefd[0]);

	// 设置信号处理函数
	// 子进程状态发生变化(退出或暂停信号)
	addsig(SIGCHLD, sig_handler);
	// 终止进程信号 kill
	addsig(SIGTERM, sig_handler);
	// 键盘输入 ctrl + c
	addsig(SIGINT, sig_handler);
	// 往读端被关闭的管道或 socket 写数据
	addsig(SIGPIPE, SIG_IGN);
}


// 父进程中 m_idx 值为－１，子进程中 m_idx 值大于等于 0. 可以据此判断接下来要运行父进程还是子进程中的代码
template<typename T>
void processpool<T>::run() {
	if(m_idx != -1) {
		run_child();
		return;
	}

	run_parent();
}


template<typename T>
void processpool<T>::run_child() {

	setup_sig_pipe();

	// 每个子进程都通过其在进程池中的序号值 m_idx 找到与父进程通信的管道
	int pipefd = m_sub_process[m_idx].m_pipefd[1];

	// 子进程需要监听管道文件描述符 pipefd，因为父进程将通过它来通知子进程 accept 新连接
	addfd(m_epollfd, pipefd);

	epoll_event events[MAX_PROCESS_NUMBER];
	T *users = new T[USER_PER_PROCESS];
	assert(users);

	int number = 0;
	int ret = -1;

	while(!m_stop) {
		number = epoll_wait(m_epollfd, events, MAX_PROCESS_NUMBER, -1);

		if((number < 0) && (errno != EINTR)) {
			printf("epoll failure\n");
			break;
		}

		for(int i = 0; i < number; i++) {
			int sockfd = events[i].data.fd;

			// 从消息管道接收到父进程的信号，即有新客户到来
			if((sockfd == pipefd) && (events[i].events & EPOLLIN)) {
				int client = 0;

				// 从父，子进程之间的管道读取数据，并将结果保存到变量 client 中．如果读取成功，则表示有新客户连接到来
				ret = recv(sockfd, (char*)&client, sizeof(client), 0);

				if(((ret < 0) && (errno != EAGAIN)) || ret == 0) {
					continue;

				} else {
					struct sockaddr_in client_address;
					socklen_t client_addrlength = sizeof(client_address);

					// 从 socket 监听上获取新到来的客户连接
					int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);

					if(connfd < 0) {
						printf("errno is: %s\n", strerror(errno));
						continue;
					}

					// 将读取到的新用户连接上的可读事件注册到本进程的 epoll 内核事件表上
					addfd(m_epollfd, connfd);

					// 模板类 T 必须实现 init 方法，以初始化一个客户连接．我们直接使用 connfd 来索引逻辑处理对象(T 类型对象)，以提高程序效率
					users[connfd].init(m_epollfd, connfd, client_address);

				}

			} else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN)) {
				// 处理子进程接收到的信号
				int sig;
				char signals[1024];

				ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);

				if(ret <= 0) {
					continue;

				} else {
					for(int i = 0; i < ret; ++i) {
						switch(signals[i]) {
							// 子进程状态发生变化(退出或暂停信号)
							case SIGCHLD:{
								pid_t pid;
								int stat;

								while((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
									continue;
								}
								break;
							}

							// 终止进程信号 kill
							case SIGTERM:
							// 键盘输入 ctrl + c
							case SIGINT:{
								m_stop = true;
								break;
							}

							default:{
								break;
							}
						}
					}
				}
			} else if(events[i].events & EPOLLIN) {
				// 如果是其他可读数据，则必然是客户请求到来．调用逻辑处理对象的 process 方法处理之
				users[sockfd].process();

			} else {
				continue;
			}
		}
	}

	// 文件描述符以及堆内存由哪个函数创建就应该由哪个函数释放
	delete []users;
	users = NULL;
	close(pipefd);
	close(m_epollfd);
}


template <typename T>
void processpool<T>::run_parent() {
	setup_sig_pipe();

	// 父进程监听 m_listenfd
	addfd(m_epollfd, m_listenfd);

	epoll_event events[MAX_PROCESS_NUMBER];
	int m_sub_process_counter = 0;
	int new_conn = 1;
	int number = 0;
	int ret = -1;

	while(!m_stop) {
		number = epoll_wait(m_epollfd, events, MAX_PROCESS_NUMBER, -1);

		if((number < 0) && (errno != EINTR)) {
			printf("epoll failure\n");
			break;
		}

		for(int i = 0; i < number; ++i) {
			int sockfd = events[i].data.fd;

			if(sockfd == m_listenfd) {
				// 如果有新的连接到来，采用 round Robin 方式将其分配给一个子进程处理
				int i = m_sub_process_counter;
				do {
					if(m_sub_process[i].m_pid != -1) {
						break;
					}

					i = (i + 1) % m_process_number;

				} while(i != m_sub_process_counter);

				if(m_sub_process[i].m_pid == -1) {
					m_stop = true;
					break;
				}

				m_sub_process_counter = (i + 1) % m_process_number;

				// 告诉第 i 个子进程有新客户到达
				send(m_sub_process[i].m_pipefd[0], (char*)&new_conn, sizeof(new_conn), 0);

				printf("send request to child: %d\n",  i);

			} else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN)) {
				int sig;
				char signals[1024];

				ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);

				if(ret <= 0) {
					continue;

				} else {
					for(int i = 0; i < ret; ++i) {
						switch(signals[i]) {
							case SIGCHLD:{
								pid_t pid;
								int stat;

								while((pid = waitpid(-1, &stat, WNOHANG)) > 0) {\
									for(int i = 0; i < m_process_number; ++i) {
										// 如果进程池中第 i 个子进程退出了，则主进程关闭相应的通信管道，并设置相应的 m_pid 为 -1 表示该子进程已退出
										if(m_sub_process[i].m_pid == pid) {
											printf("child %d leave\n", i);

											close(m_sub_process[i].m_pipefd[0]);

											m_sub_process[i].m_pid = -1;

										}

									}
								}

								// 如果所有子进程都退出了，则父进程也关闭
								m_stop = true;
								for(int i = 0; i < m_process_number; ++i) {
									if(m_sub_process[i].m_pid != -1) {
										m_stop = false;
										break;
									}
								}
								break;
							}

							case SIGTERM:
							case SIGINT:{
								// 如果父进程接收到终止信号，那么就杀死所有子进程并等待它们全部结束
								// m_stop = true;
								printf("kill all the child now\n");
								for(int i = 0; i < m_process_number; ++i) {
									int pid = m_sub_process[i].m_pid;

									if(pid != -1) {
										kill(pid, SIGTERM);
									}
								}
								break;
							}

							default:{
								break;
							}
						}
					}
				}

			} else {
				break;
			}
		}
	}

	// 由创建者关闭对应的文件描述符
	close(m_epollfd);
}



#endif