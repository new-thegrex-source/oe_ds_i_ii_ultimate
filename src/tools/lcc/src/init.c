#include "c.h"


static int curseg;		/* current segment */

/* oe-client (C99): initializers of local aggregates may hold non-constant
 * expressions. While initlocal is set, initializer() records what it would
 * have emitted (value, gap, string, byte) with its byte offset instead of
 * emitting data; initlocalvar() then either emits a static copy (all values
 * constant: the classic lcc code) or zero-fills the variable and stores each
 * value. */
static int initlocal;
static int initoff;
typedef struct initem {
	int kind;		/* 0 value tree, 1 string bytes, 2 constant (op,size,v) */
	int off, size;
	Tree e;
	char *str;
	int op;
	Value v;
} *Initem;
static List initems;

static int genconst(Tree, int);

static void initem_add(int kind, int size, Tree e, char *str, int op, Value v) {
	Initem it;
	NEW0(it, FUNC);
	it->kind = kind;
	it->off = initoff;
	it->size = size;
	it->e = e;
	it->str = str;
	it->op = op;
	it->v = v;
	initems = append(it, initems);
}

/* emit (or record) a value; returns its size */
static int initemit(Tree e) {
	if (initlocal) {
		static Value v0;
		initem_add(0, e->type->size, e, NULL, 0, v0);
		initoff += e->type->size;
		return e->type->size;
	}
	return genconst(e, 1);
}

static void initspace(int n) {
	if (initlocal)
		initoff += n;
	else
		(*IR->space)(n);
}

static void initstring(int len, char *str) {
	if (initlocal) {
		static Value v0;
		char *copy = allocate(len, FUNC);
		memcpy(copy, str, len);
		initem_add(1, len, NULL, copy, 0, v0);
		initoff += len;
	} else
		(*IR->defstring)(len, str);
}

static void initconst(int op, int size, Value v) {
	if (initlocal) {
		initem_add(2, size, NULL, NULL, op, v);
		initoff += size;
	} else
		(*IR->defconst)(op, size, v);
}

/* defpointer - initialize a pointer to p or to 0 if p==0 */
void defpointer(Symbol p) {
	if (p) {
		(*IR->defaddress)(p);
		p->ref++;
	} else {
		static Value v;
		(*IR->defconst)(P, voidptype->size, v);
	}
}

/* genconst - generate/check constant expression e; return size */
static int genconst(Tree e, int def) {
	for (;;)
		switch (generic(e->op)) {
		case ADDRG:
			if (def)
				(*IR->defaddress)(e->u.sym);
			return e->type->size;
		case CNST:
			if (e->op == CNST+P && isarray(e->type)) {
				e = cvtconst(e);
				continue;
			}
			if (def)
				(*IR->defconst)(e->type->op, e->type->size, e->u.v);
			return e->type->size;
		case RIGHT:
			assert(e->kids[0] || e->kids[1]);
			if (e->kids[1] && e->kids[0])
				error("initializer must be constant\n");
			e = e->kids[1] ? e->kids[1] : e->kids[0];
			continue;
		case CVP:
			if (isarith(e->type))
				error("cast from `%t' to `%t' is illegal in constant expressions\n",
					e->kids[0]->type, e->type);
			/* fall thru */
		case CVI: case CVU: case CVF:
			e = e->kids[0];
			continue;
		default:
			error("initializer must be constant\n");
			if (def)
				genconst(consttree(0, inttype), def);
			return inttype->size;
		}
}

/* initvalue - evaluate a constant expression for a value of integer type ty */
static Tree initvalue(Type ty) {
	Type aty;
	Tree e;

	needconst++;
	e = expr1(0);
	if ((aty = assign(ty, e)) != NULL)
		e = cast(e, aty);
	else {
		error("invalid initialization type; found `%t' expected `%t'\n",
			e->type,  ty);
		e = retype(consttree(0, inttype), ty);
	}
	needconst--;
	if (generic(e->op) != CNST) {
		error("initializer must be constant\n");
		e = retype(consttree(0, inttype), ty);
	}
	return e;
}

/* initarray - initialize array of ty of <= len bytes; if len == 0, go to } */
static int initarray(int len, Type ty, int lev) {
	int n = 0;

	do {
		initializer(ty, lev);
		n += ty->size;
		if ((len > 0 && n >= len) || t != ',')
			break;
		t = gettok();
	} while (t != '}');
	return n;
}

/* initchar - initialize array of <= len ty characters; if len == 0, go to } */
static int initchar(int len, Type ty) {
	int n = 0;
	char buf[16], *s = buf;

	if (initlocal) {
		/* local array of chars from { expressions }: values need not be constant */
		do {
			Tree e = expr1(0);
			Type aty;
			e = pointer(e);
			if ((aty = assign(ty, e)) != NULL)
				e = cast(e, aty);
			else
				error("invalid initialization type; found `%t' expected `%t'\n", e->type, ty);
			initemit(e);
			n++;
			if ((len > 0 && n >= len) || t != ',')
				break;
			t = gettok();
		} while (t != '}');
		return n;
	}
	do {
		*s++ = initvalue(ty)->u.v.i;
		if (++n%inttype->size == 0) {
			initstring(inttype->size, buf);
			s = buf;
		}
		if ((len > 0 && n >= len) || t != ',')
			break;
		t = gettok();
	} while (t != '}');
	if (s > buf)
		initstring(s - buf, buf);
	return n;
}

/* initend - finish off an initialization at level lev; accepts trailing comma */
static void initend(int lev, char follow[]) {
	if (lev == 0 && t == ',')
		t = gettok();
	test('}', follow);
}

/* initfields - initialize <= an unsigned's worth of bit fields in fields p to q */
static int initfields(Field p, Field q) {
	unsigned int bits = 0;
	int i, n = 0;

	do {
		i = initvalue(inttype)->u.v.i;
		if (fieldsize(p) < 8*p->type->size) {
			if ((p->type == inttype &&
			   (i < -(int)(fieldmask(p)>>1)-1 || i > (int)(fieldmask(p)>>1)))
			||  (p->type == unsignedtype && (i&~fieldmask(p)) !=  0))
				warning("initializer exceeds bit-field width\n");
			i &= fieldmask(p);
		}
		bits |= i<<fieldright(p);
		if (IR->little_endian) {
			if (fieldsize(p) + fieldright(p) > n)
				n = fieldsize(p) + fieldright(p);
		} else {
			if (fieldsize(p) + fieldleft(p) > n)
				n = fieldsize(p) + fieldleft(p);
		}
		if (p->link == q)
			break;
		p = p->link;
	} while (t == ',' && (t = gettok()) != 0);
	n = (n + 7)/8;
	for (i = 0; i < n; i++) {
		Value v;
		if (IR->little_endian) {
			v.u = (unsigned char)bits;
			bits >>= 8;
		} else {	/* a big endian */
			v.u = (unsigned char)(bits>>(8*(unsignedtype->size - 1)));
			bits <<= 8;
		}
		initconst(U, unsignedchar->size, v);
	}
	return n;
}

/* initstruct - initialize a struct ty of <= len bytes; if len == 0, go to } */
static int initstruct(int len, Type ty, int lev) {
	int a, n = 0;
	Field p = ty->u.sym->u.s.flist;

	do {
		if (p->offset > n) {
			initspace(p->offset - n);
			n += p->offset - n;
		}
		if (p->lsb) {
			Field q = p;
			while (q->link && q->link->offset == p->offset)
				q = q->link;
			n += initfields(p, q->link);
			p = q;
		} else {
			initializer(p->type, lev);
			n += p->type->size;
		}
		if (p->link) {
			p = p->link;
			a = p->type->align;
		} else
			a = ty->align;
		if (a && n%a) {
			initspace(a - n%a);
			n = roundup(n, a);
		}
		if ((len > 0 && n >= len) || t != ',')
			break;
		t = gettok();
	} while (t != '}');
	return n;
}

/* initializer - constexpr | { constexpr ( , constexpr )* [ , ] } */
Type initializer(Type ty, int lev) {
	int n = 0;
	Tree e;
	Type aty = NULL;
	static char follow[] = { IF, CHAR, STATIC, 0 };

	ty = unqual(ty);
	if (isscalar(ty)) {
		if (!initlocal)
			needconst++;
		if (t == '{') {
			t = gettok();
			e = expr1(0);
			initend(lev, follow);
		} else
			e = expr1(0);
		e = pointer(e);
		if ((aty = assign(ty, e)) != NULL)
			e = cast(e, aty);
		else
			error("invalid initialization type; found `%t' expected `%t'\n",
				e->type, ty);
		n = initemit(e);
		if (!initlocal) {
			deallocate(STMT);
			needconst--;
		}
	}
	if ((isunion(ty) || isstruct(ty)) && ty->size == 0) {
		static char follow[] = { CHAR, STATIC, 0 };
		error("cannot initialize undefined `%t'\n", ty);
		skipto(';', follow);
		return ty;
	} else if (isunion(ty)) {
		if (t == '{') {
			t = gettok();
			n = initstruct(ty->u.sym->u.s.flist->type->size, ty, lev + 1);
			initend(lev, follow);
		} else {
			if (lev == 0)
				error("missing { in initialization of `%t'\n", ty);
			n = initstruct(ty->u.sym->u.s.flist->type->size, ty, lev + 1);
		}
	} else if (isstruct(ty)) {
		if (t == '{') {
			t = gettok();
			n = initstruct(0, ty, lev + 1);
			test('}', follow);
		} else if (lev > 0)
			n = initstruct(ty->size, ty, lev + 1);
		else {
			error("missing { in initialization of `%t'\n", ty);
			n = initstruct(ty->u.sym->u.s.flist->type->size, ty, lev + 1);
		}
	}
	if (isarray(ty))
		aty = unqual(ty->type);
	if (isarray(ty) && ischar(aty)) {
		if (t == SCON) {
			if (ty->size > 0 && ty->size == tsym->type->size - 1)
				tsym->type = array(chartype, ty->size, 0);
			n = tsym->type->size;
			initstring(tsym->type->size, tsym->u.c.v.p);
			t = gettok();
		} else if (t == '{') {
			t = gettok();
			if (t == SCON) {
				ty = initializer(ty, lev + 1);
				initend(lev, follow);
				return ty;
			}
			n = initchar(0, aty);
			test('}', follow);
		} else if (lev > 0 && ty->size > 0)
			n = initchar(ty->size, aty);
		else {	/* eg, char c[] = 0; */
			error("missing { in initialization of `%t'\n", ty);
			n = initchar(1, aty);
		}
	} else if (isarray(ty)) {
		if (t == SCON && aty == widechar) {
			int i;
			unsigned int *s = tsym->u.c.v.p;
			if (ty->size > 0 && ty->size == tsym->type->size - widechar->size)
				tsym->type = array(widechar, ty->size/widechar->size, 0);
			n = tsym->type->size;
			for (i = 0; i < n; i += widechar->size) {
				Value v;
				v.u = *s++;
				initconst(widechar->op, widechar->size, v);
			}
			t = gettok();
		} else if (t == '{') {
			t = gettok();
			if (t == SCON && aty == widechar) {
				ty = initializer(ty, lev + 1);
				initend(lev, follow);
				return ty;
			}
			n = initarray(0, aty, lev + 1);
			test('}', follow);
		} else if (lev > 0 && ty->size > 0)
			n = initarray(ty->size, aty, lev + 1);
		else {
			error("missing { in initialization of `%t'\n", ty);
			n = initarray(aty->size, aty, lev + 1);
		}
	}	
	if (ty->size) {
		if (n > ty->size)
			error("too many initializers\n");
		else if (n < ty->size)
			initspace(ty->size - n);
	} else if (isarray(ty) && ty->type->size > 0)
		ty = array(ty->type, n/ty->type->size, 0);
	else
		ty->size = n;
	return ty;
}

/* swtoseg - switch to segment seg, if necessary */
void swtoseg(int seg) {
	if (curseg != seg)
		(*IR->segment)(seg);
	curseg = seg;
}

/* address of byte off inside local p, as an lvalue of type ty */
static Tree initlvalue(Symbol p, int off, Type ty) {
	Tree a = tree(mkop(ADDRL, voidptype), ptr(chartype), NULL, NULL);
	a->u.sym = p;
	p->ref += refinc;
	if (off)
		a = simplify(ADD, ptr(chartype), a, cnsttree(signedptr, (long long)off));
	return rvalue(retype(a, ptr(ty)));
}

static int initemconst(Initem it) {
	Tree e = it->e;
	if (it->kind != 0)
		return 1;
	for (;;)
		switch (generic(e->op)) {
		case ADDRG: case CNST:
			return 1;
		case RIGHT:
			if (e->kids[1] && e->kids[0])
				return 0;
			e = e->kids[1] ? e->kids[1] : e->kids[0];
			continue;
		case CVP: case CVI: case CVU: case CVF:
			e = e->kids[0];
			continue;
		default:
			return 0;
		}
}

/* initlocalvar - initialize the local aggregate p (C99: values need not be constant) */
void initlocalvar(Symbol p) {
	Type ty, ty1;
	Initem *items;
	int i, off, allconst = 1;
	Symbol t1;

	initlocal = 1;
	initoff = 0;
	initems = NULL;
	ty = initializer(p->type, 0);
	initlocal = 0;
	if (isarray(p->type) && p->type->size == 0 && ty->size > 0)
		p->type = array(p->type->type, ty->size/ty->type->size, 0);
	items = ltov(&initems, FUNC);
	for (i = 0; items[i]; i++)
		if (!initemconst(items[i]))
			allconst = 0;
	ty = p->type;
	if (allconst) {
		/* the classic lcc way: a constant copy in the data segment, block-copied in */
		for (ty1 = ty; isarray(ty1); ty1 = ty1->type)
			;
		if (!isconst(ty) && (!isarray(ty) || !isconst(ty1)))
			ty = qual(CONST, ty);
		t1 = genident(STATIC, ty, GLOBAL);
		for (ty1 = ty; isarray(ty1); ty1 = ty1->type)
			;
		defglobal(t1, isconst(ty1) ? LIT : DATA);
		off = 0;
		for (i = 0; items[i]; i++) {
			Initem it = items[i];
			if (it->off > off)
				(*IR->space)(it->off - off);
			if (it->kind == 0)
				genconst(it->e, 1);
			else if (it->kind == 1)
				(*IR->defstring)(it->size, it->str);
			else
				(*IR->defconst)(it->op, it->size, it->v);
			off = it->off + it->size;
		}
		if (ty->size > off)
			(*IR->space)(ty->size - off);
		walk(root(asgn(p, idtree(t1))), 0, 0);
	} else {
		/* zero-fill from a static zero object, then store each value. All of it is
		 * one tree walked once: walk() frees the STMT arena the value trees live in. */
		Tree all;
		t1 = genident(STATIC, ty, GLOBAL);
		defglobal(t1, BSS);
		(*IR->space)(ty->size);
		all = asgn(p, idtree(t1));
		for (i = 0; items[i]; i++) {
			Initem it = items[i];
			Tree st = NULL;
			if (it->kind == 0) {
				st = asgntree(ASGN, initlvalue(p, it->off, it->e->type), it->e);
			} else if (it->kind == 1) {
				int k;
				for (k = 0; k < it->size; k++)
					if (it->str[k]) {
						Tree c = asgntree(ASGN, initlvalue(p, it->off + k, chartype),
							cnsttree(inttype, (long long)(unsigned char)it->str[k]));
						st = st ? tree(RIGHT, c->type, st, c) : c;
					}
			} else {
				Type cty = it->size == 1 ? unsignedchar : it->size == 2 ? unsignedshort : unsignedtype;
				st = asgntree(ASGN, initlvalue(p, it->off, cty), cnsttree(unsignedtype, it->v.u));
			}
			if (st)
				all = tree(RIGHT, st->type, all, st);
		}
		walk(root(all), 0, 0);
	}
	p->ref = 1;
}
