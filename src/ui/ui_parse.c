/*
 * The menu files' tokens, with the directives of Tremulous' precompiler that
 * its menus use: #include "file", #define NAME tokens (no arguments), #undef,
 * #ifdef / #ifndef / #else / #endif, and C enums (menudef.h), whose names
 * become defines of their values. Numbers are read as Tremulous does: a
 * number, a minus and a number, or an expression in parentheses.
 */
#include "ui_local.h"

#define MAX_SOURCES		16
#define MAX_DEFINES		1024
#define MAX_IFS			32
#define MAX_FILE		0x40000

typedef struct {
	char		*text;				/* malloc'd */
	int			pos;
	int			line;
	char		name[64];			/* the file, or the define being expanded */
	qboolean	isDefine;
	qboolean	lineStart;			/* nothing but blanks since the last newline */
} source_t;

typedef struct {
	char		name[64];
	char		*value;
} define_t;

static source_t	sources[MAX_SOURCES];
static int		numSources;
static define_t	defines[MAX_DEFINES];
static int		numDefines;
static int		ifs[MAX_IFS];		/* 1: this level's tokens are read */
static int		numIfs;
static token_t	unread;
static qboolean	hasUnread;
static token_t	lastToken;			/* for warnings */

static char *Parse_Strdup( const char *s ) {
	size_t	n = strlen( s ) + 1;
	char	*d = malloc( n );
	if ( d ) {
		memcpy( d, s, n );
	}
	return d;
}

void Parse_Warning( const token_t *t, const char *fmt, ... ) {
	char	buf[512];
	va_list	ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	if ( !t ) {
		t = &lastToken;
	}
	UI_Printf( "^3%s, line %d: %s\n", t->file, t->line, buf );
}

/* ---------------------------------------------------------------- sources */

static qboolean Parse_PushFile( const char *path ) {
	source_t	*s;
	char		*buf;
	int			len;

	if ( numSources == MAX_SOURCES ) {
		UI_Printf( "^3%s: too many nested files\n", path );
		return qfalse;
	}
	buf = malloc( MAX_FILE );
	if ( !buf ) {
		return qfalse;
	}
	len = trap_FS_ReadFile( path, buf, MAX_FILE );
	if ( len < 0 ) {
		free( buf );
		return qfalse;
	}
	if ( len >= MAX_FILE ) {
		UI_Printf( "^3%s: longer than %d bytes, cut\n", path, MAX_FILE - 1 );
	}
	s = &sources[numSources++];
	memset( s, 0, sizeof( *s ) );
	s->text = buf;
	s->line = 1;
	s->lineStart = qtrue;
	strncpy( s->name, path, sizeof( s->name ) - 1 );
	return qtrue;
}

static qboolean Parse_DefineActive( const char *name ) {
	int i;
	for ( i = 0; i < numSources; i++ ) {
		if ( sources[i].isDefine && !strcmp( sources[i].name, name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void Parse_PushDefine( const define_t *d ) {
	source_t *s;
	if ( numSources == MAX_SOURCES ) {
		UI_Printf( "^3#define %s: nested too deep\n", d->name );
		return;
	}
	s = &sources[numSources++];
	memset( s, 0, sizeof( *s ) );
	s->text = Parse_Strdup( d->value );
	s->line = numSources > 1 ? sources[numSources - 2].line : 1;
	s->isDefine = qtrue;
	strncpy( s->name, d->name, sizeof( s->name ) - 1 );
}

static void Parse_PopSource( void ) {
	free( sources[numSources - 1].text );
	numSources--;
}

/* the file that the current source belongs to */
static const source_t *Parse_FileSource( void ) {
	int i;
	for ( i = numSources - 1; i >= 0; i-- ) {
		if ( !sources[i].isDefine ) {
			return &sources[i];
		}
	}
	return NULL;
}

qboolean Parse_Open( const char *path ) {
	Parse_Close();
	numIfs = 0;
	return Parse_PushFile( path );
}

void Parse_Close( void ) {
	int i;
	while ( numSources ) {
		Parse_PopSource();
	}
	for ( i = 0; i < numDefines; i++ ) {
		free( defines[i].value );
	}
	numDefines = 0;
	hasUnread = qfalse;
}

/* ---------------------------------------------------------------- defines */

static define_t *Parse_FindDefine( const char *name ) {
	int i;
	for ( i = numDefines - 1; i >= 0; i-- ) {
		if ( !strcmp( defines[i].name, name ) ) {
			return &defines[i];
		}
	}
	return NULL;
}

static void Parse_AddDefine( const char *name, const char *value ) {
	define_t *d = Parse_FindDefine( name );
	if ( !d ) {
		if ( numDefines == MAX_DEFINES ) {
			UI_Printf( "^3#define %s: too many defines\n", name );
			return;
		}
		d = &defines[numDefines++];
		strncpy( d->name, name, sizeof( d->name ) - 1 );
		d->name[sizeof( d->name ) - 1] = 0;
	} else {
		free( d->value );
	}
	d->value = Parse_Strdup( value );
}

static void Parse_RemoveDefine( const char *name ) {
	define_t *d = Parse_FindDefine( name );
	if ( d ) {
		free( d->value );
		*d = defines[--numDefines];
	}
}

/* ---------------------------------------------------------------- raw tokens */

static int Parse_Peek( const source_t *s, int ofs ) {
	return (unsigned char)s->text[s->pos + ofs];
}

/* blanks and comments; qfalse at the end of the source */
static qboolean Parse_SkipBlanks( source_t *s ) {
	for ( ;; ) {
		int c = Parse_Peek( s, 0 );
		if ( !c ) {
			return qfalse;
		}
		if ( c == '\n' ) {
			s->line++;
			s->lineStart = qtrue;
			s->pos++;
		} else if ( c <= ' ' ) {
			s->pos++;
		} else if ( c == '/' && Parse_Peek( s, 1 ) == '/' ) {
			while ( Parse_Peek( s, 0 ) && Parse_Peek( s, 0 ) != '\n' ) {
				s->pos++;
			}
		} else if ( c == '/' && Parse_Peek( s, 1 ) == '*' ) {
			s->pos += 2;
			while ( Parse_Peek( s, 0 ) && !( Parse_Peek( s, 0 ) == '*' && Parse_Peek( s, 1 ) == '/' ) ) {
				if ( Parse_Peek( s, 0 ) == '\n' ) {
					s->line++;
				}
				s->pos++;
			}
			if ( Parse_Peek( s, 0 ) ) {
				s->pos += 2;
			}
		} else {
			return qtrue;
		}
	}
}

static qboolean Parse_IsNameChar( int c, qboolean first ) {
	if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' ) {
		return qtrue;
	}
	return !first && ( ( c >= '0' && c <= '9' ) || c == '.' || c == '/' || c == '\\' );
}

/* one token of s (blanks already skipped) */
static void Parse_RawToken( source_t *s, token_t *t ) {
	int c = Parse_Peek( s, 0 ), n = 0;
	const source_t *f = Parse_FileSource();

	memset( t, 0, sizeof( *t ) );
	t->line = s->line;
	if ( f ) {
		strncpy( t->file, f->name, sizeof( t->file ) - 1 );
	}
	s->lineStart = qfalse;

	if ( c == '"' ) {
		t->type = TT_STRING;
		s->pos++;
		while ( ( c = Parse_Peek( s, 0 ) ) != 0 && c != '"' ) {
			if ( c == '\\' && Parse_Peek( s, 1 ) ) {
				c = Parse_Peek( s, 1 );
				c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
				s->pos++;
			} else if ( c == '\n' ) {
				s->line++;
			}
			if ( n < (int)sizeof( t->string ) - 1 ) {
				t->string[n++] = (char)c;
			}
			s->pos++;
		}
		if ( c == '"' ) {
			s->pos++;
		}
		return;
	}
	if ( ( c >= '0' && c <= '9' ) || ( c == '.' && Parse_Peek( s, 1 ) >= '0' && Parse_Peek( s, 1 ) <= '9' ) ) {
		float	value = 0, frac = 0.1f;
		int		dot = 0;
		t->type = TT_NUMBER;
		if ( c == '0' && ( Parse_Peek( s, 1 ) == 'x' || Parse_Peek( s, 1 ) == 'X' ) ) {
			int hex = 0;
			t->string[n++] = '0', t->string[n++] = 'x';
			s->pos += 2;
			for ( ;; ) {
				c = Parse_Peek( s, 0 );
				if ( c >= '0' && c <= '9' ) {
					hex = hex * 16 + c - '0';
				} else if ( c >= 'a' && c <= 'f' ) {
					hex = hex * 16 + c - 'a' + 10;
				} else if ( c >= 'A' && c <= 'F' ) {
					hex = hex * 16 + c - 'A' + 10;
				} else {
					break;
				}
				if ( n < (int)sizeof( t->string ) - 1 ) {
					t->string[n++] = (char)c;
				}
				s->pos++;
			}
			t->number = (float)hex;
			return;
		}
		for ( ;; ) {
			c = Parse_Peek( s, 0 );
			if ( c >= '0' && c <= '9' ) {
				if ( dot ) {
					value += ( c - '0' ) * frac;
					frac *= 0.1f;
				} else {
					value = value * 10 + ( c - '0' );
				}
			} else if ( c == '.' && !dot ) {
				dot = 1;
			} else {
				break;
			}
			if ( n < (int)sizeof( t->string ) - 1 ) {
				t->string[n++] = (char)c;
			}
			s->pos++;
		}
		if ( c == 'f' || c == 'F' ) {
			s->pos++;	/* 1.0f */
		}
		t->number = value;
		return;
	}
	if ( Parse_IsNameChar( c, qtrue ) ) {
		t->type = TT_NAME;
		while ( Parse_IsNameChar( Parse_Peek( s, 0 ), n == 0 ) ) {
			if ( n < (int)sizeof( t->string ) - 1 ) {
				t->string[n++] = (char)Parse_Peek( s, 0 );
			}
			s->pos++;
		}
		return;
	}
	t->type = TT_PUNCT;
	t->string[0] = (char)c;
	s->pos++;
}

/* the rest of the line (a define's value), with \ continuing it */
static void Parse_RestOfLine( source_t *s, char *out, int size ) {
	int n = 0, c;
	while ( ( c = Parse_Peek( s, 0 ) ) != 0 && c != '\n' ) {
		if ( c == '\\' && ( Parse_Peek( s, 1 ) == '\n' || ( Parse_Peek( s, 1 ) == '\r' && Parse_Peek( s, 2 ) == '\n' ) ) ) {
			s->pos += Parse_Peek( s, 1 ) == '\r' ? 3 : 2;
			s->line++;
			continue;
		}
		if ( c == '/' && Parse_Peek( s, 1 ) == '/' ) {
			while ( Parse_Peek( s, 0 ) && Parse_Peek( s, 0 ) != '\n' ) {
				s->pos++;
			}
			break;
		}
		if ( n < size - 1 ) {
			out[n++] = (char)( c == '\r' ? ' ' : c );
		}
		s->pos++;
	}
	out[n] = 0;
}

static qboolean Parse_Reading( void ) {
	return numIfs == 0 || ifs[numIfs - 1];
}

/* #directive at the start of a line of file source s */
static void Parse_Directive( source_t *s ) {
	token_t	d, arg;
	char	value[1024];

	s->pos++;	/* # */
	Parse_SkipBlanks( s );
	Parse_RawToken( s, &d );
	if ( !strcmp( d.string, "ifdef" ) || !strcmp( d.string, "ifndef" ) ) {
		Parse_SkipBlanks( s );
		Parse_RawToken( s, &arg );
		if ( numIfs < MAX_IFS ) {
			int defined = Parse_FindDefine( arg.string ) != NULL;
			ifs[numIfs] = Parse_Reading() && ( d.string[2] == 'n' ? !defined : defined );
			numIfs++;
		}
	} else if ( !strcmp( d.string, "else" ) ) {
		if ( numIfs > 0 ) {
			ifs[numIfs - 1] = !ifs[numIfs - 1] && ( numIfs == 1 || ifs[numIfs - 2] );
		}
	} else if ( !strcmp( d.string, "endif" ) ) {
		if ( numIfs > 0 ) {
			numIfs--;
		}
	} else if ( !Parse_Reading() ) {
		/* skipped */
	} else if ( !strcmp( d.string, "include" ) ) {
		Parse_SkipBlanks( s );
		Parse_RawToken( s, &arg );
		if ( arg.type == TT_STRING ) {
			if ( !Parse_PushFile( arg.string ) ) {
				Parse_Warning( &d, "#include \"%s\": no such file", arg.string );
			}
			return;
		}
		Parse_Warning( &d, "#include without a \"file\"" );
	} else if ( !strcmp( d.string, "define" ) ) {
		Parse_SkipBlanks( s );
		Parse_RawToken( s, &arg );
		Parse_RestOfLine( s, value, sizeof( value ) );
		if ( arg.type == TT_NAME ) {
			Parse_AddDefine( arg.string, value );
		}
		return;
	} else if ( !strcmp( d.string, "undef" ) ) {
		Parse_SkipBlanks( s );
		Parse_RawToken( s, &arg );
		Parse_RemoveDefine( arg.string );
	} else {
		Parse_Warning( &d, "#%s is not supported", d.string );
	}
	Parse_RestOfLine( s, value, sizeof( value ) );
}

/* enum { A, B = 5, C }: A, B, C become defines */
static void Parse_Enum( void ) {
	token_t	t;
	float	value = 0;
	char	buf[32];

	if ( !Parse_Expect( "{" ) ) {
		return;
	}
	while ( Parse_Token( &t ) ) {
		if ( !strcmp( t.string, "}" ) ) {
			break;
		}
		if ( t.type != TT_NAME ) {
			Parse_Warning( &t, "enum: a name expected, found %s", t.string );
			continue;
		}
		{
			token_t eq;
			char	name[64];
			strncpy( name, t.string, sizeof( name ) - 1 );
			name[sizeof( name ) - 1] = 0;
			if ( Parse_Token( &eq ) ) {
				if ( !strcmp( eq.string, "=" ) ) {
					Parse_Number( &value );
				} else {
					Parse_Unread( &eq );
				}
			}
			snprintf( buf, sizeof( buf ), "%d", (int)value );
			Parse_AddDefine( name, buf );
			value += 1;
		}
		if ( Parse_Token( &t ) && strcmp( t.string, "," ) ) {
			if ( !strcmp( t.string, "}" ) ) {
				break;
			}
			Parse_Unread( &t );
		}
	}
	if ( Parse_Token( &t ) && strcmp( t.string, ";" ) ) {
		Parse_Unread( &t );
	}
}

qboolean Parse_Token( token_t *t ) {
	if ( hasUnread ) {
		*t = unread;
		hasUnread = qfalse;
		lastToken = *t;
		return qtrue;
	}
	while ( numSources ) {
		source_t	*s = &sources[numSources - 1];
		define_t	*d;

		if ( !Parse_SkipBlanks( s ) ) {
			Parse_PopSource();
			continue;
		}
		if ( Parse_Peek( s, 0 ) == '#' && s->lineStart && !s->isDefine ) {
			Parse_Directive( s );
			continue;
		}
		Parse_RawToken( s, t );
		if ( !Parse_Reading() ) {
			continue;
		}
		if ( t->type == TT_NAME ) {
			if ( !strcmp( t->string, "enum" ) ) {
				Parse_Enum();
				continue;
			}
			d = Parse_FindDefine( t->string );
			if ( d && !Parse_DefineActive( d->name ) ) {
				Parse_PushDefine( d );
				continue;
			}
		}
		lastToken = *t;
		return qtrue;
	}
	return qfalse;
}

void Parse_Unread( const token_t *t ) {
	unread = *t;
	hasUnread = qtrue;
}

qboolean Parse_Expect( const char *punct ) {
	token_t t;
	if ( !Parse_Token( &t ) ) {
		Parse_Warning( NULL, "\"%s\" expected at the end of the file", punct );
		return qfalse;
	}
	if ( strcmp( t.string, punct ) ) {
		Parse_Warning( &t, "\"%s\" expected, found \"%s\"", punct, t.string );
		Parse_Unread( &t );
		return qfalse;
	}
	return qtrue;
}

/* ---------------------------------------------------------------- values */

static qboolean Parse_Expression( float *out );

static qboolean Parse_Factor( float *out ) {
	token_t t;
	if ( !Parse_Token( &t ) ) {
		return qfalse;
	}
	if ( !strcmp( t.string, "(" ) ) {
		if ( !Parse_Expression( out ) ) {
			return qfalse;
		}
		return Parse_Expect( ")" );
	}
	if ( !strcmp( t.string, "-" ) ) {
		if ( !Parse_Factor( out ) ) {
			return qfalse;
		}
		*out = -*out;
		return qtrue;
	}
	if ( t.type != TT_NUMBER ) {
		Parse_Warning( &t, "a number expected, found \"%s\"", t.string );
		return qfalse;
	}
	*out = t.number;
	return qtrue;
}

static qboolean Parse_Term( float *out ) {
	token_t t;
	float	v;
	if ( !Parse_Factor( out ) ) {
		return qfalse;
	}
	while ( Parse_Token( &t ) ) {
		if ( strcmp( t.string, "*" ) && strcmp( t.string, "/" ) ) {
			Parse_Unread( &t );
			break;
		}
		if ( !Parse_Factor( &v ) ) {
			return qfalse;
		}
		*out = t.string[0] == '*' ? *out * v : ( v != 0 ? *out / v : 0 );
	}
	return qtrue;
}

static qboolean Parse_Expression( float *out ) {
	token_t t;
	float	v;
	if ( !Parse_Term( out ) ) {
		return qfalse;
	}
	while ( Parse_Token( &t ) ) {
		if ( strcmp( t.string, "+" ) && strcmp( t.string, "-" ) ) {
			Parse_Unread( &t );
			break;
		}
		if ( !Parse_Term( &v ) ) {
			return qfalse;
		}
		*out = t.string[0] == '+' ? *out + v : *out - v;
	}
	return qtrue;
}

/* as Tremulous' PC_Float_Parse: a number, - and a number, or ( expression ) */
qboolean Parse_Number( float *out ) {
	token_t t;
	if ( !Parse_Token( &t ) ) {
		return qfalse;
	}
	if ( !strcmp( t.string, "(" ) ) {
		if ( !Parse_Expression( out ) ) {
			return qfalse;
		}
		return Parse_Expect( ")" );
	}
	if ( !strcmp( t.string, "-" ) ) {
		if ( !Parse_Token( &t ) || t.type != TT_NUMBER ) {
			Parse_Warning( &t, "a number expected after -" );
			return qfalse;
		}
		*out = -t.number;
		return qtrue;
	}
	if ( t.type != TT_NUMBER ) {
		Parse_Warning( &t, "a number expected, found \"%s\"", t.string );
		Parse_Unread( &t );
		return qfalse;
	}
	*out = t.number;
	return qtrue;
}

qboolean Parse_String( char *out, int size ) {
	token_t t;
	if ( !Parse_Token( &t ) ) {
		return qfalse;
	}
	if ( t.type != TT_STRING && t.type != TT_NAME && t.type != TT_NUMBER ) {
		Parse_Warning( &t, "a string expected, found \"%s\"", t.string );
		Parse_Unread( &t );
		return qfalse;
	}
	strncpy( out, t.string, size - 1 );
	out[size - 1] = 0;
	return qtrue;
}

void Parse_SkipLine( const token_t *key ) {
	token_t t;
	while ( Parse_Token( &t ) ) {
		if ( t.line != key->line || strcmp( t.file, key->file ) || !strcmp( t.string, "}" ) ) {
			if ( !strcmp( t.string, "{" ) ) {
				char skip[4096];
				Parse_Block( skip, sizeof( skip ) );
				return;
			}
			Parse_Unread( &t );
			return;
		}
		if ( !strcmp( t.string, "{" ) ) {
			char skip[4096];
			Parse_Block( skip, sizeof( skip ) );
			return;
		}
	}
}

/* the tokens up to the matching }, strings in quotes, separated by blanks */
qboolean Parse_Block( char *out, int size ) {
	token_t	t;
	int		depth = 1, n = 0;

	out[0] = 0;
	while ( Parse_Token( &t ) ) {
		const char *s = t.string;
		char		quoted[1100];
		int			len;

		if ( !strcmp( s, "{" ) ) {
			depth++;
		} else if ( !strcmp( s, "}" ) && --depth == 0 ) {
			return qtrue;
		}
		if ( t.type == TT_STRING ) {
			/* keep it one argument: quotes, with " and \ escaped */
			int i, q = 0;
			quoted[q++] = '"';
			for ( i = 0; s[i] && q < (int)sizeof( quoted ) - 3; i++ ) {
				if ( s[i] == '"' || s[i] == '\\' ) {
					quoted[q++] = '\\';
				}
				quoted[q++] = s[i];
			}
			quoted[q++] = '"';
			quoted[q] = 0;
			s = quoted;
		}
		len = (int)strlen( s );
		if ( n + len + 2 < size ) {
			if ( n ) {
				out[n++] = ' ';
			}
			memcpy( out + n, s, len );
			n += len;
			out[n] = 0;
		}
	}
	Parse_Warning( NULL, "a block without its }" );
	return qfalse;
}
