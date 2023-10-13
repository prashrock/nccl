// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "ctranGpe.h"
#include "ctranGpeImpl.h"
#include "ctranGpeKernel.h"
#include "checks.h"
#include <iostream>

ctranGpe::impl::impl() {
  CUDACHECKIGNORE(cudaHostAlloc(&this->kernelFlag, sizeof(int), cudaHostAllocDefault));
  *(this->kernelFlag) = 0;
}

ctranGpe::impl::~impl() {
  CUDACHECKIGNORE(cudaFreeHost(this->kernelFlag));
}

ncclResult_t ctranGpe::impl::enqueue(ctranGpeCmd::typeEnum type, std::unique_ptr<struct collOp> op,
    cudaStream_t stream) {
  ncclResult_t res = ncclSuccess;

  struct ctranGpeCmd *cmd = new struct ctranGpeCmd;
  cmd->type = type;

  if (type == ctranGpeCmd::typeEnum::GRAPH_ENQUEUE) {
    cmd->coll.op = std::move(op);
    cmd->coll.stream = stream;

    /* Enqueue the kernel.  It will not start till all other
     * operations on this stream have completed. */
    dim3 grid = { 1, 1, 1 };
    dim3 blocks = { 1, 1, 1 };
    void *args[] = { &this->kernelFlag };
    CUDACHECKGOTO(
        cudaLaunchKernel(cmd->coll.op->ncclKernel, grid, blocks, args, 0, cmd->coll.stream),
        res, exit);
  }

  this->m.lock();
  cmdQueue.push(cmd);
  this->m.unlock();
  c.notify_one();

exit:
  return res;
}

void ctranGpe::impl::gpeThreadFn(ctranGpe::impl *pimpl, int cudaDev) {
  CUDACHECKIGNORE(cudaSetDevice(cudaDev));

  while (1) {
    /* if we have no more commands left to process, wait for a signal */
    if (pimpl->internalCmdQueue.empty()) {
      std::unique_lock<std::mutex> lk(pimpl->m);
      pimpl->c.wait(lk, [&] { return !pimpl->cmdQueue.empty(); } );
      pimpl->internalCmdQueue.push(pimpl->cmdQueue.front());
      pimpl->cmdQueue.pop();
      pimpl->m.unlock();
    }

    auto cmd = pimpl->internalCmdQueue.front();
    pimpl->internalCmdQueue.pop();
    if (cmd->type == ctranGpeCmd::typeEnum::TERMINATE) {
      goto exit;
    }

    /* wait for the kernel to launch */
    volatile int *flag_d = pimpl->kernelFlag;
    while (*flag_d != KERNEL_STARTED);

    /* run collective */
    NCCLCHECKIGNORE(cmd->coll.op->func(cmd->coll.op.get()));

    /* stop kernel */
    *flag_d = KERNEL_TERMINATE;

    delete cmd;
  }

exit:
  return;
}
