/*
    File:       MFSLivesPseudoMount.c

    Contains:   BSD-level user space code to access MFS disks.

    Written by: DTS

    Copyright:  Copyright (c) 2006 by Apple Computer, Inc., All Rights Reserved.

    Disclaimer: IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
                ("Apple") in consideration of your agreement to the following terms, and your
                use, installation, modification or redistribution of this Apple software
                constitutes acceptance of these terms.  If you do not agree with these terms,
                please do not use, install, modify or redistribute this Apple software.

                In consideration of your agreement to abide by the following terms, and subject
                to these terms, Apple grants you a personal, non-exclusive license, under Apple's
                copyrights in this original Apple software (the "Apple Software"), to use,
                reproduce, modify and redistribute the Apple Software, with or without
                modifications, in source and/or binary forms; provided that if you redistribute
                the Apple Software in its entirety and without modifications, you must retain
                this notice and the following text and disclaimers in all such redistributions of
                the Apple Software.  Neither the name, trademarks, service marks or logos of
                Apple Computer, Inc. may be used to endorse or promote products derived from the
                Apple Software without specific prior written permission from Apple.  Except as
                expressly stated in this notice, no other rights or licenses, express or implied,
                are granted by Apple herein, including but not limited to any patent rights that
                may be infringed by your derivative works or by other works in which the Apple
                Software may be incorporated.

                The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
                WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
                WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
                PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
                COMBINATION WITH YOUR PRODUCTS.

                IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
                CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
                GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
                ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR DISTRIBUTION
                OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF CONTRACT, TORT
                (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN
                ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    Change History (most recent first):

$Log: MFSLivesPseudoMount.c,v $
Revision 1.1  2006/07/27 15:48:08  eskimo1
First checked in.


*/

#include "MFSLivesPseudoMount.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/xattr.h>

#include <libkern/OSByteOrder.h>    /** OSReadBigInt32() OSSwapBigToHostInt32() */

#include "MFSCore.h"

/////////////////////////////////////////////////////////////////////

static FILE *   gLog;

extern void MFSPMountSetLogFile(FILE *logFile)
{
    gLog = logFile;
}

// MFSPMount is used to hold the state of an MFS 'volume' that we've 'mounted'. 

struct MFSPMount {
    char *          mapAddr;                        // address of the data in memory
    size_t          mapSize;                        // size of the above
    bool            mapped;                         // whether it's mmap'd or malloc'd
    size_t          blockSize;                      // device block size; we require 512
    size_t          mdbAndVABMSizeInBytes;          // info returned by MFSMDBCheck
    uint16_t        directoryStartBlock;            // ditto
    uint16_t        directoryBlockCount;            // ditto
    uint16_t        allocationBlocksStartBlock;     // ditto
    uint32_t        allocationBlockSizeInBytes;     // ditto
};
typedef struct MFSPMount MFSPMount;

static bool IsDiskCopy42Image(int fd)
    // Returns true if the file specified by fd is a Disk Copy 4.2-style 
    // disk image.
{
    int             err;
    bool            result;
    ssize_t         attrSize;
    uint8_t         finderInfo[32];
    const char *    lastDot;
    char            containerPath[MAXPATHLEN];
    
    assert(fd >= 0);
    
    result = false;
    
    // If there is a Finder info extended attribute and the file type in there is 
    // 'dImg', we have a Disk Copy 4.2 image.
    
    attrSize = fgetxattr(fd, XATTR_FINDERINFO_NAME, finderInfo, sizeof(finderInfo), 0, 0);
    if (attrSize == sizeof(finderInfo)) {
        if ( OSReadBigInt32(&finderInfo[0], 0) == 'dImg' ) {
            if (gLog != NULL) fprintf(gLog, "[%ld]     IsDiskCopy42Image -> true (file type)\n", (long) getpid());
            result = true;
        }
    }

    // If not, let's base our decision purely on the extension.
    
    if ( ! result ) {
        containerPath[0] = 0;
        
        err = fcntl(fd, F_GETPATH, containerPath);
        if (err < 0) {
            err = errno;
        }
        if (gLog != NULL) fprintf(gLog, "[%ld]     fcntl F_GETPATH -> %d, '%s'\n", (long) getpid(), err, containerPath);
        
        if (err == 0) {
            lastDot = strrchr(containerPath, '.');
            if (lastDot != NULL) {
                if ( strcmp(lastDot, ".img") == 0 ) {
                    if (gLog != NULL) fprintf(gLog, "[%ld]     IsDiskCopy42Image -> true (extension)\n", (long) getpid());
                    result = true;
                }
            }
        }
    }

    if ( ! result ) {
        if (gLog != NULL) fprintf(gLog, "[%ld]     IsDiskCopy42Image -> false\n", (long) getpid());
    }
    
    return result;
}

static int GetContainerInfo(int fd, off_t *containerOffsetPtr, size_t *containerSizePtr, size_t *blockSizePtr)
    // Returns information about the thing that contains the MFS volume's data. 
    // This might be a file (a raw disk image or a Disk Copy 4.2 disk image) or 
    // a device (either cooked or raw).  What you get back is the offset within 
    // the container, the size of the data from that offset, and the block size.
{
    int             err;
    struct stat     sb;
    uint32_t        containerSizeFromDiskImage;
    ssize_t         bytesRead;
    
    assert(fd >= 0);
    assert(containerOffsetPtr != NULL);
    assert(containerSizePtr != NULL);
    assert(blockSizePtr != NULL);
    
    *containerOffsetPtr = 0;
    
    // Get information about the container.
    
    err = fstat(fd, &sb);
    if (err < 0) {
        err = errno;
    }
    
    // Do different things depending on whether it's a device or a file.
    
    if (err == 0) {
        switch (sb.st_mode & S_IFMT) {
            case S_IFCHR:
            case S_IFBLK:
                {
                    uint32_t        blockSize;
                    uint64_t        blockCount;
                    
                    // If it's a device, use ioctls to get the block size and count.
                    
                    blockSize  = 0;
                    blockCount = 0;
                    
                    err = ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize);
                    if (err < 0) {
                        err = errno;
                    } else {
                        *blockSizePtr = blockSize;
                    }
                    if (gLog != NULL) fprintf(gLog, "[%ld]       ioctl DKIOCGETBLOCKSIZE -> %d, %lu\n", (long) getpid(), err, (unsigned long) blockSize);
                    
                    if (err == 0) {
                        err = ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount);
                        if (err < 0) {
                            err = errno;
                        } else {
                            *containerSizePtr = blockCount * blockSize;
                            if ( ((uint64_t) *containerSizePtr) != (blockCount * blockSize) ) {
                                err = EFBIG;
                            }
                        }
                        if (gLog != NULL) fprintf(gLog, "[%ld]       ioctl DKIOCGETBLOCKCOUNT -> %d, %llu\n", (long) getpid(), err, (unsigned long long) blockCount);
                    }
                }
                break;
            case S_IFREG:
                // If it's a file, we always assume a block size of 512 bytes.

                *blockSizePtr = 512;
                
                // However, the size and offset depends on whether it's a Disk Copy 4.2 
                // disk image or a raw disk image.  In the former case, the image has a 
                // header with a fixed size of 84 bytes, and the image size is contained 
                // at a fixed offset of 64 bytes into that header.  In the latter case, 
                // we assume that the container consists of the entire file.

                if ( IsDiskCopy42Image(fd) ) {
                    *containerOffsetPtr = 84;
                    
                    bytesRead = pread(fd, &containerSizeFromDiskImage, sizeof(containerSizeFromDiskImage), 64);
                    if (bytesRead < 0) {
                        err = errno;
                    } else if (bytesRead != sizeof(containerSizeFromDiskImage)) {
                        fprintf(stderr, "GetContainerInfo: Short read (%llu, %llu).", (long long) bytesRead, (long long) sizeof(containerSizeFromDiskImage));
                        err = ECANCELED;
                    }
                    if (err == 0) {
                        *containerSizePtr = OSSwapBigToHostInt32(containerSizeFromDiskImage);
                    }
                } else {
                    *containerSizePtr = sb.st_size;
                    if ( ((off_t) *containerSizePtr) != sb.st_size ) {
                        err = EFBIG;
                    }
                }
                break;
            default:
                fprintf(stderr, "Invalid file type.");
                err = ECANCELED;
                break;
        }
    }
    
    return err;
}

extern int MFSPMountCreate(const char *containerPath, MFSPMountRef *pmountPtr)
    // See comment in header.
{
    int             err;
    int             junk;
    int             fd;
    off_t           offset;
    MFSPMountRef    pmount;

    assert(containerPath != NULL);
    assert( pmountPtr != NULL);
    assert(*pmountPtr == NULL);
    
    if (gLog != NULL) fprintf(gLog, "[%ld]   MFSPMountCreate '%s'\n", (long) getpid(), containerPath);

    // Prepare for failure.
    
    fd = -1;
    
    // Create a blank pmount.
    
    err = 0;
    pmount = calloc(1, sizeof(*pmount));
    if (pmount == NULL) {
        err = ENOMEM;
    } else {
        pmount->mapAddr = MAP_FAILED;
        pmount->mapped  = true;
    }
    
    // Open up the container.

    if (err == 0) {
        fd = open(containerPath, O_RDONLY);
        if (fd < 0) {
            err = errno;
        }
        if (gLog != NULL) fprintf(gLog, "[%ld]     open '%s' -> %d\n", (long) getpid(), containerPath, err);
    }
    
    // Get information about the container, and check it for reasonableness.
    
    if (err == 0) {
        err = GetContainerInfo(fd, &offset, &pmount->mapSize, &pmount->blockSize);
        if (gLog != NULL) fprintf(gLog, "[%ld]     GetContainerInfo '%s' -> %d, %llu, %zu, %zu\n", (long) getpid(), containerPath, err, offset, pmount->mapSize, pmount->blockSize);
    }
    if ( (err == 0) && (pmount->mapSize == 0) ) {
        fprintf(stderr, "Container size must be non-zero.\n");
        err = ECANCELED;
    }
    if ( (err == 0) && ((pmount->mapSize & (512 - 1)) != 0) ) {
        fprintf(stderr, "Container size must be a multiple of 512.\n");
        err = ECANCELED;
    }
    if ( (err == 0) && (pmount->blockSize != 512) ) {
        fprintf(stderr, "Container block size must be 512.\n");
        err = ECANCELED;
    }
    
    // Memory map the container; if that fails (which it will on a cooked or raw 
    // disk device), allocate a buffer and read the contents of the container into 
    // that buffer.
    
    if (err == 0) {
        pmount->mapAddr = mmap(NULL, pmount->mapSize, PROT_READ, MAP_FILE, fd, offset);
        if (pmount->mapAddr == MAP_FAILED) {
            err = errno;
        }
        if (gLog != NULL) fprintf(gLog, "[%ld]     mmap -> %d\n", (long) getpid(), err);
        
        if (err != 0) {
            pmount->mapped = false;
            
            err = 0;
            pmount->mapAddr = malloc(pmount->mapSize);
            if (pmount->mapAddr == NULL) {
                err = ENOMEM;
            }
            
            if (err == 0) {
                ssize_t bytesRead;
                
                bytesRead = pread(fd, pmount->mapAddr, pmount->mapSize, offset);
                if (bytesRead < 0) {
                    err = errno;
                } else if (bytesRead != pmount->mapSize) {
                    fprintf(stderr, "MFSPMountCreate: Short read (%llu, %llu).", (long long) bytesRead, (long long) pmount->mapSize);
                    err = ECANCELED;
                }
                if (gLog != NULL) fprintf(gLog, "[%ld]     read -> %d\n", (long) getpid(), err);
            }
        }
    }
    
    // Call the MFS core code to check that this is, indeed, an MFS volume, and 
    // to get important information about the volume.
    
    if (err == 0) {
        err = MFSMDBCheck(
            pmount->mapAddr + (kMFSMDBBlock * pmount->blockSize),
            pmount->mapSize / pmount->blockSize,
            &pmount->mdbAndVABMSizeInBytes,
            &pmount->directoryStartBlock,
            &pmount->directoryBlockCount,
            &pmount->allocationBlocksStartBlock,
            &pmount->allocationBlockSizeInBytes
        );
        
        if (err == EINVAL) {
            char errStr[256];

            MFSMDBGetError(pmount->mapAddr + (kMFSMDBBlock * pmount->blockSize), pmount->mapSize / pmount->blockSize, errStr, sizeof(errStr));
            if (gLog != NULL) {
                fprintf(gLog, "[%ld]     MFSMDBGetError -> %s\n", (long) getpid(), errStr);
            }
            fprintf(stderr, "Not an MFS disk (%s)\n", errStr);
            err = ECANCELED;
        } else {
            if (gLog != NULL) {
                fprintf(gLog, "[%ld]     MFSMDBCheck -> %d, %zu, %d, %d, %d, %lu\n", (long) getpid(), err, 
                    pmount->mdbAndVABMSizeInBytes,
                    (int) pmount->directoryStartBlock,
                    (int) pmount->directoryBlockCount,
                    (int) pmount->allocationBlocksStartBlock,
                    (unsigned long) pmount->allocationBlockSizeInBytes
                );
            }
        }
    }

    // Clean up.
    
    if (fd != -1) {
        junk = close(fd);
        assert(junk == 0);
    }
    if (err != 0) {
        MFSPMountDestroy(pmount);
        pmount = NULL;
    }
    assert( (err != 0) || ( (pmount->mapAddr != NULL) && (pmount->mapAddr != MAP_FAILED) ) );
    if (err == 0) {
        *pmountPtr = pmount;
    }

    if (gLog != NULL) fprintf(gLog, "[%ld]   MFSPMountCreate -> %d\n", (long) getpid(), err);
    
    return err;
}

extern void MFSPMountDestroy(MFSPMountRef pmount)
    // See comment in header.
{
    int     junk;
    
    if (pmount != NULL) {
        if (pmount->mapped) {
            if (pmount->mapAddr != MAP_FAILED) {
                junk = munmap(pmount->mapAddr, pmount->mapSize);
                assert(junk == 0);
            }
        } else {
            free(pmount->mapAddr);
        }
        free(pmount);
    }
}

extern const void * MFSPMountGetMDBVABM(MFSPMountRef pmount)
    // See comment in header.
{
    return pmount->mapAddr + (kMFSMDBBlock * pmount->blockSize);
}

extern int MFSPMountListFiles(MFSPMountRef pmount, MFSPMountFileInfo files[], size_t filesSize, size_t *fileCountPtr)
    // See comment in header.
{
    int                 err;
    uint16_t            dirBlock;
    size_t              dirOffset;
    size_t              fileCount;

    if (gLog != NULL) fprintf(gLog, "[%ld]   MFSPMountListFiles %zu\n", (long) getpid(), filesSize);
    
    assert(pmount != NULL);
    assert( (filesSize == 0) || (files != NULL) );
    assert(fileCountPtr != NULL);

    fileCount = 0;
    
    // Iterate through each directory block.
    
    err = 0;
    for (dirBlock = pmount->directoryStartBlock; dirBlock < (pmount->directoryStartBlock + pmount->directoryBlockCount); dirBlock++) {
        const char *    thisDirBlockPtr;

        if (gLog != NULL) fprintf(gLog, "[%ld]     dirBlock %d", (long) getpid(), (int) dirBlock);
        
        thisDirBlockPtr = pmount->mapAddr + (dirBlock * pmount->blockSize);
        
        dirOffset = kMFSDirectoryBlockIterateFromStart;
        do {

            // Find the next directory entry, and gather all of the necessary information about it.
            
            err = MFSDirectoryBlockIterate(
                thisDirBlockPtr,
                pmount->blockSize,
                &dirOffset,
                NULL
            );
            if (gLog != NULL) fprintf(gLog, " (%d, %zu)", err, dirOffset);

            // Now that we have everything we need to know about this directory entry, 
            // let's record it in the files array.
            
            if (err == 0) {
                if (fileCount < filesSize) {
                    files[fileCount].dirBlockPtr = thisDirBlockPtr;
                    files[fileCount].dirOffset   = dirOffset;
                }
                fileCount += 1;
            }
        } while (err == 0);

        if (gLog != NULL) fprintf(gLog, "\n");

        if (err == ENOENT) {
            // We ran off the end of this directory block, so swallow the error and 
            // let's go to the next directory block.
            err = 0;
        } else {
            // Any other error causes us to bail out.  It would be very weird if 
            // this was no error.
            assert(err != 0);
            break;
        }
    }

    *fileCountPtr = fileCount;

    if (gLog != NULL) fprintf(gLog, "[%ld]   MFSPMountListFiles -> %d, %zu\n", (long) getpid(), err, fileCount);
    
    return err;
}

typedef int (*ExtentCallback)(void *refCon, const void *extent, size_t extentSize);
    // Callback for IteratorExtents.  extent is a pointer to this extent's data. 
    // extentSize is the size of that data.  Return an errno-style error.  Returning 
    // a non-zero value will terminate extent iteration and propagate the error 
    // up from IteratorExtents.

static int IteratorExtents(
    MFSPMountRef    pmount, 
    uint16_t        dirBlock, 
    size_t          dirOffset, 
    size_t          forkIndex, 
    ExtentCallback  callback, 
    void *          refCon
)
    // For each of the extents of the forkIndex'th fork of the file whose directory is 
    // at dirOffset within dirBlock, call the callback.
{
    int             err;
    MFSForkInfo     forkInfo;
    uint32_t        forkOffset;
    uint32_t        offsetFromFirstAllocationBlockInBytes;
    uint32_t        contiguousPhysicalBytes;

    assert(pmount != NULL);
    assert( (dirBlock >= pmount->directoryStartBlock) && (dirBlock < (pmount->directoryStartBlock + pmount->directoryBlockCount)) );
    assert(dirOffset < pmount->blockSize);
    assert(forkIndex <= 1);
    assert(callback != NULL);

    // Get information about the fork.
    
    err = MFSDirectoryEntryGetForkInfo(pmount->mapAddr + (dirBlock * pmount->blockSize), dirOffset, forkIndex, &forkInfo);

    // Iterate each entry.  Note that MFSForkGetExtent errors if the fork 
    // has no data at all, so don't call it in that case.
    
    if ( (err == 0) && (forkInfo.lengthInBytes > 0) ) {
        forkOffset = 0;
        
        do {
            err = MFSForkGetExtent(
                pmount->mapAddr + (kMFSMDBBlock * pmount->blockSize),
                &forkInfo,
                forkOffset,
                &offsetFromFirstAllocationBlockInBytes,
                &contiguousPhysicalBytes
            );
            
            if (err == 0) {
                size_t  extentSize;
                
                // Trim the extent size to the logical file length (as opposed to 
                // contiguousPhysicalBytes, which is the physical length of the 
                // extent).
                
                extentSize = (forkInfo.lengthInBytes - forkOffset);
                if (extentSize > contiguousPhysicalBytes) {
                    extentSize = contiguousPhysicalBytes;
                }
                err = callback(refCon, pmount->mapAddr + (pmount->allocationBlocksStartBlock * pmount->blockSize) + offsetFromFirstAllocationBlockInBytes, extentSize);
                if (gLog != NULL) fprintf(gLog, "[%ld]     extent %lu %zu -> %d\n", (long) getpid(), (unsigned long) forkOffset, extentSize, err);
            }
            if (err == 0) {
                forkOffset += contiguousPhysicalBytes;
            }
        } while ( (err == 0) && (forkOffset < forkInfo.lengthInBytes) );
    }
    return err;
}

static int DataForkExtentCallback(void *refCon, const void *extent, size_t extentSize)
    // An IterateExtents callback used to extract the data fork of a file. 
    // refCon is the file descriptor of the destination file.
{
    int         err;
    int         fd;
    ssize_t     bytesWritten;
    
    assert(extent != NULL);
    assert(extentSize > 0);
    
    fd = (int) (intptr_t) refCon;
    assert(fd >= 0);
    
    bytesWritten = write(fd, extent, extentSize);
    if (bytesWritten < 0) {
        err = errno;
    } else if (bytesWritten != extentSize) {
        fprintf(stderr, "DataForkExtentCallback: Short write (%llu, %llu).", (long long) bytesWritten, (long long) extentSize);
        err = ECANCELED;
    } else {
        err = 0;
    }
    
    return err;
}

static int RsrcForkExtentCallback(void *refCon, const void *extent, size_t extentSize)
    // An IterateExtents callback used to extract the resource fork of a file. 
    // refCon is a pointer to a pointer to the next character in the resource fork 
    // buffer (see rsrcForkBuffer in ExtractFile).
{
    char **     cursorPtr;
    
    assert(extent != NULL);
    assert(extentSize > 0);

    cursorPtr = (char **) refCon;
    assert( cursorPtr != NULL);
    assert(*cursorPtr != NULL);
    
    memcpy(*cursorPtr, extent, extentSize);
    *cursorPtr += extentSize;
    
    return 0;
}

static int ExtractFile(MFSPMountRef pmount, uint16_t dirBlock, size_t dirOffset, const char *destPath)
    // Extract the file whose directory entry is at dirOffset within dirBlock into a file 
    // to be created at destPath.  The destination file must not exist.
{
    int                 err;
    int                 fd;
    int                 junk;
    MFSForkInfo         rsrcForkInfo;
    char *              rsrcForkBuffer;
    char *              rsrcForkCursor;
    uint8_t             finderInfo[32];
    static const uint8_t kEmptyFinderInfo[32];
    bool                didCreate;

    assert(pmount != NULL);
    assert( (dirBlock >= pmount->directoryStartBlock) && (dirBlock < (pmount->directoryStartBlock + pmount->directoryBlockCount)) );
    assert(dirOffset < pmount->blockSize);
    assert(destPath != NULL);
    
    rsrcForkBuffer = NULL;
    
    // Create the file.  I don't support overwriting because I don't want to go to 
    // the trouble of clearing out any existing forks or metadata.
    
    err = 0;
    fd = open(destPath, O_RDWR | O_CREAT | O_EXCL, DEFFILEMODE);
    if (fd < 0) {
        err = errno;
    }
    didCreate = (err == 0);
    if (gLog != NULL) fprintf(gLog, "[%ld]     open '%s' -> %d\n", (long) getpid(), destPath, err);
    
    // Data fork
    
    if (err == 0) {
        if (gLog != NULL) fprintf(gLog, "[%ld]     data fork\n", (long) getpid());
        err = IteratorExtents(pmount, dirBlock, dirOffset, 0, DataForkExtentCallback, (void *) (intptr_t) fd);
    }
    
    // Resource fork

    // The supported way for BSD-level code to set the resource fork is via [f]setxattr. 
    // Opening up "destPath/..namedfork/rsrc" would have been easier, but we actively 
    // recommend against that approach.
    
    // Accumulate the resource fork contents in RAM so that we can write it out with one 
    // fsetxattr call.  This will make the atomic, which is generally a good thing.  
    // The memory requirements aren't a big issue because a resource fork is limited to 
    // (roughly) 16 MB.
    
    if (err == 0) {
        err = MFSDirectoryEntryGetForkInfo(pmount->mapAddr + (dirBlock * pmount->blockSize), dirOffset, 1, &rsrcForkInfo);
    }
    if ( (err == 0) && (rsrcForkInfo.lengthInBytes != 0) ) {
        if (gLog != NULL) fprintf(gLog, "[%ld]     rsrc fork\n", (long) getpid());

        rsrcForkBuffer = malloc(rsrcForkInfo.lengthInBytes);
        if (rsrcForkBuffer == NULL) {
            err = ENOMEM;
        }

        if (err == 0) {
            rsrcForkCursor = rsrcForkBuffer;
            
            err = IteratorExtents(pmount, dirBlock, dirOffset, 1, RsrcForkExtentCallback, &rsrcForkCursor);     
            
            assert( (err != 0) || (rsrcForkCursor == (rsrcForkBuffer + rsrcForkInfo.lengthInBytes)) );
        }
        if (err == 0) {
            err = fsetxattr(fd, XATTR_RESOURCEFORK_NAME, rsrcForkBuffer, rsrcForkInfo.lengthInBytes, 0, 0);
            if (err < 0) {
                err = errno;
            }
            if (gLog != NULL) fprintf(gLog, "[%ld]       fsetxattr -> %d\n", (long) getpid(), err);
        }
    }
    
    // Finder info
    
    // The supported way for BSD-level code to set the Finder info is via [f]setxattr.

    if (err == 0) {
        memset(finderInfo, 0, sizeof(finderInfo));
        
        err = MFSDirectoryEntryGetFinderInfo(pmount->mapAddr + (dirBlock * pmount->blockSize), dirOffset, finderInfo);
    }
    if ( (err == 0) & (memcmp(finderInfo, kEmptyFinderInfo, sizeof(finderInfo)) != 0) ) {
        if (gLog != NULL) fprintf(gLog, "[%ld]     Finder info\n", (long) getpid());

        err = fsetxattr(fd, XATTR_FINDERINFO_NAME, finderInfo, sizeof(finderInfo), 0, 0);
        if (err < 0) {
            err = errno;
        }
        if (gLog != NULL) fprintf(gLog, "[%ld]       fsetxattr -> %d\n", (long) getpid(), err);
    }   
    
    // Dates

    // The only way to set a file's creation date is via setattrlist.  Given that 
    // I'm already in bed with that call, I might as well use it to set the 
    // modification date at the same time.
    
    if (err == 0) {
        struct vnode_attr   attr;
        struct attrlist attrList;
        struct {
            struct timespec createTime;
            struct timespec modifyTime;
        } attrBuf;

        if (gLog != NULL) fprintf(gLog, "[%ld]     dates\n", (long) getpid());

        // Get the creation and modification date from MFS core.
        
        VATTR_INIT(&attr);
        VATTR_WANTED(&attr, va_create_time);
        VATTR_WANTED(&attr, va_modify_time);
        
        err = MFSDirectoryEntryGetAttr(pmount->mapAddr + (dirBlock * pmount->blockSize), dirOffset, &attr);

        // Set them for the destination file.
        
        if (err == 0) {
            memset(&attrList, 0, sizeof(attrList));
            attrList.bitmapcount = 5;
            attrList.commonattr = ATTR_CMN_CRTIME | ATTR_CMN_MODTIME;

            attrBuf.createTime = attr.va_create_time;
            attrBuf.modifyTime = attr.va_modify_time;
            
            err = setattrlist(destPath, &attrList, &attrBuf, sizeof(attrBuf), 0);           // Why is there no fsetattrlist? <rdar://problem/3570921>
            if (err < 0) {
                err = errno;
            }
            if (gLog != NULL) fprintf(gLog, "[%ld]       setattrlist -> %d\n", (long) getpid(), err);
        }
        
        // Throw away any error; we just don't care if this fails, and it may well 
        // fail for odd reasons (for example, on file systems that don't support 
        // backup dates).
        
        err = 0;
    }

    // Clean up
    
    free(rsrcForkBuffer);
    if (fd >= 0) {
        junk = close(fd);
        assert(junk == 0);
    }
    if ( (err != 0) && didCreate ) {
        junk = unlink(destPath);        // Why is there no funlink?
        assert(junk == 0);
    }
    
    return err;
}

extern int MFSPMountExtractFile(MFSPMountRef pmount, const char *fileName, const char *outputFilePath)
    // See comment in header.
{
    int                 err;
    uint16_t            dirBlock;
    size_t              dirOffset;
    char *              tempBuffer;
    const char *        createdFilePath;

    assert(pmount != NULL);
    assert(fileName != NULL);
    // outputFilePath may be NULL

    if (gLog != NULL) fprintf(gLog, "[%ld]   MFSPMountExtractFile '%s' '%s'\n", (long) getpid(), fileName, ((outputFilePath != NULL) ? outputFilePath : "") );

    // Prepare for failure.
    
    tempBuffer = NULL;
    
    // Initialise the MFS core.

    err = 0;
    
    // Search the directory for the requested file.  Start by allocating a 
    // temporary buffer for use by MFSDirectoryBlockFindEntryByName.
    
    if (err == 0) {
        tempBuffer = malloc(kMFSDirectoryBlockFindEntryByNameTempBufferSize);
        if (tempBuffer == NULL) {
            err = ENOMEM;
        }
    }
    if (err == 0) {
        // MFSDirectoryBlockFindEntryByName requires the first byte of the buffer 
        // be zero on the first call.
        
        *tempBuffer = 0;

        dirBlock = pmount->directoryStartBlock;
        dirOffset = 0;              // to make the logging pretty in the case of an error

        do {
            if (dirBlock >= (pmount->directoryStartBlock + pmount->directoryBlockCount)) {
                err = ENOENT;
                break;
            }
            
            if (gLog != NULL) fprintf(gLog, "[%ld]     dirBlock %d\n", (long) getpid(), (int) dirBlock);
            
            err = MFSDirectoryBlockFindEntryByName(
                pmount->mapAddr + (dirBlock * pmount->blockSize),
                pmount->blockSize,
                fileName,
                strlen(fileName),
                tempBuffer,
                &dirOffset,
                NULL
            );

            if (err != ENOENT) {
                break;
            }
            dirBlock += 1;
        } while (true);
        
        if (gLog != NULL) fprintf(gLog, "[%ld]     dirOffset %d %zu\n", (long) getpid(), err, dirOffset);
    }
    
    // Create the extracted file.
    
    if (err == 0) {
        if (outputFilePath != NULL) {
            createdFilePath = outputFilePath;
        } else {
            createdFilePath = fileName;
        }

        err = ExtractFile(pmount, dirBlock, dirOffset, createdFilePath);
    }
    
    // Clean up.
    
    free(tempBuffer);

    if (gLog != NULL) fprintf(gLog, "[%ld]   MFSPMountExtractFile -> %d\n", (long) getpid(), err);
    
    return err;
}

