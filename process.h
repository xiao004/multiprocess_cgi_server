//描述一个子进程的类

#ifndef PROCESS_H
#define PROCESS_H

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


// 描述一个子进程的类
class process {
public:
	process() : m_pid(-1) {}

public:
	// 目标子进程的 pid
	pid_t m_pid;

	// 父进程和子进程通信的管道
	int m_pipefd[2];
	
};

#endif