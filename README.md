# RDMA optimization on MXNet/ps-lite

* This optimization is based on MXNet 0.10
* This optimization is done on Linux Centos Sugon cluster(10 nodes).
* This optimization requires infiniband card & lmlx4 library

### Attention
* `ps-lite-rdma-final/` source code is different from 'report/final-submit/' source code. But both of them can run smmothly.  
* The differences between these two source code files are:
  * ps-lite-rdma-final is completely written by Lin Zhiqi (the owner of this repository), the final-submit source code is written by Song Xiaoniu.
  * The major difference between this two source code is the basic model of RDMA QP and CQ
    * ps-lite-rdma-final uses 1 shared send cq (not srq!) on all QPs. Each QP has its own recv cq.
    * final-submit use the RDMA model that each QP has its own send cq and recv cq
    * Features ps-lite-rdma-final has but final-sbumit doesn't have:
      * Parallel memcpy (by unlocking early locks of rdma send operation)
      * multi-post-recv-request(repeatly post multi recv request at end of connection setup, thus can have higher performance when facing with n workers - 1 server)
* These two codes have similar performance. But due to final-submit has more sample tests results, so we finally use this version to submmit the final-report. 
