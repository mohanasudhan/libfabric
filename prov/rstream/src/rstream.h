#include <string.h>

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include "rdma/providers/fi_log.h"

#include <ofi.h>
#include <ofi_util.h>
#include <ofi_proto.h>
#include <ofi_prov.h>
#include <ofi_enosys.h>

#define RSTREAM_CAPS (FI_MSG | FI_SEND | FI_RECV | FI_LOCAL_COMM | FI_REMOTE_COMM)
#define RSTREAM_DEFAULT_QP_SIZE 384
#define RSTREAM_MAX_CTRL_TX 2
#define RSTREAM_MR_BITS 15
#define RSTREAM_DEFAULT_MR_SEG_SIZE (1 << RSTREAM_MR_BITS)

#define RSTREAM_MAX_POLL_TIME 10

#define RSTREAM_MAX_MR_BITS 20
#define RSTREAM_MR_MAX (1ULL << RSTREAM_MAX_MR_BITS)
#define RSTREAM_MR_LEN_MASK (RSTREAM_MR_MAX - 1)
#define RSTREAM_CREDIT_OFFSET RSTREAM_MAX_MR_BITS
#define RSTREAM_CREDIT_BITS 9
#define RSTREAM_CREDITS_MAX (1ULL << RSTREAM_CREDIT_BITS)
#define RSTREAM_CREDIT_MASK ((RSTREAM_CREDITS_MAX - 1) << RSTREAM_CREDIT_OFFSET)
#define RSTREAM_RSOCKETV2 2

/*iWARP, have to also track msg len [msglen, target_credits, target_mr_len]*/
#define RSTREAM_USING_IWARP (rstream_info.ep_attr->protocol == FI_PROTO_IWARP)
#define RSTREAM_IWARP_DATA_SIZE sizeof(uint32_t)

#define RSTREAM_IWARP_MSG_BIT (1ULL << 31)
#define RSTREAM_IWARP_MSG_BIT_MASK (RSTREAM_IWARP_MSG_BIT - 1)
#define RSTREAM_IWARP_IMM_MSG_LEN (1ULL << RSTREAM_MAX_MR_BITS) /* max transmission size */

extern struct fi_info rstream_info;
extern struct fi_provider rstream_prov;
extern struct util_prov rstream_util_prov;
extern struct fi_fabric_attr rstream_fabric_attr;

/* util structs ~ user layer fds */

struct rstream_fabric {
	struct util_fabric util_fabric;
	struct fid_fabric *msg_fabric;
};

struct rstream_domain {
	struct util_domain util_domain;
	struct fid_domain *msg_domain;
};

enum rstream_msg_type {
	RSTREAM_CTRL_MSG,
	RSTREAM_REG_MSG
};

struct rstream_mr_seg {
	void *data_start;
	uint32_t size;
	uint32_t avail_size;
	uint64_t start_offset;
	uint64_t end_offset;
};

struct rstream_lmr_data {
	void *base_addr;
	void *ldesc;
	uint64_t rkey;
	struct fid_mr *mr;
	struct rstream_mr_seg tx;
	struct rstream_mr_seg rx;
	uint64_t recv_buffer_offset;
};

struct rstream_rmr_data {
	struct rstream_mr_seg mr;
	uint64_t rkey;
};

struct rstream_cm_data {
	uint64_t base_addr;
	uint64_t rkey;
	uint32_t rmr_size;
	uint16_t max_rx_credits;
	uint8_t version;
	uint8_t reserved;
};

struct rstream_tx_ctx {
	struct fi_context *tx_ctxs;
	uint32_t num_in_use;
	uint32_t free_index;
	uint32_t front;
};

struct rstream_window {
	uint16_t max_tx_credits;
	uint16_t tx_credits;
	uint16_t ctrl_credits;
	uint16_t max_target_rx_credits;
	uint16_t target_rx_credits;
	uint16_t max_rx_credits;
	uint16_t rx_credits;
};

struct rstream_cq_data {
	uint32_t total_len;
	uint16_t num_completions;
};

struct rstream_ep {
	struct util_ep util_ep;
	struct fid_ep *ep_fd;
	struct fid_domain *msg_domain;
	struct rstream_lmr_data local_mr;
	struct rstream_rmr_data remote_data;
	struct fid_cq *recv_cq;
	struct fid_cq *send_cq;
	struct rstream_window qp_win;
	struct fi_context *rx_ctxs;
	uint32_t rx_ctx_index;
	struct rstream_tx_ctx tx_ctx;
	struct rstream_cq_data cq_data;
};

struct rstream_pep {
	struct util_pep util_pep;
	struct fid_pep	*pep_fd;
};

struct rstream_eq {
	struct util_eq util_eq;
	struct fid_eq *eq_fd;
	uint32_t cm_data_len;
	struct fi_eq_cm_entry *cm_entry;
	uint32_t prev_cm_state;
	RbtHandle ep_map;
};

struct rstream_timer {
	struct timeval start;
	struct timeval end;
	uint32_t poll_time;
};

extern ssize_t rstream_post_cq_data_recv(struct rstream_ep *ep,
	const struct fi_cq_data_entry *cq_entry);

extern int rstream_info_to_rstream(uint32_t version, const struct fi_info *core_info,
	struct fi_info *info);
extern int rstream_info_to_core(uint32_t version, const struct fi_info *rstream_info,
	struct fi_info *core_info);
extern void rstream_set_info(struct fi_info *info);
extern struct fi_ops_cm rstream_ops_cm;
extern struct fi_ops_cm rstream_ops_pep_cm;
extern struct fi_ops_msg rstream_ops_msg;
extern int rstream_passive_ep(struct fid_fabric *fabric, struct fi_info *info,
	struct fid_pep **pep, void *context);
extern void rstream_process_cm_event(struct rstream_ep *ep, void *cm_data);

int rstream_fabric_open(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
	void *context);
int rstream_domain_open(struct fid_fabric *fabric, struct fi_info *info,
	struct fid_domain **domain, void *context);
int rstream_ep_open(struct fid_domain *domain, struct fi_info *info,
	struct fid_ep **ep_fid, void *context);
int rstream_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
	struct fid_eq **eq, void *context);
int rstream_info_to_core(uint32_t version, const struct fi_info *rstream_info,
	struct fi_info *core_info);
