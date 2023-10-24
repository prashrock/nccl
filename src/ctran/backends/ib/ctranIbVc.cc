// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <iostream>
#include <vector>
#include <thread>
#include <unistd.h>
#include "nccl.h"
#include "checks.h"
#include "ctranIbBase.h"
#include "ctranIb.h"
#include "ctranIbImpl.h"
#include "ctranIbVc.h"

uint64_t ncclParamIbPkey();
uint64_t ncclParamIbGidIndex();
uint64_t ncclParamIbTc();
uint64_t ncclParamIbSl();
uint64_t ncclParamIbTimeout();
uint64_t ncclParamIbRetryCnt();

struct busCard {
  enum ibv_mtu mtu;
  uint64_t spn;
  uint64_t iid;
  uint32_t controlQpn;
  uint32_t dataQpn;
  uint8_t port;
};

ctranIb::impl::vc::vc(struct ibv_context *context, struct ibv_pd *pd, struct ibv_cq *cq,
    int port, int peerRank) {
  this->peerRank = peerRank;
  this->controlQp = nullptr;
  this->dataQp = nullptr;
  this->context = context;
  this->pd = pd;
  this->cq = cq;
  this->port = port;

  NCCLCHECKIGNORE(wrap_ibv_reg_mr(&this->sendCtrl.mr, pd, (void *) this->sendCtrl.cmsg,
        MAX_CONTROL_MSGS * sizeof(struct controlMsg),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ));

  NCCLCHECKIGNORE(wrap_ibv_reg_mr(&this->recvCtrl.mr, pd, (void *) this->recvCtrl.cmsg,
        MAX_CONTROL_MSGS * sizeof(struct controlMsg),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ));

  for (int i = 0; i < MAX_CONTROL_MSGS; i++) {
    this->sendCtrl.freeMsg.push_back(&this->sendCtrl.cmsg[i]);
  }

  this->isReady_ = false;
  this->notifications = 0;
}

ctranIb::impl::vc::~vc() {
  NCCLCHECKIGNORE(wrap_ibv_dereg_mr(this->sendCtrl.mr));
  NCCLCHECKIGNORE(wrap_ibv_dereg_mr(this->recvCtrl.mr));

  if (this->controlQp != nullptr) {
    /* we don't need to clean up the posted WQEs; destroying the QP
     * will automatically clear them */
    NCCLCHECKIGNORE(wrap_ibv_destroy_qp(this->controlQp));
    NCCLCHECKIGNORE(wrap_ibv_destroy_qp(this->dataQp));
  }
}

bool ctranIb::impl::vc::isReady() {
  bool r;

  this->m.lock();
  r = this->isReady_;
  this->m.unlock();

  return r;
}

void ctranIb::impl::vc::setReady() {
  this->m.lock();
  this->isReady_ = true;
  this->m.unlock();
}

std::size_t ctranIb::impl::vc::getBusCardSize() {
  return sizeof(struct busCard);
}

ncclResult_t ctranIb::impl::vc::getLocalBusCard(void *localBusCard) {
  ncclResult_t res = ncclSuccess;
  struct busCard *busCard = reinterpret_cast<struct busCard *>(localBusCard);

  struct ibv_port_attr portAttr;
  NCCLCHECKIGNORE(wrap_ibv_query_port(this->context, this->port, &portAttr));
  this->maxMsgSize = portAttr.max_msg_sz;

  /* create QP */
  struct ibv_qp_init_attr initAttr;
  memset(&initAttr, 0, sizeof(struct ibv_qp_init_attr));
  initAttr.send_cq = this->cq;
  initAttr.recv_cq = this->cq;
  initAttr.qp_type = IBV_QPT_RC;
  initAttr.sq_sig_all = 0;
  initAttr.cap.max_send_wr = MAX_SEND_WR;
  initAttr.cap.max_recv_wr = MAX_CONTROL_MSGS;
  initAttr.cap.max_send_sge = 1;
  initAttr.cap.max_recv_sge = 1;
  initAttr.cap.max_inline_data = 0;
  NCCLCHECKGOTO(wrap_ibv_create_qp(&this->controlQp, this->pd, &initAttr), res, exit);
  NCCLCHECKGOTO(wrap_ibv_create_qp(&this->dataQp, this->pd, &initAttr), res, exit);

  /* set QP to INIT state */
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_INIT;
  qpAttr.pkey_index = ncclParamIbPkey();
  qpAttr.port_num = this->port;
  qpAttr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
  NCCLCHECKGOTO(
    wrap_ibv_modify_qp(this->controlQp, &qpAttr,
                       IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS),
    res, exit);
  NCCLCHECKGOTO(
    wrap_ibv_modify_qp(this->dataQp, &qpAttr,
                       IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS),
    res, exit);

  /* create local business card */
  busCard->port = this->port;
  busCard->controlQpn = this->controlQp->qp_num;
  busCard->dataQpn = this->dataQp->qp_num;
  busCard->mtu = portAttr.active_mtu;

  union ibv_gid gid;
  NCCLCHECKGOTO(wrap_ibv_query_gid(this->context, this->port, ncclParamIbGidIndex(), &gid), res, exit);
  busCard->spn = gid.global.subnet_prefix;
  busCard->iid = gid.global.interface_id;

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::setupVc(void *busCard, uint32_t *controlQp, uint32_t *dataQp) {
  ncclResult_t res = ncclSuccess;
  struct ibv_qp_attr qpAttr;
  struct busCard *remoteBusCard = reinterpret_cast<struct busCard *>(busCard);

  *controlQp = this->controlQp->qp_num;
  *dataQp = this->dataQp->qp_num;

  /* set QP to RTR state */
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_RTR;
  qpAttr.path_mtu = remoteBusCard->mtu;
  qpAttr.rq_psn = 0;
  qpAttr.max_dest_rd_atomic = 1;
  qpAttr.min_rnr_timer = 12;
  qpAttr.ah_attr.is_global = 1;
  qpAttr.ah_attr.grh.dgid.global.subnet_prefix = remoteBusCard->spn;
  qpAttr.ah_attr.grh.dgid.global.interface_id = remoteBusCard->iid;
  qpAttr.ah_attr.grh.flow_label = 0;
  qpAttr.ah_attr.grh.sgid_index = ncclParamIbGidIndex();
  qpAttr.ah_attr.grh.hop_limit = 255;
  qpAttr.ah_attr.grh.traffic_class = ncclParamIbTc();
  qpAttr.ah_attr.sl = ncclParamIbSl();
  qpAttr.ah_attr.src_path_bits = 0;
  qpAttr.ah_attr.port_num = remoteBusCard->port;

  qpAttr.dest_qp_num = remoteBusCard->controlQpn;
  NCCLCHECKGOTO(
    wrap_ibv_modify_qp(this->controlQp, &qpAttr,
                       IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                       IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER), res, exit);

  qpAttr.dest_qp_num = remoteBusCard->dataQpn;
  NCCLCHECKGOTO(
    wrap_ibv_modify_qp(this->dataQp, &qpAttr,
                       IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                       IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER), res, exit);

  /* set QP to RTS state */
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_RTS;
  qpAttr.timeout = ncclParamIbTimeout();
  qpAttr.retry_cnt = ncclParamIbRetryCnt();
  qpAttr.rnr_retry = 7;
  qpAttr.sq_psn = 0;
  qpAttr.max_rd_atomic = 1;
  NCCLCHECKGOTO(
    wrap_ibv_modify_qp(this->controlQp, &qpAttr,
                       IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                       IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC), res, exit);
  NCCLCHECKGOTO(
    wrap_ibv_modify_qp(this->dataQp, &qpAttr,
                       IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                       IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC), res, exit);

  /* post control WQEs */
  for (int i = 0; i < MAX_CONTROL_MSGS; i++) {
    NCCLCHECKGOTO(this->postRecvCtrlMsg(&this->recvCtrl.cmsg[i]), res, exit);
    this->recvCtrl.postedMsg.push_back(&this->recvCtrl.cmsg[i]);

    NCCLCHECKGOTO(this->postRecvNotifyMsg(), res, exit);
  }

  this->setReady();

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::postRecvCtrlMsg(struct controlMsg *cmsg) {
  ncclResult_t res = ncclSuccess;

  struct ibv_sge sg;
  memset(&sg, 0, sizeof(sg));
  sg.addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cmsg));
  sg.length = sizeof(struct controlMsg);
  sg.lkey = this->recvCtrl.mr->lkey;

  struct ibv_recv_wr postWr, *badWr;
  memset(&postWr, 0, sizeof(postWr));
  postWr.wr_id = 0;
  postWr.next = nullptr;
  postWr.sg_list = &sg;
  postWr.num_sge = 1;
  NCCLCHECKGOTO(wrap_ibv_post_recv(this->controlQp, &postWr, &badWr), res, exit);

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::postSendCtrlMsg(struct controlMsg *cmsg) {
  ncclResult_t res = ncclSuccess;

  struct ibv_sge sg;
  memset(&sg, 0, sizeof(sg));
  sg.addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cmsg));
  sg.length = sizeof(struct controlMsg);
  sg.lkey = this->sendCtrl.mr->lkey;

  struct ibv_send_wr postWr, *badWr;
  memset(&postWr, 0, sizeof(postWr));
  postWr.wr_id = 0;
  postWr.next = nullptr;
  postWr.sg_list = &sg;
  postWr.num_sge = 1;
  postWr.opcode = IBV_WR_SEND;
  postWr.send_flags = IBV_SEND_SIGNALED;
  NCCLCHECKGOTO(wrap_ibv_post_send(this->controlQp, &postWr, &badWr), res, exit);

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::postPutMsg(const void *sbuf, void *dbuf, std::size_t len_,
    uint32_t lkey, uint32_t rkey, bool localNotify, bool notify) {
  ncclResult_t res = ncclSuccess;

  uint64_t offset = 0;
  uint64_t len = len_;

  while (len > 0) {
    uint64_t toSend = std::min(len, static_cast<uint64_t>(this->maxMsgSize));

    struct ibv_sge sg;
    memset(&sg, 0, sizeof(sg));
    sg.addr = reinterpret_cast<uint64_t>(sbuf) + offset;
    sg.length = toSend;
    sg.lkey = lkey;

    struct ibv_send_wr wr, *badWr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 0;
    wr.next = nullptr;
    wr.sg_list = &sg;
    wr.num_sge = 1;

    if (len > this->maxMsgSize) {
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.send_flags = 0;
    } else {
      if (notify == true) {
        wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      } else {
        wr.opcode = IBV_WR_RDMA_WRITE;
      }

      if (localNotify == true) {
        wr.send_flags = IBV_SEND_SIGNALED;
      } else {
        wr.send_flags = 0;
      }
    }
    wr.wr.rdma.remote_addr = reinterpret_cast<uint64_t>(dbuf) + offset;
    wr.wr.rdma.rkey = rkey;

    NCCLCHECKGOTO(wrap_ibv_post_send(this->dataQp, &wr, &badWr), res, exit);

    len -= toSend;
    offset += toSend;
  }

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::postRecvNotifyMsg() {
  ncclResult_t res = ncclSuccess;

  struct ibv_recv_wr wr, *badWr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0;
  wr.next = nullptr;
  wr.num_sge = 0;
  NCCLCHECKGOTO(wrap_ibv_post_recv(this->dataQp, &wr, &badWr), res, exit);

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::processCqe(enum ibv_wc_opcode opcode, uint64_t wrId) {
  ncclResult_t res = ncclSuccess;

  switch (opcode) {
    case IBV_WC_SEND:
      {
        auto req = this->sendCtrl.postedReq.front();
        this->sendCtrl.postedReq.pop_front();
        req->complete();

        auto cmsg = this->sendCtrl.postedMsg.front();
        this->sendCtrl.postedMsg.pop_front();
        this->sendCtrl.freeMsg.push_back(cmsg);

        if (!this->sendCtrl.enqueuedWr.empty()) {
          auto enqueuedWr = this->sendCtrl.enqueuedWr.front();
          this->sendCtrl.enqueuedWr.pop_front();

          cmsg = this->sendCtrl.freeMsg.front();
          this->sendCtrl.freeMsg.pop_front();

          cmsg->remoteAddr = reinterpret_cast<uint64_t>(enqueuedWr->enqueued.send.buf);
          cmsg->rkey = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(enqueuedWr->enqueued.send.ibRegElem));
          NCCLCHECKGOTO(this->postSendCtrlMsg(cmsg), res, exit);

          this->sendCtrl.postedMsg.push_back(cmsg);
          this->sendCtrl.postedReq.push_back(req);
          delete enqueuedWr;
        }
      }
      break;

    case IBV_WC_RECV:
      {
        auto cmsg = this->recvCtrl.postedMsg.front();
        this->recvCtrl.postedMsg.pop_front();

        if (this->recvCtrl.enqueuedWr.empty()) {
          /* unexpected message */
          struct controlWr *unexWr = new struct controlWr;
          unexWr->enqueued.unex.remoteAddr = cmsg->remoteAddr;
          unexWr->enqueued.unex.rkey = cmsg->rkey;
          this->recvCtrl.unexWr.push_back(unexWr);
        } else {
          auto enqueuedWr = this->recvCtrl.enqueuedWr.front();
          this->recvCtrl.enqueuedWr.pop_front();

          *(enqueuedWr->enqueued.recv.buf) = reinterpret_cast<void *>(cmsg->remoteAddr);
          enqueuedWr->enqueued.recv.key->rkey = cmsg->rkey;
          enqueuedWr->enqueued.recv.req->complete();

          delete enqueuedWr;
        }

        NCCLCHECKGOTO(this->postRecvCtrlMsg(cmsg), res, exit);
        this->recvCtrl.postedMsg.push_back(cmsg);
      }
      break;

    case IBV_WC_RDMA_WRITE:
      {
        auto req = this->put.postedWr.front();
        this->put.postedWr.pop_front();
        req->complete();
      }
      break;

    case IBV_WC_RECV_RDMA_WITH_IMM:
      {
        this->notifications++;
        NCCLCHECKGOTO(this->postRecvNotifyMsg(), res, exit);
      }
      break;

    default:
      WARN("CTRAN-IB: Found unknown opcode: %d", opcode);
      res = ncclSystemError;
      goto exit;
  }

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::isendCtrl(void *buf, void *ibRegElem, ctranIbRequest *req) {
  ncclResult_t res = ncclSuccess;

  if (this->sendCtrl.freeMsg.empty()) {
    auto enqueuedWr = new struct controlWr;
    enqueuedWr->enqueued.send.buf = buf;
    enqueuedWr->enqueued.send.ibRegElem = ibRegElem;
    enqueuedWr->enqueued.send.req = req;
    this->sendCtrl.enqueuedWr.push_back(enqueuedWr);
  } else {
    auto cmsg = this->sendCtrl.freeMsg.front();
    this->sendCtrl.freeMsg.pop_front();

    cmsg->remoteAddr = reinterpret_cast<uint64_t>(buf);
    cmsg->rkey = reinterpret_cast<struct ibv_mr *>(ibRegElem)->rkey;
    NCCLCHECKGOTO(this->postSendCtrlMsg(cmsg), res, exit);
    this->sendCtrl.postedMsg.push_back(cmsg);
    this->sendCtrl.postedReq.push_back(req);
  }

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::irecvCtrl(void **buf, struct ctranIbRemoteAccessKey *key, ctranIbRequest *req) {
  ncclResult_t res = ncclSuccess;

  if (this->recvCtrl.unexWr.empty()) {
    auto enqueuedWr = new struct controlWr;
    enqueuedWr->enqueued.recv.buf = buf;
    enqueuedWr->enqueued.recv.key = key;
    enqueuedWr->enqueued.recv.req = req;
    this->recvCtrl.enqueuedWr.push_back(enqueuedWr);
  } else {
    auto unexWr = this->recvCtrl.unexWr.front();
    this->recvCtrl.unexWr.pop_front();

    *buf = reinterpret_cast<void *>(unexWr->enqueued.unex.remoteAddr);
    key->rkey = unexWr->enqueued.unex.rkey;
    req->complete();

    delete unexWr;
  }

  return res;
}

ncclResult_t ctranIb::impl::vc::iput(const void *sbuf, void *dbuf, std::size_t len, void *ibRegElem,
    struct ctranIbRemoteAccessKey remoteAccessKey, bool notify, ctranIbRequest *req) {
  ncclResult_t res = ncclSuccess;
  struct ibv_mr *smr;
  uint32_t rkey;

  smr = reinterpret_cast<struct ibv_mr *>(ibRegElem);
  if (smr == nullptr) {
    WARN("CTRAN-IB: memory registration not found for addr %p", sbuf);
    res = ncclSystemError;
    goto exit;
  }

  rkey = remoteAccessKey.rkey;

  bool localNotify;
  if (req != nullptr) {
    localNotify = true;
    this->put.postedWr.push_back(req);
  } else {
    localNotify = false;
  }

  NCCLCHECKGOTO(this->postPutMsg(sbuf, dbuf, len, smr->lkey, rkey, localNotify, notify), res, exit);

exit:
  return res;
}

ncclResult_t ctranIb::impl::vc::checkNotify(bool *notify) {
  ncclResult_t res = ncclSuccess;

  if (this->notifications) {
    *notify = true;
    this->notifications--;
  } else {
    *notify = false;
  }

  return res;
}
