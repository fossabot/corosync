#define WORKER_THREAD_COUNT 2
/*
 * Copyright (c) 2003-2005 MontaVista Software, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@mvista.com)
 *
 * This software licensed under BSD license, the text of which follows:
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The first version of this code was based upon Yair Amir's PhD thesis:
 *	http://www.cs.jhu.edu/~yairamir/phd.ps) (ch4,5). 
 *
 * The current version of totemsrp implements the Totem protocol specified in:
 * 	http://citeseer.ist.psu.edu/amir95totem.html
 *
 * The deviations from the above published protocols are:
 * - encryption of message contents with SOBER128
 * - authentication of meessage contents with SHA1/HMAC
 * - token hold mode where token doesn't rotate on unused ring - reduces cpu
 *   usage on 1.6ghz xeon from 35% to less then .1 % as measured by top
 */

#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>
#include <sys/sysinfo.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <sys/poll.h>

#include "aispoll.h"
#include "totemsrp.h"
#include "../include/queue.h"
#include "../include/sq.h"
#include "../include/list.h"
#include "hdb.h"
#include "swab.h"

#include "crypto.h"
#define AUTHENTICATION 1 /* use authentication */
#define ENCRYPTION 1	 /* use encryption */

#define LOCALHOST_IP					inet_addr("127.0.0.1")
#define QUEUE_RTR_ITEMS_SIZE_MAX		2000 /* allow 512 retransmit items */
#define NEW_MESSAGE_QUEUE_SIZE_MAX		2000 /* allow 500 messages to be queued */
#define RETRANS_MESSAGE_QUEUE_SIZE_MAX	2000 /* allow 500 messages to be queued */
#define RECEIVED_MESSAGE_QUEUE_SIZE_MAX	2000 /* allow 500 messages to be queued */
#define MAXIOVS							5	
#define RETRANSMIT_ENTRIES_MAX			30
#define MISSING_MCAST_WINDOW			128
#define TIMEOUT_STATE_GATHER_JOIN		100
#define TIMEOUT_STATE_GATHER_CONSENSUS	200
#define TOKEN_RETRANSMITS_BEFORE_LOSS	4
#define TIMEOUT_TOKEN					200
#define TIMEOUT_TOKEN_RETRANSMIT		(int)(TIMEOUT_TOKEN / (TOKEN_RETRANSMITS_BEFORE_LOSS + 0.2))
#define TIMEOUT_TOKEN_HOLD				(int)(TIMEOUT_TOKEN_RETRANSMIT * 0.8 - (1000/HZ))
#define TIMEOUT_MERGE_DETECT			200
#define PACKET_SIZE_MAX					2000
#define FAIL_TO_RECV_CONST				250
#define SEQNO_UNCHANGED_CONST			20
#define TIMEOUT_DOWNCHECK           	1000

/*
 * we compare incoming messages to determine if their endian is
 * different - if so convert them
 *
 * do not change
 */
#define ENDIAN_LOCAL					 0xff22

enum message_type {
	MESSAGE_TYPE_ORF_TOKEN = 0,			/* Ordering, Reliability, Flow (ORF) control Token */
	MESSAGE_TYPE_MCAST = 1,				/* ring ordered multicast message */
	MESSAGE_TYPE_MEMB_MERGE_DETECT = 2,	/* merge rings if there are available rings */
	MESSAGE_TYPE_MEMB_JOIN = 3, 		/* membership join message */
	MESSAGE_TYPE_MEMB_COMMIT_TOKEN = 4,	/* membership commit token */
	MESSAGE_TYPE_TOKEN_HOLD_CANCEL = 5,	/* cancel the holding of the token */
};

/* 
 * New membership algorithm local variables
 */
struct consensus_list_item {
	struct in_addr addr;
	int set;
};


struct token_callback_instance {
	struct list_head list;
	int (*callback_fn) (enum totem_callback_token_type type, void *);
	enum totem_callback_token_type callback_type;
	int delete;
	void *data;
};


struct totemsrp_socket {
	int mcast;
	int token;
};

#define HMAC_HASH_SIZE 20
struct security_header {
	unsigned char hash_digest[HMAC_HASH_SIZE]; /* The hash *MUST* be first in the data structure */
	unsigned char salt[16]; /* random number */
} __attribute__((packed));

struct message_header {
	struct security_header security_header;
	char type;
	char encapsulated;
//	unsigned short filler;
	unsigned short endian_detector;
} __attribute__((packed));

struct mcast {
	struct message_header header;
	int seq;
	int this_seqno;
	struct memb_ring_id ring_id;
	struct in_addr source;
	int guarantee;
} __attribute__((packed));

/*
 * MTU - multicast message header - IP header - UDP header
 *
 * On lossy switches, making use of the DF UDP flag can lead to loss of
 * forward progress.  So the packets must be fragmented by a higher layer
 *
 * This layer can only handle packets of MTU size.
 */
#define FRAGMENT_SIZE (PACKET_SIZE_MAX - sizeof (struct mcast) - 20 - 8)

struct rtr_item  {
	struct memb_ring_id ring_id;
	int seq;
}__attribute__((packed));

struct orf_token {
	struct message_header header;
	int seq;
	int token_seq;
	int aru;
	struct in_addr aru_addr;
	struct memb_ring_id ring_id; 
	short int fcc;
	int retrans_flg;
	int rtr_list_entries;
	struct rtr_item rtr_list[0];
}__attribute__((packed));

struct memb_join {
	struct message_header header;
	struct in_addr proc_list[PROCESSOR_COUNT_MAX];
	int proc_list_entries;
	struct in_addr failed_list[PROCESSOR_COUNT_MAX];
	int failed_list_entries;
	unsigned long long ring_seq;
} __attribute__((packed));

struct memb_merge_detect {
	struct message_header header;
	struct memb_ring_id ring_id;
} __attribute__((packed));

struct token_hold_cancel {
	struct message_header header;
	struct memb_ring_id ring_id;
} __attribute__((packed));

struct memb_commit_token_memb_entry {
	struct memb_ring_id ring_id;
	int aru;
	int high_delivered;
	int received_flg;
}__attribute__((packed));

struct memb_commit_token {
	struct message_header header;
	int token_seq;
	struct memb_ring_id ring_id;
	unsigned int retrans_flg;
	int memb_index;
	int addr_entries;
	struct in_addr addr[PROCESSOR_COUNT_MAX];
	struct memb_commit_token_memb_entry memb_list[PROCESSOR_COUNT_MAX];
}__attribute__((packed));

struct worker_thread_group {
	int threadcount;
	int last_scheduled;
	struct worker_thread *threads;
	void (*worker_fn) (void *thread_state, void *work_item);
};

struct thread_data {
	void *thread_state;
	void *data;
};

struct worker_thread {
	struct worker_thread_group *worker_thread_group;
	pthread_mutex_t new_work_mutex;
	pthread_cond_t new_work_cond;
	pthread_cond_t cond;
	pthread_mutex_t done_work_mutex;
	pthread_cond_t done_work_cond;
	pthread_t thread_id;
	struct queue queue;
	void *thread_state;
	struct thread_data thread_data;
};

struct mcast_worker_fn_work_item {
	struct sort_queue_item *sort_queue_item;
	struct totemsrp_instance *instance;
};

struct message_item {
	struct mcast *mcast;
	struct iovec iovec[MAXIOVS];
	int iov_len;
};

struct sort_queue_item {
	struct iovec iovec[MAXIOVS];
	int iov_len;
};

struct orf_token_mcast_thread_state {
	unsigned char iobuf[9000];
	prng_state prng_state;
};

enum memb_state {
	MEMB_STATE_OPERATIONAL = 1,
	MEMB_STATE_GATHER = 2,
	MEMB_STATE_COMMIT = 3,
	MEMB_STATE_RECOVERY = 4
};

struct totemsrp_instance {
	/*
	 * Authentication of messages
	 */
	hmac_state totemsrp_hmac_state;
	prng_state totemsrp_prng_state;

	unsigned char totemsrp_private_key[1024];
	unsigned int totemsrp_private_key_len;

	int stats_sent;
	int stats_recv;
	int stats_delv;
	int stats_remcasts;
	int stats_orf_token;
	struct timeval stats_tv_start;

	/*
	 * Flow control mcasts and remcasts on last and current orf_token
	 */
	int fcc_remcast_last;
	int fcc_mcast_last;
	int fcc_mcast_current;
	int fcc_remcast_current;
	struct consensus_list_item consensus_list[PROCESSOR_COUNT_MAX];

	int consensus_list_entries;

	struct in_addr my_proc_list[PROCESSOR_COUNT_MAX];

	struct in_addr my_failed_list[PROCESSOR_COUNT_MAX];

	struct in_addr my_new_memb_list[PROCESSOR_COUNT_MAX];

	struct in_addr my_trans_memb_list[PROCESSOR_COUNT_MAX];

	struct in_addr my_memb_list[PROCESSOR_COUNT_MAX];

	struct in_addr my_deliver_memb_list[PROCESSOR_COUNT_MAX];

	int my_proc_list_entries;

	int my_failed_list_entries;

	int my_new_memb_entries;

	int my_trans_memb_entries;

	int my_memb_entries;

	int my_deliver_memb_entries;

	struct memb_ring_id my_ring_id;

	struct memb_ring_id my_old_ring_id;

	int my_aru_count;

	int my_merge_detect_timeout_outstanding;

	int my_last_aru;

	int my_seq_unchanged;

	int my_received_flg;

	int my_high_seq_received;

	int my_install_seq;

	int my_rotation_counter;

	int my_set_retrans_flg;

	int my_retrans_flg_count;

	unsigned int my_high_ring_delivered;

	unsigned int timeout_token;

	unsigned int timeout_token_retransmit;

	unsigned int timeout_token_hold;

	unsigned int token_retransmits_before_loss;

	unsigned int timeout_state_gather_join;

	unsigned int timeout_state_gather_consensus;

	unsigned int timeout_merge_detect;

	unsigned int timeout_downcheck;

	unsigned int fail_to_recv_const;

	/*
	 * Queues used to order, deliver, and recover messages
	 */
	struct queue new_message_queue;

	struct queue retrans_message_queue;

	struct sq regular_sort_queue;

	struct sq recovery_sort_queue;

	/*
	 * File descriptors in use by TOTEMSRP
	 */
	struct totemsrp_socket totemsrp_sockets[2];

	/*
	 * Received up to and including
	 */
	int my_aru;

	int my_high_delivered;

	struct list_head token_callback_received_listhead;

	struct list_head token_callback_sent_listhead;

	char orf_token_retransmit[15000]; // sizeof (struct orf_token) + sizeof (struct rtr_item) * RETRANSMIT_ENTRIES_MAX];

	int orf_token_retransmit_size;

	int my_token_seq;

	/*
	 * Timers
	 */
	poll_timer_handle timer_orf_token_timeout;

	poll_timer_handle timer_orf_token_retransmit_timeout;

	poll_timer_handle timer_orf_token_hold_retransmit_timeout;

	poll_timer_handle timer_merge_detect_timeout;

	poll_timer_handle memb_timer_state_gather_join_timeout;

	poll_timer_handle memb_timer_state_gather_consensus_timeout;

	poll_timer_handle memb_timer_state_commit_timeout;

	poll_timer_handle timer_netif_check_timeout;

	/*
	 * Function and data used to log messages
	 */
	int totemsrp_log_level_security;

	int totemsrp_log_level_error;

	int totemsrp_log_level_warning;

	int totemsrp_log_level_notice;

	int totemsrp_log_level_debug;

	void (*totemsrp_log_printf) (int level, char *format, ...);

	enum memb_state memb_state;

	struct sockaddr_in my_id;

	struct sockaddr_in next_memb;

	struct sockaddr_in memb_local_sockaddr_in;

	char iov_buffer[15000]; //PACKET_SIZE_MAX];

	struct iovec totemsrp_iov_recv;

	poll_handle *totemsrp_poll_handle;

	struct totem_interface *totemsrp_interfaces;

	int totemsrp_interface_count;

	int netif_state_report;

	int netif_bind_state;

	struct worker_thread_group worker_thread_group;

	struct worker_thread_group worker_thread_group_orf_token_mcast;

	/*
	 * Function called when new message received
	 */
	int (*totemsrp_recv) (char *group, struct iovec *iovec, int iov_len);

	/*
	 * Multicast address
	 */
	struct sockaddr_in sockaddr_in_mcast;

	void (*totemsrp_deliver_fn) (
		struct in_addr source_addr,
		struct iovec *iovec,
		int iov_len,
		int endian_conversion_required);

	void (*totemsrp_confchg_fn) (
		enum totem_configuration_type configuration_type,
		struct in_addr *member_list, int member_list_entries,
		struct in_addr *left_list, int left_list_entries,
		struct in_addr *joined_list, int joined_list_entries,
		struct memb_ring_id *ring_id);

	char iov_encrypted_buffer[15000];

	struct iovec iov_encrypted;

	int global_seqno;

	int my_token_held;

	unsigned long long token_ring_id_seq;

	int log_digest;

	int last_released;

	int set_aru;

	int totemsrp_brake;	

	int old_ring_state_saved;

	int old_ring_state_aru;

	int old_ring_state_high_seq_received;

	int ring_saved;

	int my_last_seq;

	struct timeval tv_old;

	int firstrun;
};

struct message_handlers {
	int count;
	int (*handler_functions[6]) (struct totemsrp_instance *,
		struct sockaddr_in *, struct iovec *, int, int, int);
};

/*
 * forward decls
 */
static int message_handler_orf_token (struct totemsrp_instance *, struct sockaddr_in *, struct iovec *, int, int, int);

static int message_handler_mcast (struct totemsrp_instance *, struct sockaddr_in *, struct iovec *, int, int, int);

static int message_handler_memb_merge_detect (struct totemsrp_instance *, struct sockaddr_in *, struct iovec *, int, int, int);

static int message_handler_memb_join (struct totemsrp_instance *, struct sockaddr_in *, struct iovec *, int, int, int);

static int message_handler_memb_commit_token (struct totemsrp_instance *, struct sockaddr_in *, struct iovec *, int, int, int);

static int message_handler_token_hold_cancel (struct totemsrp_instance *, struct sockaddr_in *, struct iovec *, int, int, int);

static void memb_ring_id_create_or_load (struct totemsrp_instance *, struct memb_ring_id *);
static int recv_handler (poll_handle handle, int fd, int revents, void *data, unsigned int *prio);
static int netif_determine (struct totemsrp_instance *instance, struct sockaddr_in *bindnet, struct sockaddr_in *bound_to,int *interface_up);
static int loopback_determine (struct sockaddr_in *bound_to);
static void netif_down_check (struct totemsrp_instance *instance);

static void token_callbacks_execute (struct totemsrp_instance *instance, enum totem_callback_token_type type);


#define NETIF_STATE_REPORT_UP		1	
#define NETIF_STATE_REPORT_DOWN		2

#define BIND_STATE_UNBOUND	0
#define BIND_STATE_REGULAR	1
#define BIND_STATE_LOOPBACK	2

static int totemsrp_build_sockets (
	struct totemsrp_instance *instance,
	struct sockaddr_in *sockaddr_mcast,
	struct sockaddr_in *sockaddr_bindnet,
	struct totemsrp_socket *sockets,
	struct sockaddr_in *bound_to,
	int *interface_up);

static int totemsrp_build_sockets_loopback (
	struct totemsrp_instance *instance,
	struct sockaddr_in *sockaddr_mcast,
	struct sockaddr_in *sockaddr_bindnet,
	struct totemsrp_socket *sockets,
	struct sockaddr_in *bound_to);

static void memb_state_gather_enter (struct totemsrp_instance *instance);
static void messages_deliver_to_app (struct totemsrp_instance *instance, int skip, int end_point);
static int orf_token_mcast (struct totemsrp_instance *instance, struct orf_token *oken,
	int fcc_mcasts_allowed, struct sockaddr_in *system_from);
static int messages_free (struct totemsrp_instance *instance, int token_aru);

static void encrypt_and_sign (struct totemsrp_instance *instance, struct iovec *iovec, int iov_len);
static int authenticate_and_decrypt (struct totemsrp_instance *instance, struct iovec *iov);
static int recv_handler (poll_handle handle, int fd, int revents, void *data, unsigned int *prio);
static void memb_ring_id_store (struct totemsrp_instance *instance, struct memb_commit_token *commit_token);
static void memb_state_commit_token_update (struct totemsrp_instance *instance, struct memb_commit_token *memb_commit_token);
static int memb_state_commit_token_send (struct totemsrp_instance *instance, struct memb_commit_token *memb_commit_token);
static void memb_state_commit_token_create (struct totemsrp_instance *instance, struct memb_commit_token *commit_token);
static int token_hold_cancel_send (struct totemsrp_instance *instance);
static void orf_token_endian_convert (struct orf_token *in, struct orf_token *out);
static void memb_commit_token_endian_convert (struct memb_commit_token *in, struct memb_commit_token *out);
static void memb_join_endian_convert (struct memb_join *in, struct memb_join *out);
static void mcast_endian_convert (struct mcast *in, struct mcast *out);
static void timer_function_orf_token_timeout (void *data);
static void timer_function_token_retransmit_timeout (void *data);
static void timer_function_token_hold_retransmit_timeout (void *data);
static void timer_function_merge_detect_timeout (void *data);


/*
 * All instances in one database
 */
static struct saHandleDatabase totemsrp_instance_database = {
	.handleCount				= 0,
	.handles					= 0,
	.handleInstanceDestructor	= 0
};
struct message_handlers totemsrp_message_handlers = {
	6,
	{
		message_handler_orf_token,
		message_handler_mcast,
		message_handler_memb_merge_detect,
		message_handler_memb_join,
		message_handler_memb_commit_token,
		message_handler_token_hold_cancel
	}
};

void instance_initialize (struct totemsrp_instance *instance)
{
	memset (instance, 0, sizeof (struct totemsrp_instance));

	list_init (&instance->token_callback_received_listhead);

	list_init (&instance->token_callback_sent_listhead);

	instance->my_received_flg = 1;

	instance->timeout_token = TIMEOUT_TOKEN;

	instance->timeout_token_retransmit = TIMEOUT_TOKEN_RETRANSMIT;


	instance->token_retransmits_before_loss = TOKEN_RETRANSMITS_BEFORE_LOSS;

	instance->timeout_state_gather_join = TIMEOUT_STATE_GATHER_JOIN;

	instance->timeout_state_gather_consensus = TIMEOUT_STATE_GATHER_CONSENSUS;

	instance->timeout_merge_detect = TIMEOUT_MERGE_DETECT;

	instance->timeout_downcheck = TIMEOUT_DOWNCHECK;

	instance->fail_to_recv_const = FAIL_TO_RECV_CONST;

	instance->my_token_seq = -1;

	instance->memb_state = MEMB_STATE_OPERATIONAL;

	instance->netif_state_report = NETIF_STATE_REPORT_UP | NETIF_STATE_REPORT_DOWN;

	instance->totemsrp_iov_recv.iov_base = instance->iov_buffer;

	instance->totemsrp_iov_recv.iov_len = sizeof (instance->iov_buffer);

	instance->iov_encrypted.iov_base = instance->iov_encrypted_buffer;

	instance->iov_encrypted.iov_len = sizeof (instance->iov_encrypted_buffer);

	instance->set_aru = -1;
}

#ifdef CODE_COVERAGE_COMPILE_OUT
void print_digest (char *where, unsigned char *digest)
{
	int i;

	printf ("DIGEST %s:\n", where);
	for (i = 0; i < 16; i++) {
		printf ("%x ", digest[i]);
	}
	printf ("\n");
}

void print_msg (unsigned char *msg, int size)
{
	int i;
	printf ("MSG CONTENTS START\n");
	for (i = 0; i < size; i++) {
		printf ("%x ", msg[i]);
		if ((i % 16) == 15) {
			printf ("\n");
		}
	}
	printf ("MSG CONTENTS DONE\n");
}
#endif

static void orf_token_mcast_worker_fn (void *thread_state, void *work_item);

static void *worker_thread (void *thread_data_in) {
	struct thread_data *thread_data = (struct thread_data *)thread_data_in;
	struct orf_token_mcast_thread_state *orf_token_mcast_thread_state =
		(struct orf_token_mcast_thread_state *)thread_data->thread_state;
	struct worker_thread *worker_thread =
		(struct worker_thread *)thread_data->data;
	void *data_for_worker_fn;

	for (;;) {
		pthread_mutex_lock (&worker_thread->new_work_mutex);
		if (queue_is_empty (&worker_thread->queue) == 1) {
		pthread_cond_wait (&worker_thread->new_work_cond,
			&worker_thread->new_work_mutex);
		}

		data_for_worker_fn = queue_item_get (&worker_thread->queue);
		worker_thread->worker_thread_group->worker_fn (orf_token_mcast_thread_state, data_for_worker_fn);
		queue_item_remove (&worker_thread->queue);
		pthread_mutex_unlock (&worker_thread->new_work_mutex);
		pthread_mutex_lock (&worker_thread->done_work_mutex);
		if (queue_is_empty (&worker_thread->queue) == 1) {
			pthread_cond_signal (&worker_thread->done_work_cond);
		}
		pthread_mutex_unlock (&worker_thread->done_work_mutex);
	}
	return (0);
}

static int worker_thread_group_init (
	struct worker_thread_group *worker_thread_group,
	int threads,
	int items_max,
	int item_size,
	int thread_state_size,
	void (*thread_state_constructor)(void *),
	void (*worker_fn)(void *thread_state, void *work_item))
{
	int i;

	worker_thread_group->threadcount = threads;
	worker_thread_group->last_scheduled = 0;
	worker_thread_group->worker_fn = worker_fn;
	worker_thread_group->threads = malloc (sizeof (struct worker_thread) *
		threads);
	if (worker_thread_group->threads == 0) {
		return (-1);
	}

	for (i = 0; i < threads; i++) {
		worker_thread_group->threads[i].thread_state = malloc (thread_state_size);
		thread_state_constructor (worker_thread_group->threads[i].thread_state);
		worker_thread_group->threads[i].worker_thread_group = worker_thread_group;
		pthread_mutex_init (&worker_thread_group->threads[i].new_work_mutex, NULL);
		pthread_cond_init (&worker_thread_group->threads[i].new_work_cond, NULL);
		pthread_mutex_init (&worker_thread_group->threads[i].done_work_mutex, NULL);
		pthread_cond_init (&worker_thread_group->threads[i].done_work_cond, NULL);
		queue_init (&worker_thread_group->threads[i].queue, items_max,
			item_size);

		worker_thread_group->threads[i].thread_data.thread_state =
			worker_thread_group->threads[i].thread_state;
		worker_thread_group->threads[i].thread_data.data = &worker_thread_group->threads[i];
		pthread_create (&worker_thread_group->threads[i].thread_id,
			NULL, worker_thread, &worker_thread_group->threads[i].thread_data);
	}
	return (0);
}

static void worker_thread_group_exit (
	struct worker_thread_group *worker_thread_group)
{
	int i;

	for (i = 0; i < worker_thread_group->threadcount; i++) {
		pthread_cancel (worker_thread_group->threads[i].thread_id);
	}
}

static void worker_thread_group_work_add (
	struct worker_thread_group *worker_thread_group,
	void *item)
{
	int schedule;

	schedule = (worker_thread_group->last_scheduled + 1) % (worker_thread_group->threadcount);
	worker_thread_group->last_scheduled = schedule;

	pthread_mutex_lock (&worker_thread_group->threads[schedule].new_work_mutex);
	queue_item_add (&worker_thread_group->threads[schedule].queue, item);
	pthread_cond_signal (&worker_thread_group->threads[schedule].new_work_cond);
	pthread_mutex_unlock (&worker_thread_group->threads[schedule].new_work_mutex);
}

static void worker_thread_group_wait (
	struct worker_thread_group *worker_thread_group)
{
	int i;

	for (i = 0; i < worker_thread_group->threadcount; i++) {
		pthread_mutex_lock (&worker_thread_group->threads[i].done_work_mutex);
		if (queue_is_empty (&worker_thread_group->threads[i].queue) == 0) {
			pthread_cond_wait (&worker_thread_group->threads[i].done_work_cond,
				&worker_thread_group->threads[i].done_work_mutex);
		}
		pthread_mutex_unlock (&worker_thread_group->threads[i].done_work_mutex);
	}
}

static void orf_token_mcast_thread_state_constructor (
	void *orf_token_mcast_thread_state_in)
{
	struct orf_token_mcast_thread_state *orf_token_mcast_thread_state =
		(struct orf_token_mcast_thread_state *)orf_token_mcast_thread_state_in;
	memset (orf_token_mcast_thread_state, 0,
		sizeof (orf_token_mcast_thread_state));

	rng_make_prng (128, PRNG_SOBER,
		&orf_token_mcast_thread_state->prng_state, NULL);
}

/*
 * Exported interfaces
 */
int totemsrp_initialize (
	poll_handle *poll_handle,
	totemsrp_handle *handle,
	struct totem_config *totem_config,

	void (*deliver_fn) (
		struct in_addr source_addr,
		struct iovec *iovec,
		int iov_len,
		int endian_conversion_required),
	void (*confchg_fn) (
		enum totem_configuration_type configuration_type,
		struct in_addr *member_list, int member_list_entries,
		struct in_addr *left_list, int left_list_entries,
		struct in_addr *joined_list, int joined_list_entries,
		struct memb_ring_id *ring_id))
{
	unsigned int *timeouts = totem_config->timeouts;
	struct totemsrp_instance *instance;
	SaErrorT error;
	int i;

	error = saHandleCreate (&totemsrp_instance_database,
		sizeof (struct totemsrp_instance), handle);
	if (error != SA_OK) {
		goto error_exit;
	}
	error = saHandleInstanceGet (&totemsrp_instance_database, *handle,
		(void *)&instance);
	if (error != SA_OK) {
		goto error_destroy;
	}

	instance_initialize (instance);

	/*
	 * Configure logging
	 */
	instance->totemsrp_log_level_security = totem_config->totem_logging_configuration.log_level_security;
	instance->totemsrp_log_level_error = totem_config->totem_logging_configuration.log_level_error;
	instance->totemsrp_log_level_warning = totem_config->totem_logging_configuration.log_level_warning;
	instance->totemsrp_log_level_notice = totem_config->totem_logging_configuration.log_level_notice;
	instance->totemsrp_log_level_debug = totem_config->totem_logging_configuration.log_level_debug;
	instance->totemsrp_log_printf = totem_config->totem_logging_configuration.log_printf;

	instance->timeout_token_hold = (int)(instance->timeout_token_retransmit * 0.8 - (1000/HZ));

	/*
	 * Initialize random number generator for later use to generate salt
	 */
	memcpy (instance->totemsrp_private_key, totem_config->private_key,
		totem_config->private_key_len);

	instance->totemsrp_private_key_len = totem_config->private_key_len;

	rng_make_prng (128, PRNG_SOBER, &instance->totemsrp_prng_state, NULL);

	/*
	 * Initialize local variables for totemsrp
	 */
	memcpy (&instance->sockaddr_in_mcast, &totem_config->mcast_addr, 
			sizeof (struct sockaddr_in));
	memset (&instance->next_memb, 0, sizeof (struct sockaddr_in));
	memset (instance->iov_buffer, 0, PACKET_SIZE_MAX);

	/*
	 * Initialize thread group data structure
	 */
	worker_thread_group_init (&instance->worker_thread_group_orf_token_mcast,
		WORKER_THREAD_COUNT, 128, sizeof (struct mcast_worker_fn_work_item),
		sizeof (struct orf_token_mcast_thread_state),
		orf_token_mcast_thread_state_constructor,
		orf_token_mcast_worker_fn);

	/*
	 * Update our timeout values if they were specified in the openais.conf
	 * file.
	 */
	for (i = 0; i < MAX_TOTEM_TIMEOUTS; i++) {
		if (!timeouts[i]) {
			continue;
		}
		switch (i) {
		case TOTEM_TOKEN:
			instance->timeout_token = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Overriding token timeout to (%u ms)\n", timeouts[i]);
			instance->timeout_token_retransmit = (int)(instance->timeout_token / (instance->token_retransmits_before_loss + 0.2));
			instance->timeout_token_hold = (int)(instance->timeout_token_retransmit * 0.8 - (1000/HZ));
			break;
		case TOTEM_RETRANSMIT_TOKEN:
			instance->timeout_token_retransmit = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Overriding token retransmit timeout to (%u ms)\n", timeouts[i]);
			break;
		case TOTEM_RETRANSMITS_BEFORE_LOSS:
			instance->token_retransmits_before_loss = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Overriding retransmits before loss (%u retrans)\n", timeouts[i]);
			break;
		case TOTEM_HOLD_TOKEN:
			instance->timeout_token_hold = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Overriding token hold timeout to (%u ms)\n", timeouts[i]);
			break;
		case TOTEM_JOIN:
			instance->timeout_state_gather_join = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Join Timeout set to %u ms\n", timeouts[i]);
			break;
		case TOTEM_CONSENSUS:
			instance->timeout_state_gather_consensus = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Consensus Timeout set to %u ms\n", timeouts[i]);
			break;
		case TOTEM_MERGE:
			instance->timeout_merge_detect = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Merge Detect Timeout set to %u ms\n", timeouts[i]);
			break;
		case TOTEM_DOWNCHECK:
			instance->timeout_downcheck = timeouts[i];
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Downcheck Timeout set to %u ms\n", timeouts[i]);
			break;
		case TOTEM_FAIL_RECV_CONST:
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Failed To Receive Const set to %u\n", timeouts[i]);
			instance->fail_to_recv_const = timeouts[i];
			break;
		default:
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
					"Received unknown timeout type: %d\n", timeouts[i]);
			break;
		}
	}

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"Token Timeout (%d ms) retransmit timeout (%d ms)\n",
		instance->timeout_token, instance->timeout_token_retransmit);
	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"token hold (%d ms) retransmits before loss (%d retrans)\n",
		instance->timeout_token_hold, instance->token_retransmits_before_loss);

	queue_init (&instance->new_message_queue, NEW_MESSAGE_QUEUE_SIZE_MAX,
		sizeof (struct message_item));

	queue_init (&instance->retrans_message_queue, RETRANS_MESSAGE_QUEUE_SIZE_MAX,
		sizeof (struct message_item));

	sq_init (&instance->regular_sort_queue,
		QUEUE_RTR_ITEMS_SIZE_MAX, sizeof (struct sort_queue_item), 0);

	sq_init (&instance->recovery_sort_queue,
		QUEUE_RTR_ITEMS_SIZE_MAX, sizeof (struct sort_queue_item), 0);

	instance->totemsrp_interfaces = totem_config->interfaces;
	instance->totemsrp_interface_count = 1;
	instance->totemsrp_poll_handle = poll_handle;

	netif_down_check (instance);

	memb_state_gather_enter (instance);

	instance->totemsrp_deliver_fn = deliver_fn;

	instance->totemsrp_confchg_fn = confchg_fn;

	return (0);

error_destroy:
	saHandleDestroy (&totemsrp_instance_database, *handle);

error_exit:
	return (-1);
}

void totemsrp_finalize (
	totemsrp_handle handle)
{
	struct totemsrp_instance *instance;
	SaErrorT error;

	error = saHandleInstanceGet (&totemsrp_instance_database, handle,
		(void *)&instance);
	if (error != SA_OK) {
		return;
	}

	worker_thread_group_exit (&instance->worker_thread_group_orf_token_mcast);

	saHandleInstancePut (&totemsrp_instance_database, handle);
}


/*
 * Set operations for use by the membership algorithm
 */
static void memb_consensus_reset (struct totemsrp_instance *instance)
{
	instance->consensus_list_entries = 0;
}

static void memb_set_subtract (
	struct in_addr *out_list, int *out_list_entries,
	struct in_addr *one_list, int one_list_entries,
	struct in_addr *two_list, int two_list_entries)
{
	int found = 0;
	int i;
	int j;

	*out_list_entries = 0;

	for (i = 0; i < one_list_entries; i++) {
		for (j = 0; j < two_list_entries; j++) {
			if (one_list[i].s_addr == two_list[j].s_addr) {
				found = 1;
				break;
			}
		}
		if (found == 0) {
			out_list[*out_list_entries].s_addr = one_list[i].s_addr;
			*out_list_entries = *out_list_entries + 1;
		}
		found = 0;
	}
}

/*
 * Set consensus for a specific processor
 */
static void memb_consensus_set (
	struct totemsrp_instance *instance,
	struct in_addr *addr)
{
	int found = 0;
	int i;

	for (i = 0; i < instance->consensus_list_entries; i++) {
		if (addr->s_addr == instance->consensus_list[i].addr.s_addr) {
			found = 1;
			break; /* found entry */
		}
	}
	instance->consensus_list[i].addr.s_addr = addr->s_addr;
	instance->consensus_list[i].set = 1;
	if (found == 0) {
		instance->consensus_list_entries++;
	}
	return;
}

/*
 * Is consensus set for a specific processor
 */
static int memb_consensus_isset (
	struct totemsrp_instance *instance,
	struct in_addr *addr)
{
	int i;

	for (i = 0; i < instance->consensus_list_entries; i++) {
		if (addr->s_addr == instance->consensus_list[i].addr.s_addr) {
			return (instance->consensus_list[i].set);
		}
	}
	return (0);
}

/*
 * Is consensus agreed upon based upon consensus database
 */
static int memb_consensus_agreed (
	struct totemsrp_instance *instance)
{
	struct in_addr token_memb[PROCESSOR_COUNT_MAX];
	int token_memb_entries = 0;
	int agreed = 1;
	int i;

	memb_set_subtract (token_memb, &token_memb_entries,
		instance->my_proc_list, instance->my_proc_list_entries,
		instance->my_failed_list, instance->my_failed_list_entries);

	for (i = 0; i < token_memb_entries; i++) {
		if (memb_consensus_isset (instance, &token_memb[i]) == 0) {
			agreed = 0;
			break;
		}
	}
	assert (token_memb_entries >= 1);

	return (agreed);
}

static void memb_consensus_notset (
	struct totemsrp_instance *instance,
	struct in_addr *no_consensus_list,
	int *no_consensus_list_entries,
	struct in_addr *comparison_list,
	int comparison_list_entries)
{
	int i;

	*no_consensus_list_entries = 0;

	for (i = 0; i < instance->my_proc_list_entries; i++) {
		if (memb_consensus_isset (instance, &instance->my_proc_list[i]) == 0) {
			no_consensus_list[*no_consensus_list_entries].s_addr = instance->my_proc_list[i].s_addr;
			*no_consensus_list_entries = *no_consensus_list_entries + 1;
		}
	}
}

/*
 * Is set1 equal to set2 Entries can be in different orders
 */
static int memb_set_equal (struct in_addr *set1, int set1_entries,
	struct in_addr *set2, int set2_entries)
{
	int i;
	int j;

	int found = 0;

	if (set1_entries != set2_entries) {
		return (0);
	}
	for (i = 0; i < set2_entries; i++) {
		for (j = 0; j < set1_entries; j++) {
			if (set1[j].s_addr == set2[i].s_addr) {
				found = 1;
				break;
			}
		}
		if (found == 0) {
			return (0);
		}
		found = 0;
	}
	return (1);
}

/*
 * Is subset fully contained in fullset
 */
static int memb_set_subset (
	struct in_addr *subset, int subset_entries,
	struct in_addr *fullset, int fullset_entries)
{
	int i;
	int j;
	int found = 0;

	if (subset_entries > fullset_entries) {
		return (0);
	}
	for (i = 0; i < subset_entries; i++) {
		for (j = 0; j < fullset_entries; j++) {
			if (subset[i].s_addr == fullset[j].s_addr) {
				found = 1;
			}
		}
		if (found == 0) {
			return (0);
		}
		found = 1;
	}
	return (1);
}

/*
 * merge subset into fullset taking care not to add duplicates
 */
static void memb_set_merge (
	struct in_addr *subset, int subset_entries,
	struct in_addr *fullset, int *fullset_entries)
{
	int found = 0;
	int i;
	int j;

	for (i = 0; i < subset_entries; i++) {
		for (j = 0; j < *fullset_entries; j++) {
			if (fullset[j].s_addr == subset[i].s_addr) {
				found = 1;
				break;
			}	
		}
		if (found == 0) {
			fullset[j].s_addr = subset[i].s_addr;
			*fullset_entries = *fullset_entries + 1;
		}
		found = 0;
	}
	return;
}

static void memb_set_and (
	struct in_addr *set1, int set1_entries,
	struct in_addr *set2, int set2_entries,
	struct in_addr *and, int *and_entries)
{
	int i;
	int j;
	int found = 0;

	*and_entries = 0;

	for (i = 0; i < set2_entries; i++) {
		for (j = 0; j < set1_entries; j++) {
			if (set1[j].s_addr == set2[i].s_addr) {
				found = 1;
				break;
			}
		}
		if (found) {
			and[*and_entries].s_addr = set1[j].s_addr;
			*and_entries = *and_entries + 1;
		}
		found = 0;
	}
	return;
}

#ifdef CODE_COVERGE
static void memb_set_print (
	char *string,
	struct in_addr *list,
	int list_entries)
{
	int i;
	printf ("List '%s' contains %d entries:\n", string, list_entries);

	for (i = 0; i < list_entries; i++) {
		printf ("addr %s\n", inet_ntoa (list[i]));
	}
}
#endif

static void reset_token_retransmit_timeout (struct totemsrp_instance *instance)
{
	poll_timer_delete (*instance->totemsrp_poll_handle,
		instance->timer_orf_token_retransmit_timeout);
	poll_timer_add (*instance->totemsrp_poll_handle,
		instance->timeout_token_retransmit,
		(void *)instance,
		timer_function_token_retransmit_timeout,
		&instance->timer_orf_token_retransmit_timeout);

}

static void start_merge_detect_timeout (struct totemsrp_instance *instance)
{
	if (instance->my_merge_detect_timeout_outstanding == 0) {
		poll_timer_add (*instance->totemsrp_poll_handle,
			instance->timeout_merge_detect,
			(void *)instance,
			timer_function_merge_detect_timeout,
			&instance->timer_merge_detect_timeout);

		instance->my_merge_detect_timeout_outstanding = 1;
	}
}

static void cancel_merge_detect_timeout (struct totemsrp_instance *instance)
{
	poll_timer_delete (*instance->totemsrp_poll_handle, instance->timer_merge_detect_timeout);
	instance->my_merge_detect_timeout_outstanding = 0;
}

/*
 * ring_state_* is used to save and restore the sort queue
 * state when a recovery operation fails (and enters gather)
 */
static void old_ring_state_save (struct totemsrp_instance *instance)
{
	if (instance->old_ring_state_saved == 0) {
		instance->old_ring_state_saved = 1;
		instance->old_ring_state_aru = instance->my_aru;
		instance->old_ring_state_high_seq_received = instance->my_high_seq_received;
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"Saving state aru %d high seq recieved %d\n",
			instance->my_aru, instance->my_high_seq_received);
	}
}

static void ring_save (struct totemsrp_instance *instance)
{
	if (instance->ring_saved == 0) {
		instance->ring_saved = 1;
		memcpy (&instance->my_old_ring_id, &instance->my_ring_id,
			sizeof (struct memb_ring_id));
	}
}

static void ring_reset (struct totemsrp_instance *instance)
{
	instance->ring_saved = 0;
}

static void ring_state_restore (struct totemsrp_instance *instance)
{
	if (instance->old_ring_state_saved) {
		instance->my_ring_id.rep.s_addr = 0;
		instance->my_aru = instance->old_ring_state_aru;
		instance->my_high_seq_received = instance->old_ring_state_high_seq_received;
		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
			"Restoring instance->my_aru %d my high seq received %d\n",
			instance->my_aru, instance->my_high_seq_received);
	}
}

static void old_ring_state_reset (struct totemsrp_instance *instance)
{
	instance->old_ring_state_saved = 0;
}

static void reset_token_timeout (struct totemsrp_instance *instance) {
	poll_timer_delete (*instance->totemsrp_poll_handle, instance->timer_orf_token_timeout);
	poll_timer_add (*instance->totemsrp_poll_handle,
		instance->timeout_token,
		(void *)instance,
		timer_function_orf_token_timeout,
		&instance->timer_orf_token_timeout);
}

static void cancel_token_timeout (struct totemsrp_instance *instance) {
	poll_timer_delete (*instance->totemsrp_poll_handle, instance->timer_orf_token_timeout);
}

static void cancel_token_retransmit_timeout (struct totemsrp_instance *instance)
{
	poll_timer_delete (*instance->totemsrp_poll_handle, instance->timer_orf_token_retransmit_timeout);
}

static void start_token_hold_retransmit_timeout (struct totemsrp_instance *instance)
{
	poll_timer_add (*instance->totemsrp_poll_handle,
		instance->timeout_token_hold,
		(void *)instance,
		timer_function_token_hold_retransmit_timeout,
		&instance->timer_orf_token_hold_retransmit_timeout);
}

static void cancel_token_hold_retransmit_timeout (struct totemsrp_instance *instance)
{
	poll_timer_delete (*instance->totemsrp_poll_handle,
		instance->timer_orf_token_hold_retransmit_timeout);
}

static void memb_state_consensus_timeout_expired (
		struct totemsrp_instance *instance)
{
	struct in_addr no_consensus_list[PROCESSOR_COUNT_MAX];
	int no_consensus_list_entries;

	if (memb_consensus_agreed (instance)) {
		memb_consensus_reset (instance);

		memb_consensus_set (instance, &instance->my_id.sin_addr);

		reset_token_timeout (instance); // REVIEWED
	} else {
		memb_consensus_notset (instance, no_consensus_list,
			&no_consensus_list_entries,
			instance->my_proc_list, instance->my_proc_list_entries);

		memb_set_merge (no_consensus_list, no_consensus_list_entries,
			instance->my_failed_list, &instance->my_failed_list_entries);

		memb_state_gather_enter (instance);
	}
}

static int memb_join_message_send (struct totemsrp_instance *instance);

static int memb_merge_detect_transmit (struct totemsrp_instance *instance);

/*
 * Timers used for various states of the membership algorithm
 */
static void timer_function_orf_token_timeout (void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"The token was lost in state %d from timer %x\n", instance->memb_state, data);
	switch (instance->memb_state) {
		case MEMB_STATE_OPERATIONAL:
			netif_down_check (instance);	
			memb_state_gather_enter (instance);
			break;

		case MEMB_STATE_GATHER:
			memb_state_consensus_timeout_expired (instance);
			memb_state_gather_enter (instance);
			break;

		case MEMB_STATE_COMMIT:
			memb_state_gather_enter (instance);
			break;
		
		case MEMB_STATE_RECOVERY:
			ring_state_restore (instance);
			memb_state_gather_enter (instance);
			break;
	}
}

static void memb_timer_function_state_gather (void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;

	switch (instance->memb_state) {
	case MEMB_STATE_OPERATIONAL:
	case MEMB_STATE_RECOVERY:
		assert (0); /* this should never happen */
		break;
	case MEMB_STATE_GATHER:
	case MEMB_STATE_COMMIT:
		memb_join_message_send (instance);

		/*
		 * Restart the join timeout
		`*/
		poll_timer_delete (*instance->totemsrp_poll_handle, instance->memb_timer_state_gather_join_timeout);
	
		poll_timer_add (*instance->totemsrp_poll_handle,
			instance->timeout_state_gather_join,
			(void *)instance,
			memb_timer_function_state_gather,
			&instance->memb_timer_state_gather_join_timeout);
		break;
	}
}

static void memb_timer_function_gather_consensus_timeout (void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;
	memb_state_consensus_timeout_expired (instance);
}

static void deliver_messages_from_recovery_to_regular (struct totemsrp_instance *instance)
{
	int i;
	struct sort_queue_item *recovery_message_item;
	struct sort_queue_item regular_message_item;
	int res;
	void *ptr;
	struct mcast *mcast;

	instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
		"recovery to regular %d-%d\n", 1, instance->my_aru);

	/*
	 * Move messages from recovery to regular sort queue
	 */
// todo should i be initialized to 0 or 1 ?
	for (i = 1; i <= instance->my_aru; i++) {
		res = sq_item_get (&instance->recovery_sort_queue, i, &ptr);
		if (res != 0) {
			continue;
		}
printf ("Transferring message with seq id %d\n", i);
		recovery_message_item = (struct sort_queue_item *)ptr;

		/*
		 * Convert recovery message into regular message
		 */
		if (recovery_message_item->iov_len > 1) {
			mcast = recovery_message_item->iovec[1].iov_base;
			memcpy (&regular_message_item.iovec[0],
				&recovery_message_item->iovec[1],
				sizeof (struct iovec) * recovery_message_item->iov_len);
		} else {
			mcast = recovery_message_item->iovec[0].iov_base;
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
				"encapsulated is %d\n",
				mcast->header.encapsulated);
			if (mcast->header.encapsulated == 1) {
				/*
				 * Message is a recovery message encapsulated
				 * in a new ring message
				 */
				regular_message_item.iovec[0].iov_base =
					recovery_message_item->iovec[0].iov_base + sizeof (struct mcast);
				regular_message_item.iovec[0].iov_len =
				recovery_message_item->iovec[0].iov_len - sizeof (struct mcast);
				regular_message_item.iov_len = 1;
				mcast = regular_message_item.iovec[0].iov_base;
			} else {
printf ("not encapsulated\n");
				continue; /* TODO this case shouldn't happen */
				/*
				 * Message is originated on new ring and not
				 * encapsulated
				 */
				regular_message_item.iovec[0].iov_base =
					recovery_message_item->iovec[0].iov_base;
				regular_message_item.iovec[0].iov_len =
				recovery_message_item->iovec[0].iov_len;
			}
		}

		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
			"comparing if ring id is for this processors old ring seqno %d\n",
			 mcast->seq);

		/*
		 * Only add this message to the regular sort
		 * queue if it was originated with the same ring
		 * id as the previous ring
		 */
		if (memcmp (&instance->my_old_ring_id, &mcast->ring_id,
			sizeof (struct memb_ring_id)) == 0) {

		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"adding msg with seq no %d\n", mcast->seq, mcast->this_seqno);

			regular_message_item.iov_len = recovery_message_item->iov_len;
			res = sq_item_inuse (&instance->regular_sort_queue, mcast->seq);
			if (res == 0) {
				sq_item_add (&instance->regular_sort_queue,
					&regular_message_item, mcast->seq);
				if (mcast->seq > instance->old_ring_state_high_seq_received) {
					instance->old_ring_state_high_seq_received = mcast->seq;
				}
			}
		} else {
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
				"-not adding msg with seq no %d\n", mcast->seq);
		}
	}
}

/*
 * Change states in the state machine of the membership algorithm
 */
static void memb_state_operational_enter (struct totemsrp_instance *instance)
{
	struct in_addr joined_list[PROCESSOR_COUNT_MAX];
	int joined_list_entries = 0;
	struct in_addr left_list[PROCESSOR_COUNT_MAX];
	int left_list_entries = 0;
	int aru_save;

	old_ring_state_reset (instance);
	ring_reset (instance);
	deliver_messages_from_recovery_to_regular (instance);

	instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
		"Delivering to app %d to %d\n",
		instance->my_high_delivered + 1, instance->old_ring_state_high_seq_received);

	aru_save = instance->my_aru;
	instance->my_aru = instance->old_ring_state_aru;

	messages_deliver_to_app (instance, 0, instance->old_ring_state_high_seq_received);

	/*
	 * Calculate joined and left list
	 */
	memb_set_subtract (left_list, &left_list_entries,
		instance->my_memb_list, instance->my_memb_entries,
		instance->my_trans_memb_list, instance->my_trans_memb_entries);

	memb_set_subtract (joined_list, &joined_list_entries,
		instance->my_new_memb_list, instance->my_new_memb_entries,
		instance->my_trans_memb_list, instance->my_trans_memb_entries);

	/*
	 * Deliver transitional configuration to application
	 */
	instance->totemsrp_confchg_fn (TOTEM_CONFIGURATION_TRANSITIONAL,
		instance->my_trans_memb_list, instance->my_trans_memb_entries,
		left_list, left_list_entries,
		0, 0, &instance->my_ring_id);
		
// TODO we need to filter to ensure we only deliver those
// messages which are part of instance->my_deliver_memb
	messages_deliver_to_app (instance, 1, instance->old_ring_state_high_seq_received);

	instance->my_aru = aru_save;

	/*
	 * Deliver regular configuration to application
	 */
	instance->totemsrp_confchg_fn (TOTEM_CONFIGURATION_REGULAR,
		instance->my_new_memb_list, instance->my_new_memb_entries,
		0, 0,
		joined_list, joined_list_entries, &instance->my_ring_id);

	/*
	 * Install new membership
	 */
	instance->my_memb_entries = instance->my_new_memb_entries;
	memcpy (instance->my_memb_list, instance->my_new_memb_list,
		sizeof (struct in_addr) * instance->my_memb_entries);
	instance->last_released = 0;
	instance->my_set_retrans_flg = 0;
	/*
	 * The recovery sort queue now becomes the regular
	 * sort queue.  It is necessary to copy the state
	 * into the regular sort queue.
	 */
	sq_copy (&instance->regular_sort_queue, &instance->recovery_sort_queue);
	instance->my_last_aru = 0;

	instance->my_proc_list_entries = instance->my_new_memb_entries;
	memcpy (instance->my_proc_list, instance->my_new_memb_list,
		sizeof (struct in_addr) * instance->my_memb_entries);

	instance->my_failed_list_entries = 0;
	instance->my_high_delivered = instance->my_aru;
// TODO the recovery messages are leaked

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"entering OPERATIONAL state.\n");
	instance->memb_state = MEMB_STATE_OPERATIONAL;

	return;
}

static void memb_state_gather_enter (struct totemsrp_instance *instance)
{
	memb_set_merge (&instance->my_id.sin_addr, 1,
		instance->my_proc_list, &instance->my_proc_list_entries);

	memb_join_message_send (instance);

	/*
	 * Restart the join timeout
	 */
	poll_timer_delete (*instance->totemsrp_poll_handle, instance->memb_timer_state_gather_join_timeout);

	poll_timer_add (*instance->totemsrp_poll_handle,
		instance->timeout_state_gather_join,
		(void *)instance,
		memb_timer_function_state_gather,
		&instance->memb_timer_state_gather_join_timeout);

	/*
	 * Restart the consensus timeout
	 */
	poll_timer_delete (*instance->totemsrp_poll_handle,
		instance->memb_timer_state_gather_consensus_timeout);

	poll_timer_add (*instance->totemsrp_poll_handle,
		instance->timeout_state_gather_consensus,
		(void *)instance,
		memb_timer_function_gather_consensus_timeout,
		&instance->memb_timer_state_gather_consensus_timeout);

	/*
	 * Cancel the token loss and token retransmission timeouts
	 */
	cancel_token_retransmit_timeout (instance); // REVIEWED
	cancel_token_timeout (instance); // REVIEWED
	cancel_merge_detect_timeout (instance);

	memb_consensus_reset (instance);

	memb_consensus_set (instance, &instance->my_id.sin_addr);

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"entering GATHER state.\n");

	instance->memb_state = MEMB_STATE_GATHER;

	return;
}

static void timer_function_token_retransmit_timeout (void *data);

static void memb_state_commit_enter (
	struct totemsrp_instance *instance,
	struct memb_commit_token *commit_token)
{
	ring_save (instance);

	old_ring_state_save (instance); 

	memb_state_commit_token_update (instance, commit_token);

	memb_state_commit_token_send (instance, commit_token);

	memb_ring_id_store (instance, commit_token);

	poll_timer_delete (*instance->totemsrp_poll_handle, instance->memb_timer_state_gather_join_timeout);

	instance->memb_timer_state_gather_join_timeout = 0;

	poll_timer_delete (*instance->totemsrp_poll_handle, instance->memb_timer_state_gather_consensus_timeout);

	instance->memb_timer_state_gather_consensus_timeout = 0;

	reset_token_timeout (instance); // REVIEWED
	reset_token_retransmit_timeout (instance); // REVIEWED

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"entering COMMIT state.\n");

	instance->memb_state = MEMB_STATE_COMMIT;

	return;
}

static void memb_state_recovery_enter (
	struct totemsrp_instance *instance,
	struct memb_commit_token *commit_token)
{
	int i;
#ifdef COMPILE_OUT
	int local_received_flg = 1;
#endif
	unsigned int low_ring_aru;
	unsigned int messages_originated = 0;

	instance->my_high_ring_delivered = 0;

	sq_reinit (&instance->recovery_sort_queue, 0);
	queue_reinit (&instance->retrans_message_queue);

	low_ring_aru = instance->old_ring_state_high_seq_received;

	memb_state_commit_token_send (instance, commit_token);

instance->my_token_seq = -1;
	/*
	 * Build regular configuration
	 */
	instance->my_new_memb_entries = commit_token->addr_entries;

	memcpy (instance->my_new_memb_list, commit_token->addr,
		sizeof (struct in_addr) * instance->my_new_memb_entries);

	/*
	 * Build transitional configuration
	 */
	memb_set_and (instance->my_new_memb_list, instance->my_new_memb_entries,
		instance->my_memb_list, instance->my_memb_entries,
		instance->my_trans_memb_list, &instance->my_trans_memb_entries);

	for (i = 0; i < instance->my_new_memb_entries; i++) {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"position [%d] member %s:\n", i, inet_ntoa (commit_token->addr[i]));
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"previous ring seq %lld rep %s\n",
			commit_token->memb_list[i].ring_id.seq,
			inet_ntoa (commit_token->memb_list[i].ring_id.rep));

		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"aru %d high delivered %d received flag %d\n",
			commit_token->memb_list[i].aru,
			commit_token->memb_list[i].high_delivered,
			commit_token->memb_list[i].received_flg);

		assert (commit_token->memb_list[i].ring_id.rep.s_addr);
	}
	/*
	 * Determine if any received flag is false
	 */
#ifdef COMPILE_OUT
	for (i = 0; i < commit_token->addr_entries; i++) {
		if (memb_set_subset (&instance->my_new_memb_list[i], 1,
			instance->my_trans_memb_list, instance->my_trans_memb_entries) &&

			commit_token->memb_list[i].received_flg == 0) {
#endif
			instance->my_deliver_memb_entries = instance->my_trans_memb_entries;
			memcpy (instance->my_deliver_memb_list, instance->my_trans_memb_list,
				sizeof (struct in_addr) * instance->my_trans_memb_entries);
#ifdef COMPILE_OUT
			local_received_flg = 0;
			break;
		}
	}
#endif
//	if (local_received_flg == 0) {
		/*
		 * Calculate my_low_ring_aru, instance->my_high_ring_delivered for the transitional membership
		 */
		for (i = 0; i < commit_token->addr_entries; i++) {
printf ("comparing %d old ring %s.%lld with commit ring %s.%lld.\n", i,
	inet_ntoa (instance->my_old_ring_id.rep), instance->my_old_ring_id.seq,
	inet_ntoa (commit_token->memb_list[i].ring_id.rep),
	commit_token->memb_list[i].ring_id.seq);
printf ("memb set subset %d\n", 
	memb_set_subset (&instance->my_new_memb_list[i], 1, instance->my_deliver_memb_list, instance->my_deliver_memb_entries));
			if (memb_set_subset (&instance->my_new_memb_list[i], 1,
				instance->my_deliver_memb_list,
				 instance->my_deliver_memb_entries) &&

			memcmp (&instance->my_old_ring_id,
				&commit_token->memb_list[i].ring_id,
				sizeof (struct memb_ring_id)) == 0) {
	
				if (low_ring_aru == 0 ||
					low_ring_aru > commit_token->memb_list[i].aru) {

					low_ring_aru = commit_token->memb_list[i].aru;
				}
				if (instance->my_high_ring_delivered < commit_token->memb_list[i].high_delivered) {
					instance->my_high_ring_delivered = commit_token->memb_list[i].high_delivered;
				}
			}
		}
		assert (low_ring_aru != 0xffffffff);
		/*
		 * Cpy all old ring messages to instance->retrans_message_queue
		 */
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"copying all old ring messages from %d-%d.\n",
			low_ring_aru + 1, instance->old_ring_state_high_seq_received);
			
		for (i = low_ring_aru + 1; i <= instance->old_ring_state_high_seq_received; i++) {

			struct sort_queue_item *sort_queue_item;
			struct message_item message_item;
			void *ptr;
			int res;

			res = sq_item_get (&instance->regular_sort_queue, i, &ptr);
			if (res != 0) {
printf ("-not copying %d-\n", i);
				continue;
			}
printf ("copying %d\n", i);
			sort_queue_item = ptr;
			assert (sort_queue_item->iov_len > 0);
			assert (sort_queue_item->iov_len <= MAXIOVS);
			messages_originated++;
			memset (&message_item, 0, sizeof (struct message_item));
// TODO LEAK
			message_item.mcast = malloc (sizeof (struct mcast));
			assert (message_item.mcast);
			memcpy (message_item.mcast, sort_queue_item->iovec[0].iov_base,
				sizeof (struct mcast));
			memcpy (&message_item.mcast->ring_id, &instance->my_ring_id,
				sizeof (struct memb_ring_id));
			message_item.mcast->header.encapsulated = 1;
			message_item.iov_len = sort_queue_item->iov_len;
			memcpy (&message_item.iovec, &sort_queue_item->iovec, sizeof (struct iovec) *
				sort_queue_item->iov_len);
			queue_item_add (&instance->retrans_message_queue, &message_item);
		}
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"Originated %d messages in RECOVERY.\n", messages_originated);
//	}

	instance->my_aru = 0;
	instance->my_aru_count = 0;
	instance->my_seq_unchanged = 0;
	instance->my_high_seq_received = 0;
	instance->my_install_seq = 0;

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "entering RECOVERY state.\n");
	reset_token_timeout (instance); // REVIEWED
	reset_token_retransmit_timeout (instance); // REVIEWED

	instance->memb_state = MEMB_STATE_RECOVERY;
	return;
}

static void encrypt_and_sign_worker (
	struct totemsrp_instance *instance,
	unsigned char *buf,
	int *buf_len,
	struct iovec *iovec,
	int iov_len,
	prng_state *prng_state_in)
{
	int i;
	unsigned char *addr;
	unsigned char keys[48];
	struct security_header *header;
	unsigned char *hmac_key = &keys[32];
	unsigned char *cipher_key = &keys[16];
	unsigned char *initial_vector = &keys[0];
	unsigned long len;
	int outlen = 0;
	hmac_state hmac_state;
	prng_state keygen_prng_state;
	prng_state stream_prng_state;

	header = (struct security_header *)buf;
	addr = buf + sizeof (struct security_header);

	memset (keys, 0, sizeof (keys));
	memset (header->salt, 0, sizeof (header->salt));

#if (defined(ENCRYPTION) || defined(AUTHENITCATION))
	/*
	 * Generate MAC, CIPHER, IV keys from private key
	 */
	sober128_read (header->salt, sizeof (header->salt), prng_state_in);
	sober128_start (&keygen_prng_state);
	sober128_add_entropy (instance->totemsrp_private_key,
		instance->totemsrp_private_key_len,
		&keygen_prng_state);	
	sober128_add_entropy (header->salt, sizeof (header->salt),
		&keygen_prng_state);

	sober128_read (keys, sizeof (keys), &keygen_prng_state);
#endif

#ifdef ENCRYPTION
	/*
	 * Setup stream cipher
	 */
	sober128_start (&stream_prng_state);
	sober128_add_entropy (cipher_key, 16, &stream_prng_state);	
	sober128_add_entropy (initial_vector, 16, &stream_prng_state);	
#endif

	/*
	 * Copy header of message, then remainder of message, then encrypt it
	 */
	memcpy (addr, iovec[0].iov_base + sizeof (struct security_header),
		iovec[0].iov_len - sizeof (struct security_header));
	addr += iovec[0].iov_len - sizeof (struct security_header);
	outlen += iovec[0].iov_len;

	for (i = 1; i < iov_len; i++) {
		memcpy (addr, iovec[i].iov_base, iovec[i].iov_len);
		addr += iovec[i].iov_len;
		outlen += iovec[i].iov_len;
	}

	/*
 	 * Encrypt message by XORing stream cipher data
	 */
#ifdef ENCRYPTION
	sober128_read (buf + sizeof (struct security_header),
		outlen - sizeof (struct security_header),
		&stream_prng_state);
#endif

#ifdef AUTHENTICATION
	memset (&hmac_state, 0, sizeof (hmac_state));

	/*
	 * Sign the contents of the message with the hmac key and store signature in message
	 */
	hmac_init (&hmac_state, DIGEST_SHA1, hmac_key, 16);

	hmac_process (&hmac_state, 
		buf + HMAC_HASH_SIZE,
		outlen - HMAC_HASH_SIZE);

	len = hash_descriptor[DIGEST_SHA1]->hashsize;

	hmac_done (&hmac_state, header->hash_digest, &len);
#endif
	*buf_len = outlen;
}

static void encrypt_and_sign (
	struct totemsrp_instance *instance,
	struct iovec *iovec,
	int iov_len)
{
	char *addr = instance->iov_encrypted.iov_base +
		sizeof (struct security_header);
	int i;
	unsigned char keys[48];
	struct security_header *header = instance->iov_encrypted.iov_base;
	prng_state keygen_prng_state;
	prng_state stream_prng_state;
	unsigned char *hmac_key = &keys[32];
	unsigned char *cipher_key = &keys[16];
	unsigned char *initial_vector = &keys[0];
	unsigned long len;

	instance->iov_encrypted.iov_len = 0;

	memset (keys, 0, sizeof (keys));
	memset (header->salt, 0, sizeof (header->salt));

#if (defined(ENCRYPTION) || defined(AUTHENITCATION))
	/*
	 * Generate MAC, CIPHER, IV keys from private key
	 */
	sober128_read (header->salt, sizeof (header->salt),
		&instance->totemsrp_prng_state);
	sober128_start (&keygen_prng_state);
	sober128_add_entropy (instance->totemsrp_private_key,
		instance->totemsrp_private_key_len, &keygen_prng_state);	
	sober128_add_entropy (header->salt, sizeof (header->salt), &keygen_prng_state);

	sober128_read (keys, sizeof (keys), &keygen_prng_state);
#endif

#ifdef ENCRYPTION
	/*
	 * Setup stream cipher
	 */
	sober128_start (&stream_prng_state);
	sober128_add_entropy (cipher_key, 16, &stream_prng_state);	
	sober128_add_entropy (initial_vector, 16, &stream_prng_state);	
#endif

#ifdef CODE_COVERAGE_COMPILE_OUT
if (log_digest) {
printf ("new encryption\n");
print_digest ("salt", header->salt);
print_digest ("initial_vector", initial_vector);
print_digest ("cipher_key", cipher_key);
print_digest ("hmac_key", hmac_key);
}
#endif

	/*
	 * Copy header of message, then remainder of message, then encrypt it
	 */
	memcpy (addr, iovec[0].iov_base + sizeof (struct security_header),
		iovec[0].iov_len - sizeof (struct security_header));
	addr += iovec[0].iov_len - sizeof (struct security_header);
	instance->iov_encrypted.iov_len += iovec[0].iov_len;

	for (i = 1; i < iov_len; i++) {
		memcpy (addr, iovec[i].iov_base, iovec[i].iov_len);
		addr += iovec[i].iov_len;
		instance->iov_encrypted.iov_len += iovec[i].iov_len;
	}

	/*
 	 * Encrypt message by XORing stream cipher data
	 */
#ifdef ENCRYPTION
	sober128_read (instance->iov_encrypted.iov_base + sizeof (struct security_header),
		instance->iov_encrypted.iov_len - sizeof (struct security_header),
		&stream_prng_state);
#endif

#ifdef AUTHENTICATION
	memset (&instance->totemsrp_hmac_state, 0, sizeof (hmac_state));

	/*
	 * Sign the contents of the message with the hmac key and store signature in message
	 */
	hmac_init (&instance->totemsrp_hmac_state, DIGEST_SHA1, hmac_key, 16);

	hmac_process (&instance->totemsrp_hmac_state, 
		instance->iov_encrypted.iov_base + HMAC_HASH_SIZE,
		instance->iov_encrypted.iov_len - HMAC_HASH_SIZE);

	len = hash_descriptor[DIGEST_SHA1]->hashsize;

	hmac_done (&instance->totemsrp_hmac_state, header->hash_digest, &len);
#endif

#ifdef COMPILE_OUT
print_digest ("initial_vector", initial_vector);
print_digest ("cipher_key", cipher_key);
print_digest ("hmac_key", hmac_key);
print_digest ("salt", header->salt);
print_digest ("sent digest", header->hash_digest);
#endif
}

/*
 * Only designed to work with a message with one iov
 */
static int authenticate_and_decrypt (
	struct totemsrp_instance *instance,
	struct iovec *iov)
{
	unsigned char keys[48];
	struct security_header *header = iov[0].iov_base;
	prng_state keygen_prng_state;
	prng_state stream_prng_state;
	unsigned char *hmac_key = &keys[32];
	unsigned char *cipher_key = &keys[16];
	unsigned char *initial_vector = &keys[0];
	unsigned char digest_comparison[HMAC_HASH_SIZE];
	unsigned long len;
	int res = 0;

	instance->iov_encrypted.iov_len = 0;

#ifdef COMPILE_OUT
	printf ("Decryption message\n");
	print_msg (header, iov[0].iov_len);
#endif
#if (defined(ENCRYPTION) || defined(AUTHENITCATION))
	/*
	 * Generate MAC, CIPHER, IV keys from private key
	 */
	memset (keys, 0, sizeof (keys));
	sober128_start (&keygen_prng_state);
	sober128_add_entropy (instance->totemsrp_private_key,
		instance->totemsrp_private_key_len, &keygen_prng_state);	
	sober128_add_entropy (header->salt, sizeof (header->salt), &keygen_prng_state);

	sober128_read (keys, sizeof (keys), &keygen_prng_state);
#endif

#ifdef ENCRYPTION
	/*
	 * Setup stream cipher
	 */
	sober128_start (&stream_prng_state);
	sober128_add_entropy (cipher_key, 16, &stream_prng_state);	
	sober128_add_entropy (initial_vector, 16, &stream_prng_state);	
#endif

#ifdef CODE_COVERAGE_COMPILE_OUT
if (log_digest) {
printf ("New decryption\n");
print_digest ("salt", header->salt);
print_digest ("initial_vector", initial_vector);
print_digest ("cipher_key", cipher_key);
print_digest ("hmac_key", hmac_key);
}
#endif

#ifdef AUTHENTICATION
	/*
	 * Authenticate contents of message
	 */
	hmac_init (&instance->totemsrp_hmac_state, DIGEST_SHA1, hmac_key, 16);

	hmac_process (&instance->totemsrp_hmac_state, 
		iov->iov_base + HMAC_HASH_SIZE,
		iov->iov_len - HMAC_HASH_SIZE);

	len = hash_descriptor[DIGEST_SHA1]->hashsize;
	assert (HMAC_HASH_SIZE >= len);
	hmac_done (&instance->totemsrp_hmac_state, digest_comparison, &len);

#ifdef PRINTDIGESTS
print_digest ("received digest", header->hash_digest);
print_digest ("calculated digest", digest_comparison);
#endif
	if (memcmp (digest_comparison, header->hash_digest, len) != 0) {
#ifdef CODE_COVERAGE_COMPILE_OUT
print_digest ("initial_vector", initial_vector);
print_digest ("cipher_key", cipher_key);
print_digest ("hmac_key", hmac_key);
print_digest ("salt", header->salt);
print_digest ("sent digest", header->hash_digest);
print_digest ("calculated digest", digest_comparison);
printf ("received message size %d\n", iov->iov_len);
#endif
		instance->totemsrp_log_printf (instance->totemsrp_log_level_security, "Received message has invalid digest... ignoring.\n");
		res = -1;
		return (-1);
	}
#endif /* AUTHENTICATION */
	
	/*
	 * Decrypt the contents of the message with the cipher key
	 */
#ifdef ENCRYPTION
	sober128_read (iov->iov_base + sizeof (struct security_header),
		iov->iov_len - sizeof (struct security_header),
		&stream_prng_state);
#endif

	return (res);
	return (0);
}

int totemsrp_new_msg_signal (totemsrp_handle handle)
{
	struct totemsrp_instance *instance;
	SaErrorT error;

	error = saHandleInstanceGet (&totemsrp_instance_database, handle,
		(void *)&instance);
	if (error != SA_OK) {
		goto error_exit;
	}

	token_hold_cancel_send (instance);

	saHandleInstancePut (&totemsrp_instance_database, handle);
	return (0);
error_exit:
	return (-1);
}

int totemsrp_mcast (
	totemsrp_handle handle,
	struct iovec *iovec,
	int iov_len,
	int guarantee)
{
	int i;
	int j;
	struct message_item message_item;
	struct totemsrp_instance *instance;
	SaErrorT error;

	error = saHandleInstanceGet (&totemsrp_instance_database, handle,
		(void *)&instance);
	if (error != SA_OK) {
		goto error_exit;
	}
	
	if (queue_is_full (&instance->new_message_queue)) {
		return (-1);
	}
	for (j = 0, i = 0; i < iov_len; i++) {
		j+= iovec[i].iov_len;
	}

	memset (&message_item, 0, sizeof (struct message_item));

	/*
	 * Allocate pending item
	 */
// TODO LEAK
	message_item.mcast = malloc (sizeof (struct mcast));
	if (message_item.mcast == 0) {
		goto error_mcast;
	}

	/*
	 * Set mcast header
	 */
	message_item.mcast->header.type = MESSAGE_TYPE_MCAST;
	message_item.mcast->header.endian_detector = ENDIAN_LOCAL;
	message_item.mcast->header.encapsulated = 2;
	message_item.mcast->guarantee = guarantee;
	message_item.mcast->source.s_addr = instance->my_id.sin_addr.s_addr;

	for (i = 0; i < iov_len; i++) {
// TODO LEAK
		message_item.iovec[i].iov_base = malloc (iovec[i].iov_len);

		if (message_item.iovec[i].iov_base == 0) {
			goto error_iovec;
		}

		memcpy (message_item.iovec[i].iov_base, iovec[i].iov_base,
			iovec[i].iov_len);

		message_item.iovec[i].iov_len = iovec[i].iov_len;
	}

	message_item.iov_len = iov_len;

	instance->totemsrp_log_printf (instance->totemsrp_log_level_debug, "mcasted message added to pending queue\n");
	queue_item_add (&instance->new_message_queue, &message_item);

	saHandleInstancePut (&totemsrp_instance_database, handle);
	return (0);

error_iovec:
	saHandleInstancePut (&totemsrp_instance_database, handle);
	for (j = 0; j < i; j++) {
		free (message_item.iovec[j].iov_base);
	}
	return (-1);

error_mcast:
	saHandleInstancePut (&totemsrp_instance_database, handle);

error_exit:
	return (0);
}

/*
 * Determine if there is room to queue a new message
 */
int totemsrp_avail (totemsrp_handle handle)
{
	int avail;
	struct totemsrp_instance *instance;
	SaErrorT error;

	error = saHandleInstanceGet (&totemsrp_instance_database, handle,
		(void *)&instance);
	if (error != SA_OK) {
		goto error_exit;
	}

	queue_avail (&instance->new_message_queue, &avail);

	saHandleInstancePut (&totemsrp_instance_database, handle);

	return (avail);

error_exit:
	return (0);
}

static int netif_determine (
	struct totemsrp_instance *instance,
	struct sockaddr_in *bindnet,
	struct sockaddr_in *bound_to,
	int *interface_up)
{
	struct sockaddr_in *sockaddr_in;
	int id_fd;
	struct ifconf ifc;
	int numreqs = 0;
	int res;
	int i;
	in_addr_t mask_addr;

	*interface_up = 0;

	/*
	 * Generate list of local interfaces in ifc.ifc_req structure
	 */
	id_fd = socket (AF_INET, SOCK_STREAM, 0);
	ifc.ifc_buf = 0;
	do {
		numreqs += 32;
		ifc.ifc_len = sizeof (struct ifreq) * numreqs;
		ifc.ifc_buf = (void *)realloc(ifc.ifc_buf, ifc.ifc_len);
		res = ioctl (id_fd, SIOCGIFCONF, &ifc);
		if (res < 0) {
			close (id_fd);
			return -1;
		}
	} while (ifc.ifc_len == sizeof (struct ifreq) * numreqs);
	res = -1;

	/*
	 * Find interface address to bind to
	 */
	for (i = 0; i < ifc.ifc_len / sizeof (struct ifreq); i++) {
		sockaddr_in = (struct sockaddr_in *)&ifc.ifc_ifcu.ifcu_req[i].ifr_ifru.ifru_addr;
		mask_addr = inet_addr ("255.255.255.0");

		if ((sockaddr_in->sin_family == AF_INET) &&
			(sockaddr_in->sin_addr.s_addr & mask_addr) ==
			(bindnet->sin_addr.s_addr & mask_addr)) {

			bound_to->sin_addr.s_addr = sockaddr_in->sin_addr.s_addr;
			res = i;

			if (ioctl(id_fd, SIOCGIFFLAGS, &ifc.ifc_ifcu.ifcu_req[i]) < 0) {
				printf ("couldn't do ioctl\n");
			}

			*interface_up = ifc.ifc_ifcu.ifcu_req[i].ifr_ifru.ifru_flags & IFF_UP;
			break; /* for */
		}
	}
	free (ifc.ifc_buf);
	close (id_fd);
	
	return (res);
}

static int loopback_determine (struct sockaddr_in *bound_to)
{

	bound_to->sin_addr.s_addr = LOCALHOST_IP;
	if (&bound_to->sin_addr.s_addr == 0) {
		return -1;
	}
	return 1;
}


/*
 * If the interface is up, the sockets for totem are built.  If the interface is down
 * this function is requeued in the timer list to retry building the sockets later.
 */
static void timer_function_netif_check_timeout (
	void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;
	int res;
	int interface_no;
	int interface_up;


	/*
	* Build sockets for every interface
	*/
	for (interface_no = 0; interface_no < instance->totemsrp_interface_count; interface_no++) {

		netif_determine (instance,
			&instance->totemsrp_interfaces[interface_no].bindnet,
			&instance->totemsrp_interfaces[interface_no].boundto,
			&interface_up);

		if (((instance->netif_bind_state & BIND_STATE_LOOPBACK) && (!interface_up))
				|| ((instance->netif_bind_state & BIND_STATE_REGULAR) && (interface_up)))	{
			break;
		}

		instance->totemsrp_log_printf(instance->totemsrp_log_level_debug,"network interface UP  %s\n",
			inet_ntoa (instance->totemsrp_interfaces[interface_no].boundto.sin_addr));
	
		if (instance->totemsrp_sockets[interface_no].mcast > 0) {
			close (instance->totemsrp_sockets[interface_no].mcast);
		 	poll_dispatch_delete (*instance->totemsrp_poll_handle,
			instance->totemsrp_sockets[interface_no].mcast);
		}
		if (instance->totemsrp_sockets[interface_no].token > 0) {
			close (instance->totemsrp_sockets[interface_no].token);
			poll_dispatch_delete (*instance->totemsrp_poll_handle,
			instance->totemsrp_sockets[interface_no].token);
		}

		if (!interface_up) {
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,"Interface is down binding to LOOPBACK addr.\n");
			instance->netif_bind_state = BIND_STATE_LOOPBACK;
			res = totemsrp_build_sockets_loopback(instance,
				&instance->sockaddr_in_mcast,
				&instance->totemsrp_interfaces[interface_no].bindnet,
				&instance->totemsrp_sockets[interface_no],
				&instance->totemsrp_interfaces[interface_no].boundto);
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,"network interface LOCAL %s\n",
				inet_ntoa (instance->totemsrp_interfaces[interface_no].boundto.sin_addr));

			poll_dispatch_add (*instance->totemsrp_poll_handle, instance->totemsrp_sockets[interface_no].token,
					POLLIN, instance, recv_handler, UINT_MAX);

			continue;
		}

		instance->netif_bind_state = BIND_STATE_REGULAR;

		/*
		* Create and bind the multicast and unicast sockets
		*/
		res = totemsrp_build_sockets (instance,
			&instance->sockaddr_in_mcast,
			&instance->totemsrp_interfaces[interface_no].bindnet,
			&instance->totemsrp_sockets[interface_no],
			&instance->totemsrp_interfaces[interface_no].boundto,
			&interface_up);

		poll_dispatch_add (*instance->totemsrp_poll_handle, instance->totemsrp_sockets[interface_no].mcast,
			POLLIN, instance, recv_handler, UINT_MAX);

		poll_dispatch_add (*instance->totemsrp_poll_handle, instance->totemsrp_sockets[interface_no].token,
			POLLIN, instance, recv_handler, UINT_MAX);
	}

	memcpy (&instance->my_id, &instance->totemsrp_interfaces->boundto, sizeof (struct sockaddr_in));	
	/*
	* This stuff depends on totemsrp_build_sockets
	*/
	if (instance->firstrun == 0) {
		instance->firstrun += 1;
		memcpy (&instance->my_memb_list[0], &instance->totemsrp_interfaces->boundto,
			sizeof (struct sockaddr_in));
		memb_ring_id_create_or_load (instance, &instance->my_ring_id);
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "Created or loaded sequence id %lld.%s for this ring.\n",
		instance->my_ring_id.seq, inet_ntoa (instance->my_ring_id.rep));
	}

	if (interface_up) {
		if (instance->netif_state_report & NETIF_STATE_REPORT_UP) {
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
				" The network interface is now up.\n");		
			instance->netif_state_report = NETIF_STATE_REPORT_DOWN;
			memb_state_gather_enter (instance);
		}
		/*
		 * If this is a single processor, detect downs which may not 
		 * be detected by token loss when the interface is downed
		 */
		if (instance->my_memb_entries <= 1) {
			poll_timer_add (*instance->totemsrp_poll_handle,
				instance->timeout_downcheck,
				(void *)instance,
				timer_function_netif_check_timeout,
				&instance->timer_netif_check_timeout);
		}
	} else {		
		if (instance->netif_state_report & NETIF_STATE_REPORT_DOWN) {
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
				"The network interface is down.\n");
			memb_state_gather_enter (instance);
		}
		instance->netif_state_report = NETIF_STATE_REPORT_UP;

		/*
		* Add a timer to retry building interfaces and request memb_gather_enter
		*/
		cancel_token_timeout (instance);
		poll_timer_add (*instance->totemsrp_poll_handle,
			instance->timeout_downcheck,
			(void *)instance,
			timer_function_netif_check_timeout,
			&instance->timer_netif_check_timeout);
	}
}


/*
 * Check if an interface is down and reconfigure
 * totemsrp waiting for it to come back up
 */
static void netif_down_check (struct totemsrp_instance *instance)
{
	timer_function_netif_check_timeout (instance);
}

static int totemsrp_build_sockets_loopback (
	struct totemsrp_instance *instance,
	struct sockaddr_in *sockaddr_mcast,
	struct sockaddr_in *sockaddr_bindnet,
	struct totemsrp_socket *sockets,
	struct sockaddr_in *bound_to)
{
	struct ip_mreq mreq;
	struct sockaddr_in sockaddr_in;
	int res;

	memset (&mreq, 0, sizeof (struct ip_mreq));

	/*
	 * Determine the ip address bound to and the interface name
	 */
	res = loopback_determine (bound_to);

	if (res == -1) {
		return (-1);
	}

	/* TODO this should be somewhere else */
	instance->memb_local_sockaddr_in.sin_addr.s_addr = bound_to->sin_addr.s_addr;
	instance->memb_local_sockaddr_in.sin_family = AF_INET;
	instance->memb_local_sockaddr_in.sin_port = sockaddr_mcast->sin_port;

	sockaddr_in.sin_family = AF_INET;
	sockaddr_in.sin_port = sockaddr_mcast->sin_port;

	 /*
	 * Setup unicast socket
	 */
	sockets->token = socket (AF_INET, SOCK_DGRAM, 0);
	if (sockets->token == -1) {
		perror ("socket2");
		return (-1);
	}

	/*
	 * Bind to unicast socket used for token send/receives	
	 * This has the side effect of binding to the correct interface
	 */
	sockaddr_in.sin_addr.s_addr = bound_to->sin_addr.s_addr;
	res = bind (sockets->token, (struct sockaddr *)&sockaddr_in,
			sizeof (struct sockaddr_in));
	if (res == -1) {
		perror ("bind2 failed");
		return (-1);
	}

	memcpy(&instance->sockaddr_in_mcast, &sockaddr_in,
		sizeof(struct sockaddr_in));
	sockets->mcast = sockets->token;

	return (0);
}


static int totemsrp_build_sockets (
	struct totemsrp_instance *instance,
	struct sockaddr_in *sockaddr_mcast,
	struct sockaddr_in *sockaddr_bindnet,
	struct totemsrp_socket *sockets,
	struct sockaddr_in *bound_to,
	int *interface_up)
{
	struct ip_mreq mreq;
	struct sockaddr_in sockaddr_in;
	char flag;
	int res;
	
	memset (&mreq, 0, sizeof (struct ip_mreq));

	/*
	 * Determine the ip address bound to and the interface name
	 */
	res = netif_determine (instance,
		sockaddr_bindnet,
		bound_to,
		interface_up);

	if (res == -1) {
		return (-1);
	}

	/* TODO this should be somewhere else */
	instance->memb_local_sockaddr_in.sin_addr.s_addr = bound_to->sin_addr.s_addr;
	instance->memb_local_sockaddr_in.sin_family = AF_INET;
	instance->memb_local_sockaddr_in.sin_port = sockaddr_mcast->sin_port;

	/*
	 * Create multicast socket
	 */
	sockets->mcast = socket (AF_INET, SOCK_DGRAM, 0);
	if (sockets->mcast == -1) {
		perror ("socket");
		return (-1);
	}

	if (setsockopt (sockets->mcast, SOL_IP, IP_MULTICAST_IF,
		&bound_to->sin_addr, sizeof (struct in_addr)) < 0) {

		instance->totemsrp_log_printf (instance->totemsrp_log_level_warning, "Could not bind to device for multicast, group messaging may not work properly. (%s)\n", strerror (errno));
	}

	/*
	 * Bind to multicast socket used for multicast send/receives
	 */
	sockaddr_in.sin_family = AF_INET;
	sockaddr_in.sin_addr.s_addr = sockaddr_mcast->sin_addr.s_addr;
	sockaddr_in.sin_port = sockaddr_mcast->sin_port;
	res = bind (sockets->mcast, (struct sockaddr *)&sockaddr_in,
		sizeof (struct sockaddr_in));
	if (res == -1) {
		perror ("bind failed");
		return (-1);
	}

	/*
	 * Setup unicast socket
	 */
	sockets->token = socket (AF_INET, SOCK_DGRAM, 0);
	if (sockets->token == -1) {
		perror ("socket2");
		return (-1);
	}

	/*
	 * Bind to unicast socket used for token send/receives
	 * This has the side effect of binding to the correct interface
	 */
	sockaddr_in.sin_addr.s_addr = bound_to->sin_addr.s_addr;
	res = bind (sockets->token, (struct sockaddr *)&sockaddr_in,
		sizeof (struct sockaddr_in));
	if (res == -1) {
		perror ("bind2 failed");
		return (-1);
	}

#ifdef CONFIG_USE_BROADCAST
/* This config option doesn't work */
{
	int on = 1;
	setsockopt (sockets->mcast, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof (on));
}
#else
	/*
	 * Join group membership on socket
	 */
	mreq.imr_multiaddr.s_addr = sockaddr_mcast->sin_addr.s_addr;
	mreq.imr_interface.s_addr = bound_to->sin_addr.s_addr;

	res = setsockopt (sockets->mcast, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		&mreq, sizeof (mreq));
	if (res == -1) {
		perror ("join multicast group failed");
		return (-1);
	}

#endif
	/*
	 * Turn on multicast loopback
	 */
	flag = 1;
	res = setsockopt (sockets->mcast, IPPROTO_IP, IP_MULTICAST_LOOP,
		&flag, sizeof (flag));
	if (res == -1) {
		perror ("turn off loopback");
		return (-1);
	}

	return (0);
}
	
/*
 * Misc Management
 */
static int in_addr_compare (const void *a, const void *b) {
	struct in_addr *in_addr_a = (struct in_addr *)a;
	struct in_addr *in_addr_b = (struct in_addr *)b;

	return (in_addr_a->s_addr > in_addr_b->s_addr);
}

/*
 * ORF Token Management
 */
/* 
 * Recast message to mcast group if it is available
 */
static int orf_token_remcast (
	struct totemsrp_instance *instance,
	int seq)
{
	struct msghdr msg_mcast;
	struct sort_queue_item *sort_queue_item;
	int res;
	struct mcast *mcast;
	void *ptr;

	struct sq *sort_queue;

	if (instance->memb_state == MEMB_STATE_RECOVERY) {
		sort_queue = &instance->recovery_sort_queue;
	} else {
		sort_queue = &instance->regular_sort_queue;
	}

	/*
	 * Get RTR item at seq, if not available, return
	 */
	res = sq_item_get (sort_queue, seq, &ptr);
	if (res != 0) {
		return -1;
	}

	sort_queue_item = ptr;

	mcast = (struct mcast *)sort_queue_item->iovec[0].iov_base;

	encrypt_and_sign (instance, sort_queue_item->iovec, sort_queue_item->iov_len);

	/*
	 * Build multicast message
	 */
	msg_mcast.msg_name = (caddr_t)&instance->sockaddr_in_mcast;
	msg_mcast.msg_namelen = sizeof (struct sockaddr_in);
	msg_mcast.msg_iov = &instance->iov_encrypted;
	msg_mcast.msg_iovlen = 1;
	msg_mcast.msg_control = 0;
	msg_mcast.msg_controllen = 0;
	msg_mcast.msg_flags = 0;

	/*
	 * Multicast message
	 */
	res = sendmsg (instance->totemsrp_sockets[0].mcast, &msg_mcast, MSG_NOSIGNAL | MSG_DONTWAIT);
	
	if (res == -1) {
		return (-1);
	}
	instance->stats_sent += res;
	return (0);
}


/*
 * Free all freeable messages from ring
 */
static int messages_free (
	struct totemsrp_instance *instance,
	int token_aru)
{
	struct sort_queue_item *regular_message;
	int i, j;
	int res;
	int log_release = 0;
	int release_to;

	release_to = token_aru;
	if (release_to > instance->my_last_aru) {
		release_to = instance->my_last_aru;
	}
	if (release_to > instance->my_high_delivered) {
		release_to = instance->my_high_delivered;
	}

	/*
	 * Release retransmit list items if group aru indicates they are transmitted
	 */
	for (i = instance->last_released; i <= release_to; i++) {
		void *ptr;
		res = sq_item_get (&instance->regular_sort_queue, i, &ptr);
		if (res == 0) {
			regular_message = ptr;
			for (j = 0; j < regular_message->iov_len; j++) {
				free (regular_message->iovec[j].iov_base);
			}
		}
		sq_items_release (&instance->regular_sort_queue, i);
		instance->last_released = i + 1;
		log_release = 1;
	}

 	if (log_release) {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
			"releasing messages up to and including %d\n", release_to);
	}
	return (0);
}

static void update_aru (
	struct totemsrp_instance *instance)
{
	int i;
	int res;
	struct sq *sort_queue;

	if (instance->memb_state == MEMB_STATE_RECOVERY) {
		sort_queue = &instance->recovery_sort_queue;
	} else {
		sort_queue = &instance->regular_sort_queue;
	}

	for (i = instance->my_aru + 1; i <= instance->my_high_seq_received; i++) {
		void *ptr;

		res = sq_item_get (sort_queue, i, &ptr);
		/*
		 * If hole, stop assembly
		 */
		if (res != 0) {
			break;
		}
		instance->my_aru = i;
	}
//	instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
//		"setting received flag to FALSE %d %d\n",
//		instance->my_aru, instance->my_high_seq_received);
	instance->my_received_flg = 0;
	if (instance->my_aru == instance->my_high_seq_received) {
//		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
//			"setting received flag to TRUE %d %d\n",
//			instance->my_aru, instance->my_high_seq_received);
		instance->my_received_flg = 1;
	}
}

static void orf_token_mcast_worker_fn (void *thread_state, void *work_item)
{
	struct mcast_worker_fn_work_item *mcast_worker_fn_work_item =
		(struct mcast_worker_fn_work_item *)work_item;
	struct sort_queue_item *sort_queue_item = mcast_worker_fn_work_item->sort_queue_item;
	struct orf_token_mcast_thread_state *orf_token_mcast_thread_state =
		(struct orf_token_mcast_thread_state *)thread_state;
	struct totemsrp_instance *instance = mcast_worker_fn_work_item->instance;
	struct msghdr msg_mcast;
	int res = 0;
	int buf_len;
	struct iovec iov;

	/*
	 * Encrypt and digest the message
	 */
	encrypt_and_sign_worker (
		instance,
		orf_token_mcast_thread_state->iobuf, &buf_len,
		sort_queue_item->iovec, sort_queue_item->iov_len,
		&orf_token_mcast_thread_state->prng_state);

	iov.iov_base = orf_token_mcast_thread_state->iobuf;
	iov.iov_len = buf_len;

	/*
	 * Build multicast message
	 */
	msg_mcast.msg_name = &instance->sockaddr_in_mcast;
	msg_mcast.msg_namelen = sizeof (struct sockaddr_in);
	msg_mcast.msg_iov = &iov;
	msg_mcast.msg_iovlen = 1;
	msg_mcast.msg_control = 0;
	msg_mcast.msg_controllen = 0;
	msg_mcast.msg_flags = 0;

	/*
	 * Multicast message
	 * An error here is recovered by the multicast algorithm
	 */
	res = sendmsg (instance->totemsrp_sockets[0].mcast, &msg_mcast, MSG_NOSIGNAL | MSG_DONTWAIT);

	instance->iov_encrypted.iov_len = PACKET_SIZE_MAX;

	if (res > 0) {
		instance->stats_sent += res;
	}
}

/*
 * Multicasts pending messages onto the ring (requires orf_token possession)
 */
static int orf_token_mcast (
	struct totemsrp_instance *instance,
	struct orf_token *token,
	int fcc_mcasts_allowed,
	struct sockaddr_in *system_from)
{
	struct mcast_worker_fn_work_item work_item;
	struct message_item *message_item = 0;
	struct queue *mcast_queue;
	struct sq *sort_queue;
	struct sort_queue_item sort_queue_item;
	struct sort_queue_item *sort_queue_item_ptr;
	struct mcast *mcast;

	if (instance->memb_state == MEMB_STATE_RECOVERY) {
		mcast_queue = &instance->retrans_message_queue;
		sort_queue = &instance->recovery_sort_queue;
		reset_token_retransmit_timeout (instance); // REVIEWED
	} else {
		mcast_queue = &instance->new_message_queue;
		sort_queue = &instance->regular_sort_queue;
	}

	for (instance->fcc_mcast_current = 0; instance->fcc_mcast_current < fcc_mcasts_allowed; instance->fcc_mcast_current++) {
		if (queue_is_empty (mcast_queue)) {
			break;
		}
		message_item = (struct message_item *)queue_item_get (mcast_queue);
		/* preincrement required by algo */
		if (instance->old_ring_state_saved &&
			(instance->memb_state == MEMB_STATE_GATHER ||
			instance->memb_state == MEMB_STATE_COMMIT)) {

			instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
				"not multicasting at seqno is %d\n",
			token->seq);
			return (0);
		}
		message_item->mcast->seq = ++token->seq;
		message_item->mcast->this_seqno = instance->global_seqno++;

		/*
		 * Build IO vector
		 */
		memset (&sort_queue_item, 0, sizeof (struct sort_queue_item));
		sort_queue_item.iovec[0].iov_base = message_item->mcast;
		sort_queue_item.iovec[0].iov_len = sizeof (struct mcast);
	
		mcast = sort_queue_item.iovec[0].iov_base;
	
		memcpy (&sort_queue_item.iovec[1], message_item->iovec,
			message_item->iov_len * sizeof (struct iovec));

		memcpy (&mcast->ring_id, &instance->my_ring_id, sizeof (struct memb_ring_id));

		sort_queue_item.iov_len = message_item->iov_len + 1;

		assert (sort_queue_item.iov_len < 16);

		/*
		 * Add message to retransmit queue
		 */
		sort_queue_item_ptr = sq_item_add (sort_queue,
			&sort_queue_item, message_item->mcast->seq);

// XXX	printf ("ORIG [%s.%d-%d]\n", inet_ntoa (message_item->mcast->source),
// XXX		message_item->mcast->seq, message_item->mcast->this_seqno);

		/*
		 * Only encrypt and send on wire if we are single processor
		 */
#define CONFIG_THREADED_SEND


#ifdef CONFIG_THREADED_SEND

			work_item.sort_queue_item = sort_queue_item_ptr;
			work_item.instance = instance;

			/*
			 * If threads, add message to worker thread queue
			 */
			worker_thread_group_work_add (&instance->worker_thread_group_orf_token_mcast,
				&work_item);
#else
			/*
			 * If no threads, execute worker thread directly
			 */
//			orf_token_mcast_worker_fn (&worker_data);
#endif
		/*
		 * Delete item from pending queue
		 */
		queue_item_remove (mcast_queue);
	}


	assert (instance->fcc_mcast_current < 100);

	/*
	 * If messages mcasted, deliver any new messages to totemg
	 */
	instance->my_high_seq_received = token->seq;
		
	update_aru (instance);
	/*
	 * Return 1 if more messages are available for single node clusters
	 */
	return (instance->fcc_mcast_current);
}

/*
 * Remulticasts messages in orf_token's retransmit list (requires orf_token)
 * Modify's orf_token's rtr to include retransmits required by this process
 */
static int orf_token_rtr (
	struct totemsrp_instance *instance,
	struct orf_token *orf_token,
	int *fcc_allowed)
{
	int res;
	int i, j;
	int found;
	int total_entries;
	struct sq *sort_queue;
	struct rtr_item *rtr_list;

	if (instance->memb_state == MEMB_STATE_RECOVERY) {
		sort_queue = &instance->recovery_sort_queue;
	} else {
		sort_queue = &instance->regular_sort_queue;
	}

	rtr_list = &orf_token->rtr_list[0];
	if (orf_token->rtr_list_entries) {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
			"Retransmit List %d\n", orf_token->rtr_list_entries);
		for (i = 0; i < orf_token->rtr_list_entries; i++) {
			instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
				"%d ", rtr_list[i].seq);
		}
		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug, "\n");
	}

	total_entries = orf_token->rtr_list_entries;

	/*
	 * Retransmit messages on orf_token's RTR list from RTR queue
	 */
	for (instance->fcc_remcast_current = 0, i = 0;
		instance->fcc_remcast_current <= *fcc_allowed && i < orf_token->rtr_list_entries;) {

		/*
		 * If this retransmit request isn't from this configuration,
		 * try next rtr entry
		 */
 		if (memcmp (&rtr_list[i].ring_id, &instance->my_ring_id,
			sizeof (struct memb_ring_id)) != 0) {

			i += 1;
			continue;
		}

		assert (rtr_list[i].seq > 0);
		res = orf_token_remcast (instance, rtr_list[i].seq);
		if (res == 0) {
			/*
			 * Multicasted message, so no need to copy to new retransmit list
			 */
			orf_token->rtr_list_entries -= 1;
			assert (orf_token->rtr_list_entries >= 0);
			memmove (&rtr_list[i], &rtr_list[i + 1],
				sizeof (struct rtr_item) * (orf_token->rtr_list_entries));

			instance->fcc_remcast_current++;
			instance->stats_remcasts++;
		} else {
			i += 1;
		}
	}
	*fcc_allowed = *fcc_allowed - instance->fcc_remcast_current - 1;

#ifdef COMPILE_OUT
for (i = 0; i < orf_token->rtr_list_entries; i++) {
	assert (rtr_list_old[index_old].seq != -1);
}
#endif

	/*
	 * Add messages to retransmit to RTR list
	 * but only retry if there is room in the retransmit list
	 */
	for (i = instance->my_aru + 1;
		orf_token->rtr_list_entries < RETRANSMIT_ENTRIES_MAX &&
		i <= instance->my_high_seq_received;
		i++) {

		/*
		 * Find if a message is missing from this processor
		 */
		res = sq_item_inuse (sort_queue, i);
		if (res == 0) {
			/*
			 * Determine if missing message is already in retransmit list
			 */
			found = 0;
			for (j = 0; j < orf_token->rtr_list_entries; j++) {
				if (i == rtr_list[j].seq) {
					found = 1;
				}
			}
			if (found == 0) {
				/*
				 * Missing message not found in current retransmit list so add it
				 */
				memcpy (&rtr_list[orf_token->rtr_list_entries].ring_id,
					&instance->my_ring_id, sizeof (struct memb_ring_id));
				rtr_list[orf_token->rtr_list_entries].seq = i;
				orf_token->rtr_list_entries++;
			}
		}
	}
	return (instance->fcc_remcast_current);
}

static void token_retransmit (struct totemsrp_instance *instance) {
	struct iovec iovec;
	struct msghdr msg_orf_token;
	int res;

	iovec.iov_base = instance->orf_token_retransmit;
	iovec.iov_len = instance->orf_token_retransmit_size;

	msg_orf_token.msg_name = &instance->next_memb;
	msg_orf_token.msg_namelen = sizeof (struct sockaddr_in);
	msg_orf_token.msg_iov = &iovec;
	msg_orf_token.msg_iovlen = 1;
	msg_orf_token.msg_control = 0;
	msg_orf_token.msg_controllen = 0;
	msg_orf_token.msg_flags = 0;
	
	res = sendmsg (instance->totemsrp_sockets[0].token, &msg_orf_token, MSG_NOSIGNAL);
}

/*
 * Retransmit the regular token if no mcast or token has
 * been received in retransmit token period retransmit
 * the token to the next processor
 */
static void timer_function_token_retransmit_timeout (void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;

	switch (instance->memb_state) {
	case MEMB_STATE_GATHER:
		break;
	case MEMB_STATE_COMMIT:
		break;
	case MEMB_STATE_OPERATIONAL:
	case MEMB_STATE_RECOVERY:
		token_retransmit (instance);
		reset_token_retransmit_timeout (instance); // REVIEWED
		break;
	}
}

static void timer_function_token_hold_retransmit_timeout (void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;

	switch (instance->memb_state) {
	case MEMB_STATE_GATHER:
		break;
	case MEMB_STATE_COMMIT:
		break;
	case MEMB_STATE_OPERATIONAL:
	case MEMB_STATE_RECOVERY:
		token_retransmit (instance);
		break;
	}
}

static void timer_function_merge_detect_timeout(void *data)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;

	instance->my_merge_detect_timeout_outstanding = 0;

	switch (instance->memb_state) {
	case MEMB_STATE_OPERATIONAL:
		if (instance->my_ring_id.rep.s_addr == instance->my_id.sin_addr.s_addr) {
			memb_merge_detect_transmit (instance);
		}
		break;
	case MEMB_STATE_GATHER:
	case MEMB_STATE_COMMIT:
	case MEMB_STATE_RECOVERY:
		break;
	}
}

/*
 * Send orf_token to next member (requires orf_token)
 */
static int token_send (
	struct totemsrp_instance *instance,
	struct orf_token *orf_token,
	int forward_token)
{
	struct msghdr msg_orf_token;
	struct iovec iovec;
	int res;

	iovec.iov_base = (char *)orf_token;
	iovec.iov_len = sizeof (struct orf_token) +
		(orf_token->rtr_list_entries * sizeof (struct rtr_item));

	encrypt_and_sign (instance, &iovec, 1);

	/*
	 * Keep an encrypted copy in case the token retransmit timer expires
	 */
	memcpy (instance->orf_token_retransmit, instance->iov_encrypted.iov_base, instance->iov_encrypted.iov_len);
	instance->orf_token_retransmit_size = instance->iov_encrypted.iov_len;

	/*
	 * IF the user doesn't want the token forwarded, then dont send
	 * it but keep an encrypted copy for the retransmit timeout
	 */ 
	if (forward_token == 0) {
		return (0);
	}
	
	/*
	 * Send the message
	 */
	msg_orf_token.msg_name = &instance->next_memb;
	msg_orf_token.msg_namelen = sizeof (struct sockaddr_in);
	msg_orf_token.msg_iov = &instance->iov_encrypted;
	msg_orf_token.msg_iovlen = 1;
	msg_orf_token.msg_control = 0;
	msg_orf_token.msg_controllen = 0;
	msg_orf_token.msg_flags = 0;

	res = sendmsg (instance->totemsrp_sockets[0].token, &msg_orf_token, MSG_NOSIGNAL);
	if (res == -1) {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
			"Couldn't send token to addr %s %s %d\n",
			inet_ntoa (instance->next_memb.sin_addr), 
			strerror (errno), instance->totemsrp_sockets[0].token);
	}
	
	/*
	 * res not used here errors are handled by algorithm
	 */
	if (res > 0) {
		instance->stats_sent += res;
	}

	return (res);
}

static int token_hold_cancel_send (struct totemsrp_instance *instance)
{
	struct token_hold_cancel token_hold_cancel;
	struct iovec iov;
	struct msghdr msghdr;

	/*
	 * Only cancel if the token is currently held
	 */
	if (instance->my_token_held == 0) {
		return (0);
	}
	instance->my_token_held = 0;

	/*
	 * Build message
	 */
	token_hold_cancel.header.type = MESSAGE_TYPE_TOKEN_HOLD_CANCEL;
	token_hold_cancel.header.endian_detector = ENDIAN_LOCAL;
	memcpy (&token_hold_cancel.ring_id, &instance->my_ring_id,
		sizeof (struct memb_ring_id));

	iov.iov_base = &token_hold_cancel;
	iov.iov_len = sizeof (struct token_hold_cancel);

	encrypt_and_sign (instance, &iov, 1);

	/*
	 * Build multicast message
	 */
	msghdr.msg_name = (caddr_t)&instance->sockaddr_in_mcast;
	msghdr.msg_namelen = sizeof (struct sockaddr_in);
	msghdr.msg_iov = &instance->iov_encrypted;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = 0;
	msghdr.msg_controllen = 0;
	msghdr.msg_flags = 0;

	/*
	 * Multicast message
	 */
	sendmsg (instance->totemsrp_sockets[0].mcast, &msghdr, MSG_NOSIGNAL | MSG_DONTWAIT);

	return (0);
}

static int orf_token_send_initial (struct totemsrp_instance *instance)
{
	struct orf_token orf_token;
	int res;

	orf_token.header.type = MESSAGE_TYPE_ORF_TOKEN;
	orf_token.header.endian_detector = ENDIAN_LOCAL;
	orf_token.header.encapsulated = 0;
	orf_token.seq = 0;
	orf_token.token_seq = 0;
	orf_token.retrans_flg = 1;
	instance->my_set_retrans_flg = 1;
/*
	if (queue_is_empty (&instance->retrans_message_queue) == 1) {
		orf_token.retrans_flg = 0;
	} else {
		orf_token.retrans_flg = 1;
		instance->my_set_retrans_flg = 1;
	}
*/
		
	orf_token.aru = 0;
//	orf_token.aru_addr.s_addr = 0;//instance->my_id.sin_addr.s_addr;
	orf_token.aru_addr.s_addr = instance->my_id.sin_addr.s_addr;
	memcpy (&orf_token.ring_id, &instance->my_ring_id, sizeof (struct memb_ring_id));
	orf_token.fcc = 0;

	orf_token.rtr_list_entries = 0;

	res = token_send (instance, &orf_token, 1);

	return (res);
}

static void memb_state_commit_token_update (
	struct totemsrp_instance *instance,
	struct memb_commit_token *memb_commit_token)
{
	int memb_index_this;

	memb_index_this = (memb_commit_token->memb_index + 1) % memb_commit_token->addr_entries;
	memcpy (&memb_commit_token->memb_list[memb_index_this].ring_id,
		&instance->my_old_ring_id, sizeof (struct memb_ring_id));
assert (instance->my_old_ring_id.rep.s_addr != 0);

	memb_commit_token->memb_list[memb_index_this].aru = instance->old_ring_state_aru;
	/*
	 *  TODO high delivered is really instance->my_aru, but with safe this
	 * could change?
	 */
	memb_commit_token->memb_list[memb_index_this].high_delivered = instance->my_high_delivered;
	memb_commit_token->memb_list[memb_index_this].received_flg = instance->my_received_flg;
}

static int memb_state_commit_token_send (struct totemsrp_instance *instance,
	struct memb_commit_token *memb_commit_token)
{
	struct msghdr msghdr;
	struct iovec iovec;
	int res;
	int memb_index_this;
	int memb_index_next;

	memb_commit_token->token_seq++;
	memb_index_this = (memb_commit_token->memb_index + 1) % memb_commit_token->addr_entries;
	memb_index_next = (memb_index_this + 1) % memb_commit_token->addr_entries;
	memb_commit_token->memb_index = memb_index_this;

	iovec.iov_base = memb_commit_token;
	iovec.iov_len = sizeof (struct memb_commit_token);

	encrypt_and_sign (instance, &iovec, 1);

	instance->next_memb.sin_addr.s_addr = memb_commit_token->addr[memb_index_next].s_addr;
	instance->next_memb.sin_family = AF_INET;
	instance->next_memb.sin_port = instance->sockaddr_in_mcast.sin_port;

	msghdr.msg_name = &instance->next_memb;
	msghdr.msg_namelen = sizeof (struct sockaddr_in);
	msghdr.msg_iov = &instance->iov_encrypted;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = 0;
	msghdr.msg_controllen = 0;
	msghdr.msg_flags = 0;

	res = sendmsg (instance->totemsrp_sockets[0].token, &msghdr, MSG_NOSIGNAL | MSG_DONTWAIT);
	return (res);
}

static int memb_lowest_in_config (struct totemsrp_instance *instance)
{
	struct in_addr token_memb[PROCESSOR_COUNT_MAX];
	int token_memb_entries = 0;
	struct in_addr lowest_addr;
	int i;

	lowest_addr.s_addr = 0xFFFFFFFF;

	memb_set_subtract (token_memb, &token_memb_entries,
		instance->my_proc_list, instance->my_proc_list_entries,
		instance->my_failed_list, instance->my_failed_list_entries);

	/*
	 * find representative by searching for smallest identifier
	 */
	for (i = 0; i < token_memb_entries; i++) {
		if (lowest_addr.s_addr > token_memb[i].s_addr) {
			lowest_addr.s_addr = token_memb[i].s_addr;
		}
	}
	return (instance->my_id.sin_addr.s_addr == lowest_addr.s_addr);
}

static void memb_state_commit_token_create (
	struct totemsrp_instance *instance,
	struct memb_commit_token *commit_token)
{
	struct in_addr token_memb[PROCESSOR_COUNT_MAX];
	int token_memb_entries = 0;

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"Creating commit token because I am the rep.\n");

	memb_set_subtract (token_memb, &token_memb_entries,
		instance->my_proc_list, instance->my_proc_list_entries,
		instance->my_failed_list, instance->my_failed_list_entries);

	memset (commit_token, 0, sizeof (struct memb_commit_token));
	commit_token->header.type = MESSAGE_TYPE_MEMB_COMMIT_TOKEN;
	commit_token->header.endian_detector = ENDIAN_LOCAL;
	commit_token->header.encapsulated = 0;

	commit_token->ring_id.rep.s_addr = instance->my_id.sin_addr.s_addr;

	commit_token->ring_id.seq = instance->token_ring_id_seq + 4;
	qsort (token_memb, token_memb_entries, 
		sizeof (struct in_addr), in_addr_compare);
	memcpy (commit_token->addr, token_memb,
		token_memb_entries * sizeof (struct in_addr));
	memset (commit_token->memb_list, 0,
		sizeof (struct memb_commit_token_memb_entry) * PROCESSOR_COUNT_MAX);
	commit_token->memb_index = token_memb_entries - 1;
	commit_token->addr_entries = token_memb_entries;
}

static int memb_join_message_send (struct totemsrp_instance *instance)
{
	struct msghdr msghdr;
	struct iovec iovec;
	struct memb_join memb_join;
	int res;

	memb_join.header.type = MESSAGE_TYPE_MEMB_JOIN;
	memb_join.header.endian_detector = ENDIAN_LOCAL;
	memb_join.header.encapsulated = 0;

	memb_join.ring_seq = instance->my_ring_id.seq;

	memcpy (memb_join.proc_list, instance->my_proc_list,
		instance->my_proc_list_entries * sizeof (struct in_addr));
	memb_join.proc_list_entries = instance->my_proc_list_entries;

	memcpy (memb_join.failed_list, instance->my_failed_list,
		instance->my_failed_list_entries * sizeof (struct in_addr));
	memb_join.failed_list_entries = instance->my_failed_list_entries;
		
	iovec.iov_base = &memb_join;
	iovec.iov_len = sizeof (struct memb_join);

	encrypt_and_sign (instance, &iovec, 1);

	msghdr.msg_name = &instance->sockaddr_in_mcast;
	msghdr.msg_namelen = sizeof (struct sockaddr_in);
	msghdr.msg_iov = &instance->iov_encrypted;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = 0;
	msghdr.msg_controllen = 0;
	msghdr.msg_flags = 0;

	res = sendmsg (instance->totemsrp_sockets[0].mcast, &msghdr, MSG_NOSIGNAL | MSG_DONTWAIT);
	return (res);
}

static int memb_merge_detect_transmit (struct totemsrp_instance *instance) 
{
	struct msghdr msghdr;
	struct iovec iovec;
	struct memb_merge_detect memb_merge_detect;
	int res;

	memb_merge_detect.header.type = MESSAGE_TYPE_MEMB_MERGE_DETECT;
	memb_merge_detect.header.endian_detector = ENDIAN_LOCAL;
	memb_merge_detect.header.encapsulated = 0;
	memcpy (&memb_merge_detect.ring_id, &instance->my_ring_id,
		sizeof (struct memb_ring_id));

	iovec.iov_base = &memb_merge_detect;
	iovec.iov_len = sizeof (struct memb_merge_detect);

	encrypt_and_sign (instance, &iovec, 1);

	msghdr.msg_name = &instance->sockaddr_in_mcast;
	msghdr.msg_namelen = sizeof (struct sockaddr_in);
	msghdr.msg_iov = &instance->iov_encrypted;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = 0;
	msghdr.msg_controllen = 0;
	msghdr.msg_flags = 0;

	res = sendmsg (instance->totemsrp_sockets[0].mcast, &msghdr,
		MSG_NOSIGNAL | MSG_DONTWAIT);

	return (res);
}

static void memb_ring_id_create_or_load (
	struct totemsrp_instance *instance,
	struct memb_ring_id *memb_ring_id)
{
	int fd;
	int res;
	char filename[256];

	sprintf (filename, "/tmp/ringid_%s",
		inet_ntoa (instance->my_id.sin_addr));
	fd = open (filename, O_RDONLY, 0777);
	if (fd > 0) {
		res = read (fd, &memb_ring_id->seq, sizeof (unsigned long long));
		assert (res == sizeof (unsigned long long));
		close (fd);
	} else
	if (fd == -1 && errno == ENOENT) {
		memb_ring_id->seq = 0;
		umask(0);
		fd = open (filename, O_CREAT|O_RDWR, 0777);
		if (fd == -1) {
			printf ("couldn't create file %d %s\n", fd, strerror(errno));
		}
		res = write (fd, &memb_ring_id->seq, sizeof (unsigned long long));
		assert (res == sizeof (unsigned long long));
		close (fd);
	} else {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_warning,
			"Couldn't open %s %s\n", filename, strerror (errno));
	}
	
	memb_ring_id->rep.s_addr = instance->my_id.sin_addr.s_addr;
	assert (memb_ring_id->rep.s_addr);
	instance->token_ring_id_seq = memb_ring_id->seq;
}

static void memb_ring_id_store (
	struct totemsrp_instance *instance,
	struct memb_commit_token *commit_token)
{
	char filename[256];
	int fd;
	int res;

	sprintf (filename, "/tmp/ringid_%s",
		inet_ntoa (instance->my_id.sin_addr));

	fd = open (filename, O_WRONLY, 0777);
	if (fd == -1) {
		fd = open (filename, O_CREAT|O_RDWR, 0777);
	}
	if (fd == -1) {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_warning,
			"Couldn't store new ring id %llx to stable storage (%s)\n",
				commit_token->ring_id.seq, strerror (errno));
		assert (0);
		return;
	}
	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
		"Storing new sequence id for ring %d\n", commit_token->ring_id.seq);
	assert (fd > 0);
	res = write (fd, &commit_token->ring_id.seq, sizeof (unsigned long long));
	assert (res == sizeof (unsigned long long));
	close (fd);
	memcpy (&instance->my_ring_id, &commit_token->ring_id, sizeof (struct memb_ring_id));
	instance->token_ring_id_seq = instance->my_ring_id.seq;
}

void print_stats (totemsrp_handle handle)
{
	struct timeval tv_end;
	struct totemsrp_instance *instance;
	SaAisErrorT error;

	gettimeofday (&tv_end, NULL);
	
	error = saHandleInstanceGet (&totemsrp_instance_database, handle,
		(void *)&instance);
	if (error != SA_OK) {
		return;
	}

	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "Bytes recv %d\n", instance->stats_recv);
	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "Bytes sent %d\n", instance->stats_sent);
	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "Messages delivered %d\n", instance->stats_delv);
	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "Re-Mcasts %d\n", instance->stats_remcasts);
	instance->totemsrp_log_printf (instance->totemsrp_log_level_notice, "Tokens process %d\n", instance->stats_orf_token);

	saHandleInstancePut (&totemsrp_instance_database, handle);
}

int totemsrp_callback_token_create (
	totemsrp_handle handle,
	void **handle_out,
	enum totem_callback_token_type type,
	int delete,
	int (*callback_fn) (enum totem_callback_token_type type, void *),
	void *data)
{
	struct token_callback_instance *callback_handle;
	struct totemsrp_instance *instance;
	SaErrorT error;

	error = saHandleInstanceGet (&totemsrp_instance_database, handle,
		(void *)&instance);
	if (error != SA_OK) {
		goto error_exit;
	}

	callback_handle = (struct token_callback_instance *)malloc (sizeof (struct token_callback_instance));
	if (callback_handle == 0) {
		return (-1);
	}
	*handle_out = (void *)callback_handle;
	list_init (&callback_handle->list);
	callback_handle->callback_fn = callback_fn;
	callback_handle->data = data;
	callback_handle->callback_type = type;
	callback_handle->delete = delete;
	switch (type) {
	case TOTEM_CALLBACK_TOKEN_RECEIVED:
		list_add (&callback_handle->list, &instance->token_callback_received_listhead);
		break;
	case TOTEM_CALLBACK_TOKEN_SENT:
		list_add (&callback_handle->list, &instance->token_callback_sent_listhead);
		break;
	}

	saHandleInstancePut (&totemsrp_instance_database, handle);

error_exit:
	return (0);
}

void totemsrp_callback_token_destroy (totemsrp_handle handle, void **handle_out)
{
	struct token_callback_instance *h;

	if (*handle_out) {
 		h = (struct token_callback_instance *)*handle_out;
		list_del (&h->list);
		free (h);
		h = NULL;
		*handle_out = 0;
	}
}

void totem_callback_token_type (struct totemsrp_instance *instance, void *handle)
{
	struct token_callback_instance *token_callback_instance = (struct token_callback_instance *)handle;

	list_del (&token_callback_instance->list);
	free (token_callback_instance);
}

static void token_callbacks_execute (
	struct totemsrp_instance *instance,
	enum totem_callback_token_type type)
{
	struct list_head *list;
	struct list_head *list_next;
	struct list_head *callback_listhead = 0;
	struct token_callback_instance *token_callback_instance;
	int res;
	int del;

	switch (type) {
	case TOTEM_CALLBACK_TOKEN_RECEIVED:
		callback_listhead = &instance->token_callback_received_listhead;
		break;
	case TOTEM_CALLBACK_TOKEN_SENT:
		callback_listhead = &instance->token_callback_sent_listhead;
		break;
	default:
		assert (0);
	}
	
	for (list = callback_listhead->next; list != callback_listhead;
		list = list_next) {

		token_callback_instance = list_entry (list, struct token_callback_instance, list);

		list_next = list->next;
		del = token_callback_instance->delete;
		if (del == 1) {
			list_del (list);
		}

		res = token_callback_instance->callback_fn (
			token_callback_instance->callback_type,
			token_callback_instance->data);
		/*
		 * This callback failed to execute, try it again on the next token
		 */
		if (res == -1 && del == 1) {
			list_add (list, callback_listhead);
		} else	if (del) {
			free (token_callback_instance);
		}
	}
}

/*
 * Message Handlers
 */

/*
 * message handler called when TOKEN message type received
 */
static int message_handler_orf_token (
	struct totemsrp_instance *instance,
	struct sockaddr_in *system_from,
	struct iovec *iovec,
	int iov_len,
	int bytes_received,
	int endian_conversion_needed)
{
	char token_storage[1500];
	char token_convert[1500];
	struct orf_token *token;
	unsigned int prio = UINT_MAX;
	struct pollfd ufd;
	int nfds;
	struct orf_token *token_ref = (struct orf_token *)iovec->iov_base;
	int transmits_allowed;
	int forward_token;
	int mcasted;
	int last_aru;
	int low_water;

#ifdef GIVEINFO
	struct timeval tv_current;
	struct timeval tv_diff;

	gettimeofday (&tv_current, NULL);
	timersub (&tv_current, &tv_old, &tv_diff);
	memcpy (&tv_old, &tv_current, sizeof (struct timeval));

	if ((((float)tv_diff.tv_usec) / 100.0) > 5.0) {
		printf ("OTHERS %0.4f ms\n", ((float)tv_diff.tv_usec) / 100.0);
	}
#endif

#ifdef RANDOM_DROP
	if (random () % 100 < 10) {
		return (0);
	}
#endif

	/*
	 * Handle merge detection timeout
	 */
	if (token_ref->seq == instance->my_last_seq) {
		start_merge_detect_timeout (instance);
		instance->my_seq_unchanged += 1;
	} else {
		cancel_merge_detect_timeout (instance);
		cancel_token_hold_retransmit_timeout (instance);
		instance->my_seq_unchanged = 0;
	}

	instance->my_last_seq = token_ref->seq;

	assert (bytes_received >= sizeof (struct orf_token));
//	assert (bytes_received == sizeof (struct orf_token) +
//		(sizeof (struct rtr_item) * token_ref->rtr_list_entries);
	/*
	 * Make copy of token and retransmit list in case we have
	 * to flush incoming messages from the kernel queue
	 */
	token = (struct orf_token *)token_storage;
	memcpy (token, iovec->iov_base, sizeof (struct orf_token));
	memcpy (&token->rtr_list[0], iovec->iov_base + sizeof (struct orf_token),
		sizeof (struct rtr_item) * RETRANSMIT_ENTRIES_MAX);

	if (endian_conversion_needed) {
		orf_token_endian_convert (token, (struct orf_token *)token_convert);
		token = (struct orf_token *)token_convert;
	}
	/*
	 * flush incoming queue from kernel
	 */
	do {
		ufd.fd = instance->totemsrp_sockets[0].mcast;
		ufd.events = POLLIN;
		nfds = poll (&ufd, 1, 0);
		if (nfds == 1 && ufd.revents & POLLIN) {
			instance->totemsrp_iov_recv.iov_len = PACKET_SIZE_MAX;
			recv_handler (0, instance->totemsrp_sockets[0].mcast,
				ufd.revents, instance, &prio);
		}
	} while (nfds == 1);

	/*
	 * Determine if we should hold (in reality drop) the token
	 */
	instance->my_token_held = 0;
	if (instance->my_ring_id.rep.s_addr == instance->my_id.sin_addr.s_addr &&
		instance->my_seq_unchanged > SEQNO_UNCHANGED_CONST) {
		instance->my_token_held = 1;
	} else
	if (instance->my_ring_id.rep.s_addr != instance->my_id.sin_addr.s_addr &&
		instance->my_seq_unchanged >= SEQNO_UNCHANGED_CONST) {
		instance->my_token_held = 1;
	}

	/*
	 * Hold onto token when there is no activity on ring and
	 * this processor is the ring rep
	 */
	forward_token = 1;
	if (instance->my_ring_id.rep.s_addr == instance->my_id.sin_addr.s_addr) {
		if (instance->my_token_held) {
			forward_token = 0;
		}
	}

	token_callbacks_execute (instance, TOTEM_CALLBACK_TOKEN_RECEIVED);

	switch (instance->memb_state) {
	case MEMB_STATE_COMMIT:
		 /* Discard token */
		break;

	case MEMB_STATE_OPERATIONAL:
		messages_free (instance, token->aru);
	case MEMB_STATE_GATHER:
		/*
		 * DO NOT add break, we use different free mechanism in recovery state
		 */

	case MEMB_STATE_RECOVERY:
		last_aru = instance->my_last_aru;
		instance->my_last_aru = token->aru;

		/*
		 * Discard tokens from another configuration
		 */
		if (memcmp (&token->ring_id, &instance->my_ring_id,
			sizeof (struct memb_ring_id)) != 0) {

			return (0); /* discard token */
		}

		/*
		 * Discard retransmitted tokens
		 */
		if (instance->my_token_seq >= token->token_seq) {
			reset_token_retransmit_timeout (instance);
			reset_token_timeout (instance);
			return (0); /* discard token */
		}		
		transmits_allowed = 30;
		mcasted = orf_token_rtr (instance, token, &transmits_allowed);

	        if ((last_aru + MISSING_MCAST_WINDOW) < token->seq) {
			transmits_allowed = 0;
		}
		mcasted = orf_token_mcast (instance, token, transmits_allowed, system_from);
		if (instance->my_aru < token->aru ||
			instance->my_id.sin_addr.s_addr == token->aru_addr.s_addr || 
			token->aru_addr.s_addr == 0) {
			
			token->aru = instance->my_aru;
			if (token->aru == token->seq) {
				token->aru_addr.s_addr = 0;
			} else {
				token->aru_addr.s_addr = instance->my_id.sin_addr.s_addr;
			}
		}
		if (token->aru == last_aru && token->aru_addr.s_addr != 0) {
			instance->my_aru_count += 1;
		} else {
			instance->my_aru_count = 0;
		}

		if (instance->my_aru_count > instance->fail_to_recv_const &&
			token->aru_addr.s_addr != instance->my_id.sin_addr.s_addr) {
			
printf ("FAILED TO RECEIVE\n");
// TODO if we fail to receive, it may be possible to end with a gather
// state of proc == failed = 0 entries
			memb_set_merge (&token->aru_addr, 1,
				instance->my_failed_list,
				&instance->my_failed_list_entries);

			ring_state_restore (instance);

			memb_state_gather_enter (instance);
		} else {
			instance->my_token_seq = token->token_seq;
			token->token_seq += 1;

			if (instance->memb_state == MEMB_STATE_RECOVERY) {
				/*
				 * instance->my_aru == instance->my_high_seq_received means this processor
				 * has recovered all messages it can recover
				 * (ie: its retrans queue is empty)
				 */
				low_water = instance->my_aru;
				if (low_water > last_aru) {
					low_water = last_aru;
				}
// TODO is this code right
				if (queue_is_empty (&instance->retrans_message_queue) == 0 ||
					low_water != instance->my_high_seq_received) {

					if (token->retrans_flg == 0) {
						token->retrans_flg = 1;
						instance->my_set_retrans_flg = 1;
					}
				} else
				if (token->retrans_flg == 1 && instance->my_set_retrans_flg) {
					token->retrans_flg = 0;
				}
				instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
					"token retrans flag is %d my set retrans flag%d retrans queue empty %d count %d, low_water %d aru %d\n", 
					token->retrans_flg, instance->my_set_retrans_flg,
					queue_is_empty (&instance->retrans_message_queue),
					instance->my_retrans_flg_count, low_water, token->aru);
				if (token->retrans_flg == 0) { 
					instance->my_retrans_flg_count += 1;
				} else {
					instance->my_retrans_flg_count = 0;
				}
				if (instance->my_retrans_flg_count == 2) {
					instance->my_install_seq = token->seq;
				}
				instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
					"install seq %d aru %d high seq received %d\n",
					instance->my_install_seq, instance->my_aru, instance->my_high_seq_received);
				if (instance->my_retrans_flg_count >= 2 && instance->my_aru >= instance->my_install_seq && instance->my_received_flg == 0) {
					instance->my_received_flg = 1;
					instance->my_deliver_memb_entries = instance->my_trans_memb_entries;
					memcpy (instance->my_deliver_memb_list, instance->my_trans_memb_list,
						sizeof (struct in_addr) * instance->my_trans_memb_entries);
				}
				if (instance->my_retrans_flg_count >= 3 && token->aru >= instance->my_install_seq) {
					instance->my_rotation_counter += 1;
				} else {
					instance->my_rotation_counter = 0;
				}
				if (instance->my_rotation_counter == 2) {
				instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
					"retrans flag count %d token aru %d install seq %d aru %d %d\n",
					instance->my_retrans_flg_count, token->aru, instance->my_install_seq,
					instance->my_aru, token->seq);

					memb_state_operational_enter (instance);
					instance->my_rotation_counter = 0;
					instance->my_retrans_flg_count = 0;
				}
			}
	
#ifdef CONFIG_THREADED_SEND
			worker_thread_group_wait (&instance->worker_thread_group_orf_token_mcast);
#endif

			token_send (instance, token, forward_token); 

#ifdef GIVEINFO
gettimeofday (&tv_current, NULL);
timersub (&tv_current, &tv_old, &tv_diff);
memcpy (&tv_old, &tv_current, sizeof (struct timeval));
if ((((float)tv_diff.tv_usec) / 100.0) > 5.0) {
printf ("I held %0.4f ms\n", ((float)tv_diff.tv_usec) / 100.0);
}
#endif
			if (instance->memb_state == MEMB_STATE_OPERATIONAL) {
				messages_deliver_to_app (instance, 0, instance->my_high_seq_received);
			}

			/*
			 * Deliver messages after token has been transmitted
			 * to improve performance
			 */
			reset_token_timeout (instance); // REVIEWED
			reset_token_retransmit_timeout (instance); // REVIEWED
			if (instance->my_id.sin_addr.s_addr == instance->my_ring_id.rep.s_addr &&
				instance->my_token_held == 1) {

				start_token_hold_retransmit_timeout (instance);
			}

			token_callbacks_execute (instance, TOTEM_CALLBACK_TOKEN_SENT);
		}
		break;
	}
	return (0);
}

static void messages_deliver_to_app (
	struct totemsrp_instance *instance,
	int skip,
	int end_point)
{
    struct sort_queue_item *sort_queue_item_p;
    int i;
    int res;
    struct mcast *mcast;

	instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
		"Delivering %d to %d\n", instance->my_high_delivered + 1,
		end_point);

	/*
	 * Deliver messages in order from rtr queue to pending delivery queue
	 */
	for (i = instance->my_high_delivered + 1; i <= end_point; i++) {
		void *ptr;

		res = sq_item_get (&instance->regular_sort_queue, i, &ptr);
		if (res != 0 && skip) {
printf ("-skipping %d-\n", i);
			instance->my_high_delivered = i;
			continue;
		}

		/*
		 * If hole, stop assembly
		 */
		if (res != 0) {
			break;
		}

		sort_queue_item_p = ptr;

		mcast = sort_queue_item_p->iovec[0].iov_base;
		assert (mcast != (struct mcast *)0xdeadbeef);

// XXX		printf ("[%s.%d-%d]\n", inet_ntoa (mcast->source),
// XXX			mcast->seq, mcast->this_seqno);

		/*
		 * Skip messages not originated in instance->my_deliver_memb
		 */
		if (skip &&
			memb_set_subset (&mcast->source,
				1,
				instance->my_deliver_memb_list,
				instance->my_deliver_memb_entries) == 0) {

printf ("-skipping %d - wrong ip", i);
			instance->my_high_delivered = i;
			continue;
		}
		instance->my_high_delivered = i;

		/*
		 * Message found
		 */
		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
			"Delivering MCAST message with seq %d to pending delivery queue\n",
			mcast->seq);

		/*
		 * Message is locally originated multicast
		 */
	 	if (sort_queue_item_p->iov_len > 1 &&
			sort_queue_item_p->iovec[0].iov_len == sizeof (struct mcast)) {
			instance->totemsrp_deliver_fn (
				mcast->source,
				&sort_queue_item_p->iovec[1],
				sort_queue_item_p->iov_len - 1,
				mcast->header.endian_detector != ENDIAN_LOCAL);
		} else {
			sort_queue_item_p->iovec[0].iov_len -= sizeof (struct mcast);
			sort_queue_item_p->iovec[0].iov_base += sizeof (struct mcast);

			instance->totemsrp_deliver_fn (
				mcast->source,
				sort_queue_item_p->iovec,
				sort_queue_item_p->iov_len,
				mcast->header.endian_detector != ENDIAN_LOCAL);

			sort_queue_item_p->iovec[0].iov_len += sizeof (struct mcast);
			sort_queue_item_p->iovec[0].iov_base -= sizeof (struct mcast);
		}
		instance->stats_delv += 1;
	}

	instance->my_received_flg = 0;
	if (instance->my_aru == instance->my_high_seq_received) {
//		instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
//			"setting received flag to TRUE %d %d\n",
//			instance->my_aru, instance->my_high_seq_received);
		instance->my_received_flg = 1;
	}
}

/*
 * recv message handler called when MCAST message type received
 */
static int message_handler_mcast (
	struct totemsrp_instance *instance,
	struct sockaddr_in *system_from,
	struct iovec *iovec,
	int iov_len,
	int bytes_received,
	int endian_conversion_needed)
{
	struct sort_queue_item sort_queue_item;
	struct sq *sort_queue;
	struct mcast mcast_header;

	
	if (endian_conversion_needed) {
		mcast_endian_convert (iovec[0].iov_base, &mcast_header);
	} else {
		memcpy (&mcast_header, iovec[0].iov_base, sizeof (struct mcast));
	}

	if (mcast_header.header.encapsulated == 1) {
		sort_queue = &instance->recovery_sort_queue;
	} else {
		sort_queue = &instance->regular_sort_queue;
	}
	assert (bytes_received < PACKET_SIZE_MAX);
#ifdef RANDOM_DROP
if (random()%100 < 50) {
	return (0);
}
#endif
	if (system_from->sin_addr.s_addr != instance->my_id.sin_addr.s_addr) {
		cancel_token_retransmit_timeout (instance);
	}

	/*
	 * If the message is foreign execute the switch below
	 */
	if (memcmp (&instance->my_ring_id, &mcast_header.ring_id,
		sizeof (struct memb_ring_id)) != 0) {

		switch (instance->memb_state) {
		case MEMB_STATE_OPERATIONAL:
			memb_set_merge (&system_from->sin_addr, 1,
				instance->my_proc_list, &instance->my_proc_list_entries);
			memb_state_gather_enter (instance);
			break;

		case MEMB_STATE_GATHER:
			if (!memb_set_subset (&system_from->sin_addr,
				1,
				instance->my_proc_list,
				instance->my_proc_list_entries)) {

				memb_set_merge (&system_from->sin_addr, 1,
					instance->my_proc_list, &instance->my_proc_list_entries);
				memb_state_gather_enter (instance);
				return (0);
			}
			break;

		case MEMB_STATE_COMMIT:
			/* discard message */
			break;

		case MEMB_STATE_RECOVERY:
			/* discard message */
			break;
		}
		return (0);
	}

	instance->totemsrp_log_printf (instance->totemsrp_log_level_debug,
		"Received ringid(%s:%lld) seq %d\n",
		inet_ntoa (mcast_header.ring_id.rep),
		mcast_header.ring_id.seq,
		mcast_header.seq);
	/*
	 * Add mcast message to rtr queue if not already in rtr queue
	 * otherwise free io vectors
	 */
	if (bytes_received > 0 && bytes_received < PACKET_SIZE_MAX &&
		instance->my_aru < mcast_header.seq &&
		sq_item_inuse (sort_queue, mcast_header.seq) == 0) {

		/*
		 * Allocate new multicast memory block
		 */
// TODO LEAK
		sort_queue_item.iovec[0].iov_base = malloc (bytes_received);
		if (sort_queue_item.iovec[0].iov_base == 0) {
			return (-1); /* error here is corrected by the algorithm */
		}
		memcpy (sort_queue_item.iovec[0].iov_base, iovec[0].iov_base,
			bytes_received);
		sort_queue_item.iovec[0].iov_len = bytes_received;
		assert (sort_queue_item.iovec[0].iov_len > 0);
		assert (sort_queue_item.iovec[0].iov_len < PACKET_SIZE_MAX);
		sort_queue_item.iov_len = 1;
		
		if (mcast_header.seq > instance->my_high_seq_received) {
			instance->my_high_seq_received = mcast_header.seq;
		}

		sq_item_add (sort_queue, &sort_queue_item, mcast_header.seq);
	}

	if (instance->memb_state == MEMB_STATE_OPERATIONAL) {
		update_aru (instance);
		messages_deliver_to_app (instance, 0, instance->my_high_seq_received);
	}

/* TODO remove from retrans message queue for old ring in recovery state */
	return (0);
}

static int message_handler_memb_merge_detect (
	struct totemsrp_instance *instance,
	struct sockaddr_in *system_from,
	struct iovec *iovec,
	int iov_len,
	int bytes_received,
	int endian_conversion_needed)
{
	struct memb_merge_detect *memb_merge_detect = iovec[0].iov_base;

	/*
	 * do nothing if this is a merge detect from this configuration
	 */
	if (memcmp (&instance->my_ring_id, &memb_merge_detect->ring_id,
		sizeof (struct memb_ring_id)) == 0) {

		return (0);
	}

	/*
	 * Execute merge operation
	 */
	switch (instance->memb_state) {
	case MEMB_STATE_OPERATIONAL:
		memb_set_merge (&system_from->sin_addr, 1,
			instance->my_proc_list, &instance->my_proc_list_entries);
		memb_state_gather_enter (instance);
		break;

	case MEMB_STATE_GATHER:
		if (!memb_set_subset (&system_from->sin_addr,
			1,
			instance->my_proc_list,
			instance->my_proc_list_entries)) {

			memb_set_merge (&system_from->sin_addr, 1,
				instance->my_proc_list, &instance->my_proc_list_entries);
			memb_state_gather_enter (instance);
			return (0);
		}
		break;

	case MEMB_STATE_COMMIT:
		/* do nothing in commit */
		break;

	case MEMB_STATE_RECOVERY:
		/* do nothing in recovery */
		break;
	}
	return (0);
}

static int memb_join_process (
	struct totemsrp_instance *instance,
	struct memb_join *memb_join,
	struct sockaddr_in *system_from)
{
	struct memb_commit_token my_commit_token;

	if (memb_set_equal (memb_join->proc_list,
		memb_join->proc_list_entries,
		instance->my_proc_list,
		instance->my_proc_list_entries) &&

	memb_set_equal (memb_join->failed_list,
		memb_join->failed_list_entries,
		instance->my_failed_list,
		instance->my_failed_list_entries)) {

		memb_consensus_set (instance, &system_from->sin_addr);
	
		if (memb_consensus_agreed (instance) &&
			memb_lowest_in_config (instance)) {

			memb_state_commit_token_create (instance, &my_commit_token);
	
			memb_state_commit_enter (instance, &my_commit_token);
		} else {
			return (0);
		}
	} else
	if (memb_set_subset (memb_join->proc_list,
		memb_join->proc_list_entries,
		instance->my_proc_list,
		instance->my_proc_list_entries) &&

		memb_set_subset (memb_join->failed_list,
		memb_join->failed_list_entries,
		instance->my_failed_list,
		instance->my_failed_list_entries)) {

		return (0);
	} else
	if (memb_set_subset (&system_from->sin_addr, 1,
		instance->my_failed_list, instance->my_failed_list_entries)) {

		return (0);
	} else {
		memb_set_merge (memb_join->proc_list,
			memb_join->proc_list_entries,
			instance->my_proc_list, &instance->my_proc_list_entries);

		if (memb_set_subset (&instance->my_id.sin_addr, 1,
			memb_join->failed_list, memb_join->failed_list_entries)) {

			memb_set_merge (&system_from->sin_addr, 1,
				instance->my_failed_list, &instance->my_failed_list_entries);
		} else {
			memb_set_merge (memb_join->failed_list,
				memb_join->failed_list_entries,
				instance->my_failed_list, &instance->my_failed_list_entries);
		}
		memb_state_gather_enter (instance);
		return (1); /* gather entered */
	}
	return (0); /* gather not entered */
}

static void memb_join_endian_convert (struct memb_join *in, struct memb_join *out)
{
	int i;

	out->header.type = in->header.type;
	out->header.endian_detector = ENDIAN_LOCAL;
	out->proc_list_entries = swab32 (in->proc_list_entries);
	out->failed_list_entries = swab32 (in->failed_list_entries);
	out->ring_seq = swab64 (in->ring_seq);
	for (i = 0; i < out->proc_list_entries; i++) {
		out->proc_list[i].s_addr = in->proc_list[i].s_addr;
	}
	for (i = 0; i < out->failed_list_entries; i++) {
		out->failed_list[i].s_addr = in->failed_list[i].s_addr;
	}
}

static void memb_commit_token_endian_convert (struct memb_commit_token *in, struct memb_commit_token *out)
{
	int i;

	out->header.type = in->header.type;
	out->header.endian_detector = ENDIAN_LOCAL;
	out->token_seq = swab32 (in->token_seq);
	out->ring_id.rep.s_addr = in->ring_id.rep.s_addr;
	out->ring_id.seq = swab64 (in->ring_id.seq);
	out->retrans_flg = swab32 (in->retrans_flg);
	out->memb_index = swab32 (in->memb_index);
	out->addr_entries = swab32 (in->addr_entries);
	for (i = 0; i < out->addr_entries; i++) {
		out->addr[i].s_addr = in->addr[i].s_addr;
		out->memb_list[i].ring_id.rep.s_addr =
			in->memb_list[i].ring_id.rep.s_addr;
		out->memb_list[i].ring_id.seq =
			swab64 (in->memb_list[i].ring_id.seq);
		out->memb_list[i].aru = swab32 (in->memb_list[i].aru);
		out->memb_list[i].high_delivered = swab32 (in->memb_list[i].high_delivered);
		out->memb_list[i].received_flg = swab32 (in->memb_list[i].received_flg);
	}
}

static void orf_token_endian_convert (struct orf_token *in, struct orf_token *out)
{
	int i;

	out->header.type = in->header.type;
	out->header.endian_detector = ENDIAN_LOCAL;
	out->seq = swab32 (in->seq);
	out->token_seq = swab32 (in->token_seq);
	out->aru = swab32 (in->aru);
	out->ring_id.rep.s_addr = in->ring_id.rep.s_addr;
	out->ring_id.seq = swab64 (in->ring_id.seq);
	out->fcc = swab32 (in->fcc);
	out->retrans_flg = swab32 (in->retrans_flg);
	out->rtr_list_entries = swab32 (in->rtr_list_entries);
	for (i = 0; i < out->rtr_list_entries; i++) {
		out->rtr_list[i].ring_id.rep.s_addr = in->rtr_list[i].ring_id.rep.s_addr;
		out->rtr_list[i].ring_id.seq = swab64 (in->rtr_list[i].ring_id.seq);
		out->rtr_list[i].seq = swab32 (in->rtr_list[i].seq);
	}
}

static void mcast_endian_convert (struct mcast *in, struct mcast *out)
{
	out->header.type = in->header.type;
	out->header.endian_detector = ENDIAN_LOCAL;
	out->seq = swab32 (in->seq);
	out->ring_id.rep.s_addr = in->ring_id.rep.s_addr;
	out->ring_id.seq = swab64 (in->ring_id.seq);
	out->source = in->source;
	out->guarantee = in->guarantee;
}

static int message_handler_memb_join (
	struct totemsrp_instance *instance,
	struct sockaddr_in *system_from,
	struct iovec *iovec,
	int iov_len,
	int bytes_received,
	int endian_conversion_needed)
{
	struct memb_join *memb_join;
	struct memb_join memb_join_convert;

	int gather_entered;

	if (endian_conversion_needed) {
		memb_join = &memb_join_convert;
		memb_join_endian_convert (iovec->iov_base, &memb_join_convert);
	} else {
		memb_join = (struct memb_join *)iovec->iov_base;
	}

	if (instance->token_ring_id_seq < memb_join->ring_seq) {
		instance->token_ring_id_seq = memb_join->ring_seq;
	}
	switch (instance->memb_state) {
		case MEMB_STATE_OPERATIONAL:
			gather_entered = memb_join_process (instance,
				memb_join, system_from);
			if (gather_entered == 0) {
				memb_state_gather_enter (instance);
			}
			break;

		case MEMB_STATE_GATHER:
			memb_join_process (instance, memb_join, system_from);
			break;
	
		case MEMB_STATE_COMMIT:
			if (memb_set_subset (&system_from->sin_addr,
				1,
				instance->my_new_memb_list,
				instance->my_new_memb_entries) &&

				memb_join->ring_seq >= instance->my_ring_id.seq) {

				memb_join_process (instance, memb_join, system_from);
				memb_state_gather_enter (instance);
			}
			break;

		case MEMB_STATE_RECOVERY:
			if (memb_set_subset (&system_from->sin_addr,
				1,
				instance->my_new_memb_list,
				instance->my_new_memb_entries) &&

				memb_join->ring_seq >= instance->my_ring_id.seq) {

				ring_state_restore (instance);

				memb_join_process (instance,memb_join,
					system_from);
				memb_state_gather_enter (instance);
			}
			break;
	}
	return (0);
}

static int message_handler_memb_commit_token (
	struct totemsrp_instance *instance,
	struct sockaddr_in *system_from,
	struct iovec *iovec,
	int iov_len,
	int bytes_received,
	int endian_conversion_needed)
{
	struct memb_commit_token memb_commit_token_convert;
	struct memb_commit_token *memb_commit_token;
	struct in_addr sub[PROCESSOR_COUNT_MAX];
	int sub_entries;

	
	if (endian_conversion_needed) {
		memb_commit_token = &memb_commit_token_convert;
		memb_commit_token_endian_convert (iovec->iov_base, memb_commit_token);
	} else {
		memb_commit_token = (struct memb_commit_token *)iovec->iov_base;
	}
	
/* TODO do we need to check for a duplicate token?
	if (memb_commit_token->token_seq > 0 &&
		instance->my_token_seq >= memb_commit_token->token_seq) {

		printf ("already received commit token %d %d\n",
			memb_commit_token->token_seq, instance->my_token_seq);
		return (0);
	}
*/
#ifdef RANDOM_DROP
if (random()%100 < 10) {
	return (0);
}
#endif
	switch (instance->memb_state) {
		case MEMB_STATE_OPERATIONAL:
			/* discard token */
			break;

		case MEMB_STATE_GATHER:
			memb_set_subtract (sub, &sub_entries,
				instance->my_proc_list, instance->my_proc_list_entries,
				instance->my_failed_list, instance->my_failed_list_entries);
			
			if (memb_set_equal (memb_commit_token->addr,
				memb_commit_token->addr_entries,
				sub,
				sub_entries) &&

				memb_commit_token->ring_id.seq > instance->my_ring_id.seq) {

				memb_state_commit_enter (instance, memb_commit_token);
			}
			break;

		case MEMB_STATE_COMMIT:
			if (memcmp (&memb_commit_token->ring_id, &instance->my_ring_id,
				sizeof (struct memb_ring_id)) == 0) {
//			 if (memb_commit_token->ring_id.seq == instance->my_ring_id.seq) {
				memb_state_recovery_enter (instance, memb_commit_token);
			}
			break;

		case MEMB_STATE_RECOVERY:
			instance->totemsrp_log_printf (instance->totemsrp_log_level_notice,
				"Sending initial ORF token\n");

			if (instance->my_id.sin_addr.s_addr == instance->my_ring_id.rep.s_addr) {
				// TODO convert instead of initiate
				orf_token_send_initial (instance);
				reset_token_timeout (instance); // REVIEWED
				reset_token_retransmit_timeout (instance); // REVIEWED
			}
			break;
	}
	return (0);
}

static int message_handler_token_hold_cancel (
	struct totemsrp_instance *instance,
	struct sockaddr_in *system_from,
	struct iovec *iovec,
	int iov_len,
	int bytes_received,
	int endian_conversion_needed)
{
	struct token_hold_cancel *token_hold_cancel = (struct token_hold_cancel *)iovec->iov_base;

	if (memcmp (&token_hold_cancel->ring_id, &instance->my_ring_id,
		sizeof (struct memb_ring_id)) == 0) {

		instance->my_seq_unchanged = 0;
		if (instance->my_ring_id.rep.s_addr == instance->my_id.sin_addr.s_addr) {
			timer_function_token_retransmit_timeout (instance);
		}
	}
	return (0);
}

static int recv_handler (
	poll_handle handle,
	int fd,
	int revents,
	void *data,
	unsigned int *prio)
{
	struct totemsrp_instance *instance = (struct totemsrp_instance *)data;
	struct msghdr msg_recv;
	struct message_header *message_header;
	struct sockaddr_in system_from;
	int bytes_received;
	int res = 0;

	*prio = UINT_MAX;

	/*
	 * Receive datagram
	 */
	msg_recv.msg_name = &system_from;
	msg_recv.msg_namelen = sizeof (struct sockaddr_in);
	msg_recv.msg_iov = &instance->totemsrp_iov_recv;
	msg_recv.msg_iovlen = 1;
	msg_recv.msg_control = 0;
	msg_recv.msg_controllen = 0;
	msg_recv.msg_flags = 0;

	bytes_received = recvmsg (fd, &msg_recv, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (bytes_received == -1) {
		return (0);
	} else {
		instance->stats_recv += bytes_received;
	}
	if (bytes_received < sizeof (struct message_header)) {
		instance->totemsrp_log_printf (instance->totemsrp_log_level_security, "Received message is too short...  ignoring %d.\n", bytes_received);
		return (0);
	}

	message_header = (struct message_header *)msg_recv.msg_iov->iov_base;

	/*
	 * Authenticate and if authenticated, decrypt datagram
	 */
	instance->totemsrp_iov_recv.iov_len = bytes_received;
	res = authenticate_and_decrypt (instance, &instance->totemsrp_iov_recv);
	instance->log_digest = 0;
	if (res == -1) {
		instance->totemsrp_iov_recv.iov_len = PACKET_SIZE_MAX;
		return 0;
	}

	if (instance->stats_tv_start.tv_usec == 0) {
		gettimeofday (&instance->stats_tv_start, NULL);
	}

	/*
	 * Handle incoming message
	 */
	message_header = (struct message_header *)msg_recv.msg_iov[0].iov_base;
	totemsrp_message_handlers.handler_functions[(int)message_header->type] (
		instance,
		&system_from,
		msg_recv.msg_iov,
		msg_recv.msg_iovlen,
		bytes_received,
		message_header->endian_detector != ENDIAN_LOCAL);

	instance->totemsrp_iov_recv.iov_len = PACKET_SIZE_MAX;
	return (0);
}
