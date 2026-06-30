#include "heapfile.h"
#include "error.h"

// routine to create a heapfile
const Status createHeapFile(const string fileName)
{
    File* 		file;
    Status 		status;
    FileHdrPage*	hdrPage;
    int			hdrPageNo;
    int			newPageNo;
    Page*		newPage;

    // try to open the file. This should return an error
    status = db.openFile(fileName, file);
    if (status != OK)
    {
		// file doesn't exist. First create it and allocate
		// an empty header page and data page.

		// Step 1: Create the file on disk
		status = db.createFile(fileName);
		if (status != OK) return status;

		// Step 2: Open the newly created file
		status = db.openFile(fileName, file);
		if (status != OK) return status;

		// Step 3: Allocate the header page
		status = bufMgr->allocPage(file, hdrPageNo, newPage);
		if (status != OK) return status;

		// Cast the page to a FileHdrPage and initialize it
		hdrPage = (FileHdrPage*) newPage;
		strncpy(hdrPage->fileName, fileName.c_str(), MAXNAMESIZE);
		hdrPage->pageCnt = 0;
		hdrPage->recCnt = 0;

		// Step 4: Allocate the first data page
		status = bufMgr->allocPage(file, newPageNo, newPage);
		if (status != OK) return status;

		// Initialize the data page
		newPage->init(newPageNo);
		newPage->setNextPage(-1);

		// Step 5: Update header page with first/last page info
		hdrPage->firstPage = newPageNo;
		hdrPage->lastPage = newPageNo;
		hdrPage->pageCnt = 1;

		// Step 6: Unpin both pages, marking them dirty
		status = bufMgr->unPinPage(file, hdrPageNo, true);
		if (status != OK) return status;

		status = bufMgr->unPinPage(file, newPageNo, true);
		if (status != OK) return status;

		// Step 7: Close the file
		status = db.closeFile(file);
		if (status != OK) return status;

		return OK;
    }
    return (FILEEXISTS);
}

// routine to destroy a heapfile
const Status destroyHeapFile(const string fileName)
{
	return (db.destroyFile (fileName));
}

// constructor opens the underlying file
HeapFile::HeapFile(const string & fileName, Status& returnStatus)
{
    Status 	status;
    Page*	pagePtr;

    cout << "opening file " << fileName << endl;

    // open the file and read in the header page and the first data page
    if ((status = db.openFile(fileName, filePtr)) == OK)
    {
		// Step 1: Get the page number of the header page
		int hdrPageNo;
		status = filePtr->getFirstPage(hdrPageNo);
		if (status != OK) {
			returnStatus = status;
			return;
		}

		// Step 2: Read and pin the header page
		status = bufMgr->readPage(filePtr, hdrPageNo, pagePtr);
		if (status != OK) {
			returnStatus = status;
			return;
		}

		// Initialize header page data members
		headerPage = (FileHdrPage*) pagePtr;
		headerPageNo = hdrPageNo;
		hdrDirtyFlag = false;

		// Step 3: Read and pin the first data page
		curPageNo = headerPage->firstPage;
		status = bufMgr->readPage(filePtr, curPageNo, curPage);
		if (status != OK) {
			// Unpin header page before returning
			bufMgr->unPinPage(filePtr, headerPageNo, hdrDirtyFlag);
			returnStatus = status;
			return;
		}
		curDirtyFlag = false;
		curRec = NULLRID;

		returnStatus = OK;
    }
    else
    {
    	cerr << "open of heap file failed\n";
		returnStatus = status;
		return;
    }
}

// the destructor closes the file
HeapFile::~HeapFile()
{
    Status status;
    cout << "invoking heapfile destructor on file " << headerPage->fileName << endl;

    // see if there is a pinned data page. If so, unpin it 
    if (curPage != NULL)
    {
    	status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
		curPage = NULL;
		curPageNo = 0;
		curDirtyFlag = false;
		if (status != OK) cerr << "error in unpin of date page\n";
    }
	
	 // unpin the header page
    status = bufMgr->unPinPage(filePtr, headerPageNo, hdrDirtyFlag);
    if (status != OK) cerr << "error in unpin of header page\n";
	
	// status = bufMgr->flushFile(filePtr);  // make sure all pages of the file are flushed to disk
	// if (status != OK) cerr << "error in flushFile call\n";
	// before close the file
	status = db.closeFile(filePtr);
    if (status != OK)
    {
		cerr << "error in closefile call\n";
		Error e;
		e.print (status);
    }
}

// Return number of records in heap file

const int HeapFile::getRecCnt() const
{
  return headerPage->recCnt;
}

// retrieve an arbitrary record from a file.
// if record is not on the currently pinned page, the current page
// is unpinned and the required page is read into the buffer pool
// and pinned.  returns a pointer to the record via the rec parameter

const Status HeapFile::getRecord(const RID & rid, Record & rec)
{
    Status status;

    // cout<< "getRecord. record (" << rid.pageNo << "." << rid.slotNo << ")" << endl;

    // Check if the desired record is on the currently pinned page
    if (curPage != NULL && curPageNo == rid.pageNo)
    {
        // Record is on the current page
        status = curPage->getRecord(rid, rec);
        if (status != OK) return status;
        curRec = rid;
        return OK;
    }
    else
    {
        // Need to unpin current page and read the correct page
        if (curPage != NULL)
        {
            status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
            if (status != OK) return status;
            curPage = NULL;
        }

        // Read the page containing the desired record
        curPageNo = rid.pageNo;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK) return status;
        curDirtyFlag = false;

        // Now get the record from the page
        status = curPage->getRecord(rid, rec);
        if (status != OK) return status;
        curRec = rid;
        return OK;
    }
}

HeapFileScan::HeapFileScan(const string & name,
			   Status & status) : HeapFile(name, status)
{
    filter = NULL;
}

const Status HeapFileScan::startScan(const int offset_,
				     const int length_,
				     const Datatype type_, 
				     const char* filter_,
				     const Operator op_)
{
    if (!filter_) {                        // no filtering requested
        filter = NULL;
        return OK;
    }
    
    if ((offset_ < 0 || length_ < 1) ||
        (type_ != STRING && type_ != INTEGER && type_ != FLOAT) ||
        (type_ == INTEGER && length_ != sizeof(int)
         || type_ == FLOAT && length_ != sizeof(float)) ||
        (op_ != LT && op_ != LTE && op_ != EQ && op_ != GTE && op_ != GT && op_ != NE))
    {
        return BADSCANPARM;
    }

    offset = offset_;
    length = length_;
    type = type_;
    filter = filter_;
    op = op_;

    return OK;
}


const Status HeapFileScan::endScan()
{
    Status status;
    // generally must unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = NULL;
        curPageNo = 0;
		curDirtyFlag = false;
        return status;
    }
    return OK;
}

HeapFileScan::~HeapFileScan()
{
    endScan();
}

const Status HeapFileScan::markScan()
{
    // make a snapshot of the state of the scan
    markedPageNo = curPageNo;
    markedRec = curRec;
    return OK;
}

const Status HeapFileScan::resetScan()
{
    Status status;
    if (markedPageNo != curPageNo) 
    {
		if (curPage != NULL)
		{
			status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
			if (status != OK) return status;
		}
		// restore curPageNo and curRec values
		curPageNo = markedPageNo;
		curRec = markedRec;
		// then read the page
		status = bufMgr->readPage(filePtr, curPageNo, curPage);
		if (status != OK) return status;
		curDirtyFlag = false; // it will be clean
    }
    else curRec = markedRec;
    return OK;
}


const Status HeapFileScan::scanNext(RID& outRid)
{
    Status 	status = OK;
    RID		nextRid;
    RID		tmpRid;
    int 	nextPageNo;
    Record      rec;

    // If curPage is NULL, read the first data page
    if (curPage == NULL)
    {
        curPageNo = headerPage->firstPage;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK) return status;
        curDirtyFlag = false;
        curRec = NULLRID;
    }

    // Start scanning from current position
    while (true)
    {
        // If curRec is NULLRID, get the first record on the current page
        if (curRec.pageNo == -1 && curRec.slotNo == -1)
        {
            status = curPage->firstRecord(tmpRid);
        }
        else
        {
            // Get the next record after curRec
            status = curPage->nextRecord(curRec, tmpRid);
        }

        // Process records on this page
        while (status == OK)
        {
            // Get the record data to check the predicate
            status = curPage->getRecord(tmpRid, rec);
            if (status != OK) return status;

            // Check if the record matches the filter
            if (matchRec(rec))
            {
                // Found a matching record
                curRec = tmpRid;
                outRid = curRec;
                return OK;
            }

            // Move to the next record on this page
            curRec = tmpRid;
            status = curPage->nextRecord(curRec, tmpRid);
        }

        // No more records on this page (ENDOFPAGE or NORECORDS)
        // Move to the next page
        status = curPage->getNextPage(nextPageNo);
        if (status != OK) return status;

        // Check if there is a next page
        if (nextPageNo == -1)
        {
            // No more pages - end of file
            return FILEEOF;
        }

        // Unpin the current page and read the next page
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        if (status != OK) return status;

        curPageNo = nextPageNo;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK) return status;
        curDirtyFlag = false;
        curRec = NULLRID;
    }
}


// returns pointer to the current record.  page is left pinned
// and the scan logic is required to unpin the page 

const Status HeapFileScan::getRecord(Record & rec)
{
    return curPage->getRecord(curRec, rec);
}

// delete record from file. 
const Status HeapFileScan::deleteRecord()
{
    Status status;

    // delete the "current" record from the page
    status = curPage->deleteRecord(curRec);
    curDirtyFlag = true;

    // reduce count of number of records in the file
    headerPage->recCnt--;
    hdrDirtyFlag = true; 
    return status;
}


// mark current page of scan dirty
const Status HeapFileScan::markDirty()
{
    curDirtyFlag = true;
    return OK;
}

const bool HeapFileScan::matchRec(const Record & rec) const
{
    // no filtering requested
    if (!filter) return true;

    // see if offset + length is beyond end of record
    // maybe this should be an error???
    if ((offset + length -1 ) >= rec.length)
	return false;

    float diff = 0;                       // < 0 if attr < fltr
    switch(type) {

    case INTEGER:
        int iattr, ifltr;                 // word-alignment problem possible
        memcpy(&iattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ifltr,
               filter,
               length);
        diff = iattr - ifltr;
        break;

    case FLOAT:
        float fattr, ffltr;               // word-alignment problem possible
        memcpy(&fattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ffltr,
               filter,
               length);
        diff = fattr - ffltr;
        break;

    case STRING:
        diff = strncmp((char *)rec.data + offset,
                       filter,
                       length);
        break;
    }

    switch(op) {
    case LT:  if (diff < 0.0) return true; break;
    case LTE: if (diff <= 0.0) return true; break;
    case EQ:  if (diff == 0.0) return true; break;
    case GTE: if (diff >= 0.0) return true; break;
    case GT:  if (diff > 0.0) return true; break;
    case NE:  if (diff != 0.0) return true; break;
    }

    return false;
}

InsertFileScan::InsertFileScan(const string & name,
                               Status & status) : HeapFile(name, status)
{
  //Do nothing. Heapfile constructor will bread the header page and the first
  // data page of the file into the buffer pool
}

InsertFileScan::~InsertFileScan()
{
    Status status;
    // unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, true);
        curPage = NULL;
        curPageNo = 0;
        if (status != OK) cerr << "error in unpin of data page\n";
    }
}

// Insert a record into the file
const Status InsertFileScan::insertRecord(const Record & rec, RID& outRid)
{
    Page*	newPage;
    int		newPageNo;
    Status	status, unpinstatus;
    RID		rid;

    // check for very large records
    if ((unsigned int) rec.length > PAGESIZE-DPFIXED)
    {
        // will never fit on a page, so don't even bother looking
        return INVALIDRECLEN;
    }

    // If curPage is NULL, read the last page into the buffer
    if (curPage == NULL)
    {
        curPageNo = headerPage->lastPage;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK) return status;
        curDirtyFlag = false;
    }

    // Try to insert the record into the current page
    status = curPage->insertRecord(rec, rid);

    if (status == OK)
    {
        // Record inserted successfully - do bookkeeping
        outRid = rid;
        curRec = rid;
        curDirtyFlag = true;
        headerPage->recCnt++;
        hdrDirtyFlag = true;
        return OK;
    }

    // Current page is full (NOSPACE) - need to allocate a new page
    // First, unpin the current page
    status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
    if (status != OK) return status;

    // Allocate a new page
    status = bufMgr->allocPage(filePtr, newPageNo, newPage);
    if (status != OK) return status;

    // Initialize the new page
    newPage->init(newPageNo);
    newPage->setNextPage(-1);

    // Link the old last page to the new page:
    // Read the old last page, set its nextPage, then unpin it
    Page* prevLastPage;
    status = bufMgr->readPage(filePtr, headerPage->lastPage, prevLastPage);
    if (status != OK) {
        bufMgr->unPinPage(filePtr, newPageNo, true);
        return status;
    }
    prevLastPage->setNextPage(newPageNo);
    status = bufMgr->unPinPage(filePtr, headerPage->lastPage, true);
    if (status != OK) {
        bufMgr->unPinPage(filePtr, newPageNo, true);
        return status;
    }

    // Update the header page
    headerPage->lastPage = newPageNo;
    headerPage->pageCnt++;
    hdrDirtyFlag = true;

    // Make the new page the current page
    curPage = newPage;
    curPageNo = newPageNo;
    curDirtyFlag = true;

    // Now insert the record into the new page
    status = curPage->insertRecord(rec, rid);
    if (status != OK) return status;

    // Bookkeeping
    outRid = rid;
    curRec = rid;
    headerPage->recCnt++;

    return OK;
}
