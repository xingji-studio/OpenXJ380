/*------------------------------------------------------------------------*/
/* A Sample Code of User Provided OS Dependent Functions for FatFs        */
/*------------------------------------------------------------------------*/

#include "fs/fatfs/ff.h"
#include <fs/fatfs/private.h>

#if FF_USE_LFN == 3 /* Use dynamic memory allocation */

/*------------------------------------------------------------------------*/
/* Allocate/Free a Memory Block                                           */
/*------------------------------------------------------------------------*/

void *ff_memalloc(           /* Returns pointer to the allocated memory block (null if not enough core) */
                  uint msize /* Number of bytes to allocate */
)
{
    // Original Apache-2.0 port addition; existing FatFs sample terms are retained.
    // Original port hook: factory sessions own bounded slabs before any I/O.
    // Exhaustion must not fall back to a heap allocation after publication.
    bool handled = false;
    void *storage = fatfs_esp_memalloc(msize, &handled);
    return handled ? storage : malloc((size_t)msize); /* Existing ordinary allocation. */
}

void ff_memfree(void *mblock /* Pointer to the memory block to free (no effect if null) */
)
{
    // Original Apache-2.0 port addition; existing FatFs sample terms are retained.
    if (!fatfs_esp_memfree(mblock)) free(mblock); /* Existing ordinary release. */
}

#endif

#if FF_FS_REENTRANT /* Mutal exclusion */
/*------------------------------------------------------------------------*/
/* Definitions of Mutex                                                   */
/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/
/* Create a Mutex                                                         */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount function to create a new mutex
/  or semaphore for the volume. When a 0 is returned, the f_mount function
/  fails with FR_INT_ERR.
*/

int ff_mutex_create(        /* Returns 1:Function succeeded or 0:Could not create the mutex */
                    int vol /* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
    return 1;
}

/*------------------------------------------------------------------------*/
/* Delete a Mutex                                                         */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount function to delete a mutex or
/  semaphore of the volume created with ff_mutex_create function.
*/

void ff_mutex_delete(        /* Returns 1:Function succeeded or 0:Could not delete due to an error */
                     int vol /* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
}

/*------------------------------------------------------------------------*/
/* Request a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/* This function is called on enter file functions to lock the volume.
/  When a 0 is returned, the file function fails with FR_TIMEOUT.
*/

int ff_mutex_take(        /* Returns 1:Succeeded or 0:Timeout */
                  int vol /* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
}

/*------------------------------------------------------------------------*/
/* Release a Grant to Access the Volume                                   */
/*------------------------------------------------------------------------*/
/* This function is called on leave file functions to unlock the volume.
 */

void ff_mutex_give(int vol /* Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system mutex (FF_VOLUMES) */
)
{
}

#endif /* FF_FS_REENTRANT */
