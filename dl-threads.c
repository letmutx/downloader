#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>

#define MAX_SIZE (5 * 1024 * 1024)

struct header {
	int status;
	ssize_t contentlength;
};

char *readline(int sockfd);
struct header *parse_response(int sockfd);
char *errortostr(int status);
int get_address(char *domain, char *port, struct addrinfo **server);
int connect_to(struct addrinfo *server);
int download_manager(char *domain, char *port, char *url, char *filename);
struct header *request_full(int sockfd, char *action, char *domain, char *url);
struct header *request_range(int sockfd, char *action, char *domain, char *url, long start, long end);
int download_without_threads(char *domain, char *url, struct addrinfo *servinfo, FILE *fp);
int thread_manager(char *domain, char *url, struct addrinfo *servinfo, FILE *fp, long contentlength);

int main(int argc, char *argv[]) {
	int rv;
	char *filename, *domain, *port, *url;

	if (argc != 5) {
		fprintf(stderr, "usage: dl domain port url filename\n");
		return 1;
	}

	domain = argv[1];
	port = argv[2];
	url = argv[3];
	filename = argv[4];

	if ((rv = download_manager(domain, port, url, filename))) {
		fprintf(stderr, "download: %s\n", errortostr(rv));
		return rv;
	}

	return 0;
}

int download_without_threads(char *domain, char *url, struct addrinfo *servinfo, FILE *fp) {
	int numbytes, sockfd;
	struct header *hdrs;
	char *buf = malloc(512);

	sockfd = connect_to(servinfo);

	if (sockfd == -1) {
		free(buf);
		return 3;
	}

	hdrs = request_full(sockfd, "GET", domain, url);

	if (hdrs->status != 200) {
		fprintf(stderr, "status: %d\n", hdrs->status);
		free(buf);
		free(hdrs);
		return 4;
	}

	while ((numbytes = read(sockfd, buf, 512)) > 0) {
		fwrite(buf, numbytes, 1, fp);
	}

	free(hdrs);
	free(buf);
	return 1;
}

struct threaddata {
	char *domain;
	char *url;
	struct addrinfo *server;
	long start;
	long end;
};

struct threadreturn {
	char *buf;
	long length;
};

struct threaddata *create_data(char *domain, char *url, struct addrinfo *server, long start, long end) {
	struct threaddata *td = malloc(sizeof(*td));
	td->domain = domain;
	td->url = url;
	td->server = server;
	td->start = start;
	td->end = end;
	return td;
}

void *download(void *arg) {
	int sockfd, numbytes = 0, nb;
	struct threaddata *td = arg;
	struct header *hdrs;
	struct threadreturn *tr;
	char *buf, *cur;

	sockfd = connect_to(td->server);

	if (sockfd == -1) {
		free(td);
		pthread_exit(NULL);
	}

	hdrs = request_range(sockfd, "GET", td->domain, td->url, td->start, td->end);

	if (hdrs->status != 206) {
		fprintf(stderr, "status: %d\n", hdrs->status);
		free(hdrs);
		free(td);
		close(sockfd);
		pthread_exit(NULL);
	}

	buf = malloc(td->end - td->start + 1);
	cur = buf;

	free(hdrs);
	free(td);

	tr = malloc(sizeof(*tr));
	tr->buf = buf;
	tr->length = td->end - td->start + 1;

	while (numbytes < tr->length) {
		nb = read(sockfd, cur, 512);
		if (nb < 0) {
			free(buf);
			free(tr);
			close(sockfd);
			pthread_exit(NULL);
		}
		cur += nb;
		numbytes += nb;
	}

	close(sockfd);

	pthread_exit(tr);
}

struct header *request_range(int sockfd, char *action, char *domain, char *url, long start, long end) {
	char *msg = NULL;

	asprintf(&msg, "%s %s HTTP/1.1", action, url);
	dprintf(sockfd, "%s\r\n", msg);
	free(msg);

	asprintf(&msg, "Host: %s", domain);
	dprintf(sockfd, "%s\r\n", msg);
	free(msg);

	asprintf(&msg, "Range: bytes=%ld-%ld", start, end);
	dprintf(sockfd, "%s\r\n", msg);
	free(msg);

	dprintf(sockfd, "%s\r\n", "Connection: close");
	dprintf(sockfd, "\r\n");

	free(msg);
	return parse_response(sockfd);
}

int thread_manager(char *domain, char *url, struct addrinfo *server, FILE *fp, long contentlength) {
	int failed = 0;
	struct threaddata *td;
	struct threadreturn *tr;
	long start = 0, part = contentlength / 4;
	pthread_t ids[4];

	td = create_data(domain, url, server, 0, part);
	pthread_create(ids, NULL, download, td); 
	td = create_data(domain, url, server, part + 1, 2 * part);
	pthread_create(ids + 1, NULL, download, td);
	td = create_data(domain, url, server, 2 * part + 1, 3 * part);
	pthread_create(ids + 2, NULL, download, td);
	td = create_data(domain, url, server, 3 * part + 1, contentlength - 1);
	pthread_create(ids + 3, NULL, download, td);

	for (int i = 0; i < 4; i++) {
		if (pthread_join(ids[i], (void **)&tr)) {
			fprintf(stderr, "pthread_join: failed\n");;
			for (; i < 4; i++) {
				pthread_cancel(ids[i]);
			}
			return 6;
		}
		if (!tr) {
			failed = 1;
			continue;
		}
		if (!failed) {
			fprintf(stdout, "written: %ld\n", tr->length);
			fwrite(tr->buf, tr->length, 1, fp);
		}
		free(tr->buf);
		free(tr);
	}
	return failed == 0;
}

int download_manager(char *domain, char *port, char *url, char *filename) {
	int sockfd, rv;
	struct addrinfo *server;
	struct header *response_hdrs;

	FILE *out = fopen(filename, "w");

	if (!out) {
		return 1;
	}

	rv = get_address(domain, port, &server);

	if (rv) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 2;
	}

	sockfd = connect_to(server);

	if (sockfd == -1) {
		freeaddrinfo(server);
		return 3;
	}

	response_hdrs = request_full(sockfd, "HEAD", domain, url);

	if (!response_hdrs) {
		return 6;
	}

	if (response_hdrs->status != 200) {
		fprintf(stderr, "status: %d\n", response_hdrs->status);
		freeaddrinfo(server);
		free(response_hdrs);
		return 4;
	}

	if (response_hdrs->contentlength <= MAX_SIZE) {
		rv = download_without_threads(domain, url, server, out);
	}
	else {
		rv = thread_manager(domain, url, server, out, response_hdrs->contentlength);
	}

	close(sockfd);
	freeaddrinfo(server);
	free(response_hdrs);
	fclose(out);

	return rv ? 0 : 5;
}

int connect_to(struct addrinfo *server) {
	int sockfd;
	struct addrinfo *serv_iter;

	for (serv_iter = server; serv_iter != NULL; serv_iter = serv_iter->ai_next) {
		sockfd = socket(serv_iter->ai_family, serv_iter->ai_socktype, serv_iter->ai_protocol);

		if (sockfd == -1) {
			fprintf(stderr, "socket: failed to create a socket\n");
			continue;
		}

		if (connect(sockfd, serv_iter->ai_addr,
					serv_iter->ai_addrlen) != 0) {
			close(sockfd);
			fprintf(stderr, "connect: failed to connect\n");
			continue;
		}
		break;
	}
	return serv_iter ? sockfd : -1;
}

int get_address(char *domain, char *port, struct addrinfo **server) {
	int rv;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rv = getaddrinfo(domain, port, &hints, server);

	return rv;
}

char *errortostr(int status) {
	switch(status) {
		case 1:
			return "File cannot be opened!";
		case 2:
			return "Resolving the address failed!";
		case 3:
			return "creating a socket failed!";
		case 4:
			return "HTTP response status not 200!";
		case 5:
			return "Error downloading!";
		case 6:
			return "Empty response!";
		default:
			return "";
	}
}

char *readline(int sockfd) {
	int newline = 0;
	char cur;
	long size = 200, i = 0;
	char *line = malloc(size);

	while (read(sockfd, &cur, 1) > 0) {
		if (cur == '\r') {
			newline = 1;
		}
		else if (newline && cur == '\n') {
			newline = 0;
			break;
		}
		else {
			line[i++] = cur;
			newline = 0;
		}

		if (i == size) {
			size += 200;
			line = realloc(line, size);
		}
	}
	line[i] = 0;
	line = realloc(line, i ? i + 1 : i);
	return line;
}

struct header *parse_response(int sockfd) {
	struct header *h;
	char *line, *token, *saveptr, *delim = ": ";

	char *contlen = "Content-Length";
	int clen = sizeof("Content-Length");


	line = readline(sockfd);

	if (!line) {
		return NULL;
	}

	h = malloc(sizeof(*h));
	memset(h, 0, sizeof(*h));
	h->contentlength = -1;

	token = strtok_r(line, delim, &saveptr);
	token = strtok_r(NULL, delim, &saveptr);
	sscanf(token, "%d", &(h->status));
	free(line);

	// get past content length
	while ((line = readline(sockfd))) {
		token = strtok_r(line, delim, &saveptr);

		if (!strncasecmp(token, contlen, clen)) {
			token = strtok_r(NULL, delim, &saveptr);
			sscanf(token, "%ld", &(h->contentlength));

			free(line);
			break;
		}

		free(line);
	}

	// get past all headers
	while (line = readline(sockfd)) {
		free(line);
	}

	return h;
}

struct header *request_full(int sockfd, char *action, char *domain, char *url) {
	char *msg = NULL;

	asprintf(&msg, "%s %s HTTP/1.1", action, url);
	dprintf(sockfd, "%s\r\n", msg);
	free(msg);

	asprintf(&msg, "Host: %s", domain);
	dprintf(sockfd, "%s\r\n", msg);
	free(msg);

	dprintf(sockfd, "%s\r\n", "Connection: close");

	dprintf(sockfd, "\r\n");

	free(msg);

	return parse_response(sockfd);
}
