
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
#include <fstream>

typedef std::pair<uint64_t, uint64_t> SatoshiRange;
typedef std::vector<SatoshiRange> SatoshiRanges;
typedef std::vector<SatoshiRanges> ManySatoshiRanges;

struct Outpoint {
    uint256_t txhash;
    int outindex;
    Outpoint() {}
    Outpoint(Hash256 h, int oi)
        :outindex(oi)
        {
            memcpy(txhash.v, h, 32);
        }

    bool operator < (const Outpoint & r) const {
        for (int i = 0; i < 32; ++i) {
            uint8_t lc = txhash.v[i], rc = r.txhash.v[i];
            if (lc == rc) continue;
            return (lc < rc);
        }
        return outindex < r.outindex;
    }
};

typedef std::map<Outpoint, SatoshiRanges> TSRMap;
typedef std::map<uint64_t, Outpoint> SatoshiMap;

uint64_t satoshiBeforeBlock(int height) {
    return (height-1) * 50 * uint64_t(100000000); // TODO: fixme
}

struct FTS_UTXO: public Callback
{
    int64_t cutoffBlock;
    optparse::OptionParser parser;
    TSRMap utxoRanges;
    SatoshiMap satoshiMap;

    SatoshiRanges inRanges, coinbaseInRanges;
    std::vector<uint64_t> outValues, coinbaseOutValues;
    Hash160 curTxHash, coinbaseTxHash;
    uint64_t curHeight;
    bool curTxHasInputs;

    double startFirstPass, startSecondPass;


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
    virtual void aliases(std::vector<const char*> &v) const {  v.push_back("fts");   }

    virtual int init(int argc, const char *argv[])  {
        optparse::Values &values = parser.parse_args(argc, argv);
        cutoffBlock = values.get("atBlock");
        
        if(0<=cutoffBlock) {
            info("only taking into account transactions before block %" PRIu64 "\n", cutoffBlock);
        }
        
        info("analyzing blockchain ...");
        startFirstPass = usecs();
        return 0;
    }
    virtual void start(const Block *s, const Block *e) {
        startSecondPass = usecs();
    }

    virtual void startTX(const uint8_t *p, const uint8_t *hash) {
        inRanges.clear();
        outValues.clear();
        curTxHash = hash;
        curTxHasInputs = false;
    }

    virtual void processTX(SatoshiRanges &inRanges, std::vector<uint64_t> &outValues, Hash256 curTxHash, bool isCoinbase = false) {
        //showHex(curTxHash); std::cout << std::endl;

        SatoshiRange curRange(0, 0);
        SatoshiRanges::iterator inRangeIt = inRanges.begin();
        
        for (size_t i = 0; i < outValues.size(); ++i) {
            Outpoint outpt(curTxHash, i);
            SatoshiRanges outRanges;
            uint64_t val_left = outValues[i];
            while (val_left > 0) {
                if (curRange.first == curRange.second) { // cur range is empty
                    if (inRangeIt == inRanges.end())
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
                satoshiMap.insert(std::pair<uint64_t, Outpoint>(orange.first, outpt));
                //if (!isCoinbase)
                //std::cout << i << ":" <<  orange.first << ":" << orange.second << std::endl;
            }
            utxoRanges.insert(std::pair<Outpoint, SatoshiRanges>(outpt, outRanges));
        }

        if (!isCoinbase) {
            // ranges which were not consumed by outputs go to coinbase tx
            if (curRange.first != curRange.second)
                coinbaseInRanges.push_back(curRange);
            while (inRangeIt != inRanges.end()) {
                coinbaseInRanges.push_back(*inRangeIt);
                ++inRangeIt;
            }
        } else {
            // TODO
        }
    }


    virtual void endTX(const uint8_t *p) {
        if (curTxHasInputs) 
            processTX(inRanges, outValues, curTxHash);
        else {
            coinbaseInRanges.clear();
            coinbaseInRanges.push_back(SatoshiRange(satoshiBeforeBlock(curHeight), satoshiBeforeBlock(curHeight + 1)));            
            coinbaseOutValues = outValues;
            coinbaseTxHash = curTxHash;
        }             
    }

    virtual void endBlock(const Block *b) {
        processTX(coinbaseInRanges, coinbaseOutValues, coinbaseTxHash, true);
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
            curTxHasInputs = true;
            Outpoint outpt(upTXHash, outputIndex);
            SatoshiRanges& sr = utxoRanges[outpt];
            for (SatoshiRanges::iterator it = sr.begin(); it != sr.end(); ++it) {
                inRanges.push_back(*it);
                satoshiMap.erase(it->first);
            }
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
    
    bool find_txout(uint64_t satoshi, Outpoint& op) {
        SatoshiMap::iterator it = satoshiMap.upper_bound(satoshi); // next range
        if (it == satoshiMap.begin())
            return false;
        if (it == satoshiMap.end()) {
            //TODO: check out of range here
            return false;
            op = satoshiMap.rbegin()->second;
        } else
            op = (--it)->second;
        return true;
    }

    virtual void wrapup()
    {
        info("done\n");
        std::cout << "UTXO count: " << utxoRanges.size() << std::endl
                  << "Range count: " << satoshiMap.size() << std::endl;

        double endt = usecs();
        info("first  pass done in %.3f seconds\n", (endt - startFirstPass)*1e-6);
        info("second pass done in %.3f seconds\n", (endt - startSecondPass)*1e-6);

        std::ifstream inf("fts.target");
        while (inf.good()) {
          uint64_t satoshi;
          inf >> satoshi;
          std::cout << satoshi << " ";
          if (inf.good()) {
              Outpoint op;
              if (find_txout(satoshi, op)) {
                  showHex(op.txhash.v);
                  std::cout << " " << op.outindex;
              } else {
                  std::cout << " 0";
              }
              std::cout << std::endl;

          }
        }
        printf("\n");
        exit(0);
    }

    virtual void startBlock(const Block *b, uint64_t chainSize)
    {
        curHeight = b->height;
        std::cout << "parsing block " << curHeight << std::endl;

        if(0<=cutoffBlock && cutoffBlock<=b->height) {
            wrapup();
        }
    }

};

static FTS_UTXO fts_utxo;

