/*
 * Resources, input/output streams, files and BMP images.
 *
 * The original read resources with ISHELL_LoadResDataEx from orcsandelves2.bar
 * and inflated them through an IUnzipAStream. Here Brew_loadResource() hands us
 * the already decompressed bytes (cached for the whole run), so:
 *   - Resource_openRaw points the stream at the cached bytes (ownsResData = 1,
 *     "freeing" them only clears the flag, like IShell_FreeResData);
 *   - Resource_openCompressed keeps that and reports the decompressed size;
 *   - Resource_open(loadAll) copies them into a malloc'd buffer the stream (or
 *     a caller that takes it over, like Sound_loadSound) owns.
 * Everything above that layer (Stream_read clamping, the 0x4000-byte
 * DataBuffer at Applet+0x14c and its refill logic) works as in the original.
 */
#include <stdio.h>

#include "core_local.h"

/* error message formats kept in the executable (read from the user's .mod) */
#define MOD_FMT_OUT_OF_MEMORY 0x45e80u /* "... %d" */
#define MOD_FMT_BAD_BPP       0x4710cu /* "... %d ... %d" (resource, bpp) */

/* ------------------------------------------------------------ InputStream */

/* 0x00003d8c */
InputStream *InputStream_ctor(InputStream *self)
{
	if (!self)
		self = calloc(1, sizeof(*self));
	if (self) {
		self->kind = 0;
		self->data = NULL;
		self->size = 0;
		self->pos = 0;
		self->f00 = 0;
		self->isOpen = 1;
		self->ownsResData = 0;
		self->unzipStream = NULL;
		self->memStream = NULL;
		self->file = NULL;
	}
	return self;
}

/* 0x0001b8e4  IShell_FreeResData: the resource cache keeps the bytes. */
void ResFile_freeResData(InputStream *rf)
{
	if (rf->ownsResData != 0)
		rf->ownsResData = 0;
}

/* 0x00009f10  Release whatever the stream holds (resource data, heap copy, file). */
void InputStream_close(InputStream *is)
{
	/* (the unzip/memory IAStreams of the original are never created here) */
	if (is->unzipStream || is->memStream) {
		is->unzipStream = NULL;
		is->memStream = NULL;
		is->data = NULL;
		is->size = 0;
	}
	if (is->ownsResData == 0) {
		if (is->data) {
			free(is->data);
			is->data = NULL;
		}
	} else {
		ResFile_freeResData(is);
		is->data = NULL;
		is->size = 0;
	}
	if (is->file) {
		IFILE_Release(is->file);
		is->file = NULL;
	}
}

/* 0x00004040 */
void InputStream_dtor(InputStream *self, int del)
{
	if (self->isOpen != 0)
		InputStream_close(self);
	if (del)
		free(self);
}

/* 0x0001bbb0 */
int InputStream_getSize(InputStream *is)
{
	return is->size;
}

/* 0x0001bbb8 */
void *InputStream_getBuffer(InputStream *is)
{
	return is->data;
}

/* 0x0003173c  Copy up to len bytes from the read position to dst+off. */
int Stream_read(InputStream *s, void *dst, int off, int len)
{
	uint32_t end = (uint32_t)s->pos + (uint32_t)len;
	if ((uint32_t)s->size < end)
		len -= (int)(end - (uint32_t)s->size);
	if (len > 0)
		memmove((uint8_t *)dst + off, s->data + s->pos, (size_t)len);
	s->pos += len;
	return len;
}

/* 0x00031648 */
int Stream_readUByte(InputStream *s)
{
	return s->data[s->pos++];
}

/* 0x00031720  (identical twin of Stream_readUByte in the binary) */
int Stream_readUByte2(InputStream *s)
{
	return s->data[s->pos++];
}

/* 0x00031630 */
int Stream_readBool(InputStream *s)
{
	return Stream_readUByte(s) != 0;
}

/* 0x00031704 */
int Stream_readByte(InputStream *s)
{
	return (int8_t)s->data[s->pos++];
}

/* 0x000316ec  Signed little-endian 16-bit. */
int Stream_readShort(InputStream *s)
{
	int pos = s->pos;
	s->pos = pos + 2;
	return Util_shortAt((char *)s->data, pos);
}

/* 0x000316d4 */
int Stream_readInt(InputStream *s)
{
	int pos = s->pos;
	s->pos = pos + 4;
	return core_le32(s->data + pos);
}

/* 0x000317a8  Read len bytes from the stream into the work buffer and rewind it. */
void ByteBuf_fill(DataBuffer *buf, InputStream *s, int len)
{
	buf->pos = 0;
	buf->len = Stream_read(s, buf->data, 0, len);
}

/* 0x00031664  {int count; int values[count]} -> out (values malloc'd). */
void Stream_readIntArray(DataBuffer *tmp, InputStream *s, IntArray *out)
{
	int count, i;
	int32_t *values;
	ByteBuf_fill(tmp, s, 4);
	count = Resource_shiftInt(tmp);
	values = malloc((size_t)(count > 0 ? count : 1) * sizeof(int32_t));
	ByteBuf_fill(tmp, s, count * 4);
	for (i = 0; i < count; i++)
		values[i] = Resource_shiftInt(tmp);
	out->count = count;
	out->data = values;
}

/* 0x000023cc */
void Array_free(IntArray *arr)
{
	free(arr->data);
	arr->data = NULL;
	arr->count = 0;
}

/* 0x000040c4 */
void Array_dtor(IntArray *arr, int del)
{
	free(arr->data);
	arr->data = NULL;
	arr->count = 0;
	if (del)
		free(arr);
}

/* 0x00008148  Skip n bytes by reading them through the work buffer in
 * 0x4000-byte chunks. */
void InputStream_skipChunked(DataBuffer *buf, InputStream *file, int n, int x)
{
	(void)x;
	while (n > 0) {
		int len = n > 0x4000 ? 0x4000 : n;
		Stream_read(file, buf->data, 0, len);
		n -= len;
	}
}

/* ------------------------------------------------------------ DataBuffer (Applet+0x14c) */

/* 0x00001044  Move the unread tail to the front and top the buffer up. */
void Resource_bufferRefill(DataBuffer *res, InputStream *stream)
{
	int left = res->len - res->pos;
	int got;
	if (left > 0)
		memmove(res->data, res->data + res->pos, (size_t)left);
	res->pos = 0;
	res->len = left;
	got = Stream_read(stream, res->data, left, 0x4000 - left);
	if (got > 0)
		res->len += got;
}

/* 0x00001814  Make sure at least n unread bytes are buffered. */
void Resource_bufferEnsure(DataBuffer *res, InputStream *stream, int n)
{
	if (res->len - res->pos < n)
		Resource_bufferRefill(res, stream);
}

/* 0x000024b8 */
void Resource_bufferFill(DataBuffer *res, InputStream *stream)
{
	res->len = 0;
	res->pos = 0;
	Resource_bufferRefill(res, stream);
}

/* 0x0000363c */
int Resource_bufferByte(DataBuffer *res, int i)
{
	return res->data[i];
}

/* 0x00008380 */
int InputStream_peekByte(DataBuffer *buf, int pos)
{
	return buf->data[pos];
}

/* 0x0003b8a0 */
int Resource_shiftByte(DataBuffer *r)
{
	return r->data[r->pos++];
}

/* 0x0003b9f8 */
int Resource_shiftUByte(DataBuffer *r)
{
	return r->data[r->pos++];
}

/* 0x0003b8b8  Map coordinate: byte * 8. */
int Resource_shiftCoord(DataBuffer *r)
{
	return r->data[r->pos++] << 3;
}

/* 0x0003b8d8 */
int Resource_shiftInt(DataBuffer *r)
{
	int v = core_le32(r->data + r->pos);
	r->pos += 4;
	return v;
}

/* 0x0003b9c4 */
int Resource_shiftShort(DataBuffer *r)
{
	int v = core_le16(r->data + r->pos);
	r->pos += 2;
	return v;
}

/* 0x0003ba10 */
int Resource_shiftUShort(DataBuffer *r)
{
	int v = (uint16_t)core_le16(r->data + r->pos);
	r->pos += 2;
	return v;
}

/* 0x0003bacc */
int Resource_shortAt(DataBuffer *r, int off)
{
	return core_le16(r->data + off);
}

/* 0x0003baf0  Signed LE 16-bit at p+off. */
int Util_shortAt(char *p, int off)
{
	return core_le16((const uint8_t *)p + off);
}

/* 0x00024528  LE 32-bit at buf->data + ofs. */
int Bytes_readIntAt8(DataBuffer *buf, int ofs)
{
	return core_le32(buf->data + ofs);
}

/* 0x00024564  LE 32-bit at raw address (uint8_t *)buf + ofs. Callers pass
 * either a DataBuffer or a plain byte buffer (stream data) here. */
int Bytes_readInt(DataBuffer *buf, int ofs)
{
	return core_le32((const uint8_t *)buf + ofs);
}

/* ------------------------------------------------------------ resources */

/* 0x0002567c  Point the stream at resource resId of orcsandelves2.bar. */
int Resource_openRaw(InputStream *stream, int resId)
{
	const uint8_t *p;
	size_t size = 0;
	if (stream->ownsResData != 0 || stream->data != NULL)
		InputStream_close(stream);
	p = Brew_loadResource((int16_t)resId, &size);
	stream->ownsResData = p != NULL;
	if (p) {
		stream->data = (uint8_t *)p;
		stream->size = (int32_t)size;
	} else {
		App_setErrorCode(g_app, 1);
	}
	return p != NULL;
}

/* 0x0002b06c  Open the resource for inflating; size = uncompressed size. */
int Resource_openCompressed(InputStream *stream, int resId)
{
	/* Brew_loadResource already inflated the data, so its size is the
	 * uncompressed size the original read from the gzip trailer. */
	return Resource_openRaw(stream, (int16_t)resId);
}

/* 0x00028250  loadAll != 0: whole uncompressed resource in a malloc'd buffer. */
int Resource_open(InputStream *stream, int resId, int loadAll)
{
	Applet *app = g_app;
	const uint8_t *src;
	stream->kind = 2;
	stream->pos = 0;
	stream->f00 = 0;
	if (loadAll == 0)
		return Resource_openRaw(stream, (int16_t)resId);
	if (!Resource_openCompressed(stream, resId))
		return 0;
	src = stream->data;
	/* 4 zeroed spare bytes: a truncated save is rejected by its magic checks,
	 * which read a whole int (the phone just read past the heap block). */
	stream->data = calloc(1, (size_t)(stream->size > 0 ? stream->size : 0) + 4);
	if (stream->data == NULL) {
		App_setErrorCode(app, 1);
		App_setErrorStr(app, (char *)ModData_str(&g_mod, MOD_FMT_OUT_OF_MEMORY), stream->size);
		return 0;
	}
	memcpy(stream->data, src, (size_t)stream->size);
	ResFile_freeResData(stream);
	return 1;
}

/* ------------------------------------------------------------ files */

/* 0x00025eec  Read a whole file from the app's directory into the stream. */
int FileStream_open(InputStream *stream, String *name)
{
	Applet *app = g_app;
	IFile *f;
	stream->kind = 1;
	stream->pos = 0;
	stream->f00 = 0;
	/* IFILEMGR_GetInfo: fails when the file does not exist */
	if (IFILEMGR_Test(app->fileMgr, String_cstr(name)) != SUCCESS)
		return 0;
	f = IFILEMGR_OpenFile(app->fileMgr, String_cstr(name), _OFM_READ);
	stream->size = f ? IFILE_Size(f) : 0;
	/* 4 zeroed spare bytes: a truncated save is rejected by its magic checks,
	 * which read a whole int (the phone just read past the heap block). */
	stream->data = calloc(1, (size_t)(stream->size > 0 ? stream->size : 0) + 4);
	if (stream->data == NULL) {
		IFILE_Release(f);
		return 0;
	}
	stream->file = f;
	if (f) {
		if (IFILE_Read(f, stream->data, (uint32_t)stream->size) != 0)
			return 1;
		IFILE_Release(stream->file);
		stream->file = NULL;
	}
	return 0;
}

/* 0x00003ddc */
OutputStream *OutputStream_ctor(OutputStream *self)
{
	if (!self)
		self = calloc(1, sizeof(*self));
	if (self) {
		self->app = g_app;
		memset(self->buf, 0, sizeof(self->buf));
		self->pos = 0;
		self->total = 0;
		self->error = 0;
		self->file = NULL;
		self->memoryOnly = 0;
	}
	return self;
}

/* 0x0001b798  Write the buffered bytes; on failure error = IFileMgr last error. */
int OutputStream_flush(OutputStream *os)
{
	int n = os->pos;
	if (n > 0) {
		os->total += n;
		if (os->memoryOnly == 0 && (int)IFILE_Write(os->file, os->buf, (uint32_t)n) < 1) {
			os->error = IFILEMGR_GetLastError(os->app->fileMgr);
			return 0;
		}
		os->pos = 0;
	}
	return 1;
}

/* 0x00009fd0  Flush and close. */
void Resource_dispose(OutputStream *os)
{
	OutputStream_flush(os);
	if (os->file) {
		IFILE_Release(os->file);
		os->file = NULL;
	}
}

/* 0x00004074 */
void OutputStream_dtor(OutputStream *self, int del)
{
	Resource_dispose(self);
	if (del)
		free(self);
}

/* 0x0002ccb0  mode 4 = create (an existing file is removed first),
 * 8 = append (falls back to create when the file does not exist). */
int File_open(OutputStream *file, String *name, int mode)
{
	IFileMgr *fm;
	int r;
	if (file->file != NULL) {
		file->error = 1;
		return 0;
	}
	file->error = 0;
	if (file->memoryOnly != 0)
		return 1;
	fm = file->app->fileMgr;
	if (IFILEMGR_Test(fm, String_cstr(name)) == SUCCESS) {
		if (mode == _OFM_CREATE) {
			r = IFILEMGR_Remove(fm, String_cstr(name));
			if (r != SUCCESS) {
				file->error = r;
				return 0;
			}
		}
	} else if (mode == _OFM_APPEND) {
		mode = _OFM_CREATE;
	}
	file->file = IFILEMGR_OpenFile(fm, String_cstr(name), mode);
	if (file->file == NULL) {
		file->error = IFILEMGR_GetLastError(fm);
		IFILEMGR_Remove(fm, String_cstr(name));
		return 0;
	}
	return 1;
}

/* 0x00045cdc  Buffered write through the 256-byte buffer. */
void OutStream_write(OutputStream *os, void *buf, int n)
{
	const uint8_t *p = buf;
	for (;;) {
		int room;
		if (os->pos + n <= 0x100) {
			memmove(os->buf + os->pos, p, (size_t)n);
			os->pos += n;
			return;
		}
		room = 0x100 - os->pos;
		memmove(os->buf + os->pos, p, (size_t)room);
		os->pos = 0x100;
		n -= room;
		p += room;
		if (!OutputStream_flush(os))
			return;
	}
}

/* 0x00045c6c  The value goes out little-endian, like the ARM's memory order. */
void OutStream_writeByte(OutputStream *os, int v)
{
	uint8_t b = (uint8_t)v;
	OutStream_write(os, &b, 1);
}

/* 0x00045c88 */
void OutStream_writeBool(OutputStream *os, int v)
{
	uint8_t b = (uint8_t)v;
	OutStream_write(os, &b, 1);
}

/* 0x00045ca4 */
void OutStream_writeInt(OutputStream *os, int v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	OutStream_write(os, b, 4);
}

/* 0x00045cc0 */
void OutStream_writeShort(OutputStream *os, int v)
{
	uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
	OutStream_write(os, b, 2);
}

/* ------------------------------------------------------------ images */

/* Release an IDIB view created by Image_load. */
static void dib_release(IDIB *dib)
{
	free(dib);
}

/* 0x0001b938 */
void App_freeImage(Applet *app, Image *img)
{
	int before = App_getUsedMemory(app);
	if (img != NULL && img->bitmap != NULL) {
		dib_release(img->dib);
		IBITMAP_Release(img->bitmap);
		img->dib = NULL;
		img->bitmap = NULL;
	}
	app->imageMem -= before - App_getUsedMemory(app);
}

/* 0x00025ff4  Load image resource 3000+bmpIdx (a 4- or 8-bit Windows BMP) into
 * an RGB565 bitmap. With `transparent`, magenta (0xff00ff) becomes the
 * bitmap's transparent colour. Afterwards image->dib describes the bitmap's
 * pixels (the original queried the bitmap's IDIB interface). */
void Image_load(Applet *app, int bmpIdx, Image *image, int transparent)
{
	InputStream res;
	const uint8_t *bmp;
	int pixelOfs, width, height, bpp, numColors, rowBits, srcStride;
	uint16_t palette[256];
	IBitmap *bitmap;
	AEEBitmapInfo info;
	int memBefore = App_getUsedMemory(app);

	InputStream_ctor(&res);
	if (!Resource_open(&res, (int16_t)(bmpIdx + 3000), 1)) {
		g_app->errorCode = 0x16;
		goto out;
	}
	bmp = InputStream_getBuffer(&res);
	pixelOfs = core_le32(bmp + 10);
	width = core_le32(bmp + 0x12) & 0xffff;
	height = core_le32(bmp + 0x16) & 0xffff;
	bpp = (uint16_t)Util_shortAt((char *)bmp, 0x1c);
	numColors = core_le32(bmp + 0x2e);
	if (bpp != 4 && bpp != 8) {
		g_app->errorCode = 1;
		App_setErrorStr(app, (char *)ModData_str(&g_mod, MOD_FMT_BAD_BPP), bmpIdx, bpp);
		goto out;
	}
	if (numColors == 0)
		numColors = 1 << bpp;
	numColors &= 0xffff;

	/* Palette (BGRA quads after the 40-byte info header) -> native colours.
	 * Converting it up front is what IBITMAP_BltIn did from the 4/8-bit IDIB. */
	memset(palette, 0, sizeof(palette));
	for (int i = 0; i < numColors && i < 256; i++) {
		const uint8_t *q = bmp + 0x36 + i * 4;
		palette[i] = (uint16_t)IBITMAP_RGBToNative(NULL, MAKE_RGB(q[2], q[1], q[0]));
	}
	image->transparent = transparent;

	/* rows are padded to 32 bits and stored bottom-up */
	rowBits = bpp * width;
	if (rowBits & 0x1f)
		rowBits = (rowBits / 32) * 32 + 32;
	srcStride = rowBits / 8;

	if (IBITMAP_CreateCompatibleBitmap(app->deviceBitmap, &image->bitmap, width, height) != SUCCESS) {
		InputStream_close(&res);
		g_app->errorCode = 1;
		goto out;
	}
	bitmap = image->bitmap;
	for (int y = 0; y < height; y++) {
		const uint8_t *row = bmp + pixelOfs + srcStride * (height - 1 - y);
		uint16_t *dst = bitmap->px + y * bitmap->pitch;
		for (int x = 0; x < width; x++) {
			int idx = bpp == 8 ? row[x] : (x & 1 ? row[x >> 1] & 0x0f : row[x >> 1] >> 4);
			dst[x] = palette[idx];
		}
	}
	InputStream_close(&res);
	Brew_hdBitmap(bmpIdx + 3000, bitmap);   /* port: a mod's picture (or the id Tech 3 font) in its place */

	if (IBITMAP_GetInfo(bitmap, &info) != SUCCESS) {
		g_app->errorCode = 1;
		goto out;
	}
	image->width = (int32_t)info.cx;
	image->height = (int32_t)info.cy;
	image->depth = (int32_t)info.nDepth;
	if (transparent != 0 &&
	    IBITMAP_SetTransparencyColor(bitmap, IBITMAP_RGBToNative(bitmap, MAKE_RGB(0xff, 0x00, 0xff))) != SUCCESS) {
		g_app->errorCode = 1;
		goto out;
	}

	/* the bitmap's IDIB view (RGB565, pitch in bytes) */
	image->dib = calloc(1, sizeof(IDIB));
	if (image->dib == NULL) {
		g_app->errorCode = 1;
	} else {
		image->dib->pBmp = (uint8_t *)bitmap->px;
		image->dib->cx = (uint16_t)bitmap->width;
		image->dib->cy = (uint16_t)bitmap->height;
		image->dib->nPitch = (int16_t)(bitmap->pitch * 2);
		image->dib->nDepth = 16;
		image->dib->nColorScheme = 16; /* IDIB_COLORSCHEME_565 */
		image->dib->ncTransparent = bitmap->transparent;
	}
	app->imageMem += App_getUsedMemory(app) - memBefore;
out:
	InputStream_dtor(&res, 0);
}
