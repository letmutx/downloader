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

#define MIN_SIZE (26214400)

struct header {
	int status;
	ssize_t contentlength;
};

struct info {
	int sockfd;
	size_t part_size;
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

struct header *request(int sockfd, char *action, char *domain, char *url, int bytes, long r_start, long r_end) {
	char *msg = NULL;
	asprintf(&msg, "%s %s HTTP/1.0", action, url);
	dprintf(sockfd, "%s\r\n", msg);
	asprintf(&msg, "Host: %s", domain);
	dprintf(sockfd, "%s\r\n", msg);
	if (bytes) {
		asprintf(&msg, "bytes: %ld-%ld", r_start, r_end);
		dprintf(sockfd, "%s\r\n", msg);
	}
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

int download_no_thread(char *domain, char *port, char *url, struct addrinfo *servinfo, FILE *fp) {
	int numbytes, sockfd;
	struct header *hdrs;
	char *buf = malloc(512);

	if ((sockfd = connect_to_server(servinfo)) == -1) {
		free(buf);
		return 0;
	}

	hdrs = request(sockfd, "GET", domain, url, 0, -1, -1);
	
	if (hdrs->status != 200) {
		free(buf);
		free(hdrs);
		return 0;
	}

	while ((numbytes = read(sockfd, buf, 512)) > 0) {
		fwrite(buf, numbytes, 1, fp);
	}

	free(hdrs);
	free(buf);
	return 1;
}

void *download(void *arg) {
	struct sock_fd *s = arg;
	char *buf = malloc(sizeof(1));
	pthread_exit(NULL);
}

int download_manager(char *domain, char *url, struct addrinfo *servinfo, FILE *fp, long contentlength) {
	char *buf;
	pthread_t ids[4];
	long lengths[4];
	long temp = 0;
	int sockfd, start = 0, part = contentlength / 4;
	struct info *t_info;
	for (int i = 0; i < 4; i++) {
		sockfd = connect_to_server(servinfo);
		if (sockfd == -1) {
			return 0;
		}
		t_info = malloc(sizeof(*t_info));
		t_info->sockfd = sockfd;
		
		lengths[i] = t_info->part_size = part + temp;
		pthread_create(ids + i, NULL, download, t_info);
	}

	for (int i = 0; i < 4; i++) {
		pthread_join(ids[i], (void **)&buf);
		if (!buf) {
			return 0;
		}
		else {
			fwrite(buf, lengths[i], 1, fp);
		}
	}
	return 1;
}

int main(int argc, char *argv[]) {
	FILE *fp, *ret;
	int sockfd, rv, t = 0, valid = 0, numbytes = 0;
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

	hdrs = request(sockfd, "HEAD", domain, url, 0, -1, -1);

	if (hdrs->status != 200) {
		fprintf(stderr, "status: %d\n", hdrs->status);
		return 5;
	}

	close(sockfd);

	if (hdrs->contentlength <= MIN_SIZE) {
		rv = download_no_thread(domain, port, url, servinfo, fp);
		if (!rv) {
			fprintf(stderr, "download: failed\n");
		}
	}
	else {
		// start threads		
	}

	freeaddrinfo(servinfo);

	fclose(fp);

	free(buf);
	free(hdrs);
	
	return 0;
}

