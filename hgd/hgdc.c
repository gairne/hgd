#define _GNU_SOURCE	/* linux */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "hgd.h"

char			*user;
char			*host = "127.0.0.1";
int			 port = HGD_DFL_PORT;
int			 sock_fd;

void
hgd_read_config()
{
	/* XXX */
}

void
hgd_exit_nicely()
{
	close(sock_fd);
	exit(EXIT_FAILURE);
}

int
hgd_check_svr_response(char *resp, uint8_t x)
{
	int			len, err = 0;
	char			*trunc = NULL;

	len = strlen(resp);

	if (hgd_debug) {
		trunc = strdup(resp);
		trunc[len - 2] = 0; /* remove \r\n */
		DPRINTF("%s: check reponse '%s'\n", __func__, trunc);
		free(trunc);
	} else
		trunc = trunc; /* silence compiler */

	if (len < 2) {
		fprintf(stderr, "%s: malformed server response\n", __func__);
		err = -1;
	} else if ((resp[0] != 'o') || (resp[1] != 'k')) {
		if (len < 5)
			fprintf(stderr, "%s: malformed server response\n",
			    __func__);
		else
			fprintf(stderr, "%s: failure: %s\n",
			    __func__, &resp[4]);
		err = -1;
	}

	if ((err == -1) && (x))
		hgd_exit_nicely();

	return err;
}

void
hgd_setup_socket()
{
	struct sockaddr_in	addr;
	char			*resp;

	/* set up socket address */
	/* XXX dns */
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(host);
	addr.sin_port = htons(port);

	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0)
		errx(EXIT_FAILURE, "%s: can't make socket", __func__);

	if (connect(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(sock_fd);
		errx(EXIT_FAILURE, "%s: can't connect", __func__);
	}

	/* expect a hello message */
	resp = hgd_sock_recv_line(sock_fd);
	hgd_check_svr_response(resp, 1);
	free(resp);

	DPRINTF("%s: connected to %s\n", __func__, host);
}
void
hgd_usage()
{
	fprintf(stderr, "usage: XXX\n");
}

/* upload and queue a file to the playlist */
#define HGD_BINARY_SEND_CHUNK	(2 << 8)
int
hgd_req_queue(char **args)
{
	FILE			*f;
	struct stat		st;
	ssize_t			written = 0, fsize, chunk_sz;
	char			*chunk = NULL, *filename = args[0];
	char			*q_req, *resp;

	DPRINTF("%s: will queue '%s'\n", __func__, args[0]);

	/* XXX strip filename, so it is just a basename */

	if (stat(filename, &st) < 0) {
		warn("%s: cannot stat '%s'\n", __func__, filename);
		return -1;
	}
	fsize = st.st_size;

	/* send request to upload */
	xasprintf(&q_req, "q|%s|%d", filename, fsize);
	hgd_sock_send_line(sock_fd, q_req);
	free(q_req);

	/* check we are allowed */
	resp = hgd_sock_recv_line(sock_fd);
	if (hgd_check_svr_response(resp, 0) == -1) {
		free(resp);
		return -1;
	}
	free(resp);

	f = fopen(filename, "r");
	if (f == NULL) {
		warn("%s: fopen '%s'", __func__, filename);
		return -1;
	}

	while (written != fsize) {
		if (fsize - written < HGD_BINARY_SEND_CHUNK)
			chunk_sz = fsize - written;
		else
			chunk_sz = HGD_BINARY_SEND_CHUNK;

		if (fread(chunk, chunk_sz, 1, f) != 1) {
			warn("%s: retrying fread", __func__);
			continue;
		}

		hgd_sock_send_bin(sock_fd, chunk, chunk_sz);

		written += chunk_sz;
		DPRINTF("%s: progress %d/%d bytes\n", __func__,
		   (int)  written, (int) fsize);
	}

	resp = hgd_sock_recv_line(sock_fd);
	if (hgd_check_svr_response(resp, 0) == -1) {
		free(resp);
		return -1;
	}

	DPRINTF("%s: transfer complete\n", __func__);

	free(resp);



	return 1;
}

/* lookup for request despatch */
struct hgd_req_despatch req_desps[] = {
	/*{"ls",		0,	hgd_req_playlist}, */
	/*"np",		0,	hgd_req_now_playing}, */
	/*"vote-off",	0,	hgd_req_vote_off}, */
	{"q",		1,	hgd_req_queue},
	{NULL,		0,	NULL} /* terminate */
};

/* parse command line args */
void
hgd_exec_req(int argc, char **argv)
{
	struct hgd_req_despatch	*desp, *correct_desp = NULL;

	for (desp = req_desps; desp->req != NULL; desp++) {
		if (strcmp(desp->req, argv[1]) != 0)
			continue;
		if (argc - 2 != desp->n_args)
			continue;

		correct_desp = desp; /* found it */
		break;
	}

	if (correct_desp == NULL) {
		hgd_usage();
		hgd_exit_nicely();
	}

	DPRINTF("%s: despatching request '%s'\n", __func__, correct_desp->req);
	correct_desp->handler(&argv[2]);
}

int
main(int argc, char **argv)
{
	char			*user_cmd, *resp;

	if (argc < 2)
		errx(EXIT_FAILURE, "%s: implement usage XXX", __func__);

	user = getenv("USER");
	if (user == NULL)
		errx(EXIT_FAILURE, "%s: can't get username", __func__);

	hgd_setup_socket();

	/* identify ourselves */
	xasprintf(&user_cmd, "user|%s", user);
	hgd_sock_send_line(sock_fd, user_cmd);

	resp = hgd_sock_recv_line(sock_fd);
	hgd_check_svr_response(resp, 1);
	free(resp);

	DPRINTF("%s: identified as %s\n", __func__, user);

	/* do whatever the user wants */
	hgd_exec_req(argc, argv);

	/* sign off */
	hgd_sock_send_line(sock_fd, "bye");
	resp = hgd_sock_recv_line(sock_fd);
	hgd_check_svr_response(resp, 1);
	free(resp);

	close(sock_fd);

	exit (EXIT_SUCCESS);
}