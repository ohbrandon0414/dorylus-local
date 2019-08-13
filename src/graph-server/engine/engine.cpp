#include <iostream>
#include <fstream>
#include <cmath>
#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/algorithm/string/classification.hpp>    // Include boost::for is_any_of.
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <string>
#include <thread>
#include <cstdlib>
#include <omp.h>
#include <cerrno>
#include "engine.hpp"


/** Extern class-wide fields. */
Graph Engine::graph;
ThreadPool* Engine::dataPool = NULL;
unsigned Engine::dThreads;
ThreadPool* Engine::computePool = NULL;
unsigned Engine::cThreads;
std::string Engine::graphFile;
std::string Engine::outFile;
std::string Engine::featuresFile;
std::string Engine::layerConfigFile;
std::string Engine::labelsFile;
std::string Engine::dshMachinesFile;
std::string Engine::myPrIpFile;
std::string Engine::myPubIpFile;
unsigned Engine::dataserverPort;
std::string Engine::coordserverIp;
unsigned Engine::coordserverPort;
LambdaComm *Engine::lambdaComm = NULL;
IdType Engine::currId = 0;
Lock Engine::lockCurrId;
Lock Engine::lockRecvWaiters;
Cond Engine::condRecvWaitersEmpty;
Lock Engine::lockHalt;
unsigned Engine::nodeId;
unsigned Engine::numNodes;
std::map<IdType, unsigned> Engine::recvWaiters;
Barrier Engine::barComp;
std::vector<unsigned> Engine::layerConfig;
unsigned Engine::numLayers = 0;
FeatType **Engine::localVerticesZData = NULL;
FeatType **Engine::localVerticesActivationData = NULL;
FeatType **Engine::ghostVerticesActivationData = NULL;
FeatType *Engine::localVerticesDataBuf = NULL;
FeatType *Engine::localVerticesLabels = NULL;
unsigned Engine::iteration = 0;
bool Engine::undirected = false;
bool Engine::halt = false;
double Engine::timeInit = 0.0;
double Engine::timeProcess = 0.0;
double Engine::timeOutput = 0.0;
std::vector<double> Engine::vecTimeAggregate;
std::vector<double> Engine::vecTimeLambda;
std::vector<double> Engine::vecTimeSendout;


/**
 *
 * Initialize the engine with the given command line arguments.
 * 
 */
void
Engine::init(int argc, char *argv[]) {
    printLog(404, "Engine starts initialization...\n");
    timeInit = -getTimer();

    parseArgs(argc, argv);
    
    // Initialize the node manager and communication manager.
    NodeManager::init(dshMachinesFile, myPrIpFile, myPubIpFile);    // NodeManger should go first.
    nodeId = NodeManager::getMyNodeId();
    numNodes = NodeManager::getNumNodes();
    assert(numNodes <= 256);    // Cluster size limitation.
    outFile += std::to_string(nodeId);
    CommManager::init();

    // Set number of layers and number of features in each layer. Also store the prefix sum of config for offset querying use.
    readLayerConfigFile(layerConfigFile);
    numLayers = layerConfig.size() - 1;

    // Read in the graph and subscribe vertex global ID topics.
    std::set<IdType> inTopics;
    std::vector<IdType> outTopics;
    readGraphBS(graphFile, inTopics, outTopics);
    printGraphMetrics();

    // Create the global contiguous memory for vertices' data, according to the given layer config and number of local vertices.
    // Create the global contiguous memory for ghost vertices' data similarly. Then read in initial features.
    localVerticesZData = new FeatType *[numLayers + 1];
    localVerticesActivationData = new FeatType *[numLayers + 1];
    ghostVerticesActivationData = new FeatType *[numLayers + 1];

    // Create data storage area for each layer.
    for (size_t i = 0; i <= numLayers; ++i) {
        localVerticesZData[i] = new FeatType[layerConfig[i] * graph.getNumLocalVertices()];
        localVerticesActivationData[i] = new FeatType[layerConfig[i] * graph.getNumLocalVertices()];
        ghostVerticesActivationData[i] = new FeatType[layerConfig[i] * graph.getNumGhostVertices()];
    }

    // Create labels storage area. Read in labels and store as one-hot format.
    localVerticesLabels = new FeatType[layerConfig[numLayers] * graph.getNumLocalVertices()];

    // Set a local index for all ghost vertices along the way. This index is used for indexing within the ghost data arrays.
    IdType ghostCount = 0;
    for (auto it = graph.getGhostVertices().begin(); it != graph.getGhostVertices().end(); ++it)
        it->second.setLocalId(ghostCount++);

    // Read in initial feature values (input features) & labels.
    readFeaturesFile(featuresFile);
    readLabelsFile(labelsFile);

    // Initialize synchronization utilities.
    lockCurrId.init();
    lockRecvWaiters.init();
    condRecvWaitersEmpty.init(lockRecvWaiters);
    lockHalt.init();

    // Initialize computation thread barrier.
    barComp.init(cThreads);

    // Create computation workers thread pool.
    computePool = new ThreadPool(cThreads);
    computePool->createPool();
    printLog(nodeId, "Created %u computation threads.\n", cThreads);

    // Create data communicators thread pool.
    dataPool = new ThreadPool(dThreads);
    dataPool->createPool();
    printLog(nodeId, "Created %u data communicator threads.\n", dThreads);

    // Compact the graph.
    graph.compactGraph();

    // Create the lambda communication manager.
    lambdaComm = new LambdaComm(NodeManager::getNode(nodeId).pubip, dataserverPort, coordserverIp, coordserverPort, nodeId, 10, 1);

    timeInit += getTimer();
    printLog(nodeId, "Engine initialization complete.\n");

    // Make sure all nodes finish initailization. No need to add a global barrier before running then.
    NodeManager::barrier();
}



/**
 *
 * Whether I am the master mode or not.
 * 
 */
bool
Engine::master() {
    return NodeManager::amIMaster();
}


/**
 *
 * Run the engine with the given vertex program (whose `update()` member function is customized).
 * Will start a bunch of worker threads and a bunch of data communicator threads.
 * 
 */
void
Engine::run() {
    printLog(nodeId, "Engine starts running...\n");

    timeProcess = -getTimer();

    // Set initial conditions.
    currId = 0;
    iteration = 0;
    halt = false;

    // Create buffer for first-layer aggregation.
    localVerticesDataBuf = new FeatType[layerConfig[0] * graph.getNumLocalVertices()];

    // Start data communicators.
    dataPool->perform(dataCommunicator);

    // Start workers.
    computePool->perform(worker);

    // Join all workers.
    computePool->sync();

    timeProcess += getTimer();

    // Join all data communicators.
    dataPool->sync();

    printLog(nodeId, "Engine completes the processing at iteration %u.\n", iteration);
}


/**
 *
 * Write output stuff to the tmp directory for every local vertex.
 * Write engine timing metrics to the logfile.
 * 
 */
void
Engine::output() {

    timeOutput = -getTimer();

    // Output results.
    std::ofstream outStream(outFile.c_str());

    if (!outStream.good())
        printLog(nodeId, "Cannot open output file: %s [Reason: %s]\n", outFile.c_str(), std::strerror(errno));

    assert(outStream.good());

    //
    // The following are full feature values outputing.
    //
    // for (Vertex& v : graph.getVertices()) {
    //     outStream << v.getGlobalId() << ": ";
    //     for (size_t i = 0; i <= numLayers; ++i) {
    //         FeatType *dataPtr = localVertexActivationDataPtr(v.getLocalId(), i);
    //         for (size_t j = 0; j < layerConfig[i]; ++j)
    //             outStream << dataPtr[j] << " ";
    //         outStream << "| ";
    //     }
    //     outStream << std::endl;
    // }

    //
    // The following are labels outputing.
    //
    // for (Vertex& v : graph.getVertices()) {
    //     outStream << v.getGlobalId() << ": ";
    //     FeatType *labelsPtr = localVertexLabelsPtr(v.getLocalId());
    //     for (LabelType i = 0; i < layerConfig[numLayers]; ++i)
    //         outStream << labelsPtr[i] << " ";
    //     outStream << std::endl;
    // }

    timeOutput += getTimer();

    // Benchmarking results.
    if (master()) {
        assert(vecTimeAggregate.size() == numLayers);
        assert(vecTimeLambda.size() == numLayers);
        assert(vecTimeSendout.size() == numLayers);
        printEngineMetrics();
    }
}


/**
 *
 * Destroy the engine.
 * 
 */
void
Engine::destroy() {
    printLog(nodeId, "Destroying the engine...\n");

    NodeManager::destroy();
    CommManager::destroy();
    computePool->destroyPool();
    dataPool->destroyPool();

    lockCurrId.destroy();
    lockRecvWaiters.destroy();
    condRecvWaitersEmpty.destroy();
    lockHalt.destroy();

    lambdaComm->sendShutdownMessage();
    delete lambdaComm;

    for (size_t i = 0; i <= numLayers; ++i) {
        delete[] localVerticesZData[i];
        delete[] localVerticesActivationData[i];
        delete[] ghostVerticesActivationData[i];
    }
    delete[] localVerticesZData;
    delete[] localVerticesActivationData;
    delete[] ghostVerticesActivationData;

    delete[] localVerticesDataBuf;
    delete[] localVerticesLabels;
}


/////////////////////////////////////////////////
// Below are private functions for the engine. //
/////////////////////////////////////////////////


/**
 *
 * Major part of the engine's computation logic is done by workers. When the engine runs it wakes threads up from the thread pool
 * and assign a worker function for each.
 * 
 */
void
Engine::worker(unsigned tid, void *args) {

    // A Stopwatch for composing steps of run().
    double timeWorker = getTimer();

    // Outer while loop. Looping infinitely, looking for a new task to handle.
    while (1) {

        // Get current vertex that need to be handled.
        lockCurrId.lock();
        IdType lvid = currId++;
        lockCurrId.unlock();

        // All local vertices have been processed. Hit the barrier and wait for next iteration / decide to halt.
        if (lvid >= graph.getNumLocalVertices()) {

            // Non-master threads.
            if (tid != 0) {

                //## Worker barrier 1: Everyone reach to this point, then only master will work. ##//
                barComp.wait();

                //## Worker barrier 2: Master has finished its checking work. ##//
                barComp.wait();

                // If master says halt then go to death; else continue for the new iteration.
                if (halt)
                    return;
                else
                    continue;

            // Master thread (tid == 0).
            } else {

                //## Worker barrier 1: Everyone reach to this point, then only master will work. ##//
                barComp.wait();

                vecTimeAggregate.push_back(getTimer() - timeWorker);
                timeWorker = getTimer();
                printLog(nodeId, "Iteration %u finishes. Invoking lambda...\n", iteration);

                // Start a new lambda communication context.
                lambdaComm->startContext(localVerticesDataBuf, localVerticesZData[iteration + 1], localVerticesActivationData[iteration + 1],
                                         graph.getNumLocalVertices(), getNumFeats(iteration), getNumFeats(iteration + 1), iteration);

                // Trigger a request towards the coordicate server. Wait until the request completes.
                std::thread treq([&] {
                    lambdaComm->requestLambdas();
                });
                treq.join();

                vecTimeLambda.push_back(getTimer() - timeWorker);
                timeWorker = getTimer();
                printLog(nodeId, "All lambda requests finished. Results received.\n");

                // End this lambda communciation context.
                lambdaComm->endContext();

                // Loop through all local vertices and do the data send out work. If there are any remote edges for a vertex, should send this vid to
                // other nodes for their ghost's update.
                for (IdType id = 0; id < graph.getNumLocalVertices(); ++id) {
                    Vertex& v = graph.getVertex(id);
                    for (unsigned i = 0; i < v.getNumOutEdges(); ++i) {
                        if (v.getOutEdge(i).getEdgeLocation() == REMOTE_EDGE_TYPE) {
                            IdType gvid = graph.localToGlobalId[id];

                            // Record that this vid gets broadcast in this iteration. Should wait for its corresponding respond.
                            lockRecvWaiters.lock();
                            recvWaiters[gvid] = numNodes;
                            lockRecvWaiters.unlock();

                            FeatType *dataPtr = localVertexActivationDataPtr(id, iteration + 1);
                            CommManager::dataPushOut(gvid, dataPtr, getNumFeats(iteration + 1) * sizeof(FeatType));

                            break;
                        }
                    }
                }

                // Wait for all remote schedulings sent by me to be handled.
                lockRecvWaiters.lock();
                if (!recvWaiters.empty())
                    condRecvWaitersEmpty.wait();
                lockRecvWaiters.unlock();

                //## Global Iteration barrier. ##//
                NodeManager::barrier();

                vecTimeSendout.push_back(getTimer() - timeWorker);
                timeWorker = getTimer();
                printLog(nodeId, "Global barrier after ghost data exchange crossed.\n");

                // There is a new layer ahead, please start a new iteration.
                if (++iteration < numLayers) {
                    printLog(nodeId, "Starting a new iteration %u at %.3lf ms...\n", iteration, timeProcess + getTimer());

                    // Reset data buffer area's size.
                    delete[] localVerticesDataBuf;
                    localVerticesDataBuf = new FeatType[layerConfig[iteration] * graph.getNumLocalVertices()];

                    // Reset current id.
                    currId = 0;       // This is unprotected by lockCurrId because only master executes.

                    //## Worker barrier 2: Starting a new iteration. ##//
                    barComp.wait();

                    continue;

                // No more, so deciding to halt. But still needs the communicator to check if there will be further scheduling invoked by ghost
                // vertices. If so we are stilling going to the next iteration.
                } else {
                    printLog(nodeId, "Deciding to halt at iteration %u...\n", iteration);

                    // Set this to signal data communicator to end its life.
                    lockHalt.lock();
                    halt = true;
                    lockHalt.unlock();

                    //## Worker barrier 2: Going to die. ##//
                    barComp.wait();

                    return;
                }
            }
        }

        // Doing the aggregation. Data send out used to be here, but we changed it to after lambda finishes.
        aggregateFromNeighbors(lvid);
    }
}


/**
 *
 * Major part of the engine's communication logic is done by data threads. These threads loop asynchronously with computation workers.
 * 
 */
void
Engine::dataCommunicator(unsigned tid, void *args) {
    IdType topic;
    FeatType *msgbuf = (FeatType *) new char[MAX_MSG_SIZE];

    // While loop, looping infinitely to get the next message.
    while (1) {

        // No message in queue.
        if (!CommManager::dataPullIn(&topic, msgbuf, MAX_MSG_SIZE)) {

            // Computation workers done their work, so communicator goes to death as well.
            if (halt) {
                delete[] msgbuf;
                return;
            }

        // Pull in the next message, and process this message.
        } else {

            // A normal ghost value broadcast.
            if (0 <= topic && topic < graph.getNumGlobalVertices()) {
                IdType gvid = topic;

                // Update the ghost vertex if it is one of mine.
                if (graph.containsGhostVertex(gvid)) {
                    FeatType *dataPtr = ghostVertexActivationDataPtr(graph.getGhostVertex(gvid).getLocalId(), iteration + 1);
                    memcpy(dataPtr, msgbuf, getNumFeats(iteration + 1) * sizeof(FeatType));
                }

                // Using MAX_IDTYPE - gvid as the receive signal topic for vertex gvid.
                CommManager::dataPushOut(MAX_IDTYPE - gvid, NULL, 0);

            // A respond to a broadcast, and the topic vertex is in my local vertices. I should update the
            // corresponding recvWaiter's value. If waiters become empty, send a signal in case the workers are
            // waiting on it to be empty at the iteration barrier.
            } else if (MAX_IDTYPE >= topic && topic > MAX_IDTYPE - graph.getNumGlobalVertices()) {
                IdType gvid = MAX_IDTYPE - topic;

                if (graph.globalToLocalId.find(gvid) != graph.globalToLocalId.end()) {
                    lockRecvWaiters.lock();
                    assert(recvWaiters.find(gvid) != recvWaiters.end());
                    --recvWaiters[gvid];
                    if (recvWaiters[gvid] == 0) {
                        recvWaiters.erase(gvid);
                        if (recvWaiters.empty())
                            condRecvWaitersEmpty.signal();
                    }
                    lockRecvWaiters.unlock();
                }
            }
        }
    }
}


/**
 *
 * Get number of features in the current layer.
 * 
 */
unsigned
Engine::getNumFeats(unsigned layer) {
    return layerConfig[layer];
}


/**
 *
 * Get the data pointer in dataAll area.
 * 
 */
FeatType *
Engine::localVertexZDataPtr(IdType lvid, unsigned layer) {
    return (FeatType *) (localVerticesZData[layer]) + lvid * getNumFeats(layer);
}

FeatType *
Engine::localVertexActivationDataPtr(IdType lvid, unsigned layer) {
    return (FeatType *) (localVerticesActivationData[layer]) + lvid * getNumFeats(layer);
}

FeatType *
Engine::ghostVertexActivationDataPtr(IdType lvid, unsigned layer) {
    return (FeatType *) (ghostVerticesActivationData[layer]) + lvid * getNumFeats(layer);
}


/**
 *
 * Get the data pointer to a local vertex's data buffer.
 * 
 */
FeatType *
Engine::localVertexDataBufPtr(IdType lvid, unsigned layer) {
    return (FeatType *) localVerticesDataBuf + lvid * getNumFeats(layer);
}


/**
 *
 * Get the data pointer to a local vertex's labels area.
 * 
 */
FeatType *
Engine::localVertexLabelsPtr(IdType lvid) {
    return (FeatType *) localVerticesLabels + lvid * getNumFeats(numLayers);
}


/**
 *
 * Aggregate numFeats feature values starting from offset from all neighbors (including self). Then write the results to the
 * data buffer area for serialization. The results are to be used for being sent to lambda threads.
 * 
 */
void
Engine::aggregateFromNeighbors(IdType lvid) {
    unsigned numFeats = getNumFeats(iteration);

    // Read out data of the current iteration of given vertex.
    FeatType *currDataDst = localVertexDataBufPtr(lvid, iteration);
    FeatType *currDataPtr = localVertexActivationDataPtr(lvid, iteration);
    memcpy(currDataDst, currDataPtr, numFeats * sizeof(FeatType));

    // Apply normalization factor on the current data.
    Vertex& v = graph.getVertex(lvid);
    for (unsigned i = 0; i < numFeats; ++i)
        currDataDst[i] *= v.getNormFactor();

    // Aggregate from incoming neighbors.
    for (unsigned i = 0; i < v.getNumInEdges(); ++i) {
        FeatType *otherDataPtr;
        EdgeType normFactor = v.getInEdge(i).getData();

        if (v.getInEdge(i).getEdgeLocation() == LOCAL_EDGE_TYPE)    // Local vertex.
            otherDataPtr = localVertexActivationDataPtr(v.getSourceVertexLocalId(i), iteration);
        else                                                        // Ghost vertex.
            otherDataPtr = ghostVertexActivationDataPtr(v.getSourceVertexLocalId(i), iteration);

        // TODO: Locks on the data array area is not properly set yet. But does not affect forward prop.
        for (unsigned j = 0; j < numFeats; ++j)
            currDataDst[j] += (otherDataPtr[j] * normFactor);
    }
}


/**
 *
 * Print engine metrics of processing time.
 * 
 */
void
Engine::printEngineMetrics() {
    printLog(nodeId, "Engine METRICS: Initialization takes %.3lf ms\n", timeInit);
    printLog(nodeId, "Engine METRICS: Time per stage:\n");
    for (size_t i = 0; i < numLayers; ++i) {
        printLog(nodeId, "\t\t\tAggregation   %2d  %.3lf ms\n", i, vecTimeAggregate[i]);
        printLog(nodeId, "\t\t\tLambda        %2d  %.3lf ms\n", i, vecTimeLambda[i]);
        printLog(nodeId, "\t\t\tGhost update  %2d  %.3lf ms\n", i, vecTimeSendout[i]);
    }
    printLog(nodeId, "Engine METRICS: Total processing time %.3lf ms\n", timeProcess);
    printLog(nodeId, "Engine METRICS: Output writing takes %.3lf ms\n", timeOutput);
}


/**
 *
 * Print my graph's metrics.
 * 
 */
void
Engine::printGraphMetrics() {
    printLog(nodeId, "Graph METRICS: %u global vertices, %llu global edges, %u local edges.\n",
                     graph.getNumGlobalVertices(), graph.getNumGlobalEdges(), graph.getNumLocalVertices());
}


/**
 *
 * Parse command line arguments.
 * 
 */
void
Engine::parseArgs(int argc, char *argv[]) {
    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
        ("help", "Produce help message")

        ("graphfile", boost::program_options::value<std::string>(), "Path to the binary file contatining the edge list")
        ("featuresfile", boost::program_options::value<std::string>(), "Path to the file containing the vertex features")
        ("layerfile", boost::program_options::value<std::string>(), "Layer configuration file")
        ("labelsfile", boost::program_options::value<std::string>(), "Target labels file")
        ("dshmachinesfile", boost::program_options::value<std::string>(), "DSH machines file")
        ("pripfile", boost::program_options::value<std::string>(), "File containing my private ip")
        ("pubipfile", boost::program_options::value<std::string>(), "File containing my public ip")

        ("tmpdir", boost::program_options::value<std::string>(), "Temporary directory")

        ("dataserverport", boost::program_options::value<unsigned>(), "The port exposing to the coordination server")
        ("coordserverip", boost::program_options::value<std::string>(), "The private IP address of the coordination server")
        ("coordserverport", boost::program_options::value<unsigned>(), "The port of the listener on the coordination server")

        // Default is directed graph!
        ("undirected", boost::program_options::value<unsigned>()->default_value(unsigned(0), "0"), "Graph type is undirected or not")

        ("dthreads", boost::program_options::value<unsigned>(), "Number of data threads")
        ("cthreads", boost::program_options::value<unsigned>(), "Number of compute threads")

        ("dataport", boost::program_options::value<unsigned>(), "Port for data communication")
        ("ctrlport", boost::program_options::value<unsigned>(), "Port start for control communication")
        ("nodeport", boost::program_options::value<unsigned>(), "Port for node manager")
        ;

    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(desc).allow_unregistered().run(), vm);
    boost::program_options::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        exit(-1);
    }

    assert(vm.count("dthreads"));
    dThreads = vm["dthreads"].as<unsigned>();   // Communicator threads.

    assert(vm.count("cthreads"));
    cThreads = vm["cthreads"].as<unsigned>();   // Computation threads.

    assert(vm.count("graphfile"));
    graphFile = vm["graphfile"].as<std::string>();

    assert(vm.count("featuresfile"));
    featuresFile = vm["featuresfile"].as<std::string>();

    assert(vm.count("layerfile"));
    layerConfigFile = vm["layerfile"].as<std::string>();

    assert(vm.count("labelsfile"));
    labelsFile = vm["labelsfile"].as<std::string>();

    assert(vm.count("dshmachinesfile"));
    dshMachinesFile = vm["dshmachinesfile"].as<std::string>();

    assert(vm.count("pripfile"));
    myPrIpFile = vm["pripfile"].as<std::string>();

    assert(vm.count("pubipfile"));
    myPubIpFile = vm["pubipfile"].as<std::string>();

    assert(vm.count("tmpdir"));
    outFile = vm["tmpdir"].as<std::string>() + "/output_";  // Still needs to append the node id, after node manager set up.

    assert(vm.count("dataserverport"));
    dataserverPort = vm["dataserverport"].as<unsigned>();

    assert(vm.count("coordserverip"));
    coordserverIp = vm["coordserverip"].as<std::string>();

    assert(vm.count("coordserverport"));
    coordserverPort = vm["coordserverport"].as<unsigned>();

    assert(vm.count("undirected"));
    undirected = (vm["undirected"].as<unsigned>() == 0) ? false : true;

    assert(vm.count("dataport"));
    unsigned data_port = vm["dataport"].as<unsigned>();
    CommManager::setDataPort(data_port);

    assert(vm.count("ctrlport"));
    unsigned ctrl_port = vm["ctrlport"].as<unsigned>();
    CommManager::setControlPortStart(ctrl_port);

    assert(vm.count("nodeport"));
    unsigned node_port = vm["nodeport"].as<unsigned>();
    NodeManager::setNodePort(node_port);

    printLog(404, "Parsed configuration: dThreads = %u, cThreads = %u, graphFile = %s, featuresFile = %s, dshMachinesFile = %s, "
                  "myPrIpFile = %s, myPubIpFile = %s, undirected = %s, data port set -> %u, control port set -> %u, node port set -> %u\n",
                  dThreads, cThreads, graphFile.c_str(), featuresFile.c_str(), dshMachinesFile.c_str(),
                  myPrIpFile.c_str(), myPubIpFile.c_str(), undirected ? "true" : "false", data_port, ctrl_port, node_port);
}


/**
 *
 * Read in the layer configuration file.
 * 
 */
void
Engine::readLayerConfigFile(std::string& layerConfigFileName) {
    std::ifstream infile(layerConfigFileName.c_str());
    if (!infile.good())
        printLog(nodeId, "Cannot open layer configuration file: %s [Reason: %s]\n", layerConfigFileName.c_str(), std::strerror(errno));

    assert(infile.good());

    // Loop through each line.
    std::string line;
    while (!infile.eof()) {
        std::getline(infile, line);
        boost::algorithm::trim(line);

        if (line.length() > 0)
            layerConfig.push_back(std::stoul(line));
    }

    assert(layerConfig.size() > 1);
}


/**
 *
 * Read in the initial features file.
 * 
 */
void
Engine::readFeaturesFile(std::string& featuresFileName) {
    std::ifstream infile(featuresFileName.c_str());
    if (!infile.good())
        printLog(nodeId, "Cannot open features file: %s [Reason: %s]\n", featuresFileName.c_str(), std::strerror(errno));

    assert(infile.good());

    FeaturesHeaderType fHeader;
    infile.read((char *) &fHeader, sizeof(FeaturesHeaderType));
    assert(fHeader.numFeatures == layerConfig[0]);

    IdType gvid = 0;
    
    LabelType nFeats = fHeader.numFeatures;
    std::vector<FeatType> feature_vec;
    feature_vec.reserve(nFeats);
    FeatType curr;

    while (infile.read(reinterpret_cast<char *> (&curr) , sizeof(FeatType))){
        feature_vec.push_back(curr);
        if (feature_vec.size() == nFeats) {

            // Set the vertex's initial values, if it is one of my local vertices / ghost vertices.
            if (graph.containsGhostVertex(gvid)) {      // Global vertex.
                FeatType *actDataPtr = ghostVertexActivationDataPtr(graph.getGhostVertex(gvid).getLocalId(), 0);
                memcpy(actDataPtr, feature_vec.data(), feature_vec.size() * sizeof(FeatType));
            } else if (graph.containsVertex(gvid)) {    // Local vertex.
                FeatType *zDataPtr = localVertexZDataPtr(graph.getVertexByGlobal(gvid).getLocalId(), 0);
                FeatType *actDataPtr = localVertexActivationDataPtr(graph.getVertexByGlobal(gvid).getLocalId(), 0);
                memcpy(zDataPtr, feature_vec.data(), feature_vec.size() * sizeof(FeatType));
                memcpy(actDataPtr, feature_vec.data(), feature_vec.size() * sizeof(FeatType));
            }

            ++gvid;
            feature_vec.clear();
        }
    }
    infile.close();

    assert(gvid == graph.getNumGlobalVertices());
}


/**
 *
 * Read in the labels file, store the labels in one-hot format.
 * 
 */
void
Engine::readLabelsFile(std::string& labelsFileName) {
    std::ifstream infile(labelsFileName.c_str());
    if (!infile.good())
        printLog(nodeId, "Cannot open labels file: %s [Reason: %s]\n", labelsFileName.c_str(), std::strerror(errno));

    assert(infile.good());

    LabelsHeaderType fHeader;
    infile.read((char *) &fHeader, sizeof(LabelsHeaderType));
    assert(fHeader.labelKinds == layerConfig[numLayers]);

    IdType gvid = 0;
    
    LabelType lKinds = fHeader.labelKinds;
    LabelType curr;
    FeatType one_hot_arr[lKinds] = {0};

    while (infile.read(reinterpret_cast<char *> (&curr) , sizeof(LabelType))) {

        // Set the vertex's label values, if it is one of my local vertices & is labeled.
        if (graph.containsVertex(gvid)) {

            // Convert into a one-hot array.
            assert(curr < lKinds);
            memset(one_hot_arr, 0, lKinds * sizeof(FeatType));
            one_hot_arr[curr] = 1.0;

            FeatType *labelPtr = localVertexLabelsPtr(graph.getVertexByGlobal(gvid).getLocalId());
            memcpy(labelPtr, one_hot_arr, lKinds * sizeof(FeatType));
        }

        ++gvid;
    }
    infile.close();

    assert(gvid == graph.getNumGlobalVertices());
}


/**
 *
 * Read in the partition file.
 * 
 */
void
Engine::readPartsFile(std::string& partsFileName, Graph& lGraph) {
    std::ifstream infile(partsFileName.c_str());
    if (!infile.good())
        printLog(nodeId, "Cannot open patition file: %s [Reason: %s]\n", partsFileName.c_str(), std::strerror(errno));

    assert(infile.good());

    short partId;
    IdType lvid = 0;
    IdType gvid = 0;

    std::string line;
    while (std::getline(infile, line)) {
        if (line.size() == 0 || (line[0] < '0' || line[0] > '9'))
            continue;

        std::istringstream iss(line);
        if (!(iss >> partId))
            break;

        lGraph.appendVertexPartitionId(partId);

        if (partId == nodeId) {
            lGraph.localToGlobalId[lvid] = gvid;
            lGraph.globalToLocalId[gvid] = lvid;

            ++lvid;
        }
        ++gvid;
    }

    lGraph.setNumGlobalVertices(gvid);
    lGraph.setNumLocalVertices(lvid);
}


/**
 *
 * Process an edge read from the binary snap file.
 * 
 */
void
Engine::processEdge(IdType& from, IdType& to, Graph& lGraph, std::set<IdType> *inTopics, std::set<IdType> *oTopics) {
    if (lGraph.getVertexPartitionId(from) == nodeId) {
        IdType lFromId = lGraph.globalToLocalId[from];
        IdType toId;
        EdgeLocationType eLocation;

        if (lGraph.getVertexPartitionId(to) == nodeId) {
            toId = lGraph.globalToLocalId[to];
            eLocation = LOCAL_EDGE_TYPE;
        } else {
            toId = to;
            eLocation = REMOTE_EDGE_TYPE;
            lGraph.getVertex(lFromId).setVertexLocation(BOUNDARY_VERTEX);

            if (oTopics != NULL)
                oTopics->insert(from);
        }

        lGraph.getVertex(lFromId).addOutEdge(OutEdge(toId, eLocation, EdgeType()));
    }

    if (lGraph.getVertexPartitionId(to) == nodeId) {
        IdType lToId = lGraph.globalToLocalId[to];
        IdType fromId;
        EdgeLocationType eLocation;

        if (lGraph.getVertexPartitionId(from) == nodeId) {
            fromId = lGraph.globalToLocalId[from];
            eLocation = LOCAL_EDGE_TYPE;
        } else {
            fromId = from;
            eLocation = REMOTE_EDGE_TYPE;

            if (!lGraph.containsGhostVertex(from))
                lGraph.getGhostVertices()[from] = GhostVertex();
            lGraph.getGhostVertex(from).addOutEdge(lToId);

            if (inTopics != NULL)
                inTopics->insert(from);
        }

        lGraph.getVertex(lToId).addInEdge(InEdge(fromId, eLocation, EdgeType()));
    }
}


/**
 *
 * Set the normalization factors on all edges.
 * 
 */
void
Engine::setEdgeNormalizations() {
    for (Vertex& vertex : graph.getVertices()) {
        unsigned dstDeg = vertex.getNumInEdges() + 1;
        float dstNorm = std::pow(dstDeg, -.5);
        vertex.setNormFactor(dstNorm * dstNorm);
        for (unsigned i = 0; i < vertex.getNumInEdges(); ++i) {
            InEdge& e = vertex.getInEdge(i);
            IdType vid = e.getSourceId();
            if (e.getEdgeLocation() == LOCAL_EDGE_TYPE) {
                unsigned srcDeg = graph.getVertex(vid).getNumInEdges() + 1;
                float srcNorm = std::pow(srcDeg, -.5);
                e.setData(srcNorm * dstNorm);
            } else {
                unsigned ghostDeg = graph.getGhostVertex(vid).getDegree() + 1;
                float ghostNorm = std::pow(ghostDeg, -.5);
                e.setData(ghostNorm * dstNorm);
            }
        }
    }
}


/**
 *
 * Finds the in degree of all ghost vertices.
 * 
 */
void
Engine::findGhostDegrees(std::string& fileName) {
    std::ifstream infile(fileName.c_str(), std::ios::binary);
    if (!infile.good())
        printLog(nodeId, "Cannot open BinarySnap file: %s\n", fileName.c_str());

    assert(infile.good());

    BSHeaderType bsHeader;
    infile.read((char *) &bsHeader, sizeof(bsHeader));

    IdType srcdst[2];
    while (infile.read((char *) srcdst, bsHeader.sizeOfVertexType * 2)) {
        if (srcdst[0] == srcdst[1])
            continue;

        if (graph.containsGhostVertex(srcdst[1]))
            graph.getGhostVertex(srcdst[1]).incrementDegree();
    }
    
    infile.close();
}


/**
 *
 * Read and parse the graph from the graph binary snap file.
 * 
 */
void
Engine::readGraphBS(std::string& fileName, std::set<IdType>& inTopics, std::vector<IdType>& outTopics) {
    
    // Read in the partition file.
    std::string partsFileName = fileName + PARTS_EXT;
    readPartsFile(partsFileName, graph);

    // Initialize the graph based on the partition info.
    graph.getVertices().resize(graph.getNumLocalVertices());
    for (IdType i = 0; i < graph.getNumLocalVertices(); ++i) {
        graph.getVertex(i).setLocalId(i);
        graph.getVertex(i).setGlobalId(graph.localToGlobalId[i]);
        graph.getVertex(i).setVertexLocation(INTERNAL_VERTEX);
        graph.getVertex(i).setGraphPtr(&graph);
    }

    // Read in the binary snap edge file.
    std::string edgeFileName = fileName + EDGES_EXT;
    std::ifstream infile(edgeFileName.c_str(), std::ios::binary);
    if(!infile.good())
        printLog(nodeId, "Cannot open BinarySnap file: %s\n", edgeFileName.c_str());

    assert(infile.good());

    BSHeaderType bSHeader;
    infile.read((char *) &bSHeader, sizeof(bSHeader));
    assert(bSHeader.sizeOfVertexType == sizeof(IdType));

    // Loop through all edges and process them.
    std::set<IdType> oTopics;
    IdType srcdst[2];
    while (infile.read((char *) srcdst, bSHeader.sizeOfVertexType * 2)) {
        if (srcdst[0] == srcdst[1])
            continue;

        processEdge(srcdst[0], srcdst[1], graph, &inTopics, &oTopics);
        if (undirected)
            processEdge(srcdst[1], srcdst[0], graph, &inTopics, &oTopics);
        graph.incrementNumGlobalEdges();
    }

    infile.close();

    // Extra works added.
    graph.setNumGhostVertices(graph.getGhostVertices().size());
    findGhostDegrees(edgeFileName);
    setEdgeNormalizations();

    typename std::set<IdType>::iterator it;
    for (it = oTopics.begin(); it != oTopics.end(); ++it)
        outTopics.push_back(*it);
}
