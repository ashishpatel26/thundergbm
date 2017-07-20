/*
 * DeviceSplitterKernel.h
 *
 *  Created on: 10 May 2016
 *      Author: Zeyi Wen
 *		@brief: 
 */

#ifndef DEVICESPLITTERKERNEL_H_
#define DEVICESPLITTERKERNEL_H_

#include <float.h>
#include <limits>

#include "../Splitter/DeviceSplitter.h"
#include "../../Host/UpdateOps/SplitPoint.h"
#include "../../DeviceHost/NodeStat.h"
#include "../../DeviceHost/BaseClasses/BaseSplitter.h"
#include "../../SharedUtility/getMin.h"
#include "../../SharedUtility/DataType.h"
#include "../../SharedUtility/CudaMacro.h"
#include "../../SharedUtility/binarySearch.h"
#include "../../SharedUtility/DeviceUtility.h"

//dense array
__global__ void LoadGDHessFvalueRoot(const real *pInsGD, const real *pInsHess, int numIns,
						   const int *pInsId, const real *pAllFeaValue, int numFeaValue,
						   double *pGDEachFeaValue, real *pHessEachFeaValue, real *pDenseFeaValue);
__global__ void LoadGDHessFvalue(const real *pInsGD, const real *pInsHess, int numIns,
						   const int *pInsId, const real *pAllFeaValue, const uint *pDstIndexEachFeaValue, int numFeaValue,
						   double *pGDEachFeaValue, real *pHessEachFeaValue, real *pDenseFeaValue);

__global__ void FirstFeaGain(const unsigned int *pEachFeaStartPosEachNode, int numFeaStartPos, real *pGainOnEachFeaValue, uint numFeaValue);

//helper functions
template<class T>
__device__ bool NeedUpdate(T &RChildHess, T &LChildHess)
{
	if(LChildHess >= DeviceSplitter::min_child_weight && RChildHess >= DeviceSplitter::min_child_weight)
		return true;
	return false;
}

template<class T>
__global__ void ComputeGainDense(const nodeStat *pSNodeStat, const int *pid2SNPos, real lambda,
							const double *pGDPrefixSumOnEachFeaValue, const real *pHessPrefixSumOnEachFeaValue,
							const real *pDenseFeaValue, int numofDenseValue,
							const uint *pEachFeaStartEachNode, const T *pEachFeaLenEachNode,
							const uint *pnKey, int numFea, real *pGainOnEachFeaValue, bool *pDefault2Right)
{
	const float rt_2eps = 2.0 * DeviceSplitter::rt_eps;
	//one thread loads one value
	uint gTid = GLOBAL_TID();
	if(gTid == 7409)
		printf("hi\n");
	if(gTid >= numofDenseValue)//the thread has no gain to compute, i.e. a thread per gain
		return;

	uint segId = pnKey[gTid];
	uint pid = segId / numFea;

	int snPos = pid2SNPos[pid];
	ECHECKER(snPos);

	if(gTid == 0)
	{
		//assign gain to 0 to the first feature value
    	pGainOnEachFeaValue[gTid] = 0;
		return;
	}

	//if the previous fea value is the same as the current fea value, gain is 0 for the current fea value.
	real preFvalue = pDenseFeaValue[gTid - 1], curFvalue = pDenseFeaValue[gTid];
	if(preFvalue - curFvalue <= rt_2eps && preFvalue - curFvalue >= -rt_2eps)//############## backwards is not considered!
	{//avoid same feature value different gain issue
		pGainOnEachFeaValue[gTid] = 0;
		return;
	}

	int exclusiveSumPos = gTid - 1;//following xgboost using exclusive sum on gd and hess

	//forward consideration (fvalues are sorted descendingly)
	double rChildGD = pGDPrefixSumOnEachFeaValue[exclusiveSumPos];
	double rChildHess = pHessPrefixSumOnEachFeaValue[exclusiveSumPos];
	double parentGD = pSNodeStat[snPos].sum_gd;
	double parentHess = pSNodeStat[snPos].sum_hess;
	double tempGD = parentGD - rChildGD;
	double tempHess = parentHess - rChildHess;
	bool needUpdate = NeedUpdate(rChildHess, tempHess);
    if(needUpdate == true)//need to compute the gain
    {
    	ECHECKER(tempHess > 0);
    	ECHECKER(parentHess > 0);
		double tempGain = (tempGD * tempGD)/(tempHess + lambda) +
						  	   (rChildGD * rChildGD)/(rChildHess + lambda) -
	  						   (parentGD * parentGD)/(parentHess + lambda);
    	pGainOnEachFeaValue[gTid] = tempGain;
    }
    else{
    	//assign gain to 0
    	pGainOnEachFeaValue[gTid] = 0;
    }

    //backward consideration
    int segLen = pEachFeaLenEachNode[segId];
    uint segStartPos = pEachFeaStartEachNode[segId];
    uint lastFvaluePos = segStartPos + segLen - 1;
    if(lastFvaluePos >= numofDenseValue)
    	printf("lastFvaluePos=%u, numDense=%u, segStartPos=%u, segLen=%d, segId=%u, gTid=%u\n", lastFvaluePos, numofDenseValue, segStartPos, segLen, segId, gTid);
    double totalMissingGD = parentGD - pGDPrefixSumOnEachFeaValue[lastFvaluePos];
    double totalMissingHess = parentHess - pHessPrefixSumOnEachFeaValue[lastFvaluePos];
    if(gTid == 7409)
    	printf("# missing value %f, rChildHess=%f, lastHess=%f\n", totalMissingHess, rChildHess, pHessPrefixSumOnEachFeaValue[lastFvaluePos]);
    if(totalMissingHess < 1)//there is no instance with missing values
    	return;
    //missing values to the right child
    rChildGD += totalMissingGD;
    rChildHess += totalMissingHess;
    tempGD = parentGD - rChildGD;
    tempHess = parentHess - rChildHess;
    needUpdate = NeedUpdate(rChildHess, tempHess);
    if(needUpdate == true){
    	ECHECKER(tempHess > 0);
    	ECHECKER(parentHess > 0);
    	double tempGain = (tempGD * tempGD)/(tempHess + lambda) +
			  	   	    (rChildGD * rChildGD)/(rChildHess + lambda) -
			  	   	    (parentGD * parentGD)/(parentHess + lambda);

    	if(tempGain > 0 && tempGain - pGainOnEachFeaValue[gTid] > 0.1){
    		pGainOnEachFeaValue[gTid] = tempGain;
    		pDefault2Right[gTid] = true;
    	}
    }
}

/**
 * @brief: find split points
 */
template<class T>
__global__ void FindSplitInfo(const uint *pEachFeaStartPosEachNode, const T *pEachFeaLenEachNode,
							  const real *pDenseFeaValue, const real *pfGlobalBestGain, const T *pnGlobalBestGainKey,
							  const int *pPartitionId2SNPos, const int numFea,
							  const nodeStat *snNodeStat, const double *pPrefixSumGD, const real *pPrefixSumHess,
							  const bool *pDefault2Right, const uint *pnKey,
							  SplitPoint *pBestSplitPoint, nodeStat *pRChildStat, nodeStat *pLChildStat)
{
	//a thread for constructing a split point
	int pid = threadIdx.x;//position in the dense array of nodes
	int snPos = pPartitionId2SNPos[pid];//snId to buffer id (i.e. hash value)
	ECHECKER(snPos);
	pBestSplitPoint[snPos].m_fGain = pfGlobalBestGain[pid];//change the gain back to positive
	printf("gain of pid=%d, snPos=%d is %f\n", pid, snPos, pBestSplitPoint[snPos].m_fGain);
	if(pfGlobalBestGain[pid] <= DeviceSplitter::rt_eps)//no gain
		return;

	T key = pnGlobalBestGainKey[pid];//position in the dense array
	//find best feature id
	uint bestFeaId = numFea;
	RangeBinarySearch(key, pEachFeaStartPosEachNode + pid * numFea, numFea, bestFeaId);
	CONCHECKER(bestFeaId < numFea);


	pBestSplitPoint[snPos].m_nFeatureId = bestFeaId;
	pBestSplitPoint[snPos].m_fSplitValue = 0.5f * (pDenseFeaValue[key] + pDenseFeaValue[key - 1]);
	pBestSplitPoint[snPos].m_bDefault2Right = false;

	//child node stat
	int idxPreSum = key - 1;//follow xgboost using exclusive
	if(pDefault2Right[key] == false){
		pLChildStat[snPos].sum_gd = snNodeStat[snPos].sum_gd - pPrefixSumGD[idxPreSum];
		pLChildStat[snPos].sum_hess = snNodeStat[snPos].sum_hess - pPrefixSumHess[idxPreSum];
		pRChildStat[snPos].sum_gd = pPrefixSumGD[idxPreSum];
		pRChildStat[snPos].sum_hess = pPrefixSumHess[idxPreSum];
		if(bestFeaId == 3)
			printf("default to left: pPrefixSumHess[idxPreSum=%u]=%f, tid=%d\n",
					idxPreSum, pPrefixSumHess[idxPreSum], pid);
	}
	else{
		pBestSplitPoint[snPos].m_bDefault2Right = true;

		real parentGD = snNodeStat[snPos].sum_gd;
		real parentHess = snNodeStat[snPos].sum_hess;

		uint segId = pnKey[key];
		uint segStartPos = pEachFeaStartPosEachNode[segId];
		T segLen = pEachFeaLenEachNode[segId];
		uint lastFvaluePos = segStartPos + segLen - 1;
		real totalMissingGD = parentGD - pPrefixSumGD[lastFvaluePos];
		real totalMissingHess = parentHess - pPrefixSumHess[lastFvaluePos];

		double rChildGD = totalMissingGD + pPrefixSumGD[idxPreSum];
		real rChildHess = totalMissingHess + pPrefixSumHess[idxPreSum];
		if(bestFeaId == 3)
			printf("default to right: rChildHess=%f, totalMissingHess=%f, pPrefixSumHess[idxPreSum=%u]=%f, lastFvaluePos=%u, segStartPos=%u, segLen=%u, tid=%d\n",
					rChildHess, totalMissingHess, idxPreSum, pPrefixSumHess[idxPreSum], lastFvaluePos, segStartPos, segLen, pid);
		ECHECKER(rChildHess);
		real lChildGD = parentGD - rChildGD;
		real lChildHess = parentHess - rChildHess;
		ECHECKER(lChildHess);

		pRChildStat[snPos].sum_gd = rChildGD;
		pRChildStat[snPos].sum_hess = rChildHess;
		pLChildStat[snPos].sum_gd = lChildGD;
		pLChildStat[snPos].sum_hess = lChildHess;
	}
	ECHECKER(pLChildStat[snPos].sum_hess);
	ECHECKER(pRChildStat[snPos].sum_hess);
	printf("split: f=%d, value=%f, gain=%f, gd=%f v.s. %f, hess=%f v.s. %f, buffId=%d, key=%d, pid=%d, df2Left=%d\n", bestFeaId, pBestSplitPoint[snPos].m_fSplitValue,
			pBestSplitPoint[snPos].m_fGain, pLChildStat[snPos].sum_gd, pRChildStat[snPos].sum_gd, pLChildStat[snPos].sum_hess,
			pRChildStat[snPos].sum_hess, snPos, key, pid, pDefault2Right[key]);
}

#endif /* DEVICESPLITTERKERNEL_H_ */
