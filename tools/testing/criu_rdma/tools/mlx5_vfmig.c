// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig - tiny userspace helper for the /dev/mlx5_vfmig/<bdf> cdev.
 * Intended for development and triage; production users should speak the
 * ioctls from a real CRIU plugin.
 *
 * Build via the directory Makefile:
 *   make -C tools/testing/criu_rdma tools/mlx5_vfmig
 *
 * Use:
 *   mlx5_vfmig <pf-bdf> mark_restored    <vf_id> [defer_resume]
 *   mlx5_vfmig <pf-bdf> get_vhca_id      <vf_id>
 *   mlx5_vfmig <pf-bdf> query_vf         <vf_id>
 *   mlx5_vfmig <pf-bdf> list
 *   mlx5_vfmig <pf-bdf> load_vhca_state  <vf_id> <blob_path>
 *   mlx5_vfmig <pf-bdf> save_vhca_state  <vf_id> <blob_path> [keep_suspended]
 *   mlx5_vfmig <pf-bdf> suspend_vhca     <vf_id>
 *   mlx5_vfmig <pf-bdf> resume_vhca      <vf_id>
 *   mlx5_vfmig <pf-bdf> enable_migratable <vf_id>
 *   mlx5_vfmig <pf-bdf> set_vf_uuid       <vf_id> <uuid-string>
 *   mlx5_vfmig <pf-bdf> query_qp          <vf_id> <qpn>
 *
 * Verbs accept either '_' or '-' between words.
 *
 * Examples:
 *   mlx5_vfmig 0000:00:08.0 list
 *   mlx5_vfmig 0000:00:08.0 save_vhca_state 0 /tmp/vf.blob
 *   mlx5_vfmig 0000:00:08.0 load_vhca_state 0 /tmp/vf.blob
 *   mlx5_vfmig 0000:00:08.0 mark_restored 0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../../include/uapi/linux/mlx5_vfmig.h"

static int do_enable_migratable(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_enable_migratable arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, &arg) < 0) {
		perror("ENABLE_MIGRATABLE");
		return 1;
	}
	printf("vf %u: migratable cap enabled (call before driver bind)\n",
	       vf_id);
	return 0;
}

/*
 * EXPERIMENTAL: probe the firmware's per-VHCA uctx-id allocator on a
 * bound VF by issuing CREATE_UCTX + immediate DESTROY_UCTX from the
 * PF. Used to answer "did LOAD_VHCA_STATE preserve the source's
 * uctx-id space?" empirically. See
 * include/uapi/linux/mlx5_vfmig.h for methodology.
 */
static int do_probe_uid(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_probe_uid arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_UID, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core (PROBE_UID requires the VF mdev to be interface-up)\n",
				vf_id);
		else
			perror("PROBE_UID");
		return 1;
	}
	printf("vf %u: probe_uid -> uid=%u\n", vf_id, arg.uid);
	return 0;
}

/*
 * EXPERIMENTAL: raw FW QUERY_QP on a bound VF's mdev.
 * Used by the §6.3 piggyback experiment in
 * design/uobject_restore.md to confirm pending RQ WRs and QP state
 * survive LOAD_VHCA_STATE intrinsically.
 *
 * Also used by the §S6b stale-dmac diagnostic
 * (design/qp_av_dmac_swap.md): the decoded `av_dmac` / `av_dgid` /
 * `av_sgid_index` / `av_vhca_port_num` lines emitted at the tail of
 * the output let a shell harness compare a restored QP's resolved
 * L2 destination against the local NIC's MAC and the destination
 * netdev's `ip neigh` entry. If av_dmac equals the local NIC MAC
 * the QPC is self-addressed at L2 and the dmac-stale theory is
 * confirmed; if it equals the peer's MAC, the bug is elsewhere.
 *
 * Output format is intentionally shell-eval-able (one `key=value`
 * per line) so test_fw_id_continuity.sh and
 * check_qp_av_dmac.sh can capture the result into named variables.
 */
static int do_query_qp(int fd, unsigned int vf_id, unsigned int qpn)
{
	struct mlx5_vfmig_query_qp arg = {
		.vf_id = vf_id,
		.qpn   = qpn,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_QUERY_QP, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(QUERY_QP requires the VF mdev to be "
				"interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"QUERY_QP: invalid arg (vf_id=%u qpn=%u). "
				"qpn must fit in 24 bits.\n", vf_id, qpn);
		else
			perror("QUERY_QP");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("qpn=%u\n", qpn);

	/* state-independent bookkeeping */
	printf("qpc_state=%u\n",                  arg.qpc_state);
	printf("qpc_pd=%u\n",                     arg.qpc_pd);
	printf("qpc_q_key=0x%08x\n",              arg.qpc_q_key);
	printf("qpc_uar_page=%u\n",               arg.qpc_uar_page);
	printf("qpc_log_page_size=%u\n",          arg.qpc_log_page_size);
	printf("qpc_log_sq_size=%u\n",            arg.qpc_log_sq_size);
	printf("qpc_log_rq_size=%u\n",            arg.qpc_log_rq_size);
	printf("qpc_log_msg_max=%u\n",            arg.qpc_log_msg_max);
	printf("qpc_user_index=%u\n",             arg.qpc_user_index);

	/* cross-references */
	printf("qpc_remote_qpn=%u\n",             arg.qpc_remote_qpn);
	printf("qpc_cqn_snd=%u\n",                arg.qpc_cqn_snd);
	printf("qpc_cqn_rcv=%u\n",                arg.qpc_cqn_rcv);
	printf("qpc_srqn_rmpn_xrqn=%u\n",         arg.qpc_srqn_rmpn_xrqn);

	/* PSNs */
	printf("qpc_next_send_psn=0x%06x\n",      arg.qpc_next_send_psn);
	printf("qpc_next_rcv_psn=0x%06x\n",       arg.qpc_next_rcv_psn);
	printf("qpc_last_acked_psn=0x%06x\n",     arg.qpc_last_acked_psn);

	/* queue counters */
	printf("qpc_hw_sq_wqebb_counter=%u\n",    arg.qpc_hw_sq_wqebb_counter);
	printf("qpc_sw_sq_wqebb_counter=%u\n",    arg.qpc_sw_sq_wqebb_counter);
	printf("qpc_hw_rq_counter=%u\n",          arg.qpc_hw_rq_counter);
	printf("qpc_sw_rq_counter=%u\n",          arg.qpc_sw_rq_counter);

	/* RTR-set */
	printf("qpc_path_mtu=%u\n",               arg.qpc_path_mtu);
	printf("qpc_min_rnr_nak=%u\n",            arg.qpc_min_rnr_nak);
	printf("qpc_log_rra_max=%u\n",            arg.qpc_log_rra_max);
	printf("qpc_pkey_index=%u\n",             arg.qpc_pkey_index);

	/* RTS-set */
	printf("qpc_log_sra_max=%u\n",            arg.qpc_log_sra_max);
	printf("qpc_retry_count=%u\n",            arg.qpc_retry_count);
	printf("qpc_rnr_retry=%u\n",              arg.qpc_rnr_retry);

	/*
	 * AV: emit primary_address_path as a hex blob suitable for byte-
	 * equal compare across SAVE/LOAD by the harness (single line,
	 * lowercase hex, no separators). The kernel zero-fills the tail
	 * past MLX5_FLD_SZ_BYTES(qpc, primary_address_path) so trailing
	 * bytes are stable on both sides.
	 */
	printf("qpc_primary_address_path=");
	for (size_t i = 0; i < sizeof(arg.qpc_primary_address_path); i++)
		printf("%02x", arg.qpc_primary_address_path[i]);
	printf("\n");

	/*
	 * AV decode: pull the AV subfields out of the raw blob so a
	 * shell harness can grep them as named key=value pairs without
	 * decoding ads_bits itself. Layout per
	 * include/linux/mlx5/mlx5_ifc.h struct mlx5_ifc_ads_bits, big-
	 * endian dword storage:
	 *
	 *   DW1 (offset 0x04..0x08):
	 *     plane_index[8] grh[1] mlid[7] rlid[16]
	 *     -> grh bit  = bit 7 of byte 0x05
	 *
	 *   DW2 (offset 0x08..0x0c), big-endian dword:
	 *     [bits 31..27] ack_timeout
	 *     [bits 26..24] reserved_at_45
	 *     [bits 23..16] src_addr_index   -> byte 0x09
	 *     [bits 15..12] reserved_at_50
	 *     [bits 11..8 ] stat_rate
	 *     [bits  7..0 ] hop_limit         -> byte 0x0b
	 *
	 *   rgid_rip[16] (offset 0x10..0x20):
	 *     16 bytes of dgid in normal byte order
	 *
	 *   DW9 (offset 0x24..0x28):
	 *     dei_cfi[1] eth_prio[3] sl[4] vhca_port_num[8] rmac_47_32[16]
	 *     -> vhca_port_num = byte 0x25
	 *     -> rmac[0..1]    = bytes 0x26..0x27
	 *
	 *   DW10 (offset 0x28..0x2c):
	 *     rmac_31_0[32]
	 *     -> rmac[2..5]    = bytes 0x28..0x2b
	 *
	 * The decoded fields are what the §S6b stale-dmac diagnostic
	 * (tools/testing/criu_rdma/design/qp_av_dmac_swap.md) needs:
	 * compare av_dmac against the local NIC's MAC -- if equal, the
	 * QPC is self-addressed at L2 and the dmac-stale theory is
	 * confirmed.
	 */
	{
		const unsigned char *p = arg.qpc_primary_address_path;
		unsigned char dmac[6];
		const unsigned char *dgid = &p[0x10];
		unsigned int sgid_index = p[0x09];
		unsigned int hop_limit  = p[0x0b];
		unsigned int port_num   = p[0x25];
		unsigned int grh        = (p[0x05] >> 7) & 0x1;
		/*
		 * Self-check: pkey_index occupies the low 16 bits of DW0
		 * (bytes 0x02..0x03 in big-endian dword storage). The
		 * kernel-side handler already decoded the same field via
		 * MLX5_GET(qpc, qpc, primary_address_path.pkey_index)
		 * into arg.qpc_pkey_index. If our manual byte-indexing
		 * disagrees, the IFC layout we've coded against has
		 * shifted -- bail loudly rather than silently produce a
		 * wrong dmac decode that would misdirect the §S6b
		 * diagnostic.
		 */
		unsigned int pkey_decoded = ((unsigned int)p[0x02] << 8) | p[0x03];
		if (pkey_decoded != arg.qpc_pkey_index) {
			fprintf(stderr,
				"INTERNAL ERROR: ads_bits offset self-check "
				"failed: pkey from blob (bytes 0x02,0x03 = "
				"0x%04x) != pkey from kernel IFC decode "
				"(0x%04x). The mlx5_ifc_ads_bits layout has "
				"shifted; the av_dmac/av_dgid/av_sgid_index "
				"decode below is UNTRUSTWORTHY. Refresh the "
				"byte offsets in this tool against "
				"include/linux/mlx5/mlx5_ifc.h before "
				"shipping a verdict.\n",
				pkey_decoded, arg.qpc_pkey_index);
			return 1;
		}

		dmac[0] = p[0x26];
		dmac[1] = p[0x27];
		dmac[2] = p[0x28];
		dmac[3] = p[0x29];
		dmac[4] = p[0x2a];
		dmac[5] = p[0x2b];

		printf("av_dmac=%02x:%02x:%02x:%02x:%02x:%02x\n",
		       dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5]);
		printf("av_dgid=");
		for (int i = 0; i < 16; i++) {
			printf("%02x", dgid[i]);
			if (i % 2 == 1 && i != 15)
				printf(":");
		}
		printf("\n");
		/*
		 * RoCEv2-IPv4 GIDs are ::ffff:<a.b.c.d>. Decode as a
		 * convenience so the harness can grep one line and
		 * cross-check against `ip neigh show`.
		 */
		if (dgid[0] == 0 && dgid[1] == 0 && dgid[2] == 0 &&
		    dgid[3] == 0 && dgid[4] == 0 && dgid[5] == 0 &&
		    dgid[6] == 0 && dgid[7] == 0 && dgid[8] == 0 &&
		    dgid[9] == 0 && dgid[10] == 0xff && dgid[11] == 0xff)
			printf("av_dgid_ipv4=%u.%u.%u.%u\n",
			       dgid[12], dgid[13], dgid[14], dgid[15]);
		printf("av_sgid_index=%u\n",     sgid_index);
		printf("av_hop_limit=%u\n",      hop_limit);
		printf("av_vhca_port_num=%u\n",  port_num);
		printf("av_grh=%u\n",            grh);
	}
	return 0;
}

/*
 * EXPERIMENTAL: §S3b empirical -- "is the source's FW pdn still
 * usable on the destination under a given uid scope after
 * LOAD_VHCA_STATE?". See include/uapi/linux/mlx5_vfmig.h's
 * MLX5_VFMIG_IOC_PROBE_PD block for the full methodology.
 *
 * Output is one key=value per line so the shell harness can
 * capture it into named variables.
 */
static int do_probe_pd(int fd, unsigned int vf_id, unsigned int pdn,
		       unsigned int uid_hint)
{
	struct mlx5_vfmig_probe_pd arg = {
		.vf_id    = vf_id,
		.pdn      = pdn,
		.uid_hint = uid_hint,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_PD, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_PD requires the VF mdev to be "
				"interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_PD: invalid arg "
				"(vf_id=%u pdn=%u uid_hint=%u). "
				"pdn must fit in 24 bits, uid in 16.\n",
				vf_id, pdn, uid_hint);
		else
			perror("PROBE_PD");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("pdn=%u\n", pdn);
	printf("uid_hint=%u\n", uid_hint);
	printf("fw_syndrome=0x%08x\n", arg.fw_syndrome);
	printf("fw_accept=%u\n", arg.fw_syndrome == 0 ? 1 : 0);
	return 0;
}

/*
 * EXPERIMENTAL: §S4b empirical -- "is the source's FW mkey at index
 * N still alive on the destination after LOAD_VHCA_STATE, and does
 * its (pd, len, start_addr) match the source's pre-SAVE view?".
 * See include/uapi/linux/mlx5_vfmig.h's MLX5_VFMIG_IOC_PROBE_MKEY
 * block for the full methodology.
 *
 * Output is one key=value per line so the shell harness can
 * capture it into named variables (mirror of probe_pd).
 */
static int do_probe_mkey(int fd, unsigned int vf_id,
			 unsigned int mkey_index)
{
	struct mlx5_vfmig_probe_mkey arg = {
		.vf_id      = vf_id,
		.mkey_index = mkey_index,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_MKEY, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_MKEY requires the VF mdev to be "
				"interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_MKEY: invalid arg "
				"(vf_id=%u mkey_index=0x%x). "
				"mkey_index must fit in 24 bits.\n",
				vf_id, mkey_index);
		else
			perror("PROBE_MKEY");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("mkey_index=0x%06x\n", mkey_index);
	printf("fw_syndrome=0x%08x\n", arg.fw_syndrome);
	printf("fw_accept=%u\n", arg.fw_syndrome == 0 ? 1 : 0);
	printf("fw_pd=0x%06x\n", arg.fw_pd);
	printf("fw_qpn=0x%06x\n", arg.fw_qpn);
	printf("fw_start_addr=0x%016llx\n",
	       (unsigned long long)arg.fw_start_addr);
	printf("fw_length=0x%016llx\n",
	       (unsigned long long)arg.fw_length);
	return 0;
}

/*
 * EXPERIMENTAL: §S5b empirical -- "is the source's FW CQ at cqn=N
 * still alive on the destination after LOAD_VHCA_STATE, and does
 * its (eqn, log_cq_size, log_page_size, page_offset, status, oi)
 * match the source's pre-SAVE view?". See
 * include/uapi/linux/mlx5_vfmig.h's MLX5_VFMIG_IOC_PROBE_CQN block
 * for the full methodology.
 *
 * Output is one key=value per line so the shell harness can
 * capture it into named variables (mirror of probe_mkey).
 */
static int do_probe_cqn(int fd, unsigned int vf_id, unsigned int cqn)
{
	struct mlx5_vfmig_probe_cqn arg = {
		.vf_id = vf_id,
		.cqn   = cqn,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_CQN, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_CQN requires the VF mdev to be "
				"interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_CQN: invalid arg "
				"(vf_id=%u cqn=0x%x). "
				"cqn must fit in 24 bits.\n",
				vf_id, cqn);
		else
			perror("PROBE_CQN");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("cqn=0x%06x\n", cqn);
	printf("fw_syndrome=0x%08x\n", arg.fw_syndrome);
	printf("fw_accept=%u\n", arg.fw_syndrome == 0 ? 1 : 0);
	printf("fw_eqn=0x%08x\n", arg.fw_eqn);
	printf("fw_status=%u\n", arg.fw_status);
	printf("fw_log_cq_size=%u\n", arg.fw_log_cq_size);
	printf("fw_log_page_size=%u\n", arg.fw_log_page_size);
	printf("fw_page_offset=%u\n", arg.fw_page_offset);
	printf("fw_oi=%u\n", arg.fw_oi);
	printf("fw_cqe_sz=%u\n", arg.fw_cqe_sz);
	printf("fw_apu_cq=%u\n", arg.fw_apu_cq);
	printf("fw_uar_page=0x%06x\n", arg.fw_uar_page);
	printf("fw_dbr_addr=0x%016llx\n",
	       (unsigned long long)arg.fw_dbr_addr);
	return 0;
}

/*
 * EXPERIMENTAL: §S3b "destroy direction" probe -- bracket-tests the
 * FW behaviour when DESTROY_QP / 2RST_QP is issued under a uid_hint
 * that may or may not match the QPC's owning uid. Three FW commands
 * per call (QUERY_QP -> destroy/2RST -> QUERY_QP) so the caller can
 * tell whether the operation actually destroyed the QPC, or merely
 * silently no-op'd.
 *
 * Output format: one key=value per line so the harness can capture
 * each cell into a uid-lane x op-mode matrix.
 */
static int do_probe_qp_teardown(int fd, unsigned int vf_id,
				unsigned int qpn,
				unsigned int uid_hint,
				unsigned int op_mode)
{
	struct mlx5_vfmig_probe_qp_teardown arg = {
		.vf_id    = vf_id,
		.qpn      = qpn,
		.uid_hint = uid_hint,
		.op_mode  = op_mode,
	};
	const char *op_name;

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_QP_TEARDOWN, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_QP_TEARDOWN requires the VF mdev "
				"to be interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_QP_TEARDOWN: invalid arg "
				"(vf_id=%u qpn=0x%x uid_hint=0x%x "
				"op_mode=%u). qpn 24 bits, uid 16 bits, "
				"op_mode 0 (DESTROY) or 1 (2RST).\n",
				vf_id, qpn, uid_hint, op_mode);
		else
			perror("PROBE_QP_TEARDOWN");
		return 1;
	}

	op_name = (op_mode == MLX5_VFMIG_QP_TEARDOWN_OP_DESTROY) ?
		  "DESTROY_QP" : "2RST_QP";

	printf("vf_id=%u\n", vf_id);
	printf("qpn=0x%06x\n", qpn);
	printf("uid_hint=0x%04x\n", uid_hint);
	printf("op_mode=%u\n", op_mode);
	printf("op_name=%s\n", op_name);

	printf("pre_query_status=0x%08x\n", arg.pre_query_status);
	printf("pre_query_syndrome=0x%08x\n", arg.pre_query_syndrome);
	printf("pre_query_accept=%u\n",
	       arg.pre_query_status == 0 ? 1 : 0);
	printf("pre_qpc_state=%u\n", arg.pre_qpc_state);
	printf("pre_qpc_pd=0x%06x\n", arg.pre_qpc_pd);

	printf("op_status=0x%08x\n", arg.op_status);
	printf("op_syndrome=0x%08x\n", arg.op_syndrome);
	/*
	 * op_status is the (cast through int) errno from
	 * mlx5_cmd_exec; treat 0 as the FW-accept lane the caller
	 * wants to interpret. Cast back to int for the boolean.
	 */
	printf("op_accept=%u\n",
	       ((int)arg.op_status == 0 && arg.op_syndrome == 0) ? 1 : 0);

	printf("post_query_status=0x%08x\n", arg.post_query_status);
	printf("post_query_syndrome=0x%08x\n", arg.post_query_syndrome);
	printf("post_query_accept=%u\n",
	       arg.post_query_status == 0 ? 1 : 0);
	printf("post_qpc_state=%u\n", arg.post_qpc_state);

	/*
	 * Combined verdict for the smoking-gun check the harness wants:
	 *   qpc_alive_after_op = post_query_status == 0
	 * == "FW QPC is still queryable after the destroy/2RST".
	 *
	 * For DESTROY: qpc_alive_after_op==1 means silent-no-op.
	 * For 2RST: qpc_alive_after_op==1 always (RESET QPC is not
	 *           destroyed). Caller checks post_qpc_state ==
	 *           pre_qpc_state to detect "modify silently no-op'd".
	 */
	printf("qpc_alive_after_op=%u\n",
	       arg.post_query_status == 0 ? 1 : 0);
	return 0;
}

/*
 * EXPERIMENTAL: §S3b "DEALLOC_PD uid-gating" probe -- issues
 * DEALLOC_PD(pdn, uid_hint) on the bound VF mdev's cmdif and
 * reports the FW result. Destructive on success; the harness pairs
 * each call with PROBE_PD before/after to confirm whether the PDC
 * was actually deallocated.
 */
static int do_probe_dealloc_pd(int fd, unsigned int vf_id,
			       unsigned int pdn, unsigned int uid_hint)
{
	struct mlx5_vfmig_probe_dealloc_pd arg = {
		.vf_id    = vf_id,
		.pdn      = pdn,
		.uid_hint = uid_hint,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_DEALLOC_PD, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_DEALLOC_PD requires the VF mdev "
				"to be interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_DEALLOC_PD: invalid arg "
				"(vf_id=%u pdn=0x%x uid_hint=0x%x). "
				"pdn 24 bits, uid 16 bits.\n",
				vf_id, pdn, uid_hint);
		else
			perror("PROBE_DEALLOC_PD");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("pdn=0x%06x\n", pdn);
	printf("uid_hint=0x%04x\n", uid_hint);
	printf("op_status=0x%08x\n", arg.op_status);
	printf("op_syndrome=0x%08x\n", arg.op_syndrome);
	printf("op_accept=%u\n",
	       ((int)arg.op_status == 0 && arg.op_syndrome == 0) ? 1 : 0);
	return 0;
}

/*
 * EXPERIMENTAL: §S3b "Generalising to QP/CQ" probe (CQ branch) --
 * issues QUERY_CQ -> DESTROY_CQ(cqn, uid_hint) -> QUERY_CQ on the
 * bound VF mdev's cmdif. Destructive on success.
 *
 * Verdict from the printed key=value pairs:
 *   op_status=0 && post_query_status!=0  -> destroy worked cross-uid
 *   op_status=0 && post_query_status==0  -> silent no-op (alarm)
 *   op_status!=0                          -> destroy rejected; check
 *                                            op_syndrome class
 */
static int do_probe_cq_destroy(int fd, unsigned int vf_id,
			       unsigned int cqn, unsigned int uid_hint)
{
	struct mlx5_vfmig_probe_cq_destroy arg = {
		.vf_id    = vf_id,
		.cqn      = cqn,
		.uid_hint = uid_hint,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_CQ_DESTROY, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_CQ_DESTROY requires the VF mdev "
				"to be interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_CQ_DESTROY: invalid arg "
				"(vf_id=%u cqn=0x%x uid_hint=0x%x). "
				"cqn 24 bits, uid 16 bits.\n",
				vf_id, cqn, uid_hint);
		else
			perror("PROBE_CQ_DESTROY");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("cqn=0x%06x\n", cqn);
	printf("uid_hint=0x%04x\n", uid_hint);
	printf("pre_query_status=0x%08x\n", arg.pre_query_status);
	printf("pre_query_syndrome=0x%08x\n", arg.pre_query_syndrome);
	printf("pre_cqc_status=%u\n", arg.pre_cqc_status);
	printf("op_status=0x%08x\n", arg.op_status);
	printf("op_syndrome=0x%08x\n", arg.op_syndrome);
	printf("post_query_status=0x%08x\n", arg.post_query_status);
	printf("post_query_syndrome=0x%08x\n", arg.post_query_syndrome);
	printf("post_cqc_status=%u\n", arg.post_cqc_status);
	printf("op_accept=%u\n",
	       ((int)arg.op_status == 0 && arg.op_syndrome == 0) ? 1 : 0);
	printf("destroyed=%u\n",
	       ((int)arg.op_status == 0 && (int)arg.post_query_status != 0)
	       ? 1 : 0);
	return 0;
}

/*
 * EXPERIMENTAL: §S3b "Generalising to QP/CQ" probe (MR/mkey branch).
 * Same shape as probe_cq_destroy but driven against DESTROY_MKEY.
 */
static int do_probe_mr_destroy(int fd, unsigned int vf_id,
			       unsigned int mkey_index,
			       unsigned int uid_hint)
{
	struct mlx5_vfmig_probe_mr_destroy arg = {
		.vf_id      = vf_id,
		.mkey_index = mkey_index,
		.uid_hint   = uid_hint,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_PROBE_MR_DESTROY, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not bound to mlx5_core "
				"(PROBE_MR_DESTROY requires the VF mdev "
				"to be interface-up)\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"PROBE_MR_DESTROY: invalid arg "
				"(vf_id=%u mkey_index=0x%x uid_hint=0x%x). "
				"mkey_index 24 bits, uid 16 bits.\n",
				vf_id, mkey_index, uid_hint);
		else
			perror("PROBE_MR_DESTROY");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("mkey_index=0x%06x\n", mkey_index);
	printf("uid_hint=0x%04x\n", uid_hint);
	printf("pre_query_status=0x%08x\n", arg.pre_query_status);
	printf("pre_query_syndrome=0x%08x\n", arg.pre_query_syndrome);
	printf("pre_mkc_free=%u\n", arg.pre_mkc_free);
	printf("op_status=0x%08x\n", arg.op_status);
	printf("op_syndrome=0x%08x\n", arg.op_syndrome);
	printf("post_query_status=0x%08x\n", arg.post_query_status);
	printf("post_query_syndrome=0x%08x\n", arg.post_query_syndrome);
	printf("post_mkc_free=%u\n", arg.post_mkc_free);
	printf("op_accept=%u\n",
	       ((int)arg.op_status == 0 && arg.op_syndrome == 0) ? 1 : 0);
	printf("destroyed=%u\n",
	       ((int)arg.op_status == 0 && (int)arg.post_query_status != 0)
	       ? 1 : 0);
	return 0;
}

/*
 * MLX5_VFMIG_IOC_QUERY_AWAITING_BIND CLI wrapper. user_mr_dma
 * stage-2 success-criterion accessor: post-LOAD, asks the PF how
 * many awaiting_bind placeholders landed in the VF's
 * vfmig_iova_domain via HOST_USER_PAGE replay, plus the per-kind
 * breakdown. Used by test_user_object_replay.sh to assert
 * source-emit == dest-install on every uobject kind.
 *
 * Output format: one key=value per line for shell ingestion.
 * "by_kind_N=K" lines are emitted for N in 0..NR_KINDS-1; the
 * meaning of each index follows enum vfmig_huobj_kind
 * (0=NONE, 1=MR, 2=CQ, 3=QP, 4=SRQ, 5=DBR; 6/7 reserved).
 */
static int do_query_awaiting_bind(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_awaiting_bind arg = { .vf_id = vf_id };
	unsigned int k;

	if (ioctl(fd, MLX5_VFMIG_IOC_QUERY_AWAITING_BIND, &arg) < 0) {
		if (errno == ENODEV)
			fprintf(stderr,
				"vf %u: not tracked (call set_tracked %u 1 first)\n",
				vf_id, vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"QUERY_AWAITING_BIND: invalid arg (vf_id=%u)\n",
				vf_id);
		else
			perror("QUERY_AWAITING_BIND");
		return 1;
	}
	printf("vf_id=%u\n", vf_id);
	printf("total=%llu\n", (unsigned long long)arg.total);
	for (k = 0; k < MLX5_VFMIG_QUERY_AWAITING_BIND_NR_KINDS; k++)
		printf("by_kind_%u=%llu\n", k,
		       (unsigned long long)arg.count_by_kind[k]);
	printf("by_kind_MR=%llu\n",
	       (unsigned long long)arg.count_by_kind[1]);
	printf("by_kind_CQ=%llu\n",
	       (unsigned long long)arg.count_by_kind[2]);
	printf("by_kind_QP=%llu\n",
	       (unsigned long long)arg.count_by_kind[3]);
	printf("by_kind_SRQ=%llu\n",
	       (unsigned long long)arg.count_by_kind[4]);
	printf("by_kind_DBR=%llu\n",
	       (unsigned long long)arg.count_by_kind[5]);
	return 0;
}

static int do_set_tracked(int fd, unsigned int vf_id, unsigned int enable)
{
	struct mlx5_vfmig_set_tracked arg = {
		.vf_id  = vf_id,
		.enable = enable ? 1 : 0,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_SET_TRACKED, &arg) < 0) {
		perror("SET_TRACKED");
		return 1;
	}
	printf("vf %u: vfmig tracked=%u (call before driver bind)\n",
	       vf_id, arg.enable);
	return 0;
}

/*
 * Parse a canonical RFC 4122 UUID string ("8-4-4-4-12" hex) into 16
 * bytes in network byte order, matching what the kernel stores and
 * what util-linux's libuuid produces. Accepts uppercase or lowercase
 * hex, must contain four dashes in the canonical positions, and must
 * be exactly 36 chars long. Returns 0 on success, -EINVAL on parse
 * failure.
 */
static int parse_uuid(const char *s, unsigned char out[16])
{
	static const int dash_at[] = { 8, 13, 18, 23 };
	unsigned int i, j, b;

	if (!s || strlen(s) != 36)
		return -EINVAL;
	for (i = 0; i < sizeof(dash_at) / sizeof(dash_at[0]); i++)
		if (s[dash_at[i]] != '-')
			return -EINVAL;

	b = 0;
	for (i = 0; i < 36; i++) {
		unsigned int hi, lo;

		if (s[i] == '-')
			continue;
		if (i + 1 >= 36 || s[i + 1] == '-')
			return -EINVAL;
		for (j = 0; j < 2; j++) {
			char c = s[i + j];
			unsigned int v;

			if (c >= '0' && c <= '9')
				v = c - '0';
			else if (c >= 'a' && c <= 'f')
				v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				v = c - 'A' + 10;
			else
				return -EINVAL;
			if (j == 0)
				hi = v;
			else
				lo = v;
		}
		if (b >= 16)
			return -EINVAL;
		out[b++] = (unsigned char)((hi << 4) | lo);
		i++;
	}
	return b == 16 ? 0 : -EINVAL;
}

/*
 * Inverse of parse_uuid; writes 36 bytes + NUL. @buf must be >= 37.
 */
static void format_uuid(const unsigned char in[16], char *buf)
{
	snprintf(buf, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 in[0], in[1], in[2], in[3],
		 in[4], in[5],
		 in[6], in[7],
		 in[8], in[9],
		 in[10], in[11], in[12], in[13], in[14], in[15]);
}

static int uuid_is_zero(const unsigned char in[16])
{
	static const unsigned char zero[16] = {};

	return memcmp(in, zero, 16) == 0;
}

static int do_set_vf_uuid(int fd, unsigned int vf_id, const char *uuid_str)
{
	struct mlx5_vfmig_set_vf_uuid arg = { .vf_id = vf_id };
	int err;

	err = parse_uuid(uuid_str, arg.vf_uuid);
	if (err) {
		fprintf(stderr,
			"SET_VF_UUID: bad UUID %s (expected 8-4-4-4-12 hex, e.g. 5edc7d3e-7e44-4d8a-b8a8-50f7c8a0c3b1)\n",
			uuid_str);
		return 1;
	}
	if (uuid_is_zero(arg.vf_uuid)) {
		fprintf(stderr,
			"SET_VF_UUID: refusing to send all-zeros UUID (kernel will reject anyway)\n");
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SET_VF_UUID, &arg) < 0) {
		if (errno == EBUSY)
			fprintf(stderr,
				"vf %u: a different UUID is already stamped on this slot; "
				"sriov_numvfs=0 + sriov_numvfs=N to clear, or use the same UUID\n",
				vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"vf %u: SET_VF_UUID rejected (vf_id out of range or zero UUID)\n",
				vf_id);
		else
			perror("SET_VF_UUID");
		return 1;
	}
	printf("vf %u: vf_uuid stamped\n", vf_id);
	return 0;
}

static int do_mark(int fd, unsigned int vf_id, unsigned int defer_resume)
{
	struct mlx5_vfmig_mark_restored arg = {
		.vf_id = vf_id,
		.flags = defer_resume ?
			 MLX5_VFMIG_MARK_RESTORED_DEFER_RESUME : 0,
	};

	if (ioctl(fd, MLX5_VFMIG_IOC_MARK_RESTORED, &arg) < 0) {
		if (errno == EALREADY)
			fprintf(stderr,
				"vf %u is already marked restored\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"vf_id %u out of range (have you set sriov_numvfs?)\n",
				vf_id);
		else
			perror("MARK_RESTORED");
		return 1;
	}
	printf("marked vf %u as restored%s\n", vf_id,
	       defer_resume ? " (resume deferred to RESUME_VHCA)" : "");
	return 0;
}

/*
 * MLX5_VFMIG_IOC_SUSPEND_VHCA wrapper. Parks the VF's RDMA datapath
 * (SUSPEND_VHCA initiator+responder) ahead of a memory snapshot -- the
 * "pause" half of the stop-and-copy ordering fix. CRIU calls this at
 * the early CHECKPOINT_DEVICES hook, before the dumpee's memory is
 * copied. Idempotent. See
 * tools/testing/criu_rdma/design/datapath_pause_resume.md.
 */
static int do_suspend(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_suspend_vhca arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_SUSPEND_VHCA, &arg) < 0) {
		if (errno == EOPNOTSUPP)
			fprintf(stderr,
				"vf %u: not migration-enabled (run enable_migratable first)\n",
				vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"vf_id %u out of range (have you set sriov_numvfs?)\n",
				vf_id);
		else
			perror("SUSPEND_VHCA");
		return 1;
	}
	printf("suspended vf %u datapath\n", vf_id);
	return 0;
}

/*
 * MLX5_VFMIG_IOC_RESUME_VHCA wrapper. Un-parks a VF previously
 * suspended via suspend_vhca (or left parked by a defer-resume
 * restore). On the destination, CRIU calls this at RESUME_DEVICES_LATE
 * after all MR/ring VMAs have been restored. Idempotent.
 */
static int do_resume(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_resume_vhca arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_RESUME_VHCA, &arg) < 0) {
		if (errno == EINVAL)
			fprintf(stderr,
				"vf_id %u out of range (have you set sriov_numvfs?)\n",
				vf_id);
		else
			perror("RESUME_VHCA");
		return 1;
	}
	printf("resumed vf %u datapath\n", vf_id);
	return 0;
}

static int do_get(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_get_vhca_id arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_GET_VHCA_ID, &arg) < 0) {
		perror("GET_VHCA_ID");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x\n", vf_id, arg.vhca_id);
	return 0;
}

static int query_one(int fd, unsigned int vf_id, struct mlx5_vfmig_query_vf *out)
{
	struct mlx5_vfmig_query_vf arg = { .vf_id = vf_id };
	int err;

	/*
	 * The kernel populates @arg unconditionally -- on success and
	 * on -ERANGE -- so callers that bail on -ERANGE can still read
	 * @num_vfs (and @vf_uuid, which is zero-filled) out of @out.
	 * Copy out first, then return the verdict.
	 */
	err = ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, &arg) ? -errno : 0;
	*out = arg;
	return err;
}

static int do_query(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err = query_one(fd, vf_id, &info);

	if (err == -ERANGE) {
		fprintf(stderr,
			"vf_id %u out of range; PF has %u VFs\n",
			vf_id, info.num_vfs);
		return 1;
	}
	if (err) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x restored=%u tracked=%u (num_vfs=%u)",
	       vf_id, info.vhca_id, info.restored, info.tracked,
	       info.num_vfs);
	if (uuid_is_zero(info.vf_uuid)) {
		printf(" vf_uuid=<unset>\n");
	} else {
		char ub[37];

		format_uuid(info.vf_uuid, ub);
		printf(" vf_uuid=%s\n", ub);
	}
	return 0;
}

static int do_list(int fd)
{
	struct mlx5_vfmig_query_vf info;
	unsigned int i, n;
	int err;

	err = query_one(fd, 0, &info);
	if (err && err != -ERANGE) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	n = info.num_vfs;
	if (n == 0) {
		printf("no VFs provisioned (sriov_numvfs == 0)\n");
		return 0;
	}

	printf("%-6s %-9s %-9s %-9s %s\n",
	       "vf_id", "vhca_id", "restored", "tracked", "vf_uuid");
	for (i = 0; i < n; i++) {
		char ub[37];

		err = query_one(fd, i, &info);
		if (err) {
			errno = -err;
			fprintf(stderr, "QUERY_VF vf %u: %s\n",
				i, strerror(errno));
			continue;
		}
		if (uuid_is_zero(info.vf_uuid))
			snprintf(ub, sizeof(ub), "<unset>");
		else
			format_uuid(info.vf_uuid, ub);
		printf("%-6u 0x%04x    %-9u %-9u %s\n",
		       i, info.vhca_id, info.restored, info.tracked, ub);
	}
	return 0;
}

static int pump_blob(int load_fd, int blob_fd, size_t *out_bytes)
{
	/*
	 * Use a moderately large buffer so the FSM in the kernel sees
	 * full records in fewer write() syscalls; partial writes are
	 * also fine.
	 */
	enum { CHUNK = 1u << 16 };
	char *buf = malloc(CHUNK);
	size_t total = 0;

	if (!buf)
		return -ENOMEM;

	for (;;) {
		ssize_t r = read(blob_fd, buf, CHUNK);
		ssize_t off = 0;

		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -errno;
		}
		while (off < r) {
			ssize_t w = write(load_fd, buf + off, r - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(buf);
				return -errno;
			}
			if (w == 0) {
				free(buf);
				return -EIO;
			}
			off += w;
			total += w;
		}
	}

	free(buf);
	*out_bytes = total;
	return 0;
}

static int do_load(int fd, unsigned int vf_id, const char *blob_path)
{
	struct mlx5_vfmig_load_state arg = { .vf_id = vf_id };
	int blob_fd, load_fd, err;
	size_t bytes = 0;
	struct stat st;

	blob_fd = open(blob_path, O_RDONLY);
	if (blob_fd < 0) {
		fprintf(stderr, "open %s: %s\n", blob_path, strerror(errno));
		return 1;
	}
	if (fstat(blob_fd, &st) == 0)
		fprintf(stderr,
			"vfmig: streaming %lld bytes from %s into vf %u\n",
			(long long)st.st_size, blob_path, vf_id);

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &arg) < 0) {
		perror("LOAD_VHCA_STATE");
		close(blob_fd);
		return 1;
	}
	load_fd = arg.load_fd;

	err = pump_blob(load_fd, blob_fd, &bytes);
	close(blob_fd);
	if (err) {
		errno = -err;
		fprintf(stderr,
			"vfmig: write failed after %zu bytes: %s\n",
			bytes, strerror(errno));
		close(load_fd);
		return 1;
	}

	if (close(load_fd) < 0) {
		perror("close(load_fd)");
		return 1;
	}
	printf("loaded %zu bytes of state into vf %u\n", bytes, vf_id);
	return 0;
}

static int drain_to_file(int save_fd, int blob_fd, size_t *out_bytes)
{
	enum { CHUNK = 1u << 16 };
	char *buf = malloc(CHUNK);
	size_t total = 0;

	if (!buf)
		return -ENOMEM;

	for (;;) {
		ssize_t r = read(save_fd, buf, CHUNK);
		ssize_t off = 0;

		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -errno;
		}
		while (off < r) {
			ssize_t w = write(blob_fd, buf + off, r - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(buf);
				return -errno;
			}
			if (w == 0) {
				free(buf);
				return -EIO;
			}
			off += w;
			total += w;
		}
	}

	free(buf);
	*out_bytes = total;
	return 0;
}

static int do_save(int fd, unsigned int vf_id, const char *blob_path,
		   unsigned int flags)
{
	struct mlx5_vfmig_save_state arg = {
		.vf_id = vf_id,
		.flags = flags,
	};
	int blob_fd, save_fd, err;
	size_t bytes = 0;

	blob_fd = open(blob_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (blob_fd < 0) {
		fprintf(stderr, "open %s: %s\n", blob_path, strerror(errno));
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &arg) < 0) {
		perror("SAVE_VHCA_STATE");
		close(blob_fd);
		return 1;
	}
	save_fd = arg.save_fd;

	err = drain_to_file(save_fd, blob_fd, &bytes);
	if (close(save_fd) < 0)
		perror("close(save_fd)");
	if (close(blob_fd) < 0)
		perror("close(blob_fd)");
	if (err) {
		errno = -err;
		fprintf(stderr,
			"vfmig: drain failed after %zu bytes: %s\n",
			bytes, strerror(errno));
		return 1;
	}
	printf("saved %zu bytes from vf %u to %s%s\n", bytes, vf_id, blob_path,
	       (flags & MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED) ?
		" (left suspended)" : "");
	return 0;
}

/*
 * Verb match that accepts either "mark_restored" or "mark-restored",
 * etc. Treats '_' and '-' as equivalent so callers don't have to
 * remember which spelling the tool uses.
 */
static int verb_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a == '-') ? '_' : *a;
		char cb = (*b == '-') ? '_' : *b;

		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static int looks_like_bdf(const char *s)
{
	/* "DDDD:BB:DD.F" -- at least one ':' and no '/' */
	return strchr(s, ':') && !strchr(s, '/');
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <pf-bdf> <verb> [args]\n"
		"  mark_restored    <vf_id> [defer_resume]\n"
		"                   defer_resume: leave VHCA parked after LOAD\n"
		"                   for a later resume_vhca (CRIU restore mirror)\n"
		"  get_vhca_id      <vf_id>\n"
		"  query_vf         <vf_id>\n"
		"  list\n"
		"  load_vhca_state  <vf_id> <blob_path>\n"
		"  save_vhca_state  <vf_id> <blob_path> [keep_suspended]\n"
		"  suspend_vhca     <vf_id>\n"
		"                   pause RDMA datapath before memory snapshot\n"
		"                   (stop-and-copy ordering; CHECKPOINT_DEVICES)\n"
		"  resume_vhca      <vf_id>\n"
		"                   un-park a suspended/defer-resumed VF\n"
		"                   (RESUME_DEVICES_LATE)\n"
		"  enable_migratable <vf_id>\n"
		"  set_tracked       <vf_id> <0|1>\n"
		"  set_vf_uuid       <vf_id> <uuid-string>\n"
		"                    uuid-string: canonical 8-4-4-4-12 hex\n"
		"                    (e.g. 5edc7d3e-7e44-4d8a-b8a8-50f7c8a0c3b1)\n"
		"  probe_uid         <vf_id>     (experimental)\n"
		"  query_qp          <vf_id> <qpn>  (experimental)\n"
		"  probe_pd          <vf_id> <pdn> [<uid_hint=0>]  (experimental)\n"
		"  probe_mkey        <vf_id> <mkey_index>  (experimental)\n"
		"  probe_cqn         <vf_id> <cqn>  (experimental)\n"
		"  probe_qp_teardown <vf_id> <qpn> <uid_hint> <op_mode>\n"
		"                    op_mode: 0 = DESTROY_QP, 1 = MODIFY_QP 2RST\n"
		"                    (experimental, §S3b destroy-direction probe)\n"
		"  probe_dealloc_pd  <vf_id> <pdn> <uid_hint>\n"
		"                    (experimental, §S3b DEALLOC_PD uid-gating probe)\n"
		"  probe_cq_destroy  <vf_id> <cqn> <uid_hint>\n"
		"                    (experimental, §S3b DESTROY_CQ cross-uid probe)\n"
		"  probe_mr_destroy  <vf_id> <mkey_index> <uid_hint>\n"
		"                    (experimental, §S3b DESTROY_MKEY cross-uid probe)\n"
		"  query_awaiting_bind <vf_id>  (user_mr_dma stage-2)\n"
		"verbs accept '-' or '_' interchangeably\n",
		argv0);
}

int main(int argc, char **argv)
{
	char path[256];
	const char *target;
	const char *verb;
	int fd, ret;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	verb = argv[2];

	if (looks_like_bdf(argv[1])) {
		snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
		target = path;
	} else {
		target = argv[1];
	}

	fd = open(target, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", target, strerror(errno));
		return 1;
	}

	if (verb_eq(verb, "list")) {
		if (argc != 3)
			goto badargs;
		ret = do_list(fd);
	} else if (verb_eq(verb, "mark_restored")) {
		unsigned int defer_resume = 0;

		if (argc != 4 && argc != 5)
			goto badargs;
		if (argc == 5) {
			if (!strcmp(argv[4], "defer_resume") ||
			    !strcmp(argv[4], "defer-resume"))
				defer_resume = 1;
			else
				goto badargs;
		}
		ret = do_mark(fd, strtoul(argv[3], NULL, 0), defer_resume);
	} else if (verb_eq(verb, "suspend_vhca") ||
		   verb_eq(verb, "suspend")) {
		if (argc != 4)
			goto badargs;
		ret = do_suspend(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "resume_vhca") ||
		   verb_eq(verb, "resume")) {
		if (argc != 4)
			goto badargs;
		ret = do_resume(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "get_vhca_id")) {
		if (argc != 4)
			goto badargs;
		ret = do_get(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "query_vf")) {
		if (argc != 4)
			goto badargs;
		ret = do_query(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "load_vhca_state")) {
		if (argc != 5)
			goto badargs;
		ret = do_load(fd, strtoul(argv[3], NULL, 0), argv[4]);
	} else if (verb_eq(verb, "save_vhca_state")) {
		unsigned int flags = 0;

		if (argc < 5 || argc > 6)
			goto badargs;
		if (argc == 6) {
			if (!strcmp(argv[5], "keep_suspended") ||
			    !strcmp(argv[5], "keep-suspended"))
				flags |= MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED;
			else
				goto badargs;
		}
		ret = do_save(fd, strtoul(argv[3], NULL, 0), argv[4], flags);
	} else if (verb_eq(verb, "enable_migratable")) {
		if (argc != 4)
			goto badargs;
		ret = do_enable_migratable(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "set_tracked")) {
		if (argc != 5)
			goto badargs;
		ret = do_set_tracked(fd, strtoul(argv[3], NULL, 0),
				     strtoul(argv[4], NULL, 0));
	} else if (verb_eq(verb, "set_vf_uuid")) {
		if (argc != 5)
			goto badargs;
		ret = do_set_vf_uuid(fd, strtoul(argv[3], NULL, 0), argv[4]);
	} else if (verb_eq(verb, "probe_uid")) {
		if (argc != 4)
			goto badargs;
		ret = do_probe_uid(fd, strtoul(argv[3], NULL, 0));
	} else if (verb_eq(verb, "query_qp")) {
		if (argc != 5)
			goto badargs;
		ret = do_query_qp(fd, strtoul(argv[3], NULL, 0),
				  strtoul(argv[4], NULL, 0));
	} else if (verb_eq(verb, "probe_pd")) {
		unsigned int uid_hint = 0;

		if (argc != 5 && argc != 6)
			goto badargs;
		if (argc == 6)
			uid_hint = strtoul(argv[5], NULL, 0);
		ret = do_probe_pd(fd, strtoul(argv[3], NULL, 0),
				  strtoul(argv[4], NULL, 0), uid_hint);
	} else if (verb_eq(verb, "probe_mkey")) {
		if (argc != 5)
			goto badargs;
		ret = do_probe_mkey(fd, strtoul(argv[3], NULL, 0),
				    strtoul(argv[4], NULL, 0));
	} else if (verb_eq(verb, "probe_cqn")) {
		if (argc != 5)
			goto badargs;
		ret = do_probe_cqn(fd, strtoul(argv[3], NULL, 0),
				   strtoul(argv[4], NULL, 0));
	} else if (verb_eq(verb, "probe_qp_teardown")) {
		if (argc != 7)
			goto badargs;
		ret = do_probe_qp_teardown(fd,
				strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				strtoul(argv[5], NULL, 0),
				strtoul(argv[6], NULL, 0));
	} else if (verb_eq(verb, "probe_dealloc_pd")) {
		if (argc != 6)
			goto badargs;
		ret = do_probe_dealloc_pd(fd,
				strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				strtoul(argv[5], NULL, 0));
	} else if (verb_eq(verb, "probe_cq_destroy")) {
		if (argc != 6)
			goto badargs;
		ret = do_probe_cq_destroy(fd,
				strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				strtoul(argv[5], NULL, 0));
	} else if (verb_eq(verb, "probe_mr_destroy")) {
		if (argc != 6)
			goto badargs;
		ret = do_probe_mr_destroy(fd,
				strtoul(argv[3], NULL, 0),
				strtoul(argv[4], NULL, 0),
				strtoul(argv[5], NULL, 0));
	} else if (verb_eq(verb, "query_awaiting_bind")) {
		if (argc != 4)
			goto badargs;
		ret = do_query_awaiting_bind(fd, strtoul(argv[3], NULL, 0));
	} else {
		fprintf(stderr, "unknown verb: %s\n", verb);
		ret = 2;
	}

	close(fd);
	return ret;

badargs:
	close(fd);
	usage(argv[0]);
	return 2;
}
