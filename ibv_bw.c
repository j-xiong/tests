#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

enum test_type {
	READ,
	WRITE,
	SEND
};

#define CHECK_ERROR(stmt) \
        do { \
        	int err = (stmt); \
        	if (err) { \
			printf("%s returned error %d\n", #stmt, (err)); \
			goto err_out; \
		} \
	} while (0)

#define CHECK_NULL(stmt) \
        do { \
        	if (!(stmt)) { \
			printf("%s returned NULL\n", #stmt); \
			goto err_out; \
		} \
	} while (0)

#define MAX_SIZE	(4*1024*1024)
#define MIN_PROXY_BLOCK	(131072)
#define TX_DEPTH	(128)
#define RX_DEPTH	(128)

static struct business_card {
	int		lid;
	int		qpn;
	int		psn;
	union ibv_gid	gid;
	uint64_t	buf_addr;
	uint64_t	buf_rkey;
} me, peer;

static struct ibv_device	**dev_list;
static struct ibv_device	*dev;
static struct ibv_context	*context;
static struct ibv_pd		*pd;
static struct ibv_mr		*mr;
static struct ibv_cq		*cq;
static struct ibv_qp		*qp;

static void			*buf;
static size_t			buf_size;

static int			use_sync_ib;
static int			use_inline_send;
static int			gid_idx = -1;
static int			mtu = -1;

static double when(void)
{
	struct timeval tv;
	static struct timeval tv0;
	static int first = 1;
	int err;
 
	err = gettimeofday(&tv, NULL);
	if (err) {
		perror("gettimeofday");
		return 0;
	}
 
	if (first) {
		tv0 = tv;
		first = 0;
	}
	return (double)(tv.tv_sec - tv0.tv_sec) * 1.0e6 + (double)(tv.tv_usec - tv0.tv_usec);
}

int connect_tcp(char *host, int port)
{
	struct sockaddr_in sin;
	struct hostent *addr;
	int sockfd, newsockfd;
	int clen;

	memset(&sin, 0, sizeof(sin));

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
		perror("socket");
		exit(-1);
	}
 
	if (host) {
		if (atoi(host) > 0) {
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = inet_addr(host);
		} else {
			if ((addr = gethostbyname(host)) == NULL){
				printf("invalid hostname '%s'\n", host);
				exit(-1);
			}
			sin.sin_family = addr->h_addrtype;
			memcpy(&sin.sin_addr.s_addr, addr->h_addr, addr->h_length);
		}
		sin.sin_port = htons(port);
		if(connect(sockfd, (struct sockaddr *) &sin, sizeof(sin)) < 0){
			perror("connect");
			exit(-1);
		}
		return sockfd;
	} else {
		int one = 1;
		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int))) {
			perror("setsockopt");
			exit(-1);
		}
		memset(&sin, 0, sizeof(sin));
		sin.sin_family      = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_ANY);
		sin.sin_port        = htons(port);
		if (bind(sockfd, (struct sockaddr *) &sin, sizeof(sin)) < 0){
			perror("bind");
			exit(-1);
		}
 
		listen(sockfd, 5);
		clen = sizeof(sin);
		newsockfd = accept(sockfd, (struct sockaddr *) &sin, &clen);
		if(newsockfd < 0) {
			perror("accept");
			exit(-1);
		}
 
		close(sockfd);
		return newsockfd;
	}
}

static void sync_tcp(int sockfd)
{
	int dummy1, dummy2;

	write(sockfd, &dummy1, sizeof dummy1);
	read(sockfd, &dummy2, sizeof dummy2);
}

static int exchange_info(int sockfd)
{
	if (write(sockfd, &me, sizeof me) != sizeof me) {
		fprintf(stderr, "Couldn't send local address\n");
		return -1;
	}

	if (read(sockfd, &peer, sizeof peer) != sizeof peer) {
		fprintf(stderr, "Couldn't read remote address\n");
		return -1;
	}

	return 0;
}

static void init_buf(size_t size)
{
	int page_size = sysconf(_SC_PAGESIZE);

	buf_size = size;

	posix_memalign(&buf, page_size, buf_size);

	if (!buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		exit(-1);
	}

err_out:
	return;
}

static void free_buf(void)
{
	free(buf);
}

static int mtu_to_size(int mtu)
{
	switch (mtu) {
	case IBV_MTU_256:
		return 256;
	case IBV_MTU_512:
		return 512;
	case IBV_MTU_1024:
		return 1024;
	case IBV_MTU_2048:
		return 2048;
	case IBV_MTU_4096:
		return 4096;
	default:
		fprintf(stderr, "Invalid MTU size %d\n", mtu);
		return -1;
	}
}

static int size_to_mtu(int size)
{
	switch (size) {
	case 256:
		return IBV_MTU_256;
	case 512:
		return IBV_MTU_512;
	case 1024:
		return IBV_MTU_1024;
	case 2048:
		return IBV_MTU_2048;
	case 4096:
		return IBV_MTU_4096;
	default:
		fprintf(stderr, "Invalid MTU size %d\n", size);
		return -1;
	}
}

static int connect_ib(int port, struct business_card *dest)
{
	struct ibv_qp_attr qp_attr = {};
	int qp_rtr_flags, qp_rts_flags;

	qp_attr.qp_state		= IBV_QPS_RTR;
	qp_attr.path_mtu		= mtu;
	qp_attr.dest_qp_num		= dest->qpn;
	qp_attr.rq_psn			= dest->psn;
	qp_attr.max_dest_rd_atomic	= 16;
	qp_attr.min_rnr_timer		= 12;
	qp_attr.ah_attr.is_global	= 0;
	qp_attr.ah_attr.dlid		= dest->lid;
	qp_attr.ah_attr.sl		= 0;
	qp_attr.ah_attr.src_path_bits	= 0;
	qp_attr.ah_attr.port_num	= port;

	if (dest->gid.global.interface_id) {
		qp_attr.ah_attr.is_global	= 1;
		qp_attr.ah_attr.grh.hop_limit	= 1;
		qp_attr.ah_attr.grh.dgid	= dest->gid;
		qp_attr.ah_attr.grh.sgid_index	= gid_idx;
	}

	qp_rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		       IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	CHECK_ERROR( ibv_modify_qp(qp, &qp_attr, qp_rtr_flags) );

	qp_attr.qp_state    = IBV_QPS_RTS;
	qp_attr.timeout	    = 14;
	qp_attr.retry_cnt   = 7;
	qp_attr.rnr_retry   = 7;
	qp_attr.sq_psn	    = 0;
	qp_attr.max_rd_atomic  = 16;

	qp_rts_flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
		       IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
		       IBV_QP_MAX_QP_RD_ATOMIC;

	CHECK_ERROR( ibv_modify_qp(qp, &qp_attr, qp_rts_flags) );
	return 0;

err_out:
	return -1;
}

static void free_ib(void)
{
	ibv_destroy_qp(qp);
	ibv_destroy_cq(cq);
	if (mr)
		ibv_dereg_mr(mr);
	ibv_dealloc_pd(pd);
	ibv_close_device(context);
	ibv_free_device_list(dev_list);
}

static int init_ib(int sockfd)
{
	int ib_port = 1;
	int mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	int qp_init_flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	struct ibv_qp_attr qp_attr = {};
	struct ibv_qp_init_attr qp_init_attr = {};
	struct ibv_port_attr port_attr;
	char gid_string[33];

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -ENODEV;
	}

	dev = *dev_list;
	if (!dev) {
		fprintf(stderr, "No IB devices found\n");
		return -ENODEV;
	}

	printf("Using IB device %s\n", ibv_get_device_name(dev));

	/* open dev, pd, mr, cq */
	CHECK_NULL( (context = ibv_open_device(dev)) );
	CHECK_NULL( (pd = ibv_alloc_pd(context)) );
	CHECK_NULL( (mr = ibv_reg_mr(pd, buf, buf_size, mr_access_flags)) );
	CHECK_NULL( (cq = ibv_create_cq(context, TX_DEPTH + RX_DEPTH, NULL, NULL, 0)) );

	/* create & initialize qp */
	qp_init_attr.send_cq = cq;
	qp_init_attr.recv_cq = cq;
	qp_init_attr.cap.max_send_wr  = TX_DEPTH * (MAX_SIZE / MIN_PROXY_BLOCK);
	qp_init_attr.cap.max_recv_wr  = RX_DEPTH;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.qp_type = IBV_QPT_RC;
	CHECK_NULL( (qp = ibv_create_qp(pd, &qp_init_attr)) );

	qp_attr.qp_state        = IBV_QPS_INIT;
	qp_attr.pkey_index      = 0;
	qp_attr.port_num        = ib_port;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	CHECK_ERROR( ibv_modify_qp(qp, &qp_attr, qp_init_flags) );

	/* get local address & mtu */
	CHECK_ERROR( ibv_query_port(context, ib_port, &port_attr) );

	if (mtu == -1)
		mtu = port_attr.active_mtu;

	printf("port %d, MTU %d, MTU size %d\n", ib_port, mtu, mtu_to_size(mtu));

	/* prepare my information */
	if (gid_idx >= 0)
		CHECK_ERROR( ibv_query_gid(context, ib_port, gid_idx, &me.gid) );
	else
		memset(&me.gid, 0, sizeof me.gid);

	me.lid = port_attr.lid;
	me.qpn = qp->qp_num;
	me.psn = 0;
	me.buf_addr = (uint64_t)mr->addr;
	me.buf_rkey = mr->rkey;

	inet_ntop(AF_INET6, &me.gid, gid_string, sizeof gid_string);

	printf("Me:\tlid 0x%04x, qpn 0x%06x, buf %p, rkey %lx, gid %s\n",
		me.lid, me.qpn, me.buf_addr, me.buf_rkey, gid_string);

	/* exchange information with peer */
	CHECK_ERROR( exchange_info(sockfd) );

	inet_ntop(AF_INET6, &peer.gid, gid_string, sizeof gid_string);

	printf("Peer:\tlid 0x%04x, qpn 0x%06x, buf %p, rkey %lx, gid %s\n",
		peer.lid, peer.qpn, peer.buf_addr, peer.buf_rkey, gid_string);

	/* connect */
	CHECK_ERROR( connect_ib(ib_port, &peer ) );
	return 0;

err_out:
	free_ib();
	return -1;
}

static int post_rdma(int test_type, size_t size, int idx, int signaled)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t)buf + idx * size,
		.length = size,
		.lkey	= mr->lkey
	};
	struct ibv_send_wr wr = {
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = test_type == READ ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE,
		.send_flags = signaled ? IBV_SEND_SIGNALED : 0,
		.wr	    = {
		  .rdma     = {
		    .remote_addr = peer.buf_addr + idx * size,
		    .rkey        = peer.buf_rkey,
		  }
		}
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(qp, &wr, &bad_wr);
}

static int post_send(size_t size, int idx, int signaled)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t)buf + idx * size,
		.length = size,
		.lkey	= mr->lkey
	};
	struct ibv_send_wr wr = {
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = (signaled ? IBV_SEND_SIGNALED : 0) |
			      (use_inline_send ? IBV_SEND_INLINE : 0)
	};
	struct ibv_send_wr *bad_wr;

	//printf("%s: size %ld, signaled %d, inline_send %d\n", __func__, size, signaled, use_inline_send);
	return ibv_post_send(qp, &wr, &bad_wr);
}

static int post_recv(size_t size, int idx)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t)buf + idx * size,
		.length = size,
		.lkey	= mr->lkey
	};
	struct ibv_recv_wr wr = {
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;

	//printf("%s: size %ld\n", __func__, size);
	return ibv_post_recv(qp, &wr, &bad_wr);
}

void check_completions(struct ibv_wc *wc, int n)
{
	int i;
	static uint64_t errcnt = 0;

	for (i = 0; i < n; i++) {
		if (wc->status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Completion with error: %s. [total %ld]\n",
				ibv_wc_status_str(wc->status), ++errcnt);
		}
	}
}

static void sync_ib(size_t size)
{
	int n;
	int pending = 2;
	struct ibv_wc wc[2];

	CHECK_ERROR( post_recv(size, 0) );
	CHECK_ERROR( post_send(size, 0, 1) );

	while (pending > 0) {
		n = ibv_poll_cq(cq, 2, wc);
		if (n < 0) {
			fprintf(stderr, "poll CQ failed %d\n", n);
			return;
		} else if (n > 0) {
			check_completions(wc, n);
			pending -= n;
		}
	}

err_out:
	return;
}

void run_rdma_test(int test_type, int size, int iters, int batch)
{
	int i, completed, pending;
	int n;
	double t1, t2;
	struct ibv_wc wc[16];
	int signaled;

	t1 = when();
	for (i = completed = pending = 0; i < iters || completed < iters;) {
		while (i < iters && pending < TX_DEPTH) {
			signaled = (i % batch) == batch -1 || i == iters -1;
			CHECK_ERROR( post_rdma(test_type, size, i % batch, signaled) );
			pending++;
			i++;
		}
		do {
			n = ibv_poll_cq(cq, 16, wc);
			if (n < 0) {
				fprintf(stderr, "poll CQ failed %d\n", n);
				return;
			} else {
				check_completions(wc, n);
				pending -= n * batch;
				completed += n * batch;
			}
		} while (n > 0);
	}
	t2 = when();

	printf("%10d (x %4d) %10.2lf us %12.2lf MB/s\n", size, iters,
	       (t2 - t1), (long)size * iters / (t2 - t1));

	return;

err_out:
	printf("%10d aborted due to fail to post read request\n", size);
}

void run_send_test(int size, int iters, int batch, useconds_t delay)
{
	int i, completed, pending;
	int n;
	double t1, t2;
	struct ibv_wc wc[16];
	int signaled;

	if (delay)
		usleep(delay);

	t1 = when();
	for (i = completed = pending = 0; i < iters || completed < iters;) {
		while (i < iters && pending < TX_DEPTH) {
			signaled = (i % batch) == batch -1 || i == iters -1;
			CHECK_ERROR( post_send(size, i % batch, signaled) );
			pending++;
			i++;
		}
		do {
			n = ibv_poll_cq(cq, 16, wc);
			if (n < 0) {
				fprintf(stderr, "poll CQ failed %d\n", n);
				return;
			} else {
				check_completions(wc, n);
				pending -= n * batch;
				completed += n * batch;
			}
		} while (n > 0);
	}
	t2 = when();

	printf("%10d (x %4d) %10.2lf us %12.2lf MB/s\n", size, iters,
	       (t2 - t1), (long)size * iters / (t2 - t1));

	return;

err_out:
	printf("%10d aborted due to fail to post send request\n", size);
}

void run_recv_test(int size, int iters, int batch, useconds_t delay)
{
	int i, completed, pending;
	int n;
	double t1, t2;
	struct ibv_wc wc[16];
	int signaled;

	if (delay)
		usleep(delay);

	t1 = when();
	for (i = completed = pending = 0; i < iters || completed < iters;) {
		while (i < iters && pending < RX_DEPTH) {
			CHECK_ERROR( post_recv(size, i % batch) );
			pending++;
			i++;
		}
		do {
			n = ibv_poll_cq(cq, 16, wc);
			if (n < 0) {
				fprintf(stderr, "poll CQ failed %d\n", n);
				return;
			} else {
				check_completions(wc, n);
				pending -= n;
				completed += n;
			}
		} while (n > 0);
	}
	t2 = when();

/*
	printf("%10d (x %4d) %10.2lf us %12.2lf MB/s\n", size, iters,
	       (t2 - t1), (long)size * iters / (t2 - t1));
*/

	return;

err_out:
	printf("%10d aborted due to fail to post recv request\n", size);
}

static void usage(char *prog)
{
	printf("Usage: %s [-t read|wite|send][-s][-i][-n <iters>][-D <delay][-g <gid_idx>][-m <mtu_size>][-b][-h] server_name\n", prog);
	printf("Optins:\n");
	printf("\t-t <test_type>   Type of test to perform, can be 'read', 'write', or 'send', default: read\n");
	printf("\t-s               Sync with send/recv at the end\n");
	printf("\t-i               Use inline send\n");
	printf("\t-b               Run bidirectional test (write and read only)\n");
	printf("\t-n <iters>       Set number of iterations per message size\n");
	printf("\t-D <delay>       Microseconds to delay before posting the first recv (or send), format: <recv_delay>[,<send_delay>]\n");
	printf("\t-g <gid_idx>     Set GID index to use, needed for RoCE, default is use lid\n");
	printf("\t-m <mtu_size>    Set MTU size (512, 1024, 2048, 4096), default is auto detect\n");
	printf("\t-h               Print this message\n");
}

int main(int argc, char *argv[])
{
	char *server_name = NULL;
	unsigned int port = 12345;
	int test_type = READ;
	int iters = 1000;
	int batch = 16;
	int bidir = 0;
	useconds_t send_delay = 0;
	useconds_t recv_delay = 0;
	int sockfd;
	int size;
	int c;
	char *s;

	while ((c = getopt(argc, argv, "t:sin:D:g:m:bh")) != -1) {
		switch (c) {
		case 't':
			if (strcasecmp(optarg, "read") == 0)
				test_type = READ;
			else if (strcasecmp(optarg, "write") == 0)
				test_type = WRITE;
			else if (strcasecmp(optarg, "send") == 0)
				test_type = SEND;
			break;
		case 's':
			use_sync_ib = 1;
			break;
		case 'i':
			use_inline_send = 1;
			break;
		case 'n':
			iters = atoi(optarg);
			break;
		case 'D':
			recv_delay = atoi(optarg);
			s = strchr(optarg, ',');
			if (s)
				send_delay = atoi(s + 1);
			break;
		case 'g':
			gid_idx = atoi(optarg);
			break;
		case 'm':
			mtu = size_to_mtu(atoi(optarg));
			break;
		case 'b':
			bidir = 1;
			break;
		default:
			usage(argv[0]);
			exit(-1);
			break;
		}
	}

	if (argc > optind)
		server_name = strdup(argv[optind]);

	sockfd = connect_tcp(server_name, port);
	if (sockfd < 0) {
		fprintf(stderr, "Cannot create socket connection\n");
		exit(-1);
	}

	init_buf(MAX_SIZE * batch);
	init_ib(sockfd);

	sync_tcp(sockfd);
	for (size = 1; size <= MAX_SIZE; size <<= 1) {
		if (test_type == SEND) {
			if (server_name)
				run_send_test(size, iters, batch, send_delay);
			else
				run_recv_test(size, iters, batch, recv_delay);
		} else {
			if (server_name || bidir)
				run_rdma_test(test_type, size, iters, batch);
		}
		sync_tcp(sockfd);
	}
	sync_tcp(sockfd);

	if (use_sync_ib)
		sync_ib(4);

	free_ib();
	free_buf();
	close(sockfd);

	return 0;
}

