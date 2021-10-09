/* This program creates a pinned cache-friendly physical page */
/* mapping, and *tries* to also get a decent TLB mapping. */

/* THIS PROGRAM REQUIRES you to have CAP_SYS_ADMIN permission for */
/* reading data from /proc/self/pagemap. Unfortunately, the "safest" */
/* way to get that permission at the moment is likely sudo. */

/* A future Linux user-space CMA interface would obsolete this code. */

/* Author: Jeff Keasler */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

/* set to 1 to turn on allocation info message */
#define ECHO_QUALITY 0


/*  !0: Use posix_memalign allocation if */
/*      the cache-friendly technique fails */
#define FAILOVER_ALLOCATION 0


/*   0: posix_memalign allocator */
/*  !0: mmap allocator */
#define USE_MMAP_ALLOCATION 0


/* Pi 4 BCM2711 has */
/* 32 KB 2-way L1, 1 MB 16-way L2 */
/* FOLLOWING VALUE MUST BE A POWER OF 2, SO ROUND UP */
#define CACHE_GRANULARITY  (1024*1024)


/* Huge page size is 2MB by default, so this is a hint. */
/* MUST BE AT LEAST AS LARGE AS "#define CACHE_GRANULARITY" */
#define BLOCK_ALIGN (2*1024*1024)


/* Hard limit on attempts to allocate a "good" block */
#define MAX_ATTEMPT 1024


/*===============================================*/

/* Allocate a *pinned* cache-friendly memory block. */

/* if allocSize < pageSize, it will be set to pageSize */

/* This operation is very slow and should only be used */
/* to allocate persistent storage, e.g. a memory pool, */
/* that will be reused throughout the entire run. */


void *allocate_phys_mem(uint32_t allocSize)
{
  static void **blockArray ;
  static uint32_t pageSize ;
  static uint32_t cacheFriendlyFactor ;

  char pageFrame[8] ; /* buffer for one pageFrame */

  void *physBlock = 0 ; /* return value */

  FILE *pagemap_fp ; /* to read /proc/self/pagemap */

  uint32_t availPhysPages = sysconf(_SC_AVPHYS_PAGES) ;
  int numTrials ;
  int trial = 0 ;
  int bestBlock = -1 ;
  int bestGapCount = -1 ;

  if (pageSize == 0) {
    pageSize = sysconf(_SC_PAGESIZE) ;

    cacheFriendlyFactor = (CACHE_GRANULARITY < 2*pageSize) ?
                          1 : (CACHE_GRANULARITY/pageSize - 1) ;

    /* This memory is never deallocated */
    blockArray = (void **) malloc(MAX_ATTEMPT*sizeof(void *)) ;
  }

  /* sentinel in case an error causes early exit. */
  blockArray[0] = 0 ;

  /* round up request to nearest physical page size */
  if ((allocSize & (pageSize-1)) != 0) {
    allocSize = (allocSize & ~(pageSize-1)) + pageSize ;
  }

  if (allocSize < CACHE_GRANULARITY) {
    allocSize = CACHE_GRANULARITY ;
  }

  if (allocSize < pageSize) {
    allocSize = pageSize ;
  }

  /* Grab at most 3/4 of available memory to find "best" block */
  numTrials = (availPhysPages/(allocSize/pageSize))*3/4 ;

  if (numTrials > MAX_ATTEMPT) {
    /* guard blockArray from overflow */
    numTrials = MAX_ATTEMPT ;
  }

  pagemap_fp = fopen("/proc/self/pagemap", "rb") ;
  if (pagemap_fp == NULL) {
    /*  /proc/self/pagemap is disabled  */
    goto error_exit ;
  }

  /* get "best" contiguous physically mapped section of memory */

  for (trial=0; trial<numTrials; ++trial)
  {
    uint32_t pagemap_offset ;
    uint32_t prev_page, curr_page ; /* uint32_t works for 16 TB+ */
    const int allocPages = allocSize / pageSize ;
    int page ;
    int pageGaps = 0 ;

#if USE_MMAP_ALLOCATION
    blockArray[trial] = mmap((void *) 0, allocSize, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED |
                             MAP_NORESERVE | MAP_POPULATE, -1, (off_t) 0) ;

    if (blockArray[trial] == MAP_FAILED) {
      /* Assume we have broken mmap */
      blockArray[trial] = 0 ; /* set sentinel */
      break ;
    }
#else
    posix_memalign(&blockArray[trial], BLOCK_ALIGN , allocSize) ;
    if (blockArray[trial] == 0) {
      /* Assume we have broken posix_memalign() */
      break ;
    }
#endif

    if (mlock(blockArray[trial], allocSize)) {
      /* Assume we have somehow broken mlock */
      break ;
    }

    pagemap_offset = ((uintptr_t) blockArray[trial]) / pageSize * 8 ;
    fseek(pagemap_fp, pagemap_offset, SEEK_SET) ;
    for (page=0; page<allocPages; ++page) {
      fread(pageFrame, 8, 1, pagemap_fp) ;
      if (page != 0) {
        curr_page = * ((uint32_t *) pageFrame) ; /* good for up to 16 TB */
        if (curr_page != prev_page+1) {

          /* see if the pages are cache "compatible" when page gaps exist */
          uint32_t check = (prev_page+1) | curr_page ;
          if ( (cacheFriendlyFactor & check) != 0) {
            break ;  /* not (compatibly) contiguous! */
          }
          ++pageGaps ; /* still cache friendly, so continue */
        }
        prev_page = curr_page ;
      }
      else {
        prev_page = * ((uint32_t *) pageFrame) ;
        if (prev_page == 0) {
          /*  /proc/self/pagemap is disabled (no sudo) */
          goto error_exit ;
        }
      }
    }

    if (page == allocPages) {
      if (bestBlock == -1 || pageGaps < bestGapCount) {
        physBlock = blockArray[trial] ;
        bestBlock = trial ;
        bestGapCount = pageGaps ;
        if (bestGapCount == 0) {
          break ;
        }
      }
    }
    else { /* This allocation was not cache friendly */ }
  }

error_exit:

  if (pagemap_fp != NULL) {
    if (fclose(pagemap_fp)) {
      printf("couldn't close /proc/self/pagemap\n") ;
      exit(-1) ;
    }
  }

  if (trial == numTrials) {
    --trial ;
  }

  /* clean up */

  for (int unwind = trial ; unwind >= 0; --unwind) {
    if ((unwind != bestBlock) && (blockArray[unwind] != 0)) {
      if (munlock(blockArray[unwind], allocSize)) {
        /* silently ignore munlock errors for now */
      }
#if USE_MMAP_ALLOCATION
      munmap(blockArray[unwind], allocSize) ;
#else
      free(blockArray[unwind]) ;
#endif
    }
  }

#if ECHO_QUALITY
  if (physBlock != 0) {
   printf("Best allocation chosen from %d queries\n", trial) ;
   printf("virt addr=%lx (%d bytes).\n", physBlock, allocSize) ;
   printf("Pinned phys mem with %d 'gaps' out of %d virtual pages mapped.\n",
          bestGapCount, allocSize/pageSize) ;
   printf("Gaps were adjusted so caches still behave like contiguous mem.\n");
  }
#endif

#if FAILOVER_ALLOCATION
  if (physBlock == 0) {
    posix_memalign((void **) &physBlock, BLOCK_ALIGN , allocSize) ;
  }
#endif

  return physBlock ;
}

/*----------------------------------------------*/

/* This function MUST be used to unwind alloc_phys_mem() */

int free_phys_mem(void *physBlock, uint32_t allocSize)
{
  int err = munlock(physBlock, allocSize) ;

#if USE_MMAP_ALLOCATION
  err |= munmap(physBlock, allocSize) ;
#else
  free(physBlock) ;
#endif

  return err ;
}

/*===============================================*/
/*                Driver code                    */
/*===============================================*/

int main(int argc, char *argv[])
{
  int allocSize = 256*1024*1024 ;  /* 256 MB */

  void *physBlock =  allocate_phys_mem(allocSize);

  if (physBlock == 0) {
    printf("failed to allocate a cache-friendly memory block\n") ;
  }
  else {
    if (free_phys_mem(physBlock, allocSize)) {
      printf("trouble freeing memory\n") ;
    }
  }

  return 0 ;
}
