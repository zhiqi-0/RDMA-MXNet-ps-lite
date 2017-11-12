/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_INTERNAL_VAN_H_
#define PS_INTERNAL_VAN_H_

#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <queue>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <ctime>
#include "ps/base.h"
#include "ps/internal/message.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>
#include <byteswap.h>
#include <fcntl.h>
#include <poll.h>
#include <infiniband/verbs.h>

namespace ps {
class Resender;

class Timer{
 private:
    bool start_count_flag_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point end_;
 public:
    Timer():start_count_flag_(false) {}
    ~Timer() {}
    // Start counting time
    const bool StartCount(){
        if(start_count_flag_)
            return false;
        start_count_flag_ = true;
        start_ = std::chrono::steady_clock::now();
        return true;
    }
    // Stop counting time
    const bool StopCount(){
        if(!start_count_flag_)
            return false;
        start_count_flag_ = false;
        end_ = std::chrono::steady_clock::now();
        return true;
    }
    // Get time span after stop counting time
    const bool GetTimeSpan(double& seconds) const{
        if(start_count_flag_) return false;
        std::chrono::duration<double> time_span = 
        std::chrono::duration_cast<std::chrono::duration<double> > (end_ - start_);
        seconds = time_span.count();
        return true;
    }
    // reset the counter
    void Reset(){
        start_count_flag_ = false;
        return;
    }

};

/**
 * \brief Class RDMA Definition
 */
#define RDMA_CHECK_NP(x, y) if(!(x)) {fprintf(stderr, "Error:%s\n", y);}
// x should be equal to zero
#define RDMA_CHECK_Z(x, y) if(x) {fprintf(stderr, "Error:%s\n", y); return;}

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

const int SendBufSize = 1024*1024*256;          // 512MB send buffer for whole node
const int RecvBufSize = 1024*1024*100;          // 256 MB recv buffer for each qp

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* basic configure of RDMA
 */
struct config_t{
    char        *dev_name;              // IB device name
    int         ib_port;                // local IB port to work with
    int         cq_size;                // cq max size
    int         sr_max;                 // send request max times in one time
    int         rr_max;                 // recv request max times in one time
    int         send_sge_max;           // max send sge list size
    int         recv_sge_max;           // max recv sge list size
    size_t      sendbuf_size;           // send buffer bytes size
    size_t      recvbuf_size;           // recv buffer bytes size 
};

/* basic PostRecv Function configure of RDMA 
 */
struct rdma_wr{
    struct ibv_recv_wr rr;
    struct ibv_sge recv_sge;
    struct ibv_recv_wr *bad_rr;
};

/* remote side information
 */
struct dest_info_t{
    uint64_t        addr;           // buffer Address
    uint32_t        rkey;           // remote key
    uint32_t        qp_num;         // qp number
    uint16_t        lid;            // lid of the IB port
}__attribute__((packed));

/* rdma_recv queue buffer
 */
struct bufe{
   size_t offset;
   size_t size;
   int id;
};

 /* rdma class
  */
class RDMA{
 private:
    static int                              instance_num_;       // store the number of RDMA instance
    static struct ibv_device_attr           *device_attr_;        // device attributes
    static struct ibv_port_attr             *port_attr_;          // IB port attributes
    static struct ibv_device                **dev_list_;         // devie list
    static struct ibv_context               *ib_ctx_;            // device handle
    static struct ibv_comp_channel          *channel_;           // event channel_
    static struct ibv_pd                    *pd_;                // pd_ handle
    static struct ibv_mr                    *send_mr_;            // Send MR handle
    size_t                                  remote_offset_ = 0;
    struct ibv_mr                           *recv_mr_;            // Recv MR handle
    struct ibv_qp                           *qp_;                // qp_ handle
    struct rdma_wr                          wr_;
    int                                     send_times_;
 public:
    static struct config_t                  config_;             // config_
    static struct ibv_cq                    *send_cq_;            // Send CQ handle
    static struct ibv_cq                    *recv_cq_;            // Receive CQ handle 
    static int                              num_devices_;
    struct dest_info_t                      remote_props_;       // value to connect to remote side
    static void                             *send_buf_;           // send buff pointer
    void                                    *recv_buf_;           // recv buff pointer
    size_t                                  recvbuf_offset_ = 0;
    // --------------------------------- Basic Methods ---------------------------------------
    RDMA();
    ~RDMA();
    uint32_t                                GetRecvrkey() {return (uint32_t)recv_mr_->rkey;}
    uint32_t                                GetQPNum()  {return qp_->qp_num;}
    static int                              Getlid()    {return port_attr_->lid;}
    int                                     ModifyQPInit();
    int                                     ModifyQPRtr();
    int                                     ModifyQPRts();
    size_t                                  PostSend(uint32_t imm_data, size_t msg_size, size_t sendoffset);
    int                                     PostRecv();
    void                                    StartRecv();
    static int                              GetEvent();
    // --------------------------------- Send cycle Buffer ---------------------------------
    static std::mutex                       sendbufmu_;
    static size_t                           sendbuf_offset_;
    // --------------------------------- Recv Queue ---------------------------------
    static struct bufe                      GetBufPtr();
    static void                             AddBufPtr(size_t offset, size_t bytes, int id);
    static std::queue<struct bufe>          recv_queue_;
    static std::mutex                       recvbufmu_;
    static std::condition_variable          cond_;
};

/**
* \brief Van sends messages to remote nodes
 *
 * If environment variable PS_RESEND is set to be 1, then van will resend a
 * message if it no ACK messsage is received within PS_RESEND_TIMEOUT millisecond
 */
class Van {
 public:

  // ------------------------------------------ RDMA ------------------------------------------
  // RDMA Lists     KV Pair: {Node ID, Class RDMA*}
  std::unordered_map<int, RDMA*> rdma_ctx;
  void RDMASetUp(int id);
  bool all_rdma_ready = false;
  std::vector<std::string> split(const std::string &s, char mark);
  Timer timer;
  // ------------------------------------------ RDMA ------------------------------------------

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

 protected:
  /**
   * \brief connect to a node
   */
  virtual void Connect(const Node& node) = 0;
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

  // ------------------------------------------ RDMA ------------------------------------------
  // RDMA RecvMsg
  virtual void MsgHandle(struct bufe qe, Message *msg) = 0;
  // ------------------------------------------ RDMA ------------------------------------------

  /**
   * \brief send a mesage
   * \return the number of bytes sent
   */
  virtual int SendMsg(const Message& msg) = 0;
  /**
   * \brief pack meta into a string
   */

  // ------------------------------------------ RDMA ------------------------------------------
  // add a bool parameter to control whether create new buffer
  void PackMeta(const Meta& meta, char** meta_buf, int* buf_size, bool RDMA_flag = false);
  // ------------------------------------------ RDMA ------------------------------------------
  
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
  void RdmaRecv();
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
  std::unique_ptr<std::thread> rdma_thread;
  std::vector<int> barrier_count_;
  /** msg resender */
  Resender* resender_ = nullptr;
  int drop_rate_ = 0;
  std::atomic<int> timestamp_{0};
  DISALLOW_COPY_AND_ASSIGN(Van);
};

}  // namespace ps
#endif  // PS_INTERNAL_VAN_H_
