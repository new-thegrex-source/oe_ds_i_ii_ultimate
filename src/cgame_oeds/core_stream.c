/*
 * Resources from orcsandelves.bar, the buffered output stream used for the
 * Config/SaveData files, the plain read file (File) and byte helpers.
 *
 * The original read resources with ISHELL_LoadResDataEx and inflated gzip'd
 * ones through IMemAStream + IUnzipAStream. Here Brew_loadResource() returns
 * the already decompressed payload (cached for the whole run), so:
 *   - InputStream.resData points at the cached bytes; "freeing" it
 *     (ISHELL_FreeResData) only forgets the pointer. The cached block has no
 *     BREW resource header, so the payload starts at offset 0 (the phone's
 *     block started with a header-length byte that was skipped);
 *   - Resource_openCompressed reports the decompressed size (the original
 *     read it from the gzip trailer) and creates no IAStreams;
 *   - Resource_open(inflate) copies the bytes into a malloc'd buffer the
 *     stream owns, as the original's inflate loop did.
 */
#include <stdarg.h>
#include <stdio.h>

#include "core_local.h"
#include "ds_local.h"

/* error format kept in the executable (read from the user's .mod) */
#define MOD_FMT_TRUNCATED 0x3054cu

/* File.error after a short read (Stream_read*) */
#define FILE_ERR_SHORT_READ 0x105

/* ------------------------------------------------------------ byte helpers */

/* 0x00017b60  Little-endian 32-bit at buf+ofs. */
int Bytes_readInt(void *buf, int ofs)
{
	return core_le32((const uint8_t *)buf + ofs);
}

/* 0x00026700  Signed little-endian 16-bit at p+off. */
int Util_shortAt(char *p, int off)
{
	return core_le16((const uint8_t *)p + off);
}

/* 0x000265dc */
int Resource_shiftInt(void *buf, int *pos)
{
	int at = *pos;
	*pos = at + 4;
	return Bytes_readInt(buf, at);
}

/* 0x00026670 */
int Resource_shiftShort(void *buf, int *pos)
{
	int at = *pos;
	*pos = at + 2;
	return Util_shortAt(buf, at);
}

/* ------------------------------------------------------------ InputStream (resources) */

/* 0x00001b30 */
void InputStream_init(InputStream *is, Applet *app, int flag)
{
	is->app = app;
	is->resData = NULL;
	is->data = NULL;
	is->stream = NULL;
	is->memStream = NULL;
	is->size = 0;
	is->flag = flag;
}

/* 0x00002d00  flag != 0: the destructor closes the stream. */
InputStream *InputStream_ctor(InputStream *is, Applet *app, int flag)
{
	if (is == NULL)
		is = calloc(1, sizeof(*is));
	if (is != NULL)
		InputStream_init(is, app, flag);
	return is;
}

/* 0x00001a04  ISHELL_FreeResData: the resource cache keeps the bytes. */
void InputStream_freeResData(InputStream *is)
{
	if (is->resData != NULL)
		is->resData = NULL;
}

/* 0x000024d0 */
void InputStream_freeResData_stub(InputStream *is)
{
	InputStream_freeResData(is);
}

/* 0x00001a24  Release the unzip streams, or the resource block, or the
 * inflated buffer. (After freeing the buffer the size is left as it was.) */
void InputStream_close(InputStream *is)
{
	if (is->stream == NULL) {
		if (is->resData == NULL) {
			if (is->data == NULL)
				return;
			free(is->data);
			is->data = NULL;
			return;
		}
		InputStream_freeResData(is);
	} else {
		/* (the port never creates the IAStreams; kept for the field logic) */
		is->stream = NULL;
		is->memStream = NULL;
	}
	is->data = NULL;
	is->size = 0;
}

/* 0x00002d8c */
void InputStream_dtor(InputStream *is, int del)
{
	if (is->flag != 0)
		InputStream_close(is);
	if (del)
		free(is);
}

/* 0x00001a78 */
void *InputStream_getResData(InputStream *is)
{
	return is->resData;
}

/* 0x00001a7c */
int InputStream_getSize(InputStream *is)
{
	return is->size;
}

/* 0x00001a80 */
void *InputStream_getBuffer(InputStream *is)
{
	return is->data;
}

/* 0x00001e6c  Point the stream at resource resId of orcsandelves.bar. */
int Resource_openRaw(InputStream *is, int resId)
{
	const uint8_t *block;
	size_t size = 0;

	if (is->resData != NULL || is->data != NULL)
		InputStream_close(is);
	block = Brew_loadResource(resId & 0xffff, &size);
	is->resData = (void *)block;
	if (block == NULL) {
		Oe_print("Orcs & Elves DS: orcsandelves.bar has no resource %d\n", resId & 0xffff);
		App_setErrorCode(g_app, 1);
		return 0;
	}
	is->size = (int32_t)size;
	is->data = (uint8_t *)block; /* header length 0 in the port's blocks */
	return 1;
}

/* 0x00002334  Open a gzip'd resource for inflating; size = uncompressed size. */
int Resource_openCompressed(InputStream *is, int resId)
{
	/* Brew_loadResource already inflated the data, so is->size is what the
	 * original read from the gzip trailer (LE u32 in the last 4 bytes). */
	return Resource_openRaw(is, resId);
}

/* 0x000023d8  inflate == 0: the raw resource block. Otherwise the whole
 * uncompressed resource in a malloc'd buffer owned by the stream. */
int Resource_open(InputStream *is, int resId, int inflate)
{
	const uint8_t *src;
	if (inflate == 0)
		return Resource_openRaw(is, resId);
	if (!Resource_openCompressed(is, resId))
		return 0;
	src = is->data;
	/* 4 zeroed spare bytes past the end (callers that read a whole int at the
	 * very end would have read heap garbage on the phone) */
	is->data = calloc(1, (size_t)(is->size > 0 ? is->size : 0) + 4);
	if (is->data == NULL) {
		/* (the phone's inflate loop would have failed the same way) */
		App_setErrorCode(is->app, 0x201);
		App_setErrorStr(is->app, (char *)ModData_str(&g_mod, MOD_FMT_TRUNCATED), resId);
		InputStream_freeResData(is);
		return 0;
	}
	memcpy(is->data, src, (size_t)is->size);
	InputStream_freeResData(is);
	return 1;
}

/* Orcs & Elves DS: a file of the cartridge (NitroFS path, "maps/map_m01.bsp"; a "<path>.lz" is
 * expanded) into a malloc'd buffer the stream owns, like Resource_open(inflate) */
int DsFile_open(InputStream *is, const char *path)
{
	int size;

	if (is->resData != NULL || is->data != NULL)
		InputStream_close(is);
	size = trap_DsFile(path, NULL, 0);
	if (size < 0) {
		Oe_print("Orcs & Elves DS: %s is not on the cartridge\n", path);
		App_setErrorCode(g_app, 1);
		return 0;
	}
	is->data = calloc(1, (size_t)size + 4);
	if (is->data == NULL || trap_DsFile(path, is->data, size) != size) {
		free(is->data);
		is->data = NULL;
		App_setErrorCode(is->app, 0x201);
		return 0;
	}
	is->size = size;
	return 1;
}

/* ------------------------------------------------------------ File (plain read file) */

/* 0x00002c94 */
File *File_ctor(File *f, Applet *app)
{
	if (f == NULL)
		f = calloc(1, sizeof(*f));
	if (f != NULL) {
		f->error = 0;
		f->app = app;
		f->file = NULL;
	}
	return f;
}

/* 0x00002d40 */
void File_dtor(File *f, int del)
{
	if (f->file != NULL)
		IFILE_Release(f->file);
	if (del)
		free(f);
}

/* 0x0001cb3c */
int File_openRead(File *file, char *name)
{
	file->file = IFILEMGR_OpenFile(file->app->fileMgr, name, _OFM_READ);
	return file->file != NULL;
}

/* 0x00002958  IFILE_Seek(_SEEK_CURRENT, 0) returns the current position in BREW. */
int File_tell(File *f)
{
	return IFILE_Seek(f->file, _SEEK_CURRENT, 0);
}

/* 0x000244b4  (named after its loaders' stream; it seeks the read file) */
void InputStream_seek(File *f, int whence, int off)
{
	IFILE_Seek(f->file, whence, off);
}

/* Read n bytes; only a read of nothing at all counts as an error. Bytes not
 * read stay 0 here (the original left them as stack garbage). */
static void file_read(File *s, void *buf, int n)
{
	if (IFILE_Read(s->file, buf, (uint32_t)n) == 0)
		s->error = FILE_ERR_SHORT_READ;
}

/* 0x0001ff60 */
int Stream_readUByte(File *s)
{
	uint8_t v = 0;
	file_read(s, &v, 1);
	return v;
}

/* 0x0001ff88 */
int Stream_readByte(File *s)
{
	int8_t v = 0;
	file_read(s, &v, 1);
	return v;
}

/* 0x0001ffb0  Little-endian 32-bit. */
int Stream_readInt(File *s)
{
	uint8_t b[4] = { 0, 0, 0, 0 };
	file_read(s, b, 4);
	return core_le32(b);
}

/* 0x0001ffd4  (identical twin of Stream_readInt in the binary) */
int Stream_readInt2(File *s)
{
	uint8_t b[4] = { 0, 0, 0, 0 };
	file_read(s, b, 4);
	return core_le32(b);
}

/* 0x0001fff8  Signed little-endian 16-bit. */
int Stream_readShort(File *s)
{
	uint8_t b[2] = { 0, 0 };
	file_read(s, b, 2);
	return core_le16(b);
}

/* ------------------------------------------------------------ OutputStream (buffered write file) */

/* 0x00002cb4 */
OutputStream *OutputStream_ctor(OutputStream *os, Applet *app)
{
	if (os == NULL)
		os = calloc(1, sizeof(*os));
	if (os != NULL) {
		os->app = app;
		memset(os->buf, 0, sizeof(os->buf));
		os->buffered = 0;
		os->written = 0;
		os->error = 0;
		os->file = NULL;
		os->noWrite = 0;
	}
	return os;
}

/* 0x000019bc  Write the buffered bytes; on failure error = IFileMgr last error. */
int OutputStream_flush(OutputStream *os)
{
	int n = os->buffered;
	if (n > 0) {
		os->written += n;
		if (os->noWrite == 0 && (int)IFILE_Write(os->file, os->buf, (uint32_t)n) < 1) {
			os->error = IFILEMGR_GetLastError(os->app->fileMgr);
			return 0;
		}
		os->buffered = 0;
	}
	return 1;
}

/* 0x00002d64 */
void OutputStream_dtor(OutputStream *os, int del)
{
	OutputStream_flush(os);
	if (os->file != NULL)
		IFILE_Release(os->file);
	if (del)
		free(os);
}

/* 0x00002970 */
int OutputStream_getPosition(OutputStream *os)
{
	return os->written + os->buffered;
}

/* 0x0001cb60  mode _OFM_CREATE removes an existing file first; _OFM_APPEND
 * creates the file when it does not exist. With noWrite set the stream only
 * counts bytes. */
int File_open(OutputStream *file, char *name, int mode)
{
	IFileMgr *fm;
	file->error = 0;
	if (file->noWrite != 0)
		return 1;
	fm = file->app->fileMgr;
	if (IFILEMGR_Test(fm, name) == SUCCESS) {
		if (mode == _OFM_CREATE) {
			int r = IFILEMGR_Remove(fm, name);
			if (r != SUCCESS) {
				file->error = r;
				return 0;
			}
		}
	} else if (mode == _OFM_APPEND) {
		mode = _OFM_CREATE;
	}
	file->file = IFILEMGR_OpenFile(fm, name, mode);
	if (file->file != NULL)
		return 1;
	file->error = IFILEMGR_GetLastError(fm);
	IFILEMGR_Remove(fm, name);
	return 0;
}

/* 0x00002c20  Buffered write through the 256-byte buffer. */
void OutputStream_write(OutputStream *os, void *data, int n)
{
	const uint8_t *p = data;
	for (;;) {
		int room;
		if (os->buffered + n <= 0x100) {
			memmove(os->buf + os->buffered, p, (size_t)n);
			os->buffered += n;
			return;
		}
		room = 0x100 - os->buffered;
		memmove(os->buf + os->buffered, p, (size_t)room);
		os->buffered = 0x100;
		n -= room;
		p += room;
		if (!OutputStream_flush(os))
			return;
	}
}

/* 0x0001f790  Formatted text (512-byte buffer). */
void OutputStream_printf(OutputStream *out, char *fmt, ...)
{
	char buf[0x200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	OutputStream_write(out, buf, (int16_t)strlen(buf));
}

/* 0x000244c8  Seek the file (after flushing). In counting mode (noWrite) only
 * the position moves: whence 0 absolute, 2 relative. */
void OutputStream_seek(OutputStream *os, int whence, int off)
{
	if (!OutputStream_flush(os))
		return;
	if (os->noWrite == 0) {
		if (IFILE_Seek(os->file, whence, off) != SUCCESS) {
			os->error = IFILEMGR_GetLastError(os->app->fileMgr);
			return;
		}
		os->written = IFILE_Seek(os->file, _SEEK_CURRENT, 0); /* current position */
	} else if (whence == _SEEK_START) {
		os->written = off;
	} else if (whence == _SEEK_CURRENT) {
		os->written += off;
	}
}

/* 0x0002c6c8  Flush, then write buf straight to the file. */
void OutStream_writeDirect(void *os, void *buf, int n)
{
	OutputStream *o = os;
	if (!OutputStream_flush(o))
		return;
	o->written += n;
	if (o->noWrite == 0 && (int)IFILE_Write(o->file, buf, (uint32_t)n) < 1)
		o->error = IFILEMGR_GetLastError(o->app->fileMgr);
}

/* The values go out little-endian, the ARM's memory order. */

/* 0x0002c6a8 */
void OutStream_writeByte(void *os, int v)
{
	uint8_t b = (uint8_t)v;
	OutputStream_write(os, &b, 1);
}

/* 0x0002c6b8 */
void OutStream_writeBool(void *os, int v)
{
	uint8_t b = (uint8_t)v;
	OutputStream_write(os, &b, 1);
}

/* 0x0002c710 */
void OutStream_writeInt(void *os, int v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	OutputStream_write(os, b, 4);
}

/* 0x0002c724  (identical twin of OutStream_writeInt in the binary) */
void OutStream_writeLong(void *os, int v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	OutputStream_write(os, b, 4);
}

/* 0x0002c738 */
void OutStream_writeShort(void *os, int v)
{
	uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
	OutputStream_write(os, b, 2);
}
