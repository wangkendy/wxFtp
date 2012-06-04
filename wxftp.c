#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>

#define MAX_CLIENT 128

//需要root权限，若没有root权限，请修改这两个端口号
#define SERVER_CMD_PORT 21
#define SERVER_DAT_PORT 20

//环境变量，用于获取用户的home目录
extern char **environ;

//数据链接的模式，主动或被动
enum data_conn_mode {
	kDataConnModeActive, kDataConnModePassive
};

//用户结构体
struct Client {
	int cmd_sockfd;//命令链接描述符
	int data_sockfd;//数据链接描述符
	int authorized;//用于判断用户是否认证
	char type; //'I', 'A'
	char data_ip[20];//数据链接的IP地址，字符串形式
	enum data_conn_mode mode;//数据链接的模式
	struct in_addr sin_addr;//数据链接的IP地址，结构体形式
	unsigned short data_port;//数据链接所用的端口
	char cwd[128];//用户当前目录（相对目录，并不是在文件系统的绝对目录）
	char rnfr[64];//rename from
	char user[16];//用户名
	char pass[16];//用户密码
};

int server_sockfd;//用于监听链接
int  client_sockfd;//用于保存accept函数返回的链接描述符

//用户结构体数组，用户的链接描述符作为索引
//例如，某个用户的链接描述符为4，则client[4]就是他对应的信息。
struct Client client[MAX_CLIENT];

//用户认证，传入用户名和密码，通过认证返回0
int authenticate(char *user, char *pass);

//获取用户的home目录
int get_home_dir(char *home);

//创建socket服务端,修改全局变量server_sockfd;
int create_server_socket();

//主动模式，建立数据链接
int data_conn_active(struct in_addr *sin_addr, unsigned short port);

//被动模式，建立数据链接
int data_conn_passive(unsigned short port);

//判断目录（文件）是否存在，c_dir是否存在p_dir目录下
int file_exist(char *p_dir, char *c_dir);

int make_dir(char *p_dir, char *c_dir);

int remove_dir(char *p_dir, char *c_dir);

int rename_dir(char *p_dir, char *from, char *to);

//发送文件，传入链接描述符和要发送的文件名
int send_file(int sockfd, char *filename);

//发送文件列表，传入链接描述符和要发送的目录名，dir是文件系统的绝对路径
int send_list(int sockfd, char *dir);

//接收文件，传入链接描述符和要保存的文件。
int recv_file(int sockfd, char *filename);

//SIGINT的信号处理函数
void sigint_handler(int sig)
{
	int i;
	for(i = 0; i < MAX_CLIENT; i++) {
		//关闭所有链接描述符
		if(client[i].cmd_sockfd != 0) {
			close(i);
		}
	}
	//关闭监听socket描述符
	if(close(server_sockfd) == -1) {
		perror("close");
	}
	exit(0);
}

int main()
{
	char HOME[128];
	int client_len;
	struct sockaddr_in client_address;
	int result;
	fd_set readfds, testfds;

	struct sigaction act;
	act.sa_handler = sigint_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGINT, &act, 0);

	get_home_dir(HOME);
	printf("HOME:%s\n", HOME);

	//global variable server_sockfd will be set.
	create_server_socket();

	FD_ZERO(&readfds);
	FD_SET(server_sockfd, &readfds);

	while(1) {
		char buf[128];
		int fd;
		int nread;
	
		testfds = readfds;

		printf("server waiting\n");
		result = select(FD_SETSIZE, &testfds, (fd_set *)0, (fd_set *)0, (struct timeval *)0);

		if(result < 1) {
			perror("ftp_server");
			exit(1);
		}

		for(fd = 0; fd < FD_SETSIZE; fd++) {
			if(FD_ISSET(fd, &testfds)) {
				if(fd == server_sockfd) {//有新来的客户链接
					client_len = sizeof(client_address);
					client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address, &client_len);
					FD_SET(client_sockfd, &readfds);
					client[client_sockfd].cmd_sockfd = client_sockfd;
					client[client_sockfd].sin_addr.s_addr = client_address.sin_addr.s_addr;
					sprintf(client[client_sockfd].cwd, "/");
					printf("adding client on fd %d\n", client_sockfd);
					sprintf(buf, "220 (wxftp 1.0)\r\n");
					write(client_sockfd, buf, strlen(buf));
				} else {//客户链接发来数据
					ioctl(fd, FIONREAD, &nread);
					if(nread == 0) {//无数据可读，表示客户已主动断开链接
						close(fd);
						memset(&client[fd], 0, sizeof(struct Client));
						FD_CLR(fd, &readfds);
						printf("removing client on fd %d\n", fd);
					} else {//读取数据并当作命令来处理。
						read(fd, buf, nread);
						buf[nread] = '\0';
						printf("serving client on fd %d: %s\n", fd, buf);
						if(strncmp(buf, "USER", 4) == 0) {
							sscanf(&buf[4], "%s", client[fd].user);
							//printf("user %s\n", client[fd].user);
							sprintf(buf, "331 Password required for %s.\r\n", client[fd].user);
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "PASS", 4) == 0) {
							sscanf(&buf[4], "%s", client[fd].pass);
							if (authenticate(client[fd].user, client[fd].pass) == 0) {
								client[fd].authorized = 1;
								sprintf(buf, "230 Login successful.\r\n");
							} else {
								client[fd].authorized = 0;
								sprintf(buf, "530 Login or Password incorrect.\r\n");
							}
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "SYST", 4) == 0) {
							sprintf(buf, "215 Linux.\r\n");
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "FEAT", 4) == 0) {
							sprintf(buf, "550 Not support.\r\n");
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "PWD", 3) == 0) {
							sprintf(buf, "257 \"%s\" is current directory.\r\n", client[fd].cwd);
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "CWD", 3) == 0) {
							char dir[128];
							sscanf(&buf[3], "%s", dir);
							if (strncmp(dir, "..", 2) == 0) {
								if(strlen(client[fd].cwd) == 1) {
									sprintf(buf, "250 \"/\" is current directory.\r\n");
								} else {
									int i;
									char *cwd = client[fd].cwd;
									int len = strlen(cwd);
									for(i = len - 1; i >= 0; i--) {
										if(cwd[i] == '/' && i != len - 1) {
											cwd[i+1] = '\0';
											break;
										}
									}
									sprintf(buf, "250 CWD command successful. \"%s\" is current directory.\r\n", client[fd].cwd);
								}
							} else if(file_exist(client[fd].cwd, dir) == 0) {
								//client[fd].cwd = ;
								char *cwd = client[fd].cwd;
								int len = strlen(cwd);
								if(cwd[len-1] == '/') {
									sprintf(&client[fd].cwd[len], "%s", dir);
								} else {
									sprintf(&client[fd].cwd[len], "/%s", dir);
								}
								sprintf(buf,"250 CWD command successful. \"%s\" is current directory.\r\n",client[fd].cwd );
							} else {
								sprintf(buf,"550 CWD failed. \"%s\": no such file or directory.\r\n", dir);
							}
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "CDUP", 4) == 0) {
							if(strlen(client[fd].cwd) == 1) {
								sprintf(buf, "250 \"/\" is current directory.\r\n");
							} else {
								//make some change to client[fd].cwd
								int i;
								char *cwd = client[fd].cwd;
								int len = strlen(cwd);
								printf("%s: %d\n", cwd, len);
								for(i = len - 1; i >= 0; i--) {
									printf("%d: %c\n", i, client[fd].cwd[i]);
									if(client[fd].cwd[i] == '/' && i != len - 1) {
										client[fd].cwd[i+1] = '\0';
										break;
									}
								}
								sprintf(buf, "250 \"%s\" is current directory.\r\n",client[fd].cwd);
							}
							write(fd, buf, strlen(buf));
						} else if (strncmp(buf, "MKD", 3) == 0) {
							char dir[128];
							sscanf(&buf[3], "%s", dir);
							if (file_exist(client[fd].cwd, dir) == 0) {
								sprintf(buf, "550 Command failed. %s exists.\r\n", dir);
								write(fd, buf, strlen(buf));
							} else {
								make_dir(client[fd].cwd, dir);
								sprintf(buf, "250 Command ok. %s made.\r\n", dir);
								write(fd, buf, strlen(buf));
							}
						} else if (strncmp(buf, "RMD", 3) == 0) {
							char dir[128];
							sscanf(&buf[3], "%s", dir);
							if (file_exist(client[fd].cwd, dir) == 0) {
								remove_dir(client[fd].cwd, dir);
								sprintf(buf, "250 Command ok. %s removed.\r\n", dir);
								write(fd, buf, strlen(buf));
							} else {
								sprintf(buf, "550 Command failed. %s doesn't exist.\r\n", dir);
								write(fd, buf, strlen(buf));
							}
						} else if (strncmp(buf, "RNFR", 4) == 0) {
							char dir[128];
							sscanf(&buf[4], "%s", dir);
							if (file_exist(client[fd].cwd, dir) == 0) {
								sscanf(&buf[4], "%s", client[fd].rnfr);
								sprintf(buf, "350 File exists, ready for destination name.\r\n");
								write(fd, buf, strlen(buf));
							} else {
								sprintf(buf, "550 File/directory not found.\r\n");
								write(fd, buf, strlen(buf));
							}
						} else if (strncmp(buf, "RNTO", 4) == 0) {
							char dir[128];
							sscanf(&buf[4], "%s", dir);
							if (file_exist(client[fd].cwd, dir) == 0) {
								sprintf(buf, "550 Comman failed. %s exists.\r\n", dir);
								write(fd, buf, strlen(buf));
							} else {
								rename_dir(client[fd].cwd, client[fd].rnfr, dir);
								sprintf(buf, "250 File rename successfully.\r\n");
								write(fd, buf, strlen(buf));
							}
						} else if(strncmp(buf, "TYPE", 4) == 0) {
							char type[10];
							sscanf(&buf[4], "%s", type);
							sprintf(buf, "200 Type set to %s.\r\n", type);
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "PASV", 4) == 0) {
							int port = rand() % 1000 + 8000;
							client[fd].data_port = port;
							client[fd].mode = kDataConnModePassive;
							struct sockaddr_in name;
							int name_len = sizeof(struct sockaddr_in);
							getsockname(fd, (struct sockaddr*)&name, &name_len);
							printf("server ip address is : %s\n port: %d\n", inet_ntoa(name.sin_addr), port);
							sprintf(buf, "227 Entering Passive Mode (210,25,132,182,%d,%d)\r\n", port / 256, port % 256);
							write(fd, buf, strlen(buf));
							int sockfd = data_conn_passive(port);
							printf("PASV sockfd: %d\n", sockfd);
							client[fd].data_sockfd = sockfd;
						} else if(strncmp(buf, "PORT", 4) == 0) {
							int ip[4], port[2];
							sscanf(&buf[4], "%d,%d,%d,%d,%d,%d", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
							sprintf(client[fd].data_ip, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
							client[fd].data_port = port[0] * 256 + port[1];
							client[fd].mode = kDataConnModeActive;
							printf("IP:%s, Port:%d\n", client[fd].data_ip, client[fd].data_port);
							sprintf(buf, "200 Port command successful.\r\n");
							write(fd, buf, strlen(buf));
						} else if(strncmp(buf, "LIST", 4) == 0 || strncmp(buf, "NLST", 4) == 0) {
							int sockfd;
							if(client[fd].mode == kDataConnModeActive) {
								sockfd = data_conn_active(&client[fd].sin_addr, client[fd].data_port);
							} else if (client[fd].mode == kDataConnModePassive) {
								//sockfd = data_conn_passive(client[fd].data_port);
								sockfd = client[fd].data_sockfd;
							}
							int result = 0;
							if (sockfd != -1) {
								sprintf(buf,"150 Opening data connection for directory list.\r\n");
								write(fd, buf, strlen(buf));
								if(send_list(sockfd, client[fd].cwd) == 0) {
									sprintf(buf, "226 Transfer ok.\r\n");
								} else {
									sprintf(buf, "550 Error encountered.\r\n");
								}
								write(fd, buf, strlen(buf));
								close(sockfd);
							} else {
								printf("CREATE DATA_CONN FAILE.\n");
							}
						} else if (strncmp(buf, "RETR", 4) == 0) {
							char filename[64];
							sscanf(&buf[4], "%s", filename);
							if(file_exist(client[fd].cwd, filename) != 0) {
								sprintf(buf, "550 \"%s\": no such file.\r\n", filename);
								write(fd, buf, strlen(buf));
							} else {
								int sockfd;
								if(client[fd].mode == kDataConnModeActive) {
									sockfd = data_conn_active(&client[fd].sin_addr, client[fd].data_port);
								} else if (client[fd].mode == kDataConnModePassive) {
									sockfd = data_conn_passive(client[fd].data_port);
								}
								int result = 0;
								if (sockfd != -1) {
									sprintf(buf, "150 Opening data connection for %s\r\n", filename);
									write(fd, buf, strlen(buf));
									char filedir[128];
									int len = strlen(client[fd].cwd);
									if(client[fd].cwd[len-1] == '/') {
										sprintf(filedir, "%s%s%s", HOME, client[fd].cwd, filename);
									} else {
										sprintf(filedir, "%s%s/%s", HOME, client[fd].cwd, filename);
									}	
									result = send_file(sockfd, filedir);
									printf("send result: %d\n", result);
									if (result != -1) {
										close(sockfd);
										sprintf(buf, "226 File sent ok.\r\n");
										write(fd, buf, strlen(buf));
									}
								}
							}
						} else if (strncmp(buf, "STOR", 4) == 0) {
							char filename[64];
							sscanf(&buf[4], "%s", filename);
							int sockfd;
							if(client[fd].mode == kDataConnModeActive) {
								sockfd = data_conn_active(&client[fd].sin_addr, client[fd].data_port);
							} else if (client[fd].mode == kDataConnModePassive) {
								sockfd = data_conn_passive(client[fd].data_port);
							}
							int result = 0;
							if (sockfd != -1) {
								sprintf(buf, "150 Opening data connection for %s\r\n", filename);
								write(fd, buf, strlen(buf));
								char filedir[128];
								int len = strlen(client[fd].cwd);
								if(client[fd].cwd[len-1] == '/') {
									sprintf(filedir, "%s%s%s", HOME, client[fd].cwd, filename);
								} else {
									sprintf(filedir, "%s%s/%s", HOME, client[fd].cwd, filename);
								}	
								result = recv_file(sockfd, /*filename*/filedir);
								printf("recv result: %d\n", result);
								if (result != -1) {
									close(sockfd);
									sprintf(buf, "226 File received ok.\r\n");
									write(fd, buf, strlen(buf));
								}
							}
						} else if (strncmp(buf, "QUIT", 4) == 0) {
							sprintf(buf, "221 Goodbye.\r\n");
							write(fd, buf, strlen(buf));
							close(fd);
							memset(&client[fd], 0, sizeof(struct Client));
							FD_CLR(fd, &readfds);
							printf("removing client on fd %d\n", fd);
						} else {
							sprintf(buf, "550 Unknown command.\r\n");
							write(fd, buf, strlen(buf));
						}
					}
				}
			}
		}
	}
	return 0;
}

int authenticate(char *user, char *pass)
{
	return 0;
}

int create_server_socket()
{
	struct sockaddr_in server_address;
	int optval;
	int server_len;
	server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
	optval = 1;
	if(setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) == -1) {
		perror("setsockopt");
	}

	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);
	server_address.sin_port = htons(SERVER_CMD_PORT);
	server_len = sizeof(server_address);
	if(bind(server_sockfd, (struct sockaddr *)&server_address, server_len) < 0) {
		perror("bind failed");
		exit(1);
	}

	listen(server_sockfd, 5);
	return 0;
}


int data_conn_active(struct in_addr *sin_addr, unsigned short port)
{
	int sockfd;
	struct sockaddr_in address;
	struct sockaddr_in client_addr;
	int result;
	int optval = 1;
	
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	
	if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) == -1) {
		perror("setsockopt");
	}	

	address.sin_family = AF_INET;
	//address.sin_addr.s_addr = inet_addr(ip);
	address.sin_addr.s_addr = sin_addr->s_addr;
	address.sin_port = htons(port);
	printf("ip addr: %s\n", inet_ntoa(*sin_addr));
	
	client_addr.sin_family = AF_INET;
	client_addr.sin_port = htons(SERVER_DAT_PORT);
	client_addr.sin_addr.s_addr = inet_addr("0.0.0.0");
		
	if(bind(sockfd, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1) {
		perror("bind");
		//exit(1);
		return -1;
	}
		

	if(connect(sockfd, (struct sockaddr *)&address, sizeof(address)) == -1) {
		perror("connect");
		return -1;
	}
	
	return sockfd;
}

int data_conn_passive(unsigned short port)
{
	int data_sockfd, client_sockfd;
	struct sockaddr_in server_address, client_address;
	int client_len;
	int optval =  1;
	
	data_sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(data_sockfd < 0) {
		perror("socket()");
	}
	if(setsockopt(data_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) == -1) {
		perror("setsockopt");
	}
	
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);
	server_address.sin_port = htons(port);

	if(bind(data_sockfd, (struct sockaddr *)&server_address, sizeof(struct sockaddr)) == -1) {
		perror("bind");
		return -1;
	}

	listen(data_sockfd, 5);

	client_len = sizeof(client_address);
	client_sockfd = accept(data_sockfd, (struct sockaddr *)&client_address, &client_len);
	if (client_sockfd < 0) {
		perror("accept()");
	}
	printf("data_conn_passive client_sockfd: %d\n", client_sockfd);
	close(data_sockfd);
	return client_sockfd;
}

int send_file(int sockfd, char *filename)
{
	int fd, nread;
	unsigned char buf[128];
	printf("send_file start:%s\n", filename);
	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		perror("open");
		return -1;
	}
	while((nread = read(fd, buf, 128)) != 0) {
		write(sockfd, buf, nread);
	}
	printf("send_file ok.\n");
	return 0;
}

int recv_file(int sockfd, char *filename)
{
	int fd, nread;
	unsigned char buf[128];
	fd = open(filename, O_WRONLY | O_CREAT);
	while((nread = read(sockfd, buf, 128)) != 0) {
		write(fd, buf, nread);
	}
	printf("recv_file ok.\n");
	return 0;
}

int file_exist(char *p_dir, char *c_dir)
{
	char ls_dir[128];
	int len = strlen(p_dir);
	if(p_dir[len-1] == '/') {
		sprintf(ls_dir, "ls ~%s%s > /dev/null", p_dir, c_dir);
	} else {
		sprintf(ls_dir, "ls ~%s/%s > /dev/null", p_dir, c_dir);
	}
	return system(ls_dir);
}

int make_dir(char *p_dir, char *c_dir)
{
	char mkdir[128];
	int len = strlen(p_dir);
	if (p_dir[len-1] == '/') {
		sprintf(mkdir, "mkdir ~%s%s", p_dir, c_dir);
	} else {
		sprintf(mkdir, "mkdir ~%s/%s", p_dir, c_dir);

	}
	printf("make_dir: %s\n", mkdir);
	return system(mkdir);
}

int remove_dir(char *p_dir, char *c_dir)
{
	char rmdir[128];
	int len = strlen(p_dir);
	if (p_dir[len-1] == '/') {
		sprintf(rmdir, "rm -r ~%s%s", p_dir, c_dir);
	} else {
		sprintf(rmdir, "rm -r ~%s/%s", p_dir, c_dir);

	}
	printf("remove_dir: %s\n", rmdir);
	return system(rmdir);
}

int rename_dir(char *p_dir, char *from, char *to)
{
	char mvdir[256];
	int len = strlen(p_dir);
	if (p_dir[len-1] == '/') {
		sprintf(mvdir, "mv ~%s%s ~%s%s", p_dir, from, p_dir, to);
	} else {
		sprintf(mvdir, "mv ~%s/%s ~%s/%s", p_dir, from, p_dir, to);
	}
	printf("rename_dir: %s\n", mvdir);
	return system(mvdir);
}

int send_list(int sockfd, char *dir)
{
	char ls_dir[128];
	char *temp_file = "/tmp/ls.out";
	char buf[64] = {0};
	sprintf(ls_dir, "ls -F ~%s > %s", dir, temp_file);
	int result = system(ls_dir);
	if(result == 0) {
		FILE *file = fopen(temp_file, "r");
		while(fscanf(file, "%s", buf) != EOF) {
			int len = strlen(buf);
			//printf("%s\n", buf);
			sprintf(&buf[len], "\r\n");
			write(sockfd, buf, strlen(buf));
		}
		fclose(file);
		printf("send_list ok.\n");
	} else {
		return -1;
	}
	return 0;
}

int get_home_dir(char *home)
{
	char **env = environ;
	while(*env) {
		//printf("%s\n", *env);
		if(strncmp(*env, "HOME=", 5) == 0) {
			sprintf(home, "%s", &((*env)[5]));
			break;
		}
		env++;
	}
	return 0;
}
