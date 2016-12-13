//
// Created by mwo on 12/12/16.
//

#ifndef RESTBED_XMR_TXSEARCH_H
#define RESTBED_XMR_TXSEARCH_H



#include "MySqlConnector.h"
#include "tools.h"
#include "mylmdb.h"

#include <mysql++/mysql++.h>
#include <mysql++/ssqls.h>


#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>

namespace xmreg
{

mutex mtx;


class TxSearchException: public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/*
 * This is a thread class
 */
struct CurrentBlockchainStatus
{
    static string blockchain_path;

    static atomic<uint64_t> current_height;

    static bool testnet;

    static std::thread m_thread;

    static bool is_running;

    static uint64_t refresh_block_status_every_seconds;

    // since this class monitors current status
    // of the blockchain, its seems logical to
    // make object for accessing the blockchain here
    static xmreg::MicroCore mcore;
    static cryptonote::Blockchain* core_storage;

    static
    void start_monitor_blockchain_thread()
    {
        if (!is_running)
        {
            m_thread = std::thread{ []()
            {
                while (true)
                {
                    current_height = get_current_blockchain_height();
                    cout << "Check block height: " << current_height << endl;
                    std::this_thread::sleep_for(std::chrono::seconds(refresh_block_status_every_seconds));
                }

            }};

            is_running = true;
        }
    }

    static inline
    uint64_t
    get_current_blockchain_height()
    {
        return xmreg::MyLMDB::get_blockchain_height(blockchain_path) - 1;
    }

    static void
    set_blockchain_path(const string& path)
    {
        blockchain_path = path;
    }

    static void
    set_testnet(bool is_testnet)
    {
        testnet = is_testnet;
    }

    static bool
    init_monero_blockchain()
    {
        // enable basic monero log output
        xmreg::enable_monero_log();

         // initialize mcore and core_storage
        if (!xmreg::init_blockchain(blockchain_path,
                                    mcore, core_storage))
        {
            cerr << "Error accessing blockchain." << endl;
            return false;
        }

        return true;
    }

    static bool
    get_block(uint64_t height, block& blk)
    {
        return mcore.get_block_by_height(height, blk);
    }

    static bool
    get_block_txs(const block& blk, list<transaction>& blk_txs)
    {
        // get all transactions in the block found
        // initialize the first list with transaction for solving
        // the block i.e. coinbase tx.
        blk_txs.push_back(blk.miner_tx);

        list<crypto::hash> missed_txs;

        if (!core_storage->get_transactions(blk.tx_hashes, blk_txs, missed_txs))
        {
            cerr << "Cant get transactions in block: " << get_block_hash(blk) << endl;
            return false;
        }

        return true;
    }


};

// initialize static variables
atomic<uint64_t>        CurrentBlockchainStatus::current_height {0};
string                  CurrentBlockchainStatus::blockchain_path {"/home/mwo/.blockchain/lmdb"};
bool                    CurrentBlockchainStatus::testnet  {false};
bool                    CurrentBlockchainStatus::is_running  {false};
std::thread             CurrentBlockchainStatus::m_thread;
uint64_t                CurrentBlockchainStatus::refresh_block_status_every_seconds {60};
xmreg::MicroCore        CurrentBlockchainStatus::mcore;
cryptonote::Blockchain* CurrentBlockchainStatus::core_storage;

class TxSearch
{

    bool continue_search {true};

    // represents a row in mysql's Accounts table
    XmrAccount acc;

    // this manages all mysql queries
    // its better to when each thread has its own mysql connection object.
    // this way if one thread crashes, it want take down
    // connection for the entire service
    MySqlAccounts xmr_accounts;

    // address and viewkey for this search thread.
    account_public_address address;
    secret_key viewkey;

public:

    TxSearch() {}

    TxSearch(XmrAccount& _acc):
            acc {_acc},
            xmr_accounts()
    {

        bool testnet = CurrentBlockchainStatus::testnet;

        if (!xmreg::parse_str_address(acc.address, address, testnet))
        {
            cerr << "Cant parse string address: " << acc.address << endl;
            throw TxSearchException("Cant parse string address: " + acc.address);
        }

        if (!xmreg::parse_str_secret_key(acc.viewkey, viewkey))
        {
            cerr << "Cant parse the private key: " << acc.viewkey << endl;
            throw TxSearchException("Cant parse private key: " + acc.viewkey);
        }

    }

    void
    search()
    {

        // start searching from last block that we searched for
        // this accont
        uint64_t searched_blk_no = acc.scanned_block_height;

        if (searched_blk_no > CurrentBlockchainStatus::current_height)
        {
            throw TxSearchException("searched_blk_no > CurrentBlockchainStatus::current_height");
        }

        while(continue_search)
        {

            if (searched_blk_no > CurrentBlockchainStatus::current_height)
            {
                fmt::print("searched_blk_no {:d} and current_height {:d}\n",
                           searched_blk_no, CurrentBlockchainStatus::current_height);

                std::this_thread::sleep_for(
                        std::chrono::seconds(
                                CurrentBlockchainStatus::refresh_block_status_every_seconds)
                );

                continue;
            }

            //
            cout << " - searching tx of: " << acc << endl;

            // get block cointaining this tx
            block blk;

            if (!CurrentBlockchainStatus::get_block(searched_blk_no, blk))
            {
                cerr << "Cant get block of height: " + to_string(searched_blk_no) << endl;
                searched_blk_no =- 2; // just go back one block, and retry
                continue;
            }

            // for each tx in the given block look, get ouputs


            list<cryptonote::transaction> blk_txs;

            if (!CurrentBlockchainStatus::get_block_txs(blk, blk_txs))
            {
                throw TxSearchException("Cant get transactions in block: " + to_string(searched_blk_no));
            }



            //std::lock_guard<std::mutex> lck (mtx);
            fmt::print(" - searching block  {:d} of hash {:s} \n",
                       searched_blk_no, pod_to_hex(get_block_hash(blk)));

            for (transaction& tx: blk_txs)
            {

                crypto::hash tx_hash  = get_transaction_hash(tx);

                // cout << pod_to_hex(tx_hash) << endl;

                public_key tx_pub_key = xmreg::get_tx_pub_key_from_received_outs(tx);

                //          <public_key  , amount  , out idx>
                vector<tuple<txout_to_key, uint64_t, uint64_t>> outputs;

                outputs = get_ouputs_tuple(tx);

                // for each output, in a tx, check if it belongs
                // to the given account of specific address and viewkey

                // public transaction key is combined with our viewkey
                // to create, so called, derived key.
                key_derivation derivation;

                if (!generate_key_derivation(tx_pub_key, viewkey, derivation))
                {
                    cerr << "Cant get derived key for: "  << "\n"
                         << "pub_tx_key: " << tx_pub_key << " and "
                         << "prv_view_key" << viewkey << endl;

                    throw TxSearchException("");
                }

                for (auto& out: outputs)
                {
                    txout_to_key txout_k      = std::get<0>(out);
                    uint64_t amount           = std::get<1>(out);
                    uint64_t output_idx_in_tx = std::get<2>(out);

                    // get the tx output public key
                    // that normally would be generated for us,
                    // if someone had sent us some xmr.
                    public_key generated_tx_pubkey;

                    derive_public_key(derivation,
                                      output_idx_in_tx,
                                      address.m_spend_public_key,
                                      generated_tx_pubkey);

                    // check if generated public key matches the current output's key
                    bool mine_output = (txout_k.key == generated_tx_pubkey);

                    // if mine output has RingCT, i.e., tx version is 2
                    // need to decode its amount. otherwise its zero.
                    if (mine_output && tx.version == 2)
                    {
                        // initialize with regular amount
                        uint64_t rct_amount = amount;

                        bool r;

                        r = decode_ringct(tx.rct_signatures,
                                          tx_pub_key,
                                          viewkey,
                                          output_idx_in_tx,
                                          tx.rct_signatures.ecdhInfo[output_idx_in_tx].mask,
                                          rct_amount);

                        if (!r)
                        {
                            cerr << "Cant decode ringCT!" << endl;
                            throw TxSearchException("");
                        }

                        // cointbase txs have amounts in plain sight.
                        // so use amount from ringct, only for non-coinbase txs
                        if (!is_coinbase(tx))
                        {
                            rct_amount = amount;
                        }

                        amount = rct_amount;

                    } // if (mine_output && tx.version == 2)

                    if (mine_output)
                    {
                        // found an output associated with the given address and viewkey
                        string msg = fmt::format("block: {:d}, tx_hash:  {:s}, output_pub_key: {:s}\n",
                                                 searched_blk_no,
                                                 pod_to_hex(tx_hash),
                                                 pod_to_hex(txout_k.key));

                        cout << msg << endl;
                    }

                } // for (const auto& out: outputs)

            } // for (const transaction& tx: blk_txs)

            ++searched_blk_no;

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void
    stop()
    {
        cout << "something to stop the thread by setting continue_search=false" << endl;
    }

    ~TxSearch()
    {
        cout << "TxSearch destroyed" << endl;
    }


};

}

#endif //RESTBED_XMR_TXSEARCH_H
