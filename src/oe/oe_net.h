/*
 * oe_net.h -- network play: the lockstep between the host (a listen server,
 * src/server/sv_main.c) and its clients (src/client/cl_net.c).
 *
 * Every machine runs the same game module from the same start: a fresh game
 * (an empty save folder, netplay/), the same random seed, the same screen
 * layout, and the virtual clock (time = tick * the timer's period). What the
 * players do is the only input: their phone keys. The host is the clock: it
 * gives every key that reaches it (its own and its clients') the next tick it
 * runs, and sends the stream "tick T: these keys" to everyone; a client runs
 * tick T only once it has the stream up to T, with the keys of T first. So
 * every module goes through the same ticks with the same keys and stays the
 * same game. A client joining later gets the stream from tick 0 and catches
 * up as fast as it can.
 *
 * The transport is Quake 3's (Tremulous'): UDP through net_ip.c, connection-
 * less packets for the handshake, then a netchan (net_chan.c) per client:
 *
 *   client -> server  "getchallenge"
 *   server -> client  "challengeResponse <challenge>"
 *   client -> server  "connect <OE_PROTOCOL> <challenge> <qport> <name>"
 *   server -> client  "connectResponse <player>"  (or "print <reason>")
 *
 * then sequenced messages (msg_t), see sv_main.c / cl_net.c:
 *
 *   server -> client  long reliableAck  (the client's commands it has)
 *                     long serverTick   (ticks the host has run)
 *                     long fromTick, long toTick  (the stream for [fromTick, toTick) follows)
 *                     long inputAck     (the client's inputs it has)
 *                     short n, n x { long tick, byte player, long evt, long wParam, long dwParam }
 *                     byte n, n x { long seq, string command }   (reliable: "start ...", "print ...",
 *                                                                  "disconnect ...")
 *   client -> server  long commandAck   (the server's commands it has)
 *                     long tickAck      (the stream it has: ticks [0, tickAck))
 *                     long firstInput, byte n, n x { long evt, long wParam, long dwParam }
 *                     byte n, n x { long seq, string command }   (reliable: "say ...")
 *
 * Inputs and commands are sent again until the other side acknowledges them.
 */
#ifndef OE_NET_H
#define OE_NET_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#define OE_PROTOCOL			1
#define OE_NET_PORT			27960
#define OE_NET_MAX_PLAYERS	8			/* the host is player 0 */
#define OE_NET_TIMEOUT		30000		/* ms without a packet */

typedef struct {
	int		tick;
	int		player;
	int		evt, wParam, dwParam;		/* an OE_GAME_EVENT (EVT_KEY and its key) */
} oeNetEvent_t;

/* What a session starts with: every machine restarts its game module with it */
typedef struct {
	char	game[MAX_QPATH];			/* fs_game */
	int		seed;
	int		screenW, screenH;			/* the layout the host's module has */
	int		softkeyBar;					/* oe_softkeyBar (it changes the 3D view's height) */
} oeNetStart_t;

/* ---------------------------------------------------------------- server (src/server/sv_main.c) */

void		SV_Init( void );				/* cvars and commands: host, kick, status, say */
void		SV_Shutdown( const char *reason );
qboolean	SV_Running( void );
void		SV_PacketEvent( netadr_t from, msg_t *msg );
void		SV_Frame( void );				/* timeouts; the stream to the clients */
void		SV_LocalInput( int evt, int wParam, int dwParam );	/* a key of the host's player */
/* the host runs tick `tick`: the inputs waiting get it; returns its events (in the stream's order) */
int			SV_BeginTick( int tick, oeNetEvent_t *out, int max );

/* ---------------------------------------------------------------- client (src/client/cl_net.c) */

void		CL_NetInit( void );				/* commands: connect, disconnect, reconnect, say */
void		CL_NetDisconnect( const char *reason );
qboolean	CL_NetActive( void );			/* connected (or connecting) to a host */
void		CL_NetPacketEvent( netadr_t from, msg_t *msg );
void		CL_NetFrame( void );			/* handshake, timeouts; inputs and acks to the host */
void		CL_NetLocalInput( int evt, int wParam, int dwParam );
int			CL_NetTicksReceived( void );	/* the stream is known for ticks [0, n) */
int			CL_NetServerTick( void );		/* the ticks the host had run when it last wrote */
int			CL_NetTickEvents( int tick, oeNetEvent_t *out, int max );	/* (and forgets older ticks) */

/* ---------------------------------------------------------------- the game (src/oe/oe_host.c) */

/* a session starts (host: from the current game; client: the host's "start"): restarts the module */
qboolean	OE_NetBegin( const oeNetStart_t *start, qboolean host );
void		OE_NetEnd( void );				/* back to playing alone (the module restarts) */
void		OE_NetDescribe( oeNetStart_t *start );	/* the current game, for a host starting a session */
int			OE_NetTick( void );				/* the ticks run in the session */

#endif
