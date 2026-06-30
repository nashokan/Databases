#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <iostream>
#include <stdio.h>
#include "page.h"
#include "buf.h"

#define ASSERT(c)  { if (!(c)) { \
		       cerr << "At line " << __LINE__ << ":" << endl << "  "; \
                       cerr << "This condition should hold: " #c << endl; \
                       exit(1); \
		     } \
                   }
S
//----------------------------------------
// Constructor of the class BufMgr
//----------------------------------------

BufMgr::BufMgr(const int bufs)
{
    numBufs = bufs;

    bufTable = new BufDesc[bufs];
    memset(bufTable, 0, bufs * sizeof(BufDesc));
    for (int i = 0; i < bufs; i++) 
    {
        bufTable[i].frameNo = i;
        bufTable[i].valid = false;
    }

    bufPool = new Page[bufs];
    memset(bufPool, 0, bufs * sizeof(Page));

    int htsize = ((((int) (bufs * 1.2))*2)/2)+1;
    hashTable = new BufHashTbl (htsize);  // allocate the buffer hash table

    clockHand = bufs - 1;
}


BufMgr::~BufMgr() {

    // flush out all unwritten pages
    for (int i = 0; i < numBufs; i++) 
    {
        BufDesc* tmpbuf = &bufTable[i];
        if (tmpbuf->valid == true && tmpbuf->dirty == true) {

#ifdef DEBUGBUF
            cout << "flushing page " << tmpbuf->pageNo
                 << " from frame " << i << endl;
#endif

            tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[i]));
        }
    }

    delete [] bufTable;
    delete [] bufPool;
}


const Status BufMgr::allocBuf(int & frame) 
{
   for(int i = 0; i < numBufs * 2; i++) {

        //Check the pin count of the frame and move to next frame if pin count is above 0
        if (bufTable[clockHand].pinCnt > 0) {
            advanceClock();
            continue;
        }

        //If pin count is 0 check the refbit of the frame and if it's true set it to false and move to next frame
        if (bufTable[clockHand].refbit) {
            bufTable[clockHand].refbit = false;
            advanceClock();
            continue;
        }

        //Check if the clockHand is pointing at a valid page in the hashtable
        if (bufTable[clockHand].valid) {
            //Check if the dirty bit is set
            if (bufTable[clockHand].dirty) {
                Status write = bufTable[clockHand].file->writePage(bufTable[clockHand].pageNo, &bufPool[clockHand]);
                if (write != OK) {
                    return UNIXERR;
                }
            }

            //Remove the page from the hash table
            hashTable->remove(bufTable[clockHand].file, bufTable[clockHand].pageNo);
        }

        //Clear the frame, set frame number and return OK status code
        bufTable[clockHand].Clear();
        frame = clockHand;
        advanceClock();
        return OK;
    }

    //Return status code when all frames are pinned
    return BUFFEREXCEEDED;
}

	
const Status BufMgr::readPage(File* file, const int PageNo, Page*& page)
{
    int frameNo;
    Status status;

    bufStats.accesses++;

    //check if page is already in the buffer pool
    status=hashTable->lookup(file, PageNo, frameNo);
    if (status==OK) {
        bufTable[frameNo].pinCnt++;
        bufTable[frameNo].refbit=true;
        page=&bufPool[frameNo];
        return OK;
    }

    //allocate a new frame
    status=allocBuf(frameNo);
    if (status!=OK)
        return status;

    //read the page from disk into the buffer pool frame
    status=file->readPage(PageNo, &bufPool[frameNo]);
    if (status!=OK) {
        bufTable[frameNo].Clear();
        bufTable[frameNo].frameNo=frameNo;
        bufTable[frameNo].refbit=false;
        return status;
    }

    bufStats.diskreads++;

    //set up the frame and insert into hash table
    bufTable[frameNo].Set(file, PageNo);
    status=hashTable->insert(file, PageNo, frameNo);
    if (status != OK) {
        bufTable[frameNo].Clear();
        bufTable[frameNo].frameNo=frameNo;
        bufTable[frameNo].refbit=false;
        return status;
    }

    page=&bufPool[frameNo];
    return OK;
}


const Status BufMgr::unPinPage(File* file, const int PageNo, 
			       const bool dirty) 
{
    int frameNo;
    Status status;

    status = hashTable->lookup(file, PageNo, frameNo);
    if (status != OK)
        return status;

    BufDesc* desc = &bufTable[frameNo];

    if (desc->pinCnt == 0)
        return PAGENOTPINNED;

    desc->pinCnt--;

    if (dirty)
        desc->dirty = true;

    return OK;
}

const Status BufMgr::allocPage(File* file, int& pageNo, Page*& page) 
{
   // allocate an empty page in the specified file
   Status status = file->allocatePage(pageNo);
   if (status != OK) return status;

   // obtain a buffer pool frame
   int frameNo;
   status = allocBuf(frameNo);
   if (status != OK) return status;

   // insert entry into hash table mapping (file, pageNo) to frameNo
   status = hashTable->insert(file, pageNo, frameNo);
   if (status != OK) return status;

   // set up the frame descriptor for the new page
   bufTable[frameNo].Set(file, pageNo);
   page = &bufPool[frameNo];
   bufStats.accesses++;

   return OK;
}

const Status BufMgr::disposePage(File* file, const int pageNo) 
{
    // see if it is in the buffer pool
    Status status = OK;
    int frameNo = 0;
    status = hashTable->lookup(file, pageNo, frameNo);
    if (status == OK)
    {
        // clear the page
        bufTable[frameNo].Clear();
    }
    status = hashTable->remove(file, pageNo);

    // deallocate it in the file
    return file->disposePage(pageNo);
}

const Status BufMgr::flushFile(const File* file) 
{
  Status status;

  for (int i = 0; i < numBufs; i++) {
    BufDesc* tmpbuf = &(bufTable[i]);
    if (tmpbuf->valid == true && tmpbuf->file == file) {

      if (tmpbuf->pinCnt > 0)
	  return PAGEPINNED;

      if (tmpbuf->dirty == true) {
#ifdef DEBUGBUF
	cout << "flushing page " << tmpbuf->pageNo
             << " from frame " << i << endl;
#endif
	if ((status = tmpbuf->file->writePage(tmpbuf->pageNo,
					      &(bufPool[i]))) != OK)
	  return status;

	tmpbuf->dirty = false;
      }

      hashTable->remove(file,tmpbuf->pageNo);

      tmpbuf->file = NULL;
      tmpbuf->pageNo = -1;
      tmpbuf->valid = false;
    }

    else if (tmpbuf->valid == false && tmpbuf->file == file)
      return BADBUFFER;
  }
  
  return OK;
}


void BufMgr::printSelf(void) 
{
    BufDesc* tmpbuf;
  
    cout << endl << "Print buffer...\n";
    for (int i=0; i<numBufs; i++) {
        tmpbuf = &(bufTable[i]);
        cout << i << "\t" << (char*)(&bufPool[i]) 
             << "\tpinCnt: " << tmpbuf->pinCnt;
    
        if (tmpbuf->valid == true)
            cout << "\tvalid\n";
        cout << endl;
    };
}
