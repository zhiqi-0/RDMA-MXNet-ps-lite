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
namespace ps {

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
//==================================== RDMA ============================================
int Van::van_init_rdma_source(bool is_scheduler_){
  ctx_ = new struct van_context;
  if (!ctx_) {
    fprintf(stderr, "Couldn't create ctx_\n");
    return 1;
  }
  struct ibv_device *ib_dev;
  ctx_->dev_list = ibv_get_device_list(NULL);
  if (!ctx_->dev_list) {
    fprintf(stderr, "Couldn't get IB device list.\n");
    return 1;
  }
  ib_dev = *(ctx_->dev_list);
  if (!ib_dev) {
    fprintf(stderr, "No IB device found.\n");
    return 1;
  }
  ctx_->ib_ctx = ibv_open_device(ib_dev);
  if (!ctx_->ib_ctx) {
    fprintf(stderr, "Cannot open IB device\n");
    return 1;
  }
  ctx_->channel = ibv_create_comp_channel(ctx_->ib_ctx);
	if (!ctx_->channel) {
	  fprintf(stderr, "Cannot create comp channel\n");
	}
  ctx_->send_cq = ibv_create_cq(ctx_->ib_ctx, 20, NULL, NULL, 0);
  if (!ctx_->send_cq){
    fprintf(stderr, "Couldn't create CQ\n");
    return 1;
  }
  ctx_->recv_cq = ibv_create_cq(ctx_->ib_ctx, 20, NULL, ctx_->channel, 0);
  if (!ctx_->recv_cq){
    fprintf(stderr, "Couldn't create CQ\n");
    return 1;
  }
	if (ibv_req_notify_cq(ctx_->recv_cq, 0)) {
		fprintf(stderr, "Couldn't request CQ nofity\n");
  }
  if (is_scheduler_) {
    ctx_->size = 1 << 23;
    ctx_->send_size = 1 << 23;
  } else {
    ctx_->size = 1 << 26;
    ctx_->send_size = 1 << 26;
  }
  ctx_->page_size = sysconf(_SC_PAGESIZE);
  ctx_->port = 1;
  ctx_->pd = ibv_alloc_pd(ctx_->ib_ctx);
  if (!ctx_->pd) {
    fprintf(stderr, "Couldn't allocate PD\n");
    return 1;
  }
  return 0;
}

int Van::van_init_qp(struct qp_context* qp_ctx) {
  struct ibv_port_attr port_attr;
  qp_ctx->port = ctx_->port; //there is only one port in our IB device
  qp_ctx->current_p = 0;
  qp_ctx->dest_info.current_p = 0;
  qp_ctx->current_send_p = 0;
  qp_ctx->send_sig_count = 0;
  if (ibv_query_port(ctx_->ib_ctx, qp_ctx->port, &port_attr)){
    fprintf(stderr, "Couldn't get local port's attribution\n");
    return 1;
  }
  qp_ctx->lid = port_attr.lid;

  qp_ctx->send_buf = malloc(roundup(ctx_->send_size, ctx_->page_size));
  if (!qp_ctx->send_buf) {
    fprintf(stderr, "Couldn't allocate work buffer.\n");
    return 1;
  }

  int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
  qp_ctx->send_mr = ibv_reg_mr(ctx_->pd, qp_ctx->send_buf, ctx_->send_size, mr_flags);
  if(!qp_ctx->send_mr) {
    fprintf(stderr, "Couldn't regist MR\n");
    return 1;
  }

  qp_ctx->recv_buf = malloc(roundup(ctx_->size, ctx_->page_size));
  if (!qp_ctx->recv_buf) {
    fprintf(stderr, "Couldn't allocate work buffer.\n");
    return 1;
  }
  mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
  qp_ctx->recv_mr = ibv_reg_mr(ctx_->pd, qp_ctx->recv_buf, ctx_->size, mr_flags);
  if(!qp_ctx->recv_mr) {
    fprintf(stderr, "Couldn't regist MR\n");
    return 1;
  }
  
  struct ibv_qp_init_attr qp_attr;
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.qp_type          = IBV_QPT_RC; /* reliable connection */
  qp_attr.sq_sig_all       = 0;          /* by default, generate no wc in cq */
  qp_attr.send_cq          = ctx_->send_cq;
  qp_attr.recv_cq          = ctx_->recv_cq;
  qp_attr.cap.max_send_wr  = send_depth;
  qp_attr.cap.max_recv_wr  = rx_depth;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;
  qp_attr.cap.max_inline_data = 0;

  qp_ctx->qp = ibv_create_qp(ctx_->pd, &qp_attr);
  int err = errno;
  if(!qp_ctx->qp) {
    fprintf(stderr, "Couldn't create QP, errno = %d\n", err);
    return 1;
  }
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state         = IBV_QPS_INIT;
  attr.pkey_index       = 0;
  attr.port_num         = qp_ctx->port;
  attr.qp_access_flags  = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

  if (ibv_modify_qp(qp_ctx->qp, &attr,
      IBV_QP_STATE      |
      IBV_QP_PKEY_INDEX	|
      IBV_QP_PORT       |
      IBV_QP_ACCESS_FLAGS)) {
    fprintf(stderr, "Couldn't modify QP to init state\n");
    return 1;
  }

  return 0;
}

int Van::init_to_self() {
  to_self[0] = new struct qp_context;// send in 0
  if (!to_self[0]) {
    fprintf(stderr, "%d Couldn't malloc to self\n", my_node_.id);
  }

  if (van_init_qp(to_self[0])) {
    fprintf(stderr, "%d Counldn't create self qp\n", my_node_.id);
    return 1;
  }
  to_self[1] = new struct qp_context;// recv in 1
  if (!to_self[1]) {
    fprintf(stderr, "%d Couldn't malloc to self\n", my_node_.id);
  }
  if (van_init_qp(to_self[1])) {
    fprintf(stderr, "%d Counldn't create 2nd self qp\n", my_node_.id);
    return 1;
  }
  to_self[1]->dest_info.lid  = to_self[0]->lid;
  to_self[1]->dest_info.addr = (uint64_t)to_self[0]->recv_buf;
  to_self[1]->dest_info.rkey = to_self[0]->recv_mr->rkey;
  to_self[1]->dest_info.qpn  = to_self[0]->qp->qp_num;
  to_self[1]->dest_info.port = ctx_->port;
  if (RDMAConnect(to_self[1])) {
    fprintf(stderr, "Couldn't Connect qp\n");
    return 1;
  }
  if (van_post_recv(to_self[1], rx_depth, my_node_.id) != rx_depth) {
    fprintf(stderr, "Couldn't post recv req\n");
    return 1;
  }

  to_self[0]->dest_info.lid  = to_self[1]->lid;
  to_self[0]->dest_info.addr = (uint64_t)to_self[1]->recv_buf;
  to_self[0]->dest_info.rkey = to_self[1]->recv_mr->rkey;
  to_self[0]->dest_info.qpn  = to_self[1]->qp->qp_num;
  to_self[0]->dest_info.port = ctx_->port;
  if (RDMAConnect(to_self[0])) {
    fprintf(stderr, "Couldn't Connect qp\n");
    return 1;
  }
  if (van_post_recv(to_self[0], rx_depth, my_node_.id) != rx_depth) {
    fprintf(stderr, "Couldn't post recv req\n");
    return 1;
  }
  return 0;
}

//=================================== RDMA end =================================================
void Van::Start() {
  start = std::chrono::system_clock::now(); // count time start
  scheduler_.hostname = std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));
  scheduler_.port     = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
  scheduler_.role     = Node::SCHEDULER;
  scheduler_.id       = kScheduler;
  is_scheduler_       = Postoffice::Get()->is_scheduler();

  if (is_scheduler_) {
    my_node_ = scheduler_;
  } else {
    auto role = is_scheduler_ ? Node::SCHEDULER : (Postoffice::Get()->is_worker() ? Node::WORKER : Node::SERVER);
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

  my_node_.port = Bind(my_node_, is_scheduler_ ? 0 : 40);
  PS_VLOG(1) << "Bind to " << my_node_.DebugString();
  CHECK_NE(my_node_.port, -1) << "bind failed";

  Connect(scheduler_);
  // for debug use
  if (Environment::Get()->find("PS_DROP_MSG")) {
    drop_rate_ = atoi(Environment::Get()->find("PS_DROP_MSG"));
  }
  //==================================== RDMA =============================================
  // init all shared rdma source now
  if (van_init_rdma_source(is_scheduler_)) {
    fprintf(stderr, "Couldn't init rdma source\n");
    return;
  }
  // create the thread to poll cq
  deal_buf_thread_ = std::unique_ptr<std::thread>(
      new std::thread(&Van::deal_buf, this));
  //==================================== RDMA end =============================================
  receiver_thread_ = std::unique_ptr<std::thread>(
      new std::thread(&Van::Receiving, this));

  if (!is_scheduler_) {
    Message msg;
    msg.meta.recver = kScheduler;
    msg.meta.control.cmd = Control::ADD_NODE;
    msg.meta.control.node.push_back(my_node_);
    msg.meta.timestamp = timestamp_++;
    //======================================= RDMA ==========================================
    // this is not scheduler, create a qp to scheduler and add all required informations to the AddNode message
    struct qp_context* qp_ctx = new struct qp_context;
    if (!qp_ctx) {
      fprintf(stderr, "Couldn't malloc qp_ctx\n");
      return;
    }
    ctx_->qps.insert(std::make_pair(kScheduler, qp_ctx));
    if (van_init_qp(qp_ctx)) {
      fprintf(stderr, "Coultn't init qp\n");
      return;
    }
    std::vector<uint64_t> data = {(uint64_t)qp_ctx->lid,
                                  (uint64_t)qp_ctx->recv_buf,
                                  (uint64_t)qp_ctx->recv_mr->rkey,
                                  (uint64_t)qp_ctx->qp->qp_num,
                                  (uint64_t)qp_ctx->port};
    SArray<uint64_t> a(data);
    msg.AddData(a);
    //======================================= RDMA end ==========================================
    Send(msg);
  }

  while (!ready_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // resender
  if (Environment::Get()->find("PS_RESEND") && atoi(Environment::Get()->find("PS_RESEND")) != 0) {
    int timeout = 1000;
    if (Environment::Get()->find("PS_RESEND_TIMEOUT")) {
      timeout = atoi(Environment::Get()->find("PS_RESEND_TIMEOUT"));
    }
    resender_ = new Resender(timeout, 10, this);
  }

  if (!is_scheduler_) {
    heartbeat_thread_ = std::unique_ptr<std::thread>(
      new std::thread(&Van::Heartbeat, this));
  }
}

void Van::Stop() {
  // stop threads
  Message exit;
  exit.meta.control.cmd = Control::TERMINATE;
  exit.meta.recver = my_node_.id;
  SendMsg(exit);
  receiver_thread_->join();
  if (!is_scheduler_) heartbeat_thread_->join();
  //========================== RDMA ================================
  // join the new thread
  deal_buf_thread_->join();
  //========================== RDMA end ================================
  if (resender_) delete resender_;
  // end timer
  auto end = std::chrono::system_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  fprintf(stdout, "time: %lf\n", double(duration.count()) * std::chrono::microseconds::period::num / std::chrono::microseconds::period::den);
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

void Van::print_all_qp() {
  /* debug use. print all qp's informations */
  fprintf(stdout, "%d", my_node_.id);
  for (auto& it : ctx_->qps) {
    fprintf(stdout, "\t%d,local lid %x, addr %x, rkey %x, qpn %x\n", it.first, it.second->lid, (uint64_t)it.second->recv_buf, it.second->recv_mr->rkey, it.second->qp->qp_num);
    fprintf(stdout, "\tremote lid %x, addr %x, rkey %x qpn %x\n", it.second->dest_info.lid, it.second->dest_info.addr, it.second->dest_info.rkey, it.second->dest_info.qpn);
  }
  fprintf(stdout, "\n");
}

void Van::Receiving() {  const char* heartbeat_timeout_val = Environment::Get()->find("PS_HEARTBEAT_TIMEOUT");
  const int heartbeat_timeout = heartbeat_timeout_val ? atoi(heartbeat_timeout_val) : kDefaultHeartbeatInterval;
  Meta nodes;  // for scheduler usage
  //======================= RDMA ============================
  std::vector<RDMA_Node> rdma_nodes;  /* info of other nodes. used for scheduler */
  size_t connected = 0;               /* number of all connected nodes. used for no scheduler */
  //======================= RDMA end ============================
  while (true) {
    Message msg;
    int recv_bytes = RecvMsg(&msg);

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
    if (resender_ && resender_->AddIncomming(msg)) continue;

    if (!msg.meta.control.empty()) {
      // do some management
      auto& ctrl = msg.meta.control;
      if (ctrl.cmd == Control::TERMINATE) {
        PS_VLOG(1) << my_node_.ShortDebugString() << " is stopped";
        ready_ = false;
        rdma_ready_ = false;
        break;
      }
      else if (ctrl.cmd == Control::ADD_NODE) {
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
          //===================================== RDMA ========================================
          if (rdma_nodes.size() < num_nodes) {
            // before ready. scheduler receives other nodes' info
            SArray<uint64_t> data = SArray<uint64_t>(msg.data[0]);
            struct RDMA_Node rn;
            rn.org_node = ctrl.node[0];
            rn.dest_info.lid  = (uint16_t)data[0];
            rn.dest_info.addr = data[1];
            rn.dest_info.rkey = (uint32_t)data[2];
            rn.dest_info.qpn  = (uint32_t)data[3];
            rn.dest_info.port = (uint32_t)data[4];
            rdma_nodes.push_back(rn);
          }
          //===================================== RDMA end =======================================
          else {
            // some node dies and restarts
            CHECK(ready_);
            for (size_t i = 0; i < nodes.control.node.size() - 1; ++i) {
              const auto& node = nodes.control.node[i];
              // if nodes[i] is dead & its role is the same with the node to be added
              if (dead_set.find(node.id) != dead_set.end() && node.role == ctrl.node[0].role) {
                // replace nodes[i] with the node to be added
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
        } // sender id is empty, not decided yet

        // update my id, not useful for scheduler
        if (msg.meta.sender == kScheduler) {
        for (size_t i = 0; i < ctrl.node.size(); ++i) {
          const auto& node = ctrl.node[i];
          if (my_node_.hostname == node.hostname &&
              my_node_.port == node.port) {
            my_node_ = node;
            //================================= RDMA ==========================================
            // update scheduler's information to qps
            auto it = ctx_->qps.find(kScheduler);
            struct qp_context* qp_ctx = it->second;
            SArray<uint64_t> data = SArray<uint64_t>(msg.data[i]);
            qp_ctx->dest_info.lid  = (uint16_t)data[0];
            qp_ctx->dest_info.addr = data[1];
            qp_ctx->dest_info.rkey = (uint32_t)data[2];
            qp_ctx->dest_info.qpn  = (uint32_t)data[3];
            qp_ctx->dest_info.port = (uint32_t)data[4];
            // connect qp to scheduler
            if (RDMAConnect(qp_ctx)) {
              fprintf(stderr, "Couldn't connect qp\n");
              return;
            }
            if (van_post_recv(qp_ctx, rx_depth, kScheduler) != rx_depth) {
              fprintf(stderr, "Couldn't post recv req\n");
              return;
            }
            connected++;
            //================================= RDMA end ==========================================
            std::string rank = std::to_string(Postoffice::IDtoRank(node.id));
#ifdef _MSC_VER
            _putenv_s("DMLC_RANK", rank.c_str());
#else
            setenv("DMLC_RANK", rank.c_str(), true);
#endif
          }
        } // for node in msg.ctrl.node, msg is the back one
        }
        if (is_scheduler_) {
          time_t t = time(NULL);
          // all workers & servers has sent AddNode to shceduler
          //================================== RDMA ============================================
          if (rdma_nodes.size() == num_nodes) {
            // sort the nodes according their ip and port,
            std::sort(rdma_nodes.begin(), rdma_nodes.end(),
                      [](const RDMA_Node& a, const RDMA_Node& b) {
                        return (a.org_node.hostname.compare(b.org_node.hostname) | (a.org_node.port < b.org_node.port)) > 0;
                      });
            // assign node rank
            Message back; 
            for (auto& rnode : rdma_nodes) {
              CHECK_EQ(rnode.org_node.id, Node::kEmpty);
              int id = rnode.org_node.role == Node::SERVER ?
                       Postoffice::ServerRankToID(num_servers_) :
                       Postoffice::WorkerRankToID(num_workers_);
              PS_VLOG(1) << "assign rank=" << id << " to node " << rnode.org_node.DebugString();
              rnode.org_node.id = id;
              // scheduler create and connect qp to other nodes
              struct qp_context* qp_ctx = new struct qp_context;
              if (!qp_ctx) {
                fprintf(stderr, "Couldn't new qp_context\n");
                return;
              }
              qp_ctx->dest_info = rnode.dest_info;
              if (van_init_qp(qp_ctx)) {
                fprintf(stderr, "%d Counldn't create qp\n", my_node_.id);
                return;
              }
              ctx_->qps.insert(std::make_pair(rnode.org_node.id, qp_ctx));
              Connect(rnode.org_node);
              if (rnode.org_node.id != my_node_.id) {
                if (RDMAConnect(qp_ctx)) {
                  fprintf(stderr, "Couldn't connect qp\n");
                }
                if (van_post_recv(qp_ctx, rx_depth, id) != rx_depth) {
                  fprintf(stderr, "Couldn't post recv req\n");
                  return;
                }
              }
              if (rnode.org_node.role == Node::SERVER) ++num_servers_;
              if (rnode.org_node.role == Node::WORKER) ++num_workers_;
              Postoffice::Get()->UpdateHeartbeat(rnode.org_node.id, t);
              nodes.control.node.push_back(rnode.org_node);
              // add the qp's info to the message. same order as the msg.meta.control.node
              std::vector<uint64_t> data = {(uint64_t)qp_ctx->lid,
                                            (uint64_t)qp_ctx->recv_buf,
                                            (uint64_t)qp_ctx->recv_mr->rkey,
                                            (uint64_t)qp_ctx->qp->qp_num,
                                            (uint64_t)qp_ctx->port};
              SArray<uint64_t> a(data);
              back.AddData(a);
            }
            // push back scheduler itself
            struct RDMA_Node my_rnode_;
            my_rnode_.org_node = my_node_;
            // these info won't be used
            my_rnode_.dest_info.lid  = 0;
            my_rnode_.dest_info.qpn  = 0;
            my_rnode_.dest_info.rkey = 0;
            my_rnode_.dest_info.addr = 0;
            my_rnode_.dest_info.port = 0;
            rdma_nodes.push_back(my_rnode_);
            nodes.control.node.push_back(my_node_);
            back.meta.control.node = nodes.control.node;
            back.meta.control.cmd = Control::ADD_NODE;
            for (int r : Postoffice::Get()->GetNodeIDs(
                     kWorkerGroup + kServerGroup)) {
              back.meta.recver = r;
              back.meta.timestamp = timestamp_++;
              Send(back);
#ifdef rdma_debug_info
              fprintf(stdout, "1 send back a msg\n");
#endif
            }
            PS_VLOG(1) << "the scheduler is connected to "
                    << num_workers_ << " workers and " << num_servers_ << " servers";
            //create qp for send msg to self
            if (init_to_self()) {
              fprintf(stderr, "%d Couldn't create qp to myself\n", my_node_.id);
              return;
            }
            // rdma for scheduler is ready now
            rdma_ready_  = true;
#ifdef rdma_debug_info
            fprintf(stdout, "%d ready\n", my_node_.id);
            print_all_qp();
#endif
            // zmq_van is no longer needed
            close_zmq_van();
            ready_ = true;
          }
            //================================== RDMA end =======================================
          else if (recovery_nodes.control.node.size() > 0) {
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
        } // is scheduler
        //==================================== RDMA =======================================
        else {
          // for non-scheduler, they first receive a message from scheduler that contains info about other nodes, then create enough qp and connect with each other
          if (msg.meta.sender == kScheduler) {
            // a non-scheduer node received a AddNode message from scheduler, which contains all informations of other nodes
            for (const auto& node : ctrl.node) {
              // for each node in this msg, create a qp for it and send the qp's info to the node through zmq
              Connect(node);
              // server or worker does not connect with same role
              if (node.role != my_node_.role && node.id != kScheduler) {
                struct qp_context* qp_ctx = new struct qp_context;
                ctx_->qps.insert(std::make_pair(node.id, qp_ctx));
                if (van_init_qp(qp_ctx)) {
                  fprintf(stderr, "%d Counldn't create qp\n", my_node_.id);
                  return;
                }
                std::vector<uint64_t> data = {(uint64_t)qp_ctx->lid,
                                              (uint64_t)qp_ctx->recv_buf,
                                              (uint64_t)qp_ctx->recv_mr->rkey,
                                              (uint64_t)qp_ctx->qp->qp_num,
                                              (uint64_t)qp_ctx->port};
                SArray<uint64_t> a(data);
                Message msg;
                msg.meta.recver = node.id;
                msg.meta.control.cmd = Control::ADD_NODE;
                msg.meta.timestamp = timestamp_;
                msg.AddData(a);
                Send(msg);
              }

              if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
              if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
              PS_VLOG(1) << my_node_.ShortDebugString() << " is connected to others";
            }
          }
          else {
            // a non-scheduler node received an AddNode message from a non-scheduler node, which contains information's of that node
            // all qps have been created before
            auto it = ctx_->qps.find(msg.meta.sender);
            struct qp_context* qp_ctx = it->second;
            SArray<uint64_t> data = SArray<uint64_t>(msg.data[0]);
            qp_ctx->dest_info.lid  = (uint16_t)data[0];
            qp_ctx->dest_info.addr = data[1];
            qp_ctx->dest_info.rkey = (uint32_t)data[2];
            qp_ctx->dest_info.qpn  = (uint32_t)data[3];
            qp_ctx->dest_info.port = (uint32_t)data[4];
            RDMAConnect(qp_ctx);
            if (van_post_recv(qp_ctx, rx_depth, msg.meta.sender) != rx_depth) {
              fprintf(stderr, "Couldn't post recv req\n");
              return;
            }
            connected++;
            // all the connections are built. connect to self then ready
            if (connected == ctx_->qps.size()) {
              if (init_to_self()) {
                fprintf(stderr, "%d Couldn't connect to myself\n", my_node_.id);
                return;
              }
              rdma_ready_ = true;
#ifdef rdma_debug_info
              fprintf(stdout, "%d ready\n", my_node_.id);
              print_all_qp();
#endif
              close_zmq_van();
              ready_ = true;
            }
          }
        }
        //====================================== RDMA end ======================================
      } //add node

      else if (ctrl.cmd == Control::BARRIER) {
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
              CHECK_GT(Send(res), 0);
            }
          }
        } else {
          Postoffice::Get()->Manage(msg);
        }
      } // barrier

      else if (ctrl.cmd == Control::HEARTBEAT) {
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
      } //heartbeat
    } //!control.empty()

    else {// control empty, pure data
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

void Van::PackMeta(const Meta& meta, char** meta_buf, int* buf_size) {
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
  //=========================== RDMA =============================
  if (!rdma_ready_) {
    // if rdma ready, pack meta to send buf directly 
    *meta_buf = new char[*buf_size+1];
  }
  //=========================== RDMA end ===============================
  CHECK(pb.SerializeToArray(*meta_buf, *buf_size))
      << "failed to serialize protbuf";
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

void Van::deal_buf() {
  bool terminate = false;
  while (!ready_);
  while (rdma_ready_) {
    struct ibv_wc wc;
    int ne = 0;

    struct ibv_cq *ev_cq;
    void* ev_ctx;
    // get an event, ack and req notify it, then clear cq.
    int ret = ibv_get_cq_event(ctx_->channel, &ev_cq, &ev_ctx);
    if (ret) {
      fprintf(stderr, "Couldn't get CQ event\n");
      return;
    }
    ibv_ack_cq_events(ev_cq, 1);
    if (ibv_req_notify_cq(ctx_->recv_cq, 0)) {
      fprintf(stderr, "Couldn't request CQ nofity\n");
      return;
    }
    // clear cq
    do {
      ne = ibv_poll_cq(ctx_->recv_cq, 1, &wc);
      if (ne < 0) {
        fprintf(stderr, "Couldn't poll recv cq\n");
        return;
      }
      if (ne == 0) {
        break;
      }
      struct qp_context* qp_ctx;
      // set imm_data to terminate flag. this is required so this thread can be joined
      if (wc.imm_data & 1) {
        terminate = true;
      }

      if (wc.wr_id == my_node_.id) {
        qp_ctx = to_self[1];
      } else {
        auto it = ctx_->qps.find(wc.wr_id);
        qp_ctx = it->second;
      }

      int ret = van_post_recv(qp_ctx, 1, wc.wr_id);
      if (ret != 1) {
        fprintf(stderr, "%d Couldn't post recv request, %d\n", my_node_.id, ret);
        return;
      }

      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%d Received unsuccessed msg, status %d\n", my_node_.id, wc.status);
        return;
      }
      struct buf_node p;
      p.sender = wc.wr_id;
      p.total_size = wc.byte_len;
      if (wc.imm_data & 2) 
        qp_ctx->current_p = 0;
      p.buf = qp_ctx->recv_buf + qp_ctx->current_p;
#ifdef rdma_debug_info
      fprintf(stdout, "[%lx, %lx]\n", qp_ctx->current_p, p.total_size);
#endif
      qp_ctx->current_p += p.total_size;
      buf_queue.Push(p);
      if (terminate) {
        return;
      }
    } while (ne);
  }
}

}  // namespace ps
