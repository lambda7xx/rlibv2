#pragma once

#include "common.hpp"
#include "rnic.hpp"
#include "pre_connector.hpp"
#include "util.hpp"

#include <mutex>
#include <map>

namespace rdmaio {

class MemoryFlags {
 public:
  MemoryFlags() = default;

  MemoryFlags &set_flags(int flags) {
    protection_flags = flags;
    return *this;
  }

  int get_flags() const { return protection_flags; }

  MemoryFlags &clear_flags() {
    return set_flags(0);
  }

  MemoryFlags &add_local_write() {
    protection_flags |= IBV_ACCESS_LOCAL_WRITE;
    return *this;
  }

  MemoryFlags &add_remote_write() {
    protection_flags |= IBV_ACCESS_REMOTE_WRITE;
    return *this;
  }

  MemoryFlags &add_remote_read() {
    protection_flags |= IBV_ACCESS_REMOTE_READ;
    return *this;
  }

 private:
  int protection_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | \
      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
};

/**
 * Simple wrapper over ibv_mr struct
 */
class RemoteMemory {
 public:
  RemoteMemory(const char *addr,uint64_t size,
               const RNic &rnic,
               const MemoryFlags &flags)
      : addr(addr),size(size) {
    if (rnic.ready()) {
      mr = ibv_reg_mr(rnic.pd,(void *)addr,size,flags.get_flags());
    }
  }

  bool valid() const {
    return (mr != nullptr);
  }

  ~RemoteMemory() {
    if(mr != nullptr)
      ibv_dereg_mr(mr);
  }

  struct Attr {
    uintptr_t  buf;
    uint32_t   key;
  };

  Attr get_attr() const {
    auto key = valid() ? mr->rkey : 0;
    return { .buf = (uintptr_t)(addr), .key = key };
  }

 private:
  const char    *addr = nullptr;
  uint64_t       size;
  ibv_mr         *mr = nullptr;    // mr in the driver
}; // class remote memory

/**
 * The MemoryFactor manages all registered memory of the system
 */
class RdmaCtrl;
class RMemoryFactory {
  friend class RdmaCtrl;
 public:
  RMemoryFactory() = default;
  ~RMemoryFactory() {
    for(auto it = registered_mrs.begin();it != registered_mrs.end();++it)
      delete it->second;
  }

  IOStatus register_mr(int mr_id,
                       const char *addr,int size,RNic &rnic,const MemoryFlags flags = MemoryFlags()) {
    std::lock_guard<std::mutex> lk(this->lock);
    if(registered_mrs.find(mr_id) != registered_mrs.end())
      return WRONG_ID;
    RDMA_ASSERT(rnic.ready());
    registered_mrs.insert(std::make_pair(mr_id, new RemoteMemory(addr,size,rnic,flags)));
    RDMA_ASSERT(rnic.ready());

    if(registered_mrs[mr_id]->valid())
      return SUCC;

    registered_mrs.erase(registered_mrs.find(mr_id));
    return ERR;
  }

  static IOStatus fetch_remote_mr(int mr_id,const MacID &id,
                                  RemoteMemory::Attr &attr,
                                  const Duration_t &timeout = default_timeout) {
    Buf_t reply = Marshal::get_buffer(sizeof(ReplyHeader) + sizeof(RemoteMemory::Attr));
    auto ret = send_request(id,REQ_MR,Marshal::serialize_to_buf(static_cast<uint64_t>(mr_id)),
                            reply,timeout);
    if(ret == SUCC) {
      // further we check the reply header
      ReplyHeader header = Marshal::deserialize<ReplyHeader>(reply);
      if(header.reply_status == SUCC) {
        reply = Marshal::forward(reply,sizeof(ReplyHeader),reply.size() - sizeof(ReplyHeader));
        attr  = Marshal::deserialize<RemoteMemory::Attr>(reply);
      } else
        ret = static_cast<IOStatus>(header.reply_status);
    }
    return ret;
  }

  void deregister_mr(int mr_id) {
    std::lock_guard<std::mutex> lk(this->lock);
    auto it = registered_mrs.find(mr_id);
    if(it == registered_mrs.end())
      return;
    delete it->second;
    registered_mrs.erase(it);
  }

  RemoteMemory *get_mr(int mr_id) {
    std::lock_guard<std::mutex> lk(this->lock);
    if(registered_mrs.find(mr_id) != registered_mrs.end())
      return registered_mrs[mr_id];
  }

 private:
  std::map<int,RemoteMemory *>   registered_mrs;
  std::mutex lock;

 private:
  // fetch the MR attribute from the registered mrs
  Buf_t get_mr_attr(uint64_t id) {
    std::lock_guard<std::mutex> lk(this->lock);
    if(registered_mrs.find(id) == registered_mrs.end()) {
      return "";
    }
    auto attr = registered_mrs[id]->get_attr();
    return Marshal::serialize_to_buf(attr);
  }

  /** The RPC handler for the mr request
   * @Input = req:
   * - the attribute of MR the requester wants to fetch
   */
  Buf_t get_mr_handler(const Buf_t &req) {

    if (req.size() != sizeof(uint64_t))
      return Marshal::null_reply();

    uint64_t mr_id;
    bool res = Marshal::deserialize(req,mr_id);
    if(!res)
      return Marshal::null_reply();

    ReplyHeader reply = { .reply_status = SUCC,.reply_payload = sizeof(RemoteMemory::Attr) };

    auto mr = get_mr_attr(mr_id);

    if (mr.size() == 0) {
      reply.reply_status = NOT_READY;
      reply.reply_payload = 0;
    }

    // finally generate the reply
    auto reply_buf = Marshal::serialize_to_buf(reply);
    reply_buf.append(mr);
    return reply_buf;
  }
};

} // end namespace rdmaio