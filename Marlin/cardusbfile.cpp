#include "MarlinConfig.h"

#if ENABLED(FYS_USBDISK)
#include <string.h>
#include "Marlin.h"
#include "cardusbfile.h"

#include "CH376_hal.h"
#include "CH376_file_sys.h"
#include "CH376_debug.h"

#include "serial.h"

USBFile::USBFile(){
  dirStartClust = 0;
  memset(name,0,sizeof(name));
  size = 0;  
  attr = 0;
  is_open = false;
  curPosition_ = 0;
  writeError = false;
}

// 在sdcard 中，主要是将文件当前的位置置0，和文件所在簇号置0
void USBFile::rewind(){
  curPosition_=0;
}

void USBFile::getFilename(char * const filename) {
  strcpy(filename,name);
}

void USBFile::setFilename(char * const filename) {
  strcpy(name,filename);
}


UINT32 USBFile::getDirStartClust() {
  return dirStartClust;
}

void USBFile::setDirStartClust(UINT32 clust) {
  dirStartClust = clust;
}

UINT8 USBFile::getAttr() {
  return attr;
}

void USBFile::setAttr(UINT8 a) {
  attr=a;
}

// 打开一次 就可以获得此文件或目录的所有属性，path 表示文件名，dirFile 表示父目录，
// oflag 表示打开文件是 读还是写或者其他
// 这个函数由于进行了 FileOpen 操作，不知道会不会打断 marlin 的流程
// 暂时不用这个函数
// 这里多层打开 可能会出问题 再想想
bool USBFile::open(USBFile* dirFile, const char* path, uint8_t oflag) {
  UINT8			s;
  UINT8   buf[64];

  if (!dirFile || isOpen()) return false;

  // 将当前目录的上级目录的起始簇号设置为当前簇号,相当于打开上级目录
  // 在同一个目录中的所有文件都保有这 目录其实簇号
	CH376WriteVar32( VAR_START_CLUSTER, dirFile->dirStartClust );  
	SERIAL_ECHO("List Directory: ");
  CH376DebugOut( path );

  // 打开多级目录下的文件或者目录 
  s = CH376FileOpenPath( path );  
	if ( s != USB_INT_SUCCESS && s != ERR_OPEN_DIR ) {
    mStopIfError(s);
    return false;
  }
  
  // 文件名
  strcpy(name,path);

  // 获取目录的起始簇号 
	dirStartClust = CH376ReadVar32( VAR_START_CLUSTER );  
	//CH376FileClose( FALSE );  /* 对于根目录一定要关闭 */

  SERIAL_ECHO("clust num:");
  SERIAL_ECHO(dirFile->dirStartClust);
  SERIAL_ECHO("-");
  SERIAL_ECHO(dirStartClust);
  
  /* 读取当前文件的目录信息FAT_DIR_INFO,将相关数据调到内存中 */
  s = CH376DirInfoRead( );  /* 读取当前文件的目录信息FAT_DIR_INFO,将相关数据调到内存中 */
	if ( s != USB_INT_SUCCESS ) {
    mStopIfError(s);
    return false;
  }
	CH376ReadBlock( buf );  /* 从内存缓冲区读取FAT_DIR_INFO数据块,返回长度总是sizeof(FAT_DIR_INFO) */
  CH376EndDirInfo( );  /* 获取完FAT_DIR_INFO结构 */      

  P_FAT_DIR_INFO pDir = (P_FAT_DIR_INFO)buf;  /* 当前文件目录信息 */
	if ( pDir->DIR_Name[0] != '.' ) {  /* 不是本级或者上级目录名则继续,否则必须丢弃不处理 */
		if ( pDir->DIR_Name[0] == 0x05 ) pDir->DIR_Name[0] = 0xE5;  /* 特殊字符替换 */
    attr = pDir->DIR_Attr;
    size = pDir->DIR_FileSize;    
  }
  
  // 
  is_open = true;

  return true;
}


bool USBFile::isOpen() {
  return is_open;
}

void USBFile::setFileOpenState(bool flag){
  is_open = flag;
}

bool USBFile::close() {
  UINT8 s = CH376FileClose( TRUE );  /* 关闭文件,对于字节读写建议自动更新文件长度 */
  if ( s != USB_INT_SUCCESS ) {
    mStopIfError(s);
    return false;  
  }
  is_open = false;
  return true;
}

UINT32 USBFile::curPosition() {
  return curPosition_;
}

bool USBFile::seekSet(UINT32 pos) {
  UINT32 s = CH376ByteLocate( pos );  /* 以字节为单位移动当前文件指针到上次复制结束位置 */
  if ( s != USB_INT_SUCCESS ) {
    mStopIfError(s);
    return false;  
  }

  curPosition_ = pos;
  
  return true;
}

// 读取一个字节，错误则返回-1
INT16 USBFile::read(){
  UINT8 s;
  UINT8 buffer;
  UINT16 realCnt; // 实际读出来的字节数

  // 以字节为单位移动当前文件指针到上次复制结束位置
  /*
  s = CH376ByteLocate( curPosition_ );  
	  if ( s != USB_INT_SUCCESS ) {
    mStopIfError(s);
    return -1;  
  }
  */

  // 以字节为单位从当前位置读取数据块,返回实际长度在realCnt中 
  s = CH376ByteRead( &buffer, 1, &realCnt );  			
  //s = CH376ByteReadOne(&buffer, &realCnt);
  if ( s != USB_INT_SUCCESS ) {
    mStopIfError(s);
    return -1;  
  }

  if(realCnt!=1) { // 读取到的和需要读取的字节不一样，应该是到文件末尾了
    SERIAL_ECHO("readCnt:");
    SERIAL_PRINTLN(realCnt,10);    
    return -1;
  }

  curPosition_++;
  
  return buffer;
}

/**
 * Get a string from a file.
 *
 * fgets() reads bytes from a file into the array pointed to by \a str, until
 * \a num - 1 bytes are read, or a delimiter is read and transferred to \a str,
 * or end-of-file is encountered. The string is then terminated
 * with a null byte.
 *
 * fgets() deletes CR, '\\r', from the string.  This insures only a '\\n'
 * terminates the string for Windows text files which use CRLF for newline.
 *
 * \param[out] str Pointer to the array where the string is stored.
 * \param[in] num Maximum number of characters to be read
 * (including the final null byte). Usually the length
 * of the array \a str is used.
 * \param[in] delim Optional set of delimiters. The default is "\n".
 *
 * \return For success fgets() returns the length of the string in \a str.
 * If no data is read, fgets() returns zero for EOF or -1 if an error occurred.
 **/
INT16 USBFile::fgets(char* str, INT16 num, char* delim) {
  char ch;
  INT16 n = 0;
  INT16 r = -1;
  while ((n + 1) < num && (r = read()) == 1) {
    // delete CR
    if (ch == '\r') continue;
    str[n++] = ch;
    if (!delim) {
      if (ch == '\n') break;
    }
    else {
      if (strchr(delim, ch)) break;
    }
  }
  if (r < 0) {
    // read error
    return -1;
  }
  str[n] = '\0';
  return n;
}


void USBFile::write(UINT8 *pData) {
  if(!is_open) {
    mStopIfError(0);
  }

  // 先获取pData指向数据大小
  UINT16 len=0;
  UINT8 *p = pData;
  while(p!='\0') { len++; p++;}

  // 以字节为单位向当前位置写入数据块,不知道是否有最大写入字节的限制
  UINT8 s = CH376ByteWrite( pData, len, NULL );  
	if(s!=USB_INT_SUCCESS) writeError = true;
  else writeError = false;
}

bool USBFile::isDir() {
  return (attr & ATTR_DIRECTORY);
}

UINT32 USBFile::fileSize(){
  return size;
}

/**
 * Read the next entry in a directory.
 *
 * \param[out] dir The dir_t struct that will receive the data.
 *
 * \return For success readDir() returns the number of bytes read.
 * A value of zero will be returned if end of file is reached.
 * If an error occurs, readDir() returns -1.  Possible errors include
 * readDir() called before a directory has been opened, this is not
 * a directory file or an I/O error occurred.
 * 读取目录的下一个 entry，暂时没有实现，CH376 的长文件名获取不能在枚举中进行
 */
int8_t USBFile::readDir(FAT_DIR_INFO* dir, char* longFilename) {
  int16_t n;
  // if not a directory file or miss-positioned return an error
  if (!isDir() || (0x1F & curPosition_)) return -1;

  // If we have a longFilename buffer, mark it as invalid.
  // If a long filename is found it will be filled automatically.
  if (longFilename) longFilename[0] = '\0';

  /*
  while (1) {
    // 获取一个entry信息
    n = read(dir, sizeof(dir_t));

    // 如果读取到的是目录或者文件则退出 while，并返回
  }
  */

  return 0;
}

/**
 * Remove a file.
 *
 * The directory entry and all data for the file are deleted.
 *
 * \param[in] dirFile The directory that contains the file.
 * \param[in] path Path for the file to be removed.
 *
 * \note This function should not be used to delete the 8.3 version of a
 * file that has a long name. For example if a file has the long name
 * "New Text Document.txt" you should not delete the 8.3 name "NEWTEX~1.TXT".
 *
 * \return true for success, false for failure.
 * Reasons for failure include the file is a directory, is read only,
 * \a dirFile is not a directory, \a path is not found
 * or an I/O error occurred.
 */
bool USBFile::remove(USBFile* dirFile, const char* path) {
  // 将当前目录的上级目录的起始簇号设置为当前簇号,相当于打开上级目录
  // 在同一个目录中的所有文件都保有这 目录其实簇号
  CH376WriteVar32( VAR_START_CLUSTER, dirFile->dirStartClust );  
  SERIAL_ECHO("remove: ");
  CH376DebugOut( path );

  CH376DebugOut( "Erase" );
	UINT8 s = CH376FileErase( path);  //删除文件
	if ( s != USB_INT_SUCCESS ) {
    CH376DebugOutErr( (UINT16)s );  //显示错误
    return false;
  }

  // 重新初始化一下所有属性
  dirStartClust = 0;
  memset(name,0,sizeof(name));
  size = 0;
  attr = 0;
  is_open = false;
  curPosition_ = 0;
  writeError = false;

  return true;
}



#endif
