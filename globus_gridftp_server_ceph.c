/******************************************************************************
 * GridFTP plugin for access to ceph object store
 *
 * @author Sebastien Ponce, sebastien.ponce@cern.ch
 *****************************************************************************/
#if defined(linux)
#define _LARGE_FILES
#define __USE_LARGEFILE64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <zlib.h>
#include <sys/xattr.h>

#include "globus_gridftp_server.h"
#include "dsi_ceph.h"
#include "ceph_posix.h"

#define  CA_MAXCKSUMLEN 32
#define  CA_MAXCKSUMNAMELEN 15

static
globus_version_t local_version = {
  0, /* major version number */
  1, /* minor version number */
  1157544130,
  0 /* branch ID */
};

/*
 *  utility function to make errors
 */

static globus_result_t globus_l_gfs_make_error(const char *msg) {
  char *err_str;
  globus_result_t result;
  GlobusGFSName(globus_l_gfs_make_error);
  err_str = globus_common_create_string("%s error: %s", msg,  strerror(errno));
  result = GlobusGFSErrorGeneric(err_str);
  globus_free(err_str);
  return result;
}

/* fill the statbuf into globus_gfs_stat_t */
static void fill_stat_array(globus_gfs_stat_t * filestat, struct stat64 statbuf, char *name) {
  filestat->mode = statbuf.st_mode;;
  filestat->nlink = statbuf.st_nlink;
  filestat->uid = statbuf.st_uid;
  filestat->gid = statbuf.st_gid;
  filestat->size = statbuf.st_size;

  filestat->mtime = statbuf.st_mtime;
  filestat->atime = statbuf.st_atime;
  filestat->ctime = statbuf.st_ctime;

  filestat->dev = statbuf.st_dev;
  filestat->ino = statbuf.st_ino;
  filestat->name = strdup(name);
}
/* free memory in stat_array from globus_gfs_stat_t->name */
static void free_stat_array(globus_gfs_stat_t * filestat,int count) {
  int i;
  for(i=0;i<count;i++) free(filestat[i].name);
}

/* free memory for the checksum list */
static void free_checksum_list(checksum_block_list_t *checksum_list) {
  checksum_block_list_t *checksum_list_p;
  checksum_block_list_t *checksum_list_pp;
  checksum_list_p=checksum_list;
  while(checksum_list_p->next!=NULL){
    checksum_list_pp=checksum_list_p->next;
    globus_free(checksum_list_p);
    checksum_list_p=checksum_list_pp;
  }
  globus_free(checksum_list_p);
}

// comparison of 2 checksum_block_list_t* on their offset for the use of qsort
static int offsetComparison(const void *first, const void *second) {
  checksum_block_list_t** f = (checksum_block_list_t**)first;
  checksum_block_list_t** s = (checksum_block_list_t**)second;
  long long int diff = (*f)->offset - (*s)->offset;
  // Note that we cannot simply return diff as this function should return
  // an int and the cast for values not fitting in 32 bits may screw things
  if (0 == diff) return 0;
  if (diff > 0) return 1;
  return -1;
}

/* a replacement for zlib adler32_combine for SLC4  */
#define BASE 65521UL    /* largest prime smaller than 65536 */
#define MOD(a) a %= BASE

static unsigned long adler32_combine_(unsigned int adler1,
                                       unsigned int adler2,
                                       globus_off_t len2) {
  unsigned int sum1;
  unsigned int sum2;
  unsigned int rem;
  /* the derivation of this formula is left as an exercise for the reader */
  rem = (unsigned int)(len2 % BASE);
  sum1 = adler1 & 0xffff;
  sum2 = rem * sum1;
  MOD(sum2);
  sum1 += (adler2 & 0xffff) + BASE - 1;
  sum2 += ((adler1 >> 16) & 0xffff) + ((adler2 >> 16) & 0xffff) + BASE - rem;
  if (sum1 >= BASE) sum1 -= BASE;
  if (sum1 >= BASE) sum1 -= BASE;
  if (sum2 >= (BASE << 1)) sum2 -= (BASE << 1);
  if (sum2 >= BASE) sum2 -= BASE;
  return sum1 | (sum2 << 16);
}

static unsigned long adler32_0chunks(unsigned int len) {
  return ((len%BASE) << 16) | 1;
}

static void ceph_logfunc_wrapper (char *format, va_list argp) {
  // do the printing ourselves as we cannot call the variadic globus_gfs_log_message
  int size = 1024;
  char* logstr = (char*)malloc(size);
  int written = vsnprintf(logstr, size, format, argp);
  while (written >= size) {
    size *=2;
    logstr = (char*)realloc(logstr, size);
    written = vsnprintf(logstr, size, format, argp);
  }
  // call log func with a single argument
  (*globus_gfs_log_message)(GLOBUS_GFS_LOG_DUMP, "%s", logstr);
  free(logstr);
}

/* a function to wrap all is needed to close a file */
static void globus_ceph_close(const char* func,
                              globus_l_gfs_ceph_handle_t* ceph_handle,
                              const char* error_msg) {
  char* errorBuf = NULL;
  ceph_handle->done = GLOBUS_TRUE;
  ceph_posix_close(ceph_handle->fd);
  if (error_msg) {
    ceph_handle->cached_res = globus_l_gfs_make_error(error_msg);
    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "%s: terminating transfer on error: %s\n", func, error_msg);
    errorBuf = strdup(error_msg);
  }
  else {
    ceph_handle->cached_res = GLOBUS_SUCCESS;
  }
}

/*************************************************************************
 *  start
 *  -----
 *  This function is called when a new session is initialized, ie a user
 *  connectes to the server.  This hook gives the dsi an opportunity to
 *  set internal state that will be threaded through to all other
 *  function calls associated with this session.  And an oppertunity to
 *  reject the user.
 *
 *  finished_info.info.session.session_arg should be set to an DSI
 *  defined data structure.  This pointer will be passed as the void *
 *  user_arg parameter to all other interface functions.
 *
 *  NOTE: at nice wrapper function should exist that hides the details
 *        of the finished_info structure, but it currently does not.
 *        The DSI developer should jsut follow this template for now
 ************************************************************************/
static void globus_l_gfs_ceph_start(globus_gfs_operation_t op,
                                    globus_gfs_session_info_t *session_info) {
  globus_l_gfs_ceph_handle_t *ceph_handle;
  globus_gfs_finished_info_t finished_info;
  char *func="globus_l_gfs_ceph_start";

  GlobusGFSName(globus_l_gfs_ceph_start);
  ceph_handle = (globus_l_gfs_ceph_handle_t *)
    globus_malloc(sizeof(globus_l_gfs_ceph_handle_t));
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: started, uid: %u, gid: %u\n",
                         func, getuid(),getgid());
  globus_mutex_init(&ceph_handle->mutex,NULL);

  memset(&finished_info, '\0', sizeof(globus_gfs_finished_info_t));
  finished_info.type = GLOBUS_GFS_OP_SESSION_START;
  finished_info.result = GLOBUS_SUCCESS;
  finished_info.info.session.session_arg = ceph_handle;
  finished_info.info.session.username = session_info->username;
  finished_info.info.session.home_dir = NULL; /* if null we will go to HOME directory */

  ceph_handle->checksum_list=NULL;
  ceph_handle->checksum_list_p=NULL;
  globus_gridftp_server_operation_finished(op, GLOBUS_SUCCESS, &finished_info);
}

/*************************************************************************
 *  destroy
 *  -------
 *  This is called when a session ends, ie client quits or disconnects.
 *  The dsi should clean up all memory they associated with the session
 *  here.
 ************************************************************************/
static void globus_l_gfs_ceph_destroy(void *user_arg) {
  globus_l_gfs_ceph_handle_t *ceph_handle;
  ceph_handle = (globus_l_gfs_ceph_handle_t *) user_arg;
  globus_mutex_destroy(&ceph_handle->mutex);
  globus_free(ceph_handle);
}

/*************************************************************************
 *  stat
 *  ----
 *  This interface function is called whenever the server needs
 *  information about a given file or resource.  It is called then an
 *  LIST is sent by the client, when the server needs to verify that
 *  a file exists and has the proper permissions, etc.
 ************************************************************************/
static void globus_l_gfs_ceph_stat(globus_gfs_operation_t op,
                                   globus_gfs_stat_info_t *stat_info,
                                   void *user_arg) {
  globus_gfs_stat_t *              stat_array;
  int                              stat_count;
  globus_l_gfs_ceph_handle_t *     ceph_handle;
  char *                           func="globus_l_gfs_ceph_stat";
  struct stat64                    statbuf;
  int                              status=0;
  globus_result_t                  result;

  GlobusGFSName(globus_l_gfs_ceph_stat);
  ceph_handle = (globus_l_gfs_ceph_handle_t *) user_arg;
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, "%s: %s\n",
                         func, stat_info->pathname);
  status = ceph_posix_stat64(stat_info->pathname, &statbuf);
  if (status != 0) {
    result=globus_l_gfs_make_error("stat64");
    globus_gridftp_server_finished_stat(op,result,NULL, 0);
    return;
  }
  stat_array = (globus_gfs_stat_t *) globus_calloc(1, sizeof(globus_gfs_stat_t));
  if (stat_array==NULL) {
    result=GlobusGFSErrorGeneric("error: memory allocation failed");
    globus_gridftp_server_finished_stat(op,result,NULL, 0);
    return;
  }
  stat_count=1;
  fill_stat_array(&(stat_array[0]), statbuf, stat_info->pathname);
  globus_gridftp_server_finished_stat(op, GLOBUS_SUCCESS, stat_array, stat_count);
  free_stat_array(stat_array, stat_count);
  globus_free(stat_array);
  return;
}

/*************************************************************************
 *  command
 *  -------
 *  This interface function is called when the client sends a 'command'.
 *  commands are such things as mkdir, remdir, delete.  The complete
 *  enumeration is below.
 *
 *  To determine which command is being requested look at:
 *      cmd_info->command
 *
 *      GLOBUS_GFS_CMD_MKD = 1,
 *      GLOBUS_GFS_CMD_RMD,
 *      GLOBUS_GFS_CMD_DELE,
 *      GLOBUS_GFS_CMD_RNTO,
 *      GLOBUS_GFS_CMD_RNFR,
 *      GLOBUS_GFS_CMD_CKSM,
 *      GLOBUS_GFS_CMD_SITE_CHMOD,
 *      GLOBUS_GFS_CMD_SITE_DSI
 ************************************************************************/
static void globus_l_gfs_ceph_command(globus_gfs_operation_t op,
                                      globus_gfs_command_info_t *cmd_info,
                                      void *user_arg) {
  globus_result_t result;
  (void)cmd_info;
  (void)user_arg;
  GlobusGFSName(globus_l_gfs_ceph_command);
  /* in gridftp disk server we do not allow to perform commads */
  result=GlobusGFSErrorGeneric("error: commands denied");
  globus_gridftp_server_finished_command(op, result, GLOBUS_NULL);
  return;
}

int ceph_handle_open(char *path,
                     int flags,
                     int mode,
                     globus_l_gfs_ceph_handle_t *ceph_handle) {
  int       rc;
  char *    func="ceph_handle_open";

  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: %s\n", func, path);
  rc = ceph_posix_open(path, flags, mode);
  ceph_handle->fileSize = 0;
  return (rc);
}

/* receive from client */
static void globus_l_gfs_file_net_read_cb(globus_gfs_operation_t op,
                                          globus_result_t result,
                                          globus_byte_t *buffer,
                                          globus_size_t nbytes,
                                          globus_off_t offset,
                                          globus_bool_t eof,
                                          void *user_arg) {
  globus_off_t                 start_offset;
  globus_l_gfs_ceph_handle_t * ceph_handle;
  ssize_t                      bytes_written;
  unsigned long                adler;
  checksum_block_list_t**      checksum_array;
  checksum_block_list_t *      checksum_list_pp;
  unsigned long                index;
  unsigned long                i;
  unsigned long                file_checksum;
  char                         ckSumbuf[CA_MAXCKSUMLEN+1] = "0";
  char *                       ckSumalg = "ADLER32"; /* we only support Adler32 for gridftp */
  char *                       func = "globus_l_gfs_file_net_read_cb";

  ceph_handle = (globus_l_gfs_ceph_handle_t *) user_arg;

  globus_mutex_lock(&ceph_handle->mutex);
  {
    if (eof) ceph_handle->done = GLOBUS_TRUE;
    ceph_handle->outstanding--;
    if(result != GLOBUS_SUCCESS) {
      ceph_handle->cached_res = result;
      ceph_handle->done = GLOBUS_TRUE;
    }
    else if (nbytes > 0) {
      start_offset = ceph_posix_lseek64(ceph_handle->fd, offset, SEEK_SET);
      if (start_offset != offset) {
        ceph_handle->cached_res = globus_l_gfs_make_error("seek");
        ceph_handle->done = GLOBUS_TRUE;
      } else {
        bytes_written = ceph_posix_write(ceph_handle->fd, buffer, nbytes);
        if (bytes_written < 0) {
          globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: write error, return code %d \n",func, -bytes_written);
          ceph_handle->cached_res = GLOBUS_FAILURE;
          ceph_handle->done = GLOBUS_TRUE;
        } else {
          /* fill the checksum list  */
          /* we will have a lot of checksums blocks in the list */
          adler = adler32(0L, Z_NULL, 0);
          adler = adler32(adler, buffer, nbytes);

          ceph_handle->checksum_list_p->next=
            (checksum_block_list_t *)globus_malloc(sizeof(checksum_block_list_t));

          if (ceph_handle->checksum_list_p->next==NULL) {
            ceph_handle->cached_res = GLOBUS_FAILURE;
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: malloc error \n",func);
            ceph_handle->done = GLOBUS_TRUE;
            globus_mutex_unlock(&ceph_handle->mutex);
            return;
          }
          ceph_handle->checksum_list_p->next->next=NULL;
          ceph_handle->checksum_list_p->offset=offset;
          ceph_handle->checksum_list_p->size=bytes_written;
          ceph_handle->checksum_list_p->csumvalue=adler;
          ceph_handle->checksum_list_p=ceph_handle->checksum_list_p->next;
          ceph_handle->number_of_blocks++;
          /* end of the checksum section */
          if((globus_size_t)bytes_written < nbytes) {
            errno = ENOSPC;
            ceph_handle->cached_res = globus_l_gfs_make_error("write");
            ceph_handle->done = GLOBUS_TRUE;
            free_checksum_list(ceph_handle->checksum_list);
          } else {
            globus_gridftp_server_update_bytes_written(op,offset,nbytes);
            ceph_handle->fileSize += bytes_written;
          }
        }
      }
    }

    globus_free(buffer);
    /* if not done just register the next one */
    if (!ceph_handle->done) globus_l_gfs_ceph_read_from_net(ceph_handle);
    /* if done and there are no outstanding callbacks finish */
    else if(ceph_handle->outstanding == 0) {
      if (ceph_handle->number_of_blocks > 0) {
        /* checksum calculation */
        checksum_array=(checksum_block_list_t**)
          globus_calloc(ceph_handle->number_of_blocks,sizeof(checksum_block_list_t*));
        if (checksum_array == NULL) {
          free_checksum_list(ceph_handle->checksum_list);
          ceph_handle->fileSize = 0;
          globus_ceph_close(func, ceph_handle, "Internal error (malloc failed)");
          globus_mutex_unlock(&ceph_handle->mutex);
          return;
        }
        checksum_list_pp=ceph_handle->checksum_list;
        /* sorting of the list to the array */
        index = 0;
        /* the latest block is always empty and has next pointer as NULL */
        while (checksum_list_pp->next != NULL) {
          checksum_array[index] = checksum_list_pp;
          checksum_list_pp=checksum_list_pp->next;
          index++;
        }
        qsort(checksum_array, index, sizeof(checksum_block_list_t*), offsetComparison);
        /* combine checksums, while making sure that we deal with missing chunks */
        globus_off_t chkOffset = 0;
        /* check whether first chunk is missing */
        if (checksum_array[0]->offset != 0) {
          /* first chunk is missing. Consider it full of 0s */
          chkOffset = checksum_array[0]->offset;
          file_checksum = adler32_combine_(adler32_0chunks(chkOffset),
                                           checksum_array[0]->csumvalue,
                                           checksum_array[0]->size);
        } else {
          file_checksum = checksum_array[0]->csumvalue;
        }
        chkOffset += checksum_array[0]->size;
        /* go over all received chunks */
        for (i = 1; i < ceph_handle->number_of_blocks; i++) {
          /* check the continuity with previous chunk */
          if (checksum_array[i]->offset != chkOffset) {
            // not continuous, either a chunk is missing or we have overlapping chunks
            if (checksum_array[i]->offset > chkOffset) {
              // a chunk is missing, consider it full of 0s
              globus_off_t doff = checksum_array[i]->offset - chkOffset;
              file_checksum = adler32_combine_(file_checksum, adler32_0chunks(doff), doff);
              chkOffset = checksum_array[i]->offset;
            } else {
              // overlapping chunks. This is not supported, fail the transfer
              free_checksum_list(ceph_handle->checksum_list);
              globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: Overlapping chunks detected while handling 0x%x-0x%x. The overlap starts at 0x%x\n",
                                     func,
                                     checksum_array[i]->offset,
                                     checksum_array[i]->offset+checksum_array[i]->size,
                                     chkOffset);
              globus_ceph_close(func, ceph_handle, "overlapping chunks detected when computing checksum");
              globus_mutex_unlock(&ceph_handle->mutex);
              globus_gridftp_server_finished_transfer(op, ceph_handle->cached_res);
              return;
            }
          }
          /* now handle the next chunk */
          file_checksum=adler32_combine_(file_checksum,
                                         checksum_array[i]->csumvalue,
                                         checksum_array[i]->size);
          chkOffset += checksum_array[i]->size;
        }
        sprintf(ckSumbuf, "%lx", file_checksum);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "%s: checksum for fd %d : AD 0x%lx\n",
                               func,ceph_handle->fd, file_checksum);
        globus_free(checksum_array);
        free_checksum_list(ceph_handle->checksum_list);
        /* set extended attributes */
        if (ceph_posix_fsetxattr(ceph_handle->fd,"user.checksum.type",
                                 ckSumalg, strlen(ckSumalg), 0)) {
          globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: unable to store checksum type as xattr\n", func);
        }
        else if (ceph_posix_fsetxattr(ceph_handle->fd,"user.checksum.value",
                                      ckSumbuf, strlen(ckSumbuf), 0)) {
          globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: unable to store checksum value as xattr\n", func);
        }
      }
      globus_ceph_close(func, ceph_handle, NULL);
      globus_gridftp_server_finished_transfer(op, ceph_handle->cached_res);
    }
  }
  globus_mutex_unlock(&ceph_handle->mutex);
}

static void globus_l_gfs_ceph_read_from_net
(globus_l_gfs_ceph_handle_t *ceph_handle) {
  globus_byte_t *                     buffer;
  globus_result_t                     result;
  char *                     func="globus_l_gfs_ceph_read_from_net";

  GlobusGFSName(globus_l_gfs_ceph_read_from_net);
  /* in the read case this number will vary */
  globus_gridftp_server_get_optimal_concurrency(ceph_handle->op,
                                                &ceph_handle->optimal_count);

  while(ceph_handle->outstanding < ceph_handle->optimal_count) {
    buffer=globus_malloc(ceph_handle->block_size);
    if (buffer == NULL) {
      if (ceph_handle->outstanding == 0) {
        globus_ceph_close(func, ceph_handle, "internal error (malloc failed)");
        globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                ceph_handle->cached_res);
      }
      return;
    }
    result= globus_gridftp_server_register_read(ceph_handle->op,
                                                buffer,
                                                ceph_handle->block_size,
                                                globus_l_gfs_file_net_read_cb,
                                                ceph_handle);

    if(result != GLOBUS_SUCCESS)  {
      globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,
                             "%s: register read has finished with a bad result\n",
                             func);
      globus_free(buffer);
      ceph_handle->cached_res = result;
      if (ceph_handle->outstanding == 0) {
        globus_ceph_close(func, ceph_handle, "register read has finished with a bad result");
        globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                ceph_handle->cached_res);
      }
      return;
    }
    ceph_handle->outstanding++;
  }
}

/*************************************************************************
 *  recv
 *  ----
 *  This interface function is called when the client requests that a
 *  file be transfered to the server.
 *
 *  To receive a file the following functions will be used in roughly
 *  the presented order.  They are doced in more detail with the
 *  gridftp server documentation.
 *
 *      globus_gridftp_server_begin_transfer();
 *      globus_gridftp_server_register_read();
 *      globus_gridftp_server_finished_transfer();
 *
 ************************************************************************/

static void globus_l_gfs_ceph_recv(globus_gfs_operation_t op,
                                      globus_gfs_transfer_info_t *transfer_info,
                                      void *user_arg) {
  globus_l_gfs_ceph_handle_t *     ceph_handle;

  globus_result_t                     result;
  char *                 func="globus_l_gfs_ceph_recv";
  char *                 pathname;
  int                 flags;

  GlobusGFSName(globus_l_gfs_ceph_recv);
  ceph_handle = (globus_l_gfs_ceph_handle_t *) user_arg;

  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: started\n",func);

  pathname=strdup(transfer_info->pathname);
  if(pathname==NULL) {
    result=GlobusGFSErrorGeneric("error: strdup failed");
    globus_gridftp_server_finished_transfer(op, result);
    return;
  }
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: pathname: %s \n",func,pathname);

  struct stat64 sbuf;  
  int rc = ceph_posix_stat64(pathname, &sbuf);
 
  int mode = 0666;
 
  flags = O_WRONLY | O_CREAT;
  
  if (rc == 0) { // File exists
    globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, "%s : File %s exists - about to truncate\n", __FUNCTION__, pathname);
 /*      if (transfer_info->alloc_size < sbuf.st_size)  */
      rc = ceph_posix_truncate(pathname,mode, flags, 0);

    if (rc != 0) {
        /* globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, "%s : Cannot truncate %s\n", __FUNCTION__, pathname); // reported in ceph_posix_truncate */
        free(pathname);
        result = globus_l_gfs_make_error("open/truncate");
        globus_gridftp_server_finished_transfer(op, result);
        return;
    }
  }
  /* try to open */
  if(transfer_info->truncate) flags |= O_TRUNC;

  ceph_handle->fd = ceph_handle_open(pathname, flags, 0644, ceph_handle);

  if (ceph_handle->fd < 0) {
    errno = EACCES; 
    result=globus_l_gfs_make_error("open/create");
    free(pathname);
    globus_gridftp_server_finished_transfer(op, result);
    return;
  }

  /* reset all the needed variables in the handle */
  ceph_handle->cached_res = GLOBUS_SUCCESS;
  ceph_handle->outstanding = 0;
  ceph_handle->done = GLOBUS_FALSE;
  ceph_handle->blk_length = 0;
  ceph_handle->blk_offset = 0;
  ceph_handle->op = op;

  globus_gridftp_server_get_block_size(op, &ceph_handle->block_size);
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: block size: %ld\n",
                         func,ceph_handle->block_size);

  /* here we will save all checksums for the file blocks        */
  /* malloc memory for the first element in the checksum list   */
  /* we should always have at least one block for a file        */
  ceph_handle->checksum_list=
    (checksum_block_list_t *)globus_malloc(sizeof(checksum_block_list_t));
  if (ceph_handle->checksum_list==NULL) {
    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: malloc error \n",func);
    globus_gridftp_server_finished_transfer(op, GLOBUS_FAILURE);
    return;
  }
  ceph_handle->checksum_list->next=NULL;
  ceph_handle->checksum_list_p=ceph_handle->checksum_list;
  ceph_handle->number_of_blocks=0;

  globus_gridftp_server_begin_transfer(op, 0, ceph_handle);

  globus_mutex_lock(&ceph_handle->mutex);
  {
    globus_l_gfs_ceph_read_from_net(ceph_handle);
  }
  globus_mutex_unlock(&ceph_handle->mutex);
  free(pathname);
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: finished\n",func);
  return;
}

/*************************************************************************
 *  send
 *  ----
 *  This interface function is called when the client requests to receive
 *  a file from the server.
 *
 *  To send a file to the client the following functions will be used in roughly
 *  the presented order.  They are doced in more detail with the
 *  gridftp server documentation.
 *
 *      globus_gridftp_server_begin_transfer();
 *      globus_gridftp_server_register_write();
 *      globus_gridftp_server_finished_transfer();
 *
 ************************************************************************/
static void globus_l_gfs_ceph_send(globus_gfs_operation_t op,
                                      globus_gfs_transfer_info_t *transfer_info,
                                      void *user_arg) {
  globus_l_gfs_ceph_handle_t *       ceph_handle;
  char *                 func="globus_l_gfs_ceph_send";
  char *                pathname;
  int                 i;
  globus_bool_t                       done;
  globus_result_t                     result;

  GlobusGFSName(globus_l_gfs_ceph_send);
  ceph_handle = (globus_l_gfs_ceph_handle_t *) user_arg;
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: started\n",func);

  pathname=strdup(transfer_info->pathname);
  if (pathname == NULL) {
    result = GlobusGFSErrorGeneric("error: strdup failed");
    globus_gridftp_server_finished_transfer(op, result);
    return;
  }

  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: pathname: %s\n",func,pathname);
  
  /* Check whether the file exists before going any further */
  struct stat64 sbuf;
  int rc = ceph_posix_stat64(pathname, &sbuf);
  if (rc != 0) {
     result = globus_l_gfs_make_error("open");
     free(pathname);
     globus_gridftp_server_finished_transfer(op, result);
     return;    
  }  
  
  /* mode is ignored */
  ceph_handle->fd = ceph_handle_open(pathname, O_RDONLY,
                                           0, ceph_handle);

  if (ceph_handle->fd < 0) {
    result = globus_l_gfs_make_error("open");
    free(pathname);
    globus_gridftp_server_finished_transfer(op, result);
    return;
  }

  /* reset all the needed variables in the handle */
  ceph_handle->cached_res = GLOBUS_SUCCESS;
  ceph_handle->outstanding = 0;
  ceph_handle->done = GLOBUS_FALSE;
  ceph_handle->blk_length = 0;
  ceph_handle->blk_offset = 0;
  ceph_handle->op = op;

  globus_gridftp_server_get_optimal_concurrency(op, &ceph_handle->optimal_count);
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: optimal_concurrency: %u\n",
                         func,ceph_handle->optimal_count);

  globus_gridftp_server_get_block_size(op, &ceph_handle->block_size);
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: block_size: %ld\n",
                         func,ceph_handle->block_size);

  /* here we will save all checksums for the file blocks        */
  /* malloc memory for the first element in the checksum list   */
  /* we should always have at least one block for a file        */
  ceph_handle->checksum_list=
    (checksum_block_list_t *)globus_malloc(sizeof(checksum_block_list_t));
  if (ceph_handle->checksum_list==NULL) {
    globus_gfs_log_message(GLOBUS_GFS_LOG_ERR,"%s: malloc error \n",func);
    globus_gridftp_server_finished_transfer(op, GLOBUS_FAILURE);
    return;
  }
  ceph_handle->checksum_list->next=NULL;
  ceph_handle->checksum_list_p=ceph_handle->checksum_list;
  ceph_handle->number_of_blocks=0;

  globus_gridftp_server_begin_transfer(op, 0, ceph_handle);
  done = GLOBUS_FALSE;
  globus_mutex_lock(&ceph_handle->mutex);
  {
    for(i = 0; i < ceph_handle->optimal_count && !done; i++) {
      done = globus_l_gfs_ceph_send_next_to_client(ceph_handle);
    }
  }
  globus_mutex_unlock(&ceph_handle->mutex);
  free(pathname);
  globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: finished\n",func);
}

static globus_bool_t globus_l_gfs_ceph_send_next_to_client
(globus_l_gfs_ceph_handle_t *ceph_handle) {
  globus_result_t                     result;
  globus_result_t                     res;
  globus_off_t                        read_length;
  globus_off_t                        nbread;
  globus_off_t                        start_offset;
  globus_byte_t *                     buffer;
  unsigned long                       adler;
  checksum_block_list_t**             checksum_array;
  checksum_block_list_t *             checksum_list_pp;
  unsigned long                       index;
  unsigned long                       i;
  unsigned long                       file_checksum;
  char                                ckSumbuf[CA_MAXCKSUMLEN+1];
  char                                ckSumbufdisk[CA_MAXCKSUMLEN+1];
  char                                ckSumnamedisk[CA_MAXCKSUMNAMELEN+1];
  char                                useCksum;
  int                                 xattr_len;
  char *                              func = "globus_l_gfs_ceph_send_next_to_client";

  GlobusGFSName(globus_l_gfs_ceph_send_next_to_client);

  if (ceph_handle->blk_length == 0) {
    /* check the next range to read */
    globus_gridftp_server_get_read_range(ceph_handle->op,
                                         &ceph_handle->blk_offset,
                                         &ceph_handle->blk_length);
    if(ceph_handle->blk_length == 0) {
      globus_ceph_close(func, ceph_handle, NULL);
      if (ceph_handle->outstanding == 0) {
        globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                ceph_handle->cached_res);
      }
      return ceph_handle->done;
    }
  }

  if (ceph_handle->blk_length == -1 ||
      (globus_size_t)ceph_handle->blk_length > ceph_handle->block_size)
    read_length = ceph_handle->block_size;
  else read_length = ceph_handle->blk_length;

  start_offset = ceph_posix_lseek64(ceph_handle->fd,
                                    ceph_handle->blk_offset,
                                    SEEK_SET);
  /* verify that it worked */
  if (start_offset != ceph_handle->blk_offset) {
    globus_ceph_close(func, ceph_handle, "failed to seek");
    if (ceph_handle->outstanding == 0) {
      globus_gridftp_server_finished_transfer(ceph_handle->op,
                                              ceph_handle->cached_res);
    }
    return ceph_handle->done;
  }

  buffer = globus_malloc(read_length);
  if (buffer == NULL) {
    globus_ceph_close(func, ceph_handle, "internal error (malloc failed)");
    if (ceph_handle->outstanding == 0) {
      globus_gridftp_server_finished_transfer(ceph_handle->op,
                                              ceph_handle->cached_res);
    }
    return ceph_handle->done;
  }

  nbread = ceph_posix_read(ceph_handle->fd, buffer, read_length);
  if (nbread>0) {
    /* fill the checksum list  */
    adler = adler32(0L, Z_NULL, 0);
    adler = adler32(adler, buffer, nbread);

    ceph_handle->checksum_list_p->next=
      (checksum_block_list_t *)globus_malloc(sizeof(checksum_block_list_t));

    if (ceph_handle->checksum_list_p->next==NULL) {
      globus_free(buffer);
      globus_ceph_close(func, ceph_handle, "internal error (malloc failed)");
      if (ceph_handle->outstanding == 0) {
        globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                ceph_handle->cached_res);
      }
      return ceph_handle->done;
    }
    ceph_handle->checksum_list_p->next->next=NULL;
    ceph_handle->checksum_list_p->offset=ceph_handle->blk_offset;
    ceph_handle->checksum_list_p->size=nbread;
    ceph_handle->checksum_list_p->csumvalue=adler;
    ceph_handle->checksum_list_p=ceph_handle->checksum_list_p->next;
    ceph_handle->number_of_blocks++;
  }
  if(nbread == 0) { /* eof */
    result = GLOBUS_SUCCESS;
    globus_free(buffer);

    /* checksum calculation */
    checksum_array=
      (checksum_block_list_t**)globus_calloc(ceph_handle->number_of_blocks,
                                             sizeof(checksum_block_list_t*));
    if (checksum_array==NULL){
      free_checksum_list(ceph_handle->checksum_list);
      globus_ceph_close(func, ceph_handle, "internal error (malloc failed)");
      if (ceph_handle->outstanding == 0) {
        globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                ceph_handle->cached_res);
      }
      return ceph_handle->done;
    }
    checksum_list_pp=ceph_handle->checksum_list;
    /* sorting of the list to the array */
    index = 0;
    /* the latest block is always empty and has next pointer as NULL */
    while (checksum_list_pp->next != NULL) {
      checksum_array[index] = checksum_list_pp;
      checksum_list_pp=checksum_list_pp->next;
      index++;
    }
    qsort(checksum_array, index, sizeof(checksum_block_list_t*), offsetComparison);
    /* combine here  */
    /* ************* */
    file_checksum=checksum_array[0]->csumvalue;
    for (i=1;i<ceph_handle->number_of_blocks;i++) {
      file_checksum=adler32_combine_(file_checksum,checksum_array[i]->csumvalue,
                                     checksum_array[i]->size);
    }
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,"%s: checksum for fd %d : AD 0x%lx\n",
                           func,ceph_handle->fd,file_checksum);
    globus_free(checksum_array);
    free_checksum_list(ceph_handle->checksum_list);
    /* get extended attributes */
    useCksum=1;
    xattr_len = ceph_posix_fgetxattr(ceph_handle->fd,
                                     "user.checksum.type",
                                     ckSumnamedisk,
                                     CA_MAXCKSUMNAMELEN);
      if (xattr_len < 0) {
        /* no error messages */
        useCksum = 0;
      } else {
        ckSumnamedisk[xattr_len] = '\0';
        xattr_len = ceph_posix_fgetxattr(ceph_handle->fd,
                                         "user.checksum.value",
                                         ckSumbufdisk,CA_MAXCKSUMLEN);
        if (xattr_len < 0) {
          /* error */
          ceph_handle->cached_res =
            globus_error_put (globus_object_construct (GLOBUS_ERROR_TYPE_BAD_DATA));
          if (ceph_handle->outstanding == 0) {
            globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                    ceph_handle->cached_res);
          }
          char errorBuf[1024];
          sprintf(errorBuf, "error detected while reading checksum for fd: %d, errono=%d\n", ceph_handle->fd, -xattr_len);
          globus_ceph_close(func, ceph_handle, errorBuf);
          return ceph_handle->done;
        } else {
          ckSumbufdisk[xattr_len] = '\0';
          if (strncmp(ckSumnamedisk,"ADLER32",CA_MAXCKSUMNAMELEN) != 0) {
            useCksum=1; /* for gridftp we know only ADLER32 */
          }
        }
      }

    if (useCksum) { /* we have disks and on the fly checksums here */
      sprintf(ckSumbuf, "%lx", file_checksum);
      if (strncmp(ckSumbufdisk,ckSumbuf,CA_MAXCKSUMLEN)==0) {
        globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,"%s: checksums OK! \n",func);
      } else {
        char errorBuf[1024];
        sprintf(errorBuf, "checksum error detected reading fd: %d (recorded checksum: 0x%s calculated checksum: 0x%s)\n",
                ceph_handle->fd,
                ckSumbufdisk,
                ckSumbuf);
        /* to do something in error case */
        globus_ceph_close(func, ceph_handle, errorBuf);
        ceph_handle->cached_res =
          globus_error_put (globus_object_construct (GLOBUS_ERROR_TYPE_BAD_DATA));
        if (ceph_handle->outstanding == 0) {
          globus_gridftp_server_finished_transfer(ceph_handle->op,
                                                  ceph_handle->cached_res);
        }
        return ceph_handle->done;
      }
    } else {
      globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,
                             "%s: ADLER32 checksum has not been found in extended attributes\n",
                             func);
    }
    globus_ceph_close(func, ceph_handle, NULL);
    if (ceph_handle->outstanding == 0) {
      globus_gridftp_server_finished_transfer(ceph_handle->op,
                                              ceph_handle->cached_res);
    }
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,"%s: finished (eof)\n",func);
    return ceph_handle->done;
  }
  if (nbread < 0) { /* error */
    globus_free(buffer);
    globus_ceph_close(func, ceph_handle, "error reading from disk");
    if (ceph_handle->outstanding == 0) {
      globus_gridftp_server_finished_transfer(ceph_handle->op,
                                              ceph_handle->cached_res);
    }
    return ceph_handle->done;
  }

  if (read_length>=nbread) {
    /* if we have a file with size less than block_size we do not have use parrallel connections (one will be enough) */
    ceph_handle->optimal_count--;
  }
  read_length = nbread;

  if (ceph_handle->blk_length != -1) {
    ceph_handle->blk_length -= read_length;
  }

  /* start offset? */
  res = globus_gridftp_server_register_write(ceph_handle->op,
                                             buffer,
                                             read_length,
                                             ceph_handle->blk_offset,
                                             -1,
                                             globus_l_gfs_net_write_cb,
                                             ceph_handle);

  ceph_handle->blk_offset += read_length;

  if(res != GLOBUS_SUCCESS) {
    globus_free(buffer);
    globus_ceph_close(func, ceph_handle, "error writing to network");
    ceph_handle->cached_res = res;
    if (ceph_handle->outstanding == 0) {
      globus_gridftp_server_finished_transfer(ceph_handle->op,
                                              ceph_handle->cached_res);
    }
    return ceph_handle->done;
  }

  ceph_handle->outstanding++;
  return GLOBUS_FALSE;
}


static void globus_l_gfs_net_write_cb(globus_gfs_operation_t op,
                                      globus_result_t result,
                                      globus_byte_t *buffer,
                                      globus_size_t nbytes,
                                      void *user_arg) {
  globus_l_gfs_ceph_handle_t *ceph_handle;
  char *func="globus_l_gfs_net_write_cb";
  (void)nbytes;
  ceph_handle = (globus_l_gfs_ceph_handle_t *) user_arg;

  globus_free(buffer);
  globus_mutex_lock(&ceph_handle->mutex);
  {
    ceph_handle->outstanding--;
    if(result != GLOBUS_SUCCESS) {
      ceph_handle->cached_res = result;
      ceph_handle->done = GLOBUS_TRUE;
    }
    if (!ceph_handle->done)  {
      globus_l_gfs_ceph_send_next_to_client(ceph_handle);
    } else if (ceph_handle->outstanding == 0) {
      /* this is a read, we don't care about the checksum */
      globus_ceph_close(func, ceph_handle, NULL);
      globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,"%s: finished transfer\n",func);
      globus_gridftp_server_finished_transfer(op, ceph_handle->cached_res);
    }
  }
  globus_mutex_unlock(&ceph_handle->mutex);
}


static int globus_l_gfs_ceph_activate(void);

static int globus_l_gfs_ceph_deactivate(void);

/*
 *  no need to change this
 */
static globus_gfs_storage_iface_t globus_l_gfs_ceph_dsi_iface = {
  GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING | GLOBUS_GFS_DSI_DESCRIPTOR_SENDER,
  globus_l_gfs_ceph_start,
  globus_l_gfs_ceph_destroy,
  NULL, /* list */
  globus_l_gfs_ceph_send,
  globus_l_gfs_ceph_recv,
  NULL, /* trev */
  NULL, /* active */
  NULL, /* passive */
  NULL, /* data destroy */
  globus_l_gfs_ceph_command,
  globus_l_gfs_ceph_stat,
  NULL, /* set_cred */
  NULL, /* buffer_send */
  NULL  /*realpath */
};

/*
 *  no need to change this
 */
GlobusExtensionDefineModule(globus_gridftp_server_ceph) = {
  "globus_gridftp_server_ceph",
  globus_l_gfs_ceph_activate,
  globus_l_gfs_ceph_deactivate,
  NULL,
  NULL,
  &local_version,
  NULL
};

/*
 *  no need to change this
 */
static int globus_l_gfs_ceph_activate(void) {
  globus_extension_registry_add(GLOBUS_GFS_DSI_REGISTRY,
                                "ceph",
                                GlobusExtensionMyModule(globus_gridftp_server_ceph),
                                &globus_l_gfs_ceph_dsi_iface);
  // initialize ceph wrapper log
  ceph_posix_set_logfunc(ceph_logfunc_wrapper);
  // setup defaults from environment
  ceph_posix_set_defaults(getenv("GRIDFTP_CEPH_DEFAULTS"));
  return 0;
}

/*
 *  no need to change this
 */
static int globus_l_gfs_ceph_deactivate(void) {
  globus_extension_registry_remove(GLOBUS_GFS_DSI_REGISTRY, "ceph");
  // disconnect from ceph
  ceph_posix_disconnect_all();
  return 0;
}
