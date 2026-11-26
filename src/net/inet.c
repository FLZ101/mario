#include <fs/socket.h>
#include <mm/kmalloc.h>
#include <net/inet.h>
#include <net/worker.h>
#include <signal.h>
#include <misc.h>

#include <lwip/netif.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>

#define INET_RX_PACKETS 8
#define INET_BACKLOG 8
#define INET_UDP_MAX 1472
#define INET_STREAM_CHUNK 4096
#define MSG_PEEK 0x02
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

enum inet_state {
	INET_IDLE,
	INET_LISTENING,
	INET_CONNECTING,
	INET_CONNECTED,
	INET_FAILED,
};

struct inet_address {
	uint16_t family;
	uint16_t port;
	uint32_t ip;
	uint8_t padding[8];
};

struct inet_packet {
	struct pbuf *pbuf;
	struct inet_address source;
	uint16_t offset;
};

struct inet_endpoint {
	int type;
	enum inet_state state;
	int error;
	int bound;
	int read_shutdown;
	int write_shutdown;
	int eof;
	union {
		struct tcp_pcb *tcp;
		struct udp_pcb *udp;
	};
	struct inet_address local;
	struct inet_address peer;
	struct inet_packet rx[INET_RX_PACKETS];
	unsigned int rx_head;
	unsigned int rx_count;
	struct inet_endpoint *accept_next;
	struct inet_endpoint *accept_head;
	struct inet_endpoint *accept_tail;
	int accept_count;
	int backlog;
	struct net_work close_work;
	spinlock_t event_lock;
	wait_queue_t events;
	unsigned int generation;
};

enum inet_op {
	INET_OPEN,
	INET_BIND,
	INET_LISTEN,
	INET_CONNECT,
	INET_ACCEPT,
	INET_NAME,
	INET_SHUTDOWN,
	INET_SEND,
	INET_RECEIVE,
};

struct inet_request {
	enum inet_op op;
	struct inet_endpoint *socket;
	struct inet_endpoint *accepted;
	struct inet_address address;
	int option;
	int has_address;
	int started;
	int nonblock;
	unsigned int length;
	char *buffer;
};

static u8_t inet_pcb_arg = LWIP_TCP_PCB_NUM_EXT_ARG_ID_INVALID;

static int inet_error(err_t error)
{
	switch (error) {
	case ERR_OK: return 0;
	case ERR_MEM: return -ENOBUFS;
	case ERR_BUF: return -ENOBUFS;
	case ERR_TIMEOUT: return -ETIMEDOUT;
	case ERR_RTE: return -ENETUNREACH;
	case ERR_USE: return -EADDRINUSE;
	case ERR_ALREADY: return -EALREADY;
	case ERR_ISCONN: return -EISCONN;
	case ERR_CONN: return -ENOTCONN;
	case ERR_ABRT: return -ECONNABORTED;
	case ERR_RST: return -ECONNRESET;
	case ERR_CLSD: return -EPIPE;
	default: return -EINVAL;
	}
}

static struct inet_address inet_address(const ip_addr_t *ip, u16_t port)
{
	return (struct inet_address) {
		.family = AF_INET,
		.port = lwip_htons(port),
		.ip = ip4_addr_get_u32(ip_2_ip4(ip)),
	};
}

static ip_addr_t inet_ip(const struct inet_address *address)
{
	ip_addr_t ip;

	ip4_addr_set_u32(ip_2_ip4(&ip), address->ip);
	return ip;
}

static void inet_event(struct inet_endpoint *socket)
{
	unsigned long flags;

	spin_lock_irqsave(&socket->event_lock, flags);
	__atomic_add_fetch(&socket->generation, 1, __ATOMIC_RELAXED);
	wake_up_interruptible(&socket->events);
	spin_unlock_irqrestore(&socket->event_lock, flags);
}

static struct inet_endpoint *inet_alloc(int type)
{
	struct inet_endpoint *socket = kmalloc(sizeof(*socket));

	if (!socket)
		return NULL;
	memset(socket, 0, sizeof(*socket));
	socket->type = type;
	socket->local.family = AF_INET;
	INIT_LOCK(&socket->event_lock);
	init_wait_queue(&socket->events);
	return socket;
}

static void inet_queue_packet(struct inet_endpoint *socket, struct pbuf *packet,
			      struct inet_address source)
{
	unsigned int tail = (socket->rx_head + socket->rx_count) % INET_RX_PACKETS;

	socket->rx[tail] = (struct inet_packet) { .pbuf = packet, .source = source };
	socket->rx_count++;
	inet_event(socket);
}

static void inet_pop_packet(struct inet_endpoint *socket)
{
	pbuf_free(socket->rx[socket->rx_head].pbuf);
	socket->rx_head = (socket->rx_head + 1) % INET_RX_PACKETS;
	socket->rx_count--;
}

static err_t inet_tcp_receive(void *arg, struct tcp_pcb *pcb,
			      struct pbuf *packet, err_t error)
{
	struct inet_endpoint *socket = arg;

	if (!packet) {
		socket->eof = 1;
		inet_event(socket);
	} else if (socket->read_shutdown) {
		tcp_recved(pcb, packet->tot_len);
		pbuf_free(packet);
	} else {
		if (socket->rx_count == INET_RX_PACKETS)
			return ERR_MEM;
		inet_queue_packet(socket, packet, socket->peer);
	}
	return ERR_OK;
}

static void inet_udp_receive(void *arg, struct udp_pcb *pcb, struct pbuf *packet,
			     const ip_addr_t *ip, u16_t port)
{
	struct inet_endpoint *socket = arg;

	if (socket->read_shutdown || socket->rx_count == INET_RX_PACKETS)
		pbuf_free(packet);
	else
		inet_queue_packet(socket, packet, inet_address(ip, port));
}

static void inet_tcp_destroy(u8_t id, void *arg)
{
	struct inet_endpoint *socket = arg;

	if (socket) {
		socket->tcp = NULL;
		socket->eof = 1;
		inet_event(socket);
	}
}

static const struct tcp_ext_arg_callbacks inet_tcp_ext = {
	.destroy = inet_tcp_destroy,
};

static void inet_tcp_error(void *arg, err_t error)
{
	struct inet_endpoint *socket = arg;

	socket->error = error == ERR_RST && socket->state == INET_CONNECTING ?
		-ECONNREFUSED : inet_error(error);
	socket->state = INET_FAILED;
	inet_event(socket);
}

static err_t inet_tcp_poll(void *arg, struct tcp_pcb *pcb)
{
	inet_event(arg);
	return ERR_OK;
}

static err_t inet_tcp_sent(void *arg, struct tcp_pcb *pcb, u16_t length)
{
	inet_event(arg);
	return ERR_OK;
}

static void inet_tcp_attach(struct inet_endpoint *socket, struct tcp_pcb *pcb)
{
	socket->tcp = pcb;
	tcp_arg(pcb, socket);
	tcp_recv(pcb, inet_tcp_receive);
	tcp_err(pcb, inet_tcp_error);
	tcp_sent(pcb, inet_tcp_sent);
	tcp_poll(pcb, inet_tcp_poll, 1);
	tcp_ext_arg_set_callbacks(pcb, inet_pcb_arg, &inet_tcp_ext);
	tcp_ext_arg_set(pcb, inet_pcb_arg, socket);
}

static void inet_tcp_established(struct inet_endpoint *socket, struct tcp_pcb *pcb)
{
	socket->state = INET_CONNECTED;
	socket->local = inet_address(&pcb->local_ip, pcb->local_port);
	socket->peer = inet_address(&pcb->remote_ip, pcb->remote_port);
}

static err_t inet_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t error)
{
	struct inet_endpoint *socket = arg;

	inet_tcp_established(socket, pcb);
	inet_event(socket);
	return ERR_OK;
}

static err_t inet_tcp_accept(void *arg, struct tcp_pcb *pcb, err_t error)
{
	struct inet_endpoint *listener = arg;
	struct inet_endpoint *socket = NULL;

	if (!pcb)
		return error;
	if (listener->accept_count < listener->backlog)
		socket = inet_alloc(SOCK_STREAM);
	if (!socket) {
		tcp_abort(pcb);
		return ERR_ABRT;
	}
	tcp_backlog_delayed(pcb);
	inet_tcp_attach(socket, pcb);
	inet_tcp_established(socket, pcb);
	if (listener->accept_tail)
		listener->accept_tail->accept_next = socket;
	else
		listener->accept_head = socket;
	listener->accept_tail = socket;
	listener->accept_count++;
	inet_event(listener);
	return ERR_OK;
}

static struct inet_endpoint *inet_take_connection(struct inet_endpoint *listener)
{
	struct inet_endpoint *socket = listener->accept_head;

	if (!(listener->accept_head = socket->accept_next))
		listener->accept_tail = NULL;
	listener->accept_count--;
	if (socket->tcp)
		tcp_backlog_accepted(socket->tcp);
	return socket;
}

static void inet_close_tcp(struct inet_endpoint *socket)
{
	struct tcp_pcb *pcb = socket->tcp;

	if (!pcb)
		return;
	tcp_arg(pcb, NULL);
	tcp_ext_arg_set(pcb, inet_pcb_arg, NULL);
	if (socket->state == INET_LISTENING) {
		tcp_accept(pcb, NULL);
	} else {
		tcp_err(pcb, NULL);
		tcp_sent(pcb, NULL);
		tcp_poll(pcb, NULL, 0);
		tcp_recv(pcb, NULL);
	}
	if (tcp_close(pcb) != ERR_OK)
		tcp_abort(pcb);
}

static void inet_destroy(struct inet_endpoint *socket)
{
	while (socket->accept_head) {
		struct inet_endpoint *child = inet_take_connection(socket);

		inet_close_tcp(child);
		while (child->rx_count)
			inet_pop_packet(child);
		kfree(child);
	}
	if (socket->type == SOCK_STREAM)
		inet_close_tcp(socket);
	else if (socket->udp)
		udp_remove(socket->udp);
	while (socket->rx_count)
		inet_pop_packet(socket);
	kfree(socket);
}

static int inet_open_pcb(struct inet_endpoint *socket)
{
	if (socket->type == SOCK_STREAM) {
		struct tcp_pcb *pcb = tcp_new();

		if (!pcb)
			return -ENOBUFS;
		if (inet_pcb_arg == LWIP_TCP_PCB_NUM_EXT_ARG_ID_INVALID)
			inet_pcb_arg = tcp_ext_arg_alloc_id();
		inet_tcp_attach(socket, pcb);
	} else {
		socket->udp = udp_new();
		if (!socket->udp)
			return -ENOBUFS;
		udp_recv(socket->udp, inet_udp_receive, socket);
	}
	return 0;
}

static int inet_bind_pcb(struct inet_endpoint *socket, struct inet_address *address)
{
	ip_addr_t ip = inet_ip(address);
	struct netif *interface;
	err_t error;

	if (socket->bound || socket->state != INET_IDLE)
		return -EINVAL;
	NETIF_FOREACH(interface) {
		if (ip4_addr_cmp(netif_ip4_addr(interface), ip_2_ip4(&ip)))
			break;
	}
	if (!ip4_addr_isany_val(ip) && !interface)
		return -EADDRNOTAVAIL;
	if (socket->type == SOCK_STREAM) {
		error = tcp_bind(socket->tcp, &ip, lwip_ntohs(address->port));
		if (!error)
			socket->local = inet_address(&socket->tcp->local_ip,
						     socket->tcp->local_port);
	} else {
		error = udp_bind(socket->udp, &ip, lwip_ntohs(address->port));
		if (!error)
			socket->local = inet_address(&socket->udp->local_ip,
						     socket->udp->local_port);
	}
	if (!error)
		socket->bound = 1;
	return inet_error(error);
}

static int inet_listen_pcb(struct inet_endpoint *socket, int backlog)
{
	struct tcp_pcb *pcb;
	err_t error;

	if (socket->type != SOCK_STREAM)
		return -EOPNOTSUPP;
	if (socket->state != INET_IDLE && socket->state != INET_LISTENING)
		return -EINVAL;
	backlog = backlog < 1 ? 1 : (backlog > INET_BACKLOG ? INET_BACKLOG : backlog);
	if (socket->state == INET_LISTENING) {
		tcp_backlog_set(socket->tcp, backlog);
		socket->backlog = backlog;
		return 0;
	}
	if (!socket->bound) {
		struct inet_address address = { .family = AF_INET };
		int result = inet_bind_pcb(socket, &address);

		if (result)
			return result;
	}
	tcp_ext_arg_set(socket->tcp, inet_pcb_arg, NULL);
	pcb = tcp_listen_with_backlog_and_err(socket->tcp, backlog, &error);
	if (!pcb) {
		tcp_ext_arg_set(socket->tcp, inet_pcb_arg, socket);
		return inet_error(error);
	}
	socket->tcp = pcb;
	socket->state = INET_LISTENING;
	socket->backlog = backlog;
	tcp_arg(pcb, socket);
	tcp_accept(pcb, inet_tcp_accept);
	return 0;
}

static int inet_connect_pcb(struct inet_request *request)
{
	struct inet_endpoint *socket = request->socket;
	ip_addr_t ip = inet_ip(&request->address);
	u16_t port = lwip_ntohs(request->address.port);
	err_t error;

	if (socket->type == SOCK_DGRAM) {
		ip_addr_t source;

		if (!ip4_route(ip_2_ip4(&ip)))
			return -ENETUNREACH;
		error = udp_connect(socket->udp, &ip, port);
		if (!error) {
			source = socket->udp->local_ip;
			if (ip4_addr_isany_val(source))
				source = *netif_ip_addr4(ip4_route(ip_2_ip4(&ip)));
			socket->local = inet_address(&source, socket->udp->local_port);
			socket->peer = request->address;
			socket->state = INET_CONNECTED;
			socket->bound = 1;
		}
		return inet_error(error);
	}
	if (socket->state == INET_CONNECTED)
		return request->started ? 0 : -EISCONN;
	if (socket->state == INET_FAILED)
		return socket->error;
	if (socket->state == INET_CONNECTING)
		return request->started ? -EAGAIN : -EALREADY;
	if (socket->state != INET_IDLE)
		return -EINVAL;
	error = tcp_connect(socket->tcp, &ip, port, inet_tcp_connected);
	if (error)
		return inet_error(error);
	socket->state = INET_CONNECTING;
	request->started = 1;
	return request->nonblock ? -EINPROGRESS : -EAGAIN;
}

static int inet_shut_pcb(struct inet_endpoint *socket, int how)
{
	int stop_read = how != SHUT_WR;
	int stop_write = how != SHUT_RD;

	if (socket->state != INET_CONNECTED)
		return -ENOTCONN;
	if (stop_write && !socket->write_shutdown && socket->type == SOCK_STREAM &&
	    socket->tcp) {
		err_t error = tcp_shutdown(socket->tcp, 0, 1);

		if (error)
			return inet_error(error);
	}
	if (stop_write)
		socket->write_shutdown = 1;
	if (stop_read) {
		socket->read_shutdown = 1;
		while (socket->rx_count) {
			struct inet_packet *packet = &socket->rx[socket->rx_head];

			if (socket->type == SOCK_STREAM && socket->tcp)
				tcp_recved(socket->tcp, packet->pbuf->tot_len - packet->offset);
			inet_pop_packet(socket);
		}
	}
	inet_event(socket);
	return 0;
}

static int inet_send_pcb(struct inet_request *request)
{
	struct inet_endpoint *socket = request->socket;
	unsigned int length = request->length;
	err_t error;

	if (socket->write_shutdown)
		return -EPIPE;
	if (socket->type == SOCK_STREAM) {
		if (socket->error)
			return socket->error;
		if (socket->state != INET_CONNECTED || !socket->tcp)
			return -ENOTCONN;
		if (!length)
			return 0;
		if (length > tcp_sndbuf(socket->tcp))
			length = tcp_sndbuf(socket->tcp);
		if (!length)
			return -EAGAIN;
		error = tcp_write(socket->tcp, request->buffer, length, TCP_WRITE_FLAG_COPY);
		if (error)
			return error == ERR_MEM ? -EAGAIN : inet_error(error);
		tcp_output(socket->tcp);
		return length;
	}
	if (!request->has_address && socket->state != INET_CONNECTED)
		return -EDESTADDRREQ;
	struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
	if (!packet)
		return -ENOBUFS;
	if (length)
		pbuf_take(packet, request->buffer, length);
	if (request->has_address) {
		ip_addr_t ip = inet_ip(&request->address);

		error = udp_sendto(socket->udp, packet, &ip, lwip_ntohs(request->address.port));
	} else {
		error = udp_send(socket->udp, packet);
	}
	pbuf_free(packet);
	if (socket->udp->local_port && !socket->bound) {
		socket->bound = 1;
		socket->local = inet_address(&socket->udp->local_ip, socket->udp->local_port);
	}
	return error ? inet_error(error) : (int)length;
}

static int inet_receive_pcb(struct inet_request *request)
{
	struct inet_endpoint *socket = request->socket;
	struct inet_packet *packet;
	unsigned int available;
	unsigned int copied;
	int stream = socket->type == SOCK_STREAM;

	if (stream && socket->state != INET_CONNECTED && socket->state != INET_FAILED)
		return -ENOTCONN;
	if (socket->read_shutdown || (stream && !request->length))
		return 0;
	if (!socket->rx_count)
		return socket->error ? socket->error : (socket->eof ? 0 : -EAGAIN);
	packet = &socket->rx[socket->rx_head];
	available = packet->pbuf->tot_len - packet->offset;
	copied = request->length < available ? request->length : available;
	request->address = packet->source;
	if (copied)
		pbuf_copy_partial(packet->pbuf, request->buffer, copied, packet->offset);
	if (!(request->option & MSG_PEEK)) {
		if (stream && socket->tcp)
			tcp_recved(socket->tcp, copied);
		if (!stream || copied == available)
			inet_pop_packet(socket);
		else
			packet->offset += copied;
	}
	return !stream && (request->option & MSG_TRUNC) ? available : copied;
}

static int inet_run(void *data)
{
	struct inet_request *request = data;
	struct inet_endpoint *socket = request->socket;

	switch (request->op) {
	case INET_OPEN:
		return inet_open_pcb(socket);
	case INET_BIND:
		return inet_bind_pcb(socket, &request->address);
	case INET_LISTEN:
		return inet_listen_pcb(socket, request->option);
	case INET_CONNECT:
		return inet_connect_pcb(request);
	case INET_ACCEPT:
		if (socket->state != INET_LISTENING)
			return -EINVAL;
		if (!socket->accept_head)
			return -EAGAIN;
		request->accepted = inet_take_connection(socket);
		request->address = request->accepted->peer;
		return 0;
	case INET_NAME:
		if (request->option && !socket->peer.family)
			return -ENOTCONN;
		request->address = request->option ? socket->peer : socket->local;
		return 0;
	case INET_SHUTDOWN:
		return inet_shut_pcb(socket, request->option);
	case INET_SEND:
		return inet_send_pcb(request);
	case INET_RECEIVE:
		request->address = socket->peer;
		return inet_receive_pcb(request);
	}
	return -EINVAL;
}

static int inet_exchange(struct inet_request *request)
{
	struct inet_endpoint *socket = request->socket;

	for (;;) {
		unsigned int generation = __atomic_load_n(&socket->generation, __ATOMIC_RELAXED);
		int result = net_call(inet_run, request);

		if (result != -EAGAIN || request->nonblock)
			return result;
		irq_save();
		ACQUIRE_LOCK(&socket->event_lock);
		if (generation != __atomic_load_n(&socket->generation, __ATOMIC_RELAXED)) {
			RELEASE_LOCK(&socket->event_lock);
			irq_restore();
			continue;
		}
		if (signal_pending(current)) {
			RELEASE_LOCK(&socket->event_lock);
			irq_restore();
			return -EINTR;
		}
		sleep_on(&socket->events, TASK_INTERRUPTIBLE, &socket->event_lock);
		irq_restore();
	}
}

static int inet_read_address(const void *user, unsigned int length,
			     struct inet_address *address)
{
	if (length < sizeof(*address))
		return -EINVAL;
	if (verify_area(VERIFY_READ, user, sizeof(*address)))
		return -EFAULT;
	memcpy_fromfs(address, user, sizeof(*address));
	if (address->family != AF_INET)
		return -EAFNOSUPPORT;
	memset(address->padding, 0, sizeof(address->padding));
	return 0;
}

static int inet_address_capacity(void *user, unsigned int *length)
{
	unsigned int capacity;

	if (!user)
		return 0;
	if (verify_area(VERIFY_READ, length, sizeof(*length)) ||
	    verify_area(VERIFY_WRITE, length, sizeof(*length)))
		return -EFAULT;
	capacity = get_fs_long(length);
	if (capacity > 0x7fffffffU)
		return -EINVAL;
	if (capacity > sizeof(struct inet_address))
		capacity = sizeof(struct inet_address);
	if (capacity && verify_area(VERIFY_WRITE, user, capacity))
		return -EFAULT;
	return capacity;
}

static void inet_write_address(void *user, unsigned int *length, int capacity,
			       const struct inet_address *address)
{
	if (user) {
		if (capacity && address->family)
			memcpy_tofs(user, address, capacity);
		put_fs_long(address->family ? sizeof(*address) : 0, length);
	}
}

static void inet_close_work(struct net_work *work)
{
	inet_destroy(list_entry(work, struct inet_endpoint, close_work));
}

static void inet_release_endpoint(struct inet_endpoint *socket)
{
	socket->close_work.run = inet_close_work;
	net_submit(&socket->close_work);
}

static void inet_release(struct inode *inode, struct file *file)
{
	inet_release_endpoint(file->private_data);
}

static int inet_read(struct inode *inode, struct file *file, char *buffer, int count)
{
	return inet_recvfrom(file, buffer, count, 0, NULL, NULL);
}

static int inet_write(struct inode *inode, struct file *file, char *buffer, int count)
{
	return inet_sendto(file, buffer, count, 0, NULL, 0);
}

static int inet_lseek(struct inode *inode, struct file *file, off_t offset, int origin)
{
	return -ESPIPE;
}

struct file_operations inet_fops = {
	.release = inet_release,
	.read = inet_read,
	.write = inet_write,
	.lseek = inet_lseek,
	.share_on_fork = 1,
};

static int inet_install(struct inet_endpoint *socket, int flags)
{
	struct file *file;
	int fd;

	file = get_empty_file();
	if (!file)
		return -ENFILE;
	file->f_inode = get_socket_inode();
	if (!file->f_inode) {
		put_file(file);
		return -ENFILE;
	}
	file->f_mode = 3;
	file->f_flags = O_RDWR | (flags & SOCK_NONBLOCK);
	file->f_op = &inet_fops;
	file->private_data = socket;
	fd = install_fd_file(file, flags & SOCK_CLOEXEC);
	if (fd < 0)
		put_file(file);
	return fd;
}

int inet_socket(int type, int protocol)
{
	int base_type = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
	struct inet_endpoint *socket;
	int result;

	if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM)
		return -ESOCKTNOSUPPORT;
	if (protocol && protocol != (base_type == SOCK_STREAM ? IP_PROTO_TCP : IP_PROTO_UDP))
		return -EPROTONOSUPPORT;
	socket = inet_alloc(base_type);
	if (!socket)
		return -ENOMEM;
	struct inet_request request = { .op = INET_OPEN, .socket = socket };

	result = net_call(inet_run, &request);
	if (result < 0) {
		kfree(socket);
		return result;
	}
	result = inet_install(socket, type);
	if (result < 0)
		inet_release_endpoint(socket);
	return result;
}

int inet_bind(struct file *file, const void *address, unsigned int length)
{
	struct inet_request request = { .op = INET_BIND, .socket = file->private_data };
	int result = inet_read_address(address, length, &request.address);

	return result ? result : net_call(inet_run, &request);
}

int inet_connect(struct file *file, const void *address, unsigned int length)
{
	struct inet_request request = {
		.op = INET_CONNECT, .socket = file->private_data,
		.nonblock = file->f_flags & O_NONBLOCK,
	};
	int result = inet_read_address(address, length, &request.address);

	return result ? result : inet_exchange(&request);
}

int inet_listen(struct file *file, int backlog)
{
	struct inet_request request = {
		.op = INET_LISTEN, .socket = file->private_data, .option = backlog,
	};

	return net_call(inet_run, &request);
}

int inet_accept(struct file *file, void *address, unsigned int *length, int flags)
{
	struct inet_request request = {
		.op = INET_ACCEPT, .socket = file->private_data,
		.nonblock = file->f_flags & O_NONBLOCK,
	};
	int capacity = inet_address_capacity(address, length);
	int result;

	if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
		return -EINVAL;
	if (capacity < 0)
		return capacity;
	result = inet_exchange(&request);
	if (result < 0)
		return result;
	result = inet_install(request.accepted, flags);
	if (result < 0)
		inet_release_endpoint(request.accepted);
	else
		inet_write_address(address, length, capacity, &request.address);
	return result;
}

int inet_name(struct file *file, void *address, unsigned int *length, int peer)
{
	struct inet_request request = {
		.op = INET_NAME, .socket = file->private_data, .option = peer,
	};
	int capacity;
	int result;

	if (!address)
		return -EFAULT;
	capacity = inet_address_capacity(address, length);
	if (capacity < 0)
		return capacity;
	result = net_call(inet_run, &request);
	if (!result)
		inet_write_address(address, length, capacity, &request.address);
	return result;
}

int inet_shutdown(struct file *file, int how)
{
	struct inet_request request = {
		.op = INET_SHUTDOWN, .socket = file->private_data, .option = how,
	};

	if (how < SHUT_RD || how > SHUT_RDWR)
		return -EINVAL;
	return net_call(inet_run, &request);
}

int inet_sendto(struct file *file, const char *buffer, unsigned int length,
		int flags, const void *address, unsigned int address_length)
{
	struct inet_endpoint *socket = file->private_data;
	struct inet_request request = {
		.op = INET_SEND, .socket = socket,
		.nonblock = (file->f_flags & O_NONBLOCK) || (flags & MSG_DONTWAIT),
	};
	int result;

	if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL))
		return -EOPNOTSUPP;
	if (length > 0x7fffffffU)
		return -EINVAL;
	if (socket->type == SOCK_DGRAM) {
		if (length > INET_UDP_MAX)
			return -EMSGSIZE;
		if (address) {
			result = inet_read_address(address, address_length, &request.address);
			if (result)
				return result;
			request.has_address = 1;
		}
	} else if (length > INET_STREAM_CHUNK) {
		length = INET_STREAM_CHUNK;
	}
	request.length = length;
	if (length) {
		if (verify_area(VERIFY_READ, buffer, length))
			return -EFAULT;
		request.buffer = kmalloc(length);
		if (!request.buffer)
			return -ENOMEM;
		memcpy_fromfs(request.buffer, buffer, length);
	}
	result = inet_exchange(&request);
	if (request.buffer)
		kfree(request.buffer);
	return result;
}

int inet_recvfrom(struct file *file, char *buffer, unsigned int length,
		  int flags, void *address, unsigned int *address_length)
{
	struct inet_endpoint *socket = file->private_data;
	struct inet_request request = {
		.op = INET_RECEIVE, .socket = socket, .option = flags,
		.nonblock = (file->f_flags & O_NONBLOCK) || (flags & MSG_DONTWAIT),
	};
	unsigned int limit = socket->type == SOCK_STREAM ? INET_STREAM_CHUNK : INET_UDP_MAX;
	int capacity;
	int result;

	if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC))
		return -EOPNOTSUPP;
	if (length > 0x7fffffffU)
		return -EINVAL;
	if (length > limit)
		length = limit;
	capacity = inet_address_capacity(address, address_length);
	if (capacity < 0)
		return capacity;
	request.length = length;
	if (length) {
		if (verify_area(VERIFY_WRITE, buffer, length))
			return -EFAULT;
		request.buffer = kmalloc(length);
		if (!request.buffer)
			return -ENOMEM;
	}
	result = inet_exchange(&request);
	if (result >= 0) {
		if (result && length)
			memcpy_tofs(buffer, request.buffer, (unsigned int)result < length ? result : length);
		inet_write_address(address, address_length, capacity, &request.address);
	}
	if (request.buffer)
		kfree(request.buffer);
	return result;
}
