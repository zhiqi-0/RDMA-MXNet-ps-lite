/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_INTERNAL_VAN_H_
#define PS_INTERNAL_VAN_H_

#include <unordered_map>
#include <cstdlib>
#include <unistd.h>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <ctime>
#include <infiniband/verbs.h>

#include <cstdio>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "ps/base.h"
#include "ps/internal/message.h"
#include "ps/internal/threadsafe_queue.h"

/* define this to print each message's info for debug */
//#define rdma_debug_info
#define rx_depth 1        /* post rx_depth recv requests to one qp when init */
#define send_depth 10000  /* each time when send_depth messages are sent, generate a wc in cq */ 

namespace ps {

enum {
	DEMO_RECV_WRID = 1,
	DEMO_SEND_WRID = 2
};


struct demo_dest_info {
  uint16_t lid;		  	/* Local ID */
  uint64_t addr;			/* Buffer address, why needed? */
  uint32_t rkey;			/* remote key, key for mr */
  uint32_t qpn;		  	/* Queue Pairs number */
  uint32_t port;      /* ib port */
  uint64_t current_p; /* offset of the destinaton's recv buf's next available area */
};

struct RDMA_Node {
  /* used in Receiving thread for scheduler to manage all nodes */
  struct Node org_node;             /* the origin Node struct used in origin ps-lite */
  struct demo_dest_info dest_info;  /* extra infomations needed to manage nodes */
};

struct qp_context {
  /* information & resources of one qp */
  struct ibv_qp           *qp;              /* this qp */
  struct ibv_mr           *send_mr;         /* this qp's send mr */
  struct ibv_mr           *recv_mr;         /* this qp's recv mr */
  struct demo_dest_info   dest_info;        /* dest info of the remote qp connected to this qp */
  void                    *send_buf;        /* this qp's send buf */
  void                    *recv_buf;        /* this qp's recv buf */
  uint16_t                lid;              /* local id of this qp's recv buf */
  uint32_t                port;             /* this qp's port */
  uint64_t                current_p;        /* offset of the local recv buf's next available area */
  std::mutex              send_mu_;         /* mutex of this qp's send buf */
  uint64_t                current_send_p;   /* offset of the send buf's next available area */
  int                     send_sig_count;   /* count sent message. when reaches send_depth, generate a wc and set zero */
};

struct van_context {
  /* shared rdma resources */
  struct ibv_device		                        **dev_list;		/* IB device list */
  struct ibv_context 		                      *ib_ctx;		  /* device handle */
  struct ibv_cq                               *send_cq;     /* shared complete queue for all send queue */
  struct ibv_cq                               *recv_cq;     /* shared complete queue for all recv queue */
  int						                              size;         /* size of each queue pair's recv buf */
  int                                         send_size;    /* size of each queue pair's send buf */
  int                                         page_size;    /* system page size */
  struct ibv_pd                               *pd;          /* device's protected domain */
  std::unordered_map<int, struct qp_context*> qps;          /* <id, qp_ctx> pairs */
  uint32_t                                    port;         /* available port */
  struct ibv_comp_channel                     *channel;     /* channel for events */
};

struct buf_node {
  /* buf_queue's node */
  int sender;     /* this message's sender */
  int total_size;   /* total size of this messgae */
  void* buf;        /* pointer to the beginning of this message in recv_buf */
};

class Resender;
/**
 * \brief Van sends messages to remote nodes
 *
 * If environment variable PS_RESEND is set to be 1, then van will resend a
 * message if it no ACK messsage is received within PS_RESEND_TIMEOUT millisecond
 */
class Van {
 public:
  /**
   * \brief create Van
   * \param type zmq, socket, ...
   */
  static Van* Create(const std::string& type);
  /** \brief constructer, do nothing. use \ref Start for real start */
  Van() { }
  /**\brief deconstructer, do nothing. use \ref Stop for real stop */
  virtual ~Van() { }
  /**
   * \brief start van
   *
   * must call it before calling Send
   *
   * it initalizes all connections to other nodes.  start the receiving
   * threads, which keeps receiving messages. if it is a system
   * control message, give it to postoffice::manager, otherwise, give it to the
   * accoding app.
   */
  virtual void Start();
  /**
   * \brief send a message, It is thread-safe
   * \return the number of bytes sent. -1 if failed
   */
  int Send(const Message& msg);
  /**
   * \brief return my node
   */
  const Node& my_node() const {
    CHECK(ready_) << "call Start() first";
    return my_node_;
  }
  /**
   * \brief stop van
   * stop receiving threads
   */
  virtual void Stop();
  /**
   * \brief get next available timestamp. thread safe
   */
  int GetTimestamp() { return timestamp_++; }
  /**
   * \brief whether it is ready for sending. thread safe
   */
  bool IsReady() { return ready_; }

  //=====================================================
  /* thread function for polling recv cq and pushing messages into a new queue */
  void deal_buf(); 
  /* initial all shared rdma resource */
  int van_init_rdma_source(bool); 
  /* initial each qp's own rdma resource */
  int van_init_qp(struct qp_context* qp_ctx); 
  /* init qp connected to this node itself */
  int init_to_self();
  /* shared rdma resources */
  struct van_context* ctx_ = nullptr;
  /* two qp connected to each other. used when sending msg to itself */
  struct qp_context* to_self[2] = {nullptr, nullptr};
  /* the new thread polls the recv cq and push message information to this queue. when RecvMsg, read message from this queue */
  ThreadsafeQueue<struct buf_node> buf_queue;
  /* debuf use. print all qp's information */
  void print_all_qp();
  /* post n recv requests. used in both van and zmq_van */
  virtual int van_post_recv(struct qp_context* qp_ctx, int n, int id) = 0;
  /* close zmq socket. used when all qps are built */
  virtual void close_zmq_van() = 0;
  /* count time between Start() and Stop() */
  std::chrono::system_clock::time_point start;
  /* whether rdma is ready */
  std::atomic<bool> rdma_ready_{false};
  //=====================================================

 protected:
  /**
   * \brief connect to a node
   */
  virtual void Connect(const Node& node) = 0;
  virtual int RDMAConnect(struct qp_context* qp_ctx) = 0;
  /**
   * \brief bind to my node
   * do multiple retries on binding the port. since it's possible that
   * different nodes on the same machine picked the same port
   * \return return the port binded, -1 if failed.
   */
  virtual int Bind(const Node& node, int max_retry) = 0;
  /**
   * \brief block until received a message
   * \return the number of bytes received. -1 if failed or timeout
   */
  virtual int RecvMsg(Message* msg) = 0;
  /**
   * \brief send a mesage
   * \return the number of bytes sent
   */
  virtual int SendMsg(const Message& msg) = 0;
  /**
   * \brief pack meta into a string
   */
  void PackMeta(const Meta& meta, char** meta_buf, int* buf_size);
  /**
   * \brief unpack meta from a string
   */
  void UnpackMeta(const char* meta_buf, int buf_size, Meta* meta);


  Node scheduler_;
  Node my_node_;
  bool is_scheduler_;

 private:
  /** thread function for receving */
  void Receiving();
  /** thread function for heartbeat */
  void Heartbeat();
  /** whether it is ready for sending */
  std::atomic<bool> ready_{false};
  std::atomic<size_t> send_bytes_{0};
  size_t recv_bytes_ = 0;
  int num_servers_ = 0;
  int num_workers_ = 0;
  /** the thread for receiving messages */
  std::unique_ptr<std::thread> receiver_thread_;
  /** the thread for sending heartbeat */
  std::unique_ptr<std::thread> heartbeat_thread_;
  std::vector<int> barrier_count_;
  /** msg resender */
  Resender* resender_ = nullptr;
  int drop_rate_ = 0;
  std::atomic<int> timestamp_{0};
  DISALLOW_COPY_AND_ASSIGN(Van);
  //===========================================
  /* the thread for polling recv cq */
  std::unique_ptr<std::thread> deal_buf_thread_;
  //===========================================
};
}  // namespace ps
#endif  // PS_INTERNAL_VAN_H_
