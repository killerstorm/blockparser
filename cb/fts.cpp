
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

#include <random>

typedef std::pair<uint64_t, uint64_t> SatoshiRange;
typedef std::vector<SatoshiRange> SatoshiRanges;

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

typedef std::map<Outpoint, SatoshiRanges*> TSRMap;
typedef std::map<SatoshiRange, Outpoint> SatoshiMap;

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
    std::vector<uint64_t> outValues, coinbaseOutValues, debugTargets;
    Hash256 curTxHash, coinbaseTxHash;
    uint64_t curHeight;
    bool curTxHasInputs;

    double startFirstPass, startSecondPass;

    int inputs_scanned, outputs_scanned;


    FTS_UTXO()
	:inputs_scanned(0), outputs_scanned(0)
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


        std::ifstream inf("fts.debug.target");
        while (inf.good()) {
            uint64_t satoshi;
            inf >> satoshi;
            if (inf.good()) 
                debugTargets.push_back(satoshi);
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
                satoshiMap.insert(SatoshiMap::value_type(orange, outpt));
                
                for (std::vector<uint64_t>::iterator it = debugTargets.begin(); it != debugTargets.end(); ++it) {
                    if ((orange.first <= *it) && (*it < orange.second)) {
                        uint64_t offset = (*it) - orange.first;
                        std::cout << "debug:" << (*it) << " went to ";
                        showHex(curTxHash);
                        std::cout << ":" << i << ":" << isCoinbase << " offset:" << offset  << std::endl;
                    }
                }
                    

            }
            SatoshiRanges *r = new SatoshiRanges(outRanges);
            utxoRanges.insert(std::pair<Outpoint, SatoshiRanges*>(outpt, r));
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
            Outpoint outpt(curTxHash, -1);
            if (curRange.first != curRange.second) {
                std::cout << "coin range destroyed:" << curRange.first << ":" << curRange.second << std::endl;
                satoshiMap.insert(SatoshiMap::value_type(curRange, outpt));
            }
            while (inRangeIt != inRanges.end()) {
                std::cout << "coin range destroyed:" << inRangeIt->first << ":" << inRangeIt->second << std::endl;
                satoshiMap.insert(SatoshiMap::value_type(*inRangeIt, outpt));
                ++inRangeIt;
            }
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
            inputs_scanned ++;
            curTxHasInputs = true;
            Outpoint outpt(upTXHash, outputIndex);
            SatoshiRanges* sr = utxoRanges[outpt];
            for (SatoshiRanges::iterator it = sr->begin(); it != sr->end(); ++it) {
                inRanges.push_back(*it);
                satoshiMap.erase(*it);
            }
            delete sr;
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
            outputs_scanned ++;
            outValues.push_back(value);            
        }
    
    bool find_txout(uint64_t satoshi, Outpoint& op) {
        SatoshiRange sr(satoshi, satoshi + 1);
        SatoshiMap::iterator it = satoshiMap.upper_bound(sr); // next range
        if (it == satoshiMap.end()) {
            //TODO: check out of range here
            return false;
            //op = satoshiMap.rbegin()->second;
        } else {
            sr = it->first;
            if (sr.first > satoshi) {
                --it;
                sr = it->first;
            }
            if (! ((sr.first <= satoshi) && (satoshi < sr.second))) {
                std::cout << "error in find_txout:" << sr.first << " " << satoshi << " " << sr.second << std::endl;
                return false;
            }
            op = it->second;
        }
        return true;
    }

    uint64_t integrity_check() {
        SatoshiRange prev(0, 0);
        for (SatoshiMap::iterator it = satoshiMap.begin(); it != satoshiMap.end(); ++it) {
            SatoshiRange sr = it->first;
            if (sr.first != prev.second) {
                if (prev.second < sr.first) {
                    std::cout << "satoshi hole:" << std::endl;
                } else {
                    std::cout << "wtf:" << std::endl;
                }
                std::cout << prev.first <<  ":" << prev.second << std::endl;
                std::cout << sr.first <<  ":" << sr.second << std::endl;
            }
            prev = sr;
        }
        std::cout << "last satoshi: " << prev.second << std::endl;        
        return prev.second;
    }

    virtual void wrapup()
    {
        info("done\n");
        std::cout << "UTXO count: " << utxoRanges.size() << std::endl
                  << "Range count: " << satoshiMap.size() << std::endl;

        double endt = usecs();
        info("first  pass done in %.3f seconds\n", (startSecondPass - startFirstPass)*1e-6);
        info("second pass done in %.3f seconds\n", (endt - startSecondPass)*1e-6);
        info("scanned: inputs: %i, outputs: %i\n", inputs_scanned, outputs_scanned);
        
        uint64_t last_satoshi = integrity_check();

        std::ifstream inf("fts.target");
        while (inf.good()) {
          uint64_t satoshi;
          inf >> satoshi;
          if (inf.good()) {
              std::cout << "target:" << satoshi << " ";
              
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

        
        const size_t nlookups = 1000000;
        std::vector<uint64_t> test_satoshi;
        std::mt19937 rng;
        std::uniform_int_distribution<std::mt19937::result_type> sat_dist(0, last_satoshi - 1);
        for(int i = 0; i < nlookups; ++i) {
            test_satoshi.push_back(sat_dist(rng));
        }
        double startThirdPass = usecs();
        int meaningless = 0;
        for (int i = 0; i < nlookups; ++ i) {
            Outpoint op;
            if (find_txout(test_satoshi[i], op))
                meaningless += op.outindex;
        }
        endt = usecs();
        info("third  pass done in %.3f seconds\n", (endt - startThirdPass)*1e-6);
        std::cout << meaningless << std::endl;

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

