/*
  CHAT:  A chat client using the SDL example network and GUI libraries
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* Note that this isn't necessarily the way to run a chat system.
   This is designed to exercise the network code more than be really
   functional.
*/

#include "SDL_net.h"
#include "SDL_test.h"
#include "chat.h"

#ifdef __DREAMCAST__
#include <kos.h>
#include <arch/gdb.h>
#include <kos/dbgio.h>
#include <kos/dbglog.h>
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
#else
#define vid_border_color(r, g, b) ((void)0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __DREAMCAST__
#define DEFAULT_CHAT_SERVER "192.168.0.93"

static void DreamcastFatal(const char *context, const char *error)
{
    // dbgio_dev_select("fb");
    // dbgio_enable();
    printf("SDL_net chat failed\n");
    printf("%s\n", context);
    printf("%s\n", error ? error : "Unknown error");
    thd_sleep(20000);
}
#endif


/* Global variables */
static TCPsocket tcpsock = NULL;
static UDPsocket udpsock = NULL;
static SDLNet_SocketSet socketset = NULL;
static UDPpacket **packets = NULL;
static struct {
    int active;
    Uint8 name[256+1];
} people[CHAT_MAXPEOPLE];

static char keybuf[80-sizeof(CHAT_PROMPT)+1];
static int  keypos = 0;

#define FONT_LINE_HEIGHT    (FONT_CHARACTER_SIZE + 2)

typedef struct
{
    SDL_Rect rect;
    int current;
    int numlines;
    char **lines;

} TextWindow;

static TextWindow *termwin;
static TextWindow *sendwin;

#ifdef __DREAMCAST__
static Uint32 last_textinput_tick = 0;
static char last_textinput_text[8];
#endif

static TextWindow *TextWindowCreate(int x, int y, int w, int h)
{
    TextWindow *textwin = (TextWindow *)SDL_malloc(sizeof(*textwin));

    if ( !textwin ) {
        return NULL;
    }

    textwin->rect.x = x;
    textwin->rect.y = y;
    textwin->rect.w = w;
    textwin->rect.h = h;
    textwin->current = 0;
    textwin->numlines = (h / FONT_LINE_HEIGHT);
    textwin->lines = (char **)SDL_calloc(textwin->numlines, sizeof(*textwin->lines));
    if ( !textwin->lines ) {
        SDL_free(textwin);
        return NULL;
    }
    return textwin;
}

static void TextWindowDisplay(TextWindow *textwin, SDL_Renderer *renderer)
{
    int i, y;

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for ( y = textwin->rect.y, i = 0; i < textwin->numlines; ++i, y += FONT_LINE_HEIGHT ) {
        if ( textwin->lines[i] ) {
            SDLTest_DrawString(renderer, textwin->rect.x, y, textwin->lines[i]);
        }
    }
}

static void TextWindowAddTextWithLength(TextWindow *textwin, const char *text, size_t len)
{
    size_t existing;
    SDL_bool newline = SDL_FALSE;
    char *line;

    if ( len > 0 && text[len - 1] == '\n' ) {
        --len;
        newline = SDL_TRUE;
    }

    if ( textwin->lines[textwin->current] ) {
        existing = SDL_strlen(textwin->lines[textwin->current]);
    } else {
        existing = 0;
    }

    if ( *text == '\b' ) {
        if ( existing ) {
            textwin->lines[textwin->current][existing - 1] = '\0';
        }
        return;
    }

    line = (char *)SDL_realloc(textwin->lines[textwin->current], existing + len + 1);
    if ( line ) {
        SDL_memcpy(&line[existing], text, len);
        line[existing + len] = '\0';
        textwin->lines[textwin->current] = line;
        if ( newline ) {
            if (textwin->current == textwin->numlines - 1) {
                SDL_free(textwin->lines[0]);
                SDL_memcpy(&textwin->lines[0], &textwin->lines[1], (textwin->numlines-1) * sizeof(textwin->lines[1]));
                textwin->lines[textwin->current] = NULL;
            } else {
                ++textwin->current;
            }
        }
    }
}

static void TextWindowAddText(TextWindow *textwin, const char *fmt, ...)
{
	char text[1024];
	va_list ap;

	va_start(ap, fmt);
	SDL_vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

    TextWindowAddTextWithLength(textwin, text, SDL_strlen(text));
}

static void TextWindowClear(TextWindow *textwin)
{
    int i;

    for ( i = 0; i < textwin->numlines; ++i )
    {
        if ( textwin->lines[i] ) {
            SDL_free(textwin->lines[i]);
            textwin->lines[i] = NULL;
        }
    }
    textwin->current = 0;
}

static void TextWindowDestroy(TextWindow *textwin)
{
    if ( textwin ) {
        TextWindowClear(textwin);
        SDL_free(textwin->lines);
        SDL_free(textwin);
    }
}

void SendHello(const char *name)
{
    IPaddress *myip;
    char hello[1+1+256];
    int i, n;

    /* No people are active at first */
    for ( i=0; i<CHAT_MAXPEOPLE; ++i ) {
        people[i].active = 0;
    }
    if ( tcpsock != NULL ) {
        /* Get our chat handle */
        if ( (name == NULL) &&
             ((name=getenv("CHAT_USER")) == NULL) &&
             ((name=getenv("USER")) == NULL ) ) {
            name="Unknown";
        }
        TextWindowAddText(termwin, "Using name '%s'\n", name);

        /* Construct the packet */
        hello[0] = CHAT_HELLO;
        myip = SDLNet_UDP_GetPeerAddress(udpsock, -1);
        memcpy(&hello[CHAT_HELLO_PORT], &myip->port, 2);
        if ( strlen(name) > 255 ) {
            n = 255;
        } else {
            n = strlen(name);
        }
        hello[CHAT_HELLO_NLEN] = n;
        strncpy(&hello[CHAT_HELLO_NAME], name, n);
        hello[CHAT_HELLO_NAME+n++] = 0;

        /* Send it to the server */
        SDLNet_TCP_Send(tcpsock, hello, CHAT_HELLO_NAME+n);
    }
}

void SendBuf(char *buf, int len)
{
    int i;
#ifdef __DREAMCAST__
    int active = 0;

    /* Echo our own message locally. */
    if (len > 0) {
        TextWindowAddText(termwin, "[me] ");
        TextWindowAddTextWithLength(termwin, buf, len);
        TextWindowAddText(termwin, "\n");
    }
#endif

    /* Redraw the prompt and add a newline to the buffer */
    TextWindowClear(sendwin);
    TextWindowAddText(sendwin, CHAT_PROMPT);
    buf[len++] = '\n';

    /* Send the text to each of our active channels */
    for ( i=0; i < CHAT_MAXPEOPLE; ++i ) {
        if ( people[i].active ) {
            if ( len > packets[0]->maxlen ) {
                len = packets[0]->maxlen;
            }
            memcpy(packets[0]->data, buf, len);
            packets[0]->len = len;
#ifdef __DREAMCAST__
            ++active;
            printf("Sending %d bytes to channel %d\n", len, i);
            if (SDLNet_UDP_Send(udpsock, i, packets[0]) <= 0) {
                printf("UDP send failed on channel %d: %s\n", i, SDLNet_GetError());
            }
#else
            SDLNet_UDP_Send(udpsock, i, packets[0]);
#endif
        }
    }
#ifdef __DREAMCAST__
    if (active == 0) {
        printf("No active UDP peers to send to\n");
    }
#endif
}

int HandleServerData(Uint8 *data)
{
    int used = 0;

    switch (data[0]) {
        case CHAT_ADD: {
            Uint8 which;
            IPaddress newip;

            /* Figure out which channel we got */
            which = data[CHAT_ADD_SLOT];
            if ((which >= CHAT_MAXPEOPLE) || people[which].active) {
                /* Invalid channel?? */
                break;
            }
            /* Get the client IP address */
            newip.host=SDLNet_Read32(&data[CHAT_ADD_HOST]);
            newip.port=SDLNet_Read16(&data[CHAT_ADD_PORT]);

            /* Copy name into channel */
            memcpy(people[which].name, &data[CHAT_ADD_NAME], 256);
            people[which].name[256] = 0;
            people[which].active = 1;

            /* Let the user know what happened */
            TextWindowAddText(termwin,
    "* New client on %d from %d.%d.%d.%d:%d (%s)\n", which,
        (newip.host>>24)&0xFF, (newip.host>>16)&0xFF,
            (newip.host>>8)&0xFF, newip.host&0xFF,
                    newip.port, people[which].name);

            /* Put the address back in network form */
            newip.host = SDL_SwapBE32(newip.host);
            newip.port = SDL_SwapBE16(newip.port);

            /* Bind the address to the UDP socket */
            SDLNet_UDP_Bind(udpsock, which, &newip);
        }
        used = CHAT_ADD_NAME+data[CHAT_ADD_NLEN];
        break;
        case CHAT_DEL: {
            Uint8 which;

            /* Figure out which channel we lost */
            which = data[CHAT_DEL_SLOT];
            if ( (which >= CHAT_MAXPEOPLE) ||
                        ! people[which].active ) {
                /* Invalid channel?? */
                break;
            }
            people[which].active = 0;

            /* Let the user know what happened */
            TextWindowAddText(termwin,
    "* Lost client on %d (%s)\n", which, people[which].name);

            /* Unbind the address on the UDP socket */
            SDLNet_UDP_Unbind(udpsock, which);
        }
        used = CHAT_DEL_LEN;
        break;
        case CHAT_BYE: {
            TextWindowAddText(termwin, "* Chat server full\n");
        }
        used = CHAT_BYE_LEN;
        break;
        default: {
            /* Unknown packet type?? */;
        }
        used = 0;
        break;
    }
    return(used);
}

void HandleServer(void)
{
    Uint8 data[512];
    int pos, len;
    int used;

    /* Has the connection been lost with the server? */
    len = SDLNet_TCP_Recv(tcpsock, (char *)data, 512);
    if ( len <= 0 ) {
        SDLNet_TCP_DelSocket(socketset, tcpsock);
        SDLNet_TCP_Close(tcpsock);
        tcpsock = NULL;
        TextWindowAddText(termwin, "Connection with server lost!\n");
    } else {
        pos = 0;
        while ( len > 0 ) {
            used = HandleServerData(&data[pos]);
            pos += used;
            len -= used;
            if ( used == 0 ) {
                /* We might lose data here.. oh well,
                   we got a corrupt packet from server
                 */
                len = 0;
            }
        }
    }
}
void HandleClient(void)
{
    int n;

    n = SDLNet_UDP_RecvV(udpsock, packets);
    while ( n-- > 0 ) {
#ifdef __DREAMCAST__
        printf("Received %d UDP bytes on channel %d\n", packets[n]->len, packets[n]->channel);
#endif
        if ( packets[n]->channel >= 0 ) {
            TextWindowAddText(termwin, "[%s] ",
                people[packets[n]->channel].name);
            TextWindowAddTextWithLength(termwin, (char *)packets[n]->data, packets[n]->len);
#ifdef __DREAMCAST__
        } else {
            printf("Ignoring UDP packet from unbound peer %d.%d.%d.%d:%d\n",
                (int)((packets[n]->address.host >> 0) & 0xFF),
                (int)((packets[n]->address.host >> 8) & 0xFF),
                (int)((packets[n]->address.host >> 16) & 0xFF),
                (int)((packets[n]->address.host >> 24) & 0xFF),
                SDLNet_Read16(&packets[n]->address.port));
#endif
        }
    }
}

void HandleNet(void)
{
    SDLNet_CheckSockets(socketset, 0);
    if ( SDLNet_SocketReady(tcpsock) ) {
        HandleServer();
    }
#ifdef __DREAMCAST__
    HandleClient();
#else
    if ( SDLNet_SocketReady(udpsock) ) {
        HandleClient();
    }
#endif
}

void InitGUI(int width, int height)
{
    int lines = (height / FONT_LINE_HEIGHT) - 2;

    /* Chat terminal window */
    termwin = TextWindowCreate(2, 2, width-4, lines*FONT_LINE_HEIGHT);

    /* Send-line window */
    sendwin = TextWindowCreate(2, 2+lines*FONT_LINE_HEIGHT+2, width-4, 1*FONT_LINE_HEIGHT);
    TextWindowAddText(sendwin, CHAT_PROMPT);
}

void DisplayGUI(SDL_Renderer *renderer)
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
#ifdef __DREAMCAST__
    vid_border_color(0, 255, 0);
#endif
    SDL_RenderClear(renderer);
    TextWindowDisplay(termwin, renderer);
    TextWindowDisplay(sendwin, renderer);
    SDL_RenderPresent(renderer);
}

void cleanup(int exitcode)
{
    /* Clean up the GUI */
    if ( termwin ) {
        TextWindowDestroy( termwin );
        termwin = NULL;
    }
    if ( sendwin ) {
        TextWindowDestroy( sendwin );
        sendwin = NULL;
    }
    /* Close the network connections */
    if ( tcpsock != NULL ) {
        SDLNet_TCP_Close(tcpsock);
        tcpsock = NULL;
    }
    if ( udpsock != NULL ) {
        SDLNet_UDP_Close(udpsock);
        udpsock = NULL;
    }
    if ( socketset != NULL ) {
        SDLNet_FreeSocketSet(socketset);
        socketset = NULL;
    }
    if ( packets != NULL ) {
        SDLNet_FreePacketV(packets);
        packets = NULL;
    }
    SDLNet_Quit();
    SDL_Quit();
    exit(exitcode);
}

int main(int argc, char *argv[])
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    int i, done;
#ifdef __DREAMCAST__
    const char *server;
    const char *name = NULL;
#else
    char *server;
#endif
    IPaddress serverIP;
    SDL_Event event;

#ifdef __DREAMCAST__
    // gdb_init();
    // gdb_breakpoint();
    // dbgio_dev_select("fb");
    // dbgio_enable();
    printf("chat: starting\n");
    cont_btn_callback(0,
        CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y,
        (cont_btn_callback_t)arch_exit);


#endif

#ifdef __DREAMCAST__
    /* Check command line arguments. KOS starts main as main(0, NULL). */
    if ( argc > 1 && argv != NULL ) {
        server = argv[1];
    } else {
#ifdef DEFAULT_CHAT_SERVER
        server = DEFAULT_CHAT_SERVER;
#else
        SDL_Log("Usage: %s <server>\n", argv ? argv[0] : "chat");
        exit(1);
#endif
    }

    if ( argc > 2 && argv != NULL ) {
        name = argv[2];
    }
#else
    /* Check command line arguments */
    if ( argv[1] == NULL ) {
        SDL_Log("Usage: %s <server>\n", argv[0]);
        exit(1);
    }
    server = argv[1];
#endif

    /* Initialize SDL */
    if ( SDL_Init(SDL_INIT_VIDEO) < 0 ) {
#ifdef __DREAMCAST__
        printf(
                    "Couldn't initialize SDL: %s\n",
                    SDL_GetError());
        DreamcastFatal("Couldn't initialize SDL", SDL_GetError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Couldn't initialize SDL: %s\n",
                    SDL_GetError());
#endif
        exit(1);
    }
#ifdef __DREAMCAST__
    printf("chat: SDL video initialized\n");
#endif


#ifdef __DREAMCAST__
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

    window = SDL_CreateWindow("SDL_net chat",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              640, 480,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);
    if ( window == NULL ) {
        printf("Couldn't create window: %s\n",
                     SDL_GetError());
        DreamcastFatal("Couldn't create window", SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    printf("chat: window created\n");
    
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    // SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    if ( renderer == NULL ) {
        printf("Couldn't create renderer: %s\n",
                     SDL_GetError());
        DreamcastFatal("Couldn't create renderer", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(1);
    }
    printf("chat: renderer created\n");
#else
    /* Set a 640x480 video mode */
    if ( SDL_CreateWindowAndRenderer(640, 480, 0, &window, &renderer) < 0 ) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't create window: %s\n",
                     SDL_GetError());
        SDL_Quit();
        exit(1);
    }
#endif

    /* Initialize the network */
    if ( SDLNet_Init() < 0 ) {
#ifdef __DREAMCAST__
        printf(
                     "Couldn't initialize net: %s\n",
                     SDLNet_GetError());
        DreamcastFatal("Couldn't initialize net", SDLNet_GetError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't initialize net: %s\n",
                     SDLNet_GetError());
#endif
        SDL_Quit();
        exit(1);
    }
#ifdef __DREAMCAST__
    printf("chat: SDL_net initialized\n");
#endif

    /* Go! */
    InitGUI(640, 480);
#ifdef __DREAMCAST__
    printf("chat: GUI buffers initialized\n");
#endif

    /* Allocate a vector of packets for client messages */
    packets = SDLNet_AllocPacketV(4, CHAT_PACKETSIZE);
    if ( packets == NULL ) {
#ifdef __DREAMCAST__
        printf(
                     "Couldn't allocate packets: Out of memory\n");
        DreamcastFatal("Couldn't allocate packets", "Out of memory");
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't allocate packets: Out of memory\n");
#endif
        cleanup(2);
    }

    /* Connect to remote host and create UDP endpoint */
    TextWindowAddText(termwin, "Connecting to %s ... ", server);
#ifdef __DREAMCAST__
    printf(
                 "Connecting to %s ... ", server);
#endif
    DisplayGUI(renderer);
#ifdef __DREAMCAST__
    printf("chat: first frame displayed\n");
#endif
    SDLNet_ResolveHost(&serverIP, server, CHAT_PORT);
    if ( serverIP.host == INADDR_NONE ) {
        TextWindowAddText(termwin, "Couldn't resolve hostname\n");
#ifdef __DREAMCAST__
        printf(
                     "Couldn't resolve hostname\n");
        DisplayGUI(renderer);
        DreamcastFatal("Couldn't resolve hostname", SDLNet_GetError());
#endif
    } else {
        /* If we fail, it's okay, the GUI shows the problem */
        tcpsock = SDLNet_TCP_Open(&serverIP);
        if ( tcpsock == NULL ) {
            TextWindowAddText(termwin, "Connect failed\n");
#ifdef __DREAMCAST__
            printf(
                         "Connect failed: %s\n", SDLNet_GetError());
            DisplayGUI(renderer);
            DreamcastFatal("Connect failed", SDLNet_GetError());
#endif
        } else {
            TextWindowAddText(termwin, "Connected\n");
#ifdef __DREAMCAST__
            printf(
                         "Connected\n");
#endif
        }
    }
    /* Try ports in the range {CHAT_PORT - CHAT_PORT+10} */
    for ( i=0; (udpsock == NULL) && i<10; ++i ) {
        udpsock = SDLNet_UDP_Open(CHAT_PORT+i);
    }
    if ( udpsock == NULL ) {
        SDLNet_TCP_Close(tcpsock);
        tcpsock = NULL;
        TextWindowAddText(termwin, "Couldn't create UDP endpoint\n");
#ifdef __DREAMCAST__
        printf(
                     "Couldn't create UDP endpoint\n");
        DisplayGUI(renderer);
        DreamcastFatal("Couldn't create UDP endpoint", SDLNet_GetError());
#endif
    }

    /* Allocate the socket set for polling the network */
    socketset = SDLNet_AllocSocketSet(2);
    if ( socketset == NULL ) {
#ifdef __DREAMCAST__
        printf(
                     "Couldn't create socket set: %s\n",
                     SDLNet_GetError());
        DreamcastFatal("Couldn't create socket set", SDLNet_GetError());
#else
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't create socket set: %s\n",
                     SDLNet_GetError());
#endif
        cleanup(2);
    }
    SDLNet_TCP_AddSocket(socketset, tcpsock);
    SDLNet_UDP_AddSocket(socketset, udpsock);

    /* Run the GUI, handling network data */
#ifdef __DREAMCAST__
    SendHello(name);
#else
    SendHello(argv[2]);
#endif
    done = 0;
    while ( !done ) {
        HandleNet();

        while ( SDL_PollEvent(&event) == 1 ) {
            switch ( event.type ) {
            case SDL_QUIT:
                done = 1;
                break;
            case SDL_KEYDOWN:
                switch ( event.key.keysym.sym ) {
                case SDLK_ESCAPE:
                    done = 1;
                    break;
                case SDLK_RETURN:
#ifdef __DREAMCAST__
                case SDLK_KP_ENTER:
#endif
                    /* Send our line of text */
                    SendBuf(keybuf, keypos);
                    keypos = 0;
                    break;
                case SDLK_BACKSPACE:
                    /* If there's data, back up over it */
                    if ( keypos > 0 ) {
                        TextWindowAddText(sendwin, "\b", 1);
                        --keypos;
                    }
                    break;
                default:
                    break;
                }
                break;
            case SDL_TEXTINPUT:
                {
                    size_t textlen = SDL_strlen(event.text.text);
                    Uint32 now = SDL_GetTicks();

                    if ( textlen < sizeof(keybuf) ) {
#ifdef __DREAMCAST__
                        if ((SDL_strchr(event.text.text, '\n') != NULL) ||
                            (SDL_strchr(event.text.text, '\r') != NULL)) {
                            break;
                        }
                        if ((textlen == 1) &&
                            (event.text.text[0] == last_textinput_text[0]) &&
                            ((now - last_textinput_tick) < 100)) {
                            break;
                        }
                        last_textinput_tick = now;
                        SDL_strlcpy(last_textinput_text, event.text.text, sizeof(last_textinput_text));
#endif
                        /* If the buffer is full, send it */
                        if ( (keypos + textlen) >= sizeof(keybuf) ) {
                            SendBuf(keybuf, keypos);
                            keypos = 0;
                        }
                        /* Add the text to our send buffer */
                        TextWindowAddTextWithLength(sendwin, event.text.text, textlen);
                        SDL_memcpy(&keybuf[keypos], event.text.text, textlen);
                        keypos += textlen;
                    }
                }
                break;
            default:
                break;
            }
        }

        DisplayGUI(renderer);
    }
    cleanup(0);

    /* Keep the compiler happy */
    return(0);
}
