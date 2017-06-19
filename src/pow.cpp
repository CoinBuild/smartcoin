// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"
#include "chain.h"
#include "chainparams.h"
#include "primitives/block.h"
#include "bignum.h"
#include "uint256.h"
#include "arith_uint256.h"
#include "util.h"
#include "math.h"

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    int DiffMode = 1;

    if (Params().NetworkIDString() == "test") {
		DiffMode = 4;
    }
    else {
        if (pindexLast->nHeight+1 >= forkBlock1 && pindexLast->nHeight+1 < forkBlock2) { DiffMode = 2; }
		else if (pindexLast->nHeight+1 > forkBlock2 && pindexLast->nHeight+1 < forkBlock4) { DiffMode = 3; }
		else if (pindexLast->nHeight+1 >= forkBlock4) { DiffMode = 4; }
    }

    switch (DiffMode) {
        case 1: return GetNextWorkRequired_V1(pindexLast, pblock, params);
        case 2: return KimotoGravityWell(pindexLast, pblock, params);
        case 3: return DigiShield(pindexLast, pblock, params);
		case 4: return DarkGravityWave(pindexLast, pblock, params);
        default: return DarkGravityWave(pindexLast, pblock, params);
    }
}

unsigned int GetNextWorkRequired_V1(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    static const int64_t nInterval = params.DifficultyAdjustmentInterval();
	static const int64_t nReTargetHistoryFact = 4; // look at 4 times the retarget interval
	const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
	unsigned int nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per interval
    if ((pindexLast->nHeight+1) % nInterval != 0)
    {
        // Special difficulty rule for testnet:
        if (Params().NetworkIDString() == "test")
        {
            // If the new block's timestamp is more than 2 * 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->nTime > pindexLast->nTime + params.nPowTargetSpacing * 2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Litecoin: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = nInterval-1;
    if ((pindexLast->nHeight+1) != nInterval)
        blockstogoback = nInterval;
    if (pindexLast->nHeight > COINFIX1_BLOCK) {
        blockstogoback = nReTargetHistoryFact * nInterval;
    }

    // Go back by what we want to be nReTargetHistoryFact*nInterval blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = 0;
    if (pindexLast->nHeight > COINFIX1_BLOCK)
        // obtain average actual timespan
        nActualTimespan = (pindexLast->GetBlockTime() - pindexFirst->GetBlockTime())/nReTargetHistoryFact;
    else
        nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
	
	if (fDebug) {
		LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);
	}
	
	int64_t nTargetTimespan = params.nPowTargetTimespan;

    if (nActualTimespan < nTargetTimespan/4)
        nActualTimespan = nTargetTimespan/4;
    if (nActualTimespan > nTargetTimespan*4)
        nActualTimespan = nTargetTimespan*4;

    // Retarget
    arith_uint256 bnOld;
	arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
	bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnProofOfWorkLimit)
        bnNew = bnProofOfWorkLimit;

    // Debug print
	if (fDebug) {
		LogPrintf("GetNextWorkRequired RETARGET\n");
		LogPrintf("nTargetTimespan = %d nActualTimespan = %d\n", nTargetTimespan, nActualTimespan);
		LogPrintf("Before: %08x %s\n", pindexLast->nBits, bnOld.ToString());
		LogPrintf("After: %08x %s\n", bnNew.GetCompact(), bnNew.ToString());
	}

    return bnNew.GetCompact();
}

unsigned int KimotoGravityWell(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
	unsigned int TimeDaySeconds = 60 * 60 * 24;
    uint64_t TargetBlocksSpacingSeconds = params.nPowTargetSpacing;
    int64_t PastSecondsMin = TimeDaySeconds * 0.0185;
    int64_t PastSecondsMax = TimeDaySeconds * 0.23125;
    uint64_t PastBlocksMin = PastSecondsMin / TargetBlocksSpacingSeconds;
    uint64_t PastBlocksMax = PastSecondsMax / TargetBlocksSpacingSeconds;

    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    const CBlockHeader *BlockCreating = pblock;
    BlockCreating = BlockCreating;
    uint64_t PastBlocksMass = 0;
    int64_t PastRateActualSeconds = 0;
    int64_t PastRateTargetSeconds = 0;
    double PastRateAdjustmentRatio = double(1);
    CBigNum PastDifficultyAverage;
    CBigNum PastDifficultyAveragePrev;
    double EventHorizonDeviation;
    double EventHorizonDeviationFast;
    double EventHorizonDeviationSlow;

    const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
        
    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64_t)BlockLastSolved->nHeight < PastBlocksMin)
        return bnProofOfWorkLimit.GetCompact();

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }

        PastBlocksMass++;

        if (i == 1) {
            PastDifficultyAverage.SetCompact(BlockReading->nBits);
        }
        else {
            PastDifficultyAverage = ((CBigNum().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev;
        }
        PastDifficultyAveragePrev = PastDifficultyAverage;

        PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
        PastRateTargetSeconds = TargetBlocksSpacingSeconds * PastBlocksMass;
        PastRateAdjustmentRatio = double(1);
        if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
        PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
        }
        EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass)/double(39.96)), -1.228));
        EventHorizonDeviationFast = EventHorizonDeviation;
        EventHorizonDeviationSlow = 1 / EventHorizonDeviation;

        if (PastBlocksMass >= PastBlocksMin) {
            if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
        }
        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }

        BlockReading = BlockReading->pprev;
    }
        
    bnNew = UintToArith256(PastDifficultyAverage.getuint256());
    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
        bnNew *= PastRateActualSeconds;
        bnNew /= PastRateTargetSeconds;
    }
    if (bnNew > bnProofOfWorkLimit) { bnNew = bnProofOfWorkLimit; }

    if (fDebug) {
		LogPrintf("Kimoto Gravity Well: PastRateAdjustmentRatio = %g\n", PastRateAdjustmentRatio);
        LogPrintf("Before: %08x %s\n", BlockLastSolved->nBits, bnOld.ToString());
        LogPrintf("After: %08x %s\n", bnNew.GetCompact(), bnNew.ToString());
	}
        
    return bnNew.GetCompact();
}

unsigned int DigiShield(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
	const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
	unsigned int nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();
	int64_t nTargetSpacing = 60 * 2;
    int blockstogoback = 0;
	
	bool fTestNet = Params().NetworkIDString() == "test";

	if ((!fTestNet && pblock->GetBlockTime() >= X11_START) || (fTestNet && pblock->GetBlockTime() >= 1405296000)) {
		nTargetSpacing = 60 * 2; // switch to 2 minute blocks after block 300,000
	}
	else if (!fTestNet && pindexLast->nHeight+1 >= forkBlock2 && pblock->GetBlockTime() < X11_START) {
		nTargetSpacing = 30; // 30 second blocks between block 200,000 and 300,000
	}

    // Retarget every block
    int64_t retargetTimespan = nTargetSpacing; // 2 minutes after block 300,000
    int64_t retargetSpacing = nTargetSpacing;
    int64_t retargetInterval = retargetTimespan / retargetSpacing;
    
    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per interval
    if ((pindexLast->nHeight+1) % retargetInterval != 0){
        // Special difficulty rule for testnet
		if (fTestNet){
			// If the new block's timestamp is more than 2 * 30 seconds
            //  then allow mining of a min-difficulty block.
			if (pblock->nTime > pindexLast->nTime + retargetSpacing*2)
				return nProofOfWorkLimit;
        }
        else {
			// Return the last non-special-min-difficulty-rules-block
			const CBlockIndex* pindex = pindexLast;
			while (pindex->pprev && pindex->nHeight % retargetInterval != 0 && pindex->nBits == nProofOfWorkLimit) 
                pindex = pindex->pprev;
            return pindex->nBits;
		}
      return pindexLast->nBits;
    }
    
    // DigiByte: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    blockstogoback = retargetInterval-1;
    if ((pindexLast->nHeight+1) != retargetInterval) blockstogoback = retargetInterval;
    
    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
	if (fDebug) {
		LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);
	}

    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    
    if (nActualTimespan < (retargetTimespan - (retargetTimespan/4)) ) nActualTimespan = (retargetTimespan - (retargetTimespan/4));
    if (nActualTimespan > (retargetTimespan + (retargetTimespan/2)) ) nActualTimespan = (retargetTimespan + (retargetTimespan/2));

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= retargetTimespan;
    
	if (fDebug) {
		LogPrintf("GetNextWorkRequired: retargetTimespan = %d nActualTimespan = %d\n", retargetTimespan, nActualTimespan);
		LogPrintf("Before: %08x %s\n", pindexLast->nBits, arith_uint256().SetCompact(pindexLast->nBits).ToString());
		LogPrintf("After: %08x %s\n", bnNew.GetCompact(), bnNew.ToString());
	}

    if (bnNew > bnProofOfWorkLimit)
        bnNew = bnProofOfWorkLimit;

    return bnNew.GetCompact();
}

unsigned int DarkGravityWave(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params) {
	/* current difficulty formula, darkcoin - DarkGravity v3, written by Evan Duffield - evan@darkcoin.io */
    const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
	const CBlockIndex *BlockLastSolved = pindexLast;
	const CBlockIndex *BlockReading = pindexLast;
	const CBlockHeader *BlockCreating = pblock;
	BlockCreating = BlockCreating;
	int64_t nActualTimespan = 0;
	int64_t LastBlockTime = 0;
	int64_t PastBlocksMin = 24;
	int64_t PastBlocksMax = 24;
	int64_t CountBlocks = 0;
    CBigNum PastDifficultyAverage;
    CBigNum PastDifficultyAveragePrev;
    int64_t targetSpacing = params.nPowTargetSpacing;

    if (pblock->GetBlockTime() > 1406160000) {
        targetSpacing = 60 * 2;
    }

	if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || BlockLastSolved->nHeight < PastBlocksMin) {
		return bnProofOfWorkLimit.GetCompact();
	}

	for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
		if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
		CountBlocks++;

		if(CountBlocks <= PastBlocksMin) {
			if (CountBlocks == 1) { PastDifficultyAverage.SetCompact(BlockReading->nBits); }
            else { PastDifficultyAverage = ((PastDifficultyAveragePrev * CountBlocks) + (CBigNum().SetCompact(BlockReading->nBits))) / (CountBlocks + 1); }
			PastDifficultyAveragePrev = PastDifficultyAverage;
		}

		if(LastBlockTime > 0){
			int64_t Diff = (LastBlockTime - BlockReading->GetBlockTime());
			nActualTimespan += Diff;
		}
		LastBlockTime = BlockReading->GetBlockTime();

		if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
		BlockReading = BlockReading->pprev;
	}

    CBigNum bnNew(PastDifficultyAverage);

    int64_t _nTargetTimespan = CountBlocks*targetSpacing;

	if (nActualTimespan < _nTargetTimespan/3)
		nActualTimespan = _nTargetTimespan/3;
	if (nActualTimespan > _nTargetTimespan*3)
		nActualTimespan = _nTargetTimespan*3;

	// Retarget
	bnNew *= nActualTimespan;
	bnNew /= _nTargetTimespan;

    if (UintToArith256(bnNew.getuint256()) > bnProofOfWorkLimit) {
        bnNew = CBigNum(params.powLimit);
	}

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return error("CheckProofOfWork(): nBits below minimum work");

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget) {
        return error("CheckProofOfWork(): hash doesn't match nBits");
    }

    return true;
}

arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;
    bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return 0;
    // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
    // as it's too large for a arith_uint256. However, as 2**256 is at least as large
    // as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
    // or ~bnTarget / (nTarget+1) + 1.
    return (~bnTarget / (bnTarget + 1)) + 1;
}

int64_t GetBlockProofEquivalentTime(const CBlockIndex& to, const CBlockIndex& from, const CBlockIndex& tip, const Consensus::Params& params)
{
    arith_uint256 r;
    int sign = 1;
    if (to.nChainWork > from.nChainWork) {
        r = to.nChainWork - from.nChainWork;
    } else {
        r = from.nChainWork - to.nChainWork;
        sign = -1;
    }
    r = r * arith_uint256(params.nPowTargetSpacing) / GetBlockProof(tip);
    if (r.bits() > 63) {
        return sign * std::numeric_limits<int64_t>::max();
    }
    return sign * r.GetLow64();
}
