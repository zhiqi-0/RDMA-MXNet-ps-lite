/**
 *  Copyright (c) 2015 by Contributors
 */
#include "ps/internal/van.h"
#include <thread>
#include <chrono>
#include "ps/base.h"
#include "ps/sarray.h"
#include "ps/internal/postoffice.h"
#include "ps/internal/customer.h"
#include "./network_utils.h"
#include "./meta.pb.h"
#include "./zmq_van.h"
#include "./resender.h"
#include <infiniband/verbs.h>
namespace ps {

/* RDMA Class Member definition
 *
 */
/* Initialize the static member */
// ---------------------------------------------------------------------------------------------
int RDMA::instance_num_ = 0;
int RDMA::num_devices_ = 0;

struct config_t RDMA::config_ = {
    NULL,
    1,      // ib_port
    800,     // cq_size
    1000,     // outstanding send request max
    1000,     // outstanding recv request max
    2,     // send_sge_max
    2,     // recv_sge_max
    SendBufSize,
    RecvBufSize
};
struct ibv_device                   **RDMA::dev_list_ = ibv_get_device_list(&RDMA::num_devices_);
struct ibv_device_attr              *RDMA::device_attr_ = NULL;
struct ibv_port_attr                *RDMA::port_attr_ = NULL;
struct ibv_context                  *RDMA::ib_ctx_ = NULL;
struct ibv_comp_channel             *RDMA::channel_ = NULL;
struct ibv_pd                       *RDMA::pd_ = NULL;                       
struct ibv_cq                       *RDMA::recv_cq_ = NULL;  
struct ibv_cq                       *RDMA::send_cq_ = NULL;
void                                *RDMA::send_buf_ = NULL;  
struct ibv_mr                       *RDMA::send_mr_ = NULL;
std::mutex                          RDMA::sendbufmu_;
size_t                              RDMA::sendbuf_offset_ = 0;
std::mutex                          RDMA::recvbufmu_;
std::queue<struct bufe>             RDMA::recv_queue_;
std::condition_variable             RDMA::cond_;   
// ---------------------------------------------------------------------------------------------


RDMA::RDMA(){
    ++instance_num_;
    RDMA_CHECK_NP(dev_list_, "ibv_device");
    RDMA_CHECK_NP(num_devices_, "num_devices_");
    send_times_ = 0;
    if(!ib_ctx_){
        ib_ctx_ = ibv_open_device(*dev_list_);
        RDMA_CHECK_NP(ib_ctx_, "ibv_open_device");
    }
    if(!device_attr_){
        device_attr_ = new struct ibv_device_attr;
        int rc = ibv_query_device(ib_ctx_, device_attr_);
        RDMA_CHECK_Z(rc, "ibv_query_device");
    }
    if(!port_attr_){
        port_attr_ = new struct ibv_port_attr;
        int rc = ibv_query_port(ib_ctx_, config_.ib_port, port_attr_);
        RDMA_CHECK_Z(rc, "ibv_query_port");
    } 
    if(!pd_){
        pd_ = ibv_alloc_pd(ib_ctx_);
        RDMA_CHECK_NP(pd_, "ibv_alloc_pd");
    }
    if(!channel_){
        channel_ = ibv_create_comp_channel(ib_ctx_);
        RDMA_CHECK_NP(channel_, "ibv_create_comp_channel");
    }
    if(!send_cq_){
      send_cq_ = ibv_create_cq(ib_ctx_, 1, NULL, NULL, 0);
      RDMA_CHECK_NP(send_cq_, "ibv_create_cq");
    }  
    if(!recv_cq_){
        recv_cq_ = ibv_create_cq(ib_ctx_, config_.cq_size, NULL, channel_, 0);
        RDMA_CHECK_NP(send_cq_, "ibv_create_cq");
    }
    if(!send_buf_){
      send_buf_ = malloc(config_.sendbuf_size);
      memset(send_buf_, 0, config_.sendbuf_size);
    }
    recv_buf_ = malloc(config_.recvbuf_size);
    memset(recv_buf_, 0, config_.recvbuf_size);
    if(!recv_buf_ || !send_buf_){
        fprintf(stderr, "failed to mafailed to malloc %Zu bytes to memory buffer.\n", config_.sendbuf_size);
        return;
    }
    int recv_mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    recv_mr_ = ibv_reg_mr(pd_, recv_buf_, config_.recvbuf_size, recv_mr_flags);
    if(!send_mr_){
      int send_mr_flags = IBV_ACCESS_LOCAL_WRITE;
      send_mr_ = ibv_reg_mr(pd_, send_buf_, config_.sendbuf_size, send_mr_flags);
    }
    if(!recv_mr_ || !send_mr_){
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", recv_mr_flags);
        return;
    }
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof qp_init_attr);
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 0;      
    qp_init_attr.send_cq = send_cq_;     // 将 qp_ 维护的两个队列 send_cq 和 recv_cq 绑定到两个 cq 上
    qp_init_attr.recv_cq = recv_cq_;
    qp_init_attr.cap.max_send_wr = config_.sr_max;       
    qp_init_attr.cap.max_recv_wr = config_.rr_max;
    qp_init_attr.cap.max_send_sge = config_.send_sge_max;      // 每个 SGE 都会指向一个内存中的 buffer 用于读写
    qp_init_attr.cap.max_recv_sge = config_.recv_sge_max;
    qp_ = ibv_create_qp(pd_, &qp_init_attr);
    if(!qp_){
        fprintf(stderr, "failed to create qp_.\n");
        return;
    }
    if(ibv_req_notify_cq(recv_cq_, 1)){
      fprintf(stderr, "Coudn't request CQ notification\n");
      return;
    }
    memset(&wr_.recv_sge, 0, sizeof wr_.recv_sge);
    wr_.recv_sge.addr = (uintptr_t)recv_buf_;         
    wr_.recv_sge.length = config_.recvbuf_size;
    wr_.recv_sge.lkey = recv_mr_->lkey;
    memset(&wr_.rr, 0, sizeof wr_.rr);
    wr_.rr.next = NULL;
    wr_.rr.wr_id = 0;
    wr_.rr.sg_list = &wr_.recv_sge;
    wr_.rr.num_sge = 1;

    return;
}

// 销毁资源
RDMA::~RDMA(){
  --instance_num_;
	if (qp_) {
		if (ibv_destroy_qp(qp_))
			fprintf(stderr, "failed to destroy qp_\n");
	}
  if (recv_mr_) {
		if (ibv_dereg_mr(recv_mr_))
			fprintf(stderr, "failed to deregister Recv MR\n");
  }  
  if (send_mr_ && !instance_num_) {
		if (ibv_dereg_mr(send_mr_))
			fprintf(stderr, "failed to deregister Send MR\n");
	}  
  if(recv_buf_)
      free(recv_buf_);  
  if(send_buf_ && !instance_num_)
      free(send_buf_);
	if (send_cq_ && !instance_num_) {     
		if (ibv_destroy_cq(send_cq_))
			fprintf(stderr, "failed to destroy Send CQ\n");
  }   
  if (recv_cq_ && !instance_num_) {
		if (ibv_destroy_cq(recv_cq_))
			fprintf(stderr, "failed to destroy Recv CQ\n");
	}
	if (pd_ && !instance_num_) {
		if (ibv_dealloc_pd(pd_))
			fprintf(stderr, "failed to deallocate pd_\n");
  }
  if(channel_ && !instance_num_){
    if (ibv_destroy_comp_channel(channel_))
      fprintf(stderr, "Error, ibv_destroy_comp_channel() failed.\n");
  }
	if (ib_ctx_ && !instance_num_) {
		if (ibv_close_device(ib_ctx_))
			fprintf(stderr, "failed to close device context\n");
	}
	if (dev_list_ && !instance_num_)
    ibv_free_device_list(dev_list_);
}

// qp_ 状态机转换： RESET -> INIT
int RDMA::ModifyQPInit(){
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof attr);
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = config_.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    rc = ibv_modify_qp(qp_, &attr, flags);
    return rc;
}

// qp_ 状态机转换： INIT -> RTR
int RDMA::ModifyQPRtr(){
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
  attr.timeout = 0x12;    // additional
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_256;
	attr.dest_qp_num = remote_props_.qp_num;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 0;
	attr.min_rnr_timer = 0x6;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = remote_props_.lid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = config_.ib_port;

	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
		IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	rc = ibv_modify_qp(qp_, &attr, flags);
	
	return rc;
}

// qp_ 状态机转换： RTR -> RTS
int RDMA::ModifyQPRts(){
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 12;           // change
  attr.min_rnr_timer = 0x6;   // change
	attr.retry_cnt = 7;         // change
	attr.rnr_retry = 7;         // change
	attr.sq_psn = 0;
	attr.max_rd_atomic = 0;
 	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
		IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  rc = ibv_modify_qp(qp_, &attr, flags);
	return rc;
}

size_t RDMA::PostSend(uint32_t imm_data, size_t msg_size, size_t sendoffset){
    const int kSendMaxTime = 20; 
    // if msg is out of the memory than
    if(remote_offset_ + msg_size > config_.recvbuf_size)
        remote_offset_ = 0;
    int rc;
    struct ibv_send_wr sr;
    struct ibv_sge send_sge;
    struct ibv_send_wr *bad_sr;
    send_sge.lkey = send_mr_->lkey;
    send_sge.addr = (uintptr_t)send_buf_ + sendoffset;
    send_sge.length = msg_size; 
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &send_sge;
    sr.num_sge = 1;
    sr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    sr.wr.rdma.rkey = remote_props_.rkey;
    if(++send_times_ == kSendMaxTime)
      sr.send_flags = IBV_SEND_SOLICITED | IBV_SEND_SIGNALED;
    else 
      sr.send_flags = IBV_SEND_SOLICITED;
    sr.imm_data = imm_data;                            
    sr.wr.rdma.remote_addr = remote_props_.addr + remote_offset_;

    // update remote_offset_
    remote_offset_ += msg_size;

    rc = ibv_post_send(qp_, &sr, &bad_sr);
    //rc = ibv_post_send(qp_, &wr_.sr, &wr_.bad_sr);
    if(rc){
        fprintf(stderr, "[1] RDMA Post Send failed. Exit Code: %d\n", rc);
        return -1;
    }

    if(send_times_ == kSendMaxTime){
      send_times_ = 0;
      int rc = 0;
      struct ibv_wc wc;
      do{
        rc = ibv_poll_cq(send_cq_, 1, &wc);
      }while(rc == 0);
      if(rc < 0){
        fprintf(stderr, "RDMA Post Send Poll Cq failed. Exit Code: %d\n", rc);
        return -2;
      }
    }

    return msg_size;
}

int RDMA::PostRecv(){
    int rc = ibv_post_recv(qp_, &wr_.rr, &wr_.bad_rr);
    if(rc){
        fprintf(stderr, "RDMA Post Recv failed. Exit Code: %d", rc);
        return -1;
    }
    return 0;
}

void RDMA::StartRecv(){
  int rc = 0;
  int rr_num = config_.rr_max / 10;
  for(int i = 0; i < rr_num; ++i){
    rc = PostRecv();
    if(rc){
      fprintf(stdout, "Error Node ID ?: In StartRecv: PostRecv failed.\n");
      break;
    }
  }
}

int RDMA::GetEvent(){
	struct ibv_cq *ev_cq;
	void *ev_ctx;
  ev_cq = recv_cq_;
	if(ibv_get_cq_event(channel_, &ev_cq, &ev_ctx)){
    fprintf(stderr, "Failed to get cq_event\n");
		return 1;
	}
  ibv_ack_cq_events(ev_cq, 1);
  return 0;
}

struct bufe RDMA::GetBufPtr(){
  std::unique_lock<std::mutex> lk(recvbufmu_);
  cond_.wait(lk,[]{return !recv_queue_.empty();});
  struct bufe res = recv_queue_.front();
  recv_queue_.pop();
  return res;
}

void RDMA::AddBufPtr(size_t offset, size_t bytes, int id){
  std::lock_guard<std::mutex> lk(recvbufmu_);
  //recvbufmu_.lock();
  struct bufe in = {offset, bytes, id};
  recv_queue_.push(in);
  //recvbufmu_.unlock();
  cond_.notify_all();
}


// interval in second between to heartbeast signals. 0 means no heartbeat.
// don't send heartbeast in default. because if the scheduler received a
// heartbeart signal from a node before connected to that node, then it could be
// problem.
const static int kDefaultHeartbeatInterval = 0;

Van* Van::Create(const std::string& type) {
  if (type == "zmq") {
    return new ZMQVan();
  } else {
    LOG(FATAL) << "unsupported van type: " << type;
    return nullptr;
  }
}

void Van::Start() {
  // ------------TEST------------------
  timer.StartCount();
  // ------------TEST------------------
  // get scheduler info
  scheduler_.hostname = std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));
  scheduler_.port     = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
  scheduler_.role     = Node::SCHEDULER;
  scheduler_.id       = kScheduler;
  is_scheduler_       = Postoffice::Get()->is_scheduler();

  // get my node info
  if (is_scheduler_) {
    my_node_ = scheduler_;
  } else {
    auto role = is_scheduler_ ? Node::SCHEDULER :
                (Postoffice::Get()->is_worker() ? Node::WORKER : Node::SERVER);
    const char* nhost = Environment::Get()->find("DMLC_NODE_HOST");
    std::string ip;
    if (nhost) ip = std::string(nhost);
    if (ip.empty()) {
      const char*  itf = Environment::Get()->find("DMLC_INTERFACE");
      std::string interface;
      if (itf) interface = std::string(itf);
      if (interface.size()) {
        GetIP(interface, &ip);
      } else {
        GetAvailableInterfaceAndIP(&interface, &ip);
      }
      CHECK(!interface.empty()) << "failed to get the interface";
    }
    int port = GetAvailablePort();
    const char* pstr = Environment::Get()->find("PORT");
    if (pstr) port = atoi(pstr);
    CHECK(!ip.empty()) << "failed to get ip";
    CHECK(port) << "failed to get a port";
    my_node_.hostname = ip;
    my_node_.role     = role;
    my_node_.port     = port;
    // cannot determine my id now, the scheduler will assign it later
    // set it explicitly to make re-register within a same process possible
    my_node_.id = Node::kEmpty;
  }

  // bind.
  my_node_.port = Bind(my_node_, is_scheduler_ ? 0 : 40);
  PS_VLOG(1) << "Bind to " << my_node_.DebugString();
  CHECK_NE(my_node_.port, -1) << "bind failed";

  // connect to the scheduler
  Connect(scheduler_);

  // for debug use
  if (Environment::Get()->find("PS_DROP_MSG")) {
    drop_rate_ = atoi(Environment::Get()->find("PS_DROP_MSG"));
  }
  // start receiver
  receiver_thread_ = std::unique_ptr<std::thread>(
      new std::thread(&Van::Receiving, this));

  if (!is_scheduler_) {
    // let the scheduler know myself
    Message msg;
    msg.meta.recver = kScheduler;
    msg.meta.control.cmd = Control::ADD_NODE;
    msg.meta.control.node.push_back(my_node_);
    msg.meta.timestamp = timestamp_++;
    Send(msg);
  }
  // wait until ready
  while (!ready_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // ------------------------------------------ RDMA ------------------------------------------
  // setup RDMA connection
  if(!is_scheduler_) RDMASetUp(kScheduler);
  // create own rdma ctx for end of the transportation use.
  rdma_ctx[my_node_.id] = new RDMA;
  RDMA *ctx = rdma_ctx.find(my_node_.id)->second;
  ctx->ModifyQPInit();
  ctx->remote_props_.addr = (uintptr_t)ctx->recv_buf_;
  ctx->remote_props_.rkey = ctx->GetRecvrkey();
  ctx->remote_props_.qp_num = ctx->GetQPNum();
  ctx->remote_props_.lid = ctx->Getlid();
  int rc = ctx->ModifyQPRtr();
  if(rc){
    fprintf(stderr, "Erro Node ID %d: qp_ modify to RTR failed, RecvMsg: Node ID %d.\n",
            my_node_.id, my_node_.id);
  }
  rc = ctx->ModifyQPRts();
  if(rc){
    fprintf(stderr, "Erro Node ID %d: qp_ modify to RTS failed, RecvMsg: Node ID %d.\n",
            my_node_.id, my_node_.id);
  }
  ctx->PostRecv();
  while(!all_rdma_ready){
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  rdma_thread = std::unique_ptr<std::thread>(
    new std::thread(&Van::RdmaRecv, this));

  #ifdef RLOG
  fprintf(stdout, "Node ID %d: All RDMA Connections have been set up.\n", my_node_.id);
  #endif
  // ------------------------------------------ RDMA ------------------------------------------

  // resender
  if (Environment::Get()->find("PS_RESEND") && atoi(Environment::Get()->find("PS_RESEND")) != 0) {
    int timeout = 1000;
    if (Environment::Get()->find("PS_RESEND_TIMEOUT")) {
      timeout = atoi(Environment::Get()->find("PS_RESEND_TIMEOUT"));
    }
    resender_ = new Resender(timeout, 10, this);
  }

  if (!is_scheduler_) {
    // start heartbeat thread
    heartbeat_thread_ = std::unique_ptr<std::thread>(
      new std::thread(&Van::Heartbeat, this));
  }
}
  

void Van::Stop() {
  // stop threads
  #ifdef RLOG
  fprintf(stdout, "Node ID %d: Call Van Stop.\n", my_node_.id);
  #endif
  Message exit;
  exit.meta.control.cmd = Control::TERMINATE;
  exit.meta.recver = my_node_.id;
  // ------------------------------------------ RDMA ------------------------------------------
  exit.meta.sender = my_node_.id;
  Send(exit);
  //SendMsg(exit);
  // ------------------------------------------ RDMA ------------------------------------------
  rdma_thread->join();
  receiver_thread_->join();
  if (!is_scheduler_) heartbeat_thread_->join();
  if (resender_) delete resender_;
  // ------------------------------------------ RDMA ------------------------------------------
  // delete all rdma ctx
  for(auto it = rdma_ctx.begin(); it != rdma_ctx.end(); it++)
    delete it->second;
  rdma_ctx.clear();

  // -------------------- TEST ---------------------------
  timer.StopCount();
  double span = 0;
  timer.GetTimeSpan(span);
  fprintf(stdout, "Time Span [Van::Start(), Van::Stop()] %lf s\n",span);
  // ------------------------------------------ RDMA ------------------------------------------
}

int Van::Send(const Message& msg) {
  int send_bytes = SendMsg(msg);
  CHECK_NE(send_bytes, -1);
  send_bytes_ += send_bytes;
  if (resender_) resender_->AddOutgoing(msg);
  if (Postoffice::Get()->verbose() >= 2) {
    PS_VLOG(2) << msg.DebugString();
  }
  return send_bytes;
}

void Van::Receiving() {
  const char* heartbeat_timeout_val = Environment::Get()->find("PS_HEARTBEAT_TIMEOUT");
  const int heartbeat_timeout = heartbeat_timeout_val ? atoi(heartbeat_timeout_val) : kDefaultHeartbeatInterval;
  Meta nodes;  // for scheduler usage
  // ------------------------------------------ RDMA ------------------------------------------
  int rdmaReadyNode = 0;
  int recv_num;
  if(Postoffice::Get()->is_worker()) recv_num = Postoffice::Get()->num_servers() + 1;
  else if(Postoffice::Get()->is_server()) recv_num = Postoffice::Get()->num_workers() + 1;
  else recv_num = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
  int recv_bytes = 0;
  // ------------------------------------------ RDMA ------------------------------------------
  while (true) {
    Message msg;
    // ------------------------------------------ RDMA ------------------------------------------
    if(!all_rdma_ready){
      recv_bytes = RecvMsg(&msg);
      if(recv_bytes < 0) continue;
    }
    else{
      auto it = RDMA::GetBufPtr();

      #ifdef RLOG
      fprintf(stdout, "Node ID %d: Recving Thread Starts Handling Msg at offset: %lu - %lu\n",
      my_node_.id, it.offset, it.offset + it.size);
      #endif

      recv_bytes = it.size;
      if(recv_bytes < 0) continue;
      MsgHandle(it, &msg);
    }
    // ------------------------------------------ RDMA ------------------------------------------

    // For debug, drop received message
    if (ready_ && drop_rate_ > 0) {
      unsigned seed = time(NULL) + my_node_.id;
      if (rand_r(&seed) % 100 < drop_rate_) {
        LOG(WARNING) << "Drop message " << msg.DebugString();
        continue;
      }
    }

    CHECK_NE(recv_bytes, -1);
    recv_bytes_ += recv_bytes;
    if (Postoffice::Get()->verbose() >= 2) {
      PS_VLOG(2) << msg.DebugString();
    }
    // duplicated message
    //if (resender_ && resender_->AddIncomming(msg)) continue;


    if (!msg.meta.control.empty()) {
      // do some management
      auto& ctrl = msg.meta.control;
      if (ctrl.cmd == Control::TERMINATE) {
        PS_VLOG(1) << my_node_.ShortDebugString() << " is stopped";
        ready_ = false;
        // ------------------------------------------ RDMA ------------------------------------------
        all_rdma_ready = false;
        // ------------------------------------------ RDMA ------------------------------------------
        break;
      } else if (ctrl.cmd == Control::ADD_NODE) {
        size_t num_nodes = Postoffice::Get()->num_servers() +
                           Postoffice::Get()->num_workers();
        auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout);
        std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
        Meta recovery_nodes;  // store recovery nodes
        recovery_nodes.control.cmd = Control::ADD_NODE;
        // assign an id
        if (msg.meta.sender == Meta::kEmpty) {
          CHECK(is_scheduler_);
          CHECK_EQ(ctrl.node.size(), 1);
          if (nodes.control.node.size() < num_nodes) {
            nodes.control.node.push_back(ctrl.node[0]);
          } else {
            // some node dies and restarts
            CHECK(ready_);
            for (size_t i = 0; i < nodes.control.node.size() - 1; ++i) {
              const auto& node = nodes.control.node[i];
              if (dead_set.find(node.id) != dead_set.end() && node.role == ctrl.node[0].role) {
                auto& recovery_node = ctrl.node[0];
                // assign previous node id
                recovery_node.id = node.id;
                recovery_node.is_recovery = true;
                PS_VLOG(1) << "replace dead node " << node.DebugString()
                           << " by node " << recovery_node.DebugString();
                nodes.control.node[i] = recovery_node;
                recovery_nodes.control.node.push_back(recovery_node);
                break;
              }
            }
          }
        }

        // update my id
        for (size_t i = 0; i < ctrl.node.size(); ++i) {
          const auto& node = ctrl.node[i];
          if (my_node_.hostname == node.hostname &&
              my_node_.port == node.port) {
            my_node_ = node;
            std::string rank = std::to_string(Postoffice::IDtoRank(node.id));
 #ifdef _MSC_VER
            _putenv_s("DMLC_RANK", rank.c_str());
 #else
            setenv("DMLC_RANK", rank.c_str(), true);
 #endif
          }
        }

        if (is_scheduler_) {
          time_t t = time(NULL);
          if (nodes.control.node.size() == num_nodes) {
            // sort the nodes according their ip and port,
            std::sort(nodes.control.node.begin(), nodes.control.node.end(),
                      [](const Node& a, const Node& b) {
                        return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
                      });
            // assign node rank
            for (auto& node : nodes.control.node) {
              CHECK_EQ(node.id, Node::kEmpty);
              int id = node.role == Node::SERVER ?
                       Postoffice::ServerRankToID(num_servers_) :
                       Postoffice::WorkerRankToID(num_workers_);
              PS_VLOG(1) << "assign rank=" << id << " to node " << node.DebugString();
              node.id = id;
              Connect(node);
              if (node.role == Node::SERVER) ++num_servers_;
              if (node.role == Node::WORKER) ++num_workers_;
              Postoffice::Get()->UpdateHeartbeat(node.id, t);
            }
            nodes.control.node.push_back(my_node_);
            nodes.control.cmd = Control::ADD_NODE;
            Message back; back.meta = nodes;
            for (int r : Postoffice::Get()->GetNodeIDs(
                     kWorkerGroup + kServerGroup)) {
              back.meta.recver = r;
              back.meta.timestamp = timestamp_++;
              Send(back);
            }
            PS_VLOG(1) << "the scheduler is connected to "
                    << num_workers_ << " workers and " << num_servers_ << " servers";
            ready_ = true;
          } else if (recovery_nodes.control.node.size() > 0) {
            // send back the recovery node
            CHECK_EQ(recovery_nodes.control.node.size(), 1);
            Connect(recovery_nodes.control.node[0]);
            Postoffice::Get()->UpdateHeartbeat(recovery_nodes.control.node[0].id, t);
            Message back;
            for (int r : Postoffice::Get()->GetNodeIDs(
                     kWorkerGroup + kServerGroup)) {
              if (r != recovery_nodes.control.node[0].id
                    && dead_set.find(r) != dead_set.end()) {
                // do not try to send anything to dead node
                continue;
              }
              // only send recovery_node to nodes already exist
              // but send all nodes to the recovery_node
              back.meta = (r == recovery_nodes.control.node[0].id) ? nodes : recovery_nodes;
              back.meta.recver = r;
              back.meta.timestamp = timestamp_++;
              Send(back);
            }
          }
        } else {
          for (const auto& node : ctrl.node) {
            Connect(node);
            if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
            if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
          }
          PS_VLOG(1) << my_node_.ShortDebugString() << " is connected to others";
          ready_ = true;
        }
      } else if (ctrl.cmd == Control::BARRIER) {
        if (msg.meta.request) {
          if (barrier_count_.empty()) {
            barrier_count_.resize(8, 0);
          }
          int group = ctrl.barrier_group;
          ++barrier_count_[group];
          PS_VLOG(1) << "Barrier count for " << group << " : " << barrier_count_[group];
          if (barrier_count_[group] ==
              static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size())) {
            barrier_count_[group] = 0;
            Message res;
            res.meta.request = false;
            res.meta.control.cmd = Control::BARRIER;
            for (int r : Postoffice::Get()->GetNodeIDs(group)) {
              res.meta.recver = r;
              res.meta.timestamp = timestamp_++;
              // ------------------------------------------ RDMA ------------------------------------------
              if(r == my_node_.id){
                Postoffice::Get()->Manage(res);
                continue;
              }
              // ------------------------------------------ RDMA ------------------------------------------
              CHECK_GT(Send(res), 0);
            }
          }
        } else {
          Postoffice::Get()->Manage(msg);
        }
      } else if (ctrl.cmd == Control::HEARTBEAT) {
        time_t t = time(NULL);
        for (auto &node : ctrl.node) {
          Postoffice::Get()->UpdateHeartbeat(node.id, t);
          if (is_scheduler_) {
            Message heartbeat_ack;
            heartbeat_ack.meta.recver = node.id;
            heartbeat_ack.meta.control.cmd = Control::HEARTBEAT;
            heartbeat_ack.meta.control.node.push_back(my_node_);
            heartbeat_ack.meta.timestamp = timestamp_++;
            // send back heartbeat
            Send(heartbeat_ack);
          }
        }
      }
      // ------------------------------------------ RDMA ------------------------------------------
      else if (ctrl.cmd == Control::RDMA_INIT){
        int rc;
        // Debug String
        #ifdef RLOG
        fprintf(stdout, "Node ID %d: Recv RDMA_INIT Msg from Node ID %d.\n",
                my_node_.id, msg.meta.sender);
        #endif
        
        int id = msg.meta.sender;
        if(is_scheduler_ || (Postoffice::Get()->is_server() && id != 1)) RDMASetUp(id);
        auto it = rdma_ctx.find(id);
        RDMA *ctx = it->second;
        ctx->remote_props_.qp_num = msg.meta.head;
        ctx->remote_props_.lid = msg.meta.customer_id;
        rc = ctx->ModifyQPRtr();
        if(rc){
          fprintf(stderr, "Erro Node ID %d: qp_ modify to RTR failed, RecvMsg: Node ID %d.\n",
                  my_node_.id, my_node_.id);
        }
        rc = ctx->ModifyQPRts();
        if(rc){
          fprintf(stderr, "Erro Node ID %d: qp_ modify to RTS failed, RecvMsg: Node ID %d.\n",
                  my_node_.id, my_node_.id);
        }
        else{
          #ifdef RLOG
          fprintf(stdout, "Node ID %d: qp_ connects with Node ID %d successfully.\n",
                  my_node_.id, id);
          #endif
        }
        std::vector<std::string> result = split(msg.meta.body, ':');
        ctx->remote_props_.addr = (uint64_t)std::stoull(result[0]); 
        ctx->remote_props_.rkey = (uint32_t)std::stoul(result[1]);
        ctx->StartRecv();
        //ctx->PostRecv();

        if(Postoffice::Get()->is_worker() && id == 1){
          for (int r : Postoffice::Get()->GetNodeIDs(kServerGroup))
            RDMASetUp(r);
        }
        if(++rdmaReadyNode == recv_num){
          all_rdma_ready = true;
        }
      }
    // ------------------------------------------ RDMA ------------------------------------------
    } else {
      CHECK_NE(msg.meta.sender, Meta::kEmpty);
      CHECK_NE(msg.meta.recver, Meta::kEmpty);
      CHECK_NE(msg.meta.customer_id, Meta::kEmpty);
      int id = msg.meta.customer_id;
      auto* obj = Postoffice::Get()->GetCustomer(id, 5);
      CHECK(obj) << "timeout (5 sec) to wait App " << id << " ready";
      obj->Accept(msg);
    }
  }
}

void Van::RdmaRecv(){
  const int kPollNum = 8;
  struct ibv_wc wc[kPollNum];
  int recv_bytes, rc;
  uint32_t id;
  RDMA *ctx = NULL;
  bool stop = false;
  while(true){
    rc = RDMA::GetEvent();
    if(rc < 0) // means van::stop() is called
      break;
    if (ibv_req_notify_cq(RDMA::recv_cq_, 1))
      fprintf(stderr, "Error Node ID %d: Couldn't request CQ notification\n", my_node_.id);
    do{
      rc = ibv_poll_cq(RDMA::recv_cq_, kPollNum, wc);
      if(rc < 0){   // which means poll cq failed
        fprintf(stderr, "Error Node ID %d: Outside RDMA Poll recv_cq_ failed. Exit Code: %d.\n",
        my_node_.id, rc);
        continue;
      }
      else{   // which means cq is empty now
        for(int i = 0; i < rc; ++i){    // Deal with every wc
          if(wc[i].imm_data % 2 == 1) stop = true;
          id = (wc[i].imm_data >> 1);
          recv_bytes = wc[i].byte_len;
          ctx = rdma_ctx.find(id)->second;
          if(ctx->recvbuf_offset_ + recv_bytes > RecvBufSize) ctx->recvbuf_offset_ = 0;
          RDMA::AddBufPtr(ctx->recvbuf_offset_, recv_bytes, id);
          ctx->recvbuf_offset_ += recv_bytes;
          if(ctx->PostRecv()){
            fprintf(stderr, "Error Node ID %d: PostRecv() failed.\n", my_node_.id);
          }
        }
      }
      if(stop) return;
    }while(rc > 0);
  }
}
// ------------------------------------------ RDMA ------------------------------------------
// add a bool parameter to control whether create new buffer
void Van::PackMeta(const Meta& meta, char** meta_buf, int* buf_size, bool RDMA_flag) {
// ------------------------------------------ RDMA ------------------------------------------
  // convert into protobuf
  PBMeta pb;
  pb.set_head(meta.head);
  if (meta.customer_id != Meta::kEmpty) pb.set_customer_id(meta.customer_id);
  if (meta.timestamp != Meta::kEmpty) pb.set_timestamp(meta.timestamp);
  if (meta.body.size()) pb.set_body(meta.body);
  pb.set_push(meta.push);
  pb.set_request(meta.request);
  pb.set_simple_app(meta.simple_app);
  for (auto d : meta.data_type) pb.add_data_type(d);
  if (!meta.control.empty()) {
    auto ctrl = pb.mutable_control();
    ctrl->set_cmd(meta.control.cmd);
    if (meta.control.cmd == Control::BARRIER) {
      ctrl->set_barrier_group(meta.control.barrier_group);
    } else if (meta.control.cmd == Control::ACK) {
      ctrl->set_msg_sig(meta.control.msg_sig);
    }
    for (const auto& n : meta.control.node) {
      auto p = ctrl->add_node();
      p->set_id(n.id);
      p->set_role(n.role);
      p->set_port(n.port);
      p->set_hostname(n.hostname);
      p->set_is_recovery(n.is_recovery);
    }
  }

  // to string
  *buf_size = pb.ByteSize();
  // ------------------------------------------ RDMA ------------------------------------------
  // if RDMA_flag, the buffer has been registered. Else is in TCP transportation case.
  if(!RDMA_flag) *meta_buf = new char[*buf_size+1];
  // ------------------------------------------ RDMA ------------------------------------------
  CHECK(pb.SerializeToArray(*meta_buf, *buf_size))
      << "PackMeta: failed to serialize protbuf";
}

void Van::UnpackMeta(const char* meta_buf, int buf_size, Meta* meta) {
  // to protobuf
  PBMeta pb;
  CHECK(pb.ParseFromArray(meta_buf, buf_size))
      << "failed to parse string into protobuf";

  // to meta
  meta->head = pb.head();
  meta->customer_id = pb.has_customer_id() ? pb.customer_id() : Meta::kEmpty;
  meta->timestamp = pb.has_timestamp() ? pb.timestamp() : Meta::kEmpty;
  meta->request = pb.request();
  meta->push = pb.push();
  meta->simple_app = pb.simple_app();
  meta->body = pb.body();
  meta->data_type.resize(pb.data_type_size());
  for (int i = 0; i < pb.data_type_size(); ++i) {
    meta->data_type[i] = static_cast<DataType>(pb.data_type(i));
  }
  if (pb.has_control()) {
    const auto& ctrl = pb.control();
    meta->control.cmd = static_cast<Control::Command>(ctrl.cmd());
    meta->control.barrier_group = ctrl.barrier_group();
    meta->control.msg_sig = ctrl.msg_sig();
    for (int i = 0; i < ctrl.node_size(); ++i) {
      const auto& p = ctrl.node(i);
      Node n;
      n.role = static_cast<Node::Role>(p.role());
      n.port = p.port();
      n.hostname = p.hostname();
      n.id = p.has_id() ? p.id() : Node::kEmpty;
      n.is_recovery = p.is_recovery();
      meta->control.node.push_back(n);
    }
  } else {
    meta->control.cmd = Control::EMPTY;
  }
}

void Van::Heartbeat() {
  const char* val = Environment::Get()->find("PS_HEARTBEAT_INTERVAL");
  const int interval = val ? atoi(val) : kDefaultHeartbeatInterval;
  while (interval > 0 && ready_) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    Message msg;
    msg.meta.recver = kScheduler;
    msg.meta.control.cmd = Control::HEARTBEAT;
    msg.meta.control.node.push_back(my_node_);
    msg.meta.timestamp = timestamp_++;
    Send(msg);
  }
}
// ------------------------------------------ RDMA ------------------------------------------
void Van::RDMASetUp(int id){
  Message msg;
  RDMA *ctx = new RDMA;
  ctx->ModifyQPInit();

  msg.meta.recver = id;
  msg.meta.control.cmd = Control::RDMA_INIT;
  msg.meta.timestamp = timestamp_++;

  msg.meta.head = ctx->GetQPNum();
  msg.meta.customer_id = ctx->Getlid();
  std::string ss = std::to_string((uint64_t)ctx->recv_buf_);
  ss += ":";
  ss += std::to_string((uint32_t)ctx->GetRecvrkey());
  msg.meta.body += ss;
  Send(msg);
  rdma_ctx[id] = ctx;
  
}
std::vector<std::string> Van::split(const std::string &s, char mark){
  std::vector<std::string> result;
  int i = 0;
  int j = 0;
  int size = s.size();
  
  while(j < size){
    if(s[j] == mark){
      result.push_back(s.substr(i, j));
      i = j + 1;
    }
    j++;
  }
  result.push_back(s.substr(i, j - 1));
  return result;
}
// ------------------------------------------ RDMA ------------------------------------------

}  // namespace ps
