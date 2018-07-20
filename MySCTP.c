//编译gcc -lpthread -lsctp MySCTP.c -o MySCTP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <errno.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sched.h>
#include <arpa/inet.h>

#define TOOL_VERSION "1.0"
#define MAX_BUFF_SIZE 65525
#define DEF_BUFF_SIZE 150
#define EPOLLEVENTS 65536
#define MAX_CMD_LEN 100
#define MAX_CMD_DISC_LEN 100
#define SCTP_GET_PARA   1
#define SCTP_SET_PARA   2

#define PERFORMANCE_TEST_START "performance test start"
#define PERFORMANCE_TEST_END "performance test end"
#define DATA_PERFORMANCE_TEST_START "data sending performance test start"
#define DATA_PERFORMANCE_TEST_END "data sending performance test end"

#define O_NONBLOCK 00004000
#define F_GETFL  3 /* get file->f_flags */
#define F_SETFL  4 /* set file->f_flags */

//#define DEBUG_PRINT(fd, fmt,args...) {if(!g_performance_test && !g_data_performance_test && !get_fd_pt(fd)) PRINT_Info(fmt, ##args);}

#define PRINT_Error(fmt, ...) printf(RED"%s   "fmt, get_cur_time(), ##__VA_ARGS__)
#define PRINT_Debug(fmt, ...) printf(YELLOW"%s   "fmt, get_cur_time(), ##__VA_ARGS__)
#define PRINT_Info(fmt, ...) printf(GREEN"%s   "fmt, get_cur_time(), ##__VA_ARGS__)

#define RED           "\033[0;32;31m"
#define YELLOW        "\033[1;33m"
#define GREEN         "\033[0;32;32m"

char *get_cur_time()
{
    static char s[20];
    time_t t;
    struct tm* ltime;
 
    time(&t);
    ltime = localtime(&t);
    strftime(s, 20, "%Y-%m-%d %H:%M:%S", ltime);
 
    return s;
}


unsigned char g_local_ip_1[50] = {0};
unsigned char g_local_ip_2[50] = {0};
unsigned short g_local_port = 0;
unsigned char g_remote_ip_1[50] = {0};
unsigned char g_remote_ip_2[50] = {0};
unsigned short g_remote_port = 0;
unsigned char g_test_v4 = 0;
unsigned char g_test_v6 = 0;
unsigned char g_multi_homing = 0;
unsigned char g_client_server = 0;
unsigned char g_performance_test =0;
unsigned char g_data_performance_test =0;
unsigned char g_connect_mod = 0; /* 0: connect; 1: setsockopt */
unsigned char g_pmtu = 1; /* 0: disable pmtu; 1: enable pmtu */
int g_max_conn_num = 65535;
int g_message_len = 150;
int g_data_pt_time = 10; /* 10 seconds */
unsigned long g_message_num = 65536;
unsigned long g_message_received = 0;
unsigned long g_message_total_size = 0;
int g_failure_cnt = 0;
int g_success_cnt = 0;
int g_hbinterval = 1000;
int g_sackdelay = 110; /* 0: disable sackdelay */
int g_rto_init = 200;
int g_rto_min = 150;
int g_rto_max = 200;
int g_first_fd = 0;
time_t g_first;
time_t g_last;
int g_fd_count = 0;
int g_epfd;

static bool inline add_to_epoll(int fd)
{
	struct epoll_event ev;
	int ret;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = fd;
	ret = epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);
	if (ret < 0) 
	{
		PRINT_Error("[EPOLL_CTL_ADD error] %d:%s\n", errno, strerror(errno));
		return false;
	}
	return true;
}

static bool inline del_from_epoll(int fd)
{
	struct epoll_event ev;
	int ret;

	memset(&ev, 0, sizeof(ev));
	ev.data.fd = fd;
	ret = epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, &ev);
	if (ret < 0) 
	{
		PRINT_Error("[EPOLL_CTL_DEL error] fd = %d, %d:%s\n", fd, errno, strerror(errno));
		return false;
	}
	return true;
}

#define MAX_FD 65536
#define MASK_FD 65535
#define MAX_CONFLICT 3
#define UNUSED_SK_TYPE 0
#define LISTEN_SK_TYPE 1
#define CONN_SK_TYPE  2
#define REMOVE_SK_TYPE 3

struct fd_list {
	int fd;
	unsigned char type;
	unsigned char state;
	unsigned char performance_test;
} g_fd_list[MAX_FD][MAX_CONFLICT];

pthread_mutex_t fd_list_lock;

enum sk_state {
	SK_STATE_CLOSED = 0,
	SK_STATE_LISTENING,
	SK_STATE_ACCEPTED,
	SK_STATE_CONNECTING,
	SK_STATE_COMM_UP,
	SK_STATE_MAX
};

struct sk_state_description {
	char state;
	char description[MAX_CMD_LEN];
} g_sk_state_list[SK_STATE_MAX] = {
	{SK_STATE_CLOSED, "closed"},
	{SK_STATE_LISTENING, "listening"},
	{SK_STATE_ACCEPTED, "accepted"},
	{SK_STATE_CONNECTING, "connecting"},
	{SK_STATE_COMM_UP, "established"}
};

static void inline add_to_fd_list_unlock(int fd, unsigned char type, unsigned char state)
{
	int i, key;

	key = MASK_FD&fd;
	for (i = 0; i < MAX_CONFLICT && g_fd_list[key][i].fd != 0 && g_fd_list[key][i].fd != fd; i++) 
	{
	}
	if (i < MAX_CONFLICT && g_fd_list[key][i].type == REMOVE_SK_TYPE) 
	{
		g_fd_list[key][i].type = UNUSED_SK_TYPE;
		return;
	}
	if (i < MAX_CONFLICT && g_fd_list[key][i].fd == 0) 
	{
		g_fd_list[key][i].fd = fd;
		g_fd_list[key][i].type = type;
		g_fd_list[key][i].state = state;
		g_fd_list[key][i].performance_test = g_performance_test;
		g_fd_count++;
	} 
	else if (g_fd_list[key][i].fd == fd) 
	{
		/* update_fd_list() may be called before add_to_fd_list() */
		g_fd_list[key][i].type = type;
	} 
	else 
	{
		PRINT_Error("[add_to_fd_list] too many conflict fds, key = %d\n", key);
	}
}

static void inline add_to_fd_list(int fd, unsigned char type, unsigned char state)
{
	pthread_mutex_lock(&fd_list_lock); 
	add_to_fd_list_unlock(fd, type, state);
	pthread_mutex_unlock(&fd_list_lock); 
}

static void inline update_fd_state(int fd, unsigned char state)
{
	int i, key;

	key = MASK_FD&fd;
	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_CONFLICT && g_fd_list[key][i].fd != fd; i++) 
	{
	}
	if (i < MAX_CONFLICT && g_fd_list[key][i].fd == fd) 
	{
		g_fd_list[key][i].state = state;
	} 
	else 
	{
		/* update_fd_list() may be called before add_to_fd_list() */
		add_to_fd_list_unlock(fd, 0, state);
	}
	pthread_mutex_unlock(&fd_list_lock); 
}

static void inline remove_from_fd_list(int fd)
{
	int i, key;

	key = MASK_FD&fd;
	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_CONFLICT && g_fd_list[key][i].fd != fd; i++) 
	{
	}
	if (i < MAX_CONFLICT && g_fd_list[key][i].fd == fd) 
	{
		g_fd_list[key][i].fd = 0;
		g_fd_list[key][i].type = UNUSED_SK_TYPE;
		g_fd_list[key][i].state = 0;
		g_fd_count--;
	} 
	else 
	{
		for (i = 0; i < MAX_CONFLICT && g_fd_list[key][i].fd != 0; i++) 
		{
		}
		if (i < MAX_CONFLICT && g_fd_list[key][i].fd == 0) 
		{
			g_fd_list[key][i].type = REMOVE_SK_TYPE;
		}
	}
	pthread_mutex_unlock(&fd_list_lock); 
}

unsigned char inline get_fd_type(int fd)
{
	int i, key;

	key = MASK_FD&fd;
	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_CONFLICT && g_fd_list[key][i].fd != fd; i++) 
	{
	}
	if (i < MAX_CONFLICT && g_fd_list[key][i].fd == fd) 
	{
		pthread_mutex_unlock(&fd_list_lock); 
		return g_fd_list[key][i].type;
	}
	pthread_mutex_unlock(&fd_list_lock); 
	return UNUSED_SK_TYPE;
}

unsigned char inline get_fd_pt(int fd)
{
	int i, key;

	key = MASK_FD&fd;
	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_CONFLICT && g_fd_list[key][i].fd != fd; i++) 
	{
	}
	if (i < MAX_CONFLICT && g_fd_list[key][i].fd == fd) 
	{
		pthread_mutex_unlock(&fd_list_lock); 
		return g_fd_list[key][i].performance_test;
	}
	pthread_mutex_unlock(&fd_list_lock); 
	return 0;
}

static void inline clean_fd(int fd)
{
	if (get_fd_type(fd) == CONN_SK_TYPE)
		del_from_epoll(fd);
	remove_from_fd_list(fd);
	close(fd);
}

static void inline clean_all_cnn_fds()
{
	int i, j;

	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_FD; i++) 
	{
		for (j = 0; j < MAX_CONFLICT; j++) 
		{
			if (g_fd_list[i][j].type != UNUSED_SK_TYPE) 
			{
				if (g_fd_list[i][j].type == CONN_SK_TYPE) 
				{
					del_from_epoll(g_fd_list[i][j].fd);
					close(g_fd_list[i][j].fd);
					memset(&g_fd_list[i][j], 0, sizeof(struct fd_list));
					g_fd_count--;
				}
			}
		}
	}
	pthread_mutex_unlock(&fd_list_lock); 
}

static void inline update_all_cnn_fds()
{
	int i, j;

	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_FD; i++) 
	{
		for (j = 0; j < MAX_CONFLICT; j++) 
		{
			if (g_fd_list[i][j].type != UNUSED_SK_TYPE) 
			{
				if (g_fd_list[i][j].type == CONN_SK_TYPE) 
				{
					g_fd_list[i][j].performance_test = 1;
				}
			}
		}
	}
	pthread_mutex_unlock(&fd_list_lock); 
}

static void inline print_fd_list()
{
	int i, j;

	pthread_mutex_lock(&fd_list_lock); 
	for (i = 0; i < MAX_FD; i++) 
	{
		for (j = 0; j < MAX_CONFLICT; j++) 
		{
			if (g_fd_list[i][j].fd != 0) 
			{
				PRINT_Info("[fd list] fd = %d, style = %s, state = %s\n", 
				g_fd_list[i][j].fd, 
				(g_fd_list[i][j].type == LISTEN_SK_TYPE)?"listen":"connect",
				g_sk_state_list[g_fd_list[i][j].state].description);
			}
		}
	}
	pthread_mutex_unlock(&fd_list_lock); 
	PRINT_Info("[fd list] total = %d\n", g_fd_count);
}

bool ch_to_i(unsigned char *d, char *s)
{
	bool ret = true;

	if (s[0] >= 'A' && s[0] <= 'Z')
		*d = (s[0] - 'A' + 10) << 4;
	else if (s[0] >= 'a' && s[0] <= 'z')
		*d = (s[0] - 'a' + 10) << 4;
	else if (s[0] >= '0' && s[0] <= '9')
		*d = (s[0] - '0') << 4;
	else {
		PRINT_Error("\n[wrong mac address]\n");
		ret = false;
	}

	if (s[1] >= 'A' && s[1] <= 'Z')
		*d += (s[1] - 'A' + 10);
	else if (s[1] >= 'a' && s[1] <= 'z')
		*d += (s[1] - 'a' + 10);
	else if (s[1] >= '0' && s[1] <= '9')
		*d += (s[1] - '0');
	else {
		PRINT_Error("\n[wrong mac address]\n");
		ret = false;
	}

	return ret;
}

bool get_mac_addr(unsigned char *d, char *s)
{
	int i = 0;
	bool ret = true;

	PRINT_Info("[mac address] ");

	for (i = 0; i < 6; i++, s += 3) 
	{
		ret = ch_to_i(d + i, s);
		if (!ret)
			return ret;
		printf("%02x:", d[i]);
	}
	printf("\n");

	return ret;
}

static bool inline sctp_set_events(int fd)
{
	struct sctp_event_subscribe events;
	socklen_t solen = 0;
	int ret;

	memset( (void *)&events, 0, sizeof(events) );
	events.sctp_data_io_event = 1;
	events.sctp_association_event = 1;
	events.sctp_shutdown_event = 1;
	solen = sizeof(struct sctp_event_subscribe);
	ret = setsockopt(fd, SOL_SCTP, SCTP_EVENTS, (const void *)&events, sizeof(events));
	if (ret < 0) 
	{
		PRINT_Error("[set SCTP_EVENTS error] %d:%s\n", errno, strerror(errno));
		close(fd);
		return false;
	}
	PRINT_Info("[set SCTP_EVENTS]\n");
	return true;
}

static bool inline sctp_set_non_block(int fd)
{
	int l_flags;
	int ret;

	errno = 0;
	l_flags = fcntl(fd, F_GETFL, O_NONBLOCK);
	if (l_flags<0) 
	{
		PRINT_Error("[set NONBLOCK error] %d:%s\n", errno, strerror(errno));
		close(fd);
		return false;
	}
	l_flags |= O_NONBLOCK;
	l_flags = fcntl(fd, F_SETFL, l_flags);
	if (l_flags<0) 
	{
		PRINT_Error("[set NONBLOCK error] %d:%s\n", errno, strerror(errno));
		close(fd);
		return false;
	}
	PRINT_Info("[set NONBLOCK]\n");
	return true;
}

static bool inline sctp_set_block(int fd)
{
	int l_flags;
	int ret;

	errno = 0;
	l_flags = fcntl(fd, F_GETFL, O_NONBLOCK);
	if (l_flags<0) 
	{
		PRINT_Error("[set NBLOCK error] %d:%s\n", errno, strerror(errno));
		close(fd);
		return false;
	}
	l_flags &= ~O_NONBLOCK;
	l_flags = fcntl(fd, F_SETFL, l_flags);
	if (l_flags<0) 
	{
		PRINT_Error("[set NONBLOCK error] %d:%s\n", errno, strerror(errno));
		close(fd);
		return false;
	}
	PRINT_Info("[set BLOCK]\n");
	return true;
}

static bool inline sctp_set_init_param(int fd)
{
	struct sctp_initmsg initmsg;
	int ret;

	errno = 0;
	memset( &initmsg, 0, sizeof(initmsg) );
	initmsg.sinit_num_ostreams = 8;
	initmsg.sinit_max_instreams = 8;
	initmsg.sinit_max_attempts = 6;
	initmsg.sinit_max_init_timeo = 150;
	ret = setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof(initmsg));
	if (ret < 0) 
	{
		PRINT_Error("[set SCTP_INITMSG error] %d:%s\n", errno, strerror(errno));
		close(fd);
		return false;
	}
	PRINT_Info("[set SCTP_INITMSG]\n");
	return true;
}

static bool inline sctp_bind_addr_v4(int fd)
{
	struct sockaddr_in cliaddr;
	int ret;

	errno = 0;
	bzero( (void *)&cliaddr, sizeof(cliaddr) );
	cliaddr.sin_family = AF_INET;
	cliaddr.sin_addr.s_addr = inet_addr(g_local_ip_1);
	cliaddr.sin_port = htons(g_local_port);
	ret = bind(fd, (struct sockaddr *)&cliaddr, sizeof(cliaddr));
	if (ret < 0) 
	{
		PRINT_Error("[bind %s-%d error] %d:%s\n", g_local_ip_1, g_local_port, errno, strerror(errno));
		return false;
	}
	PRINT_Info("[bind %s-%d]\n", g_local_ip_1, g_local_port);
	if (g_multi_homing) 
	{
		errno = 0;
		bzero( (void *)&cliaddr, sizeof(cliaddr) );
		cliaddr.sin_family = AF_INET;
		cliaddr.sin_addr.s_addr = inet_addr(g_local_ip_2);
		cliaddr.sin_port = htons(g_local_port);
		ret = sctp_bindx(fd, (struct sockaddr *)&cliaddr, 1, SCTP_BINDX_ADD_ADDR);
		if (ret < 0) 
		{
			PRINT_Error("[bind %s-%d error] %d:%s\n", g_local_ip_2, g_local_port, errno, strerror(errno));
			return false;
		}
		PRINT_Info("[bind %s-%d]\n", g_local_ip_2, g_local_port);
	}

	return true;
}

static bool inline sctp_bind_addr_v6(int fd)
{
	struct sockaddr_in6 cliaddr;
	int ret;

	errno = 0;
	bzero( (void *)&cliaddr, sizeof(cliaddr) );
	cliaddr.sin6_family = PF_INET6;
	inet_pton(AF_INET6, g_local_ip_1, &cliaddr.sin6_addr);
	cliaddr.sin6_port = htons(g_local_port);
	ret = bind(fd, (struct sockaddr *)&cliaddr, sizeof(struct sockaddr_in6));
	if (ret < 0) 
	{
		PRINT_Error("[bind %s-%d error] %d:%s \n", g_local_ip_1, errno, strerror(errno));
		return false;
	}
	PRINT_Info("[bind %s-%d]\n", g_local_ip_1, g_local_port);

	if (g_multi_homing) 
	{
		errno = 0;
		bzero( (void *)&cliaddr, sizeof(cliaddr) );
		cliaddr.sin6_family = PF_INET6;
		inet_pton(AF_INET6, g_local_ip_2, &cliaddr.sin6_addr);
		cliaddr.sin6_port = htons(g_local_port);
		ret = sctp_bindx(fd, (struct sockaddr *)&cliaddr, 1, SCTP_BINDX_ADD_ADDR);
		if (ret < 0) 
		{
			PRINT_Error("[bind %s-%d error] %d:%s\n", g_local_ip_2, errno, strerror(errno));
			return false;
		}
		PRINT_Info("[bind %s-%d]\n", g_local_ip_2, g_local_port);
	}

	return true;
}

static bool inline sctp_set_reuse(int fd, int opt)
{
	int reuse_address = (opt == 0)?0:1;

	errno = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address))<0)
	{
		PRINT_Error("[set SO_REUSEADDR error] %d:%s\n", errno, strerror(errno));
		return false;
	}
	PRINT_Info("[set SO_REUSEADDR]\n");

	return true;
}

static void inline sctp_rto_param(int fd, int opt)
{
	struct sctp_rtoinfo l_rtoinfo;
	int ret;
	int len = sizeof(l_rtoinfo);

	memset(&l_rtoinfo, 0, sizeof(l_rtoinfo));

	if (opt == SCTP_SET_PARA) 
	{
		l_rtoinfo.srto_initial = g_rto_init;
		l_rtoinfo.srto_min = g_rto_min;
		l_rtoinfo.srto_max = g_rto_max;
		ret = setsockopt(fd, IPPROTO_SCTP, SCTP_RTOINFO, &l_rtoinfo, len);
		if (ret < 0) 
		{
			PRINT_Error("[set SCTP_RTOINFO error] %d:%s\n", errno, strerror(errno));
			return;
		} 
		else 
		{
			PRINT_Info("[set SCTP_RTOINFO] fd = %d\n", fd);
		}
	} 
	else 
	{
		ret = getsockopt(fd, IPPROTO_SCTP, SCTP_RTOINFO, &l_rtoinfo, &len);
		if (ret < 0) 
		{
			PRINT_Error("[get SCTP_RTOINFO error] %d:%s\n", errno, strerror(errno));
			return;
		} 
		else 
		{
			PRINT_Info("[get SCTP_RTOINFO] rto_init = %d, rto_min = %d, rto_max = %d\n", 
			l_rtoinfo.srto_initial, 
			l_rtoinfo.srto_min, 
			l_rtoinfo.srto_max);
		}
	}
}

static int inline get_sctp_sk()
{
	int fd;

	errno = 0;
	if (g_test_v4) 
	{
		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
	} 
	else if (g_test_v6) 
	{
		fd = socket(PF_INET6, SOCK_STREAM, IPPROTO_SCTP);
		if (fd < 0)
			return -1;
	} 
	else
		return -1;
	if (fd < 0) 
	{
		PRINT_Error("[IPPROTO_SCTP error] %d:%s \n", errno, strerror(errno));
		return -1;
	}
	PRINT_Info("[IPPROTO_SCTP] fd = %d\n", fd);

	return fd;
}

int create_socket(bool non_block)
{
	int fd;
	int ret;

	/* create a socket */
	fd = get_sctp_sk();
	if (fd < 0) 
	{
		return -1;
	}
	/* set reuse address */
	if (!sctp_set_reuse(fd, 1)) 
	{
		close(fd);
		return -1;
	}
	/* bind local addresses */
	if ((g_test_v4 && !sctp_bind_addr_v4(fd)) || 
	(g_test_v6 && !sctp_bind_addr_v6(fd))) 
	{
		close(fd);
		return -1;
	}
	/* non block model */
	if (non_block) 
	{
		if (!sctp_set_non_block(fd)) 
		{
			close(fd);
			return -1;
		}
	}
	/* receive sctp events (data, notifications...) */
	if (!sctp_set_events(fd)) 
	{
		close(fd);
		return -1;
	}
	/* set init parameters */
	if (!sctp_set_init_param(fd)) 
	{
		close(fd);
		return -1;
	}
	return fd;
}

void sctp_peer_param(int cfd, char opt)
{
	unsigned char cli_addr[2000] = {0};
	unsigned char dest[INET6_ADDRSTRLEN];
	socklen_t socklen = 0;
	int index;
	int ret;
	struct sctp_getaddrs *sctp_peeraddr; 
	struct sockaddr *addr;
	struct sctp_paddrparams paddrparams;

	socklen = sizeof(cli_addr);
	memset(&paddrparams, 0 ,sizeof(struct sctp_paddrparams));
	memset(cli_addr, 0, sizeof(cli_addr));
	sctp_peeraddr = (struct sctp_getaddrs *)cli_addr;

	ret = getsockopt(cfd, SOL_SCTP, SCTP_GET_PEER_ADDRS, (void *)cli_addr, &socklen);
	if (ret < 0) 
	{
		PRINT_Error("[SCTP_GET_PEER_ADDRS error] %d:%s \n", errno, strerror(errno));
		return;
	}

	memset(&paddrparams, 0, sizeof(paddrparams));
	if (opt == SCTP_SET_PARA) 
	{
		if (g_hbinterval == 0) 
		{
			paddrparams.spp_flags |= SPP_HB_DISABLE;
		} 
		else 
		{
			paddrparams.spp_flags |= SPP_HB_ENABLE;
			paddrparams.spp_hbinterval = g_hbinterval;
		}
		
		if (g_pmtu == 0) 
		{
			paddrparams.spp_flags |= SPP_PMTUD_DISABLE;
		} 
		else 
		{
			paddrparams.spp_flags |= SPP_PMTUD_ENABLE;
		}
		
		if (g_sackdelay == 0) 
		{
			paddrparams.spp_flags |= SPP_SACKDELAY_DISABLE;
		} 
		else 
		{
			paddrparams.spp_flags |= SPP_SACKDELAY_ENABLE;
			paddrparams.spp_sackdelay = g_sackdelay;
		}
	}

	socklen = sizeof(paddrparams);
	if (sctp_peeraddr->addr_num) 
	{
		addr = (struct sockaddr *)(cli_addr + offsetof(struct sctp_getaddrs, addrs));
		for (index = 0; index < sctp_peeraddr->addr_num; index++) 
		{
			memset(dest, 0, sizeof(dest));
			if (addr->sa_family == AF_INET) 
			{
				PRINT_Info("[SCTP_GET_PEER_ADDRS] %s\n", inet_ntoa(((struct sockaddr_in *)addr)->sin_addr));
				memcpy(&paddrparams.spp_address, addr, sizeof(struct sockaddr_in));
				addr = (struct sockaddr *) (((char *) addr) + sizeof(struct sockaddr_in));
			} 
			else if (addr->sa_family == AF_INET6) 
			{
				memset(dest, 0, sizeof(dest));
				inet_ntop(AF_INET6, (void *)&(((struct sockaddr_in6 *)addr)->sin6_addr), dest, INET6_ADDRSTRLEN);
				PRINT_Info("[SCTP_GET_PEER_ADDRS] %s\n", dest);
				memcpy(&paddrparams.spp_address, addr, sizeof(struct sockaddr_in6));
				addr = (struct sockaddr *) (((char *) addr) + sizeof(struct sockaddr_in6));
			} 
			else 
			{
				PRINT_Info("[SCTP_GET_PEER_ADDRS error] unsupported address family %d\n", addr->sa_family);
				break;
			}
			
			if (opt == SCTP_SET_PARA) 
			{
				ret = setsockopt(cfd, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, (const void *)&paddrparams, sizeof(paddrparams));
				if (ret < 0) 
				{
					PRINT_Error("[set SCTP_PEER_ADDR_PARAMS error] %d:%s\n", errno, strerror(errno));
					return;
				}
				PRINT_Info("[set SCTP_PEER_ADDR_PARAMS]\n");
			} 
			else 
			{
				ret = getsockopt(cfd, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, (void *)&paddrparams, &socklen);
				if (ret < 0) 
				{
					PRINT_Error("[get SCTP_PEER_ADDR_PARAMS error] %d:%s\n", errno, strerror(errno));
					return;
				}
				PRINT_Info("[get SCTP_PEER_ADDR_PARAMS] hb = %d, sackdelay = %d, %s, %s, %s\n", 
					paddrparams.spp_hbinterval,
					paddrparams.spp_sackdelay,
					(paddrparams.spp_flags & SPP_HB_ENABLE)?"HB_ENABLE":"HB_DISABLE",
					(paddrparams.spp_flags & SPP_PMTUD_ENABLE)?"PMTUD_ENABLE":"PMTUD_DISABLE",
					(paddrparams.spp_flags & SPP_SACKDELAY_ENABLE)?"SACKDELAY_ENABLE":"SACKDELAY_DISABLE");
			}
		}
	}
}

static void inline print_addresses(int fd, unsigned char *from)
{
	struct sockaddr *sa;
	unsigned char dest[INET6_ADDRSTRLEN] = {0};

	sa = (struct sockaddr *)from;
	if (sa->sa_family == AF_INET) 
	{
		printf("IP = %s, port = %d\n", 
		inet_ntoa(((struct sockaddr_in *)sa)->sin_addr), 
		ntohs(((struct sockaddr_in *)sa)->sin_port));
	} 
	else if (sa->sa_family == AF_INET6) 
	{
		inet_ntop(AF_INET6, (void *)&(((struct sockaddr_in6 *)sa)->sin6_addr), dest, INET6_ADDRSTRLEN);
		printf("IP = %s, port = %d\n", dest, ntohs(((struct sockaddr_in6 *)sa)->sin6_port));
	}
}

void *listen_thd(void *argv)
{
	int sfd;
	int cfd; 
	int ret;
	unsigned char from[2*sizeof(struct sockaddr_in6)];
	socklen_t fromlen = sizeof(from);

	sfd = create_socket(false);
	if (sfd < 0) 
	{
		return;
	}
	ret = listen(sfd, 5);
	if (ret < 0) 
	{
		PRINT_Error("[listen error] %d:%s\n", errno, strerror(errno));
		close(sfd);
		return;
	}
	PRINT_Info("[listen] fd = %d\n", sfd); 
	add_to_fd_list(sfd, LISTEN_SK_TYPE, SK_STATE_LISTENING);
	sctp_rto_param(sfd, SCTP_SET_PARA);

	while(1) 
	{
		cfd = accept( sfd, (struct sockaddr *)from, (int *)&fromlen );
		if (cfd < 0) 
		{
			PRINT_Error("[accept error] %d:%s \n", errno, strerror(errno));
			break;
		}
		sctp_set_non_block(cfd);
		add_to_epoll(cfd);
		sctp_set_events(cfd);
		PRINT_Info("[accept] from: ");
		print_addresses(cfd, from);
		PRINT_Info("[accept] fd = %d\n", cfd);
		add_to_fd_list(cfd, CONN_SK_TYPE, SK_STATE_ACCEPTED);
	}

	PRINT_Info("[listen thread exit] fd = %d\n", sfd);
}

static bool inline send_sctp_msg(int fd, char* msg, int size)
{
	unsigned char buf[MAX_BUFF_SIZE];
	int len;
	int ret;

	if (fd <= 0) 
	{
		PRINT_Error("invalid fd %d\n", fd);
		return false;
	}

	memset(buf, 0, sizeof(buf));
	if (msg && size < MAX_BUFF_SIZE) 
	{
		memcpy(buf, msg, size);
		len = size;
	} 
	else 
	{
		sprintf(buf, "this is a message of length %d.", g_message_len);
		len = g_message_len;
	}
	ret = sctp_sendmsg(fd,
	     (const void *)buf,
	     (size_t) len,
	     NULL,
	     (socklen_t) 0,
	     (u_int32_t) htonl(0),
	     (u_int32_t) 0,
	     (u_int16_t) 0,
	     (u_int32_t) 0,
	     (u_int32_t) 0);
	if (ret == -1) 
	{
		if (errno == EAGAIN) 
		{
			PRINT_Debug("[sctp_sendmsg EAGAIN]\n");
			return true;
		}
		PRINT_Error("[sctp_sendmsg error] %d:%s\n", errno, strerror(errno));
		clean_fd(fd);
		return false;
	}
	if (g_data_performance_test != 1)
		PRINT_Info("[sctp_sendmsg] fd = %d, %s\n", fd, buf);
	return true;
}

static void inline data_performance_test(int fd)
{
	int i = 0;
	time_t current;

	sctp_set_block(fd);
	if (!send_sctp_msg(fd, DATA_PERFORMANCE_TEST_START, sizeof(DATA_PERFORMANCE_TEST_START)))
		return;
	g_data_performance_test = 1;
	g_message_received = 0;
	g_message_total_size = 0;
	time(&g_first);
	g_last = g_first;
	PRINT_Info("[start] 0 second\n");
	while (g_data_pt_time == 0 || i < g_data_pt_time) 
	{
		if (!send_sctp_msg(fd, NULL, 0))
			break;
		time(&current);
		g_message_received++;
		g_message_total_size += g_message_len;
		if (current != g_last) 
		{
			i++;
			g_last = current;
			PRINT_Info("[processing...] %d seconds, %ul data sent, size = %ul\n", current - g_first, g_message_received, g_message_total_size);
		}
	}
	g_data_performance_test = 0;
	send_sctp_msg(fd, DATA_PERFORMANCE_TEST_END, sizeof(DATA_PERFORMANCE_TEST_END));
	time(&g_last);
	PRINT_Info("[end] %d seconds, total %d data sent, size = %d\n", g_last - g_first, g_message_received, g_message_total_size);
	sctp_set_non_block(fd);
}

static bool inline sctp_get_stats(int fd)
{
	int l_len = 0;
	int ret;
	struct sctp_assoc_stats l_gas;

	memset(&l_gas, 0, sizeof(l_gas));
	l_len = sizeof(l_gas);

	ret = getsockopt(fd, IPPROTO_SCTP, SCTP_GET_ASSOC_STATS, &l_gas, &l_len);
	if (ret < 0) 
	{
		PRINT_Error("[get SCTP_GET_ASSOC_STATS error] %d:%s\n", errno, strerror(errno));
		return false;
	} 
	else
		PRINT_Info("[get SCTP_GET_ASSOC_STATS] sas_opackets = %lu, sas_ipackets = %lu\n", 
		  l_gas.sas_opackets,
		  l_gas.sas_ipackets);
	return true;
}

bool process_sctp_msg(int fd)
{
	int l_len = 0;
	int l_msg_flags;
	int ret;
	struct sctp_sndrcvinfo rcvinfo;
	union sctp_notification *l_notif;
	struct sctp_assoc_change *l_asschg;
	struct sctp_shutdown_event *l_shutdown;
	struct msghdr *p_msghdr;
	unsigned char from[2*sizeof(struct sockaddr_in6)];
	socklen_t fromlen = sizeof(from);
	unsigned char buf[MAX_BUFF_SIZE];

	memset(buf, 0, sizeof(buf));
	errno = 0;
	l_msg_flags = 0;
	ret = sctp_recvmsg((int) fd,
	                  (void *)buf,
	                  (size_t)MAX_BUFF_SIZE,
	                  (struct sockaddr *)from,
	                  &fromlen,
	                  (struct sctp_sndrcvinfo *)&rcvinfo,
	                  (int *)&l_msg_flags);
	if (ret == -1) 
	{
		if (errno != EAGAIN) 
		{
			PRINT_Error("[sctp_recvmsg error] %d:%s\n", errno, strerror(errno));
			clean_fd(fd);
		}
		return false;
	}
	PRINT_Info("[sctp_recvmsg] from: ");
	print_addresses(fd, from);

	if (l_msg_flags & MSG_NOTIFICATION) 	//收到一条通知消息
	{
		PRINT_Info("[sctp_recvmsg] fd = %d\n", fd);
		l_notif = (union sctp_notification *)buf;
		if (l_notif->sn_header.sn_type == SCTP_ASSOC_CHANGE) 
		{
			l_asschg = &l_notif->sn_assoc_change;
			switch (l_asschg->sac_state) 
			{
			case SCTP_COMM_UP:
				update_fd_state(fd, SK_STATE_COMM_UP);
				PRINT_Info("notification = SCTP_COMM_UP\n");
				if (g_performance_test) 
				{
					g_success_cnt++;
					switch (g_success_cnt)
					{
						case 10000:
						case 20000:
						case 30000:
						case 40000:
						case 50000:
						case 60000:
						time(&g_last);
						PRINT_Info("[processing...] %d seconds, %d\n", g_last - g_first, g_success_cnt);
						default:
						break;
					}
					if (g_success_cnt == 1)
					{
						time(&g_first);
						if (g_client_server == 0 && g_performance_test) 
						{  // client side
							g_first_fd = fd;
							send_sctp_msg(fd, PERFORMANCE_TEST_START, sizeof(PERFORMANCE_TEST_START));
							PRINT_Info("[start] 0 second\n");
						}
					}
				 } 
				else
				{
					;
				}
				break;
			case SCTP_COMM_LOST:
				PRINT_Info("notification = SCTP_COMM_LOST\n");
				clean_fd(fd);
			break;
			case SCTP_SHUTDOWN_COMP:
				PRINT_Info("notification = SCTP_SHUTDOWN_COMP\n");
				clean_fd(fd);
			break;
			case SCTP_CANT_STR_ASSOC:
				PRINT_Info("notification = SCTP_CANT_STR_ASSOC\n");
				clean_fd(fd);
			break;
			default:
				PRINT_Info("UNKNOWN_NOTIFICATION = %d\n", l_asschg->sac_state);
				clean_fd(fd);
			break;
			}
		} 
		else if (l_notif->sn_header.sn_type == SCTP_SHUTDOWN_EVENT) 
		{
			l_shutdown = &l_notif->sn_shutdown_event;
			if (l_shutdown->sse_type == SCTP_SHUTDOWN_EVENT) 
			{
				PRINT_Info("notification = SCTP_SHUTDOWN_EVENT\n");
				clean_fd(fd);
			}
		}
	} 
	else if (l_msg_flags & MSG_EOR) 	//收到一条DATA
	{
		if (!g_data_performance_test) 
		{
			PRINT_Info("[sctp_recvmsg] fd = %d, %s\n", fd, buf);
		}
		else
		{
			g_message_received++;
			g_message_total_size += ret;
		}
		if (strcmp(buf, PERFORMANCE_TEST_START) == 0)
		{
			g_success_cnt = 1;
			g_performance_test = 1;
			time(&g_first);
			PRINT_Info("[start] 0 second\n");
		}
		else if (strcmp(buf, PERFORMANCE_TEST_END) == 0) 
		{
			time(&g_last);
			PRINT_Info("[end] %d seconds, %d\n", g_last - g_first, g_success_cnt);
			/* disable the print log for all of the connection sockets */
			update_all_cnn_fds();
			g_performance_test = 0;
			g_success_cnt = 0;
		}
		else if(strcmp(buf, DATA_PERFORMANCE_TEST_START) == 0) 
		{
			g_data_performance_test = 1;
			g_message_received = 0;
			g_message_total_size = 0;
			time(&g_first);
			PRINT_Info("[start] 0 second\n");
		}
		else if (strcmp(buf, DATA_PERFORMANCE_TEST_END) == 0)
		{
			g_data_performance_test = 0;
			time(&g_last);
			PRINT_Info("[sctp_recvmsg] fd = %d, %s\n", fd, buf);
			g_message_total_size -= ret;
			g_message_received--;
			PRINT_Info("[end] %d seconds, total %d data received, size = %d\n", g_last - g_first, g_message_received, g_message_total_size);
		}
	} 
	else 
		PRINT_Info("[sctp_recvmsg] fd = %d, message flag = %d\n", fd, l_msg_flags);

	return true;
}

void *process_epoll_events(void *argv)
{
	int epoll_cnt;
	int index;
	int fd;
	struct epoll_event epevents[EPOLLEVENTS];
	struct epoll_event ev;

	while(1)
	{
		errno = 0;
		epoll_cnt = epoll_wait(g_epfd, epevents, EPOLLEVENTS, -1);
		for(index = 0 ; index < epoll_cnt; index++)
		{
			fd = epevents[index].data.fd;
			if (epevents[index].events & EPOLLERR) 
			{
				PRINT_Info("[EPOLLERR] fd = %d\n", fd);
				clean_fd(fd);
			}
			else if (epevents[index].events & EPOLLIN) 
			{
				process_sctp_msg(fd);
				memset(&ev, 0, sizeof(ev));
				ev.events = EPOLLIN | EPOLLET;
				ev.data.fd = fd;
				if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &ev) < 0) 
				{
				}
			}
			else if (epevents[index].events & EPOLLOUT)
			{
				PRINT_Info("[EPOLLOUT]\n");
			}
		}
	}
}

bool set_server_addr(struct sockaddr_in6 *servaddr_v6)
{
	struct sockaddr *p_addr;
	int addr_n;

	bzero( (void *)servaddr_v6, sizeof(struct sockaddr_in6) );
	p_addr = (struct sockaddr *) servaddr_v6;

	addr_n = 0;
	errno = 0;
	if (g_test_v4) 
	{
		((struct sockaddr_in *)p_addr)->sin_family = AF_INET;
		((struct sockaddr_in *)p_addr)->sin_port = htons(g_remote_port);
		((struct sockaddr_in *)p_addr)->sin_addr.s_addr = inet_addr(g_remote_ip_1);
	} 
	else if (g_test_v6) 
	{
		((struct sockaddr_in6 *)p_addr)->sin6_family = AF_INET6;
		((struct sockaddr_in6 *)p_addr)->sin6_port = htons(g_remote_port);
		if (inet_pton(AF_INET6, g_remote_ip_1, &(((struct sockaddr_in6 *)p_addr)->sin6_addr)) < 0)
			return false;
	} 
	else
		return false;

	return true;
}

void new_sctp_connect()
{
	int fd;
	int ret;
	int index;
	struct sockaddr_in6 servaddr_v6[2];

	if (!set_server_addr(&(servaddr_v6[0]))) 
	{
		PRINT_Error("[SET_SERVER_ADDR_ERROR]\n");
		return;
	}

	g_failure_cnt = 0;
	g_success_cnt = 0;

	for (index = 0; index < g_max_conn_num && g_success_cnt < g_max_conn_num; index++)
	{
		fd = create_socket(g_performance_test?false:true);
		if (fd < 0)
		{
			PRINT_Error("[create socket error] %d:%s\n", errno, strerror(errno));
			if (!g_performance_test) 
			{
				return;
			}
			g_failure_cnt++;
			continue;
		}
		errno = 0;
		if (g_connect_mod == 0) 
		{
			ret = connect( fd, (struct sockaddr *)servaddr_v6, sizeof(servaddr_v6) );
		} 
		else 
		{
			ret = setsockopt( fd, SOL_SCTP, SCTP_SOCKOPT_CONNECTX, (struct sockaddr *)&servaddr_v6, g_test_v4?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6) );
		}
		if (ret < 0 && errno != EINPROGRESS) 
		{
			PRINT_Error("[connect error] port = %d, %d:%s\n", g_local_port, errno, strerror(errno));
			close(fd);
			if (!g_performance_test) 
			{
				return;
			}
			g_failure_cnt++;
			continue;
		}
		add_to_epoll(fd);
		add_to_fd_list(fd, CONN_SK_TYPE, SK_STATE_CONNECTING);
		PRINT_Info("[connect] fd = %d, state = connecting\n", fd);
		if (!g_performance_test) 
			return;
		g_local_port++;
		if (g_local_port == 0)
			g_local_port = 1;
	}
	send_sctp_msg(g_first_fd, PERFORMANCE_TEST_END, sizeof(PERFORMANCE_TEST_END));
	PRINT_Info("[end] %d seconds, %d\n", g_last - g_first, g_success_cnt);
	update_all_cnn_fds();
	g_performance_test = 0;
}

#define CLOSE_MD_SHUTDOWN  1
#define CLOSE_MD_ABORT   2

enum cmd_ids {
	CMD_CLOSE = 0,
	CMD_SHUTDOWN,
	CMD_ABORT,
	CMD_GET_CT,
	CMD_SEND,
	CMD_DATA_PT,
	CMD_SET_PATH,
	CMD_GET_PATH,
	CMD_SET_RTO,
	CMD_GET_RTO,
	CMD_DATA_SIZE,
	CMD_DATA_TIME,
	CMD_HB_INT,
	CMD_SACKDELAY,
	CMD_PMTU,
	CMD_RTO_INIT,
	CMD_RTO_MIN,
	CMD_RTO_MAX,
	CMD_CONNECT_MOD,
	CMD_LPORT,
	CMD_RPORT,
	CMD_V6_MOD,
	CMD_MULTI,
	CMD_SADDR1,
	CMD_SADDR2,
	CMD_DADDR1,
	CMD_DADDR2,
	CMD_CONNECT,
	CMD_LISTEN,
	CMD_LIST_FD,
	CMD_CAT_ASSOCS,
	CMD_CLEAN,
	CMD_PT,
	CMD_MAX_ID
};

#define CMD_FD_MIN   CMD_CLOSE
#define CMD_FD_MAX   CMD_GET_RTO

#define CMD_VALUE_MIN  CMD_DATA_SIZE
#define CMD_VALUE_MAX  CMD_MULTI

#define CMD_CHAR_MIN  CMD_SADDR1
#define CMD_CHAR_MAX  CMD_DADDR2

struct cmd_list {
 unsigned char cmd_id;
 unsigned char cmd_name[MAX_CMD_LEN];
 unsigned char cmd_description[MAX_CMD_DISC_LEN];
} g_cmd_list[CMD_MAX_ID] = {
	{CMD_CLOSE        , "close"    , "[fd], close the socket                           "},
	{CMD_SHUTDOWN     , "shutdown" , "[fd], shutdown the association                   "},
	{CMD_ABORT        , "abort"    , "[fd], abort the association                      "},
	{CMD_GET_CT       , "getct"    , "[fd], get the counters                           "},
	{CMD_SEND         , "send"     , "[fd], send a data                                "},
	{CMD_DATA_PT      , "dt"       , "[fd], performance test for sending data          "},
	{CMD_SET_PATH     , "setp"     , "[fd], set path parameters                        "},
	{CMD_GET_PATH     , "getp"     , "[fd], get path parameters                        "},
	{CMD_SET_RTO      , "setr"     , "[fd], set rto parameters                         "},
	{CMD_GET_RTO      , "getr"     , "[fd], get rto parameters                         "},
	{CMD_DATA_SIZE    , "size"     , "[value], set the size of the data to be sent     "},
	{CMD_DATA_TIME    , "st"       , "[value], set the data sending time (seconds)     "},
	{CMD_HB_INT       , "hb"       , "[value], set the heartbeat interval              "},
	{CMD_SACKDELAY    , "sackdelay", "[value], set the sackdelay value                 "},
	{CMD_PMTU         , "pmtu"     , "[value], enable(1)/disable(0) pmtu               "},
	{CMD_RTO_INIT     , "rtoi"     , "[value], set the rto initial value               "},
	{CMD_RTO_MIN      , "rtom"     , "[value], set the rto min value                   "},
	{CMD_RTO_MAX      , "rtox"     , "[value], set the rto max value                   "},
	{CMD_CONNECT_MOD  , "cm"       , "[value], connect model (0:connect; 1: setsockopt)"},
	{CMD_LPORT        , "lport"    , "[value], set the local port                      "},
	{CMD_RPORT        , "rport"    , "[value], set the remote port                     "},
	{CMD_V6_MOD       , "v6"       , "[value], 0: IPv4; 1:IPv6                         "},
	{CMD_MULTI        , "multi"    , "[value], 0: single-homing; 1:multi-homing        "},
	{CMD_SADDR1       , "saddr1"   , "[ip], set the primary source ip                  "},
	{CMD_SADDR2       , "saddr2"   , "[ip], set the secondary source ip                "},
	{CMD_DADDR1       , "daddr1"   , "[ip], set the primary dest ip                    "},
	{CMD_DADDR2       , "daddr2"   , "[ip], set the secondary dest ip                  "},
	{CMD_CONNECT      , "connect"  , "[null], create a new connect                     "},
	{CMD_LISTEN       , "listen"   , "[null], create a listen socket                   "},
	{CMD_LIST_FD      , "ls"       , "[null], list the fds                             "},
	{CMD_CAT_ASSOCS   , "cat"      , "[null], cat the assocs file                      "},
	{CMD_CLEAN        , "clean"    , "[null], clean the fds                            "},
	{CMD_PT           , "pt"       , "[null], start performance test                   "}
};

static void inline print_cmd_help()
{
	int i;

	for (i = 0; i < CMD_MAX_ID; i++) 
	{
		PRINT_Info("[command %d] %s %s\n", i, g_cmd_list[i].cmd_name, g_cmd_list[i].cmd_description);
	}
}

static int inline get_cmd_id(char *cmd)
{
	int i;

	if (cmd == NULL)
	return CMD_MAX_ID;

	for (i = 0; i < CMD_MAX_ID && !strstr(cmd, g_cmd_list[i].cmd_name); i++);

	return i;
}

void close_association(int fd, char model)
{
	struct linger l_lger;
	int ret;

	if (fd <= 0) 
	{
		PRINT_Error("[close a bad fd] fd = %d\n", fd);
		return;
	}

	if (model == CLOSE_MD_ABORT)
	{
		memset(&l_lger, 0, sizeof(l_lger));
		l_lger.l_onoff = 1;
		l_lger.l_linger = 0;
		ret = setsockopt(fd, SOL_SOCKET, SO_LINGER, &l_lger, sizeof(l_lger));
		if (ret < 0)
		{
			PRINT_Error("[set SO_LINGER error] %d:%s\n", errno, strerror(errno));
			return;
		}
	}

	clean_fd(fd);
	PRINT_Info("[closed] fd = %d\n", fd);
}

int g_cmd_fd = 0;
static void update_cmd_fd(int *fd)
{
	if (*fd != 0)
	{
		g_cmd_fd = *fd;
	}
	else if (g_cmd_fd != 0) 
	{
		*fd = g_cmd_fd;
	}
}

void process_input_command()
{
	char command[MAX_CMD_LEN] = {0};
	char s_value[MAX_CMD_LEN] = {0};
	int cmd_id;
	int fd;
	int value;
	int i;
	char *temp;
	pthread_attr_t attr = {0};
	pthread_t tid1 = 0, tid2 = 0;
	struct sched_param param = {0};

	pthread_attr_init (&attr);   
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	param.sched_priority = 99;
	pthread_attr_setschedparam (&attr, &param);

	while(1)
	{
		memset(command, 0, sizeof(command));
		fd = 0;
		value = 0;
		fgets(command, sizeof(command), stdin);
		cmd_id = get_cmd_id(command);
		if (cmd_id >= CMD_FD_MIN && cmd_id <= CMD_FD_MAX) 
		{
			fd = atoi(command + strlen(g_cmd_list[cmd_id].cmd_name));
		} 
		else if (cmd_id >= CMD_VALUE_MIN && cmd_id <= CMD_VALUE_MAX) 
		{
			value = atoi(command + strlen(g_cmd_list[cmd_id].cmd_name));
		} 
		else if (cmd_id >= CMD_CHAR_MIN && cmd_id <= CMD_CHAR_MAX) 
		{
			temp = strchr(command, ' ');
			memset(s_value, 0, sizeof(s_value));
			i = 0;
			while (*temp != 0 && i < sizeof(s_value)) 
			{
				if (*temp != ' ' && *temp != '\n')
				 	s_value[i++] = *temp;
				temp++;
			}
		}
		switch (cmd_id) 
		{
			case CMD_CLOSE:
			case CMD_SHUTDOWN:
				close_association(fd, CLOSE_MD_SHUTDOWN);
				g_cmd_fd = 0;
				break;
			case CMD_ABORT:
				close_association(fd, CLOSE_MD_ABORT);
				g_cmd_fd = 0;
				break;
			case CMD_GET_CT:
				update_cmd_fd(&fd);
				sctp_get_stats(fd);
				break;
			case CMD_SEND:
				update_cmd_fd(&fd);
				send_sctp_msg(fd, NULL, 0);
				break;
			case CMD_DATA_PT:
				update_cmd_fd(&fd);
				data_performance_test(fd);
				break;
			case CMD_SET_PATH:
				update_cmd_fd(&fd);
				sctp_peer_param(fd, SCTP_SET_PARA);
				break;
			case CMD_GET_PATH:
				update_cmd_fd(&fd);
				sctp_peer_param(fd, SCTP_GET_PARA);
				break;
			case CMD_SET_RTO:
				update_cmd_fd(&fd);
				sctp_rto_param(fd, SCTP_SET_PARA);
				break;
			case CMD_GET_RTO:
				update_cmd_fd(&fd);
				sctp_rto_param(fd, SCTP_GET_PARA);
				break;
			case CMD_DATA_SIZE:
				g_message_len = value;
				PRINT_Info("[message length] %d\n", g_message_len);
				break;
			case CMD_DATA_TIME:
				g_data_pt_time = value;
				PRINT_Info("[st] %d\n", g_data_pt_time);
				break;
			case CMD_HB_INT:
				g_hbinterval = value;
				PRINT_Info("[heartbeat interval] %d, %s\n", g_hbinterval, g_hbinterval == 0?"disabled":"enabled");
				break;
			case CMD_SACKDELAY:
				g_sackdelay = value;
				PRINT_Info("[sack delay] %d, %s\n", g_sackdelay, g_sackdelay == 0?"disabled":"enabled");
				break;
			case CMD_PMTU:
				g_pmtu = (value == 0)?0:1;
				PRINT_Info("[pmtu] %s\n", (g_pmtu == 0)?"disabled":"enabled");
				break;
			case CMD_RTO_INIT:
				g_rto_init = value;
				PRINT_Info("[rto_init] %d\n", g_rto_init);
				break;
			case CMD_RTO_MIN:
				g_rto_min = value;
				PRINT_Info("[rto_min] %d\n", g_rto_min);
				break;
			case CMD_RTO_MAX:
				g_rto_max = value;
				PRINT_Info("[rto_max] %d\n", g_rto_max);
				break;
			case CMD_CONNECT_MOD:
				g_connect_mod = (value == 0)?0:1;
				PRINT_Info("[connect model] %s\n", (g_connect_mod == 0)?"connect":"setsockopt");
				break;
			case CMD_LPORT:
				g_local_port = value;
				PRINT_Info("[local port] %d\n", g_local_port);
				break;
			case CMD_RPORT:
				g_remote_port = value;
				PRINT_Info("[remote port] %d\n", g_remote_port);
				break;
			case CMD_V6_MOD:
				g_test_v6 = (value != 0)?1:0;
				g_test_v4 = (g_test_v6 == 0)?1:0;
				PRINT_Info("[%s]\n", (g_test_v6 == 1)?"IPv6 test":"IPv4 test");
				break;
			case CMD_MULTI:
				g_multi_homing = (value == 0)?0:1;
				PRINT_Info("[%s]\n", (g_multi_homing == 0)?"single-homing":"multi-homing");
				break;
			case CMD_SADDR1:
				memcpy(g_local_ip_1, s_value, sizeof(g_local_ip_1));
				PRINT_Info("[primary source ip] %s\n", g_local_ip_1);
				break;
			case CMD_SADDR2:
				memcpy(g_local_ip_2, s_value, sizeof(g_local_ip_2));
				PRINT_Info("[secondary source ip] %s\n", g_local_ip_2);
				break;
			case CMD_DADDR1:
				memcpy(g_remote_ip_1, s_value, sizeof(g_remote_ip_1));
				PRINT_Info("[primary remoute ip] %s\n", g_remote_ip_1);
				break;
			case CMD_DADDR2:
				memcpy(g_remote_ip_2, s_value, sizeof(g_remote_ip_2));
				PRINT_Info("[secondary source ip] %s\n", g_remote_ip_2);
				break;
			case CMD_CONNECT:
				new_sctp_connect();
				break;
			case CMD_LISTEN:
				if (pthread_create(&tid2, &attr, (void *)listen_thd, (void *)NULL) < 0) 
				{
					PRINT_Error("[pthread_create listen_thd error] %d:%s\n", errno, strerror(errno));
				}
				break;
			case CMD_LIST_FD:
				print_fd_list();
				break;
			case CMD_CAT_ASSOCS:
				PRINT_Info("[cat /proc/net/sctp/eps]\n");
				system("cat /proc/net/sctp/eps");
				PRINT_Info("[cat /proc/net/sctp/assocs]\n");
				system("cat /proc/net/sctp/assocs");
				PRINT_Info("[cat /proc/net/sctp/remaddr]\n");
				system("cat /proc/net/sctp/remaddr");
				PRINT_Info("[cat end]\n");
				break;
			case CMD_CLEAN:
				clean_all_cnn_fds();
				break;
			case CMD_PT:
				if (g_client_server != 0) 
				{
					PRINT_Debug("[pt] cannot be executed at the server side\n");
					break;
				}
				PRINT_Info("wait for 2 seconds for cleaning...\n");
				clean_all_cnn_fds();
				sleep(2);
				g_performance_test = 1;
				new_sctp_connect();
				break;
			default:
				print_cmd_help();
			break;
		}
	}
}

/* 
 -l4: local ipv4 address
 -l6: local ipv6 address
 -lp: local port
 -r4: remote ipv4 address
 -r6: remote ipv6 address
 -rp: remote port
 -c:  client
 -s:  server
 -pt: performance test
 -mc: max connection number
 -ml: send message length
 -cm: connection model
*/
void print_help(void)
{
	PRINT_Info(" -h: print help command\r\n");
	PRINT_Info(" -l4: local ipv4 address\r\n");
	PRINT_Info(" -l6: local ipv6 address\r\n");
	PRINT_Info(" -lp: local port\r\n");
	PRINT_Info(" -r4: remote ipv4 address\r\n");
	PRINT_Info(" -r6: remote ipv6 address\r\n");
	PRINT_Info(" -rp: remote port\r\n");
	PRINT_Info(" -c:  client\r\n");
	PRINT_Info(" -s:  server\r\n");
	PRINT_Info(" -pt: performance test\r\n");
	PRINT_Info(" -mc: max connection number\r\n");
	PRINT_Info(" -ml: send message length\r\n");
	PRINT_Info(" -cm: connection model\r\n");
}

bool get_parameters(int argc, char ** argv)
{
	int i, la = 0, ra = 0;

	g_multi_homing = 0;
	g_performance_test = 0;

	if(argc == 1)
	{
		print_help();
		return false;
	}
	for (i = 1; i < argc; i++) 
	{
		if (argv[i][0] != '-') 
		{
			continue;
		}

		if (argv[i][1] == 'h' && argc == 2)
		{
			print_help();
			return false;
		}
		else if (argv[i][1] == 'l' && argv[i][2] == '4') 
		{
			if (g_test_v6) 
			{
				PRINT_Error("[mixed IPv4 and IPv6 address]\n");
				return false;
			}
			if (la == 0) 
			{
				strcpy(g_local_ip_1, &(argv[i + 1][0]));
				la = 1;
				g_test_v4 = 1;
				PRINT_Info("[primary source IP] %s\n", g_local_ip_1);
			} 
			else if (la == 1) 
			{
				strcpy(g_local_ip_2, &(argv[i + 1][0]));
				la = 2;
				g_multi_homing = 1;
				PRINT_Info("[secondary source IP] %s\n", g_local_ip_2);
			}
		} 
		else if (argv[i][1] == 'r' && argv[i][2] == '4') 
		{
			if (g_test_v6) 
			{
				PRINT_Error("[mixed IPv4 and IPv6 address]\n");
				return false;
			}
			if (ra == 0) 
			{
				strcpy(g_remote_ip_1, &(argv[i + 1][0]));
				ra = 1;
				g_test_v4 = 1;
				PRINT_Info("[primary dest IP] %s\n", g_remote_ip_1);
			} 
			else if (ra == 1) 
			{
				strcpy(g_remote_ip_2, &(argv[i + 1][0]));
				ra = 2;
				PRINT_Info("[secondary dest IP] %s\n", g_remote_ip_2);
			}
		} 
		else if (argv[i][1] == 'l' && argv[i][2] == '6') 
		{
			if (g_test_v4) 
			{
				PRINT_Error("[mixed IPv4 and IPv6 address]\n");
				return false;
			}
			if (la == 0) 
			{
				strcpy(g_local_ip_1, &(argv[i + 1][0]));
				la = 1;
				g_test_v6 = 1;
				PRINT_Info("[primary source IP]: %s\n", g_local_ip_1);
			} 
			else if (la == 1) 
			{
				strcpy(g_local_ip_2, &(argv[i + 1][0]));
				la = 2;
				g_multi_homing = 1;
				PRINT_Info("[secondary source IP] %s\n", g_local_ip_2);
			}
		} 
		else if (argv[i][1] == 'r' && argv[i][2] == '6') 
		{
			if (g_test_v4) 
			{
				PRINT_Error("[mixed IPv4 and IPv6 address]\n");
				return false;
			}
			if (ra == 0) 
			{
				strcpy(g_remote_ip_1, &(argv[i + 1][0]));
				g_test_v6 = 1;
				ra = 1;
				PRINT_Info("[primary dest IP] %s\n", g_remote_ip_1);
			} 
			else if (ra == 1) 
			{
				strcpy(g_remote_ip_2, &(argv[i + 1][0]));
				ra = 2;
				PRINT_Info("[secondary dest IP] %s\n", g_remote_ip_2);
			}
		} 
		else if (argv[i][1] == 'l' && argv[i][2] == 'p') 
		{
			g_local_port = atoi(&argv[i + 1][0]);
			PRINT_Info("[local port] %d\n", g_local_port);
		} 
		else if (argv[i][1] == 'r' && argv[i][2] == 'p') 
		{
			g_remote_port = atoi(&argv[i + 1][0]);
			PRINT_Info("[remote port] %d\n", g_remote_port);
		} 
		else if (argv[i][1] == 'c' && argv[i][2] == 'm') 
		{
			g_connect_mod = 1;
			PRINT_Info("connect model is set to \"setsockopt\"\n");
		} 
		else if (argv[i][1] == 'c') 
		{
			g_client_server = 0;
			PRINT_Info("[client]\r\n");
		} 
		else if (argv[i][1] == 's') 
		{
			g_client_server = 1;
			PRINT_Info("[server]\r\n");
		} 
		else if (argv[i][1] == 'p' && argv[i][2] == 't') 
		{
			g_performance_test = 1;
			PRINT_Info("[performance test]\r\n");
		} 
		else if (argv[i][1] == 'm' && argv[i][2] == 'c') 
		{
			g_max_conn_num = atoi(&argv[i + 1][0]);
			PRINT_Info("[max connection number] %d\n", g_max_conn_num);
		} 
		else if (argv[i][1] == 'm' && argv[i][2] == 'l') 
		{
			g_message_len = atoi(&argv[i + 1][0]);
			if (g_message_len > MAX_BUFF_SIZE - 1) 
			{
				PRINT_Debug("[too large message length] %d, max = %d\n", g_message_len, MAX_BUFF_SIZE - 1);
				g_message_len = DEF_BUFF_SIZE;
			}
			PRINT_Info("[message length] %d\n", g_message_len);
		} 
		else
			PRINT_Error("[unkown parameter] %s\n", argv[i]);
	}
	return true;
}

int main(int argc, char ** argv)
{
	int fd, ret;
	struct epoll_event epevents[EPOLLEVENTS];
	pthread_attr_t attr = {0};
	struct sched_param param = {0};
	pthread_t tid1 = 0, tid2 = 0;

	PRINT_Info("***sctp-test-tool version %s***\n\n", TOOL_VERSION);

	if (!(get_parameters(argc, argv)))
		return -1;

	memset(g_fd_list, 0, sizeof(g_fd_list));
	pthread_mutex_init(&fd_list_lock, NULL);

	pthread_attr_init (&attr);   
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	param.sched_priority = 99;
	pthread_attr_setschedparam (&attr, &param);

	g_epfd = epoll_create(g_max_conn_num*2);
	if (g_epfd < 0) 
	{
		PRINT_Error("[EPOLL_ERROR] %d:%s \n", errno, strerror(errno));
		return -1;
	}

	ret = pthread_create(&tid1, &attr, (void *)process_epoll_events, (void *)NULL);
	if (ret < 0) 
	{
		PRINT_Error("[pthread_create error] %d:%s\n", errno, strerror(errno));
		return -1;
	}

	if (g_client_server == 0) 
	{ // client side
		new_sctp_connect();
	} 
	else 
	{// server side
		ret = pthread_create(&tid2, &attr, (void *)listen_thd, (void *)NULL);
		if (ret < 0) 
		{
			PRINT_Error("[pthread_create error] %d:%s\n", errno, strerror(errno));
			return -1;
		}
	}

	process_input_command();
	pthread_mutex_destroy(&fd_list_lock);

	return 0;
}
