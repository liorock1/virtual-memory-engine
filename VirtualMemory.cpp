#include "VirtualMemory.h"
#include "MemoryConstants_test1.h"
#include "PhysicalMemory.h"
#include <cstdint>
#include <algorithm>

#define ZERO 0

/* ===================================================================== */
/*                                 HELPERS                               */
/* ===================================================================== */

/**
 * Convert the pair (frame #, row inside that frame) into the physical word address that PMread/PMwrite expect.
 **/
static inline uint64_t phys(uint64_t frame, uint64_t row)
{
    return frame*PAGE_SIZE + row;
}

/**
 * all rows in the page are between 0 and PAGE_SIZE -1 and because all the PAGE_SIZE are in power of 2
 * Subtracting 1 from a power of two turns every low bit into 1.
 * that's why each va's(virtual-adders) bit that is the offsetOf range and up will stay up
 **/
static inline uint64_t offsetOf(uint64_t va)
{
    return va & (PAGE_SIZE -1);
}

/**
 * this func will return the specific row/node in the level/table that we need to look in.
 * for example if we need to look for the the node in level 2 , we will put the virtual address & num_level
 * and get num of node that that written in the  virtual address.
 **/
static inline uint64_t indexAtLevel(uint64_t va, int level)
{
    int k = OFFSET_WIDTH + OFFSET_WIDTH * (TABLES_DEPTH - 1 - level);
    // number of bits till the level we need is in the offset area
    return (va >> k) & (PAGE_SIZE - 1);
    // shift left k times till the level we need is in the offset area and we put zero elsewhere.
}

/**
 * clearing the frame -turn it into an empty page table or an all-zero data page.
 * Only clears if not a leaf (table frame).
 **/
static void clearFrame(uint64_t frame, bool isLeaf) // CHANGED
{
    if (isLeaf) return; // CHANGED
    for (uint64_t i = 0; i < PAGE_SIZE; ++i)
        PMwrite(phys(frame,i), 0);
}

/* ===================================================================== */
/*                       DFS SCAN  (helper for allocator)                */
/* ===================================================================== */

/*note that UINT64_MAX is a MARKER, mean that it  signals “nothing yet” or “not found.”
 * we use it because  is a value that cannot be mistaken for any legal data.
 * Virtual page numbers range from 0 to NUM_PAGES-1. < 2^64.
 */
struct  scanInfo
{
    // priority 1 ->first empty table we see
    uint64_t emptyFrame  = UINT64_MAX;      //frame id of that table
    uint64_t emptyParent = UINT64_MAX;      //frame id of its parent table
    uint64_t emptyRowInParent = UINT64_MAX; //row in parent that points to it

    //priority 2 -> highest-index frame ever referenced
    uint64_t maxFrame = ZERO;

    //priority 3 -> the best eviction candidate
    uint64_t victimFrame = UINT64_MAX; //frame that holds the data page
    uint64_t victimParent= UINT64_MAX; // it's parent table's frame
    uint64_t victimRowInParent = ZERO; //the row that hold the frame addr in parent
    uint64_t victimPage = ZERO; //virtual-page number of the victim;
    uint64_t victimDistance = ZERO; //cyclic distance to the page we need
};

/**
 * this func is the way we decide how to choose the victim.
 * @return the distance on the NUM-PAGES-sized circle.
 */
static inline uint64_t cyclicDistance(uint64_t a,uint64_t b)
{
    uint64_t diff = (a > b) ? (a-b) : (b-a);
    return std::min<uint64_t>(diff, NUM_PAGES - diff);
}

/**
 * Recursively scans the page-table tree.
 * scan only fills the ScanInfo struct.  It never allocates or evicts by itself.
 * The decision happens later in allocateFrame, which reads the filled struct and returns the chosen frame to walk.
 *
 * @param frame         curr frame we are visiting.
 * @param depth         ZER0 = root,TABLES_DEPTH = leaf = data-page(we never call on data page).
 * @param pagePrefix    already constructed high bits of the virtual page number.
 * @param targetPage    page that triggered the allocation (for cyclic dist.).
 * @param info          in/out accumulator for results.
 */
static void scan(uint64_t frame,int depth,uint64_t pagePrefix,uint64_t targetPage,scanInfo &info,uint64_t parentFrame)
{
    bool allZero = true;

    /* iterate over every row in the current table */
    for (uint64_t row = 0; row < PAGE_SIZE; ++row)
    {
        word_t entry;
        PMread(phys(frame,row),&entry);

        if(entry == ZERO) continue;

        allZero = false; //not an empty table

        info.maxFrame = std::max<uint64_t>(info.maxFrame,(uint64_t)entry); //update if needed the maxFrame

        //newPrefix is how the DFS “grows” the virtual-page number as it moves one level deeper in the page-table tree.
        uint64_t newPrefix = (pagePrefix << OFFSET_WIDTH) | row;

        if( depth + 1 < TABLES_DEPTH) //means that were not in the data-page level, o we recursively repeat scan
        {
            scan(entry,depth+1,newPrefix,targetPage,info,parentFrame);
        }
        else // depth + 1 == TABLES_DEPTH means we are in data-page level
        {
            uint64_t dist = cyclicDistance(newPrefix,targetPage);

            if(dist >info.victimDistance)
            {
                info.victimDistance = dist; //this is the max cyclicDistance
                info.victimFrame = entry; //is the num of the frame physically holds the data page we may evict.
                info.victimPage = newPrefix; //the full virtual page number of the page in that frame.
                info.victimRowInParent = row; //is the index inside that parent table whose cell contained entry.
                info.victimParent = frame; // is the table we are currently scanning—the parent of the data page.
            }
        }
    }

    if(allZero && frame == parentFrame)
    {
        return;
    }
    /* if ALL entries were 0 and this table is not the root, remember it */
    if(allZero && frame != ZERO && info.emptyFrame == UINT64_MAX)
    {
        info.emptyFrame        = frame;
        info.emptyParent       = UINT64_MAX;   // we’ll fill these below
        info.emptyRowInParent  = 0; // just for now;
    }

    /* Fill parent/row for first empty table lazily (cheapest place) */
    if(info.emptyFrame != UINT64_MAX && info.emptyParent == UINT64_MAX )
    {
        // We are in the parent of that frame if any row points to emptyFrame
        for(uint64_t row = ZERO; row < PAGE_SIZE ; row++)
        {
            word_t entry;
            PMread(phys(frame, row), &entry);

            if((uint64_t)entry == info.emptyFrame)
            {
                info.emptyParent = frame;
                info.emptyRowInParent = row;
                break;
            }
        }
    }
}

/* ===================================================================== */
/*                      ALLOCATE FRAME  (spec compliant)                 */
/* ===================================================================== */

/**
 *
 * @param parentFrame
 * @param ParentRow
 * @param targetPage
 * @param isLeaf      true if the frame is for a leaf (data page)
 * @return the new frame well going to use by the data we keep in the info and by the priority were set to us.
 */
static uint64_t allocateFrame(uint64_t parentFrame, uint64_t ParentRow, uint64_t targetPage, bool isLeaf) // CHANGED
{
    scanInfo info;
    scan(0,0,0,targetPage,info,parentFrame);

    /* ---------- Priority #1 : reuse an empty table ------------------ */
    if(info.emptyFrame != UINT64_MAX &&  info.emptyFrame != parentFrame)
    {
        /* detach it from its parent */
        PMwrite(phys(info.emptyParent, info.emptyRowInParent), 0); //parent now does not point on any table.
        clearFrame(info.emptyFrame, isLeaf); // CHANGED
        return info.emptyFrame;
    }

    /* ---------- Priority #2 : take a brand-new frame ---------------- */
    if(info.maxFrame  + 1 < NUM_FRAMES ) // means that at least one frame is free to be used
    {
        uint64_t newFrame = info.maxFrame  + 1;
        clearFrame(newFrame, isLeaf); // CHANGED
        return newFrame;
    }

    /* ---------- Priority #3 : evict victim page --------------------- */
    /* victimFrame, victimParent, victimRowInParent guaranteed valid */
    PMevict(info.victimFrame, info.victimPage); //evicting the frame_number from the specific data_page.
    PMwrite(phys(info.victimParent, info.victimRowInParent), 0); // now its parent doesn't point to any table.
    clearFrame(info.victimFrame, isLeaf); // CHANGED
    return info.victimFrame;
}

/* ===================================================================== */
/*                      INTERNAL PAGE-TABLE WALKER                       */
/* ===================================================================== */
/**
 * this function is for walking func for the VMread/write, if we need to allocate new frame than we call it
 * with our allocateFrame func witch with our scan func decide how to allocate it.
 *
 * @param va        the virtual address
 * @param create    if false → stop + return false on first missing pointer
 *                  if true  → allocate frames/tables on demand
 * @param leafFrame_out (out) frame number that contains the data page
 * @return          true on success, false on unmapped page when create==false
 */
static bool walk(uint64_t va, bool create, uint64_t &leafFrame_out)
{
    uint64_t frame = ZERO; // root

    for(int level = 0; level < TABLES_DEPTH; ++level)
    {
        uint64_t row;
        word_t   child;

        row = indexAtLevel(va,level);
        PMread(phys(frame,row), &child);
        if (child == 0) // page fault on this row
        {
            if (!create) { return false; } // VMread without create

            /* allocate a usable frame according to the three priorities */
            bool isLeaf = (level + 1 == TABLES_DEPTH); // CHANGED
            uint64_t newFrame = allocateFrame(frame,row,va >> OFFSET_WIDTH,isLeaf); // CHANGED

            if(isLeaf) // means that we are in the data_page level
            {
                PMrestore(newFrame, va >> OFFSET_WIDTH);  // bring page from swap
            }
            else // intermediate TABLE
            {
                clearFrame(newFrame, false); //empty the table
            }

            PMwrite(phys(frame, row), newFrame);//we link it to the parent.
            child = newFrame;
        }
        frame = child; //descend to the next_level.
    }
    leafFrame_out = frame;  // reached the data page
    return true;
}

/* ===================================================================== */
/*                      PUBLIC READ / WRITE API                          */
/* ===================================================================== */

/**
 * We Initialize 0 in each row in the first frame of the PM.
 * that's for marking frame 0, and set that he not point to any other page or table.
 **/
void VMinitialize()
{
    clearFrame(0, false); // root lives in frame 0 forever
}

/**
 * reads a word from the given virtual address and puts its content in *value.
 * @param virtualAddress
 * @param value
 * @return 1 for success 0 for failure
 */
int VMread(uint64_t virtualAddress, word_t *value)
{
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE) { return ZERO; }

    uint64_t leafFrame;

    walk(virtualAddress, true, leafFrame);

    PMread(phys(leafFrame, offsetOf(virtualAddress)), value);
    return 1;
}

/**
 * writes a word to the given virtual address
 * @param virtualAddress
 * @param value
 * @return 1 for success 0 for failure
 */
int VMwrite(uint64_t virtualAddress, word_t value)
{

    if (virtualAddress >= VIRTUAL_MEMORY_SIZE) { return ZERO; }
    /* (A) Translate the address and CREATE pages on demand */
    uint64_t leafFrame;
    walk(virtualAddress, true, leafFrame);
    /* (B) Write the value into physical memory */
    PMwrite( phys(leafFrame, offsetOf(virtualAddress)), value );

    return 1;
}