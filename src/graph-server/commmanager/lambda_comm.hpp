#ifndef __LAMBDA_COMM_HPP__
#define __LAMBDA_COMM_HPP__

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include "../utils/utils.hpp"


static const int32_t HEADER_SIZE = sizeof(int32_t) * 4;
enum OP { PUSH, PULL, REQ, RESP };

// Serialization functions
template<class T>
void serialize(char* buf, int32_t offset, T val) {
    std::memcpy(buf + (offset * sizeof(T)), &val, sizeof(T));
}

template<class T>
T parse(const char* buf, int32_t offset) {
    T val;
    std::memcpy(&val, buf + (offset * sizeof(T)), sizeof(T));
    return val;
}

// ID represents either layer or data partition, depending on server responding
void populateHeader(char* header, int32_t op, int32_t id, int32_t rows = 0,
    int32_t cols = 0);
// End serialization functions


struct Matrix {
    int32_t rows;
    int32_t cols;

    FeatType* data;

    Matrix() {
        rows = 0;
        cols = 0;
    }

    Matrix(int _rows, int _cols) {
        rows = _rows;
        cols = _cols;
    }

    Matrix(int _rows, int _cols, FeatType* _data) {
        rows = _rows;
        cols = _cols;

        data = _data;
    }

    Matrix(int _rows, int _cols, char* _data) {
        rows = _rows;
        cols = _cols;

        data = (FeatType*)_data;
    }

    FeatType* getData() const { return data; }
        size_t getDataSize() const { return rows * cols * sizeof(FeatType); }

    void setRows(int32_t _rows) { rows = _rows; }
    void setCols(int32_t _cols) { cols = _cols; }
    void setDims(int32_t _rows, int32_t _cols) { rows = _rows; cols = _cols; }
    void setData(FeatType* _data) { data = _data; }

    bool empty() { return rows == 0 || cols == 0; }

        std::string shape() {
        return "(" + std::to_string(rows) + "," + std::to_string(cols) + ")";
    }

    std::string str() {
        std::stringstream output;
        output << "Matrix Dims: " << shape() << "\n";
        for (int32_t i = 0; i < rows; ++i) {
            for (int32_t j = 0; j < cols; ++j) {
                output << data[i*cols + j] << " ";
            }
            output << "\n";
        }

        return output.str();
    }
};


class server_worker {
public:
	server_worker(zmq::context_t& ctx_, int32_t sock_type, int32_t nParts_,
	  int32_t nextIterCols_, int32_t& counter_, Matrix& data_,
	  FeatType* zData_, FeatType* actData_);

	// Continuously listens for incoming lambda connections
	// and either sends a partitioned matrix or receives
	// computed results
	void work();
	
private:
	/**
	 * Partitions the data matrix according to the partition id and
	 * send it to the lambda thread for computation
	 */
	void sendMatrixChunk(zmq::socket_t& socket, zmq::message_t &client_id,
	  int32_t partId);

	/**
	 * Accepts an incoming connection from a lambda thread and receives
	 * two matrices, a 'Z' matrix and the 'activations' matrix
	 */
	void recvMatrixChunks(zmq::socket_t& socket, int32_t partId, int32_t rows,
	  int32_t cols);

	Matrix& data;

	int32_t nextIterCols;
	FeatType* zData;
	FeatType* actData;

	zmq::context_t &ctx;
	zmq::socket_t worker;

	int32_t bufSize;
	int32_t partRows;
	int32_t partCols;
	int32_t nParts;
	int32_t offset;

	// counting down until all lambdas have returned
	static std::mutex count_mutex;
	int32_t& count;
};


class LambdaComm {
public:
	LambdaComm(FeatType* data_, std::string& nodeIp_, unsigned port_,
	  int32_t rows_, int32_t cols_, int32_t nextIterCols_,
	  int32_t nParts_, int32_t numListeners_);
	
	/**
	 * binds to a public port and a backend routing port for the 
	 * worker threads to connect to. Spawns 'numListeners' number
	 * of workers and connects the frontend socket to the backend
	 * by proxy.
	 */
	void run();

	/**
	 * sends a request to the coordination server for a given
	 * number of lambda threads
	 */
	void requestLambdas(std::string& coordserverIp,
	  std::string& coordserverPort, int32_t layer);

	/**
	 * Return a contiguous buffer representing the Z data for this
	 * partition
	 */
	FeatType* getZData();

	/**
	 * Return a contiguous buffer representing the activations data
	 * for this partition
	 */
	FeatType* getActivationData();


private:
	Matrix data;

	int32_t nextIterCols;
	FeatType* zData;
	FeatType* actData;
		
	int32_t nParts;
	int32_t numListeners;
	int32_t counter;

	zmq::context_t ctx;
	zmq::socket_t frontend;
	zmq::socket_t backend;
	std::string& nodeIp;
	unsigned port;
};

#endif