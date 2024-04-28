/* From: https://chromium.googlesource.com/chromium/src.git/+/4.1.249.1050/third_party/sqlite/src/os_symbian.cc
 * https://github.com/spsoft/spmemvfs/tree/master/spmemvfs
 * http://www.sqlite.org/src/doc/trunk/src/test_ESP32vfs.c
 * http://www.sqlite.org/src/doc/trunk/src/test_vfstrace.c
 * http://www.sqlite.org/src/doc/trunk/src/test_onefile.c
 * http://www.sqlite.org/src/doc/trunk/src/test_vfs.c
 * https://github.com/nodemcu/nodemcu-firmware/blob/master/app/sqlite3/esp8266.c
 **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sqlite3.h>
#include <Arduino.h>
//#include <esp_spi_flash.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/param.h>
#include <errno.h>
#include <fcntl.h>
#include "shox96_0_2.h"

#undef dbg_printf
//#define dbg_printf(...) Serial.printf(__VA_ARGS__)
#define dbg_printf(...) 0

extern "C" {
    void SerialPrintln(const char *str) {
        //Serial.println(str);
    }
}

// From https://stackoverflow.com/questions/19758270/read-varint-from-linux-sockets#19760246
// Encode an unsigned 64-bit varint.  Returns number of encoded bytes.
// 'buffer' must have room for up to 10 bytes.
int encode_unsigned_varint(uint8_t *buffer, uint64_t value) {
	int encoded = 0;
	do {
		uint8_t next_byte = value & 0x7F;
		value >>= 7;
		if (value)
			next_byte |= 0x80;
		buffer[encoded++] = next_byte;
	} while (value);
	return encoded;
}

uint64_t decode_unsigned_varint(const uint8_t *data, int &decoded_bytes) {
	int i = 0;
	uint64_t decoded_value = 0;
	int shift_amount = 0;
	do {
		decoded_value |= (uint64_t)(data[i] & 0x7F) << shift_amount;     
		shift_amount += 7;
	} while ((data[i++] & 0x80) != 0);
	decoded_bytes = i;
	return decoded_value;
}

/*
** Size of the write buffer used by journal files in bytes.
*/
#ifndef SQLITE_RP2040VFS_BUFFERSZ
# define SQLITE_RP2040VFS_BUFFERSZ 8192
#endif

/*
** The maximum pathname length supported by this VFS.
*/
#define MAXPATHNAME 100

/*
** When using this VFS, the sqlite3_file* handles that SQLite uses are
** actually pointers to instances of type ESP32File.
*/
typedef struct RP2040File RP2040File;
struct RP2040File {
  sqlite3_file base;              /* Base class. Must be first. */
  FILE *fp;                       /* File descriptor */

  char *aBuffer;                  /* Pointer to malloc'd buffer */
  int nBuffer;                    /* Valid bytes of data in zBuffer */
  sqlite3_int64 iBufferOfst;      /* Offset in file of zBuffer[0] */
};

/*
** Write directly to the file passed as the first argument. Even if the
** file has a write-buffer (ESP32File.aBuffer), ignore it.
*/
static int RP2040DirectWrite(
  RP2040File *p,                    /* File handle */
  const void *zBuf,               /* Buffer containing data to write */
  int iAmt,                       /* Size of data to write in bytes */
  sqlite_int64 iOfst              /* File offset to write to */
){
  off_t ofst;                     /* Return value from lseek() */
  size_t nWrite;                  /* Return value from write() */

  //Serial.println("fn: DirectWrite:");

  ofst = fseek(p->fp, iOfst, SEEK_SET); //lseek(p->fd, iOfst, SEEK_SET);
  if( ofst != 0 ){
    //Serial.println("Seek error");
    return SQLITE_IOERR_WRITE;
  }

  nWrite = fwrite(zBuf, 1, iAmt, p->fp); // write(p->fd, zBuf, iAmt);
  if( nWrite!=iAmt ){
    //Serial.println("Write error");
    return SQLITE_IOERR_WRITE;
  }

  //Serial.println("fn:DirectWrite:Success");

  return SQLITE_OK;
}

/*
** Flush the contents of the ESP32File.aBuffer buffer to disk. This is a
** no-op if this particular file does not have a buffer (i.e. it is not
** a journal file) or if the buffer is currently empty.
*/
static int RP2040FlushBuffer(RP2040File *p){
  int rc = SQLITE_OK;
  //Serial.println("fn: FlushBuffer");
  if( p->nBuffer ){
    rc = RP2040DirectWrite(p, p->aBuffer, p->nBuffer, p->iBufferOfst);
    p->nBuffer = 0;
  }
  //Serial.println("fn:FlushBuffer:Success");
  return rc;
}

/*
** Close a file.
*/
static int RP2040Close(sqlite3_file *pFile){
  int rc;
  //Serial.println("fn: Close");
  RP2040File *p = (RP2040File*)pFile;
  rc = RP2040FlushBuffer(p);
  sqlite3_free(p->aBuffer);
  fclose(p->fp);
  //Serial.println("fn:Close:Success");
  return rc;
}

/*
** Read data from a file.
*/
static int RP2040Read(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
      //Serial.println("fn: Read");
  RP2040File *p = (RP2040File*)pFile;
  off_t ofst;                     /* Return value from lseek() */
  int nRead;                      /* Return value from read() */
  int rc;                         /* Return code from ESP32FlushBuffer() */

  /* Flush any data in the write buffer to disk in case this operation
  ** is trying to read data the file-region currently cached in the buffer.
  ** It would be possible to detect this case and possibly save an 
  ** unnecessary write here, but in practice SQLite will rarely read from
  ** a journal file when there is data cached in the write-buffer.
  */
  rc = RP2040FlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  ofst = fseek(p->fp, iOfst, SEEK_SET); //lseek(p->fd, iOfst, SEEK_SET);
  //if( ofst != 0 ){
  //  return SQLITE_IOERR_READ;
  //}
  nRead = fread(zBuf, 1, iAmt, p->fp); // read(p->fd, zBuf, iAmt);

  if( nRead==iAmt ){
    //Serial.println("fn:Read:Success");
    return SQLITE_OK;
  }else if( nRead>=0 ){
    return SQLITE_IOERR_SHORT_READ;
  }

  return SQLITE_IOERR_READ;
}

/*
** Write data to a crash-file.
*/
static int RP2040Write(
  sqlite3_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
      //Serial.println("fn: Write");
  RP2040File *p = (RP2040File*)pFile;
  
  if( p->aBuffer ){
    char *z = (char *)zBuf;       /* Pointer to remaining data to write */
    int n = iAmt;                 /* Number of bytes at z */
    sqlite3_int64 i = iOfst;      /* File offset to write to */

    while( n>0 ){
      int nCopy;                  /* Number of bytes to copy into buffer */

      /* If the buffer is full, or if this data is not being written directly
      ** following the data already buffered, flush the buffer. Flushing
      ** the buffer is a no-op if it is empty.  
      */
      if( p->nBuffer==SQLITE_RP2040VFS_BUFFERSZ || p->iBufferOfst+p->nBuffer!=i ){
        int rc = RP2040FlushBuffer(p);
        if( rc!=SQLITE_OK ){
          return rc;
        }
      }
      assert( p->nBuffer==0 || p->iBufferOfst+p->nBuffer==i );
      p->iBufferOfst = i - p->nBuffer;

      /* Copy as much data as possible into the buffer. */
      nCopy = SQLITE_RP2040VFS_BUFFERSZ - p->nBuffer;
      if( nCopy>n ){
        nCopy = n;
      }
      memcpy(&p->aBuffer[p->nBuffer], z, nCopy);
      p->nBuffer += nCopy;

      n -= nCopy;
      i += nCopy;
      z += nCopy;
    }
  }else{
    return RP2040DirectWrite(p, zBuf, iAmt, iOfst);
  }
  //Serial.println("fn:Write:Success");

  return SQLITE_OK;
}

/*
** Truncate a file. This is a no-op for this VFS (see header comments at
** the top of the file).
*/
static int RP2040Truncate(sqlite3_file *pFile, sqlite_int64 size){
      //Serial.println("fn: Truncate");
#if 0
  if( ftruncate(((RP2040File *)pFile)->fd, size) ) return SQLITE_IOERR_TRUNCATE;
#endif
  //Serial.println("fn:Truncate:Success");
  return SQLITE_OK;
}

/*
** Sync the contents of the file to the persistent media.
*/
static int ESP32Sync(sqlite3_file *pFile, int flags){
      //Serial.println("fn: Sync");
  RP2040File *p = (RP2040File*)pFile;
  int rc;

  rc = RP2040FlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  rc = fflush(p->fp);
  if (rc != 0)
    return SQLITE_IOERR_FSYNC;
  rc = fsync(fileno(p->fp));
  //if (rc == 0)
    //Serial.println("fn:Sync:Success");
  return SQLITE_OK; // ignore fsync return value // (rc==0 ? SQLITE_OK : SQLITE_IOERR_FSYNC);
}

/*
** Write the size of the file in bytes to *pSize.
*/
static int RP2040FileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
      //Serial.println("fn: FileSize");
  RP2040File *p = (RP2040File*)pFile;
  int rc;                         /* Return code from fstat() call */
  struct stat sStat;              /* Output of fstat() call */

  /* Flush the contents of the buffer to disk. As with the flush in the
  ** RP2040Read() method, it would be possible to avoid this and save a write
  ** here and there. But in practice this comes up so infrequently it is
  ** not worth the trouble.
  */
  rc = RP2040FlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }

	struct stat st;
	int fno = fileno(p->fp);
	if (fno < 0)
		return SQLITE_IOERR_FSTAT;
	if (fstat(fno, &st))
		return SQLITE_IOERR_FSTAT;
  *pSize = st.st_size;
  //Serial.println("fn:FileSize:Success");
  return SQLITE_OK;
}

/*
** Locking functions. The xLock() and xUnlock() methods are both no-ops.
** The xCheckReservedLock() always indicates that no other process holds
** a reserved lock on the database file. This ensures that if a hot-journal
** file is found in the file-system it is rolled back.
*/
static int RP2040Lock(sqlite3_file *pFile, int eLock){
  return SQLITE_OK;
}
static int RP2040Unlock(sqlite3_file *pFile, int eLock){
  return SQLITE_OK;
}
static int RP2040CheckReservedLock(sqlite3_file *pFile, int *pResOut){
  *pResOut = 0;
  return SQLITE_OK;
}

/*
** No xFileControl() verbs are implemented by this VFS.
*/
static int RP2040FileControl(sqlite3_file *pFile, int op, void *pArg){
  return SQLITE_OK;
}

/*
** The xSectorSize() and xDeviceCharacteristics() methods. These two
** may return special values allowing SQLite to optimize file-system 
** access to some extent. But it is also safe to simply return 0.
*/
static int RP2040SectorSize(sqlite3_file *pFile){
  return 0;
}
static int RP2040DeviceCharacteristics(sqlite3_file *pFile){
  return 0;
}

#ifndef F_OK
# define F_OK 0
#endif
#ifndef R_OK
# define R_OK 4
#endif
#ifndef W_OK
# define W_OK 2
#endif

/*
** Query the file-system to see if the named file exists, is readable or
** is both readable and writable.
*/
static int RP2040Access(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  int rc;                         /* access() return code */
  int eAccess = F_OK;             /* Second argument to access() */
      //Serial.println("fn: Access");

  assert( flags==SQLITE_ACCESS_EXISTS       /* access(zPath, F_OK) */
       || flags==SQLITE_ACCESS_READ         /* access(zPath, R_OK) */
       || flags==SQLITE_ACCESS_READWRITE    /* access(zPath, R_OK|W_OK) */
  );

  if( flags==SQLITE_ACCESS_READWRITE ) eAccess = R_OK|W_OK;
  if( flags==SQLITE_ACCESS_READ )      eAccess = R_OK;

  rc = access(zPath, eAccess);
  *pResOut = (rc==0);
  //Serial.println("fn:Access:Success");
  return SQLITE_OK;
}

static char dbrootpath[MAXPATHNAME+1];

/*
** Open a file handle.
*/
static int RP2040Open(
  sqlite3_vfs *pVfs,              /* VFS */
  const char *zName,              /* File to open, or 0 for a temp file */
  sqlite3_file *pFile,            /* Pointer to ESP32File struct to populate */
  int flags,                      /* Input SQLITE_OPEN_XXX flags */
  int *pOutFlags                  /* Output SQLITE_OPEN_XXX flags (or NULL) */
){
  static const sqlite3_io_methods RP2040io = {
    1,                            /* iVersion */
    RP2040Close,                    /* xClose */
    RP2040Read,                     /* xRead */
    RP2040Write,                    /* xWrite */
    RP2040Truncate,                 /* xTruncate */
    RP2040Sync,                     /* xSync */
    RP2040FileSize,                 /* xFileSize */
    RP2040Lock,                     /* xLock */
    RP2040Unlock,                   /* xUnlock */
    RP2040CheckReservedLock,        /* xCheckReservedLock */
    ERP2040FileControl,             /* xFileControl */
    RP2040SectorSize,               /* xSectorSize */
    RP2040DeviceCharacteristics     /* xDeviceCharacteristics */
  };

  RP2040File *p = (RP2040File*)pFile; /* Populate this structure */
  int oflags = 0;                 /* flags to pass to open() call */
  char *aBuf = 0;
	char mode[5];
      //Serial.println("fn: Open");

	strcpy(mode, "r");

  if( flags&SQLITE_OPEN_MAIN_JOURNAL ){
    aBuf = (char *)sqlite3_malloc(SQLITE_RP2040VFS_BUFFERSZ);
    if( !aBuf ){
      return SQLITE_NOMEM;
    }
  }

	if( flags&SQLITE_OPEN_CREATE || flags&SQLITE_OPEN_READWRITE 
          || flags&SQLITE_OPEN_MAIN_JOURNAL ) {
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    int rc = (zName == 0 ? -1 : stat( zName, &st ));
    //Serial.println(zName);
		if (rc < 0) {
      strcpy(mode, "w+");
      //int fd = open(zName, (O_CREAT | O_RDWR | O_EXCL), S_IRUSR | S_IWUSR);
      //close(fd);
      //oflags |= (O_CREAT | O_RDWR);
      //Serial.println("Create mode");
    } else
      strcpy(mode, "r+");
	}

  memset(p, 0, sizeof(RP2040File));
  //p->fd = open(zName, oflags, 0600);
  //p->fd = open(zName, oflags, S_IRUSR | S_IWUSR);
  if (zName == 0) {
    //generate a temporary file name
    char *tName = tmpnam(NULL);
    tName[4] = '_';
    size_t len = strlen(dbrootpath);
    memmove(tName + len, tName, strlen(tName) + 1);
    memcpy(tName, dbrootpath, len);    
    p->fp = fopen(tName, mode);
    //https://stackoverflow.com/questions/64424287/how-to-delete-a-file-in-c-using-a-file-descriptor
    //for temp file, then no need to handle in esp32close
    unlink(tName);
    //Serial.println("Temporary file name generated: " + String(tName) + " mode: " + String(mode));
  } else {
    //detect database root as folder for temporary files, every newly openened db will change this path
    //this mainly fixes that vfs's have their own root name like /sd
    char *ext = strrchr(zName, '.');
    bool isdb = false;
    if (ext) {
      isdb = (strcmp(ext+1,"db") == 0);
    }
    if (isdb) {      
      char zDir[MAXPATHNAME+1];
      int i=0;
      strcpy(zDir,zName);

      for(i=1; zDir[i]!='/'; i++) {};
      zDir[i] = '\0';

      strcpy(dbrootpath, zDir);
    }

    p->fp = fopen(zName, mode);
  }

  if( p->fp == NULL){
    if (aBuf)
      sqlite3_free(aBuf);
    //Serial.println("Can't open");
    return SQLITE_CANTOPEN;
  }
  p->aBuffer = aBuf;

  if( pOutFlags ){
    *pOutFlags = flags;
  }
  p->base.pMethods = &RP2040io;
  //Serial.println("fn:Open:Success");
  return SQLITE_OK;
}

/*
** Delete the file identified by argument zPath. If the dirSync parameter
** is non-zero, then ensure the file-system modification to delete the
** file has been synced to disk before returning.
*/
static int RP2040Delete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  int rc;                         /* Return code */

      //Serial.println("fn: Delete");

  rc = unlink(zPath);
  if( rc!=0 && errno==ENOENT ) return SQLITE_OK;

  if( rc==0 && dirSync ){
    FILE *dfd;                    /* File descriptor open on directory */
    int i;                        /* Iterator variable */
    char zDir[MAXPATHNAME+1];     /* Name of directory containing file zPath */

    /* Figure out the directory name from the path of the file deleted. */
    sqlite3_snprintf(MAXPATHNAME, zDir, "%s", zPath);
    zDir[MAXPATHNAME] = '\0';
    for(i=strlen(zDir); i>1 && zDir[i]!='/'; i++);
    zDir[i] = '\0';

    /* Open a file-descriptor on the directory. Sync. Close. */
    dfd = fopen(zDir, "r");
    if( dfd == (void *)NULL ){
      rc = -1;
    }else{
      rc = fflush(dfd);
      rc = fsync(fileno(dfd));
      fclose(dfd);
    }
  }
  //if (rc == 0)
    //Serial.println("fn:Delete:Success");
  return (rc==0 ? SQLITE_OK : SQLITE_IOERR_DELETE);
}

/*
** Argument zPath points to a nul-terminated string containing a file path.
** If zPath is an absolute path, then it is copied as is into the output 
** buffer. Otherwise, if it is a relative path, then the equivalent full
** path is written to the output buffer.
**
** This function assumes that paths are UNIX style. Specifically, that:
**
**   1. Path components are separated by a '/'. and 
**   2. Full paths begin with a '/' character.
*/
static int RP2040FullPathname(
  sqlite3_vfs *pVfs,              /* VFS */
  const char *zPath,              /* Input path (possibly a relative path) */
  int nPathOut,                   /* Size of output buffer in bytes */
  char *zPathOut                  /* Pointer to output buffer */
){
      //Serial.print("fn: FullPathName");
  //char zDir[MAXPATHNAME+1];
  //if( zPath[0]=='/' ){
  //  zDir[0] = '\0';
  //}else{
  //  if( getcwd(zDir, sizeof(zDir))==0 ) return SQLITE_IOERR;
  //}
  //zDir[MAXPATHNAME] = '\0';
	strncpy( zPathOut, zPath, nPathOut );

  //sqlite3_snprintf(nPathOut, zPathOut, "%s/%s", zDir, zPath);
  zPathOut[nPathOut-1] = '\0';
  //Serial.println("fn:Fullpathname:Success");

  return SQLITE_OK;
}

/*
** The following four VFS methods:
**
**   xDlOpen
**   xDlError
**   xDlSym
**   xDlClose
**
** are supposed to implement the functionality needed by SQLite to load
** extensions compiled as shared objects. This simple VFS does not support
** this functionality, so the following functions are no-ops.
*/
static void *RP2040DlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return 0;
}
static void RP2040DlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
  zErrMsg[nByte-1] = '\0';
}
static void (*RP2040DlSym(sqlite3_vfs *pVfs, void *pH, const char *z))(void){
  return 0;
}
static void RP2040DlClose(sqlite3_vfs *pVfs, void *pHandle){
  return;
}

/*
** Parameter zByte points to a buffer nByte bytes in size. Populate this
** buffer with pseudo-random data.
*/
static int RP2040Randomness(sqlite3_vfs *pVfs, int nByte, char *zByte){
  return SQLITE_OK;
}

/*
** Sleep for at least nMicro microseconds. Return the (approximate) number 
** of microseconds slept for.
*/
static int RP2040Sleep(sqlite3_vfs *pVfs, int nMicro){
  sleep(nMicro / 1000000);
  usleep(nMicro % 1000000);
  return nMicro;
}

/*
** Set *pTime to the current UTC time expressed as a Julian day. Return
** SQLITE_OK if successful, or an error code otherwise.
**
**   http://en.wikipedia.org/wiki/Julian_day
**
** This implementation is not very good. The current time is rounded to
** an integer number of seconds. Also, assuming time_t is a signed 32-bit 
** value, it will stop working some time in the year 2038 AD (the so-called
** "year 2038" problem that afflicts systems that store time this way). 
*/
static int RP2040CurrentTime(sqlite3_vfs *pVfs, double *pTime){
  time_t t = time(0);
  *pTime = t/86400.0 + 2440587.5; 
  return SQLITE_OK;
}

/*
** This function returns a pointer to the VFS implemented in this file.
** To make the VFS available to SQLite:
**
**   sqlite3_vfs_register(sqlite3_ESP32vfs(), 0);
*/
sqlite3_vfs *sqlite3_RP2040vfs(void){
  static sqlite3_vfs RP2040vfs = {
    1,                            // iVersion
    sizeof(RP2040File),             // szOsFile
    MAXPATHNAME,                  // mxPathname
    0,                            // pNext
    "RP2040",                       // zName
    0,                            // pAppData
    RP2040Open,                     // xOpen
    RP2040Delete,                   // xDelete
    RP2040Access,                   // xAccess
    RP2040FullPathname,             // xFullPathname
    RP2040DlOpen,                   // xDlOpen
    RP2040DlError,                  // xDlError
    RP2040DlSym,                    // xDlSym
    RP2040DlClose,                  // xDlClose
    RP2040Randomness,               // xRandomness
    RP2040Sleep,                    // xSleep
    RP2040CurrentTime,              // xCurrentTime
  };
  return &RP2040vfs;
}

static void shox96_0_2c(sqlite3_context *context, int argc, sqlite3_value **argv) {
  int nIn, nOut;
  long int nOut2;
  const unsigned char *inBuf;
  unsigned char *outBuf;
	unsigned char vInt[9];
	int vIntLen;

  assert( argc==1 );
  nIn = sqlite3_value_bytes(argv[0]);
  inBuf = (unsigned char *) sqlite3_value_blob(argv[0]);
  nOut = 13 + nIn + (nIn+999)/1000;
  vIntLen = encode_unsigned_varint(vInt, (uint64_t) nIn);

  outBuf = (unsigned char *) malloc( nOut+vIntLen );
	memcpy(outBuf, vInt, vIntLen);
  nOut2 = shox96_0_2_compress((const char *) inBuf, nIn, (char *) &outBuf[vIntLen], NULL);
  sqlite3_result_blob(context, outBuf, nOut2+vIntLen, free);
}

static void shox96_0_2d(sqlite3_context *context, int argc, sqlite3_value **argv) {
  unsigned int nIn, nOut, rc;
  const unsigned char *inBuf;
  unsigned char *outBuf;
  long int nOut2;
  uint64_t inBufLen64;
	int vIntLen;

  assert( argc==1 );

  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB)
	  return;

  nIn = sqlite3_value_bytes(argv[0]);
  if (nIn < 2){
    return;
  }
  inBuf = (unsigned char *) sqlite3_value_blob(argv[0]);
  inBufLen64 = decode_unsigned_varint(inBuf, vIntLen);
	nOut = (unsigned int) inBufLen64;
  outBuf = (unsigned char *) malloc( nOut );
  //nOut2 = (long int)nOut;
  nOut2 = shox96_0_2_decompress((const char *) (inBuf + vIntLen), nIn - vIntLen, (char *) outBuf, NULL);
  //if( rc!=Z_OK ){
  //  free(outBuf);
  //}else{
    sqlite3_result_blob(context, outBuf, nOut2, free);
  //}
} 

static void unishox1c(sqlite3_context *context, int argc, sqlite3_value **argv) {
  int nIn, nOut;
  long int nOut2;
  const unsigned char *inBuf;
  unsigned char *outBuf;
	unsigned char vInt[9];
	int vIntLen;

  assert( argc==1 );
  nIn = sqlite3_value_bytes(argv[0]);
  inBuf = (unsigned char *) sqlite3_value_blob(argv[0]);
  nOut = 13 + nIn + (nIn+999)/1000;
  vIntLen = encode_unsigned_varint(vInt, (uint64_t) nIn);

  outBuf = (unsigned char *) malloc( nOut+vIntLen );
	memcpy(outBuf, vInt, vIntLen);
  nOut2 = shox96_0_2_compress((const char *) inBuf, nIn, (char *) &outBuf[vIntLen], NULL);
  sqlite3_result_blob(context, outBuf, nOut2+vIntLen, free);
}

static void unishox1d(sqlite3_context *context, int argc, sqlite3_value **argv) {
  unsigned int nIn, nOut, rc;
  const unsigned char *inBuf;
  unsigned char *outBuf;
  long int nOut2;
  uint64_t inBufLen64;
	int vIntLen;

  assert( argc==1 );

  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB)
	  return;

  nIn = sqlite3_value_bytes(argv[0]);
  if (nIn < 2){
    return;
  }
  inBuf = (unsigned char *) sqlite3_value_blob(argv[0]);
  inBufLen64 = decode_unsigned_varint(inBuf, vIntLen);
	nOut = (unsigned int) inBufLen64;
  outBuf = (unsigned char *) malloc( nOut );
  //nOut2 = (long int)nOut;
  nOut2 = shox96_0_2_decompress((const char *) (inBuf + vIntLen), nIn - vIntLen, (char *) outBuf, NULL);
  //if( rc!=Z_OK ){
  //  free(outBuf);
  //}else{
    sqlite3_result_blob(context, outBuf, nOut2, free);
  //}
} 

int registerFunctions(sqlite3 *db, const char **pzErrMsg, const struct sqlite3_api_routines *pThunk) {
  sqlite3_create_function(db, "shox96_0_2c", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, shox96_0_2c, 0, 0);
  sqlite3_create_function(db, "shox96_0_2d", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, shox96_0_2d, 0, 0);
  sqlite3_create_function(db, "unishox1c", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, unishox1c, 0, 0);
  sqlite3_create_function(db, "unishox1d", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, unishox1d, 0, 0);
  return SQLITE_OK;
}

void errorLogCallback(void *pArg, int iErrCode, const char *zMsg) {
  //Serial.printf("(%d) %s\n", iErrCode, zMsg);
}

int sqlite3_os_init(void){
  //sqlite3_config(SQLITE_CONFIG_LOG, errorLogCallback, NULL);
  sqlite3_vfs_register(sqlite3_ESP32vfs(), 1);
  sqlite3_auto_extension((void (*)())registerFunctions);
  return SQLITE_OK;
}

int sqlite3_os_end(void){
  return SQLITE_OK;
}
