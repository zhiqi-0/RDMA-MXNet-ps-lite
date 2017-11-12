/**
 *  Copyright (c) 2015 by Contributors
 */
#include "ps/internal/customer.h"
#include "ps/internal/postoffice.h"
namespace ps {

const int Node::kEmpty = std::numeric_limits<int>::max();
const int Meta::kEmpty = std::numeric_limits<int>::max();

Customer::Customer(int id, const Customer::RecvHandle& recv_handle)
    : id_(id), recv_handle_(recv_handle) {
  Postoffice::Get()->AddCustomer(this);
  // create a Customer thread, codes in Receiving
  recv_thread_ = std::unique_ptr<std::thread>(new std::thread(&Customer::Receiving, this));
}

Customer::~Customer() {
  Postoffice::Get()->RemoveCustomer(this);
  Message msg;
  msg.meta.control.cmd = Control::TERMINATE;
  recv_queue_.Push(msg);
  recv_thread_->join();
}

// returns timestamp
int Customer::NewRequest(int recver) {
  // when pull a request, call this function
  std::lock_guard<std::mutex> lk(tracker_mu_);
  int num = Postoffice::Get()->GetNodeIDs(recver).size();
  tracker_.push_back(std::make_pair(num, 0)); // std::vector<std::pair<int, int>> tracker_
  return tracker_.size() - 1;
}

// wait until the request is finished
void Customer::WaitRequest(int timestamp) {
  std::unique_lock<std::mutex> lk(tracker_mu_);
  tracker_cond_.wait(lk, [this, timestamp]{
      return tracker_[timestamp].first == tracker_[timestamp].second;
    });
}

// get the number of responses received for the request
int Customer::NumResponse(int timestamp) {
  std::lock_guard<std::mutex> lk(tracker_mu_);
  return tracker_[timestamp].second;
}

// add a number of responses to timestamp
void Customer::AddResponse(int timestamp, int num) {
  std::lock_guard<std::mutex> lk(tracker_mu_);
  tracker_[timestamp].second += num;
}

void Customer::Receiving() {
  while (true) {
    Message recv;
    // thread safe queue
    recv_queue_.WaitAndPop(&recv);
    //fprintf(stdout, "Customer got a msg, cmd %d, req %d, tmstp %d\n", recv.meta.control.cmd, recv.meta.request, recv.meta.timestamp);
    if (!recv.meta.control.empty() &&
        recv.meta.control.cmd == Control::TERMINATE) { // kill this thread when receive TERMINATE
      break;
    }
    //fprintf(stdout, "check req 1st %d\n", recv.meta.request);
    recv_handle_(recv);
    if (!recv.meta.request) {
      //fprintf(stdout, "check req 2nd %d\n", recv.meta.request);
      // mutex, released when exit if
      std::lock_guard<std::mutex> lk(tracker_mu_);
      //fprintf(stdout, "traker size %d, timestamp %d\n", tracker_.size(), recv.meta.timestamp);
      tracker_[recv.meta.timestamp].second++;
      tracker_cond_.notify_all();
    }
  }
}

}  // namespace ps
