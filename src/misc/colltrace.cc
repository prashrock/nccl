#ifdef ENABLE_COLLTRACE
#include "colltrace.h"
#include "internal.h"

#include <unistd.h>
#include <chrono>
#include <sstream>
#include <string>
#include <fstream>

#ifndef COLLTRACE_IO_FB
#define COLLTRACE_IO_FB
#endif

void CollTrace::outputResults(){
  std::stringstream stream;
  stream << "[\n  {\n";
  for (auto it = results_.begin(); it != results_.end(); ++it) {
    if (it != results_.begin()) {
      stream << "  },\n  {\n";
    }
    stream << "    \"coll\": \"" << it->info.opName << "\",\n"
          << "    \"msg_size\": \""
          << (it->info.count * ncclTypeSize(it->info.datatype)) << "\",\n"
          << "    \"latency\": " << it->latency << "\n";
  }
  stream << "  }\n]";

  std::string fileName = std::to_string(rank_) + "_online.json";

  // If local env variable is set, then write profiling data to file
  char* subDirEnv = getenv("NCCL_COLLTRACE_LOCAL_SUBDIR");
  if(subDirEnv){
    std::string localSubDir(subDirEnv);
    INFO(NCCL_ALL, "Rank %lu: Writing %lu online profiler data to local directory: %s", rank_, results_.size(), localSubDir.c_str());
    std::ofstream out(localSubDir + "/" + fileName);
    out << stream.str();
    out.close();
  }

  COLLTRACE_IO_FB;
}

void* CollTrace::measureLatency() {
  INFO(NCCL_INIT, "Rank %lu: Started CollTrace worker thread", rank_);

  while (true) {
    std::unique_ptr<EventInfo> curEvent = eventQueue_.tryPop();
    if(curEvent){
      if (curEvent->info.count != 0) {
        CUDACHECKIGNORE(cudaEventSynchronize(curEvent->stop.get()));
        float latency;
        CUDACHECKIGNORE(cudaEventElapsedTime(&latency, curEvent->start.get(), curEvent->stop.get()));
        ResultInfo result;
        result.info = curEvent->info;
        result.latency = latency;
        results_.push_back(result);
        eventPool_.add(std::move(curEvent->start));
        eventPool_.add(std::move(curEvent->stop));
      }
    } else {
      if (workerThreadExitSignal_ && eventQueue_.isEmpty()) {
        outputResults();
        break;
      }
    }
  }

  return NULL;
}

void* CollTrace::measureLatencyWrapper(CollTrace* collTrace){
  return collTrace->measureLatency();
}

ncclResult_t CollTrace::startWorkerThread(int rank) {
  // create worker thread
  rank_ = rank;
  profilingWorkerThread_ = std::thread{ measureLatencyWrapper, this };

  return ncclSuccess;
}

std::unique_ptr<EventInfo> CollTrace::getEventFromPool(){
  std::unique_ptr<EventInfo> eventInfo(new EventInfo);
  eventInfo->start = eventPool_.takeOne();
  eventInfo->stop = eventPool_.takeOne();
  if(!eventInfo->start || !eventInfo->stop){
    std::unique_ptr<EventInfo> nullEventInfo(nullptr);
    return nullEventInfo;
  }
  return eventInfo;
}

ncclResult_t CollTrace::enqueueEvent(std::unique_ptr<EventInfo> eventInfo) {
  eventQueue_.push(std::move(eventInfo));

  return ncclSuccess;
}

ncclResult_t CollTrace::exit() {
  workerThreadExitSignal_ = true;
  profilingWorkerThread_.join();

  return ncclSuccess;
}
#endif