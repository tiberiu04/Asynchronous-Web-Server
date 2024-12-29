// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

#define ECHO_LISTEN_PORT		8888
#define MAX_EVENTS 10
#define BIGGER_BUFSIZE 2097153

#define ERROR_MSG															\
	"HTTP/1.1 404 Not Found\r\n"                                           \
	"Server: Neard/9.9.9\r\n"                                              \
	"Accept-Ranges: bytes\r\n"                                             \
	"Content-Length: 0\r\n"                                                \
	"Vary: Accept-Encoding\r\n"                                            \
	"Connection: close\r\n"                                                \
	"Content-Type: text/html\r\n"                                          \
	"\r\n"

#define SUCCESS_MSG                                                                 \
	"HTTP/1.1 200 OK\r\n"                                                  \
	"Server: Neard/9.9.9\r\n"                                              \
	"Accept-Ranges: bytes\r\n"                                             \
	"Content-Length: %ld\r\n"                                              \
	"Vary: Accept-Encoding\r\n"                                            \
	"Connection: close\r\n"                                                \
	"Content-Type: text/html\r\n"                                          \
	"\r\n"

static int listenfd;

static int epollfd;

static io_context_t ctx;

typedef struct {
	const char *path;
	enum resource_type type;
} route_entry;

// Routes
static const route_entry routes[] = {
	{"/static/", RESOURCE_TYPE_STATIC},
	{"/dynamic/", RESOURCE_TYPE_DYNAMIC},
	{NULL, RESOURCE_TYPE_NONE}
};

static inline int add_to_epoll_in(int efd, int fd, struct connection *conn)
{
	int rc = w_epoll_add_ptr_in(efd, fd, conn);

	DIE(rc < 0, "w_epoll_add_ptr_in");
	return rc;
}

static inline int add_to_epoll_out(int efd, int fd, struct connection *conn)
{
	int rc = w_epoll_add_ptr_out(efd, fd, conn);

	DIE(rc < 0, "w_epoll_add_ptr_out");
	return rc;
}

static inline int remove_from_epoll(int efd, int fd, struct connection *conn)
{
	int rc = w_epoll_remove_ptr(efd, fd, conn);

	DIE(rc < 0, "w_epoll_remove_ptr");
	return rc;
}

static inline int switch_to_epoll_in(int efd, int fd, struct connection *conn)
{
	int rc = w_epoll_update_ptr_in(efd, fd, conn);

	DIE(rc < 0, "w_epoll_update_ptr_in");
	return rc;
}

static inline int switch_to_epoll_out(int efd, int fd, struct connection *conn)
{
	int rc = w_epoll_update_ptr_out(efd, fd, conn);

	DIE(rc < 0, "w_epoll_update_ptr_out");
	return rc;
}

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	// error handling
	if (!p || !buf || len == 0) {
		ERR("Error\n");
		return -1;
	}

	struct connection *conn = (struct connection *)p->data;

	if (!conn) {
		ERR("Error\n");
		return -1;
	}

	if (len >= sizeof(conn->request_path)) {
		ERR("Error\n");
		return -1;
	}

	// copying the path into the connection structure
	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';

	conn->have_path = 1;

	return 0;
}

void connection_prepare_send_404(struct connection *conn)
{
	// copying the error message into the send buffer
	memcpy(conn->send_buffer, ERROR_MSG, sizeof(ERROR_MSG));
	conn->send_len = strlen(conn->send_buffer);
	conn->fd = -1;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	struct stat file_stat;
	// getting the file size
	if (fstat(conn->fd, &file_stat) < 0) {
		connection_prepare_send_404(conn);
		return;
	}
	// preparing the success message
	conn->file_pos = 0;
	conn->file_size = file_stat.st_size;
	conn->send_len = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
							  SUCCESS_MSG, (size_t)conn->file_size);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	// checking the path against the routes
	for (int i = 0; routes[i].path != NULL; i++) {
		if (strncmp(conn->request_path, routes[i].path, strlen(routes[i].path)) == 0)
			return routes[i].type;
	}
	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	// allocating memory for the connection structure
	return ({
		struct connection *conn = calloc(1, sizeof(struct connection));

		DIE(!conn, "calloc");
		conn->sockfd = sockfd;
		conn->fd = -1;
		conn;
	});
}

static int initialize_io_context(struct connection *conn)
{
	// initializing the io context
	if (conn->file_pos == 0) {
		if (io_setup(MAX_EVENTS, &conn->ctx) < 0) {
			perror("io_setup");
			return -1;
		}
	}
	return 0;
}

static int prepare_iocb_for_read(struct connection *conn)
{
	// error handling
	if (!conn) {
		ERR("Error: Connection pointer is NULL.\n");
		goto error;
	}

	// setting the eventfd
	io_set_eventfd(&conn->iocb, conn->eventfd);

	// preparing the iocb for read
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);

	// error handling for file not found
	if (conn->fd < 0) {
		ERR("Error: Invalid file descriptor.\n");
		goto error;
	}

	// updating the file position
	conn->file_pos += BUFSIZ;

	return 0;
	// error handling
error:
	return -1;
}

static int submit_iocb(struct connection *conn)
{
	// submitting the iocb
	conn->piocb[0] = &conn->iocb;

	// error handling
	while (io_submit(conn->ctx, 1, conn->piocb) < 0) {
		ERR("io_submit");
		return -1;
	}

	return 0;
}

// Retrying the operation up to retry_limit times to test it.
int retry_operation(int (*operation)(struct connection *), struct connection *conn,
					int retry_limit)
{
	for (int retries = 0; retries < retry_limit; ++retries) {
		if (operation(conn) >= 0)
			return 0; // Success
	}

	return -1; // Failure after retry_limit attempts
}

void connection_start_async_io(struct connection *conn)
{
	const int retry_limit = 1;

	if (!conn)
		return;

	// initializing the io context
	if (retry_operation(initialize_io_context, conn, retry_limit) < 0)
		return;

	// preparing the iocb for read
	if (retry_operation(prepare_iocb_for_read, conn, retry_limit) < 0)
		return;
}

void connection_remove(struct connection *conn)
{
	if (!conn)
		return;

	// removing the connection from the epoll
	remove_from_epoll(epollfd, conn->sockfd, conn);
	// closing the socket
	close(conn->sockfd);
	// closing the file descriptor
	close(conn->fd);
	// freeing the connection and closing it
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void)
{
	// accepting the new connection
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	int newfd = accept(listenfd, (SSA *)&client_addr, &client_len);
	// error handling
	if (newfd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			ERR("accept failed");
		return;
	}

	// setting the socket to non-blocking
	if (fcntl(newfd, F_SETFL, fcntl(newfd, F_GETFL, 0) | O_NONBLOCK) < 0) {
		ERR("fcntl failed");
		close(newfd);
		return;
	}

	// creating a new connection
	struct connection *conn = connection_create(newfd);

	if (!conn) {
		close(newfd);
		ERR("connection_create failed");
		return;
	}

	// adding the new connection to the epoll
	if (add_to_epoll_in(epollfd, newfd, conn) < 0) {
		connection_remove(conn);
		ERR("add_to_epoll_in failed");
		return;
	}

	// initializing the http parser
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
}

void receive_data(struct connection *conn)
{
	off_t total_bytes = 0;
	ssize_t bytes_read;

	// receiving the data
	do {
		bytes_read = recv(conn->sockfd, conn->recv_buffer + total_bytes, sizeof(conn->recv_buffer) - total_bytes, 0);

		// updating the total bytes
		total_bytes += (bytes_read > 0) ? bytes_read : 0;

		// finishing the loop if the bytes read is 0
		if (bytes_read == 0) {
			// exiting the loop
			break;
		} else if (bytes_read < 0) {
			// error handling
			if (errno == EINTR)
				continue;

			perror("recv");
			break;
		}
	} while (total_bytes < sizeof(conn->recv_buffer));

	// updating the receive length
	conn->recv_len = total_bytes;
}

size_t my_strncpy(char *dest, const char *src, size_t size)
{
	size_t src_len = strlen(src);
	size_t copy_len = (src_len >= size) ? size - 1 : src_len;

	if (size > 0) {
		memcpy(dest, src, copy_len);
		dest[copy_len] = '\0';
	}

	return src_len;
}

int connection_open_file(struct connection *conn)
{
	char path[BUFSIZ];
	struct stat buffer;

	// creating the path
	my_strncpy(path, ".", sizeof(path) - 1);
	strncat(path, conn->request_path, sizeof(path) - strlen(path) - 1);

	// opening the file
	conn->fd = open(path, O_RDWR);

	// the file is opened successfully
	if (conn->fd > 0) {
		// getting the file size
		if (fstat(conn->fd, &buffer) == 0) {
			conn->file_size = buffer.st_size;
			conn->state = STATE_SENDING_HEADER;
			conn->res_type = connection_get_resource_type(conn);
		} else {
			// closing the file descriptor
			close(conn->fd);
			// setting the file size to 0, the file descriptor to -1, and the state to 404
			conn->fd = -1;
			conn->file_size = 0;
			conn->state = STATE_SENDING_404;
			conn->res_type = RESOURCE_TYPE_NONE;
		}
	} else {
		// setting the file size to 0, the file descriptor to -1, and the state to 404
		conn->file_size = 0;
		conn->state = STATE_SENDING_404;
		conn->res_type = RESOURCE_TYPE_NONE;
	}

	// returning the file descriptor
	return conn->fd;
}

static int wait_for_async_events(struct connection *conn, struct io_event *events, int min_events, int max_events)
{
	int num_events = 0;
	// waiting for the async events
	num_events = io_getevents(conn->ctx, min_events, max_events, events, NULL);
	return num_events;
}

void connection_complete_async_io(struct connection *conn)
{
	// setting the state to data sent
	conn->state = STATE_DATA_SENT;
}

http_parser_settings initiliaze_setting(void)
{
	http_parser_settings settings;

	settings.on_message_begin = 0;
	settings.on_header_field = 0;
	settings.on_header_value = 0;
	settings.on_path = aws_on_path_cb;
	settings.on_url = 0;
	settings.on_fragment = 0;
	settings.on_query_string = 0;
	settings.on_body = 0;
	settings.on_headers_complete = 0;
	settings.on_message_complete = 0;

	return settings;
}

int parse_header(struct connection *conn)
{
	// initializing the settings
	http_parser_settings settings_on_path = initiliaze_setting();

	conn->request_parser.data = conn;
	// parsing the header
	return http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
}

static int open_file(const char *filename)
{
	// opening the file
	int fd = open(filename, O_RDONLY);

	if (fd == -1)
		ERR("open");

	return fd;
}

static int get_file_size(int fd, struct stat *stat_buf)
{
	// checking if the file descriptor is valid
	if (fd == -1)
		return -1;

	// getting the file size
	if (fstat(fd, stat_buf) == -1) {
		ERR("fstat");
		return -1;
	}
	return 0;
}

int loop_for_sends(struct connection *conn, int fd, off_t *offset, int which_send)
{
	ssize_t bytes_sent = 0;
	ssize_t total_bytes_sent = 0;
	// checking if we use send or senddile
	if (which_send) {
		while (conn->send_len > 0) {
			// sending the file
			bytes_sent = sendfile(conn->sockfd, fd, offset, conn->send_len);
			// checking if the bytes sent is a pozitive number and updating the total bytes sent
			if (bytes_sent > 0) {
				total_bytes_sent += bytes_sent;
				conn->send_len -= bytes_sent;
			}
		}

		return 1;
	}
send_again:
	if (conn->send_len <= 0)
		return 1; // Succes
	// sending the data
	bytes_sent = send(conn->sockfd, conn->send_buffer + total_bytes_sent, conn->send_len, 0);
	// error handling
	if (bytes_sent < 0) {
		perror("send");
		return 0; // Eroare
	}
	// updating the total bytes sent and the send length
	total_bytes_sent += bytes_sent;
	conn->send_len -= bytes_sent;

	goto send_again;

	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	int fd;
	struct stat stat_buf;
	off_t offset = 0;

	// getting the file_path
	char *filename = conn->request_path + 1;

	fd = open_file(filename);
	// getting the file size
	if (get_file_size(fd, &stat_buf) == -1) {
		// opening the file
		if (fd > 0)
			close(fd);
		// setting the state to 404
		return STATE_404_SENT;
	}

	// setting the send length
	conn->send_len = stat_buf.st_size;

	// sending the file
	if (loop_for_sends(conn, fd, &offset, 1)) {
		close(fd);
		return STATE_DATA_SENT;
	}

	return STATE_DATA_SENT;
}

int connection_send_data(struct connection *conn)
{
	// sending the data
	if (loop_for_sends(conn, 0, 0, 0)) {
		switch_to_epoll_in(epollfd, conn->sockfd, conn);
		return 0;
	}

	return 1;
}

static void handle_resource_not_found(struct connection *conn, enum connection_state state)
{
	// preparing the 404 message
	connection_prepare_send_404(conn);
	conn->state = state;
	connection_send_data(conn);
}

int connection_send_dynamic(struct connection *conn)
{
	// sending the dynamic data
	while (1) {
		// checking if the file position is bigger than the buffer size
		if (conn->file_pos >= BIGGER_BUFSIZE)
			return 0;
		// starting the async io
		connection_start_async_io(conn);
		// submitting the iocb
		int rc = submit_iocb(conn);

		DIE(rc < 0, "io_submit");
		struct io_event events[MAX_EVENTS];
		// waiting for the async events
		if (wait_for_async_events(conn, events, 1, MAX_EVENTS) < 0)
			return 0;

		// processing the async events, specifically the first one
		struct io_event *event = &events[0];
		// checking if the result is less than 0
		if (event->res < 0)
			return 0;
		// updating the send length
		conn->send_len = event->res;
		// completing the async io
		connection_complete_async_io(conn);
		// error handling
		if (!conn->send_len)
			return 0;

		// sending the data
		connection_send_data(conn);
	}
}

void handle_input(struct connection *conn)
{
	// receiving message on the socket
	receive_data(conn);

	// parsing the header
	if (parse_header(conn) < 0) {
		// the header couldn't be parsed
		handle_resource_not_found(conn, STATE_SENDING_HEADER);
	} else {
		// the header was parsed successfully
		handle_output(conn);
	}
	// closing the connection
	connection_remove(conn);
}

void handle_output(struct connection *conn)
{
	// checking if the path is valid
	conn->res_type = connection_get_resource_type(conn);

	// checking if the resource type is valid
	if (conn->res_type != RESOURCE_TYPE_STATIC && conn->res_type != RESOURCE_TYPE_DYNAMIC) {
		handle_resource_not_found(conn, STATE_SENDING_HEADER);
		return;
	}

	// checking if the resource type is dynamic and setting the state to sending data
	if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
		conn->state = STATE_SENDING_DATA;
		conn->file_pos = 0;
	}

	// opening the file
	if (connection_open_file(conn) < 0)
		goto file_error;
	else
		goto process_file;

	// error handling
file_error:
	// checking if the resource type is static
	if (conn->res_type == RESOURCE_TYPE_STATIC) {
		// preparing the 404 message
		handle_resource_not_found(conn, STATE_SENDING_HEADER);
	} else {
		// preparing the 404 message
		connection_prepare_send_404(conn);
		connection_send_data(conn);
	}
	return;

process_file:
	// checking if the resource type is static
	if (conn->res_type == RESOURCE_TYPE_STATIC) {
		// setting the state to sending header
		conn->state = STATE_SENDING_HEADER;
		// sending the static data
		connection_prepare_send_reply_header(conn);
		connection_send_data(conn);
		connection_send_static(conn);
	} else {
		// setting the state to sending header and sending the dynamic data
		connection_prepare_send_reply_header(conn);
		connection_send_dynamic(conn);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	// handling the client
	if (event & EPOLLIN) {
		// there is data to read
		handle_input(conn);
	}
}

void create_epoll(void)
{
	// creating the epoll file descriptor
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");
}

void create_listener(void)
{
	// creating the listener socket
	listenfd = tcp_create_listener(ECHO_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");
}

void add_listener_to_epoll(void)
{
	// adding the listener to the epoll
	int rc = w_epoll_add_fd_in(epollfd, listenfd);

	DIE(rc < 0, "w_epoll_add_fd_in");
}

int main(void)
{
	int rc;

	ctx = 0;

	// Creating epoll file descriptor.
	create_epoll();
	// Creating listener socket.
	create_listener();
	// Adding listener socket to epoll.
	add_listener_to_epoll();

	while (1) {
		struct epoll_event rev;
		// Waiting for events.
wait_event:
			rc = w_epoll_wait_infinite(epollfd, &rev);
			// error handling
			if (rc < 0) {
				ERR("w_epoll_wait_infinite");
				DIE(1, "Critical error occurred");
			}
			goto check_event;

check_event:
			// Checking the event.
			if (rev.data.fd == listenfd)
				goto handle_new_conn;
			// Handling the client event.
			goto handle_client_event;

handle_new_conn:
			// Handling new connection.
			handle_new_connection();
			goto wait_event;

handle_client_event:
			// Handling client event.
			handle_client(rev.events, rev.data.ptr);
			goto wait_event;
	}

	return 0;
}
