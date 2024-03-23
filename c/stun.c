#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include <gio/gio.h>
#include <glib-unix.h>

static bool br_port[4];
static uint32_t br_count = 3;
static int tun_client;

int bridge_get_port(void)
{
	int i;

	for (i = 0; i < br_count; i++) {
		if (!br_port[i]) {
			br_port[i] = true;
			return i;
		}
	}

	return -1;
}

void bridge_put_port(int index)
{
	if (index < 0 || index >= br_count)
		return;

	br_port[index] = false;
}

int tun_alloc(char *dev, bool tap)
{
	int fd, err;
	struct ifreq ifr;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd == -1)
		return -errno;

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = tap ? IFF_TAP : IFF_TUN;
	ifr.ifr_flags |= IFF_NO_PI;

	if (dev[0])
		snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);

	err = ioctl(fd, TUNSETIFF, &ifr);
	if (err == -1)
		return -errno;

	strcpy(dev, ifr.ifr_name);

	return fd;
}

static void listener_event_func(GSocketListener *listener,
				GSocketListenerEvent event,
				GSocket *socket,
				gpointer data)
{
	GSocket **socket_ret = data;

	if (event == G_SOCKET_LISTENER_LISTENED) {
		g_print("%s\n", __func__);
		*socket_ret = g_object_ref(socket);
	}
}

static GSocket *tcp_server_new(uint16_t port)
{
	GInetAddress *iaddr;
	GSocketAddress *saddr;
	GSocketListener *listener;
	GSocket *sock = NULL;
	GError *err = NULL;

	listener = g_socket_listener_new();
	g_signal_connect(listener, "event", G_CALLBACK(listener_event_func), (gpointer)&sock);
	iaddr = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
	saddr = g_inet_socket_address_new(iaddr, port);
	g_socket_listener_add_address(
		listener, saddr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, NULL, &err);

	g_object_unref(saddr);
	g_object_unref(listener);

	g_assert_no_error(err);
	g_assert(sock);

	return sock;
}

struct client_data {
	int bridge_port;

	int tun_fd;

	uint8_t *tun_buf;
	size_t tun_buf_size;

	int net_fd;
	GSocket *net_sock;

	uint8_t *net_buf;
	size_t net_buf_size;
	off_t net_buf_offset;
	size_t net_buf_expect_len;
	int net_buf_expect_type;

	GSource *net_source;
	guint tun_id;

	int free_id;
};

static gboolean client_data_free_func(gpointer data)
{
	struct client_data *client_data = data;

	if (client_data->bridge_port == -1)
		tun_client = 0;

	bridge_put_port(client_data->bridge_port);

	g_source_destroy(client_data->net_source);
	g_source_unref(client_data->net_source);
	g_object_unref(client_data->net_sock);

	if (client_data->tun_id > 0)
		g_source_remove(client_data->tun_id);

	close(client_data->tun_fd);
	g_free(client_data->tun_buf);
	g_free(client_data->net_buf);
	g_free(data);

	return G_SOURCE_REMOVE;
}

void client_data_free(struct client_data *data)
{
	printf("%s %d\n", __func__, __LINE__);
	if (data->free_id > 0)
		return;

	data->free_id = g_idle_add(client_data_free_func, data);
}

/* -errno on error, 0 on EOF, 1 on success */

int readn_generic(int fd, void *buf, off_t *offset, size_t *expect_len)
{
	for (;;) {
		ssize_t len;

		len = read(fd, buf + (*offset), *expect_len);
		if (len == 0)
			return 0;
		else if (len == -1) {
			if (errno == EINTR)
				continue;

			return -errno;
		} else {
			*offset += len;
			*expect_len -= len;

			if (*expect_len == 0)
				return 1;
		}
	}

}

/* -errno on error, 1 on success */

int writen_generic(int fd, const void *buf, off_t *offset, size_t *expect_len)
{
	for (;;) {
		ssize_t len;

		len = write(fd, buf + (*offset), *expect_len);
		if (len == -1) {
			if (errno == EINTR)
				continue;

			return -errno;
		} else {
			*offset += len;
			*expect_len = len;

			if (*expect_len == 0)
				return 1;
		}
	}
}

static gboolean client_net_func(GSocket *sock, GIOCondition cond, gpointer data)
{
	struct client_data *client = data;
	int ret;

next:
	if (!client->net_buf_expect_type) {
		ret = readn_generic(client->net_fd, client->net_buf,
				    &client->net_buf_offset, &client->net_buf_expect_len);
		if (ret <= 0) {
			if (ret == -EAGAIN) {
				return G_SOURCE_CONTINUE;
			}

			printf("net read error %d\n", ret);
			goto error;
		}

		client->net_buf_expect_type = 1;
		client->net_buf_offset = 0;
		client->net_buf_expect_len = ntohs(*(uint16_t *)client->net_buf);
	}

	if (client->net_buf_expect_type) {
		ret = readn_generic(client->net_fd, client->net_buf,
				    &client->net_buf_offset, &client->net_buf_expect_len);
		if (ret <= 0) {
			if (ret == -EAGAIN) {
				return G_SOURCE_CONTINUE;
			}
			printf("net read2 error %d\n", ret);
			goto error;
		}

		ret = write(client->tun_fd, client->net_buf, client->net_buf_offset);
		if (ret <= 0) {
			printf("tun write %d error %d\n", ret, errno);
			goto error;
		}
		else if (ret != client->net_buf_offset)
			printf("warning: tun write %d, expect %d\n", ret, (int)client->net_buf_offset);

		client->net_buf_expect_type = 0;
		client->net_buf_offset = 0;
		client->net_buf_expect_len = 2;

		goto next;
	}

	return G_SOURCE_CONTINUE;

error:
	client_data_free(client);

	return G_SOURCE_REMOVE;
}

static gboolean client_tun_func(gint fd, GIOCondition cond, gpointer data)
{
	ssize_t len;
	struct client_data *client = data;
	uint16_t plen;
	int ret;

	for (;;) {
		len = read(fd, client->tun_buf, client->tun_buf_size);
		if (len <= 0) {
			if (len == -1) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN)
					return G_SOURCE_CONTINUE;

				perror("tun read error");
			}

			goto error;
		}

		plen = htons(len);
		ret = write(client->net_fd, &plen, sizeof(plen));
		if (ret <= 0) {
			printf("net write %d error %d\n", ret, errno);
			goto error;
		}
		else if (ret != sizeof(plen))
			printf("warning: net write %d, exepct %d\n", ret, (int)sizeof(plen));

		ret = write(client->net_fd, client->tun_buf, len);
		if (ret <= 0) {
			printf("net write packet %d error %d\n", ret, errno);
			goto error;
		}
		else if (ret != len)
			printf("warning: net write %d, exepct %d\n", ret, (int)len);
	}

error:
	client->tun_id = 0;
	client_data_free(client);
	return G_SOURCE_REMOVE;
}

struct client_data *client_data_new(GSocket *client, int tun_fd, int port_index)
{
	struct client_data *client_data;
	GSource *source;

	client_data = g_new0(struct client_data, 1);

	client_data->tun_buf = g_new(uint8_t, 4096);
	client_data->tun_buf_size = 4096;

	client_data->net_buf = g_new(uint8_t, 4096);
	client_data->net_buf_size = 4096;
	client_data->net_buf_offset = 0;
	client_data->net_buf_expect_len = 2;
	client_data->net_buf_expect_type = 0;

	source = g_socket_create_source(client, G_IO_IN, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(client_net_func), client_data, NULL);
	g_source_attach(source, NULL);

	client_data->net_source = source;
	client_data->net_sock = client;
	client_data->net_fd = g_socket_get_fd(client);
	client_data->tun_id = g_unix_fd_add(tun_fd, G_IO_IN, client_tun_func, client_data);
	client_data->tun_fd = tun_fd;
	client_data->bridge_port = port_index;

	return client_data;
}

static gboolean server_func(GSocket *sock, GIOCondition cond, gpointer data)
{
	GSocket *client;
	GMainLoop *loop = data;
	GError *err = NULL;
	struct client_data *client_data;
	int tun_fd;
	char name[16];
	int port_index;

	client = g_socket_accept(sock, NULL, &err);
	if (!client) {
		g_print("accept error: %s\n", err->message);
		g_error_free(err);
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}

	port_index = bridge_get_port();

	if (port_index < 0) {
		g_object_unref(client);
		return G_SOURCE_CONTINUE;
	}

	snprintf(name, sizeof(name), "tap%d", port_index);

	tun_fd = tun_alloc(name, true);
	if (tun_fd < 0) {
		g_print("failed to alloc %s\n", name);
		bridge_put_port(port_index);
		g_object_unref(client);
		return G_SOURCE_CONTINUE;
	}

	client_data = client_data_new(client, tun_fd, port_index);

	g_unix_set_fd_nonblocking(client_data->tun_fd, TRUE, NULL);

	return G_SOURCE_CONTINUE;
}

static gboolean tun_server(GSocket *sock, GIOCondition cond, gpointer data)
{
	GSocket *client;
	GMainLoop *loop = data;
	GError *err = NULL;
	struct client_data *client_data;
	int tun_fd;
	char name[16];

	client = g_socket_accept(sock, NULL, &err);
	if (!client) {
		g_print("accept error: %s\n", err->message);
		g_error_free(err);
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}

	if (tun_client > 0) {
		g_object_unref(client);
		return G_SOURCE_CONTINUE;
	}

	snprintf(name, sizeof(name), "tun%d", 0);

	tun_fd = tun_alloc(name, false);
	if (tun_fd < 0) {
		g_print("failed to alloc %s\n", name);
		g_object_unref(client);
		return G_SOURCE_CONTINUE;
	}

	client_data = client_data_new(client, tun_fd, -1);

	g_unix_set_fd_nonblocking(client_data->tun_fd, TRUE, NULL);
	tun_client = 1;

	return G_SOURCE_CONTINUE;
}

GSocket *tcp_client_new(const char *ip, int16_t port)
{
	GSocketAddress *sockaddr;
	GSocket *sock;
	GError *err = NULL;

	sockaddr = g_inet_socket_address_new_from_string(ip, port);
	if (!sockaddr)
		return NULL;

	sock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &err);
	if (!sock) {
		g_print("socket_new: %s\n", err->message);
		g_error_free(err);
		g_object_unref(sockaddr);
		return NULL;
	}

	if (!g_socket_connect(sock, sockaddr, NULL, &err)) {
		g_print("socket_connect: %s\n", err->message);
		g_error_free(err);
		g_object_unref(sock);
		g_object_unref(sockaddr);
		return NULL;
	}

	return sock;
}

struct option_info {
	bool server;

	uint16_t port;
	char *remote_ip;
	bool tun;
};

struct option_info *option_parse(int argc, char *argv[])
{
	bool server = false;
	bool tun = false;
	char *remote_ip = NULL, *port = NULL;
	GOptionContext *context;
	GError *err = NULL;
	bool result;
	long port_id = 0;
	struct option_info *opt;

	GOptionEntry entries[] = {
		{ "server", 's', 0, G_OPTION_ARG_NONE, &server, NULL, NULL },
		{ "connect", 'c', 0, G_OPTION_ARG_STRING, &remote_ip, NULL, NULL },
		{ "port", 'p', 0, G_OPTION_ARG_STRING, &port, NULL, NULL },
		{ "tun", 0, 0, G_OPTION_ARG_NONE, &tun, NULL, NULL },
		{ NULL }
	};

	context = g_option_context_new("Virtual Switch");
	g_option_context_add_main_entries(context, entries, NULL);
	result = g_option_context_parse(context, &argc, &argv, &err);
	g_option_context_free(context);
	if (!result) {
		g_printerr("pase options: %s\n", err->message);
		g_error_free(err);
		return NULL;
	}

	if (server && remote_ip)
		return NULL;

	if (!server && !remote_ip)
		return NULL;

	if (port) {
		bool invalid = false;

		port_id = atoi(port);
		if (port <= 0 || port_id > 65536)
			invalid = true;

		if (invalid) {
			g_free(port);
			if (remote_ip)
				g_free(remote_ip);
			return NULL;
		}

		g_free(port);
		port = NULL;
	}

	opt = g_new(struct option_info, 1);
	opt->server = server;
	opt->remote_ip = remote_ip;
	opt->port = port_id == 0 ? 9500 : port_id;
	opt->tun = tun;

	return opt;
}

int main(int argc, char *argv[])
{
	GMainLoop *loop;
	GSocket *sock;
	GSource *source;
	struct option_info *opt;

	opt = option_parse(argc, argv);
	g_assert(opt);

	loop = g_main_loop_new(NULL, FALSE);

	if (opt->server) {
		sock = tcp_server_new(opt->port);
		g_assert(sock);

		source = g_socket_create_source(sock, G_IO_IN, NULL);
		g_source_set_callback(source, G_SOURCE_FUNC(server_func), loop, NULL);
		g_source_attach(source, NULL);
		g_object_unref(sock);

		sock = tcp_server_new(opt->port + 1);
		g_assert(sock);

		source = g_socket_create_source(sock, G_IO_IN, NULL);
		g_source_set_callback(source, G_SOURCE_FUNC(tun_server), loop, NULL);
		g_source_attach(source, NULL);
		g_object_unref(sock);

	} else {
		char name[16];
		int tun_fd;
		struct client_data *client_data;

		sock = tcp_client_new(opt->remote_ip, opt->port);
		g_assert(sock);

		if (!opt->tun)
			snprintf(name, sizeof(name), "tap%d", 0);
		else
			snprintf(name, sizeof(name), "tun%d", 0);

		tun_fd = tun_alloc(name, !opt->tun);
		if (tun_fd < 0) {
			g_print("failed to alloc %s\n", name);
			g_object_unref(sock);
			exit(1);
		}

		client_data = client_data_new(sock, tun_fd, -1);
		g_unix_set_fd_nonblocking(client_data->tun_fd, TRUE, NULL);
	}

	g_main_loop_run(loop);

	g_main_loop_unref(loop);

	g_free(opt);

	return 0;
}
