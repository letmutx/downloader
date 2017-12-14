#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>

#include <arpa/inet.h>

#define MIN_SIZE 26214400

struct header {
	int status;
	ssize_t contentlength;
};

struct data {
	char *domain, *port, *url;
	char *filename;
	struct addrinfo *servinfo;
	long range_start, range_end;
	pthread_t prev_id;
	int valid_prev_id;
	FILE *fp;
};

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
	struct header *h = malloc(sizeof(*h));
	h->contentlength = -1;
	char *line, *token, *saveptr, *delim = ": ";
	static char *contlen = "Content-length";
	static int clen = sizeof("Content-length");
	line = readline(sockfd);
	puts(line);
	token = strtok_r(line, delim, &saveptr);
	token = strtok_r(NULL, delim, &saveptr);
	sscanf(token, "%d", &(h->status));
	while ((line = readline(sockfd))) {
		puts(line);
		token = strtok_r(line, delim, &saveptr);
		if (!strncasecmp(token, contlen, clen)) {
			token = strtok_r(NULL, delim, &saveptr);
			sscanf(token, "%ld", &(h->contentlength));
			free(line);
			break;
		}
		free(line);
	}
	while (line = readline(sockfd)) {
		puts(line);
		free(line);
	}
	return h;
}

struct header *request(int sockfd, char *action, char *domain, char *url) {
	char *msg = NULL;
	asprintf(&msg, "%s %s HTTP/1.0", action, url);
	dprintf(sockfd, "%s\r\n", msg);
	asprintf(&msg, "Host: %s", domain);
	dprintf(sockfd, "%s\r\n", msg);
	dprintf(sockfd, "\r\n");

	free(msg);

	return parse_response(sockfd);
}

int connect_to_server(struct addrinfo *servinfo) {
	int sockfd;
	struct addrinfo *servinfo_iter;

	for (servinfo_iter = servinfo; servinfo_iter != NULL; servinfo_iter = servinfo_iter->ai_next) {
		sockfd = socket(servinfo_iter->ai_family, servinfo_iter->ai_socktype, servinfo_iter->ai_protocol);

		if (sockfd == -1) {
			fprintf(stderr, "socket: failed to create a socket\n");
			continue;
		}

		if (connect(sockfd, servinfo_iter->ai_addr,
				servinfo_iter->ai_addrlen) != 0) {
			close(sockfd);
			fprintf(stderr, "connect: failed to connect\n");
			continue;
		}
	}
	return sockfd;
}

struct data *construct_data_for_thread(char *domain, char *port, char *url, char *filename,
		struct addrinfo *servinfo, long range_start, long range_end, pthread_t prev_id, int valid) {
	struct data *d = malloc(sizeof(*d));

	d->domain = domain;
	d->port = port;
	d->url = url;
	d->filename = filename;
	d->servinfo = servinfo;
	d->range_start = range_start;
	d->range_end = range_end;
	d->prev_id = prev_id;
	d->valid_prev_id = valid;
	return d;
}

void *download(void *arg) {
	struct data *dl = arg;
	pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
	FILE *fp;
	int sockfd, rv, t = 0, valid = 0, numbytes = 0;
	pthread_t id, prev_id;
	char *domain, *port, *url, *filename, *buf = NULL, *cur;
	struct addrinfo hints, *servinfo, *working;
	struct header *hdrs;

	if (argc != 5) {
		fprintf(stderr, "usage: dl domain port url filename\n");
		return 1;
	}

	domain = argv[1];
	port = argv[2];
	url = argv[3];
	filename = argv[4];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (t < 10 && (rv = getaddrinfo(domain, port, &hints, &servinfo))) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		usleep(250);
		t++;
	}

	if (rv) {
		fprintf(stderr, "getaddrinfo: failed to resolve host\n");
		return 2;
	}
	
	if ((sockfd = connect_to_server(servinfo)) == -1) {
		fprintf(stderr, "connect_to_server: failed to connect\n");
		return 3;
	}

	// TODO: send a request with head
	// read content length, status
	// if status not 200, exit.
	// if content length not found, download the file in single thread
	// else if filesize less than 25MB, download the file in single thread
	// else split the download into 4 equal parts and start threads to download
	// join each thread and join main thread with the last thread and close the file in the main thread
	fp = fopen(filename, "w");

	if (!fp) {
		fprintf(stderr, "fopen: cannot open file\n");
		return 4;
	}

	hdrs = request(sockfd, "HEAD", domain, url);

	if (hdrs->status != 200) {
		fprintf(stderr, "status: %d\n", hdrs->status);
		return 5;
	}

	if (hdrs->contentlength != -1) {
		fprintf(stdout, "Content-Length: %ld\n", hdrs->contentlength);
	}

	buf = malloc(hdrs->contentlength + 1);
	cur = buf;

	while ((numbytes = read(sockfd, cur, 512)) > 0) {
		cur += numbytes;
	}
	*cur = 0;

	fwrite(buf, hdrs->contentlength, 1, fp);

	close(sockfd);
	freeaddrinfo(servinfo);

	fclose(fp);

	free(buf);
	free(hdrs);
	
	return 0;
}

