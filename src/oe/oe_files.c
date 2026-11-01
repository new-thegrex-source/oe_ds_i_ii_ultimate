/*
 * BREW files (IFileMgr/IFile) for the game: the saves and settings, kept in
 * the game's folder under fs_homepath (e.g. <homepath>/oe/SaveData). Shared by
 * both games; the semantics are those of brew_shim.c in oe-re (GPL-3.0):
 *   - creating fails over an existing file (EFILEEXISTS),
 *   - a write either completes or returns 0 (last error EFSFULL / EFAILED),
 *   - seeking 0 from the current position returns the position (File_tell),
 *   - seeking past the end of a writable file extends it with zeros (the game
 *     grows a new SaveData to its full size with one seek).
 */
#include "oe_local.h"

#include <errno.h>
#include <stdio.h>

/* BREW constants (brew_shim.h) */
enum { _OFM_READ = 1, _OFM_READWRITE = 2, _OFM_CREATE = 4, _OFM_APPEND = 8 };
enum { SUCCESS = 0, EFAILED = 1, EFILEEXISTS = 0x100, EFILENOEXISTS = 0x101, EFSFULL = 0x106 };
enum { _SEEK_START = 0, _SEEK_END = 1, _SEEK_CURRENT = 2 };

#define MAX_OE_FILES 16

typedef struct {
	FILE		*f;
	qboolean	writable;
} oeFile_t;

static oeFile_t	files[MAX_OE_FILES];
static char		saveDir[MAX_OSPATH];
static int		lastError;

void OE_FilesInit( const char *gameDir ) {
	Com_Memset( files, 0, sizeof( files ) );
	Q_strncpyz( saveDir, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), gameDir, "" ), sizeof( saveDir ) );
	/* FS_BuildOSPath leaves a trailing separator for the empty name */
	lastError = SUCCESS;
}

void OE_FilesShutdown( void ) {
	int i;
	for ( i = 0; i < MAX_OE_FILES; i++ ) {
		if ( files[i].f )
			fclose( files[i].f );
		files[i].f = NULL;
	}
}

/* BREW names are plain: letters, digits, '_', '-', '.', no paths */
static const char *OE_FilePath( const char *name ) {
	static char path[MAX_OSPATH];
	const char *p;
	if ( !name || !name[0] || strlen( name ) > 64 || strstr( name, ".." ) )
		return NULL;
	for ( p = name; *p; p++ ) {
		if ( !( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) || ( *p >= '0' && *p <= '9' ) ||
			*p == '_' || *p == '-' || *p == '.' ) )
			return NULL;
	}
	Com_sprintf( path, sizeof( path ), "%s%s", saveDir, name );
	return path;
}

qboolean OE_FileExists( const char *name ) {
	const char *path = OE_FilePath( name );
	FILE *f;
	if ( !path )
		return qfalse;
	f = fopen( path, "rb" );
	if ( !f )
		return qfalse;
	fclose( f );
	return qtrue;
}

int OE_FileOpen( const char *name, int mode ) {
	const char *fm = "rb";
	const char *path = OE_FilePath( name );
	FILE *f;
	int h;

	if ( !path ) {
		lastError = EFAILED;
		return 0;
	}
	for ( h = 0; h < MAX_OE_FILES && files[h].f; h++ )
		;
	if ( h == MAX_OE_FILES ) {
		lastError = EFAILED;
		return 0;
	}
	if ( mode & _OFM_CREATE ) {
		if ( OE_FileExists( name ) ) {	/* BREW refuses to create over an existing file */
			lastError = EFILEEXISTS;
			return 0;
		}
		FS_CreatePath( (char *)path );
		fm = "w+b";
	} else if ( mode & ( _OFM_READWRITE | _OFM_APPEND ) ) {
		fm = "r+b";
	}
	f = fopen( path, fm );
	if ( !f ) {
		lastError = ( mode & _OFM_CREATE ) ? EFAILED : EFILENOEXISTS;
		return 0;
	}
	if ( mode & _OFM_APPEND )
		fseek( f, 0, SEEK_END );
	files[h].f = f;
	files[h].writable = ( mode & ( _OFM_CREATE | _OFM_READWRITE | _OFM_APPEND ) ) != 0;
	lastError = SUCCESS;
	return h + 1;
}

static oeFile_t *OE_File( int h ) {
	if ( h < 1 || h > MAX_OE_FILES || !files[h - 1].f )
		return NULL;
	return &files[h - 1];
}

int OE_FileRead( int h, void *buf, int n ) {
	oeFile_t *f = OE_File( h );
	if ( !f || n <= 0 )
		return 0;
	return (int)fread( buf, 1, (size_t)n, f->f );
}

/* BREW wrote to flash synchronously and a failed write set the last error, which the save code checks; stdio
 * buffers, so flush here to see a full disk at once. Only 0 counts as a failed write for the game, so a partial
 * write returns 0 too. */
int OE_FileWrite( int h, const void *buf, int n ) {
	oeFile_t *f = OE_File( h );
	size_t w;
	if ( !f || n <= 0 )
		return 0;
	errno = 0;
	w = fwrite( buf, 1, (size_t)n, f->f );
	if ( w == (size_t)n && fflush( f->f ) != 0 )
		w = 0;
	if ( w < (size_t)n ) {
		lastError = errno == ENOSPC ? EFSFULL : EFAILED;
		return 0;
	}
	return (int)w;
}

int OE_FileSize( int h ) {
	oeFile_t *f = OE_File( h );
	long cur, end;
	if ( !f )
		return 0;
	cur = ftell( f->f );
	fseek( f->f, 0, SEEK_END );
	end = ftell( f->f );
	fseek( f->f, cur, SEEK_SET );
	return (int)end;
}

int OE_FileSeek( int h, int seekType, int pos ) {
	oeFile_t *f = OE_File( h );
	int whence;
	if ( !f )
		return EFAILED;
	if ( seekType == _SEEK_CURRENT && pos == 0 )
		return (int)ftell( f->f );
	whence = seekType == _SEEK_END ? SEEK_END : seekType == _SEEK_CURRENT ? SEEK_CUR : SEEK_SET;
	if ( fseek( f->f, pos, whence ) != 0 )
		return EFAILED;
	if ( f->writable ) {
		long want = ftell( f->f ), end;
		if ( want > 0 && fseek( f->f, 0, SEEK_END ) == 0 && ( end = ftell( f->f ) ) >= 0 ) {
			static const char zeros[256];
			while ( end < want ) {
				size_t n = want - end < (long)sizeof( zeros ) ? (size_t)( want - end ) : sizeof( zeros );
				if ( fwrite( zeros, 1, n, f->f ) != n ) {
					lastError = EFSFULL;
					return EFAILED;
				}
				end += (long)n;
			}
			if ( fflush( f->f ) != 0 || fseek( f->f, want, SEEK_SET ) != 0 )
				return EFAILED;
		}
	}
	return SUCCESS;
}

void OE_FileClose( int h ) {
	oeFile_t *f = OE_File( h );
	if ( !f )
		return;
	fclose( f->f );
	f->f = NULL;
}

int OE_FileRemove( const char *name ) {
	const char *path = OE_FilePath( name );
	lastError = ( path && remove( path ) == 0 ) ? SUCCESS : EFILENOEXISTS;
	return lastError;
}

int OE_FileLastError( void ) {
	return lastError;
}
