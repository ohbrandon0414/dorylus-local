#ifndef __NTWK_OPS_HPP__
#define __NTWK_OPS_HPP__

#include <unistd.h>

#include <zmq.hpp>

#include "../../../common/matrix.hpp"
#include "../../../common/utils.hpp"

#define SLEEP_PERIOD   1000  // us
#define TIMEOUT_PERIOD (500) // ms

#define SND_MORE true
#define NO_MORE false

/**
 *
 * Request a tensor from a server
 *
 */
Matrix requestTensor(zmq::socket_t& socket, OP op, unsigned partId,
  TYPE type = TYPE::AH, unsigned layer = 0);

/**
 *
 * Send multiplied matrix result back to dataserver.
 *
 */
void sendMatrices(Matrix& zResult, Matrix& actResult,
  zmq::socket_t& socket, unsigned id);

/**
 *
 * Send matrix back to a server.
 *
 */
void sendMatrix(Matrix& matrix, OP op, zmq::socket_t& socket, unsigned id);

// named-tensors
Matrix recvTensor(zmq::socket_t& socket);

std::vector<Matrix> reqTensors(zmq::socket_t& socket, unsigned partId,
  unsigned layer, std::vector<std::string>& tensorRequests);

void sendTensors(zmq::socket_t& socket, unsigned partId,
  unsigned layer, std::vector<Matrix>& matrices, bool ack = false);
// end named-tensors


#endif
