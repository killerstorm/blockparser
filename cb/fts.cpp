
// Dump balance of all addresses ever used in the blockchain

#include <util.h>
#include <common.h>
#include <errlog.h>
#include <option.h>
#include <rmd160.h>
#include <sha256.h>
#include <callback.h>

#include <vector>
#include <string.h>

#include <iostream>

typedef std::pair<int64_t, int64_t> SatoshiRange;
typedef std::vector<SatoshiRange> SatoshiRanges;
typedef std::vector<SatoshiRanges> ManySatoshiRanges;

struct Outpoint {
    uint256_t txhash;
    int outindex;
    Outpoint(Hash256 h, int oi)
        :outindex(oi)
        {
            memcpy(txhash.v, h, 8);
        }

    bool operator < (const Outpoint & r) const {
        for (int i = 0; i < 8; ++i) {
            if (txhash.v[i] < r.txhash.v[i]) return true;
            else if (txhash.v[i] > r.txhash.v[i]) return false;
        }
        return outindex < r.outindex;
    }
};

typedef std::map<Outpoint, SatoshiRanges> TSRMap;

uint64_t satoshiBeforeBlock(int height) {
    return height * 50 * uint64_t(100000000); // TODO: fixme
}

struct FTS_UTXO: public Callback
{
    int64_t cutoffBlock;
    optparse::OptionParser parser;
    TSRMap utxoRanges;

    ManySatoshiRanges inRanges;
    std::vector<int64_t> outValues;
    Hash160 curTxHash;
    uint64_t curHeight;

    FTS_UTXO()
    {
        parser
            .usage("[options]")
            .version("")
            .description("dump satoshi ranges of UTXOs")
            .epilog("")
        ;
        parser
            .add_option("-a", "--atBlock")
            .action("store")
            .type("int")
            .set_default(-1)
            .help("only take into account transactions in blocks strictly older than <block> (default: all)")
        ;
    }

    virtual const char                   *name() const         { return "fts_utxo"; }
    virtual const optparse::OptionParser *optionParser() const { return &parser;       }
    virtual bool                         needTXHash() const    { return true;          }

    virtual void aliases(
        std::vector<const char*> &v
    ) const
    {
        v.push_back("fts");
    }

    virtual int init(
        int argc,
        const char *argv[]
    )
    {
        optparse::Values &values = parser.parse_args(argc, argv);
        cutoffBlock = values.get("atBlock");

        if(0<=cutoffBlock) {
            info("only taking into account transactions before block %" PRIu64 "\n", cutoffBlock);
        }

        info("analyzing blockchain ...");
        return 0;
    }

    virtual void      startTX(const uint8_t *p, const uint8_t *hash)       {
        inRanges.clear();
        outValues.clear();
        curTxHash = hash;
    }

    virtual void        endTX(const uint8_t *p                     )       {
        SatoshiRanges flatInRanges;
        for (ManySatoshiRanges::iterator it = inRanges.begin(); it != inRanges.end(); ++it) {
            flatInRanges.insert(flatInRanges.end(), it->begin(), it->end());
        }

        bool isCoinbase = false;

        if (flatInRanges.empty()) { // it is a coinbase
            isCoinbase = true;
            flatInRanges.push_back(SatoshiRange(satoshiBeforeBlock(curHeight), satoshiBeforeBlock(curHeight + 1)));
        }

        Outpoint outpt(curTxHash, 8);

        SatoshiRange curRange(0, 0);
        SatoshiRanges::iterator inRangeIt = flatInRanges.begin();
        
        for (int i = 0; i < outValues.size(); ++i) {
            SatoshiRanges outRanges;
            int64_t val_left = outValues[i];
            while (val_left > 0) {
                if (curRange.first == curRange.second) { // cur range is empty
                    if (inRangeIt == flatInRanges.end())
                        throw std::range_error("wtf tx outputs exceed inputs");
                    curRange = *inRangeIt;
                    ++inRangeIt;
                }
                SatoshiRange orange = curRange;
                if ((orange.second - orange.first) > val_left) {
                    orange.second = orange.first + val_left;
                }
                val_left -= (orange.second - orange.first);
                curRange.first = orange.second;
                outRanges.push_back(orange);
                if (!isCoinbase)
                    std::cout << orange.first << ":" << orange.second << std::endl;
            }
            outpt.outindex = i;
            utxoRanges.insert(std::pair<Outpoint, SatoshiRanges>(outpt, outRanges));
        }
    }
    
    virtual void edge(
        uint64_t      value,
        const uint8_t *upTXHash,
        uint64_t      outputIndex,
        const uint8_t *outputScript,
        uint64_t      outputScriptSize,
        const uint8_t *downTXHash,
        uint64_t      inputIndex,
        const uint8_t *inputScript,
        uint64_t      inputScriptSize)
        {
            Outpoint outpt(upTXHash, outputIndex);
            inRanges.push_back(utxoRanges[outpt]);
            utxoRanges.erase(outpt);
        }
   
    virtual void endOutput(
        const uint8_t *p,                   // Pointer to TX output raw data
        uint64_t      value,                // Number of satoshis on this output
        const uint8_t *txHash,              // sha256 of the current transaction
        uint64_t      outputIndex,          // Index of this output in the current transaction
        const uint8_t *outputScript,        // Raw script (challenge to would-be spender) carried by this output
        uint64_t      outputScriptSize      // Byte size of raw script
        )
        {
            outValues.push_back(value);            
        }
    

    virtual void wrapup()
    {
        info("done\n");

        printf("\n");
        exit(0);
    }

    virtual void startBlock(
        const Block *b,
        uint64_t chainSize
    )
    {
        curHeight = b->height;
        std::cout << "parsing block " << curHeight << std::endl;        

        if(0<=cutoffBlock && cutoffBlock<=b->height) {
            wrapup();
        }
    }

};

static FTS_UTXO fts_utxo;

