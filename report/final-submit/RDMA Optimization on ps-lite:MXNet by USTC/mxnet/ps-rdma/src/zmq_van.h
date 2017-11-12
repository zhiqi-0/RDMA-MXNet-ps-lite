/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_ZMQ_VAN_H_
#define PS_ZMQ_VAN_H_
#include <zmq.h>
#include <stdlib.h>
#include <thread>
#include <string>
#include "ps/internal/van.h"
#if _MSC_VER
#define rand_r(x) rand()
#endif

namespace ps {
/**
 * \brief be smart on freeing recved data
 */
inline void FreeData(void *data, void *hint) {
  if (hint == NULL) {
    delete [] static_cast<char*>(data);
  } else {
    delete static_cast<SArray<char>*>(hint);
  }
}

/**
 * \brief ZMQ based implementation
 */
class ZMQVan : public Van {
 public:
  ZMQVan() { }
  virtual ~ZMQVan() { }

 protected:
  void Start() override {
    // start zmq
    context_ = zmq_ctx_new();
    CHECK(context_ != NULL) << "create 0mq context failed";
    zmq_ctx_set(context_, ZMQ_MAX_SOCKETS, 65536);
    // zmq_ctx_set(context_, ZMQ_IO_THREADS, 4);
    Van::Start();
  }
  //=================================== RDMA =======================================
  /* close qp */
  void RDMACloseQP(struct qp_context* qp_ctx) {
    if (!qp_ctx) return;
    if (ibv_destroy_qp(qp_ctx->qp)) {
      fprintf(stderr, "Couldn't destory QP\n");
      return;
    }
    if (ibv_dereg_mr(qp_ctx->send_mr)) {
      fprintf(stderr, "Couldn't deregister MR\n");
      return;
    }
    if (ibv_dereg_mr(qp_ctx->recv_mr)) {
      fprintf(stderr, "Couldn't deregister MR\n");
      return;
    }
    free(qp_ctx->send_buf);
    free(qp_ctx->recv_buf);
    delete qp_ctx;
  }
  //================================== RDMA end ====================================
  
  void close_zmq_van() override {
    int linger = 0;
    int rc = zmq_setsockopt(receiver_, ZMQ_LINGER, &linger, sizeof(linger));
    CHECK(rc == 0 || errno == ETERM);
    CHECK_EQ(zmq_close(receiver_), 0);
    for (auto& it : senders_) {
      int rc = zmq_setsockopt(it.second, ZMQ_LINGER, &linger, sizeof(linger));
      CHECK(rc == 0 || errno == ETERM);
      CHECK_EQ(zmq_close(it.second), 0);
    }
    zmq_ctx_destroy(context_);
  }

  void Stop() override {
    PS_VLOG(1) << my_node_.ShortDebugString() << " is stopping";
    Van::Stop();
    // ============================================= RDMA ====================================
    for (auto& it : ctx_->qps) {
      RDMACloseQP(it.second);
    }
    RDMACloseQP(to_self[0]);
    RDMACloseQP(to_self[1]);
    if (ibv_dealloc_pd(ctx_->pd)) {
      fprintf(stderr, "Couldn't deallocate PD\n");
    }
    if (ibv_destroy_cq(ctx_->send_cq)) {
      fprintf(stderr, "Couldn't destory send CQ\n");
    }
    if (ibv_destroy_cq(ctx_->recv_cq)) {
      fprintf(stderr, "Couldn't destory recv CQ\n");
    }
    if (ibv_destroy_comp_channel(ctx_->channel)) {
      fprintf(stderr, "Couldn't destory channel\n");
    }
    if (ibv_close_device(ctx_->ib_ctx)) {
      fprintf(stderr, "Couldn't release context\n");
    }
    ibv_free_device_list(ctx_->dev_list);
    delete ctx_;
    //============================================== RDMA end ================================
    
  }

  int Bind(const Node& node, int max_retry) override {
    receiver_ = zmq_socket(context_, ZMQ_ROUTER);
    CHECK(receiver_ != NULL)
        << "create receiver socket failed: " << zmq_strerror(errno);
    int local = GetEnv("DMLC_LOCAL", 0);
    std::string addr = local ? "ipc:///tmp/" : "tcp://*:";
    int port = node.port;
    unsigned seed = static_cast<unsigned>(time(NULL)+port);
    for (int i = 0; i < max_retry+1; ++i) {
      auto address = addr + std::to_string(port);
      if (zmq_bind(receiver_, address.c_str()) == 0) break;
      if (i == max_retry) {
        port = -1;
      } else {
        port = 10000 + rand_r(&seed) % 40000;
      }
    }
    return port;
  }
  //===================================== RDMA =========================================
  /* Modify RDMA QP Pairs to RTS Status */
  int RDMAConnect(struct qp_context* qp_ctx) {
    {
      /* Modify RDMA QP Pairs from init status to RTR status */
      struct ibv_qp_attr attr;
      memset(&attr, 0, sizeof(attr));
		  attr.qp_state                 = IBV_QPS_RTR;
			attr.path_mtu                 = IBV_MTU_4096;
			attr.dest_qp_num              = qp_ctx->dest_info.qpn;
			attr.rq_psn                   = 0;
			attr.max_dest_rd_atomic       = 0;
			attr.min_rnr_timer            = 12;
			attr.ah_attr.is_global        = 0;
			attr.ah_attr.dlid             = qp_ctx->dest_info.lid;
		  attr.ah_attr.sl               = 0;
		  attr.ah_attr.src_path_bits    = 0;
		  attr.ah_attr.port_num         = qp_ctx->dest_info.port;
		  int flags = IBV_QP_STATE        |
		  			IBV_QP_AV                 |
			  		IBV_QP_PATH_MTU	          |
				  	IBV_QP_DEST_QPN	          |
				  	IBV_QP_RQ_PSN	            |
				  	IBV_QP_MAX_DEST_RD_ATOMIC	|
				  	IBV_QP_MIN_RNR_TIMER;
      if (ibv_modify_qp(qp_ctx->qp, &attr, flags)) {
        fprintf(stderr, "Couldn't modify QP to RTR\n");
        return 1;
      }
    }
    {
      /* Modify RDMA QP Pairs from RTR status to RTS status */
      struct ibv_qp_attr attr;
      memset(&attr, 0, sizeof(attr));
      attr.qp_state = IBV_QPS_RTS;
      attr.timeout = 12;
      attr.retry_cnt = 7;
      attr.rnr_retry = 7;
      attr.sq_psn = 0;
      attr.max_rd_atomic = 0;
      int flags = IBV_QP_STATE      |
		              IBV_QP_TIMEOUT    |
		              IBV_QP_RETRY_CNT  |
		              IBV_QP_RNR_RETRY  |
		              IBV_QP_SQ_PSN     |
		              IBV_QP_MAX_QP_RD_ATOMIC;
      if (ibv_modify_qp(qp_ctx->qp, &attr, flags)) {
        fprintf(stderr, "Couldn't modify QP to RTS\n");
        return 1;
      }
    }
    return 0;
  }
  //================================= RDMA end =============================================

  void Connect(const Node& node) override {
    CHECK_NE(node.id, node.kEmpty);
    CHECK_NE(node.port, node.kEmpty);
    CHECK(node.hostname.size());
    int id = node.id;
    auto it = senders_.find(id);
    if (it != senders_.end()) {
      zmq_close(it->second);
    }
    // worker doesn't need to connect to the other workers. same for server
    if ((node.role == my_node_.role) &&
        (node.id != my_node_.id)) {
      return;
    }
    void *sender = zmq_socket(context_, ZMQ_DEALER);
    CHECK(sender != NULL)
        << zmq_strerror(errno)
        << ". it often can be solved by \"sudo ulimit -n 65536\""
        << " or edit /etc/security/limits.conf";
    if (my_node_.id != Node::kEmpty) {
      std::string my_id = "ps" + std::to_string(my_node_.id);
      zmq_setsockopt(sender, ZMQ_IDENTITY, my_id.data(), my_id.size());
    }
    // connect
    std::string addr = "tcp://" + node.hostname + ":" + std::to_string(node.port);
    if (GetEnv("DMLC_LOCAL", 0)) {
      addr = "ipc:///tmp/" + std::to_string(node.port);
    }
    if (zmq_connect(sender, addr.c_str()) != 0) {
      LOG(FATAL) <<  "connect to " + addr + " failed: " + zmq_strerror(errno);
    }
    senders_[id] = sender;
  }

  //====================================== RDMA ========================================
  int van_post_send(struct qp_context* qp_ctx, int size, bool terminate, bool wc) {
    /* if the remote recv buffer has been used up, then set offset to zero. the recver use the same method */
    bool buf_begin = false;
    if (size + qp_ctx->dest_info.current_p > ctx_->size) {
      qp_ctx->dest_info.current_p = 0;
      buf_begin = true;
    }
    /* set sge attribute */
    struct ibv_sge list;
    memset(&list, 0, sizeof(list));
    list.addr   = (uint64_t)qp_ctx->send_buf + qp_ctx->current_send_p;
    list.length = size;
    list.lkey   = qp_ctx->send_mr->lkey;
    /* set work request attribute */
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id               = DEMO_SEND_WRID;
    wr.sg_list             = &list;
    wr.num_sge             = 1;
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    /* if wc, generate a wc in send cq */
    wr.send_flags          = IBV_SEND_SOLICITED | (wc ? IBV_SEND_SIGNALED : 0);
    wr.wr.rdma.remote_addr = qp_ctx->dest_info.addr + qp_ctx->dest_info.current_p;
    wr.wr.rdma.rkey        = qp_ctx->dest_info.rkey;
    /* lowest bit: terminate */
    if (terminate) {
      wr.imm_data += 1;
    }
    /* second bit: buf offset set 0 */
    if (buf_begin) {
      wr.imm_data += 2;
    }
    struct ibv_send_wr *bad_wr;
    /* update the send buffer address, remote recv buffer address will be updated later in RDMASendMsg */
    qp_ctx->current_send_p += size;
    /* RDMA Post send */
    if(ibv_post_send(qp_ctx->qp, &wr, &bad_wr)){
      fprintf(stderr, "Couldn't post send WR\n");
      return -1;
    }
    return 0;
  }

  /* Get msg.data total bytes size. 
   * this method is required to decide whether 
   * to put message in send buf at current offset 
   * or offset 0, avoid extra memcpy 
   */
  int req_data_size(const Message& msg) {
    int size = 0;
    for (int i = 0; i < msg.data.size(); i++) {
      size += sizeof(int) + msg.data[i].size();
    }
    return size;
  }

  /* pack message into send buf */
  int PackMsg(void* buf, const Message& msg) {
    /* buf construct: meta_size, data_number, meta, data[1]_size, data[1], data[2]_size, data[2], ...*/
    int n = msg.data.size();
    int meta_size; char* meta_buf = (char*)buf + 2 * sizeof(int);
    PackMeta(msg.meta, &meta_buf, &meta_size);
#ifdef rdma_debug_info
    fprintf(stdout, "meta packed\n");
#endif
    *(int*)buf = meta_size;
    *((int*)(buf + sizeof(int))) = n;
    void* buf_tmp = buf + 2 * sizeof(int) + meta_size;
    /* total_bytes actually does not include data: only meta_size and two int */
    int total_bytes = meta_size + 2 * sizeof(int);
    for (int i = 0; i < n; i++) {
      int data_size = msg.data[i].size();
#ifdef rdma_debug_info
      fprintf(stdout, "\t\tdata size required, is %d\n", data_size);
#endif
      memcpy(buf_tmp + sizeof(int), msg.data[i].data(), data_size);
#ifdef rdma_debug_info
      fprintf(stdout, "\t\tdata %d copied\n", i);
#endif
      *(int*)buf_tmp = data_size;
      buf_tmp += data_size + sizeof(int);
    }
#ifdef rdma_debug_info
    fprintf(stdout, "data in total %d packed\n", n);
#endif
    return total_bytes;
  }

  /* RDMA Send Msg */
  int RDMASendMsg(const Message& msg) {
    int id = msg.meta.recver;
    struct qp_context* dest;
    if (id == my_node_.id) {
      dest = to_self[0];
    }
    else {
      auto it = ctx_->qps.find(id);
      if (it == ctx_->qps.end()) {
        fprintf(stdout, "there is no qp to node %d\n", id);
        return -1;
      }
      dest = it->second;
    }
#ifdef rdma_debug_info
    fprintf(stdout, "qp_ctx determined\n");
#endif
    /* lock the send buf so it won't be over write by other threads */
    std::lock_guard<std::mutex> lk(dest->send_mu_);
    int data_size = req_data_size(msg);
    /* determine whether the msg size will 
     * cross the send buffer border, if so, 
     * set offset to 0. meta size is less 
     * than 32 bytes 
     */
    if (data_size + 32 + 2 * sizeof(int) + dest->current_send_p > ctx_->send_size) {
      dest->current_send_p = 0;
    }
    /* pack msg into specified buffer address */
    int total_bytes = PackMsg(dest->send_buf + dest->current_send_p, msg) + data_size;
    dest->send_sig_count ++;
    /* post send request */
    if (van_post_send(dest, total_bytes, msg.meta.control.cmd == Control::TERMINATE, dest->send_sig_count == send_depth)) {
      fprintf(stdout, "Couldn't post send request\n");
      return -1;
    }
    /* if the number of RDMA send oprations 
     * increases to the threshold, then poll 
     * out a work completion from cq 
     */
    if (dest->send_sig_count == send_depth) {
      struct ibv_wc wc;
      int ne;
      do {
        ne = ibv_poll_cq(ctx_->send_cq, 1, &wc);
        if (ne < 0) {
          fprintf(stdout, "Couldn't complete send\n");
          return -1;
        }
      } while (ne < 1);
      /* check the work completion */
      if (wc.status != IBV_WC_SUCCESS) {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr;  
        if (ibv_query_qp(dest->qp, &attr,
              IBV_QP_STATE, &init_attr)) {
          fprintf(stderr, "Failed to query QP state\n");
          return -1;
        }
        fprintf(stderr, "%d Send to %d failed, status %d, qp_status %d\n", my_node_.id, msg.meta.recver, wc.status, attr.qp_state);
        return -1;
      }
      /* reset RDMA send operation count */
      dest->send_sig_count = 0;
    }
    /* update the remote recv buffer write address */
    dest->dest_info.current_p += total_bytes;
    
#ifdef rdma_debug_info
    sent_count++;
    fprintf(stdout, "%d to %d rdma sent success, num %d, %dB, req %d, tmstp %d\n", my_node_.id, msg.meta.recver, sent_count, total_bytes, msg.meta.request, msg.meta.timestamp);
#endif
    return total_bytes;
  }
  //====================================== RDMA end ========================================

  int SendMsg(const Message& msg) override {
#ifdef rdma_debug_info
    fprintf(stdout, "%d send msg to %d, command %d", my_node_.id, msg.meta.recver, msg.meta.control.cmd);
#endif
    //==================================== RDMA ====================================================
    /* if rdma ready, then use RDMASendMsg */
    if (rdma_ready_) {
#ifdef rdma_debug_info
      fprintf(stdout, " via rdma\n");
#endif
      return RDMASendMsg(msg);
    }
    //==================================== RDMA end ================================================
    std::lock_guard<std::mutex> lk(mu_);
    // find the socket
#ifdef rdma_debug_info
    fprintf(stdout, " via tcp\n");
#endif
    int id = msg.meta.recver;
    CHECK_NE(id, Meta::kEmpty);
    auto it = senders_.find(id);
    if (it == senders_.end()) {
      LOG(WARNING) << "there is no socket to node " << id;
      return -1;
    }
    void *socket = it->second;

    // send meta
    int meta_size; char* meta_buf;
    PackMeta(msg.meta, &meta_buf, &meta_size);
    int tag = ZMQ_SNDMORE;
    int n = msg.data.size();
    if (n == 0) tag = 0;
    zmq_msg_t meta_msg;
    zmq_msg_init_data(&meta_msg, meta_buf, meta_size, FreeData, NULL);
    while (true) {
      if (zmq_msg_send(&meta_msg, socket, tag) == meta_size) break;
      if (errno == EINTR) continue;
      LOG(WARNING) << "failed to send message to node [" << id
                   << "] errno: " << errno << " " << zmq_strerror(errno);
      return -1;
    }
    zmq_msg_close(&meta_msg);
#ifdef rdma_debug_info
    fprintf(stdout, "%d msg to %d send META done!\n", my_node_.id, msg.meta.recver);
#endif
    int send_bytes = meta_size;

    // send data
    for (int i = 0; i < n; ++i) {
      zmq_msg_t data_msg;
      SArray<char>* data = new SArray<char>(msg.data[i]);
      int data_size = data->size();
      zmq_msg_init_data(&data_msg, data->data(), data->size(), FreeData, data);
      if (i == n - 1) tag = 0;
      while (true) {
        if (zmq_msg_send(&data_msg, socket, tag) == data_size) break;
        if (errno == EINTR) continue;
        LOG(WARNING) << "failed to send message to node [" << id
                     << "] errno: " << errno << " " << zmq_strerror(errno)
                     << ". " << i << "/" << n;
        return -1;
      }
      zmq_msg_close(&data_msg);
      send_bytes += data_size;
    }
#ifdef rdma_debug_info
    sent_count++;
    fprintf(stdout, "%d msg to %d send success, num %d, %dB, req %d, tmstp %d\n", my_node_.id, msg.meta.recver, sent_count, send_bytes, msg.meta.request, msg.meta.timestamp);
#endif
    return send_bytes;
  }

  //==================================== RDMA ==========================================
  int van_post_recv(struct qp_context* qp_ctx, int n, int id) override {
    /* set sge attribute for recving msg */
    struct ibv_sge list;
    memset(&list, 0, sizeof(list));
    list.addr   = (uint64_t)qp_ctx->recv_buf;
    list.length = ctx_->size;
    list.lkey   = qp_ctx->recv_mr->lkey;
    /* set work request for recving msg */
    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = id;
    wr.sg_list = &list;
    wr.num_sge = 1;
    struct ibv_recv_wr *bad_wr;
    int i;
    int ret = 0;
    /* RDMA post recv request */
    for (i = 0; i < n; i++) {
      ret = ibv_post_recv(qp_ctx->qp, &wr, &bad_wr);
      if (ret) break;
    }
    if (i == n) return i;
    return ret;
  }

  /* Unpack msg from recv buffer, return recv bytes */
  int UnpackMsg(void* buf, Message* msg) {
    /* unpack Meta message */
    void* buf_tmp = buf;
    int meta_size = *(int*)buf_tmp;
    buf_tmp += sizeof(int);
    int data_num = *(int*)buf_tmp;
    buf_tmp += sizeof(int);
    UnpackMeta((char*)buf_tmp, meta_size, &(msg->meta));
    /* set msg meta recver */
    msg->meta.recver = my_node_.id;
    buf_tmp += meta_size;
    int total_bytes = meta_size + sizeof(int) * 2;
    /* reset msg data */
    for (int i = 0; i < data_num; i++) {
      int data_size = *(int*)buf_tmp;
      total_bytes += data_size + sizeof(int);
#ifdef rdma_debug_info
      fprintf(stdout, "\t\t\tdata_size get %d\n", data_size);
#endif
      SArray<char> data;
      data.reset((char*)(buf_tmp + sizeof(int)), data_size, [](char* buf) {});
#ifdef rdma_debug_info
      fprintf(stdout, "\t\t\tdata %d cpyed\n", i);
#endif
      msg->data.push_back(data);
      buf_tmp += sizeof(int) + data_size;
    }
    return total_bytes;
  }

  /* add msg into customer's queue and wait for being handled */
  int RDMARecvMsg(Message* msg) { 
    msg->data.clear();
    struct buf_node p;
    buf_queue.WaitAndPop(&p);

    int total_size = UnpackMsg(p.buf, msg);
    msg->meta.sender = p.sender;

#ifdef rdma_debug_info
    recv_count++;
    fprintf(stdout, "%d found a msg from %d via rdma, num %d\n", my_node_.id, msg->meta.sender, recv_count);
    fprintf(stdout, "\t%dB, cmd %d, req %d, tmstp %d\n", total_size, msg->meta.control.cmd, msg->meta.request, msg->meta.timestamp);
#endif
    return total_size;
  }
  //======================================= RDMA end =======================================
  
  int RecvMsg(Message* msg) override {
#ifdef rdma_debug_info
    fprintf(stdout, "%d wants a msg via", my_node_.id);
#endif
    if (rdma_ready_) {
#ifdef rdma_debug_info
      fprintf(stdout, " rdma\n");
#endif
      return RDMARecvMsg(msg);
    }
#ifdef rdma_debug_info
    fprintf(stdout, " tcp\n");
#endif
    msg->data.clear();
    size_t recv_bytes = 0;
    for (int i = 0; ; ++i) {
      zmq_msg_t* zmsg = new zmq_msg_t;
      CHECK(zmq_msg_init(zmsg) == 0) << zmq_strerror(errno);
      while (true) {
        if (zmq_msg_recv(zmsg, receiver_, 0) != -1) break;
        if (errno == EINTR) continue;
        LOG(WARNING) << "failed to receive message. errno: "
                     << errno << " " << zmq_strerror(errno);
        return -1;
      }
      char* buf = CHECK_NOTNULL((char *)zmq_msg_data(zmsg));
      size_t size = zmq_msg_size(zmsg);
      recv_bytes += size;

      if (i == 0) {
        // identify
        msg->meta.sender = GetNodeID(buf, size);
        msg->meta.recver = my_node_.id;
        CHECK(zmq_msg_more(zmsg));
        zmq_msg_close(zmsg);
        delete zmsg;
      } else if (i == 1) {
        // task
        UnpackMeta(buf, size, &(msg->meta));
        zmq_msg_close(zmsg);
        bool more = zmq_msg_more(zmsg);
        delete zmsg;
        if (!more) break;
      } else {
        // zero-copy
        SArray<char> data;
        data.reset(buf, size, [zmsg, size](char* buf) {
            zmq_msg_close(zmsg);
            delete zmsg;
          });
        msg->data.push_back(data);
        if (!zmq_msg_more(zmsg)) { break; }
      }
    }
#ifdef rdma_debug_info
    fprintf(stdout, "%d recved msg, num %d, %dB, req %d, tmstp %d\n", my_node_.id, ++recv_count, recv_bytes, msg->meta.request, msg->meta.timestamp);
#endif
    return recv_bytes;
  }
#ifdef rdma_debug_info
  int recv_count = 0;
  int sent_count = 0;
#endif

 private:
  /**
   * return the node id given the received identity
   * \return -1 if not find
   */
  int GetNodeID(const char* buf, size_t size) {
    if (size > 2 && buf[0] == 'p' && buf[1] == 's') {
      int id = 0;
      size_t i = 2;
      for (; i < size; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
          id = id * 10 + buf[i] - '0';
        } else {
          break;
        }
      }
      if (i == size) return id;
    }
    return Meta::kEmpty;
  }
  void *context_ = nullptr;
  /**
   * \brief node_id to the socket for sending data to this node
   */
  std::unordered_map<int, void*> senders_;
  //============================================================
  //============================================================
  std::mutex mu_;
  void *receiver_ = nullptr;
};
}  // namespace ps

#endif  // PS_ZMQ_VAN_H_




// monitors the liveness other nodes if this is
// a schedule node, or monitors the liveness of the scheduler otherwise
// aliveness monitor
// CHECK(!zmq_socket_monitor(
//     senders_[kScheduler], "inproc://monitor", ZMQ_EVENT_ALL));
// monitor_thread_ = std::unique_ptr<std::thread>(
//     new std::thread(&Van::Monitoring, this));
// monitor_thread_->detach();

// void Van::Monitoring() {
//   void *s = CHECK_NOTNULL(zmq_socket(context_, ZMQ_PAIR));
//   CHECK(!zmq_connect(s, "inproc://monitor"));
//   while (true) {
//     //  First frame in message contains event number and value
//     zmq_msg_t msg;
//     zmq_msg_init(&msg);
//     if (zmq_msg_recv(&msg, s, 0) == -1) {
//       if (errno == EINTR) continue;
//       break;
//     }
//     uint8_t *data = static_cast<uint8_t*>(zmq_msg_data(&msg));
//     int event = *reinterpret_cast<uint16_t*>(data);
//     // int value = *(uint32_t *)(data + 2);

//     // Second frame in message contains event address. it's just the router's
//     // address. no help

//     if (event == ZMQ_EVENT_DISCONNECTED) {
//       if (!is_scheduler_) {
//         PS_VLOG(1) << my_node_.ShortDebugString() << ": scheduler is dead. exit.";
//         exit(-1);
//       }
//     }
//     if (event == ZMQ_EVENT_MONITOR_STOPPED) {
//       break;
//     }
//   }
//   zmq_close(s);
// }
