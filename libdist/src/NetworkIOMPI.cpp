/** Galois Network Backend for MPI -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2017, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */

#include "galois/runtime/NetworkIO.h"
#include "galois/runtime/Tracer.h"
#include "galois/substrate/SimpleLock.h"

/**
 * MPI implementation of network IO. ASSUMES THAT MPI IS INITIALIZED
 * UPON CREATION OF AN OBJECT.
 */
class NetworkIOMPI : public galois::runtime::NetworkIO {
private:
  /**
   * Get the host id of the caller.
   *
   * @returns host id of the caller with regard to the MPI setup
   */
  static int getID() {
    int taskRank;
    handleError(MPI_Comm_rank(MPI_COMM_WORLD, &taskRank));
    return taskRank;
  }

  /**
   * Get the total number of hosts in the system.
   *
   * @returns number of hosts with regard to the MPI setup
   */
  static int getNum() {
    int numTasks;
    handleError(MPI_Comm_size(MPI_COMM_WORLD, &numTasks));
    return numTasks;
  }
  
  /**
   * Get both the ID of the caller + number of hosts.
   */
  std::pair<int, int> getIDAndHostNum() {
    return std::make_pair(getID(), getNum());
  }

  struct mpiMessage {
    uint32_t host;
    uint32_t tag;
    std::vector<uint8_t> data;
    MPI_Request req;
    //mpiMessage(message&& _m, MPI_Request _req) : m(std::move(_m)), req(_req) {}
    mpiMessage(uint32_t host, uint32_t tag, std::vector<uint8_t>&& data) 
      : host(host), tag(tag), data(std::move(data)) {}
    mpiMessage(uint32_t host, uint32_t tag, size_t len) 
      : host(host), tag(tag), data(len) {}
  };

  struct sendQueueTy {
    std::deque<mpiMessage> inflight;

    void complete() {
      while (!inflight.empty()) {
        int flag = 0;
        MPI_Status status;
        auto& f = inflight.front();
        int rv = MPI_Test(&f.req, &flag, &status);
        handleError(rv);
        if (flag)
          inflight.pop_front();
        else
          break;
      }
    }

    void send(message m) {
      //MPI_Request req;
      inflight.emplace_back(m.host, m.tag, std::move(m.data));
      auto& f = inflight.back();
      galois::runtime::trace("MPI SEND", f.host, f.tag, f.data.size(), galois::runtime::printVec(f.data));
      int rv = MPI_Isend(f.data.data(), f.data.size(), MPI_BYTE, f.host, f.tag, MPI_COMM_WORLD, &f.req);
      handleError(rv);
    }
  };

  struct recvQueueTy {
    std::deque<message> done;
    std::deque<mpiMessage> inflight;

    // FIXME: Does synchronous recieves overly halt forward progress?
    void probe() {
      int flag = 0;
      MPI_Status status;
      // check for new messages
      int rv = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag,
                          &status);
      handleError(rv);
      if (flag) {
        int nbytes;
        rv = MPI_Get_count(&status, MPI_BYTE, &nbytes);
        handleError(rv);
        #ifdef __GALOIS_BARE_MPI_COMMUNICATION__
        assert(status.MPI_TAG <= 32767);
        if (status.MPI_TAG != 32767) {
        #endif
          inflight.emplace_back(status.MPI_SOURCE, status.MPI_TAG, nbytes);
          auto& m = inflight.back();
          rv = MPI_Irecv(m.data.data(), nbytes, MPI_BYTE, status.MPI_SOURCE, 
                         status.MPI_TAG, MPI_COMM_WORLD, &m.req);
          handleError(rv);
          galois::runtime::trace("MPI IRECV", status.MPI_SOURCE, 
                                 status.MPI_TAG, m.data.size());
        #ifdef __GALOIS_BARE_MPI_COMMUNICATION__
        }
        #endif
      }

      // complete messages
      if (!inflight.empty()) {
        auto& m = inflight.front();
        int flag = 0;
        rv = MPI_Test(&m.req, &flag, MPI_STATUS_IGNORE);
        handleError(rv);
        if (flag) {
          done.emplace_back(m.host, m.tag, std::move(m.data));
          inflight.pop_front();
        }
      }
    }
  };

  sendQueueTy sendQueue;
  recvQueueTy recvQueue;

public:
  NetworkIOMPI(uint32_t& ID, uint32_t& NUM) {
    auto p = getIDAndHostNum();
    ID = p.first;
    NUM = p.second;
  }

  virtual void enqueue(message m) {
    sendQueue.send(std::move(m));
  }

  virtual message dequeue() {
    if (!recvQueue.done.empty()) {
      auto msg = std::move(recvQueue.done.front());
      recvQueue.done.pop_front();
      return msg;
    }
    return message{~0U, 0, std::vector<uint8_t>()};
  }

  virtual void progress() {
    sendQueue.complete();
    recvQueue.probe();
  }
}; // end NetworkIOMPI class

std::tuple<std::unique_ptr<galois::runtime::NetworkIO>,
                           uint32_t,
                           uint32_t> galois::runtime::makeNetworkIOMPI() {
  uint32_t ID, NUM;
  std::unique_ptr<galois::runtime::NetworkIO> n{new NetworkIOMPI(ID, NUM)};
  return std::make_tuple(std::move(n), ID, NUM);
}