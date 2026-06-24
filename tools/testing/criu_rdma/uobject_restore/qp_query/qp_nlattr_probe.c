// SPDX-License-Identifier: GPL-2.0
/*
 * qp_nlattr_probe -- raw NLDEV resource dump probe.
 *
 * Validation of commit "RDMA/nldev: emit RES_SEND_CQN +
 * RES_RECV_CQN on QP dump entries". The iproute2 `rdma`
 * userspace tool predates these attrs, so its decoder
 * surfaces them only as unknown-id pass-through (or, for
 * older versions, drops them silently). To prove the kernel
 * actually emits both ids on the wire, this probe sends an
 * RDMA_NLDEV_CMD_RES_QP_GET dump request directly on a
 * NETLINK_RDMA socket and walks every attribute of every
 * QP entry, printing the (id, len, payload) tuple. The new
 * attrs surface as id=RDMA_NLDEV_ATTR_RES_SEND_CQN /
 * RDMA_NLDEV_ATTR_RES_RECV_CQN with NLA_U32 payloads keying
 * the QP's send_cq / recv_cq restrack ids.
 *
 * Pair with a CQ dump (argv[1] == "cq") to confirm the
 * cross-reference: every SEND_CQN / RECV_CQN value seen on
 * the QP side joins back to a CQ entry's RES_CQN value.
 */

#include <errno.h>
#include <linux/netlink.h>
#include <rdma/rdma_netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Userspace netlink doesn't expose these; the kernel keeps them internal.
 * Inline equivalents -- standard alignment/header math. */
#ifndef NLA_HDRLEN
#define NLA_HDRLEN	((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif
#ifndef NLA_DATA
#define NLA_DATA(nla)	((void *)((char *)(nla) + NLA_HDRLEN))
#endif
#ifndef NLA_OK
#define NLA_OK(nla, len)	\
	((len) >= (int)sizeof(struct nlattr) &&	\
	 (nla)->nla_len >= sizeof(struct nlattr) && \
	 (nla)->nla_len <= (len))
#endif
#ifndef NLA_NEXT
#define NLA_NEXT(nla, len) \
	((len) -= NLA_ALIGN((nla)->nla_len), \
	 (struct nlattr *)((char *)(nla) + NLA_ALIGN((nla)->nla_len)))
#endif

static const char *attr_name(int id)
{
	switch (id) {
	case RDMA_NLDEV_ATTR_DEV_INDEX:		return "DEV_INDEX";
	case RDMA_NLDEV_ATTR_DEV_NAME:		return "DEV_NAME";
	case RDMA_NLDEV_ATTR_PORT_INDEX:	return "PORT_INDEX";
	case RDMA_NLDEV_ATTR_RES_LQPN:		return "RES_LQPN";
	case RDMA_NLDEV_ATTR_RES_PDN:		return "RES_PDN";
	case RDMA_NLDEV_ATTR_RES_CQN:		return "RES_CQN";
	case RDMA_NLDEV_ATTR_RES_HANDLE:	return "RES_HANDLE";
	case RDMA_NLDEV_ATTR_RES_SEND_CQN:	return "RES_SEND_CQN";
	case RDMA_NLDEV_ATTR_RES_RECV_CQN:	return "RES_RECV_CQN";
	case RDMA_NLDEV_ATTR_RES_TYPE:		return "RES_TYPE";
	case RDMA_NLDEV_ATTR_RES_STATE:		return "RES_STATE";
	case RDMA_NLDEV_ATTR_RES_PID:		return "RES_PID";
	case RDMA_NLDEV_ATTR_RES_KERN_NAME:	return "RES_KERN_NAME";
	case RDMA_NLDEV_ATTR_RES_QP_ENTRY:	return "RES_QP_ENTRY";
	case RDMA_NLDEV_ATTR_RES_QP:		return "RES_QP";
	case RDMA_NLDEV_ATTR_RES_CQ_ENTRY:	return "RES_CQ_ENTRY";
	case RDMA_NLDEV_ATTR_RES_CQ:		return "RES_CQ";
	case RDMA_NLDEV_ATTR_RES_CQE:		return "RES_CQE";
	case RDMA_NLDEV_ATTR_RES_CTXN:		return "RES_CTXN";
	case RDMA_NLDEV_ATTR_RES_USECNT:	return "RES_USECNT";
	default:				return "?";
	}
}

static int is_nested(int attr_id)
{
	switch (attr_id) {
	case RDMA_NLDEV_ATTR_RES_QP:
	case RDMA_NLDEV_ATTR_RES_QP_ENTRY:
	case RDMA_NLDEV_ATTR_RES_CQ:
	case RDMA_NLDEV_ATTR_RES_CQ_ENTRY:
	case RDMA_NLDEV_ATTR_RES_SUMMARY:
	case RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY:
		return 1;
	default:
		return 0;
	}
}

static void walk_attrs(const struct nlattr *p, int len, int depth)
{
	while (NLA_OK(p, len)) {
		int t = p->nla_type & NLA_TYPE_MASK;
		int payload_len = p->nla_len - NLA_HDRLEN;

		for (int i = 0; i < depth; i++)
			putchar(' ');
		printf("attr id=%-3d (%-15s) len=%d", t, attr_name(t),
		       payload_len);

		if ((p->nla_type & NLA_F_NESTED) || is_nested(t)) {
			puts(" (NESTED) {");
			walk_attrs((struct nlattr *)NLA_DATA(p), payload_len,
				   depth + 2);
			for (int i = 0; i < depth; i++)
				putchar(' ');
			printf("}\n");
		} else if (payload_len == 4) {
			printf(" u32=%u\n", *(uint32_t *)NLA_DATA(p));
		} else if (payload_len == 8) {
			printf(" u64=%llu\n",
			       *(unsigned long long *)NLA_DATA(p));
		} else if (payload_len == 1) {
			printf(" u8=%u\n", *(uint8_t *)NLA_DATA(p));
		} else if (payload_len > 0 && payload_len < 64) {
			printf(" str=\"");
			const char *d = NLA_DATA(p);

			for (int i = 0; i < payload_len && d[i]; i++)
				putchar(d[i]);
			printf("\"\n");
		} else {
			putchar('\n');
		}
		p = NLA_NEXT(p, len);
	}
}

int main(int argc, char **argv)
{
	int verb = (argc > 1 && !strcmp(argv[1], "cq")) ?
		   RDMA_NLDEV_CMD_RES_CQ_GET :
		   RDMA_NLDEV_CMD_RES_QP_GET;
	uint32_t dev_index = (argc > 2) ? (uint32_t)atoi(argv[2]) : 0;

	int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_RDMA);

	if (sock < 0) {
		perror("socket(NETLINK_RDMA)");
		return 1;
	}

	struct {
		struct nlmsghdr nlh;
		struct nlattr	dev_attr;
		uint32_t	dev_index;
	} __attribute__((packed)) req = {
		.nlh = {
			.nlmsg_len   = sizeof(req),
			.nlmsg_type  = RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, verb),
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
			.nlmsg_seq   = 1,
			.nlmsg_pid   = 0,
		},
		.dev_attr = {
			.nla_len	= sizeof(struct nlattr) + sizeof(uint32_t),
			.nla_type	= RDMA_NLDEV_ATTR_DEV_INDEX,
		},
		.dev_index = dev_index,
	};

	if (send(sock, &req, req.nlh.nlmsg_len, 0) < 0) {
		perror("send");
		return 1;
	}

	char buf[64 * 1024];
	int done = 0;

	while (!done) {
		int n = recv(sock, buf, sizeof(buf), 0);

		if (n <= 0) {
			if (n < 0)
				perror("recv");
			break;
		}
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
		     NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type == NLMSG_DONE) {
				done = 1;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(h);

				fprintf(stderr, "NLMSG_ERROR err=%d\n",
					e->error);
				done = 1;
				break;
			}
			int payload = h->nlmsg_len - NLMSG_HDRLEN;

			puts("--- nlmsg ---");
			walk_attrs((struct nlattr *)NLMSG_DATA(h), payload, 0);
		}
	}
	close(sock);
	return 0;
}
