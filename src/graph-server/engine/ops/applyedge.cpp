#include "../engine.hpp"

#include <omp.h>

#include <algorithm>
#include <boost/algorithm/string/classification.hpp>  // Include boost::for is_any_of.
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>


FeatType **Engine::applyEdge(EdgeType *edgsTensor, unsigned edgsCnt,
                             unsigned eFeatDim, FeatType **eSrcVtcsTensor,
                             FeatType **eDstVtcsTensor, unsigned inFeatDim,
                             unsigned outFeatDim) {
    double sttTimer = getTimer();

    // do nothing
    FeatType **outputTensor = eSrcVtcsTensor;
    eSrcVtcsTensor = NULL;

    if (vecTimeApplyEdg.size() < numLayers) {
        vecTimeApplyEdg.push_back(getTimer() - sttTimer);
    } else {
        vecTimeApplyEdg[layer] += getTimer() - sttTimer;
    }

    return outputTensor;
}


FeatType **Engine::applyEdgeBackward(EdgeType *edgsTensor, unsigned edgsCnt,
                                     unsigned eFeatDim,
                                     FeatType **eSrcVGradTensor,
                                     FeatType **eDstVGradTensor,
                                     unsigned inFeatDim, unsigned outFeatDim) {
    double sttTimer = getTimer();

    FeatType **outputTensor = eDstVGradTensor;
    eDstVGradTensor = NULL;

    if (vecTimeApplyEdg.size() < 2 * numLayers) {
        for (unsigned i = vecTimeApplyEdg.size(); i < 2 * numLayers; i++) {
            vecTimeApplyEdg.push_back(0.0);
        }
    }
    vecTimeApplyEdg[numLayers + layer - 1] += getTimer() - sttTimer;
    return outputTensor;
}



/////////////////////////////////////////////////////////
// Below are private forward functions for the engine. //
/////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////
// Below are private backward functions for the engine. //
//////////////////////////////////////////////////////////


// reshape vtcs tensor to edgs tensor. Each element in edgsTensor is a reference
// to a vertex feature. Both src vtx features and dst vtx features included in
// edgsTensor. [srcV Feats (local inEdge cnt); dstV Feats (local inEdge cnt)]
FeatType **Engine::srcVFeats2eFeats(FeatType *vtcsTensor, FeatType *ghostTensor,
                                    unsigned vtcsCnt, unsigned featDim) {
    underlyingVtcsTensorBuf = vtcsTensor;

    FeatType **eVtxFeatsBuf = new FeatType *[2 * graph.localInEdgeCnt];
    FeatType **eSrcVtxFeats = eVtxFeatsBuf;
    FeatType **eDstVtxFeats = eSrcVtxFeats + graph.localInEdgeCnt;

    unsigned long long edgeItr = 0;
    for (unsigned lvid = 0; lvid < graph.localVtxCnt; ++lvid) {
        for (unsigned long long eid = graph.forwardAdj.columnPtrs[lvid];
             eid < graph.forwardAdj.columnPtrs[lvid + 1]; ++eid) {
            unsigned srcVid = graph.forwardAdj.rowIdxs[eid];
            if (srcVid < graph.localVtxCnt) {
                eSrcVtxFeats[edgeItr] = getVtxFeat(vtcsTensor, srcVid, featDim);
            } else {
                eSrcVtxFeats[edgeItr] = getVtxFeat(
                    ghostTensor, srcVid - graph.localVtxCnt, featDim);
            }
            eDstVtxFeats[edgeItr] = getVtxFeat(vtcsTensor, lvid, featDim);
            ++edgeItr;
        }
    }

    return eVtxFeatsBuf;
}

// similar to srcVFeats2eFeats, but based on outEdges of local vertices.
// [dstV Feats (local outEdge cnt); srcV Feats (local outEdge cnt)]
FeatType **Engine::dstVFeats2eFeats(FeatType *vtcsTensor, FeatType *ghostTensor,
                                    unsigned vtcsCnt, unsigned featDim) {
    underlyingVtcsTensorBuf = vtcsTensor;

    FeatType **eVtxFeatsBuf = new FeatType *[2 * graph.localOutEdgeCnt];
    FeatType **eSrcVtxFeats = eVtxFeatsBuf;
    FeatType **eDstVtxFeats = eSrcVtxFeats + graph.localOutEdgeCnt;

    unsigned long long edgeItr = 0;
    for (unsigned lvid = 0; lvid < graph.localVtxCnt; ++lvid) {
        for (unsigned long long eid = graph.backwardAdj.rowPtrs[lvid];
             eid < graph.backwardAdj.rowPtrs[lvid + 1]; ++eid) {
            unsigned srcVid = graph.backwardAdj.columnIdxs[eid];
            if (srcVid < graph.localVtxCnt) {
                eSrcVtxFeats[edgeItr] = getVtxFeat(vtcsTensor, srcVid, featDim);
            } else {
                eSrcVtxFeats[edgeItr] = getVtxFeat(
                    ghostTensor, srcVid - graph.localVtxCnt, featDim);
            }
            eDstVtxFeats[edgeItr] = getVtxFeat(vtcsTensor, lvid, featDim);
            ++edgeItr;
        }
    }

    return eVtxFeatsBuf;
}

// FeatType *
// Engine::eFeats2srcVFeats(FeatType **edgsTensor, unsigned edgsCnt, unsigned
// featDim) {
//     FeatType *vtcsTensor = new FeatType [graph.localVtxCnt];
//     memset(vtcsTensor, 0, sizeof(FeatType) * graph.localVtxCnt);

//     unsigned edgeItr = 0;
//     for (unsigned lvid = 0; lvid < graph.localVtxCnt; ++lvid) {
//         FeatType *vtxFeat = getVtxFeat(vtcsTensor, lvid, featDim);
//         for (unsigned long long eid = graph.forwardAdj.columnPtrs[lvid]; eid
//         < graph.forwardAdj.columnPtrs[lvid + 1]; ++eid) {
//             for (unsigned j = 0; j < featDim; ++j) {
//                 vtxFeat[j] += edgsTensor[edgeItr][j];
//             }
//             ++edgeItr;
//         }
//     }

//     return vtcsTensor;
// }

// FeatType *
// Engine::eFeats2dstVFeats(FeatType **edgsTensor, unsigned edgsCnt, unsigned
// featDim) {
//     FeatType *vtcsTensor = new FeatType [graph.localVtxCnt];
//     memset(vtcsTensor, 0, sizeof(FeatType) * graph.localVtxCnt);

//     unsigned edgeItr = 0;
//     for (unsigned lvid = 0; lvid < graph.localVtxCnt; ++lvid) {
//         FeatType *vtxFeat = getVtxFeat(vtcsTensor, lvid, featDim);
//         for (unsigned long long eid = graph.backwardAdj.rowPtrs[lvid]; eid <
//         graph.backwardAdj.rowPtrs[lvid + 1]; ++eid) {
//             for (unsigned j = 0; j < featDim; ++j) {
//                 vtxFeat[j] += edgsTensor[edgeItr][j];
//             }
//             ++edgeItr;
//         }
//     }

//     return vtcsTensor;
// }