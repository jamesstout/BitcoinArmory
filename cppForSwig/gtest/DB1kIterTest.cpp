////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig                                               //            
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                   
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
#include <limits.h>
#include <iostream>
#include <stdlib.h>
#include <stdint.h>
#include <thread>
#include "gtest.h"

#include "../log.h"
#include "../BinaryData.h"
#include "../BtcUtils.h"
#include "../BlockObj.h"
#include "../StoredBlockObj.h"
#include "../PartialMerkle.h"
#include "../EncryptionUtils.h"
#include "../lmdb_wrapper.h"
#include "../BlockUtils.h"
#include "../ScrAddrObj.h"
#include "../BtcWallet.h"
#include "../BlockDataViewer.h"
#include "../cryptopp/DetSign.h"
#include "../cryptopp/integer.h"
#include "../Progress.h"
#include "../reorgTest/blkdata.h"
#include "../BDM_seder.h"
#include "../BDM_Server.h"
#include "../TxClasses.h"
#include "../txio.h"
#include "../bdmenums.h"
#include "../SwigClient.h"


#ifdef _MSC_VER
#ifdef mlock
#undef mlock
#undef munlock
#endif
#include "win32_posix.h"
#undef close

#ifdef _DEBUG
//#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>

#ifndef DBG_NEW
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#define new DBG_NEW
#endif
#endif
#endif

#define READHEX BinaryData::CreateFromHex

static uint32_t getTopBlockHeightInDB(BlockDataManager &bdm, DB_SELECT db)
{
   StoredDBInfo sdbi;
   bdm.getIFace()->getStoredDBInfo(db, 0);
   return sdbi.topBlkHgt_;
}

static uint64_t getDBBalanceForHash160(
   BlockDataManager &bdm,
   BinaryDataRef addr160
   )
{
   StoredScriptHistory ssh;

   bdm.getIFace()->getStoredScriptHistory(ssh, HASH160PREFIX + addr160);
   if (!ssh.isInitialized())
      return 0;

   return ssh.getScriptBalance();
}

// Utility function - Clean up comments later
static int char2int(char input)
{
   if (input >= '0' && input <= '9')
      return input - '0';
   if (input >= 'A' && input <= 'F')
      return input - 'A' + 10;
   if (input >= 'a' && input <= 'f')
      return input - 'a' + 10;
   return 0;
}

// This function assumes src to be a zero terminated sanitized string with
// an even number of [0-9a-f] characters, and target to be sufficiently large
static void hex2bin(const char* src, unsigned char* target)
{
   while (*src && src[1])
   {
      *(target++) = char2int(*src) * 16 + char2int(src[1]);
      src += 2;
   }
}

#if ! defined(_MSC_VER) && ! defined(__MINGW32__)
/////////////////////////////////////////////////////////////////////////////
static void rmdir(string src)
{
   char* syscmd = new char[4096];
   sprintf(syscmd, "rm -rf %s", src.c_str());
   system(syscmd);
   delete[] syscmd;
}

/////////////////////////////////////////////////////////////////////////////
static void mkdir(string newdir)
{
   char* syscmd = new char[4096];
   sprintf(syscmd, "mkdir -p %s", newdir.c_str());
   system(syscmd);
   delete[] syscmd;
}
#endif

static void concatFile(const string &from, const string &to)
{
   std::ifstream i(from, ios::binary);
   std::ofstream o(to, ios::app | ios::binary);

   o << i.rdbuf();
}

static void appendBlocks(const std::vector<std::string> &files, const std::string &to)
{
   for (const std::string &f : files)
      concatFile("../reorgTest/blk_" + f + ".dat", to);
}

static void setBlocks(const std::vector<std::string> &files, const std::string &to)
{
   std::ofstream o(to, ios::trunc | ios::binary);
   o.close();

   for (const std::string &f : files)
      concatFile("../reorgTest/blk_" + f + ".dat", to);
}

static void nullProgress(unsigned, double, unsigned, unsigned)
{

}

static BinaryData getTx(unsigned height, unsigned id)
{
   stringstream ss;
   ss << "../reorgTest/blk_" << height << ".dat";

   ifstream blkfile(ss.str(), ios::binary);
   blkfile.seekg(0, ios::end);
   auto size = blkfile.tellg();
   blkfile.seekg(0, ios::beg);

   vector<char> vec;
   vec.resize(size);
   blkfile.read(&vec[0], size);
   blkfile.close();

   BinaryRefReader brr((uint8_t*)&vec[0], size);
   StoredHeader sbh;
   sbh.unserializeFullBlock(brr, false, true);

   if (sbh.stxMap_.size() - 1 < id)
      throw range_error("invalid tx id");

   auto& stx = sbh.stxMap_[id];
   return stx.dataCopy_;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
string registerBDV(Clients* clients, const BinaryData& magic_word)
{
   Command cmd;
   cmd.method_ = "registerBDV";
   BinaryDataObject bdo(magic_word);
   cmd.args_.push_back(move(bdo));
   cmd.serialize();

   auto&& result = clients->runCommand(cmd.command_);

   auto& argVec = result.getArgVector();
   auto bdvId = dynamic_pointer_cast<DataObject<BinaryDataObject>>(argVec[0]);
   return bdvId->getObj().toStr();
}

void goOnline(Clients* clients, const string& id)
{
   Command cmd;
   cmd.method_ = "goOnline";
   cmd.ids_.push_back(id);
   cmd.serialize();
   clients->runCommand(cmd.command_);
}

const shared_ptr<BDV_Server_Object> getBDV(Clients* clients, const string& id)
{
   return clients->get(id);
}

void regWallet(Clients* clients, const string& bdvId,
   const vector<BinaryData>& scrAddrs, const string& wltName)
{
   Command cmd;
   unsigned isNewInt = (unsigned int)false;

   BinaryDataObject bdo(wltName);
   cmd.args_.push_back(move(bdo));
   cmd.args_.push_back(move(BinaryDataVector(scrAddrs)));
   cmd.args_.push_back(move(isNewInt));

   cmd.method_ = "registerWallet";
   cmd.ids_.push_back(bdvId);
   cmd.serialize();

   auto&& result = clients->runCommand(cmd.command_);

   //check result
   auto& argVec = result.getArgVector();
   auto retint = dynamic_pointer_cast<DataObject<unsigned>>(argVec[0]);
   if (retint->getObj() == 0)
      throw runtime_error("server returned false to registerWallet query");
}

void regLockbox(Clients* clients, const string& bdvId,
   const vector<BinaryData>& scrAddrs, const string& wltName)
{
   Command cmd;
   unsigned isNewInt = (unsigned int)false;

   BinaryDataObject bdo(wltName);
   cmd.args_.push_back(move(bdo));
   cmd.args_.push_back(move(BinaryDataVector(scrAddrs)));
   cmd.args_.push_back(move(isNewInt));

   cmd.method_ = "registerLockbox";
   cmd.ids_.push_back(bdvId);
   cmd.serialize();

   auto&& result = clients->runCommand(cmd.command_);

   //check result
   auto& argVec = result.getArgVector();
   auto retint = dynamic_pointer_cast<DataObject<unsigned>>(argVec[0]);
   if (retint->getObj() == 0)
      throw runtime_error("server returned false to registerWallet query");
}

void waitOnSignal(Clients* clients, const string& bdvId,
   string command, const string& signal)
{
   Command cmd;
   cmd.method_ = "registerCallback";
   cmd.ids_.push_back(bdvId);

   BinaryDataObject bdo(command);
   cmd.args_.push_back(move(bdo));
   cmd.serialize();

   auto processCallback = [&](Arguments args)->bool
   {
      auto& argVec = args.getArgVector();

      for (auto arg : argVec)
      {
         auto argstr = dynamic_pointer_cast<DataObject<BinaryDataObject>>(arg);
         if (argstr == nullptr)
            continue;

         auto&& cb = argstr->getObj().toStr();
         if (cb == signal)
            return true;
      }

      return false;
   };

   while (1)
   {
      auto&& result = clients->runCommand(cmd.command_);

      if (processCallback(move(result)))
         return;
   }
}

void waitOnBDMReady(Clients* clients, const string& bdvId)
{
   waitOnSignal(clients, bdvId, "waitOnBDV", "BDM_Ready");
}

void waitOnNewBlockSignal(Clients* clients, const string& bdvId)
{
   waitOnSignal(clients, bdvId, "getStatus", "NewBlock");
}

void waitOnNewZcSignal(Clients* clients, const string& bdvId)
{
   waitOnSignal(clients, bdvId, "getStatus", "BDV_ZC");
}

void waitOnWalletRefresh(Clients* clients, const string& bdvId)
{
   waitOnSignal(clients, bdvId, "getStatus", "BDV_Refresh");
}

void triggerNewBlockNotification(BlockDataManagerThread* bdmt)
{
   auto nodePtr = bdmt->bdm()->networkNode_;
   auto nodeUnitTest = (NodeUnitTest*)nodePtr.get();

   nodeUnitTest->mockNewBlock();
}

struct ZcVector
{
   vector<Tx> zcVec_;

   void push_back(BinaryData rawZc, unsigned zcTime)
   {
      Tx zctx(rawZc);
      zctx.setTxTime(zcTime);

      zcVec_.push_back(move(zctx));
   }
};

void pushNewZc(BlockDataManagerThread* bdmt, const ZcVector& zcVec)
{
   auto zcConf = bdmt->bdm()->zeroConfCont_;

   ZeroConfContainer::ZcActionStruct newzcstruct;
   newzcstruct.action_ = Zc_NewTx;

   map<BinaryData, Tx> newzcmap;

   for (auto& newzc : zcVec.zcVec_)
   {
      auto&& zckey = zcConf->getNewZCkey();
      newzcmap[zckey] = newzc;
   }

   newzcstruct.setData(move(newzcmap));
   zcConf->newZcStack_.push_back(move(newzcstruct));
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
class DB1kIter : public ::testing::Test
{
protected:
   BlockDataManagerThread *theBDMt_;
   Clients* clients_;

   void initBDM(void)
   {
      ScrAddrFilter::init();
      theBDMt_ = new BlockDataManagerThread(config);
      iface_ = theBDMt_->bdm()->getIFace();

      auto mockedShutdown = [](void)->void {};
      clients_ = new Clients(theBDMt_, mockedShutdown);
   }

   /////////////////////////////////////////////////////////////////////////////
   virtual void SetUp()
   {
      LOGDISABLESTDOUT();
      magic_ = READHEX(MAINNET_MAGIC_BYTES);
      ghash_ = READHEX(MAINNET_GENESIS_HASH_HEX);
      gentx_ = READHEX(MAINNET_GENESIS_TX_HASH_HEX);
      zeros_ = READHEX("00000000");

      blkdir_ = string("./blkfiletest");
      homedir_ = string("./fakehomedir");
      ldbdir_ = string("./ldbtestdir");

      rmdir(blkdir_);
      rmdir(homedir_);
      rmdir(ldbdir_);

      mkdir(blkdir_);
      mkdir(homedir_);
      mkdir(ldbdir_);

      // Put the first 5 blocks into the blkdir
      blk0dat_ = BtcUtils::getBlkFilename(blkdir_, 0);
      setBlocks({ "0", "1", "2", "3", "4", "5" }, blk0dat_);

      config.armoryDbType_ = ARMORY_DB_BARE;
      config.blkFileLocation_ = blkdir_;
      config.dbDir_ = ldbdir_;
      config.threadCount_ = 3;

      config.genesisBlockHash_ = ghash_;
      config.genesisTxHash_ = gentx_;
      config.magicBytes_ = magic_;
      config.nodeType_ = Node_UnitTest;

      wallet1id = BinaryData("wallet1");
      wallet2id = BinaryData("wallet2");
      LB1ID = BinaryData(TestChain::lb1B58ID);
      LB2ID = BinaryData(TestChain::lb2B58ID);

      initBDM();
   }

   /////////////////////////////////////////////////////////////////////////////
   virtual void TearDown(void)
   {
      if (clients_ != nullptr)
      {
         clients_->exitRequestLoop();
         clients_->shutdown();
      }

      delete clients_;
      delete theBDMt_;

      theBDMt_ = nullptr;
      clients_ = nullptr;

      rmdir(blkdir_);
      rmdir(homedir_);

#ifdef _MSC_VER
      rmdir("./ldbtestdir");
      mkdir("./ldbtestdir");
#else
      string delstr = ldbdir_ + "/*";
      rmdir(delstr);
#endif
      LOGENABLESTDOUT();
      CLEANUP_ALL_TIMERS();
   }

   BlockDataManagerConfig config;

   LMDBBlockDatabase* iface_;
   BinaryData magic_;
   BinaryData ghash_;
   BinaryData gentx_;
   BinaryData zeros_;

   string blkdir_;
   string homedir_;
   string ldbdir_;
   string blk0dat_;

   BinaryData wallet1id;
   BinaryData wallet2id;
   BinaryData LB1ID;
   BinaryData LB2ID;
};

////////////////////////////////////////////////////////////////////////////////
TEST_F(DB1kIter, DbInit1kIter)
{
   theBDMt_->start(config.initMode_);
   auto&& bdvID = registerBDV(clients_, magic_);

   vector<BinaryData> scrAddrVec;
   scrAddrVec.push_back(TestChain::scrAddrA);
   scrAddrVec.push_back(TestChain::scrAddrB);
   scrAddrVec.push_back(TestChain::scrAddrC);
   scrAddrVec.push_back(TestChain::scrAddrD);
   scrAddrVec.push_back(TestChain::scrAddrE);
   scrAddrVec.push_back(TestChain::scrAddrF);

   const vector<BinaryData> lb1ScrAddrs
   {
      TestChain::lb1ScrAddr,
      TestChain::lb1ScrAddrP2SH
   };
   const vector<BinaryData> lb2ScrAddrs
   {
      TestChain::lb2ScrAddr,
      TestChain::lb2ScrAddrP2SH
   };

   regWallet(clients_, bdvID, scrAddrVec, "wallet1");
   regLockbox(clients_, bdvID, lb1ScrAddrs, TestChain::lb1B58ID);
   regLockbox(clients_, bdvID, lb2ScrAddrs, TestChain::lb2B58ID);

   //wait on signals
   goOnline(clients_, bdvID);
   waitOnBDMReady(clients_, bdvID);

   clients_->exitRequestLoop();
   clients_->shutdown();

   delete clients_;
   delete theBDMt_;

   auto fakeprog = [](BDMPhase, double, unsigned, unsigned)->void
   {};

   for (unsigned i = 0; i<1000; i++)
   {
      cout << "iter: " << i << endl;
      initBDM();
      auto bdm = theBDMt_->bdm();
      bdm->doInitialSyncOnLoad_Rebuild(fakeprog);

      clients_->exitRequestLoop();
      clients_->shutdown();

      delete clients_;
      delete theBDMt_;
   }

   //one last init so that TearDown doesn't blow up
   initBDM();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// Now actually execute all the tests
////////////////////////////////////////////////////////////////////////////////
GTEST_API_ int main(int argc, char **argv)
{
#ifdef _MSC_VER
   _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

   WSADATA wsaData;
   WORD wVersion = MAKEWORD(2, 0);
   WSAStartup(wVersion, &wsaData);
#endif

   DataMeta::initTypeMap();

   std::cout << "Running main() from gtest_main.cc\n";

   // Setup the log file 
   STARTLOGGING("cppTestsLog.txt", LogLvlDebug2);
   //LOGDISABLESTDOUT();

   testing::InitGoogleTest(&argc, argv);
   int exitCode = RUN_ALL_TESTS();

   FLUSHLOG();
   CLEANUPLOG();

   return exitCode;
}
