/*
 * This file is part of the Eccoin project
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2016 The Bitcoin Core developers
 * Copyright (c) 2014-2018 The Eccoin developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "main.h"

#include "addrman.h"
#include "args.h"
#include "arith_uint256.h"
#include "chain/chain.h"
#include "chain/checkpoints.h"
#include "checkqueue.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "crypto/hash.h"
#include "init.h"
#include "kernel.h"
#include "merkleblock.h"
#include "messages.h"
#include "net.h"
#include "networks/netman.h"
#include "networks/networktemplate.h"
#include "policy/policy.h"
#include "pow.h"
#include "processblock.h"
#include "processheader.h"
#include "processtx.h"
#include "random.h"
#include "script/script.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "services/mempool.h"
#include "tinyformat.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util/util.h"
#include "util/utilmoneystr.h"
#include "util/utilstrencodings.h"
#include "validationinterface.h"
#include "versionbits.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/foreach.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/thread.hpp>
#include <random>
#include <random>
#include <sstream>

/**
 * Global state
 */

CCriticalSection cs_main;


int64_t nTimeBestReceived = 0;


CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
bool fImporting = false;
bool fReindex = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
unsigned int nBytesPerSigOp = DEFAULT_BYTES_PER_SIGOP;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
bool fAlerts = DEFAULT_ALERTS;
bool fEnableReplacement = DEFAULT_ENABLE_REPLACEMENT;

/** Fees smaller than this (in satoshi) are considered zero fee (for relaying, mining and transaction creation) */
CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);

CTxMemPool mempool(::minRelayTxFee);


std::map<uint256, COrphanTx> mapOrphanTransactions GUARDED_BY(cs_main);
;
std::map<uint256, std::set<uint256> > mapOrphanTransactionsByPrev GUARDED_BY(cs_main);
;


/**
 * Returns true if there are nRequired or more blocks of minVersion or above
 * in the last Consensus::Params::nMajorityWindow blocks, starting at pstart and going backwards.
 */
bool IsSuperMajority(int minVersion,
    const CBlockIndex *pstart,
    unsigned nRequired,
    const Consensus::Params &consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const std::string strMessageMagic = "ECC Signed Message:\n";


CBlockIndex *pindexBestInvalid;

/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
 * as good as our current tip or better. Entries may be failed, though, and pruning nodes may be
 * missing the data for the block.
 */
std::set<CBlockIndex *, CBlockIndexWorkComparator> setBlockIndexCandidates;

/** Number of nodes with fSyncStarted. */
int nSyncStarted = 0;

/** All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
 * Pruned nodes may have entries where B is missing data.
 */
std::multimap<CBlockIndex *, CBlockIndex *> mapBlocksUnlinked;

CCriticalSection cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;

/**
 * Every received block is assigned a unique and increasing identifier, so we
 * know which one to give priority in case of a fork.
 */
CCriticalSection cs_nBlockSequenceId;

/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
uint32_t nBlockSequenceId = 1;

/**
 * Sources of received blocks, saved to be able to send them reject
 * messages or ban them when processing happens afterwards. Protected by
 * cs_main.
 */

/** Number of preferable block download peers. */
int nPreferredDownload = 0;

/** Dirty block index entries. */
std::set<CBlockIndex *> setDirtyBlockIndex;

/** Dirty block file entries. */
std::set<int> setDirtyFileInfo;

/** Number of peers from which we're downloading blocks. */
int nPeersWithValidatedDownloads = 0;

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

int GetHeight()
{
    LOCK(cs_main);
    return pnetMan->getChainActive()->chainActive.Height();
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
    {
        return true;
    }
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
    {
        return true;
    }
    for (auto const &txin : tx.vin)
    {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
        {
            return false;
        }
    }
    return true;
}

bool CheckFinalTx(const CTransaction &tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = pnetMan->getChainActive()->chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ?
                                   pnetMan->getChainActive()->chainActive.Tip()->GetMedianTimePast() :
                                   GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

/**
 * Calculates the block height and previous block's median time past at
 * which the transaction will be considered final in the context of BIP 68.
 * Also removes from the vector of input heights any entries which did not
 * correspond to sequence locked inputs as they do not affect the calculation.
 */
static std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx,
    int flags,
    std::vector<int> *prevHeights,
    const CBlockIndex &block)
{
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2 && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68)
    {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++)
    {
        const CTxIn &txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG)
        {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG)
        {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight - 1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK)
                                                                << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) -
                                              1);
        }
        else
        {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

static bool EvaluateSequenceLocks(const CBlockIndex &block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int> *prevHeights, const CBlockIndex &block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

bool TestLockPointValidity(const LockPoints *lp)
{
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the chain
    if (lp->maxInputBlock)
    {
        // Check whether chainActive is an extension of the block at which the LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!pnetMan->getChainActive()->chainActive.Contains(lp->maxInputBlock))
        {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

bool CheckSequenceLocks(const CTransaction &tx, int flags, LockPoints *lp, bool useExistingLockPoints)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);

    CBlockIndex *tip = pnetMan->getChainActive()->chainActive.Tip();
    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate
    // height based locks because when SequenceLocks() is called within
    // ConnectBlock(), the height of the block *being*
    // evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than chainActive.Height()
    index.nHeight = tip->nHeight + 1;

    std::pair<int, int64_t> lockPair;
    if (useExistingLockPoints)
    {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    }
    else
    {
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        CCoinsViewMemPool viewMemPool(pnetMan->getChainActive()->pcoinsTip.get(), mempool);
        std::vector<int> prevheights;
        prevheights.resize(tx.vin.size());
        for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++)
        {
            const CTxIn &txin = tx.vin[txinIndex];
            Coin coin;
            if (!viewMemPool.GetCoin(txin.prevout, coin))
            {
                return error("%s: Missing input", __func__);
            }
            if (coin.nHeight == MEMPOOL_HEIGHT)
            {
                // Assume all mempool transaction confirm in the next block
                prevheights[txinIndex] = tip->nHeight + 1;
            }
            else
            {
                prevheights[txinIndex] = coin.nHeight;
            }
        }
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp)
        {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of
            // all the blocks which have sequence locked prevouts.
            // This hash needs to still be on the chain
            // for these LockPoint calculations to be valid
            // Note: It is impossible to correctly calculate a maxInputBlock
            // if any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height
            // is 0, which is equivalent to no sequence lock. Since we assume
            // input height of tip+1 for mempool txs and test the resulting
            // lockPair from CalculateSequenceLocks against tip+1.  We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            int maxInputHeight = 0;
            for (auto height : prevheights)
            {
                // Can ignore mempool inputs since we'll fail if they had non-zero locks
                if (height != tip->nHeight + 1)
                {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    return EvaluateSequenceLocks(index, lockPair);
}


unsigned int GetLegacySigOpCount(const CTransaction &tx)
{
    unsigned int nSigOps = 0;
    for (auto const &txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (auto const &txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction &tx, const CCoinsViewCache &inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut &prevout = inputs.AccessCoin(tx.vin[i].prevout).out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
        {
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
        }
    }
    return nSigOps;
}

void LimitMempoolSize(CTxMemPool &pool, size_t limit, unsigned long age)
{
    std::vector<COutPoint> vCoinsToUncache;
    int expired = pool.Expire(GetTime() - age, vCoinsToUncache);
    for (const COutPoint &txin : vCoinsToUncache)
    {
        pnetMan->getChainActive()->pcoinsTip->Uncache(txin);
    }
    if (expired != 0)
        LogPrint("mempool", "Expired %i transactions from the memory pool\n", expired);

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint &removed : vNoSpendsRemaining)
    {
        pnetMan->getChainActive()->pcoinsTip->Uncache(removed);
    }
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state)
{
    return strprintf("%s%s (code %i)", state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", " + state.GetDebugMessage(), state.GetRejectCode());
}

bool AcceptToMemoryPoolWorker(CTxMemPool &pool,
    CValidationState &state,
    const CTransactionRef &ptx,
    bool fLimitFree,
    bool *pfMissingInputs,
    bool fOverrideMempoolLimit,
    bool fRejectAbsurdFee,
    std::vector<COutPoint> &vCoinsToUncache)
{
    const CTransaction &tx = *ptx;
    AssertLockHeld(cs_main);

    if (!CheckTransaction(tx, state))
    {
        return false;
    }

    // Coinbase/Coinstake is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase() || tx.IsCoinStake())
    {
        return state.DoS(100, false, REJECT_INVALID, "coinbase");
    }

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason))
    {
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);
    }

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
    {
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");
    }

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
    {
        return state.Invalid(false, REJECT_ALREADY_KNOWN, "txn-already-in-mempool");
    }

    // Check for conflicts with in-memory transactions
    {
        LOCK(pool.cs); // protect pool.mapNextTx
        for (auto const &txin : tx.vin)
        {
            auto itConflicting = pool.mapNextTx.find(txin.prevout);
            if (itConflicting != pool.mapNextTx.end())
            {
                // Disable replacement feature for good
                return state.Invalid(false, REJECT_CONFLICT, "txn-mempool-conflict");
            }
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        LockPoints lp;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pnetMan->getChainActive()->pcoinsTip.get(), pool);
            view.SetBackend(viewMemPool);

            // do all inputs exist?
            if (pfMissingInputs)
            {
                *pfMissingInputs = false;
                BOOST_FOREACH (const CTxIn txin, tx.vin)
                {
                    // At this point we begin to collect coins that are potential candidates for uncaching because as
                    // soon as we make the call below to view.HaveCoin() any missing coins will be pulled into cache.
                    // Therefore, any coin in this transaction that is not already in cache will be tracked here such
                    // that if this transaction fails to enter the memory pool, we will then uncache those coins that
                    // were not already present, unless the transaction is an orphan.
                    //
                    // We still want to keep orphantx coins in the event the orphantx is finally accepted into the
                    // mempool or shows up in a block that is mined.  Therefore if pfMissingInputs returns true then
                    // any coins in vCoinsToUncache will NOT be uncached.
                    if (!pnetMan->getChainActive()->pcoinsTip->HaveCoinInCache(txin.prevout))
                    {
                        vCoinsToUncache.push_back(txin.prevout);
                    }

                    if (!view.HaveCoin(txin.prevout))
                    {
                        // fMissingInputs and not state.IsInvalid() is used to detect this condition, don't set
                        // state.Invalid()
                        *pfMissingInputs = true;
                    }
                }
                if (*pfMissingInputs == true)
                    return false;
            }

            // Bring the best block into scope
            view.GetBestBlock();

            nValueIn = view.GetValueIn(tx);

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);

            // Only accept BIP68 sequence locked transactions that can be mined in the next
            // block; we don't want our mempool filled up with transactions that can't
            // be mined yet.
            // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
            // CoinsViewCache instead of create its own
            if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp))
            {
                return state.DoS(0, false, REJECT_NONSTANDARD, "non-BIP68-final");
            }
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view))
        {
            return state.Invalid(false, REJECT_NONSTANDARD, "bad-txns-nonstandard-inputs");
        }

        unsigned int nSigOps = GetLegacySigOpCount(tx);
        nSigOps += GetP2SHSigOpCount(tx, view);

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        // nModifiedFees includes any fee deltas from PrioritiseTransaction
        CAmount nModifiedFees = nFees;
        double nPriorityDummy = 0;
        pool.ApplyDeltas(hash, nPriorityDummy, nModifiedFees);

        CAmount inChainInputValue;
        double dPriority = view.GetPriority(tx, pnetMan->getChainActive()->chainActive.Height(), inChainInputValue);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        for (auto const &txin : tx.vin)
        {
            const Coin coin = view.AccessCoin(txin.prevout);
            if (coin.IsCoinBase())
            {
                fSpendsCoinbase = true;
                break;
            }
        }

        CTxMemPoolEntry entry(ptx, nFees, GetTime(), dPriority, pnetMan->getChainActive()->chainActive.Height(),
            pool.HasNoInputsOf(tx), inChainInputValue, fSpendsCoinbase, nSigOps, lp);
        unsigned int nSize = entry.GetTxSize();

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        if ((nSigOps > MAX_STANDARD_TX_SIGOPS) || (nBytesPerSigOp && nSigOps > nSize / nBytesPerSigOp))
        {
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-txns-too-many-sigops", false, strprintf("%d", nSigOps));
        }

        CAmount mempoolRejectFee =
            pool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFee(nSize);
        if (mempoolRejectFee > 0 && nModifiedFees < mempoolRejectFee)
        {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool min fee not met", false,
                strprintf("%d < %d", nFees, mempoolRejectFee));
        }
        else if (gArgs.GetBoolArg("-relaypriority", DEFAULT_RELAYPRIORITY) &&
                 nModifiedFees < ::minRelayTxFee.GetFee(nSize) &&
                 !AllowFree(entry.GetPriority(pnetMan->getChainActive()->chainActive.Height() + 1)))
        {
            // Require that free transactions have sufficient priority to be mined in the next block.
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
        }

        // Continuously rate-limit free (really, very-low-fee) transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        static const double maxFeeCutoff =
            boost::lexical_cast<double>(gArgs.GetArg("-maxlimitertxfee", DEFAULT_MAXLIMITERTXFEE));
        // starting value for feeCutoff in satoshi per byte
        static const double initFeeCutoff =
            boost::lexical_cast<double>(gArgs.GetArg("-minlimitertxfee", DEFAULT_MINLIMITERTXFEE));
        static const int nLimitFreeRelay = gArgs.GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY);

        // get current memory pool size
        uint64_t poolBytes = pool.GetTotalTxSize();

        // Calculate feeCutoff in satoshis per byte:
        //   When the feeCutoff is larger than the satoshiPerByte of the
        //   current transaction then spam blocking will be in effect. However
        //   Some free transactions will still get through based on -limitfreerelay
        static double feeCutoff = initFeeCutoff;
        static double nFreeLimit = nLimitFreeRelay;
        static int64_t nLastTime = GetTime();

        int64_t nNow = GetTime();

        // When the mempool starts falling use an exponentially decaying ~24 hour window:
        // nFreeLimit = nFreeLimit + ((double)(DEFAULT_LIMIT_FREE_RELAY - nFreeLimit) / pow(1.0 - 1.0/86400,
        // (double)(nNow - nLastTime)));
        nFreeLimit /= std::pow(1.0 - 1.0 / 86400, (double)(nNow - nLastTime));

        // When the mempool starts falling use an exponentially decaying ~24 hour window:
        feeCutoff *= std::pow(1.0 - 1.0 / 86400, (double)(nNow - nLastTime));

        uint64_t nLargestBlockSeen = MAX_BLOCK_SIZE;
        if (poolBytes < nLargestBlockSeen)
        {
            feeCutoff = std::max(feeCutoff, initFeeCutoff);
            nFreeLimit = std::min(nFreeLimit, (double)nLimitFreeRelay);
        }
        else if (poolBytes < (nLargestBlockSeen * MAX_BLOCK_SIZE_MULTIPLIER))
        {
            // Gradually choke off what is considered a free transaction
            feeCutoff =
                std::max(feeCutoff, initFeeCutoff + ((maxFeeCutoff - initFeeCutoff) * (poolBytes - nLargestBlockSeen) /
                                                        (nLargestBlockSeen * (MAX_BLOCK_SIZE_MULTIPLIER - 1))));

            // Gradually choke off the nFreeLimit as well but leave at least DEFAULT_MIN_LIMITFREERELAY
            // So that some free transactions can still get through
            nFreeLimit = std::min(
                nFreeLimit, ((double)nLimitFreeRelay - ((double)(nLimitFreeRelay - DEFAULT_MIN_LIMITFREERELAY) *
                                                           (double)(poolBytes - nLargestBlockSeen) /
                                                           (nLargestBlockSeen * (MAX_BLOCK_SIZE_MULTIPLIER - 1)))));
            if (nFreeLimit < DEFAULT_MIN_LIMITFREERELAY)
                nFreeLimit = DEFAULT_MIN_LIMITFREERELAY;
        }
        else
        {
            feeCutoff = maxFeeCutoff;
            nFreeLimit = DEFAULT_MIN_LIMITFREERELAY;
        }
        minRelayTxFee = CFeeRate(feeCutoff * 1000);
        LogPrint("MEMPOOL",
            "MempoolBytes:%d  LimitFreeRelay:%.5g  FeeCutOff:%.4g  FeesSatoshiPerByte:%.4g  TxBytes:%d  TxFees:%d\n",
            poolBytes, nFreeLimit, ((double)::minRelayTxFee.GetFee(nSize)) / nSize, ((double)nFees) / nSize, nSize,
            nFees);
        if (fLimitFree && nModifiedFees < ::minRelayTxFee.GetFee(nSize))
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount >= gArgs.GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) * 10 * 1000)
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "rate limited free transaction");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
            dFreeCount += nSize;
        }

        if (fRejectAbsurdFee && tx.nVersion == 1 && nFees > ::minRelayTxFee.GetFee(nSize) * 10000)
        {
            LogPrintf("Absurdly-high-fee of %d for tx with version of 1 \n", nFees);
            return state.Invalid(false, REJECT_HIGHFEE, "absurdly-high-fee",
                strprintf("%d > %d", nFees, ::minRelayTxFee.GetFee(nSize) * 10000));
        }

        if (fRejectAbsurdFee && tx.nVersion == 2 && nFees > 100000000)
        {
            LogPrintf("Absurdly-high-fee of %d for tx with version of 2 \n", nFees);
            return state.Invalid(false, REJECT_HIGHFEE, "absurdly-high-fee", strprintf("%d > %d", nFees, 100000000));
        }


        // Calculate in-mempool ancestors, up to a limit.
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants = gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000;
        std::string errString;

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!CheckInputs(tx, state, view, true, STANDARD_SCRIPT_VERIFY_FLAGS, true, nullptr))
        {
            LogPrint("MEMPOOL", "CheckInputs failed for tx: %s\n", tx.GetHash().ToString().c_str());
            return false;
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.

        if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, nullptr))
        {
            return error(
                "%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s, %s",
                __func__, hash.ToString(), FormatStateMessage(state));
        }
        {
            LOCK(pool.cs);
            if (!pool._CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize,
                    nLimitDescendants, nLimitDescendantSize, errString))
            {
                return state.DoS(0, false, REJECT_NONSTANDARD, "too-long-mempool-chain", false, errString);
            }
        }

        {
            LOCK(pool.cs);
            // Store transaction in memory
            pool.addUnchecked(hash, entry, setAncestors, !pnetMan->getChainActive()->IsInitialBlockDownload());
        }

        // trim mempool and check if tx was trimmed
        if (!fOverrideMempoolLimit)
        {
            LimitMempoolSize(pool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
                gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            if (!pool.exists(hash))
            {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool full");
            }
        }
    }
    GetMainSignals().TransactionAddedToMempool(ptx);
    return true;
}

bool AcceptToMemoryPool(CTxMemPool &pool,
    CValidationState &state,
    const CTransactionRef &tx,
    bool fLimitFree,
    bool *pfMissingInputs,
    bool fOverrideMempoolLimit,
    bool fRejectAbsurdFee)
{
    std::vector<COutPoint> vCoinsToUncache;
    bool res = AcceptToMemoryPoolWorker(
        pool, state, tx, fLimitFree, pfMissingInputs, fOverrideMempoolLimit, fRejectAbsurdFee, vCoinsToUncache);
    if (pfMissingInputs && !res && !*pfMissingInputs)
    {
        for (const COutPoint &remove : vCoinsToUncache)
        {
            pnetMan->getChainActive()->pcoinsTip->Uncache(remove);
        }
    }
    return res;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(const CBlock &block, CDiskBlockPos &pos, const CMessageHeader::MessageMagic &messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, block);
    fileout << FLATDATA(messageStart) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock &block, const CDiskBlockPos &pos, const Consensus::Params &consensusParams)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());

    // Read block
    try
    {
        filein >> block;
    }
    catch (const std::exception &e)
    {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    // Check the header
    if (block.IsProofOfWork())
    {
        if (!CheckProofOfWork(block.GetHash(), block.nBits, consensusParams))
            return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());
    }
    return true;
}

bool ReadBlockFromDisk(CBlock &block, const CBlockIndex *pindex, const Consensus::Params &consensusParams)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos(), consensusParams))
    {
        return false;
    }
    if (block.GetHash() != pindex->GetBlockHash())
    {
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
            pindex->ToString(), pindex->GetBlockPos().ToString());
    }
    return true;
}

void UpdateCoins(const CTransaction &tx, CValidationState &state, CCoinsViewCache &inputs, CTxUndo &txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinBase())
    {
        txundo.vprevout.reserve(tx.vin.size());
        for (auto const &txin : tx.vin)
        {
            txundo.vprevout.emplace_back();
            inputs.SpendCoin(txin.prevout, &txundo.vprevout.back());
        }
    }
    // add outputs
    AddCoins(inputs, tx, nHeight);
}

void UpdateCoins(const CTransaction &tx, CValidationState &state, CCoinsViewCache &inputs, int nHeight)
{
    CTxUndo txundo;
    UpdateCoins(tx, state, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()()
{
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(
            scriptSig, scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, cacheStore), &error))
    {
        return false;
    }
    return true;
}

int GetSpendHeight(const CCoinsViewCache &inputs)
{
    LOCK(cs_main);
    CBlockIndex *pindexPrev = pnetMan->getChainActive()->mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}

namespace Consensus
{
bool CheckTxInputs(const CTransaction &tx, CValidationState &state, const CCoinsViewCache &inputs, int64_t nSpendHeight)
{
    // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
    // for an attacker to attempt to split the network.
    if (!inputs.HaveInputs(tx))
        return state.Invalid(false, 0, "", "Inputs unavailable");

    CAmount nValueIn = 0;
    CAmount nFees = 0;
    nSpendHeight = -1;
    for (uint64_t i = 0; i < tx.vin.size(); i++)
    {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin &coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase or coinstake, check that it's matured
        if (coin.IsCoinBase() || tx.IsCoinStake())
        {
            if (nSpendHeight == -1)
                nSpendHeight = GetSpendHeight(inputs);
            if (nSpendHeight - coin.nHeight < COINBASE_MATURITY &&
                pnetMan->getChainActive()->chainActive.Tip()->nHeight > 1600000)
                return state.Invalid(false, REJECT_INVALID, "bad-txns-premature-spend-of-coinbase",
                    strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check for negative or overflow input values
        nValueIn += coin.out.nValue;
        if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");
    }

    if (!tx.IsCoinStake())
    {
        if (nValueIn < tx.GetValueOut())
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
                strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())));
        // Tally transaction fees
        CAmount nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    }
    else
    {
        // ppcoin: coin stake tx earns reward instead of paying fee
        uint64_t nCoinAge;
        if (!tx.GetCoinAge(nCoinAge))
        {
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-cant-get-coin-age", false,
                strprintf("ConnectInputs() : %s unable to get coin age for coinstake",
                                 tx.GetHash().ToString().substr(0, 10).c_str()));
        }

        int64_t nStakeReward = tx.GetValueOut() - nValueIn;
        if (nStakeReward >
            GetProofOfStakeReward(tx.GetCoinAge(nCoinAge, true), nSpendHeight) + DEFAULT_TRANSACTION_MINFEE)
        {
            if (fDebug)
            {
                LogPrintf("nStakeReward = %d , CoinAge = %d \n", nStakeReward, nCoinAge);
            }
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-stake-reward-too-high", false,
                strprintf("ConnectInputs() : %s stake reward exceeded", tx.GetHash().ToString().substr(0, 10).c_str()));
        }
    }
    return true;
}
} // namespace Consensus

bool CheckInputs(const CTransaction &tx,
    CValidationState &state,
    const CCoinsViewCache &inputs,
    bool fScriptChecks,
    unsigned int flags,
    bool cacheStore,
    std::vector<CScriptCheck> *pvChecks)
{
    if (!tx.IsCoinBase())
    {
        if (!Consensus::CheckTxInputs(tx, state, inputs, GetSpendHeight(inputs)))
            return false;

        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks)
        {
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                const COutPoint &prevout = tx.vin[i].prevout;
                const Coin &coin = inputs.AccessCoin(prevout);
                assert(!coin.IsSpent());

                // We very carefully only pass in things to CScriptCheck which
                // are clearly committed. This provides
                // a sanity check that our caching is not introducing consensus
                // failures through additional data in, eg, the coins being
                // spent being checked as a part of CScriptCheck.
                const CScript &scriptPubKey = coin.out.scriptPubKey;
                const CAmount amount = coin.out.nValue;

                // Verify signature
                CScriptCheck check(scriptPubKey, amount, tx, i, flags, cacheStore);
                if (pvChecks)
                {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                }
                else if (!check())
                {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS)
                    {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check2(
                            scriptPubKey, amount, tx, i, flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore);
                        if (check2())
                        {
                            return state.Invalid(
                                false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)",
                                                               ScriptErrorString(check.GetScriptError())));
                        }
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after a soft-fork
                    // super-majority vote has passed.
                    return state.DoS(100, false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)",
                                                                     ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

/** Abort with a message */
bool AbortNode(const std::string &strMessage, const std::string &userMessage)
{
    strMiscWarning = strMessage;
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see debug.log for details") : userMessage, "",
        CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AbortNode(CValidationState &state, const std::string &strMessage, const std::string &userMessage)
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld)
    {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld)
    {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

// Protected by cs_main
VersionBitsCache versionbitscache;

int32_t ComputeBlockVersion(const CBlockIndex *pindexPrev, const Consensus::Params &params)
{
    LOCK(cs_main);
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++)
    {
        ThresholdState state = VersionBitsState(pindexPrev, params, (Consensus::DeploymentPos)i, versionbitscache);
        if (state == THRESHOLD_LOCKED_IN || state == THRESHOLD_STARTED)
        {
            nVersion |= VersionBitsMask(params, (Consensus::DeploymentPos)i);
        }
    }

    return nVersion;
}

// Protected by cs_main
ThresholdConditionCache warningcache[VERSIONBITS_NUM_BITS];

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed depending on the mode we're called with
 * if they're too large, if it's been a while since the last write,
 * or always and in all cases if we're in prune mode and are deleting files.
 */
bool FlushStateToDisk(CValidationState &state, FlushStateMode mode)
{
    LOCK2(cs_main, cs_LastBlockFile);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    try
    {
        int64_t nNow = GetTimeMicros();
        // Avoid writing/flushing immediately after startup.
        if (nLastWrite == 0)
        {
            nLastWrite = nNow;
        }
        if (nLastFlush == 0)
        {
            nLastFlush = nNow;
        }
        if (nLastSetChain == 0)
        {
            nLastSetChain = nNow;
        }
        size_t cacheSize = pnetMan->getChainActive()->pcoinsTip->DynamicMemoryUsage();
        // The cache is large and close to the limit, but we have time now (not in the middle of a block processing).
        bool fCacheLarge = mode == FLUSH_STATE_PERIODIC && cacheSize * (10.0 / 9) > nCoinCacheUsage;
        // The cache is over the limit, we have to write now.
        bool fCacheCritical = mode == FLUSH_STATE_IF_NEEDED && cacheSize > nCoinCacheUsage;
        // It's been a while since we wrote the block index to disk. Do this frequently, so we don't need to redownload
        // after a crash.
        bool fPeriodicWrite =
            mode == FLUSH_STATE_PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
        // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
        bool fPeriodicFlush =
            mode == FLUSH_STATE_PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
        // Combine all conditions that result in a full cache flush.
        bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush;
        // Write blocks and block index to disk.
        if (fDoFullFlush || fPeriodicWrite)
        {
            // Depend on nMinDiskSpace to ensure we can write block index
            if (!CheckDiskSpace(0))
                return state.Error("out of disk space");
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block and undo files).
            {
                std::vector<std::pair<int, const CBlockFileInfo *> > vFiles;
                vFiles.reserve(setDirtyFileInfo.size());
                for (std::set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end();)
                {
                    vFiles.push_back(std::make_pair(*it, &vinfoBlockFile[*it]));
                    setDirtyFileInfo.erase(it++);
                }
                std::vector<const CBlockIndex *> vBlocks;
                vBlocks.reserve(setDirtyBlockIndex.size());
                for (std::set<CBlockIndex *>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end();)
                {
                    vBlocks.push_back(*it);
                    setDirtyBlockIndex.erase(it++);
                }
                if (!pnetMan->getChainActive()->pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks))
                {
                    return AbortNode(state, "Files to write to block index database");
                }
            }
            nLastWrite = nNow;
        }
        // Flush best chain related state. This can only be done if the blocks / block index write was also done.
        if (fDoFullFlush)
        {
            // Typical CCoins structures on disk are around 128 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(128 * 2 * 2 * pnetMan->getChainActive()->pcoinsTip->GetCacheSize()))
                return state.Error("out of disk space");
            // Flush the chainstate (which may refer to block index entries).
            if (!pnetMan->getChainActive()->pcoinsTip->Flush())
                return AbortNode(state, "Failed to write to coin database");
            nLastFlush = nNow;
        }
        if (fDoFullFlush || ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) &&
                                nNow > nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000))
        {
            // Update best block in wallet (so we can detect restored wallets).
            GetMainSignals().SetBestChain(pnetMan->getChainActive()->chainActive.GetLocator());
            nLastSetChain = nNow;
        }
    }
    catch (const std::runtime_error &e)
    {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk()
{
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
void PruneBlockIndexCandidates()
{
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex *, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() &&
           setBlockIndexCandidates.value_comp()(*it, pnetMan->getChainActive()->chainActive.Tip()))
    {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}


bool InvalidateBlock(CValidationState &state, const Consensus::Params &consensusParams, CBlockIndex *pindex)
{
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    while (pnetMan->getChainActive()->chainActive.Contains(pindex))
    {
        CBlockIndex *pindexWalk = pnetMan->getChainActive()->chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state, consensusParams))
        {
            mempool.removeForReorg(pnetMan->getChainActive()->pcoinsTip.get(),
                pnetMan->getChainActive()->chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
            return false;
        }
    }

    LimitMempoolSize(mempool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
        gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add it again.
    BlockMap::iterator it = pnetMan->getChainActive()->mapBlockIndex.begin();
    while (it != pnetMan->getChainActive()->mapBlockIndex.end())
    {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx &&
            !setBlockIndexCandidates.value_comp()(it->second, pnetMan->getChainActive()->chainActive.Tip()))
        {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    mempool.removeForReorg(pnetMan->getChainActive()->pcoinsTip.get(),
        pnetMan->getChainActive()->chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    return true;
}

bool ReconsiderBlock(CValidationState &state, CBlockIndex *pindex)
{
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block
    if (!pindex->IsValid())
    {
        pindex->nStatus &= ~BLOCK_FAILED_MASK;
        setDirtyBlockIndex.insert(pindex);
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && pindex->nChainTx &&
            setBlockIndexCandidates.value_comp()(pnetMan->getChainActive()->chainActive.Tip(), pindex))
        {
            setBlockIndexCandidates.insert(pindex);
        }
        if (pindex == pindexBestInvalid)
        {
            // Reset invalid block marker if it was pointing to one of those.
            pindexBestInvalid = NULL;
        }
    }

    // Remove the invalidity flag from all descendants.
    BlockMap::iterator it = pnetMan->getChainActive()->mapBlockIndex.begin();
    while (it != pnetMan->getChainActive()->mapBlockIndex.end())
    {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex)
        {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx &&
                setBlockIndexCandidates.value_comp()(pnetMan->getChainActive()->chainActive.Tip(), it->second))
            {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid)
            {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL)
    {
        if (pindex->nStatus & BLOCK_FAILED_MASK)
        {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(const CBlock &block,
    CValidationState &state,
    CBlockIndex *pindexNew,
    const CDiskBlockPos &pos)
{
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx)
    {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex *> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty())
        {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (pnetMan->getChainActive()->chainActive.Tip() == NULL ||
                !setBlockIndexCandidates.value_comp()(pindex, pnetMan->getChainActive()->chainActive.Tip()))
            {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex *, CBlockIndex *>::iterator,
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator>
                range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second)
            {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    }
    else
    {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE))
        {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

bool FindBlockPos(CValidationState &state,
    CDiskBlockPos &pos,
    unsigned int nAddSize,
    unsigned int nHeight,
    uint64_t nTime,
    bool fKnown)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile)
    {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown)
    {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE)
        {
            nFile++;
            if (vinfoBlockFile.size() <= nFile)
            {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if ((int)nFile != nLastBlockFile)
    {
        if (!fKnown)
        {
            LogPrintf("Leaving block file %i: %s\n", nLastBlockFile, vinfoBlockFile[nLastBlockFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown)
    {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks)
        {
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos))
            {
                FILE *file = OpenBlockFile(pos);
                if (file)
                {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE,
                        pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}


bool CheckBlock(const CBlock &block, CValidationState &state, bool fCheckPOW, bool fCheckMerkleRoot)
{
    // These are checks that are independent of context.

    if (block.fChecked)
    {
        return true;
    }

    if (block.IsProofOfWork() && fCheckPOW &&
        !CheckProofOfWork(block.GetHash(), block.nBits, pnetMan->getActivePaymentNetwork()->GetConsensus()))
    {
        return state.DoS(50, error("CheckBlockHeader(): proof of work failed"), REJECT_INVALID, "high-hash");
    }

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, fCheckPOW))
    {
        return false;
    }

    // Check the merkle root.
    if (fCheckMerkleRoot)
    {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
        {
            return state.DoS(
                100, error("CheckBlock(): hashMerkleRoot mismatch"), REJECT_INVALID, "bad-txnmrklroot", true);
        }

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
        {
            return state.DoS(
                100, error("CheckBlock(): duplicate transaction"), REJECT_INVALID, "bad-txns-duplicate", true);
        }
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // Size limits
    if (block.vtx.empty() || block.vtx.size() > MAX_BLOCK_SIZE ||
        ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
    {
        return state.DoS(100, error("CheckBlock(): size limits failed"), REJECT_INVALID, "bad-blk-length");
    }

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase())
    {
        return state.DoS(100, error("CheckBlock(): first tx is not coinbase"), REJECT_INVALID, "bad-cb-missing");
    }

    for (unsigned int i = 1; i < block.vtx.size(); i++)
    {
        if (block.vtx[i]->IsCoinBase())
        {
            return state.DoS(100, error("CheckBlock(): more than one coinbase"), REJECT_INVALID, "bad-cb-multiple");
        }
    }

    // PoS: only the second transaction can be the optional coinstake
    for (unsigned int i = 2; i < block.vtx.size(); i++)
    {
        if (block.vtx[i]->IsCoinStake())
        {
            return state.DoS(100, error("CheckBlock() : coinstake in wrong position"));
        }
    }

    // PoS: coinbase output should be empty if proof-of-stake block
    if (block.IsProofOfStake() && (block.vtx[0]->vout.size() != 1 || !block.vtx[0]->vout[0].IsEmpty()))
    {
        return state.DoS(0, error("CheckBlock() : coinbase output not empty for proof-of-stake block"));
    }

    // Check transactions
    for (auto const &tx : block.vtx)
    {
        if (!CheckTransaction(*tx, state))
        {
            return state.Invalid(false, state.GetRejectCode(), state.GetRejectReason(),
                strprintf("Transaction check failed (txid %s) %s", tx->GetId().ToString(), state.GetDebugMessage()));
        }
        if (tx->nVersion == 2)
        {
            CServiceTransaction stx;
            if (!g_stxmempool->lookup(tx->serviceReferenceHash, stx))
            {
                LogPrintf("tx with hash %s pays for service transaction with hash %s but none can be found \n",
                    tx->GetHash().GetHex().c_str(), tx->serviceReferenceHash.GetHex().c_str());
                // we should request this stx
                LogPrintf("Requesting stx %s\n", tx->serviceReferenceHash.GetHex().c_str());
                {
                    CInv inv(MSG_STX, tx->serviceReferenceHash);
                    g_connman->ForEachNode([&inv](CNode *pnode) { pnode->AskFor(inv); });
                }
                // this will be reprocessed when we get the stx. if the stx is invalid but they paid for it, oh well
            }
            else
            {
                if (!CheckServiceTransaction(stx, *tx, state))
                {
                    // we dont want the block to fail on an ans error if everything was valid coin wise
                    LogPrintf("CheckBlock(): CheckServiceTransaction of %s failed with %s. This is a non fatal error\n",
                        tx->GetHash().ToString(), FormatStateMessage(state));
                }
                else
                {
                    ProcessServiceCommand(stx, *tx, state, &block);
                }
            }
        }
        // PoS: check transaction timestamp
        if (block.GetBlockTime() < (int64_t)tx->nTime)
        {
            return state.DoS(50, error("CheckBlock() : block timestamp earlier than transaction timestamp"));
        }
    }

    unsigned int nSigOps = 0;
    for (auto const &tx : block.vtx)
    {
        nSigOps += GetLegacySigOpCount(*tx);
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
    {
        return state.DoS(100, error("CheckBlock(): out-of-bounds SigOpCount"), REJECT_INVALID, "bad-blk-sigops");
    }

    // PoS: check block signature
    if (!block.CheckBlockSignature())
    {
        return state.DoS(100, error("CheckBlock() : bad block signature"), REJECT_INVALID, "bad-block-sig");
    }

    if (fCheckPOW && fCheckMerkleRoot)
    {
        block.fChecked = true;
    }

    return true;
}

bool CheckIndexAgainstCheckpoint(const CBlockIndex *pindexPrev,
    CValidationState &state,
    const CNetworkTemplate &chainparams,
    const uint256 &hash)
{
    if (*pindexPrev->phashBlock == chainparams.GetConsensus().hashGenesisBlock)
        return true;

    int nHeight = pindexPrev->nHeight + 1;
    // Don't accept any forks from the main chain prior to last checkpoint
    CBlockIndex *pcheckpoint = Checkpoints::GetLastCheckpoint(chainparams.Checkpoints());
    if (pcheckpoint && nHeight < pcheckpoint->nHeight)
        return state.DoS(100, error("%s: forked chain older than last checkpoint (height %d)", __func__, nHeight));

    return true;
}

bool ContextualCheckBlock(const CBlock &block, CValidationState &state, CBlockIndex *const pindexPrev)
{
    const int nHeight = pindexPrev == NULL ? 0 : pindexPrev->nHeight + 1;
    const Consensus::Params &consensusParams = pnetMan->getActivePaymentNetwork()->GetConsensus();

    // Start enforcing BIP113 (Median Time Past) using versionbits logic.
    int nLockTimeFlags = 0;
    nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;

    int64_t nLockTimeCutoff =
        (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST) ? pindexPrev->GetMedianTimePast() : block.GetBlockTime();

    // Check that all transactions are finalized
    for (auto const &tx : block.vtx)
    {
        if (!IsFinalTx(*tx, nHeight, nLockTimeCutoff))
        {
            return state.DoS(
                10, error("%s: contains a non-final transaction", __func__), REJECT_INVALID, "bad-txns-nonfinal");
        }
    }

    // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
    // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
    if (block.nVersion >= 2 &&
        IsSuperMajority(2, pindexPrev, consensusParams.nMajorityEnforceBlockUpgrade, consensusParams))
    {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(), block.vtx[0]->vin[0].scriptSig.begin()))
        {
            return state.DoS(
                100, error("%s: block height mismatch in coinbase", __func__), REJECT_INVALID, "bad-cb-height");
        }
    }

    return true;
}


bool IsSuperMajority(int minVersion,
    const CBlockIndex *pstart,
    unsigned nRequired,
    const Consensus::Params &consensusParams)
{
    unsigned int nFound = 0;
    for (int i = 0; i < consensusParams.nMajorityWindow && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}


/**
 * BLOCK PRUNING CODE
 */

/* Calculate the amount of disk space the block & undo files currently use */
uint64_t CalculateCurrentUsage()
{
    uint64_t retval = 0;
    for (auto const &file : vinfoBlockFile)
    {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = fs::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

FILE *OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    fs::path path = GetBlockPosFilename(pos, prefix);
    fs::create_directories(path.parent_path());
    FILE *file = fopen(path.string().c_str(), fReadOnly ? "rb" : "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file)
    {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos)
    {
        if (fseek(file, pos.nPos, SEEK_SET))
        {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE *OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) { return OpenDiskFile(pos, "blk", fReadOnly); }
FILE *OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) { return OpenDiskFile(pos, "rev", fReadOnly); }
fs::path GetBlockPosFilename(const CDiskBlockPos &pos, const char *prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}


//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

std::string GetWarnings(const std::string &strFor)
{
    std::string strStatusBar;
    std::string strRPC;
    std::string strGUI;

    if (!CLIENT_VERSION_IS_RELEASE)
    {
        strStatusBar =
            "This is a pre-release test build - use at your own risk - do not use for mining or merchant applications";
        strGUI = _(
            "This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");
    }

    if (gArgs.GetBoolArg("-testsafemode", DEFAULT_TESTSAFEMODE))
        strStatusBar = strRPC = strGUI = "testsafemode enabled";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        strStatusBar = strGUI = strMiscWarning;
    }

    if (fLargeWorkForkFound)
    {
        strStatusBar = strRPC =
            "Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.";
        strGUI =
            _("Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
    }
    else if (fLargeWorkInvalidChainFound)
    {
        strStatusBar = strRPC = "Warning: We do not appear to fully agree with our peers! You may need to upgrade, or "
                                "other nodes may need to upgrade.";
        strGUI = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes "
                   "may need to upgrade.");
    }

    if (strFor == "gui")
        return strGUI;
    else if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings(): invalid parameter");
    return "error";
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst,
        nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst), DateTimeStrFormat("%Y-%m-%d", nTimeLast));
}

ThresholdState VersionBitsTipState(const Consensus::Params &params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsState(pnetMan->getChainActive()->chainActive.Tip(), params, pos, versionbitscache);
}

class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup()
    {
        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cmaincleanup;

// ppcoin: find last block index up to pindex
const CBlockIndex *GetLastBlockIndex(const CBlockIndex *pindex, bool fProofOfStake)
{
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;
    return pindex;
}


unsigned int GetNextTargetRequired(const CBlockIndex *pindexLast, bool fProofOfStake)
{
    arith_uint256 bnTargetLimit = UintToArith256(pnetMan->getActivePaymentNetwork()->GetConsensus().powLimit);

    if (fProofOfStake)
    {
        // Proof-of-Stake blocks has own target limit since nVersion=3 supermajority on mainNet and always on testNet
        bnTargetLimit = UintToArith256(pnetMan->getActivePaymentNetwork()->GetConsensus().posLimit);
    }

    if (pindexLast == NULL)
        return bnTargetLimit.GetCompact(); // genesis block

    const CBlockIndex *pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);

    // Special rule for regtest: we never retarget.
    if (pnetMan->getActivePaymentNetwork()->GetConsensus().fPowNoRetargeting)
    {
        return pindexPrev->nBits;
    }

    if (pindexPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // first block
    const CBlockIndex *pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // second block

    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();
    if (nActualSpacing < 0)
    {
        nActualSpacing = 1;
    }
    else if (nActualSpacing > pnetMan->getActivePaymentNetwork()->GetConsensus().nTargetTimespan)
    {
        nActualSpacing = pnetMan->getActivePaymentNetwork()->GetConsensus().nTargetTimespan;
    }

    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    int64_t spacing;
    int64_t targetSpacing = pnetMan->getActivePaymentNetwork()->GetConsensus().nTargetSpacing;
    if (pindexPrev->GetMedianTimePast() > SERVICE_UPGRADE_HARDFORK)
    {
        targetSpacing = 150;
    }
    if (fProofOfStake)
    {
        spacing = targetSpacing;
    }
    else
    {
        spacing = std::min(
            (3 * (int64_t)targetSpacing), ((int64_t)targetSpacing * (1 + pindexLast->nHeight - pindexPrev->nHeight)));
    }
    int64_t nTargetSpacing = spacing;
    int64_t nInterval = pnetMan->getActivePaymentNetwork()->GetConsensus().nTargetTimespan / nTargetSpacing;
    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    if (bnNew > bnTargetLimit)
    {
        bnNew = bnTargetLimit;
    }

    return bnNew.GetCompact();
}

int generateMTRandom(unsigned int s, int range)
{
    std::mt19937 gen(s);
    std::uniform_int_distribution<> dist(0, range);
    return dist(gen);
}

static const int64_t nMinSubsidy = 1 * COIN;
static const CAmount OLD_MAX_MONEY = 50000000000 * COIN;

// miner's coin base reward
int64_t GetProofOfWorkReward(int64_t nFees, const int nHeight, uint256 prevHash)
{
    int64_t nSubsidy = 100000 * COIN;

    if (nHeight == 1)
    {
        nSubsidy = 0.0099 * OLD_MAX_MONEY;
        return nSubsidy + nFees;
    }
    else if (nHeight > 86400) // will be blocked all the pow after CUTOFF_HEIGHT
    {
        return nMinSubsidy + nFees;
    }

    std::string cseed_str = prevHash.ToString().substr(15, 7);
    const char *cseed = cseed_str.c_str();
    long seed = hex2long(cseed);
    nSubsidy += generateMTRandom(seed, 200000) * COIN;

    return nSubsidy + nFees;
}

int64_t ValueFromAmountAsInt(int64_t amount) { return amount / COIN; }
const int YEARLY_BLOCKCOUNT = 700800;
// miner's coin stake reward based on coin age spent (coin-days)
int64_t GetProofOfStakeReward(int64_t nCoinAge, int nHeight)
{
    int64_t nRewardCoinYear = 2.5 * MAX_MINT_PROOF_OF_STAKE;
    int64_t CMS = pnetMan->getChainActive()->chainActive.Tip()->nMoneySupply;
    if (CMS == MAX_MONEY)
    {
        // if we are already at max money supply limits (25 billion coins, we return 0 as no new coins are to be minted
        if (fDebug)
        {
            LogPrintf("GetProofOfStakeReward(): create=%i nCoinAge=%d\n", 0, nCoinAge);
        }
        return 0;
    }
    if (nHeight > 500000 && nHeight < 1005000)
    {
        int64_t nextMoney = (ValueFromAmountAsInt(CMS) + nRewardCoinYear);
        if (nextMoney > MAX_MONEY)
        {
            int64_t difference = nextMoney - MAX_MONEY;
            nRewardCoinYear = nextMoney - difference;
        }
        if (nextMoney == MAX_MONEY)
        {
            nRewardCoinYear = 0;
        }
        int64_t nSubsidy = nCoinAge * nRewardCoinYear / 365;
        if (fDebug)
        {
            LogPrintf("GetProofOfStakeReward(): create=%s nCoinAge=%d\n", FormatMoney(nSubsidy).c_str(), nCoinAge);
        }
        return nSubsidy;
    }

    nRewardCoinYear = 25 * CENT; // 25%
    int64_t nSubsidy = nCoinAge * nRewardCoinYear / 365;
    if (nHeight >= 1005000)
    {
        int64_t nextMoney = CMS + nSubsidy;
        // this conditional should only happen once
        if (nextMoney > MAX_MONEY)
        {
            // CMS + subsidy = nextMoney
            // nextMoney - MAX = difference and we should take this difference away from nSubsidy so nSubsidy stops at
            // max money and doesnt go over
            // credits go to cvargos for this fix
            int64_t difference = (nextMoney - MAX_MONEY);
            nSubsidy = nSubsidy - difference;
        }
    }
    if (fDebug)
    {
        LogPrintf("GetProofOfStakeReward(): create=%s nCoinAge=%d\n", FormatMoney(nSubsidy).c_str(), nCoinAge);
    }
    return nSubsidy;
}
